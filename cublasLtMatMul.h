#pragma once
#include "helper.h"
#include <string>
#include <vector>

struct cublasLtAlgoAttr_t {
    int algo_id;
    int tile_id;
    int reduction_scheme;
    int swizzle;
    int custom_option;
    size_t workspace_size;
    float wave_count;
};

std::vector<Result_t> ProfileLtGemm(const Param_t& param, bool all_algo, int loop, bool debug);