/*
 * NEX Compress — LZ Fast Encoder (V5)
 * Single-pass LZ4-style encoder with direct compact output.
 * No intermediate token lists. No entropy stage. Pure speed.
 *
 * Format per sequence:
 *   [token] [extra_lit_len?] [literals...] [offset:2LE] [extra_match_len?]
 *
 * token byte: high 4 bits = literal count, low 4 bits = match_length - 4
 * If lit_count >= 15: extra bytes follow (add 255 until remainder < 255)
 * If match_length >= 19: extra bytes follow similarly
 * Last sequence has no offset or match_length (end-of-block literals).
 *
 * Header: [original_size:4LE] [compressed_payload...]
 */

#include "nex_internal.h"

/* ── Hash Table ─────────────────────────────────────────────────── */

#define LZF_HASH_BITS    17
#define LZF_HASH_SIZE    (1 << LZF_HASH_BITS)
#define LZF_MIN_MATCH    4
#define LZF_MAX_DISTANCE 65535
#define LZF_ML_BITS      4
#define LZF_ML_MASK      ((1U << LZF_ML_BITS) - 1)
#define LZF_RUN_BITS     4
#define LZF_RUN_MASK     ((1U << LZF_RUN_BITS) - 1)

static inline uint32_t lzf_hash(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761U) >> (32 - LZF_HASH_BITS);
}

/* ── Write variable-length extra count ──────────────────────────── */

static inline uint8_t *lzf_write_count(uint8_t *op, size_t count) {
    while (count >= 255) {
        *op++ = 255;
        count -= 255;
    }
    *op++ = (uint8_t)count;
    return op;
}

/* ── Read variable-length extra count ───────────────────────────── */

static inline size_t lzf_read_count(const uint8_t **pp, const uint8_t *end) {
    size_t count = 0;
    while (*pp < end && **pp == 255) {
        count += 255;
        (*pp)++;
    }
    if (*pp < end) {
        count += **pp;
        (*pp)++;
    }
    return count;
}

/* ═══════════════════════════════════════════════════════════════════
 * LZ Fast Compress — Single-Pass Greedy
 * ═══════════════════════════════════════════════════════════════════ */

nex_status_t nex_lz_fast_compress(const uint8_t *in, size_t in_size,
                                   nex_buffer_t *out, int level,
                                   const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;

    if (in_size == 0) {
        out->size = 0;
        return NEX_OK;
    }

    /* Worst case: 4-byte header + slightly expanded data */
    size_t max_out = 4 + in_size + (in_size / 255) + 16;
    if (out->capacity < max_out) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, max_out);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = max_out;
    }

    /* Hash table — positions of 4-byte hashes */
    uint32_t *htable = (uint32_t *)calloc(LZF_HASH_SIZE, sizeof(uint32_t));
    if (!htable) return NEX_ERR_NOMEM;
    memset(htable, 0xFF, LZF_HASH_SIZE * sizeof(uint32_t));

    uint8_t *op = out->data;
    const uint8_t *ip = in;
    const uint8_t *in_end = in + in_size;
    const uint8_t *match_limit = in_end - 5; /* need 5 bytes for safe read */
    const uint8_t *lit_start = ip;

    /* Write original size header */
    uint32_t orig_size = (uint32_t)in_size;
    memcpy(op, &orig_size, 4);
    op += 4;

    if (in_size < LZF_MIN_MATCH + 1) {
        /* Too small for any match — emit as all-literal sequence */
        goto emit_last_literals;
    }

    /* Hash the first position */
    htable[lzf_hash(ip)] = 0;
    ip++;

    while (ip < match_limit) {
        /* Probe hash table for a match */
        uint32_t h = lzf_hash(ip);
        uint32_t ref_pos = htable[h];
        const uint8_t *ref = in + ref_pos;
        htable[h] = (uint32_t)(ip - in);

        /* Check if match is valid */
        if (ref_pos == 0xFFFFFFFF ||
            (size_t)(ip - ref) > LZF_MAX_DISTANCE ||
            ref < in) {
            ip++;
            continue;
        }

        /* Verify 4-byte match */
        uint32_t v1, v2;
        memcpy(&v1, ip, 4);
        memcpy(&v2, ref, 4);
        if (v1 != v2) {
            ip++;
            continue;
        }

        /* ── Found a match! ── */

        /* Extend match forward */
        size_t match_len = 4;
        size_t max_extend = (size_t)(in_end - ip);
        if (max_extend > LZF_MAX_DISTANCE) max_extend = LZF_MAX_DISTANCE;
        while (match_len < max_extend && ip[match_len] == ref[match_len]) {
            match_len++;
        }

        /* Emit literal run + match */
        size_t lit_len = (size_t)(ip - lit_start);
        size_t ml_code = match_len - LZF_MIN_MATCH;

        /* Token byte */
        uint8_t *token_ptr = op++;
        uint8_t token = 0;

        /* Literal length in token */
        if (lit_len >= LZF_RUN_MASK) {
            token = (LZF_RUN_MASK << LZF_ML_BITS);
            op = lzf_write_count(op, lit_len - LZF_RUN_MASK);
        } else {
            token = (uint8_t)(lit_len << LZF_ML_BITS);
        }

        /* Match length in token */
        if (ml_code >= LZF_ML_MASK) {
            token |= LZF_ML_MASK;
        } else {
            token |= (uint8_t)ml_code;
        }
        *token_ptr = token;

        /* Copy literals */
        memcpy(op, lit_start, lit_len);
        op += lit_len;

        /* Offset (2-byte LE) */
        uint16_t offset = (uint16_t)(ip - ref);
        memcpy(op, &offset, 2);
        op += 2;

        /* Extra match length */
        if (ml_code >= LZF_ML_MASK) {
            op = lzf_write_count(op, ml_code - LZF_ML_MASK);
        }

        /* Update hash for skipped positions */
        ip += match_len;
        lit_start = ip;

        /* Hash the positions we're skipping over (at least current) */
        if (ip < match_limit) {
            htable[lzf_hash(ip - 2)] = (uint32_t)(ip - 2 - in);
        }
    }

emit_last_literals:;
    /* Emit remaining literals as final sequence (no match) */
    size_t last_lit = (size_t)(in_end - lit_start);
    if (last_lit > 0) {
        uint8_t *token_ptr = op++;
        if (last_lit >= LZF_RUN_MASK) {
            *token_ptr = (LZF_RUN_MASK << LZF_ML_BITS);
            op = lzf_write_count(op, last_lit - LZF_RUN_MASK);
        } else {
            *token_ptr = (uint8_t)(last_lit << LZF_ML_BITS);
        }
        memcpy(op, lit_start, last_lit);
        op += last_lit;
    }

    out->size = (size_t)(op - out->data);
    free(htable);
    return NEX_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * LZ Fast Decompress — Token-by-Token
 * ═══════════════════════════════════════════════════════════════════ */

nex_status_t nex_lz_fast_decompress(const uint8_t *in, size_t in_size,
                                     nex_buffer_t *out, int level,
                                     const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;

    if (in_size < 4) return NEX_ERR_CORRUPT;

    const uint8_t *ip = in;
    const uint8_t *in_end = in + in_size;

    /* Read original size */
    uint32_t orig_size;
    memcpy(&orig_size, ip, 4);
    ip += 4;

    if (out->capacity < orig_size) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, orig_size);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = orig_size;
    }

    uint8_t *op = out->data;
    uint8_t *out_end = out->data + orig_size;

    while (ip < in_end) {
        /* Read token */
        uint8_t token = *ip++;
        size_t lit_len = (token >> LZF_ML_BITS) & LZF_RUN_MASK;

        /* Extra literal length */
        if (lit_len == LZF_RUN_MASK) {
            lit_len += lzf_read_count(&ip, in_end);
        }

        /* Copy literals */
        if (lit_len > 0) {
            if (ip + lit_len > in_end || op + lit_len > out_end) {
                return NEX_ERR_CORRUPT;
            }
            memcpy(op, ip, lit_len);
            ip += lit_len;
            op += lit_len;
        }

        /* Check if this is the last sequence (no match follows) */
        if (ip >= in_end) break;

        /* Read offset */
        if (ip + 2 > in_end) return NEX_ERR_CORRUPT;
        uint16_t offset;
        memcpy(&offset, ip, 2);
        ip += 2;

        if (offset == 0) return NEX_ERR_CORRUPT;

        /* Match length */
        size_t match_len = (token & LZF_ML_MASK) + LZF_MIN_MATCH;
        if ((token & LZF_ML_MASK) == LZF_ML_MASK) {
            match_len += lzf_read_count(&ip, in_end);
        }

        /* Copy match (may overlap) */
        uint8_t *match_src = op - offset;
        if (match_src < out->data || op + match_len > out_end) {
            return NEX_ERR_CORRUPT;
        }

        /* Byte-by-byte copy for overlapping matches */
        for (size_t i = 0; i < match_len; i++) {
            op[i] = match_src[i];
        }
        op += match_len;
    }

    out->size = (size_t)(op - out->data);
    if (out->size != orig_size) return NEX_ERR_CORRUPT;

    return NEX_OK;
}
