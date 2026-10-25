#pragma once
#include <stdint.h>

/* Xiaomi Mi 10 / umi resource profile for review only.
 * This header does not map registers, change clocks, or start the GPU.
 */
struct UmiA650Platform {
    uint32_t chip_id;
    uint64_t gpu_base, gpu_size;
    uint64_t gmu_base, gmu_size;
    uint64_t gpucc_base, gpucc_size;
    uint64_t smmu_base, smmu_size;
};

static const struct UmiA650Platform umi_a650_platform = {
    0x06050002u,
    0x03d00000ull, 0x00040000ull,
    0x03d6a000ull, 0x00030000ull,
    0x03d90000ull, 0x00009000ull,
    0x03da0000ull, 0x00010000ull
};
