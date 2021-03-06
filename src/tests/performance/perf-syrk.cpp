/* ************************************************************************
 * Copyright 2013 Advanced Micro Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ************************************************************************/


/*
 * Syrk performance test cases
 */

#include <stdlib.h>             // srand()
#include <string.h>             // memcpy()
#include <gtest/gtest.h>
#include <clBLAS.h>

#include <common.h>
#include <clBLAS-wrapper.h>
#include <BlasBase.h>
#include <syrk.h>
#include <blas-random.h>

#ifdef PERF_TEST_WITH_ACML
#include <blas-internal.h>
#include <blas-wrapper.h>
#endif

#include "PerformanceTest.h"

/*
 * NOTE: operation factor means overall number
 *       of multiply and add per each operation involving
 *       2 matrix elements
 */

using namespace std;
using namespace clMath;

#define CHECK_RESULT(ret)                                                   \
do {                                                                        \
    ASSERT_GE(ret, 0) << "Fatal error: can not allocate resources or "      \
                         "perform an OpenCL request!" << endl;              \
    EXPECT_EQ(0, ret) << "The OpenCL version is slower in the case" <<      \
                         endl;                                              \
} while (0)

namespace clMath {

template <typename ElemType> class SyrkPerformanceTest : public PerformanceTest
{
public:
    virtual ~SyrkPerformanceTest();

    virtual int prepare(void);
    virtual nano_time_t etalonPerfSingle(void);
    virtual nano_time_t clblasPerfSingle(void);

    static void runInstance(BlasFunction fn, TestParams *params)
    {
        SyrkPerformanceTest<ElemType> perfCase(fn, params);
        int ret = 0;
        int opFactor;
        BlasBase *base;

        base = clMath::BlasBase::getInstance();

        if (fn == FN_SSYRK || fn == FN_DSYRK) {
            opFactor = 1;
        }
        else {
            opFactor = 4;
        }

        if ((fn == FN_DSYRK || fn == FN_ZSYRK) &&
            !base->isDevSupportDoublePrecision()) {

            std::cerr << ">> WARNING: The target device doesn't support native "
                         "double precision floating point arithmetic" <<
                         std::endl << ">> Test skipped" << std::endl;
            return;
        }

        if (!perfCase.areResourcesSufficient(params)) {
            std::cerr << ">> RESOURCE CHECK: Skip due to unsufficient resources" <<
                        std::endl;
        }
        else {
            ret = perfCase.run(opFactor);
        }

        ASSERT_GE(ret, 0) << "Fatal error: can not allocate resources or "
                             "perform an OpenCL request!" << endl;
        EXPECT_EQ(0, ret) << "The OpenCL version is slower in the case" << endl;
    }

private:
    SyrkPerformanceTest(BlasFunction fn, TestParams *params);

    bool areResourcesSufficient(TestParams *params);

    TestParams params_;
    ElemType alpha_;
    ElemType beta_;
    ElemType *A_;
    ElemType *C_;
    ElemType *backC_;
    cl_mem mobjA_;
    cl_mem mobjC_;
    ::clMath::BlasBase *base_;
};

template <typename ElemType>
SyrkPerformanceTest<ElemType>::SyrkPerformanceTest(
    BlasFunction fn,
    TestParams *params) : PerformanceTest(fn, (problem_size_t)params->N * params->N
                                            * params->K),
                        params_(*params), mobjA_(NULL), mobjC_(NULL)
{
    A_ = new ElemType[params_.rowsA * params_.columnsA];
    C_ = new ElemType[params_.rowsC * params_.columnsC];
    backC_ = new ElemType[params_.rowsC * params_.columnsC];

    base_ = ::clMath::BlasBase::getInstance();
}

template <typename ElemType>
SyrkPerformanceTest<ElemType>::~SyrkPerformanceTest()
{
    delete[] A_;
    delete[] C_;
    delete[] backC_;

    clReleaseMemObject(mobjC_);
    clReleaseMemObject(mobjA_);
}

/*
 * Check if available OpenCL resources are sufficient to
 * run the test case
 */
template <typename ElemType> bool
SyrkPerformanceTest<ElemType>::areResourcesSufficient(TestParams *params)
{
    clMath::BlasBase *base;
    size_t gmemSize, allocSize, maxMatrSize;
    size_t n = params->N, k = params->K;

    base = clMath::BlasBase::getInstance();
    gmemSize = (size_t)base->availGlobalMemSize(0);
    allocSize = (size_t)base->maxMemAllocSize();

    maxMatrSize = gmemSize / 3;

    maxMatrSize = std::min(maxMatrSize, allocSize);

    return (n * k * sizeof(ElemType) < maxMatrSize);
}

template <typename ElemType> int
SyrkPerformanceTest<ElemType>::prepare(void)
{
    bool useAlpha = base_->useAlpha();
    bool useBeta = base_->useBeta();

    if (useAlpha) {
        alpha_ = convertMultiplier<ElemType>(params_.alpha);
    }
    if (useBeta) {
        beta_ = convertMultiplier<ElemType>(params_.beta);
    }

    randomGemmMatrices<ElemType>(params_.order, params_.transA, clblasNoTrans,
        params_.N, params_.N, params_.K, useAlpha, &alpha_, A_, params_.lda,
        NULL, 0, useBeta, &beta_, C_, params_.ldc);


    mobjA_ = base_->createEnqueueBuffer(A_, params_.rowsA * params_.columnsA *
                                        sizeof(ElemType),
                                        params_.offA * sizeof(ElemType),
                                        CL_MEM_READ_ONLY);
    if (mobjA_) {
        mobjC_ = base_->createEnqueueBuffer(backC_, params_.rowsC * params_.columnsC *
                                            sizeof(ElemType),
                                            params_.offCY * sizeof(ElemType),
                                            CL_MEM_READ_WRITE);
    }

    return (mobjC_) ? 0 : -1;
}

template <typename ElemType> nano_time_t
SyrkPerformanceTest<ElemType>::etalonPerfSingle(void)
{
    nano_time_t time = 0;
    clblasOrder order;
    size_t lda, ldc;

#ifndef PERF_TEST_WITH_ROW_MAJOR
    if (params_.order == clblasRowMajor) {
        cerr << "Row major order is not allowed" << endl;
        return NANOTIME_ERR;
    }
#endif

    memcpy(C_, backC_, params_.rowsC * params_.columnsC * sizeof(ElemType));
    order = params_.order;
    lda = params_.lda;
    ldc = params_.ldc;

#ifdef PERF_TEST_WITH_ACML

// #warning "SYRK performance test not implemented"
    time = NANOTIME_MAX;
    order = order;
    lda = lda;
    ldc = ldc;

#endif  // PERF_TEST_WITH_ACML

    return time;
}


template <typename ElemType> nano_time_t
SyrkPerformanceTest<ElemType>::clblasPerfSingle(void)
{
    nano_time_t time;
    cl_event event;
    cl_int status;
    cl_command_queue queue = base_->commandQueues()[0];

    status = clEnqueueWriteBuffer(queue, mobjC_, CL_TRUE, 0,
                                  params_.rowsC * params_.columnsC *
                                  sizeof(ElemType), backC_, 0, NULL, &event);
    if (status != CL_SUCCESS) {
        cerr << "Matrix C buffer object enqueuing error, status = " <<
                 status << endl;

        return NANOTIME_ERR;
    }

    status = clWaitForEvents(1, &event);
    if (status != CL_SUCCESS) {
        cout << "Wait on event failed, status = " <<
                status << endl;

        return NANOTIME_ERR;
    }

    event = NULL;
    status = (cl_int)clMath::clblas::syrk(params_.order,
        params_.uplo, params_.transA, params_.N, params_.K, alpha_,
        mobjA_, params_.offA, params_.lda, beta_, mobjC_, params_.offCY,
        params_.ldc, 1, &queue, 0, NULL, &event);
    if (status != CL_SUCCESS) {
        cerr << "The CLBLAS SYRK function failed, status = " <<
                status << endl;

        return NANOTIME_ERR;
    }
    status = flushAll(1, &queue);
    if (status != CL_SUCCESS) {
        cerr << "clFlush() failed, status = " << status << endl;
        return NANOTIME_ERR;
    }

    time = getCurrentTime();
    status = waitForSuccessfulFinish(1, &queue, &event);
    if (status == CL_SUCCESS) {
        time = getCurrentTime() - time;
    }
    else {
        cerr << "Waiting for completion of commands to the queue failed, "
                "status = " << status << endl;
        time = NANOTIME_ERR;
    }

    return time;
}

} // namespace clMath

// ssyrk performance test
TEST_P(SYRK, ssyrk)
{
    TestParams params;

    getParams(&params);
    SyrkPerformanceTest<float>::runInstance(FN_SSYRK, &params);
}

// dsyrk performance test case
TEST_P(SYRK, dsyrk)
{
    TestParams params;

    getParams(&params);
    SyrkPerformanceTest<double>::runInstance(FN_DSYRK, &params);
}

// csyrk performance test
TEST_P(SYRK, csyrk)
{
    TestParams params;

    getParams(&params);
    SyrkPerformanceTest<FloatComplex>::runInstance(FN_CSYRK, &params);
}

// zsyrk performance test case
TEST_P(SYRK, zsyrk)
{
    TestParams params;

    getParams(&params);
    SyrkPerformanceTest<DoubleComplex>::runInstance(FN_ZSYRK, &params);
}
