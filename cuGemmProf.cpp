#include "cxxopts.hpp"
#include "cublasGemmEx.h"
#include "cublasLtMatMul.h"
#include "helper.h"
#include "macro.h"
#include "verify.h"
#include <cublas_v2.h>
#include <cublasLt.h>
#include <algorithm>
#include <map>
#include <string>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cfloat>
#include <cstdint>

cxxopts::ParseResult Parse(int argc, const char* argv[]) {
  try {
    cxxopts::Options options(argv[0], "GEMM testing");

    options.positional_help("[optional args]").show_positional_help();

    options.add_options()
    ("m", "m dimension", cxxopts::value<int>()->default_value("32"))
    ("n", "n dimension", cxxopts::value<int>()->default_value("32"))
    ("k", "k dimension", cxxopts::value<int>()->default_value("32"))
    ("d", "device ID", cxxopts::value<int>()->default_value("0"))
    ("l", "loop", cxxopts::value<int>()->default_value("1"))
    ("ta", "set A to CUBLAS_OP_T, else CUBLAS_OP_N")
    ("tb", "set B to CUBLAS_OP_T, else CUBLAS_OP_N")
    ("type", "slect combination of types",
        cxxopts::value< std::vector<int> >()->default_value("5"))
    ("algo", "assgin algorithm ID (0~23)", cxxopts::value< std::vector<int> >())
    ("tensor_algo", "assgin TensorOp algorithm ID (0~15)", cxxopts::value< std::vector<int> >())
    ("all_algo", "run all algorithms")
    ("w, workspace", "workspace size, unit: MiB", cxxopts::value<size_t>()->default_value("0"))
    ("help", "print help");

    auto result = options.parse(argc, (char**&)argv);

    std::string type_info;
    type_info = "available combination of types:\n"
                "ID, ComputeType, Atype,      Btype,      Ctype\n"
                "0,  {CUDA_R_16F, CUDA_R_16F, CUDA_R_16F, CUDA_R_16F}\n"
                "1,  {CUDA_R_32I, CUDA_R_8I,  CUDA_R_8I,  CUDA_R_32I}\n"
                "2,  {CUDA_R_32F, CUDA_R_16F, CUDA_R_16F, CUDA_R_16F}\n"
                "3,  {CUDA_R_32F, CUDA_R_8I,  CUDA_R_8I,  CUDA_R_32F}\n"
                "4,  {CUDA_R_32F, CUDA_R_16F, CUDA_R_16F, CUDA_R_32F}\n"
                "5,  {CUDA_R_32F, CUDA_R_32F, CUDA_R_32F, CUDA_R_32F}\n"
                "6,  {CUDA_R_64F, CUDA_R_64F, CUDA_R_64F, CUDA_R_64F}\n"
                "7,  {CUDA_C_32F, CUDA_C_8I,  CUDA_C_8I,  CUDA_C_32F}\n"
                "8,  {CUDA_C_32F, CUDA_C_32F, CUDA_C_32F, CUDA_C_32F}\n"
                "9,  {CUDA_C_64F, CUDA_C_64F, CUDA_C_64F, CUDA_C_64F}\n";


    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        std::cout << type_info;
        exit(EXIT_SUCCESS);
    }

    return result;

  } catch (const cxxopts::OptionException& e) {
    std::cout << "error parsing options: " << e.what() << std::endl;
    exit(EXIT_FAILURE);
  }
}

std::string Mask2Str(const std::vector<bool>& mask) {
    std::string info;
    auto count = std::count_if(mask.begin(), mask.end(),
        [](bool x) { return x; });
    if (count == mask.size()) {
        info = "all meet, ";
    }
    else {
        info = "(";
        for (auto bit : mask) {
            info += std::to_string(static_cast<int>(bit)) + ".";
        }
        info += "), ";
    }
    return info;
}

std::string Dp4aRestrictions(const Param_t& param) {
    std::vector<bool> mask(2);
    mask[0] = param.lda % 4 == 0;
    mask[1] = param.ldb % 4 == 0;
    return Mask2Str(mask);
}

std::string TensorCoreRestrictions(const Param_t& param) {
    // refer to https://docs.nvidia.com/cuda/cublas/#tensorop-restrictions
    std::vector<bool> mask(8);
    mask[0] = param.m % 4 == 0;
    mask[1] = param.k % 8 == 0;
    mask[2] = reinterpret_cast<intptr_t>(param.A) % 16 == 0;
    mask[3] = reinterpret_cast<intptr_t>(param.B) % 16 == 0;
    mask[4] = reinterpret_cast<intptr_t>(param.C) % 16 == 0;
    mask[5] = param.lda % (16 / Dtype2Size(param.dtype.Atype)) == 0;
    mask[6] = param.ldb % (16 / Dtype2Size(param.dtype.Btype)) == 0;
    mask[7] = param.ldc % (16 / Dtype2Size(param.dtype.Ctype)) == 0;
    return Mask2Str(mask);
}

std::vector<cublasGemmAlgo_t> SetupAlgo(const cxxopts::ParseResult& parse, const char option[],
    const std::vector<cublasGemmAlgo_t>& all_options) {

    std::vector<cublasGemmAlgo_t> select_algo;
        if (parse.count("all_algo")) {
            select_algo = all_options;
        }
        else if (parse.count(option)) {
            auto select_id = parse[option].as< std::vector<int> >();

            struct Int2Algo {
                cublasGemmAlgo_t base_;
                Int2Algo(cublasGemmAlgo_t base) : base_(base) {};
                cublasGemmAlgo_t operator()(int id) {
                    return static_cast<cublasGemmAlgo_t>(id + static_cast<int>(base_));
                }
            };
            Int2Algo functor(all_options[1]);

            select_algo.resize(select_id.size());
            std::transform(select_id.begin(), select_id.end(), select_algo.begin(), functor);
        }
        else {
            select_algo.push_back(all_options[0]);
        }
        return select_algo;
}

int main (int argc, const char* argv[]) {

    auto result = Parse(argc, argv);

    const std::vector<Dtypes_t> gemm_types = {
        Dtypes_t{CUDA_R_16F, CUDA_R_16F, CUDA_R_16F, CUDA_R_16F},
        Dtypes_t{CUDA_R_32I, CUDA_R_8I,  CUDA_R_8I,  CUDA_R_32I},
        Dtypes_t{CUDA_R_32F, CUDA_R_16F, CUDA_R_16F, CUDA_R_16F},
        Dtypes_t{CUDA_R_32F, CUDA_R_8I,  CUDA_R_8I,  CUDA_R_32F},
        Dtypes_t{CUDA_R_32F, CUDA_R_16F, CUDA_R_16F, CUDA_R_32F},
        Dtypes_t{CUDA_R_32F, CUDA_R_32F, CUDA_R_32F, CUDA_R_32F},
        Dtypes_t{CUDA_R_64F, CUDA_R_64F, CUDA_R_64F, CUDA_R_64F},
        Dtypes_t{CUDA_C_32F, CUDA_C_8I,  CUDA_C_8I,  CUDA_C_32F},
        Dtypes_t{CUDA_C_32F, CUDA_C_32F, CUDA_C_32F, CUDA_C_32F},
        Dtypes_t{CUDA_C_64F, CUDA_C_64F, CUDA_C_64F, CUDA_C_64F},
    };

    const std::vector<cublasGemmAlgo_t> cuda_algos = {
        CUBLAS_GEMM_DEFAULT,
        CUBLAS_GEMM_ALGO0,
        CUBLAS_GEMM_ALGO1,
        CUBLAS_GEMM_ALGO2,
        CUBLAS_GEMM_ALGO3,
        CUBLAS_GEMM_ALGO4,
        CUBLAS_GEMM_ALGO5,
        CUBLAS_GEMM_ALGO6,
        CUBLAS_GEMM_ALGO7,
        CUBLAS_GEMM_ALGO8,
        CUBLAS_GEMM_ALGO9,
        CUBLAS_GEMM_ALGO10,
        CUBLAS_GEMM_ALGO11,
        CUBLAS_GEMM_ALGO12,
        CUBLAS_GEMM_ALGO13,
        CUBLAS_GEMM_ALGO14,
        CUBLAS_GEMM_ALGO15,
        CUBLAS_GEMM_ALGO16,
        CUBLAS_GEMM_ALGO17,
        CUBLAS_GEMM_ALGO18,
        CUBLAS_GEMM_ALGO19,
        CUBLAS_GEMM_ALGO20,
        CUBLAS_GEMM_ALGO21,
        CUBLAS_GEMM_ALGO22,
        CUBLAS_GEMM_ALGO23
    };

    const std::vector<cublasGemmAlgo_t> tensor_algos = {
        CUBLAS_GEMM_DEFAULT_TENSOR_OP,
        CUBLAS_GEMM_ALGO0_TENSOR_OP,
        CUBLAS_GEMM_ALGO1_TENSOR_OP,
        CUBLAS_GEMM_ALGO2_TENSOR_OP,
        CUBLAS_GEMM_ALGO3_TENSOR_OP,
        CUBLAS_GEMM_ALGO4_TENSOR_OP,
        CUBLAS_GEMM_ALGO5_TENSOR_OP,
        CUBLAS_GEMM_ALGO6_TENSOR_OP,
        CUBLAS_GEMM_ALGO7_TENSOR_OP,
        CUBLAS_GEMM_ALGO8_TENSOR_OP,
        CUBLAS_GEMM_ALGO9_TENSOR_OP,
        CUBLAS_GEMM_ALGO10_TENSOR_OP,
        CUBLAS_GEMM_ALGO11_TENSOR_OP,
        CUBLAS_GEMM_ALGO12_TENSOR_OP,
        CUBLAS_GEMM_ALGO13_TENSOR_OP,
        CUBLAS_GEMM_ALGO14_TENSOR_OP,
        CUBLAS_GEMM_ALGO15_TENSOR_OP,
    };


    auto device_id = result["d"].as<int>();
    RUNTIME_API_CALL(cudaSetDevice(device_id));
    cudaDeviceProp prop;
    RUNTIME_API_CALL(cudaGetDeviceProperties(&prop, device_id));

    Param_t param;    
    param.m = result["m"].as<int>();
    param.n = result["n"].as<int>();
    param.k = result["k"].as<int>();
    param.transa = result.count("ta") ? CUBLAS_OP_T : CUBLAS_OP_N;
    param.lda = (param.transa == CUBLAS_OP_N) ? param.m : param.k; 
    param.transb = result.count("tb") ? CUBLAS_OP_T : CUBLAS_OP_N;
    param.ldb = (param.transb == CUBLAS_OP_N) ? param.k : param.n;
    param.ldc = param.m;
    param.workspace_size = result["w"].as<size_t>() << 20;
    if (param.workspace_size) {
        RUNTIME_API_CALL(cudaMalloc(&param.workspace, param.workspace_size));
    }
    else {
        param.workspace = nullptr;
    }

    std::cout << "device, op(A), op(B), m, n, k, ComputeType, Atype, Btype, Ctype, "
        "Dp4aRestrictions(lda.ldb), TensorCoreRestrictions(m.k.A.B.C.lda.ldb.ldc), "
        "algo, time(ms), GFLOPS" << std::endl;

    std::string dims_info;
    dims_info = std::string(prop.name) + ", " + param.GetDimsInfo();

    auto selected_dtypes = result["type"].as< std::vector<int> >();

    for (auto dtype_id : selected_dtypes) {

        auto dtypes = gemm_types[dtype_id];
        param.dtype = dtypes;

        std::string all_info;
        all_info = dims_info + param.dtype.GetDtypeInfo();

        if (param.dtype.Atype == CUDA_R_8I) {
            all_info += Dp4aRestrictions(param);
        }
        else {
            all_info += "NA, ";
        }

        auto src_dtype_size = Dtype2Size(dtypes.Atype);
        auto dst_dtype_size = Dtype2Size(dtypes.Ctype);

        void* dev_A;
        RUNTIME_API_CALL(cudaMalloc(&dev_A, param.m * param.k * src_dtype_size));
        InitMatrix(dev_A,
            (param.transa == CUBLAS_OP_N) ? param.m : param.k,
            (param.transa == CUBLAS_OP_N) ? param.k : param.m,
            param.lda, param.dtype.Atype);
        void* dev_B;
        RUNTIME_API_CALL(cudaMalloc(&dev_B, param.k * param.n * src_dtype_size));
        InitMatrix(dev_B,
            (param.transb == CUBLAS_OP_N) ? param.k : param.n,
            (param.transb == CUBLAS_OP_N) ? param.n : param.k,
            param.ldb, param.dtype.Btype);
        void* dev_C;
        RUNTIME_API_CALL(cudaMalloc(&dev_C, param.m * param.n * dst_dtype_size));
        RUNTIME_API_CALL(cudaMemset(dev_C, 0, param.m * param.n * dst_dtype_size));
        void* dev_D;
        RUNTIME_API_CALL(cudaMalloc(&dev_D, param.m * param.n * dst_dtype_size));
        RUNTIME_API_CALL(cudaMemset(dev_D, 0, param.m * param.n * dst_dtype_size));

        param.A = dev_A;
        param.B = dev_B;
        param.C = dev_C;
        param.D = dev_D;

        auto compute_dtype_size = Dtype2Size(dtypes.computeType);
 
        void* host_alpha;
        host_alpha = AllocAlphaScale(dtypes.computeType);

        void* host_beta;
        host_beta = malloc(compute_dtype_size);
        memset(host_beta, 0, compute_dtype_size);

        param.alpha = host_alpha;
        param.beta  = host_beta;

        NaiveGemm(
            param.transa,
            param.transb,
            param.m, param.n, param.k,
            param.A, param.dtype.Atype, param.lda,
            param.B, param.dtype.Btype, param.ldb,
            param.D, param.dtype.Ctype, param.ldc,
            param.dtype.computeType);

        auto loop = result["l"].as<int>();

        auto select_algo = SetupAlgo(result, "algo", cuda_algos);
        ProfileGemm(param, select_algo, all_info + "NA, ", loop);

        if (prop.major > 6) {
            auto info = TensorCoreRestrictions(param);
            select_algo = SetupAlgo(result, "tensor_algo", tensor_algos);
            ProfileGemm(param, select_algo, all_info + info, loop);
        }

        ProfileGemmLt(param, all_info + "NA, ", loop);

        RUNTIME_API_CALL(cudaFree(dev_A));
        RUNTIME_API_CALL(cudaFree(dev_B));
        RUNTIME_API_CALL(cudaFree(dev_C));
        RUNTIME_API_CALL(cudaFree(dev_D));
        free(host_alpha);
        free(host_beta);
    }
    return 0;
}
