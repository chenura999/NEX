/*
 * NEX Compress — Transform Module
 * BWT (with prefix-doubling SA), MTF, RLE, Delta transforms
 */

#include "nex_internal.h"

/* ═══════════════════════════════════════════════════════════════════
 * Burrows-Wheeler Transform (BWT)
 * Uses prefix-doubling suffix array construction: O(n log²n)
 *
 * Output format:
 *   [4 bytes] original size (uint32 LE)
 *   [4 bytes] primary index (position of original first char)
 *   [N bytes] BWT output
 * ═══════════════════════════════════════════════════════════════════ */

/* ── SA-IS (Linear Time O(N) Suffix Array) ─────────────── */

#define CHR(i) (cs == sizeof(int) ? ((int*)s)[i] : ((const uint8_t*)s)[i])

static void getCounts(const void *s, int *c, int n, int k, int cs) {
    for (int i = 0; i < k; i++) c[i] = 0;
    for (int i = 0; i < n; i++) c[CHR(i)]++;
}

static void getBuckets(const int *c, int *b, int k, int end) {
    int sum = 0;
    if (end) { for (int i = 0; i < k; i++) { sum += c[i]; b[i] = sum; } }
    else     { for (int i = 0; i < k; i++) { b[i] = sum; sum += c[i]; } }
}

static void induceSA(const uint8_t *t, int *SA, const void *s, int *c, int *b, int n, int k, int cs) {
    int i, j;
    getBuckets(c, b, k, 0); /* head */
    for (i = 0; i < n; i++) {
        j = SA[i] - 1;
        if (j >= 0 && !(t[j >> 3] & (1 << (j & 7)))) SA[b[CHR(j)]++] = j;
    }
    getBuckets(c, b, k, 1); /* tail */
    for (i = n - 1; i >= 0; i--) {
        j = SA[i] - 1;
        if (j >= 0 && (t[j >> 3] & (1 << (j & 7)))) SA[--b[CHR(j)]] = j;
    }
}

static void SA_IS(const void *s, int *SA, int n, int k, int cs) {
    int i, j;
    uint8_t *t = (uint8_t *)calloc((n >> 3) + 1, 1);
    if (!t) return;
    
    t[(n - 1) >> 3] |= (1 << ((n - 1) & 7));
    for (i = n - 2; i >= 0; i--) {
        int c1 = CHR(i), c2 = CHR(i + 1);
        if (c1 < c2 || (c1 == c2 && (t[(i + 1) >> 3] & (1 << ((i + 1) & 7)))))
            t[i >> 3] |= (1 << (i & 7));
    }

    int *c = (int *)malloc(k * sizeof(int));
    int *b = (int *)malloc(k * sizeof(int));
    if (!c || !b) { free(t); free(c); free(b); return; }

    getCounts(s, c, n, k, cs);
    getBuckets(c, b, k, 1);
    for (i = 0; i < n; i++) SA[i] = -1;
    for (i = 1; i < n; i++) {
        if ((t[i >> 3] & (1 << (i & 7))) && !(t[(i - 1) >> 3] & (1 << ((i - 1) & 7))))
            SA[--b[CHR(i)]] = i;
    }
    induceSA(t, SA, s, c, b, n, k, cs);

    int n1 = 0;
    for (i = 0; i < n; i++) {
        int p = SA[i];
        if (p > 0 && (t[p >> 3] & (1 << (p & 7))) && !(t[(p - 1) >> 3] & (1 << ((p - 1) & 7))))
            SA[n1++] = p;
    }

    for (i = n1; i < n; i++) SA[i] = -1;
    int name = 0, prev = -1;
    for (i = 0; i < n1; i++) {
        int pos = SA[i], diff = 0;
        for (int d = 0; d < n; d++) {
            if (prev == -1 || CHR(pos + d) != CHR(prev + d) || 
                (!!(t[(pos + d) >> 3] & (1 << ((pos + d) & 7))) != !!(t[(prev + d) >> 3] & (1 << ((prev + d) & 7))))) {
                diff = 1; break;
            }
            if (d > 0 && (t[(pos + d) >> 3] & (1 << ((pos + d) & 7))) && !(t[(pos + d - 1) >> 3] & (1 << ((pos + d - 1) & 7))))
                break;
        }
        if (diff) { name++; prev = pos; }
        pos = (pos >> 1);
        SA[n1 + pos] = name - 1;
    }
    
    j = n - 1;
    for (i = n - 1; i >= n1; i--) { if (SA[i] >= 0) SA[j--] = SA[i]; }

    int *SA1 = SA, *s1 = SA + n - n1;
    if (name < n1) {
        SA_IS(s1, SA1, n1, name, sizeof(int));
    } else {
        for (i = 0; i < n1; i++) SA1[s1[i]] = i;
    }

    getBuckets(c, b, k, 1);
    for (i = 1, j = 0; i < n; i++) {
        if ((t[i >> 3] & (1 << (i & 7))) && !(t[(i - 1) >> 3] & (1 << ((i - 1) & 7))))
            s1[j++] = i;
    }
    for (i = 0; i < n1; i++) SA1[i] = s1[SA1[i]];
    for (i = n1; i < n; i++) SA[i] = -1;
    for (i = n1 - 1; i >= 0; i--) {
        j = SA[i]; SA[i] = -1;
        SA[--b[CHR(j)]] = j;
    }
    induceSA(t, SA, s, c, b, n, k, cs);

    free(c); free(b); free(t);
}

static void build_suffix_array(const uint8_t *data, size_t n, uint32_t *sa_out) {
    if (n == 0) return;
    
    // We use String Doubling to accurately simulate CYCLIC shift string sorting (BWT)
    size_t nn = n * 2;
    int *SA = (int *)malloc((nn + 1) * sizeof(int));
    int *data_pad = (int *)malloc((nn + 1) * sizeof(int));
    
    if (!SA || !data_pad) { free(SA); free(data_pad); return; }
    
    // Pad characters (+1) to reserve 0 as the strict EOF sentinel
    for(size_t i = 0; i < n; i++) {
        data_pad[i] = data[i] + 1;
        data_pad[i + n] = data[i] + 1;
    }
    data_pad[nn] = 0;

    SA_IS(data_pad, SA, nn + 1, 257, sizeof(int));
    
    // Extract the original N cyclic suffixes (those whose starting index < N)
    size_t j = 0;
    for (size_t i = 0; i <= nn; i++) {
        if (SA[i] < (int)n) {
            sa_out[j++] = (uint32_t)SA[i];
            if (j == n) break;
        }
    }
    
    free(SA);
    free(data_pad);
}



nex_status_t nex_bwt_forward(const uint8_t *in, size_t in_size,
                              nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;
    if (in_size == 0) {
        out->size = 0;
        return NEX_OK;
    }

    /* Build suffix array */
    uint32_t *sa = (uint32_t *)malloc(in_size * sizeof(uint32_t));
    if (!sa) return NEX_ERR_NOMEM;

    build_suffix_array(in, in_size, sa);

    /* Produce BWT output */
    size_t out_needed = 8 + in_size;
    if (out->capacity < out_needed) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, out_needed);
        if (!new_data) { free(sa); return NEX_ERR_NOMEM; }
        out->data = new_data;
        out->capacity = out_needed;
    }

    uint32_t orig_size = (uint32_t)in_size;
    uint32_t primary_idx = 0;

    uint8_t *bwt_out = out->data + 8;
    for (size_t i = 0; i < in_size; i++) {
        if (sa[i] == 0) {
            primary_idx = (uint32_t)i;
            bwt_out[i] = in[in_size - 1];
        } else {
            bwt_out[i] = in[sa[i] - 1];
        }
    }

    /* Write header */
    memcpy(out->data, &orig_size, 4);
    memcpy(out->data + 4, &primary_idx, 4);

    out->size = out_needed;
    free(sa);
    return NEX_OK;
}

/* ── Inverse BWT ─────────────────────────────────────────────────── */

nex_status_t nex_bwt_inverse(const uint8_t *in, size_t in_size,
                              nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;
    if (in_size < 8) return NEX_ERR_CORRUPT;

    uint32_t orig_size, primary_idx;
    memcpy(&orig_size, in, 4);
    memcpy(&primary_idx, in + 4, 4);

    /* Sanity check: reject absurdly large sizes (DoS prevention) */
    if (orig_size > (256 * 1024 * 1024)) return NEX_ERR_CORRUPT;

    const uint8_t *bwt = in + 8;
    size_t n = orig_size;

    if (in_size < 8 + n) return NEX_ERR_CORRUPT;
    if (primary_idx >= n) return NEX_ERR_CORRUPT;

    /* Allocate output */
    if (out->capacity < n) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, n);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = n;
    }

    /* Count occurrences */
    uint32_t count[256];
    memset(count, 0, sizeof(count));
    for (size_t i = 0; i < n; i++) count[bwt[i]]++;

    /* Cumulative counts */
    uint32_t cumul[256];
    uint32_t sum = 0;
    for (int i = 0; i < 256; i++) {
        cumul[i] = sum;
        sum += count[i];
    }

    /* Build transformation vector T */
    uint32_t *T = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!T) return NEX_ERR_NOMEM;

    uint32_t occ[256];
    memcpy(occ, cumul, sizeof(occ));
    for (size_t i = 0; i < n; i++) {
        T[i] = occ[bwt[i]]++;
    }

    /* Follow the chain to reconstruct */
    uint32_t idx = primary_idx;
    for (size_t i = n; i > 0; i--) {
        out->data[i - 1] = bwt[idx];
        idx = T[idx];
    }

    out->size = n;
    free(T);
    return NEX_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * Move-to-Front + Run-Length Encoding (combined stage)
 *
 * MTF: transforms bytes based on recency, produces many zeros for
 * sorted/BWT data. RLE: encodes runs of zeros efficiently.
 *
 * Output format:
 *   [4 bytes] original size (uint32 LE)
 *   [N bytes] MTF+RLE encoded data
 *
 * RLE scheme for zeros:
 *   0x00 followed by count byte: run of (count+1) zeros
 *   Non-zero bytes: literal MTF values
 * ═══════════════════════════════════════════════════════════════════ */

nex_status_t nex_mtf_rle_encode(const uint8_t *in, size_t in_size,
                                 nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;
    if (in_size == 0) {
        out->size = 0;
        return NEX_OK;
    }

    /* MTF table */
    uint8_t mtf_table[256];
    for (int i = 0; i < 256; i++) mtf_table[i] = (uint8_t)i;

    /* Allocate worst-case output: 4 + 2*n */
    size_t est = 4 + in_size * 2;
    if (out->capacity < est) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, est);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = est;
    }

    uint32_t orig_size = (uint32_t)in_size;
    memcpy(out->data, &orig_size, 4);
    uint8_t *op = out->data + 4;

    /* MTF + RLE pass */
    size_t i = 0;
    while (i < in_size) {
        /* MTF lookup */
        uint8_t byte = in[i];
        int rank = 0;
        for (int j = 0; j < 256; j++) {
            if (mtf_table[j] == byte) { rank = j; break; }
        }

        /* Move to front */
        if (rank > 0) {
            memmove(mtf_table + 1, mtf_table, rank);
            mtf_table[0] = byte;
        }

        if (rank == 0) {
            /* Count zero run */
            size_t run = 1;
            size_t j = i + 1;
            while (j < in_size) {
                /* Check if next byte is also rank 0 */
                uint8_t next_byte = in[j];
                if (next_byte != mtf_table[0]) break;
                run++;
                j++;
            }

            /* Encode zero run: 0x00 then count-1 (in chunks of 256) */
            while (run > 0) {
                uint8_t chunk = (uint8_t)(NEX_MIN(run, 256) - 1);
                *op++ = 0x00;
                *op++ = chunk;
                run -= (size_t)chunk + 1;
            }
            i = j;
        } else {
            *op++ = (uint8_t)rank;
            i++;
        }
    }

    out->size = (size_t)(op - out->data);
    return NEX_OK;
}

nex_status_t nex_mtf_rle_decode(const uint8_t *in, size_t in_size,
                                 nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;
    if (in_size < 4) return NEX_ERR_CORRUPT;

    uint32_t orig_size;
    memcpy(&orig_size, in, 4);

    /* Sanity check: reject absurdly large sizes (DoS prevention) */
    if (orig_size > (256 * 1024 * 1024)) return NEX_ERR_CORRUPT;

    if (out->capacity < orig_size) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, orig_size);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = orig_size;
    }

    /* MTF table */
    uint8_t mtf_table[256];
    for (int i = 0; i < 256; i++) mtf_table[i] = (uint8_t)i;

    const uint8_t *ip = in + 4;
    const uint8_t *end = in + in_size;
    size_t pos = 0;

    while (ip < end && pos < orig_size) {
        uint8_t rank = *ip++;

        if (rank == 0) {
            /* Zero run */
            if (ip >= end) return NEX_ERR_CORRUPT;
            uint8_t run_len = *ip++;
            uint8_t byte = mtf_table[0];
            for (int j = 0; j <= run_len && pos < orig_size; j++) {
                out->data[pos++] = byte;
            }
        } else {
            /* Non-zero rank: lookup in MTF table */
            uint8_t byte = mtf_table[rank];
            /* Move to front */
            memmove(mtf_table + 1, mtf_table, rank);
            mtf_table[0] = byte;
            if (pos < orig_size) {
                out->data[pos++] = byte;
            }
        }
    }

    out->size = pos;
    return NEX_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * Delta Encoding
 *
 * Output format:
 *   [4 bytes] original size (uint32 LE)
 *   [N bytes] delta values (byte[i] - byte[i-1], first byte stored raw)
 * ═══════════════════════════════════════════════════════════════════ */

nex_status_t nex_delta_encode(const uint8_t *in, size_t in_size,
                               nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;
    if (in_size == 0) {
        out->size = 0;
        return NEX_OK;
    }

    size_t needed = 4 + in_size;
    if (out->capacity < needed) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, needed);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = needed;
    }

    uint32_t orig_size = (uint32_t)in_size;
    memcpy(out->data, &orig_size, 4);

    out->data[4] = in[0]; /* first byte raw */
    for (size_t i = 1; i < in_size; i++) {
        out->data[4 + i] = in[i] - in[i - 1];
    }

    out->size = needed;
    return NEX_OK;
}

nex_status_t nex_delta_decode(const uint8_t *in, size_t in_size,
                               nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;
    if (in_size < 4) return NEX_ERR_CORRUPT;

    uint32_t orig_size;
    memcpy(&orig_size, in, 4);

    /* Sanity check: reject absurdly large sizes (DoS prevention) */
    if (orig_size > (256 * 1024 * 1024)) return NEX_ERR_CORRUPT;

    if (in_size < 4 + orig_size) return NEX_ERR_CORRUPT;

    if (out->capacity < orig_size) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, orig_size);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = orig_size;
    }

    out->data[0] = in[4]; /* first byte raw */
    for (size_t i = 1; i < orig_size; i++) {
        out->data[i] = out->data[i - 1] + in[4 + i];
    }

    out->size = orig_size;
    return NEX_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * BCJ Filter (Branch/Call/Jump)
 *
 * Converts x86 relative addresses (e8 / e9) into absolute addresses
 * to increase redundancy. Completely reversible mapping.
 * ═══════════════════════════════════════════════════════════════════ */

nex_status_t nex_bcj_x86_encode(const uint8_t *in, size_t in_size,
                                 nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;
    if (in_size == 0) {
        out->size = 0;
        return NEX_OK;
    }

    if (out->capacity < in_size) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, in_size);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = in_size;
    }

    size_t i = 0;
    while (i < in_size) {
        if (i + 5 <= in_size && (in[i] == 0xE8 || in[i] == 0xE9)) {
            uint32_t rel;
            memcpy(&rel, in + i + 1, 4);
            /* Skip likely false positives (addresses way entirely out of range) */
            if (rel < 0x01000000 || rel > 0xFF000000) {
                uint32_t abs_off = rel + (uint32_t)i;
                out->data[i] = in[i];
                memcpy(out->data + i + 1, &abs_off, 4);
                i += 5;
                continue;
            }
        }
        out->data[i] = in[i];
        i++;
    }

    out->size = in_size;
    return NEX_OK;
}

nex_status_t nex_bcj_x86_decode(const uint8_t *in, size_t in_size,
                                 nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;
    if (in_size == 0) {
        out->size = 0;
        return NEX_OK;
    }

    if (out->capacity < in_size) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, in_size);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = in_size;
    }

    size_t i = 0;
    while (i < in_size) {
        if (i + 5 <= in_size && (in[i] == 0xE8 || in[i] == 0xE9)) {
            uint32_t abs_off;
            memcpy(&abs_off, in + i + 1, 4);
            /* Test if original relative offset was in bounds (reverse logic) */
            uint32_t rel = abs_off - (uint32_t)i;
            if (rel < 0x01000000 || rel > 0xFF000000) {
                out->data[i] = in[i];
                memcpy(out->data + i + 1, &rel, 4);
                i += 5;
                continue;
            }
        }
        out->data[i] = in[i];
        i++;
    }

    out->size = in_size;
    return NEX_OK;
}

