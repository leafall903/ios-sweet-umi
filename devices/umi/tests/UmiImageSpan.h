#pragma once
#include <stdint.h>

/* A client image needs its pixels, not padding after the final scanline.
 * Owned allocations still reserve full rows. Multi-plane policy is unchanged.
 */
static inline bool umi_image_minimum_span(uint32_t width, uint32_t height,
    uint32_t bpe, uint32_t ew, uint32_t eh, uint32_t row,
    uint32_t offset, uint64_t *result)
{
    if (!result || !width || !height || width > 16384 || height > 16384 ||
        !bpe || bpe > 64 || !ew || !eh || ew > 255 || eh > 255) return false;
    uint64_t pixels = ((uint64_t)width + ew - 1) / ew;
    uint64_t rows = ((uint64_t)height + eh - 1) / eh;
    uint64_t tail = pixels * bpe;
    if (row < tail) return false;
    uint64_t used = (uint64_t)offset + (rows - 1) * row + tail;
    if (!used || used > (256ULL << 20)) return false;
    *result = used;
    return true;
}
