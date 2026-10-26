/* Research diagnostic only. Loader owns/reserves the entire allocation.
 * Wire format: /sweet-ufs/dma-pool, four LE u64 values, exactly 32 bytes.
 * No allocation or hardware operation is performed by these policy helpers. */
#ifndef UMI_LOW_DMA_H
#define UMI_LOW_DMA_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define UMI_LOW_DMA_ALLOC_SIZE 0x18000ULL
#define UMI_LOW_DMA_BUFFER_SIZE 0x14000ULL
#define UMI_LOW_DMA_HEAP_START 0x98900000ULL
#define UMI_LOW_DMA_HEAP_END 0x9bc00000ULL

static bool umi_lowdma_end(uint64_t base, uint64_t size, uint64_t *end)
{
    if (!size || base > UINT64_MAX - size) return false;
    *end = base + size;
    return true;
}

static bool umi_lowdma_overlap(uint64_t start, uint64_t end,
    uint64_t other_start, uint64_t other_end)
{
    return start < other_end && other_start < end;
}

static bool umi_lowdma_pool_valid(const uint64_t pool[4],
    uint64_t kernel_base, uint64_t kernel_size)
{
    uint64_t allocation_end, buffer_end, kernel_end;
    if (pool[1] != UMI_LOW_DMA_ALLOC_SIZE || pool[3] != UMI_LOW_DMA_BUFFER_SIZE ||
        (pool[0] & 0xfff) || (pool[2] & 0x3fff) ||
        !umi_lowdma_end(pool[0], pool[1], &allocation_end) ||
        !umi_lowdma_end(pool[2], pool[3], &buffer_end) ||
        !umi_lowdma_end(kernel_base, kernel_size, &kernel_end)) return false;
    if (pool[0] < UMI_LOW_DMA_HEAP_START || allocation_end > UMI_LOW_DMA_HEAP_END ||
        allocation_end > 0x100000000ULL || pool[2] < pool[0] || buffer_end > allocation_end)
        return false;
    if (umi_lowdma_overlap(pool[0], allocation_end, kernel_base, kernel_end) ||
        umi_lowdma_overlap(pool[0], allocation_end, 0x9c000000ULL, 0x9e300000ULL) ||
        umi_lowdma_overlap(pool[0], allocation_end, 0xb0000000ULL, 0xb0400000ULL) ||
        umi_lowdma_overlap(pool[0], allocation_end, 0x23ff00000ULL, 0x240000000ULL))
        return false;
    return true;
}

/* Bounded NUL-separated compatible list. Reject unterminated strings and
 * duplicate identity entries, even if an earlier entry matched. */
static bool umi_lowdma_is_umi(const unsigned char *bytes, size_t length)
{
    static const unsigned char target[] = "xiaomi,umi";
    unsigned matches = 0;
    size_t pos = 0;
    if (!bytes || !length || length > 4096) return false;
    while (pos < length) {
        size_t begin = pos;
        while (pos < length && bytes[pos]) ++pos;
        if (pos == length) return false;
        if (pos - begin == sizeof(target) - 1) {
            size_t i = 0;
            while (i < sizeof(target) - 1 && bytes[begin + i] == target[i]) ++i;
            if (i == sizeof(target) - 1) ++matches;
        }
        ++pos;
    }
    return matches == 1;
}
#endif
