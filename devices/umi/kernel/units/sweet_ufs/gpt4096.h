/* Local review candidate; no device I/O, allocation, or external CRC library. */
#ifndef GPT4096_H
#define GPT4096_H
#include <stddef.h>
#include <stdint.h>

#define GPT4096_BLOCK_BYTES 4096u
enum gpt4096_status {
    GPT4096_OK = 0,
    GPT4096_BAD_ARGUMENT,
    GPT4096_BAD_SIGNATURE,
    GPT4096_BAD_REVISION,
    GPT4096_BAD_HEADER_SIZE,
    GPT4096_BAD_HEADER_CRC,
    GPT4096_BAD_GEOMETRY,
    GPT4096_BAD_ENTRIES_SHAPE,
    GPT4096_TABLE_TOO_LARGE,
    GPT4096_BAD_ENTRIES_CRC,
    GPT4096_BAD_PARTITION_RANGE,
    GPT4096_NOT_FOUND,
    GPT4096_AMBIGUOUS,
    GPT4096_PARTITION_OVERLAP
};

struct gpt4096_plan {
    uint64_t device_blocks, backup_lba, first_usable, last_usable;
    uint64_t entry_lba, entry_bytes, entry_blocks;
    uint32_t entry_count, entry_size, entry_crc32;
};
struct gpt4096_partition {
    uint64_t first_lba, last_lba, block_count;
    uint32_t entry_index;
};

uint32_t gpt4096_crc32(const uint8_t *data, size_t length);
/* device_blocks MUST come from independent LUN capacity, never this header.
 * max_table_bytes is the caller's actual bounded read-buffer capacity.
 * A failed call leaves output zeroed. Header/entries inputs are never changed.
 */
enum gpt4096_status gpt4096_header(const uint8_t *header, size_t header_bytes,
    uint64_t device_blocks, size_t max_table_bytes, struct gpt4096_plan *out);
/* Pass only a successful, unmodified plan; do not reparse header from a reused
 * DMA bounce buffer. The full declared table must be present, even if a matching
 * name appears in an early entry. CRC covers entry_bytes, excluding block pad.
 */
enum gpt4096_status gpt4096_find(const struct gpt4096_plan *plan,
    const uint8_t *entries, size_t available_bytes, const char *want,
    struct gpt4096_partition *out);
#endif
