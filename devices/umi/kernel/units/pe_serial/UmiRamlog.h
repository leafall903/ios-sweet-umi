/* Research diagnostic: Linux-compatible umi console RAM ring only.
 * Layout follows the board's verified EFI RamoopsSerialPortLib and Linux
 * persistent_ram_buffer. No disk, pmsg, allocation or logging calls here.
 * Callers must hold nonblocking ownership and mask local interrupts.
 */
#ifndef UMI_RAMLOG_H
#define UMI_RAMLOG_H
#include <stdint.h>
#include <stddef.h>

#define UMI_RAMLOG_BASE 0xb0000000ULL
#define UMI_RAMLOG_BYTES 0x200000U
#define UMI_RAMLOG_PMSG_BASE 0xb0200000ULL
#define UMI_RAMLOG_MAGIC 0x43474244U
struct umi_ramlog_header { uint32_t signature, start, size; };
#define UMI_RAMLOG_CAPACITY (UMI_RAMLOG_BYTES - sizeof(struct umi_ramlog_header))
_Static_assert(sizeof(struct umi_ramlog_header) == 12, "Linux DBGC header");
_Static_assert(offsetof(struct umi_ramlog_header, start) == 4, "Linux start");
_Static_assert(offsetof(struct umi_ramlog_header, size) == 8, "Linux size");
_Static_assert(UMI_RAMLOG_BASE + UMI_RAMLOG_BYTES == UMI_RAMLOG_PMSG_BASE, "stop before pmsg");
_Static_assert(UMI_RAMLOG_CAPACITY == 0x1ffff4U, "ECC-free capacity");

static inline void umi_ramlog_barrier(void)
{
    /* ml_io_map_wcomb is Normal Non-Cacheable; order payload/metadata to RAM. */
    __asm__ volatile("dsb sy" ::: "memory");
}

static int umi_ramlog_allowed(uint32_t enabled, uint32_t cpus,
    const uint8_t *compatible, size_t length)
{
    static const char expected[] = "xiaomi,umi";
    if (enabled != 1 || (cpus != 1 && cpus != 4 && cpus != 8) || compatible == NULL) return 0;
    for (size_t at = 0; at < length;) {
        size_t end = at;
        while (end < length && compatible[end] != 0) ++end;
        if (end == length) return 0; /* Reject unterminated DT strings. */
        if (end - at == sizeof(expected) - 1) {
            size_t i = 0;
            while (i < sizeof(expected) - 1 && compatible[at + i] == (uint8_t)expected[i]) ++i;
            if (i == sizeof(expected) - 1) return 1;
        }
        at = end + 1;
    }
    return 0;
}

static int umi_ramlog_valid(volatile struct umi_ramlog_header *h)
{
    return h->signature == UMI_RAMLOG_MAGIC && h->size <= UMI_RAMLOG_CAPACITY && h->start <= h->size;
}

static void umi_ramlog_prepare(volatile struct umi_ramlog_header *h)
{
    if (umi_ramlog_valid(h)) return; /* Preserve preceding EFI console text. */
    h->signature = 0;
    umi_ramlog_barrier();
    h->start = h->size = 0;
    umi_ramlog_barrier();
    h->signature = UMI_RAMLOG_MAGIC;
    umi_ramlog_barrier();
}

static void umi_ramlog_append(volatile struct umi_ramlog_header *h,
    const uint8_t *input, size_t count)
{
    if (h == NULL || input == NULL || count == 0) return;
    umi_ramlog_prepare(h);
    volatile uint8_t *data = (volatile uint8_t *)h + sizeof(*h);
    uint32_t start = h->start;
    uint32_t size = h->size;
    if (start == UMI_RAMLOG_CAPACITY) start = 0;
    if (count > UMI_RAMLOG_CAPACITY) {
        input += count - UMI_RAMLOG_CAPACITY;
        count = UMI_RAMLOG_CAPACITY;
    }
    for (size_t i = 0; i < count; ++i) {
        data[start++] = input[i];
        if (start == UMI_RAMLOG_CAPACITY) start = 0;
    }
    size = (uint32_t)((size_t)size + count > UMI_RAMLOG_CAPACITY ? UMI_RAMLOG_CAPACITY : (size_t)size + count);
    umi_ramlog_barrier();
    h->size = size;
    umi_ramlog_barrier();
    h->start = start;
    umi_ramlog_barrier();
    /* Like Linux/EFI, a reset during the update may truncate the last byte;
     * this is a recoverable ring, not a crash-atomic transaction. */
}
#endif
