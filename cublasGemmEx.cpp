#include "cublasGemmEx.h"
#include "macro.h"
#include "verify.h"
#include <algorithm>
#include <cfloat>

void ProfileGemm(const Param_t& param, const std::vector<cublasGemmAlgo_t>& algos,
    const std::string& config_info, int loop) {

    cublasHandle_t handle;
    CUBLAS_API_CALL(cublasCreate(&handle));

    cudaEvent_t start;
    cudaEvent_t end;
    cublasStatus_t ret;

    RUNTIME_API_CALL(cudaEventCreate(&start));
    RUNTIME_API_CALL(cudaEventCreate(&end));

    std::vector<Result_t> results;
    for (auto algo : algos) {

        //param.algo = algo;
        float time = 0.f;
        bool fault = false;

        RUNTIME_API_CALL(cudaEventRecord(start));
        for (int i = 0; i < loop; ++i) {
            ret = cublasGemmEx(handle,
                               param.transa, param.transb,
                               param.m, param.n, param.k,
                               param.alpha, param.A, param.dtype.Atype, param.lda,
                               param.B, param.dtype.Btype, param.ldb, param.beta,
                               param.C, param.dtype.Ctype, param.ldc,
                               param.dtype.computeType, algo);
            if (ret != CUBLAS_STATUS_SUCCESS) {
                fault = true;
                if (ret != CUBLAS_STATUS_NOT_SUPPORTED &&
                    ret != CUBLAS_STATUS_INVALID_VALUE) {
                    CUBLAS_API_CALL(ret);
                }
                break;
            }
        }
        RUNTIME_API_CALL(cudaEventRecord(end));
        RUNTIME_API_CALL(cudaEventSynchronize(end));
        RUNTIME_API_CALL(cudaEventElapsedTime(&time, start, end));

        if (!fault) {
            fault = !Verify(param.C, param.D, param.m * param.n, param.dtype.Ctype);
            if (fault) {
                PrintMatrix(param.A, param.m, param.k, param.lda, param.dtype.Atype);
                PrintMatrix(param.B, param.k, param.n, param.ldb, param.dtype.Btype);
                PrintMatrix(param.C, param.m, param.n, param.ldc, param.dtype.Ctype);
                PrintMatrix(param.D, param.m, param.n, param.ldc, param.dtype.Ctype);
            }
        }
        RUNTIME_API_CALL(cudaMemset(param.C, 0, param.m * param.n * Dtype2Size(param.dtype.Ctype)));

        float gflops = 0;
        if (!fault) { 
            time /= loop;
            float workload = (2.f * param.m * param.n * param.k) * 1e-9;
            gflops = workload / (time * 1e-3);
        }
        else {
            time = FLT_MAX;
            gflops = NAN;
        }

        results.push_back(Result_t{algo, time, gflops});
    }

    RUNTIME_API_CALL(cudaEventDestroy(start));
    RUNTIME_API_CALL(cudaEventDestroy(end));

    std::sort(results.begin(), results.end(), SortResult);

    struct PrintInfo {
        std::string info_;
        PrintInfo(const std::string& msg) : info_(msg) {}
        void operator()(Result_t x) {
            std::cout << info_ << x << std::endl;
        }
    };

    PrintInfo functor(config_info);
    std::for_each(results.begin(), results.end(), functor);

    CUBLAS_API_CALL(cublasDestroy(handle));
}
