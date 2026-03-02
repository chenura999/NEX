/*
 * NEX Compress — Container Format
 * .nex file format reader/writer with chunk table
 */

#include "nex_internal.h"

/* ═══════════════════════════════════════════════════════════════════
 * Container Layout:
 *
 * [Header: 30 bytes]
 *   Magic:          4 bytes  "NEX\x01"
 *   Version:        2 bytes  uint16 LE
 *   Flags:          2 bytes  uint16 LE
 *   Original Size:  8 bytes  uint64 LE
 *   Chunk Count:    4 bytes  uint32 LE
 *   Checksum:       8 bytes  uint64 LE (XXH64 of original)
 *   Reserved:       2 bytes
 *
 * [Chunk Table: chunk_count × 21 bytes]
 *   Per entry:
 *     Compressed Offset: 8 bytes uint64 LE
 *     Compressed Size:   4 bytes uint32 LE
 *     Original Size:     4 bytes uint32 LE
 *     Pipeline ID:       1 byte  uint8
 *     Checksum:          4 bytes uint32 LE (XXH32 of original chunk)
 *
 * [Chunk Data 0 ... N]
 *
 * [Footer: 4 bytes]
 *   Magic: "XEN\x01"
 *
 * ═══════════════════════════════════════════════════════════════════ */

#define NEX_HEADER_SIZE      30
#define NEX_CHUNK_ENTRY_SIZE 21
#define NEX_FOOTER_SIZE      4

/* ── Write Header ────────────────────────────────────────────────── */

nex_status_t nex_write_header(nex_buffer_t *out, const nex_header_t *hdr) {
    size_t needed = out->size + NEX_HEADER_SIZE;
    if (out->capacity < needed) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, needed);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = needed;
    }

    uint8_t *p = out->data + out->size;

    /* Magic */
    uint32_t magic = NEX_MAGIC;
    memcpy(p, &magic, 4); p += 4;

    /* Version */
    memcpy(p, &hdr->version, 2); p += 2;

    /* Flags */
    memcpy(p, &hdr->flags, 2); p += 2;

    /* Original size */
    memcpy(p, &hdr->original_size, 8); p += 8;

    /* Chunk count */
    memcpy(p, &hdr->chunk_count, 4); p += 4;

    /* Checksum */
    memcpy(p, &hdr->checksum, 8); p += 8;

    /* Reserved */
    uint16_t reserved = 0;
    memcpy(p, &reserved, 2); p += 2;

    out->size = needed;
    return NEX_OK;
}

/* ── Read Header ─────────────────────────────────────────────────── */

nex_status_t nex_read_header(const uint8_t *data, size_t size,
                              nex_header_t *hdr) {
    if (size < NEX_HEADER_SIZE) return NEX_ERR_FORMAT;

    const uint8_t *p = data;

    /* Verify magic */
    uint32_t magic;
    memcpy(&magic, p, 4); p += 4;
    if (magic != NEX_MAGIC) return NEX_ERR_FORMAT;

    memcpy(&hdr->version, p, 2); p += 2;
    if (hdr->version > NEX_FORMAT_VER) return NEX_ERR_UNSUPPORTED;

    memcpy(&hdr->flags, p, 2); p += 2;
    memcpy(&hdr->original_size, p, 8); p += 8;
    memcpy(&hdr->chunk_count, p, 4); p += 4;
    memcpy(&hdr->checksum, p, 8); p += 8;
    /* skip reserved 2 bytes */

    hdr->magic = magic;
    return NEX_OK;
}

/* ── Write Chunk Table ───────────────────────────────────────────── */

nex_status_t nex_write_chunk_table(nex_buffer_t *out,
                                    const nex_chunk_entry_t *entries,
                                    uint32_t count) {
    size_t table_size = (size_t)count * NEX_CHUNK_ENTRY_SIZE;
    size_t needed = out->size + table_size;

    if (out->capacity < needed) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, needed);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = needed;
    }

    uint8_t *p = out->data + out->size;

    for (uint32_t i = 0; i < count; i++) {
        const nex_chunk_entry_t *e = &entries[i];
        memcpy(p, &e->compressed_offset, 8); p += 8;
        memcpy(p, &e->compressed_size, 4);   p += 4;
        memcpy(p, &e->original_size, 4);     p += 4;
        *p++ = e->pipeline_id;
        memcpy(p, &e->checksum, 4);          p += 4;
    }

    out->size = needed;
    return NEX_OK;
}

/* ── Read Chunk Table ────────────────────────────────────────────── */

nex_status_t nex_read_chunk_table(const uint8_t *data, size_t size,
                                   nex_chunk_entry_t **entries,
                                   uint32_t count) {
    size_t table_size = (size_t)count * NEX_CHUNK_ENTRY_SIZE;
    if (size < table_size) return NEX_ERR_CORRUPT;

    *entries = (nex_chunk_entry_t *)malloc(count * sizeof(nex_chunk_entry_t));
    if (!*entries) return NEX_ERR_NOMEM;

    const uint8_t *p = data;

    for (uint32_t i = 0; i < count; i++) {
        nex_chunk_entry_t *e = &(*entries)[i];
        memcpy(&e->compressed_offset, p, 8); p += 8;
        memcpy(&e->compressed_size, p, 4);   p += 4;
        memcpy(&e->original_size, p, 4);     p += 4;
        e->pipeline_id = *p++;
        memcpy(&e->checksum, p, 4);          p += 4;
    }

    return NEX_OK;
}

/* ── Write Footer ────────────────────────────────────────────────── */

nex_status_t nex_write_footer(nex_buffer_t *out) {
    size_t needed = out->size + NEX_FOOTER_SIZE;
    if (out->capacity < needed) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, needed);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = needed;
    }

    uint8_t *p = out->data + out->size;
    uint32_t footer = 0x014E4558U; /* "XEN\x01" LE */
    memcpy(p, &footer, 4);

    out->size = needed;
    return NEX_OK;
}
