/*
 * NEX Compress — LZ Pattern Matching (V2)
 * Advanced BT4 Match Finder and Viterbi Optimal Parser
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

/* ── Rolling Hash & SIMD Match EXT ───────────────────────────────── */

#if defined(__AVX2__)
#include <immintrin.h>
/* AVX2 32-byte SIMD Match Extender */
static inline size_t lz_extend_match(const uint8_t *src, const uint8_t *ref, size_t len, size_t max_len) {
    while (len + 32 <= max_len) {
        __m256i v_src = _mm256_loadu_si256((const __m256i*)(src + len));
        __m256i v_ref = _mm256_loadu_si256((const __m256i*)(ref + len));
        __m256i cmp = _mm256_cmpeq_epi8(v_src, v_ref);
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(cmp);
        if (mask != 0xFFFFFFFF) {
            return len + __builtin_ctz(~mask);
        }
        len += 32;
    }
    while (len + 8 <= max_len) {
        uint64_t sval, rval;
        memcpy(&sval, src + len, 8);
        memcpy(&rval, ref + len, 8);
        if (sval == rval) len += 8;
        else break;
    }
    while (len < max_len && src[len] == ref[len]) len++;
    return len;
}
#else
/* Fallback 64-bit Match Extender */
static inline size_t lz_extend_match(const uint8_t *src, const uint8_t *ref, size_t len, size_t max_len) {
    while (len + 8 <= max_len) {
        uint64_t sval, rval;
        memcpy(&sval, src + len, 8);
        memcpy(&rval, ref + len, 8);
        if (sval == rval) len += 8;
        else break;
    }
    while (len < max_len && src[len] == ref[len]) len++;
    return len;
}
#endif

static inline uint32_t lz_hash4(const uint8_t *p, size_t remaining) {
    uint32_t v = 0;
    if (NEX_LIKELY(remaining >= 4)) {
        memcpy(&v, p, 4);
    } else {
        if (remaining > 0) memcpy(&v, p, remaining);
    }
    return (v * 0x9E3779B1U) >> (32 - NEX_LZ_HASH_BITS);
}

/* ── Match Finder Structures ─────────────────────────────────────── */

typedef struct {
    uint32_t *hash_table;
    uint32_t *chain_or_son; 
    const uint8_t *window;
    size_t    window_size;
    int       max_chain;
    int       min_match;
    int       max_match;
    int       hash_bits;
    size_t    hash_size;
    bool      is_bt4;
} lz_match_finder_t;

static inline uint32_t lz_hash4_mf(const lz_match_finder_t *mf, const uint8_t *p, size_t remaining) {
    uint32_t v = 0;
    if (NEX_LIKELY(remaining >= 4)) {
        memcpy(&v, p, 4);
    } else {
        if (remaining > 0) memcpy(&v, p, remaining);
    }
    return (v * 0x9E3779B1U) >> (32 - mf->hash_bits);
}

static nex_status_t lz_finder_init(lz_match_finder_t *mf,
                                    const uint8_t *data, size_t size,
                                    int level) {
    mf->window = data;
    mf->window_size = size;
    mf->min_match = NEX_LZ_MIN_MATCH;
    mf->max_match = NEX_LZ_MAX_MATCH;
    mf->is_bt4 = (level >= 6);

    if (level <= 3) {
        mf->max_chain = 16;
    } else if (level <= 5) {
        mf->max_chain = 64;
    } else if (level <= 7) {
        mf->max_chain = 128;
    } else {
        mf->max_chain = 4096;
    }

    /* Dynamic hash table sizing based on level */
    mf->hash_bits = (level >= 6) ? 20 : NEX_LZ_HASH_BITS;
    mf->hash_size = (size_t)1 << mf->hash_bits;

    mf->hash_table = (uint32_t *)calloc(mf->hash_size, sizeof(uint32_t));
    if (mf->is_bt4) {
        mf->chain_or_son = (uint32_t *)malloc(size * 2 * sizeof(uint32_t));
    } else {
        mf->chain_or_son = (uint32_t *)malloc(size * sizeof(uint32_t));
    }

    if (!mf->hash_table || !mf->chain_or_son) {
        free(mf->hash_table);
        free(mf->chain_or_son);
        return NEX_ERR_NOMEM;
    }

    memset(mf->hash_table, 0xFF, mf->hash_size * sizeof(uint32_t));
    if (mf->is_bt4) {
        memset(mf->chain_or_son, 0xFF, size * 2 * sizeof(uint32_t));
    }
    return NEX_OK;
}

static void lz_finder_free(lz_match_finder_t *mf) {
    free(mf->hash_table);
    free(mf->chain_or_son);
}

/* ── Hash Chain ──────────────────────────────────────────────────── */

static void lz_find_match_hc(lz_match_finder_t *mf, size_t pos,
                              uint32_t *best_len, uint32_t *best_offset) {
    *best_len = 0;
    *best_offset = 0;

    if (pos + mf->min_match > mf->window_size) return;
    size_t remaining = mf->window_size - pos;
    uint32_t h = lz_hash4_mf(mf, mf->window + pos, remaining);
    uint32_t cur = mf->hash_table[h];
    int chain_count = 0;
    size_t max_dist = mf->window_size; 

    while (cur != 0xFFFFFFFF && chain_count < mf->max_chain) {
        if (pos - cur > max_dist) break;
        if (cur >= pos) break;

        const uint8_t *ref = mf->window + cur;
        const uint8_t *src = mf->window + pos;
        size_t max_len = NEX_MIN((size_t)mf->max_match, mf->window_size - pos);

        /* Quick check */
        if (*best_len >= (uint32_t)mf->min_match) {
            if (*best_len >= max_len || ref[*best_len] != src[*best_len] || ref[0] != src[0]) {
                cur = mf->chain_or_son[cur];
                chain_count++;
                continue;
            }
        }

        size_t len = lz_extend_match(src, ref, 0, max_len);

        if (len >= (size_t)mf->min_match && len > *best_len) {
            *best_len = (uint32_t)len;
            *best_offset = (uint32_t)(pos - cur);
            if (len == max_len) break; 
        }

        cur = mf->chain_or_son[cur];
        chain_count++;
    }
}

static void lz_update_hash_hc(lz_match_finder_t *mf, size_t pos) {
    if (pos + mf->min_match > mf->window_size) return;
    size_t remaining = mf->window_size - pos;
    uint32_t h = lz_hash4_mf(mf, mf->window + pos, remaining);
    mf->chain_or_son[pos] = mf->hash_table[h];
    mf->hash_table[h] = (uint32_t)pos;
}

/* ── BT4 Match Finder ────────────────────────────────────────────── */

static void lz_bt4_get_matches(lz_match_finder_t *mf, size_t pos, 
                               uint32_t *match_lens, uint32_t *match_offs, uint32_t *num_matches) {
    *num_matches = 0;
    if (pos + mf->min_match > mf->window_size) {
        return;
    }
    
    size_t remaining = mf->window_size - pos;
    uint32_t hash = lz_hash4_mf(mf, mf->window + pos, remaining);
    
    uint32_t cur = mf->hash_table[hash];
    mf->hash_table[hash] = (uint32_t)pos;
    
    uint32_t ptr0 = (uint32_t)(pos * 2);     
    uint32_t ptr1 = (uint32_t)(pos * 2 + 1); 
    
    uint32_t len0 = 0, len1 = 0;
    uint32_t max_len = NEX_MIN((uint32_t)mf->max_match, (uint32_t)remaining);
    
    int count = mf->max_chain;
    uint32_t matches_found = 0;
    uint32_t best_len = 0;
    
    while (cur != 0xFFFFFFFF && count-- > 0) {
        if (cur >= pos) break; 
        
        const uint8_t *src = mf->window + pos;
        const uint8_t *ref = mf->window + cur;
        uint32_t len = (uint32_t)lz_extend_match(src, ref, NEX_MIN(len0, len1), max_len);
        
        if (len > best_len && len >= (uint32_t)mf->min_match) {
            best_len = len;
            match_lens[matches_found] = len;
            match_offs[matches_found] = (uint32_t)(pos - cur);
            matches_found++;
            if (len == max_len) {
                mf->chain_or_son[ptr0] = mf->chain_or_son[cur * 2];
                mf->chain_or_son[ptr1] = mf->chain_or_son[cur * 2 + 1];
                *num_matches = matches_found;
                return;
            }
        }
        
        if (src[len] < ref[len]) {
            mf->chain_or_son[ptr1] = cur;
            ptr1 = cur * 2;
            cur = mf->chain_or_son[ptr1];
            len1 = len;
        } else {
            mf->chain_or_son[ptr0] = cur;
            ptr0 = cur * 2 + 1;
            cur = mf->chain_or_son[ptr0];
            len0 = len;
        }
    }
    mf->chain_or_son[ptr0] = 0xFFFFFFFF;
    mf->chain_or_son[ptr1] = 0xFFFFFFFF;
    *num_matches = matches_found;
}

static void lz_bt4_skip(lz_match_finder_t *mf, size_t pos) {
    uint32_t ml[128], mo[128], nm;
    lz_bt4_get_matches(mf, pos, ml, mo, &nm);
}

/* ── Lazy Match Selection (Fallback) ─────────────────────────────── */

static bool lz_lazy_better(uint32_t len1, uint32_t off1,
                            uint32_t len2, uint32_t off2) {
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

/* ── Serialize Token Stream (Packed Literal Runs) ────────────────── */
/*
 * Format V2: header (8 bytes) + packed tokens
 *   Header: [original_size:4] [token_count:4]
 *   Token types:
 *     Literal run:  0x00 [count:2] [raw bytes...]
 *     Match:        flags|0x01 [length:1-2] [offset:1-4]
 */

static nex_status_t lz_serialize(const nex_lz_sequence_t *seq,
                                  uint32_t original_size,
                                  nex_buffer_t *out) {
    /* Worst case: header + all literals as individual runs */
    size_t est = 8 + original_size + seq->count * 8;
    if (out->capacity < est) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, est);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = est;
    }

    uint8_t *p = out->data;

    memcpy(p, &original_size, 4); p += 4;
    uint32_t count = (uint32_t)seq->count;
    memcpy(p, &count, 4); p += 4;

    size_t i = 0;
    while (i < seq->count) {
        if (!seq->tokens[i].is_match) {
            /* Collect consecutive literals into a run */
            size_t run_start = i;
            while (i < seq->count && !seq->tokens[i].is_match && (i - run_start) < 65535) {
                i++;
            }
            uint16_t run_len = (uint16_t)(i - run_start);
            *p++ = 0x00;  /* literal run marker */
            memcpy(p, &run_len, 2); p += 2;
            for (size_t j = run_start; j < run_start + run_len; j++) {
                *p++ = seq->tokens[j].literal;
            }
        } else {
            const nex_lz_token_t *tok = &seq->tokens[i];
            uint8_t flags = 0x01; 
            uint8_t offset_size, length_size;

            if (tok->offset <= 0xFF) {
                offset_size = 1; 
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

            if (length_size == 1) {
                *p++ = (uint8_t)tok->length;
            } else {
                uint16_t len16 = tok->length;
                memcpy(p, &len16, 2); p += 2;
            }

            if (offset_size == 1) {
                *p++ = (uint8_t)tok->offset;
            } else if (offset_size == 2) {
                uint16_t off16 = (uint16_t)tok->offset;
                memcpy(p, &off16, 2); p += 2;
            } else {
                memcpy(p, &tok->offset, 4); p += 4;
            }
            i++;
        }
    }

    out->size = (size_t)(p - out->data);
    return NEX_OK;
}

/* ── Deserialize Token Stream (Packed Literal Runs) ──────────────── */

static nex_status_t lz_deserialize(const uint8_t *data, size_t size,
                                    nex_lz_sequence_t *seq,
                                    uint32_t *original_size) {
    if (size < 8) return NEX_ERR_CORRUPT;
    const uint8_t *p = data;
    memcpy(original_size, p, 4); p += 4;
    uint32_t count;
    memcpy(&count, p, 4); p += 4;

    nex_status_t st = lz_seq_init(seq, count > 0 ? count : 256);
    if (st != NEX_OK) return st;

    const uint8_t *end = data + size;

    while (p < end) {
        uint8_t flags = *p++;

        if (!(flags & 0x01)) {
            /* Packed literal run: 0x00 [count:2] [raw bytes...] */
            if (p + 2 > end) { lz_seq_free(seq); return NEX_ERR_CORRUPT; }
            uint16_t run_len;
            memcpy(&run_len, p, 2); p += 2;
            if (p + run_len > end) { lz_seq_free(seq); return NEX_ERR_CORRUPT; }
            for (uint16_t j = 0; j < run_len; j++) {
                nex_lz_token_t tok = {0, *p++, 0, 0};
                lz_seq_push(seq, tok);
            }
        } else {
            /* Match token */
            nex_lz_token_t tok;
            memset(&tok, 0, sizeof(tok));
            tok.is_match = 1;

            int offset_enc = (flags >> 1) & 0x03;
            int length_enc = (flags >> 3) & 0x01;

            if (length_enc == 0) {
                if (p >= end) { lz_seq_free(seq); return NEX_ERR_CORRUPT; }
                tok.length = *p++;
            } else {
                if (p + 2 > end) { lz_seq_free(seq); return NEX_ERR_CORRUPT; }
                uint16_t len16;
                memcpy(&len16, p, 2); p += 2;
                tok.length = len16;
            }

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
            lz_seq_push(seq, tok);
        }
    }
    return NEX_OK;
}

/* ── Viterbi Optimal Parser ──────────────────────────────────────── */

typedef struct {
    uint32_t price;
    uint32_t length;
    uint32_t offset;
} lz_opt_node_t;

static inline uint32_t lz_get_literal_cost(void) {
    return 16;
}

static inline uint32_t lz_get_match_cost(uint32_t length, uint32_t offset) {
    uint32_t cost = 8;
    cost += (length <= 0xFF) ? 8 : 16;
    cost += (offset <= 0xFF) ? 8 : ((offset <= 0xFFFF) ? 16 : 32);
    return cost;
}

static nex_status_t lz_compress_optimal(lz_match_finder_t *mf, const uint8_t *in, size_t in_size, size_t start_pos, nex_lz_sequence_t *seq) {
    lz_opt_node_t *nodes = (lz_opt_node_t *)malloc((in_size + 1) * sizeof(lz_opt_node_t));
    if (!nodes) return NEX_ERR_NOMEM;

    for (size_t i = 0; i <= in_size; i++) {
        nodes[i].price = 0xFFFFFFFF;
        nodes[i].length = 0;
        nodes[i].offset = 0;
    }
    nodes[start_pos].price = 0;

    size_t pos = 0;
    for (; pos < start_pos; pos++) {
        lz_bt4_skip(mf, pos);
    }

    uint32_t match_lens[128], match_offs[128];
    uint32_t num_matches;

    for (size_t i = 0; i < in_size; i++) {
        /* If unreachable, we don't process */
        if (nodes[i].price == 0xFFFFFFFF) continue;

        /* Find matches and add to BT4 */
        lz_bt4_get_matches(mf, i, match_lens, match_offs, &num_matches);

        /* 1. Literal transaction */
        uint32_t cost_lit = nodes[i].price + lz_get_literal_cost();
        if (cost_lit < nodes[i + 1].price) {
            nodes[i + 1].price = cost_lit;
            nodes[i + 1].length = 1;
            nodes[i + 1].offset = 0;
        }

        /* 2. Match transactions — sample key sub-lengths for speed */
        for (uint32_t m = 0; m < num_matches; m++) {
            uint32_t len = match_lens[m];
            uint32_t off = match_offs[m];

            /* Only try: min_match, min_match+1, len/2, len-1, len */
            uint32_t try_lens[6];
            int try_count = 0;
            try_lens[try_count++] = mf->min_match;
            if ((uint32_t)mf->min_match + 1 <= len)
                try_lens[try_count++] = mf->min_match + 1;
            if (len / 2 > (uint32_t)mf->min_match + 1 && len / 2 < len)
                try_lens[try_count++] = len / 2;
            if (len > 1 && len - 1 > (uint32_t)mf->min_match)
                try_lens[try_count++] = len - 1;
            try_lens[try_count++] = len;

            for (int t = 0; t < try_count; t++) {
                uint32_t sub_len = try_lens[t];
                uint32_t cost_match = nodes[i].price + lz_get_match_cost(sub_len, off);
                if (cost_match < nodes[i + sub_len].price) {
                    nodes[i + sub_len].price = cost_match;
                    nodes[i + sub_len].length = sub_len;
                    nodes[i + sub_len].offset = off;
                }
            }
        }
    }

    /* Traceback to build sequence (in reverse) */
    pos = in_size;
    
    /* We push tokens in reverse order */
    while (pos > 0) {
        uint32_t len = nodes[pos].length;
        uint32_t off = nodes[pos].offset;
        
        nex_lz_token_t tok;
        if (len == 1 && off == 0) {
            tok.is_match = 0;
            tok.literal = in[pos - 1];
            tok.length = 0;
            tok.offset = 0;
        } else {
            tok.is_match = 1;
            tok.literal = 0;
            tok.length = len;
            tok.offset = off;
        }
        lz_seq_push(seq, tok);
        pos -= len;
    }
    free(nodes);

    /* Reverse the sequence */
    for (size_t i = 0; i < seq->count / 2; i++) {
        size_t j = seq->count - i - 1;
        nex_lz_token_t t = seq->tokens[i];
        seq->tokens[i] = seq->tokens[j];
        seq->tokens[j] = t;
    }

    return NEX_OK;
}

static nex_status_t lz_compress_greedy(lz_match_finder_t *mf, const uint8_t *in, size_t in_size, size_t start_pos, nex_lz_sequence_t *seq, bool use_lazy) {
    size_t pos = 0;
    for (; pos < start_pos; pos++) {
        lz_update_hash_hc(mf, pos);
    }
    
    while (pos < in_size) {
        lz_update_hash_hc(mf, pos);

        uint32_t match_len = 0, match_off = 0;
        lz_find_match_hc(mf, pos, &match_len, &match_off);

        if (match_len >= (uint32_t)mf->min_match) {
            if (use_lazy && pos + 1 < in_size) {
                lz_update_hash_hc(mf, pos + 1);
                uint32_t lazy_len = 0, lazy_off = 0;
                lz_find_match_hc(mf, pos + 1, &lazy_len, &lazy_off);

                if (lz_lazy_better(match_len, match_off, lazy_len, lazy_off)) {
                    nex_lz_token_t lit = {0, in[pos], 0, 0};
                    lz_seq_push(seq, lit);
                    pos++;
                    match_len = lazy_len;
                    match_off = lazy_off;
                }
            }

            nex_lz_token_t match_tok = {1, 0, (uint16_t)match_len, match_off};
            lz_seq_push(seq, match_tok);

            for (uint32_t i = 1; i < match_len && pos + i < in_size; i++) {
                lz_update_hash_hc(mf, pos + i);
            }
            pos += match_len;
        } else {
            nex_lz_token_t lit = {0, in[pos], 0, 0};
            lz_seq_push(seq, lit);
            pos++;
        }
    }
    return NEX_OK;
}

/* ── High-Level Compress/Decompress APIs ─────────────────────────── */

nex_status_t nex_lz_compress(const uint8_t *in, size_t in_size,
                              nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    if (in_size == 0) {
        out->size = 0;
        return NEX_OK;
    }

    uint8_t *comp_in = (uint8_t *)in;
    size_t comp_size = in_size;
    bool free_comp = false;
    size_t start_pos = 0;

    if (dict && dict_size > 0) {
        comp_size = dict_size + in_size;
        comp_in = (uint8_t *)malloc(comp_size);
        if (!comp_in) return NEX_ERR_NOMEM;
        memcpy(comp_in, dict, dict_size);
        memcpy(comp_in + dict_size, in, in_size);
        free_comp = true;
        start_pos = dict_size;
    }

    lz_match_finder_t mf;
    nex_status_t st = lz_finder_init(&mf, comp_in, comp_size, level);
    if (st != NEX_OK) {
        if (free_comp) free(comp_in);
        return st;
    }

    nex_lz_sequence_t seq;
    st = lz_seq_init(&seq, in_size / 2 + 256);
    if (st != NEX_OK) {
        lz_finder_free(&mf);
        if (free_comp) free(comp_in);
        return st;
    }

    if (mf.is_bt4) {
        st = lz_compress_optimal(&mf, comp_in, comp_size, start_pos, &seq);
    } else {
        bool use_lazy = (level >= 4);
        st = lz_compress_greedy(&mf, comp_in, comp_size, start_pos, &seq, use_lazy);
    }

    if (st == NEX_OK) {
        st = lz_serialize(&seq, (uint32_t)in_size, out);
    }

    lz_seq_free(&seq);
    lz_finder_free(&mf);
    if (free_comp) free(comp_in);
    return st;
}

nex_status_t nex_lz_decompress(const uint8_t *in, size_t in_size,
                                nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    (void)level;
    nex_lz_sequence_t seq;
    uint32_t original_size;

    nex_status_t st = lz_deserialize(in, in_size, &seq, &original_size);
    if (st != NEX_OK) return st;

    size_t out_cap = original_size;
    if (dict && dict_size > 0) {
        out_cap += dict_size;
    }

    if (out->capacity < original_size) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, original_size);
        if (!new_data) {
            lz_seq_free(&seq);
            return NEX_ERR_NOMEM;
        }
        out->data = new_data;
        out->capacity = original_size;
    }

    uint8_t *dec_buf = out->data;
    size_t dec_pos = 0;
    bool free_dec = false;

    if (dict && dict_size > 0) {
        dec_buf = (uint8_t *)malloc(out_cap);
        if (!dec_buf) {
            lz_seq_free(&seq);
            return NEX_ERR_NOMEM;
        }
        memcpy(dec_buf, dict, dict_size);
        dec_pos = dict_size;
        free_dec = true;
    }

    for (size_t i = 0; i < seq.count; i++) {
        const nex_lz_token_t *tok = &seq.tokens[i];

        if (!tok->is_match) {
            if (dec_pos >= out_cap) {
                if (free_dec) free(dec_buf);
                lz_seq_free(&seq);
                return NEX_ERR_CORRUPT;
            }
            dec_buf[dec_pos++] = tok->literal;
        } else {
            if (tok->offset == 0 || tok->offset > dec_pos) {
                if (free_dec) free(dec_buf);
                lz_seq_free(&seq);
                return NEX_ERR_CORRUPT;
            }
            size_t ref = dec_pos - tok->offset;
            for (uint16_t j = 0; j < tok->length; j++) {
                if (dec_pos >= out_cap) {
                    if (free_dec) free(dec_buf);
                    lz_seq_free(&seq);
                    return NEX_ERR_CORRUPT;
                }
                dec_buf[dec_pos++] = dec_buf[ref + j];
            }
        }
    }

    if (free_dec) {
        memcpy(out->data, dec_buf + dict_size, original_size);
        free(dec_buf);
    }

    out->size = original_size;
    lz_seq_free(&seq);
    return NEX_OK;
}
