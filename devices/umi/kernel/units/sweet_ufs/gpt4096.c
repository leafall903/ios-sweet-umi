#include "gpt4096.h"

static uint32_t
le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static uint32_t
crc_byte(uint32_t crc, uint8_t b)
{
    crc ^= b;
    for (unsigned i = 0; i < 8; i++)
        crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    return crc;
}

uint32_t
gpt4096_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; i++)
        crc = crc_byte(crc, data[i]);
    return ~crc;
}

enum gpt4096_status
gpt4096_header(const uint8_t *h, size_t header_bytes, uint64_t device_blocks,
    size_t max_table_bytes, struct gpt4096_plan *out)
{
    static const uint8_t signature[8] = { 'E','F','I',' ','P','A','R','T' };
    struct gpt4096_plan p = {0};
    uint32_t header_size, crc = UINT32_MAX;
    if (!out)
        return GPT4096_BAD_ARGUMENT;
    *out = p;
    if (!h || header_bytes < GPT4096_BLOCK_BYTES || device_blocks < 4)
        return GPT4096_BAD_ARGUMENT;
    for (unsigned i = 0; i < sizeof(signature); i++)
        if (h[i] != signature[i])
            return GPT4096_BAD_SIGNATURE;
    /* Deliberately support GPT 1.0 only; other revisions require review. */
    if (le32(h + 8) != 0x00010000u)
        return GPT4096_BAD_REVISION;
    header_size = le32(h + 12);
    if (header_size < 92 || header_size > GPT4096_BLOCK_BYTES)
        return GPT4096_BAD_HEADER_SIZE;
    for (uint32_t i = 0; i < header_size; i++)
        crc = crc_byte(crc, i >= 16 && i < 20 ? 0 : h[i]);
    if (~crc != le32(h + 16))
        return GPT4096_BAD_HEADER_CRC;

    p.device_blocks = device_blocks;
    p.backup_lba = le64(h + 32);
    p.first_usable = le64(h + 40);
    p.last_usable = le64(h + 48);
    p.entry_lba = le64(h + 72);
    p.entry_count = le32(h + 80);
    p.entry_size = le32(h + 84);
    p.entry_crc32 = le32(h + 88);
    if (le32(h + 20) != 0 || le64(h + 24) != 1 ||
        p.backup_lba != device_blocks - 1 || p.first_usable <= 1 ||
        p.first_usable > p.last_usable || p.last_usable >= p.backup_lba)
        return GPT4096_BAD_GEOMETRY;
    /* GPT 1.0 entry size is 128 * 2^n; count/location are not hardcoded. */
    if (!p.entry_count || p.entry_size < 128 ||
        (p.entry_size & (p.entry_size - 1)) != 0)
        return GPT4096_BAD_ENTRIES_SHAPE;
    if ((uint64_t)p.entry_count > UINT64_MAX / p.entry_size)
        return GPT4096_TABLE_TOO_LARGE;
    p.entry_bytes = (uint64_t)p.entry_count * p.entry_size;
    p.entry_blocks = p.entry_bytes / GPT4096_BLOCK_BYTES +
        (p.entry_bytes % GPT4096_BLOCK_BYTES != 0);
    /* Check rounded DMA read size too, not just CRC payload bytes. */
    if (p.entry_bytes > SIZE_MAX || p.entry_bytes > max_table_bytes ||
        p.entry_blocks > max_table_bytes / GPT4096_BLOCK_BYTES)
        return GPT4096_TABLE_TOO_LARGE;
    /* Primary table must fit between primary header and usable region.
     * Leave enough space for a backup table between usable end and backup
     * header, without assuming its exact placement from the primary header.
     * Subtraction form avoids overflow in extent addition/rounding.
     */
    if (p.entry_lba < 2 || p.entry_lba >= p.first_usable ||
        p.entry_blocks > p.first_usable - p.entry_lba ||
        p.entry_blocks > p.backup_lba - p.last_usable - 1)
        return GPT4096_BAD_GEOMETRY;
    *out = p;
    return GPT4096_OK;
}

static int
unused_entry(const uint8_t *e)
{
    uint8_t any = 0;
    for (unsigned i = 0; i < 16; i++)
        any |= e[i];
    return any == 0; /* Type GUID, not unique-partition GUID, defines unused. */
}

static int
name_matches(const uint8_t *e, const char *want, size_t length)
{
    for (size_t i = 0; i < length; i++)
        if (e[56 + 2 * i] != (uint8_t)want[i] || e[57 + 2 * i] != 0)
            return 0;
    /* A 36-character name fills the field; shorter names must terminate.
     * Non-ASCII suffixes never truncate to the requested ASCII name.
     */
    return length == 36 || (e[56 + 2 * length] == 0 && e[57 + 2 * length] == 0);
}

enum gpt4096_status
gpt4096_find(const struct gpt4096_plan *p, const uint8_t *entries,
    size_t available_bytes, const char *want, struct gpt4096_partition *out)
{
    struct gpt4096_partition found = {0};
    size_t length = 0;
    uint32_t matches = 0;
    if (!out)
        return GPT4096_BAD_ARGUMENT;
    *out = found;
    if (!p || !entries || !want || !p->entry_count || p->entry_size < 128 ||
        p->entry_bytes != (uint64_t)p->entry_count * p->entry_size ||
        p->entry_bytes > available_bytes)
        return GPT4096_BAD_ARGUMENT;
    while (length <= 36 && want[length]) {
        if ((uint8_t)want[length] > 0x7f)
            return GPT4096_BAD_ARGUMENT;
        length++;
    }
    if (!length || length > 36)
        return GPT4096_BAD_ARGUMENT;
    if (gpt4096_crc32(entries, (size_t)p->entry_bytes) != p->entry_crc32)
        return GPT4096_BAD_ENTRIES_CRC;
    for (uint32_t i = 0; i < p->entry_count; i++) {
        const uint8_t *e = entries + (size_t)i * p->entry_size;
        uint64_t first, last;
        if (unused_entry(e))
            continue;
        first = le64(e + 32);
        last = le64(e + 40);
        if (first < p->first_usable || first > last || last > p->last_usable)
            return GPT4096_BAD_PARTITION_RANGE;
        if (name_matches(e, want, length)) {
            found.first_lba = first;
            found.last_lba = last;
            found.block_count = last - first + 1;
            found.entry_index = i;
            matches++;
        }
    }
    if (!matches)
        return GPT4096_NOT_FOUND;
    if (matches != 1)
        return GPT4096_AMBIGUOUS;
    /* The chosen writable extent must not alias another active partition. */
    for (uint32_t i = 0; i < p->entry_count; i++) {
        const uint8_t *e = entries + (size_t)i * p->entry_size;
        if (i != found.entry_index && !unused_entry(e) &&
            le64(e + 32) <= found.last_lba && le64(e + 40) >= found.first_lba)
            return GPT4096_PARTITION_OVERLAP;
    }
    *out = found;
    return GPT4096_OK;
}
