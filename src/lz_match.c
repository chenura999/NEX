/*
 * NEX Compress — LZ Pattern Matching
 * Dictionary-based compression with hash chain matching
 */

#include "nex_internal.h"

/* ── LZ Token Serialization Format ───────────────────────────────
 *
 * Token stream format:
 *   [4 bytes] original size (uint32 LE)
 *   [4 bytes] token count (uint32 LE)
 *   For each token:
 *     [1 byte] flags:
 *       bit 0: is_match
 *       bits 1-2: offset encoding (0=8bit, 1=16bit, 2=32bit)
 *       bits 3-4: length encoding (0=8bit, 1=16bit)
 *     if literal: [1 byte] value
 *     if match: [length field] + [offset field]
 *
 * ────────────────────────────────────────────────────────────────── */

/* ── Rolling Hash ────────────────────────────────────────────────── */

static inline uint32_t lz_hash4(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 0x9E3779B1U) >> (32 - NEX_LZ_HASH_BITS);
}

/* ── Hash Chain Match Finder ─────────────────────────────────────── */

typedef struct {
    uint32_t *hash_table;   /* head of chain for each hash     */
    uint32_t *chain;        /* prev[] array for chaining       */
    const uint8_t *window;
    size_t    window_size;
    int       max_chain;
    int       min_match;
    int       max_match;
} lz_match_finder_t;

static nex_status_t lz_finder_init(lz_match_finder_t *mf,
                                    const uint8_t *data, size_t size,
                                    int level) {
    mf->window = data;
    mf->window_size = size;
    mf->min_match = NEX_LZ_MIN_MATCH;
    mf->max_match = NEX_LZ_MAX_MATCH;

    /* Scale chain depth and window with level */
    if (level <= 3) {
        mf->max_chain = 4;
    } else if (level <= 6) {
        mf->max_chain = 32;
    } else {
        mf->max_chain = NEX_LZ_MAX_CHAIN;
    }

    mf->hash_table = (uint32_t *)calloc(NEX_LZ_HASH_SIZE, sizeof(uint32_t));
    mf->chain = (uint32_t *)calloc(size, sizeof(uint32_t));

    if (!mf->hash_table || !mf->chain) {
        free(mf->hash_table);
        free(mf->chain);
        return NEX_ERR_NOMEM;
    }

    /* Initialize to sentinel */
    memset(mf->hash_table, 0xFF, NEX_LZ_HASH_SIZE * sizeof(uint32_t));
    return NEX_OK;
}

static void lz_finder_free(lz_match_finder_t *mf) {
    free(mf->hash_table);
    free(mf->chain);
}

/* Find best match at position pos */
static void lz_find_match(lz_match_finder_t *mf, size_t pos,
                           uint32_t *best_len, uint32_t *best_offset) {
    *best_len = 0;
    *best_offset = 0;

    if (pos + mf->min_match > mf->window_size) return;

    uint32_t h = lz_hash4(mf->window + pos);
    uint32_t cur = mf->hash_table[h];
    int chain_count = 0;

    /* Max distance: level-dependent, cap at position */
    size_t max_dist = mf->window_size; /* full window for now */

    while (cur != 0xFFFFFFFF && chain_count < mf->max_chain) {
        if (pos - cur > max_dist) break;
        if (cur >= pos) break;

        const uint8_t *ref = mf->window + cur;
        const uint8_t *src = mf->window + pos;
        size_t max_len = NEX_MIN((size_t)mf->max_match, mf->window_size - pos);

        /* Quick check: compare first and last bytes of current best */
        if (*best_len >= (uint32_t)mf->min_match) {
            if (ref[*best_len] != src[*best_len] || ref[0] != src[0]) {
                cur = mf->chain[cur];
                chain_count++;
                continue;
            }
        }

        /* Measure match length */
        size_t len = 0;
        while (len < max_len && ref[len] == src[len]) len++;

        if (len >= (size_t)mf->min_match && len > *best_len) {
            *best_len = (uint32_t)len;
            *best_offset = (uint32_t)(pos - cur);
            if (len == max_len) break; /* can't do better */
        }

        cur = mf->chain[cur];
        chain_count++;
    }
}

static void lz_update_hash(lz_match_finder_t *mf, size_t pos) {
    if (pos + 4 > mf->window_size) return;
    uint32_t h = lz_hash4(mf->window + pos);
    mf->chain[pos] = mf->hash_table[h];
    mf->hash_table[h] = (uint32_t)pos;
}

/* ── Lazy Match Selection ────────────────────────────────────────── */

static bool lz_lazy_better(uint32_t len1, uint32_t off1,
                            uint32_t len2, uint32_t off2) {
    /* Is match2 better than match1? Prefer longer matches,
     * break ties by preferring closer (smaller offset) matches */
    if (len2 > len1 + 1) return true;
    if (len2 == len1 + 1 && off2 < off1) return true;
    return false;
}

/* ── Token Sequence Builder ──────────────────────────────────────── */

static nex_status_t lz_seq_init(nex_lz_sequence_t *seq, size_t capacity) {
    seq->tokens = (nex_lz_token_t *)malloc(capacity * sizeof(nex_lz_token_t));
    if (!seq->tokens) return NEX_ERR_NOMEM;
    seq->count = 0;
    seq->capacity = capacity;
    return NEX_OK;
}

static nex_status_t lz_seq_push(nex_lz_sequence_t *seq, nex_lz_token_t tok) {
    if (seq->count >= seq->capacity) {
        size_t new_cap = seq->capacity * 2;
        nex_lz_token_t *new_tokens = (nex_lz_token_t *)realloc(
            seq->tokens, new_cap * sizeof(nex_lz_token_t));
        if (!new_tokens) return NEX_ERR_NOMEM;
        seq->tokens = new_tokens;
        seq->capacity = new_cap;
    }
    seq->tokens[seq->count++] = tok;
    return NEX_OK;
}

static void lz_seq_free(nex_lz_sequence_t *seq) {
    free(seq->tokens);
    seq->tokens = NULL;
    seq->count = 0;
}

/* ── Serialize Token Stream ──────────────────────────────────────── */

static nex_status_t lz_serialize(const nex_lz_sequence_t *seq,
                                  uint32_t original_size,
                                  nex_buffer_t *out) {
    /* Estimate output size: worst case = 8 bytes header + 6 per token */
    size_t est = 8 + seq->count * 6;
    if (out->capacity < est) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, est);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = est;
    }

    uint8_t *p = out->data;

    /* Header */
    memcpy(p, &original_size, 4); p += 4;
    uint32_t count = (uint32_t)seq->count;
    memcpy(p, &count, 4); p += 4;

    for (size_t i = 0; i < seq->count; i++) {
        const nex_lz_token_t *tok = &seq->tokens[i];

        if (!tok->is_match) {
            /* Literal: flags=0x00, then byte */
            *p++ = 0x00;
            *p++ = tok->literal;
        } else {
            /* Match: flags encode offset/length sizes */
            uint8_t flags = 0x01;  /* bit 0 = is_match */
            uint8_t offset_size, length_size;

            if (tok->offset <= 0xFF) {
                offset_size = 1; /* bits 1-2 = 0 → 8-bit */
            } else if (tok->offset <= 0xFFFF) {
                offset_size = 2; flags |= (1 << 1);
            } else {
                offset_size = 4; flags |= (2 << 1);
            }

            if (tok->length <= 0xFF) {
                length_size = 1;
            } else {
                length_size = 2; flags |= (1 << 3);
            }

            *p++ = flags;

            /* Write length */
            if (length_size == 1) {
                *p++ = (uint8_t)tok->length;
            } else {
                uint16_t len16 = tok->length;
                memcpy(p, &len16, 2); p += 2;
            }

            /* Write offset */
            if (offset_size == 1) {
                *p++ = (uint8_t)tok->offset;
            } else if (offset_size == 2) {
                uint16_t off16 = (uint16_t)tok->offset;
                memcpy(p, &off16, 2); p += 2;
            } else {
                memcpy(p, &tok->offset, 4); p += 4;
            }
        }
    }

    out->size = (size_t)(p - out->data);
    return NEX_OK;
}

/* ── Deserialize Token Stream ────────────────────────────────────── */

static nex_status_t lz_deserialize(const uint8_t *data, size_t size,
                                    nex_lz_sequence_t *seq,
                                    uint32_t *original_size) {
    if (size < 8) return NEX_ERR_CORRUPT;

    const uint8_t *p = data;
    memcpy(original_size, p, 4); p += 4;

    uint32_t count;
    memcpy(&count, p, 4); p += 4;

    nex_status_t st = lz_seq_init(seq, count);
    if (st != NEX_OK) return st;

    const uint8_t *end = data + size;

    for (uint32_t i = 0; i < count; i++) {
        if (p >= end) { lz_seq_free(seq); return NEX_ERR_CORRUPT; }

        nex_lz_token_t tok;
        memset(&tok, 0, sizeof(tok));

        uint8_t flags = *p++;
        tok.is_match = flags & 0x01;

        if (!tok.is_match) {
            if (p >= end) { lz_seq_free(seq); return NEX_ERR_CORRUPT; }
            tok.literal = *p++;
        } else {
            int offset_enc = (flags >> 1) & 0x03;
            int length_enc = (flags >> 3) & 0x01;

            /* Read length */
            if (length_enc == 0) {
                if (p >= end) { lz_seq_free(seq); return NEX_ERR_CORRUPT; }
                tok.length = *p++;
            } else {
                if (p + 2 > end) { lz_seq_free(seq); return NEX_ERR_CORRUPT; }
                uint16_t len16;
                memcpy(&len16, p, 2); p += 2;
                tok.length = len16;
            }

            /* Read offset */
            if (offset_enc == 0) {
                if (p >= end) { lz_seq_free(seq); return NEX_ERR_CORRUPT; }
                tok.offset = *p++;
            } else if (offset_enc == 1) {
                if (p + 2 > end) { lz_seq_free(seq); return NEX_ERR_CORRUPT; }
                uint16_t off16;
                memcpy(&off16, p, 2); p += 2;
                tok.offset = off16;
            } else {
                if (p + 4 > end) { lz_seq_free(seq); return NEX_ERR_CORRUPT; }
                memcpy(&tok.offset, p, 4); p += 4;
            }
        }

        lz_seq_push(seq, tok);
    }

    return NEX_OK;
}

/* ── LZ Compress ─────────────────────────────────────────────────── */

nex_status_t nex_lz_compress(const uint8_t *in, size_t in_size,
                              nex_buffer_t *out, int level) {
    if (in_size == 0) {
        out->size = 0;
        return NEX_OK;
    }

    lz_match_finder_t mf;
    nex_status_t st = lz_finder_init(&mf, in, in_size, level);
    if (st != NEX_OK) return st;

    nex_lz_sequence_t seq;
    st = lz_seq_init(&seq, in_size / 2 + 256);
    if (st != NEX_OK) {
        lz_finder_free(&mf);
        return st;
    }

    bool use_lazy = (level >= 4);
    size_t pos = 0;

    while (pos < in_size) {
        /* Update hash for current position */
        lz_update_hash(&mf, pos);

        /* Find match */
        uint32_t match_len = 0, match_off = 0;
        lz_find_match(&mf, pos, &match_len, &match_off);

        if (match_len >= (uint32_t)mf.min_match) {
            /* Lazy matching: check next position */
            if (use_lazy && pos + 1 < in_size) {
                lz_update_hash(&mf, pos + 1);
                uint32_t lazy_len = 0, lazy_off = 0;
                lz_find_match(&mf, pos + 1, &lazy_len, &lazy_off);

                if (lz_lazy_better(match_len, match_off, lazy_len, lazy_off)) {
                    /* Emit literal for current byte, use lazy match */
                    nex_lz_token_t lit = {0, in[pos], 0, 0};
                    lz_seq_push(&seq, lit);
                    pos++;
                    match_len = lazy_len;
                    match_off = lazy_off;
                }
            }

            /* Emit match */
            nex_lz_token_t match_tok = {1, 0, (uint16_t)match_len, match_off};
            lz_seq_push(&seq, match_tok);

            /* Update hash for skipped positions */
            for (uint32_t i = 1; i < match_len && pos + i < in_size; i++) {
                lz_update_hash(&mf, pos + i);
            }
            pos += match_len;
        } else {
            /* Emit literal */
            nex_lz_token_t lit = {0, in[pos], 0, 0};
            lz_seq_push(&seq, lit);
            pos++;
        }
    }

    /* Serialize token stream */
    st = lz_serialize(&seq, (uint32_t)in_size, out);

    lz_seq_free(&seq);
    lz_finder_free(&mf);
    return st;
}

/* ── LZ Decompress ───────────────────────────────────────────────── */

nex_status_t nex_lz_decompress(const uint8_t *in, size_t in_size,
                                nex_buffer_t *out, int level) {
    (void)level;

    nex_lz_sequence_t seq;
    uint32_t original_size;

    nex_status_t st = lz_deserialize(in, in_size, &seq, &original_size);
    if (st != NEX_OK) return st;

    /* Allocate output */
    if (out->capacity < original_size) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, original_size);
        if (!new_data) {
            lz_seq_free(&seq);
            return NEX_ERR_NOMEM;
        }
        out->data = new_data;
        out->capacity = original_size;
    }

    size_t pos = 0;
    for (size_t i = 0; i < seq.count; i++) {
        const nex_lz_token_t *tok = &seq.tokens[i];

        if (!tok->is_match) {
            if (pos >= original_size) {
                lz_seq_free(&seq);
                return NEX_ERR_CORRUPT;
            }
            out->data[pos++] = tok->literal;
        } else {
            if (tok->offset == 0 || tok->offset > pos) {
                lz_seq_free(&seq);
                return NEX_ERR_CORRUPT;
            }
            size_t ref = pos - tok->offset;
            for (uint16_t j = 0; j < tok->length; j++) {
                if (pos >= original_size) {
                    lz_seq_free(&seq);
                    return NEX_ERR_CORRUPT;
                }
                out->data[pos++] = out->data[ref + j];
            }
        }
    }

    out->size = pos;
    lz_seq_free(&seq);
    return NEX_OK;
}
