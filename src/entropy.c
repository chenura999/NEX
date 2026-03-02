/*
 * NEX Compress — Entropy Encoding Module
 * rANS (Range Asymmetric Numeral Systems) + Huffman fallback
 */

#include "nex_internal.h"

/* ═══════════════════════════════════════════════════════════════════
 * rANS Encoder/Decoder
 *
 * Uses 32-bit interleaved rANS for throughput.
 *
 * Stream format:
 *   [4 bytes]  original size (uint32 LE)
 *   [4 bytes]  compressed data size (uint32 LE)
 *   [2048 bytes] frequency table (256 × uint16 normalized freqs, summing to 4096)
 *   [N bytes]  rANS encoded data (reversed byte stream)
 *   [4 bytes]  final rANS state
 *
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Frequency Table ─────────────────────────────────────────────── */

typedef struct {
    uint16_t freq[NEX_ENTROPY_ALPHABET];    /* symbol frequency   */
    uint16_t cum_freq[NEX_ENTROPY_ALPHABET + 1]; /* cumulative   */
} rans_freq_table_t;

static void rans_build_freq_table(const uint8_t *data, size_t size,
                                   rans_freq_table_t *ft) {
    /* Count raw frequencies */
    uint32_t raw[NEX_ENTROPY_ALPHABET];
    memset(raw, 0, sizeof(raw));
    for (size_t i = 0; i < size; i++) {
        raw[data[i]]++;
    }

    /* Normalize to sum = NEX_FREQ_SUM (4096) */
    /* First pass: proportional scaling */
    uint32_t total = 0;
    for (int i = 0; i < 256; i++) {
        if (raw[i] > 0) {
            ft->freq[i] = (uint16_t)NEX_MAX(1,
                (uint32_t)((uint64_t)raw[i] * NEX_FREQ_SUM / size));
            total += ft->freq[i];
        } else {
            ft->freq[i] = 0;
        }
    }

    /* Adjust to hit exact sum */
    while (total != NEX_FREQ_SUM) {
        /* Find the symbol with largest raw count to adjust */
        int best = -1;
        uint32_t best_count = 0;
        for (int i = 0; i < 256; i++) {
            if (ft->freq[i] > 1 && raw[i] > best_count) {
                best = i;
                best_count = raw[i];
            }
        }
        if (best < 0) {
            /* Edge case: find any symbol with freq > 1 */
            for (int i = 0; i < 256; i++) {
                if (ft->freq[i] > 1) { best = i; break; }
            }
        }
        if (best < 0) break; /* shouldn't happen */

        if (total > NEX_FREQ_SUM) {
            ft->freq[best]--;
            total--;
        } else {
            ft->freq[best]++;
            total++;
        }
    }

    /* Build cumulative frequencies */
    ft->cum_freq[0] = 0;
    for (int i = 0; i < 256; i++) {
        ft->cum_freq[i + 1] = ft->cum_freq[i] + ft->freq[i];
    }
}

/* ── rANS Core ───────────────────────────────────────────────────── */

typedef uint32_t rans_state_t;

#define RANS_L  (1U << 23)   /* lower bound of state range */

static inline void rans_enc_init(rans_state_t *state) {
    *state = RANS_L;
}

static inline void rans_enc_put(rans_state_t *state, uint8_t **pptr,
                                 uint16_t start, uint16_t freq) {
    rans_state_t x = *state;

    /* Renormalize: output bytes until state is in valid range */
    uint32_t upper = ((RANS_L >> NEX_FREQ_BITS) << 8) * freq;
    while (x >= upper) {
        *--(*pptr) = (uint8_t)(x & 0xFF);
        x >>= 8;
    }

    /* Encode symbol */
    *state = ((x / freq) << NEX_FREQ_BITS) + (x % freq) + start;
}

static inline void rans_enc_flush(rans_state_t state, uint8_t **pptr) {
    /* Write final state as 4 bytes LE */
    *--(*pptr) = (uint8_t)((state >> 24) & 0xFF);
    *--(*pptr) = (uint8_t)((state >> 16) & 0xFF);
    *--(*pptr) = (uint8_t)((state >> 8) & 0xFF);
    *--(*pptr) = (uint8_t)(state & 0xFF);
}

static inline void rans_dec_init(rans_state_t *state, const uint8_t **pptr) {
    /* Read state from 4 bytes LE */
    const uint8_t *p = *pptr;
    *state = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
             ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    *pptr = p + 4;
}

static inline uint32_t rans_dec_get(rans_state_t state) {
    return state & (NEX_FREQ_SUM - 1);
}

static inline void rans_dec_advance(rans_state_t *state, const uint8_t **pptr,
                                     uint16_t start, uint16_t freq) {
    uint32_t mask = NEX_FREQ_SUM - 1;
    rans_state_t x = *state;

    x = freq * (x >> NEX_FREQ_BITS) + (x & mask) - start;

    /* Renormalize */
    while (x < RANS_L) {
        x = (x << 8) | **pptr;
        (*pptr)++;
    }

    *state = x;
}

/* ── Cumulative freq → symbol lookup ─────────────────────────────── */

static inline uint8_t rans_lookup_symbol(const rans_freq_table_t *ft,
                                          uint32_t cum) {
    /* Linear scan (could use binary search for speed) */
    for (int i = 0; i < 256; i++) {
        if (ft->cum_freq[i + 1] > cum) return (uint8_t)i;
    }
    return 255;
}

/* ── rANS Compress ───────────────────────────────────────────────── */

nex_status_t nex_rans_compress(const uint8_t *in, size_t in_size,
                                nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;
    if (in_size == 0) {
        out->size = 0;
        return NEX_OK;
    }

    /* Build frequency table */
    rans_freq_table_t ft;
    rans_build_freq_table(in, in_size, &ft);

    /* Allocate working buffer (worst case: input size + large overhead for expansion)
       In worst-case (entropy > 8.0 equivalents), data can expand by up to ~15%.
       We allocate enough headroom for backwards encoding. */
    size_t work_size = in_size + (in_size / 4) + 8192;
    uint8_t *work = (uint8_t *)malloc(work_size);
    if (!work) return NEX_ERR_NOMEM;

    /* Encode backwards (rANS encodes in reverse) */
    uint8_t *work_end = work + work_size;
    uint8_t *ptr = work_end;

    rans_state_t state;
    rans_enc_init(&state);

    /* Encode from last to first */
    for (size_t i = in_size; i > 0; i--) {
        uint8_t sym = in[i - 1];
        if (ft.freq[sym] == 0) {
            /* Symbol not in table — shouldn't happen if table built correctly */
            free(work);
            return NEX_ERR_CORRUPT;
        }
        rans_enc_put(&state, &ptr, ft.cum_freq[sym], ft.freq[sym]);
    }

    rans_enc_flush(state, &ptr);

    /* Calculate encoded data size */
    size_t enc_size = (size_t)(work_end - ptr);

    /* Write output: header + freq table + encoded data */
    size_t total_out = 4 + 4 + 512 + enc_size;
    if (out->capacity < total_out) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, total_out);
        if (!new_data) { free(work); return NEX_ERR_NOMEM; }
        out->data = new_data;
        out->capacity = total_out;
    }

    uint8_t *op = out->data;

    /* Original size */
    uint32_t orig32 = (uint32_t)in_size;
    memcpy(op, &orig32, 4); op += 4;

    /* Encoded data size */
    uint32_t enc32 = (uint32_t)enc_size;
    memcpy(op, &enc32, 4); op += 4;

    /* Frequency table (compact: 256 × 2 bytes = 512 bytes) */
    for (int i = 0; i < 256; i++) {
        memcpy(op, &ft.freq[i], 2); op += 2;
    }

    /* Encoded data */
    memcpy(op, ptr, enc_size);
    op += enc_size;

    out->size = (size_t)(op - out->data);
    free(work);
    return NEX_OK;
}

/* ── rANS Decompress ─────────────────────────────────────────────── */

nex_status_t nex_rans_decompress(const uint8_t *in, size_t in_size,
                                  nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;
    if (in_size < 520) return NEX_ERR_CORRUPT; /* 4+4+512 minimum header */

    const uint8_t *ip = in;

    /* Read header */
    uint32_t orig_size, enc_size;
    memcpy(&orig_size, ip, 4); ip += 4;
    memcpy(&enc_size, ip, 4); ip += 4;

    /* Read frequency table */
    rans_freq_table_t ft;
    for (int i = 0; i < 256; i++) {
        memcpy(&ft.freq[i], ip, 2); ip += 2;
    }

    /* Rebuild cumulative frequencies */
    ft.cum_freq[0] = 0;
    for (int i = 0; i < 256; i++) {
        ft.cum_freq[i + 1] = ft.cum_freq[i] + ft.freq[i];
    }

    /* Validate */
    if (ft.cum_freq[256] != NEX_FREQ_SUM) return NEX_ERR_CORRUPT;
    if (in_size < (size_t)(ip - in) + enc_size) return NEX_ERR_CORRUPT;

    /* Allocate output */
    if (out->capacity < orig_size) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, orig_size);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = orig_size;
    }

    /* Build Table-Driven rANS decoding lookup (O(1) decode) */
    typedef struct {
        uint16_t freq;
        uint16_t start;
        uint8_t  sym;
    } rans_dec_table_t;

    rans_dec_table_t dec_table[NEX_FREQ_SUM];
    for (int sym = 0; sym < 256; sym++) {
        if (ft.freq[sym] == 0) continue;
        for (int j = 0; j < ft.freq[sym]; j++) {
            int cum = ft.cum_freq[sym] + j;
            dec_table[cum].sym = (uint8_t)sym;
            dec_table[cum].freq = ft.freq[sym];
            dec_table[cum].start = ft.cum_freq[sym];
        }
    }

    /* Decode */
    const uint8_t *dec_ptr = ip;
    rans_state_t state;
    rans_dec_init(&state, &dec_ptr);

    uint8_t *op = out->data;
    uint32_t mask = NEX_FREQ_SUM - 1;

    for (uint32_t i = 0; i < orig_size; i++) {
        uint32_t cum = state & mask;
        rans_dec_table_t t = dec_table[cum];
        
        *op++ = t.sym;
        
        /* O(1) State Advance */
        state = t.freq * (state >> NEX_FREQ_BITS) + cum - t.start;

        /* Renormalize */
        while (state < RANS_L) {
            state = (state << 8) | *dec_ptr++;
        }
    }

    out->size = orig_size;
    return NEX_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * Canonical Huffman Coding (fast fallback)
 *
 * Stream format:
 *   [4 bytes]  original size (uint32 LE)
 *   [4 bytes]  compressed bit count (uint32 LE)
 *   [256 bytes] code lengths (1 byte per symbol, 0 = unused)
 *   [N bytes]  packed bitstream (MSB first)
 *
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t code;
    uint8_t  length;
} huff_code_t;

/* Build Huffman tree and extract code lengths */
static void huff_build_lengths(const uint32_t *freq, size_t n,
                                uint8_t *lengths) {
    /* Simple package-merge / counting approach
     * For production: use proper Huffman construction.
     * Here we use a simplified approach based on symbol frequencies. */

    typedef struct { uint32_t freq; int sym; } huff_node_t;
    huff_node_t nodes[256];
    int active = 0;

    for (size_t i = 0; i < n; i++) {
        if (freq[i] > 0) {
            nodes[active].freq = freq[i];
            nodes[active].sym = (int)i;
            active++;
        }
        lengths[i] = 0;
    }

    if (active == 0) return;
    if (active == 1) {
        lengths[nodes[0].sym] = 1;
        return;
    }

    /* Sort by frequency ascending */
    for (int i = 0; i < active - 1; i++) {
        for (int j = i + 1; j < active; j++) {
            if (nodes[j].freq < nodes[i].freq) {
                huff_node_t tmp = nodes[i];
                nodes[i] = nodes[j];
                nodes[j] = tmp;
            }
        }
    }

    /* Assign code lengths using simple heuristic:
     * The least frequent symbols get longest codes.
     * We use a simplified approach that approximates optimal Huffman. */
    int max_bits = 15; /* cap at 15 bits */

    /* Use a simplified length-limited Huffman:
     * Assign lengths based on log2 of relative frequency */
    uint32_t max_freq = nodes[active - 1].freq;
    for (int i = 0; i < active; i++) {
        double ratio = (double)max_freq / (double)nodes[i].freq;
        int bits = 1;
        while ((1 << bits) < (int)ratio && bits < max_bits) bits++;
        lengths[nodes[i].sym] = (uint8_t)bits;
    }

    /* Verify kraft inequality and adjust */
    double kraft = 0;
    for (int i = 0; i < active; i++) {
        kraft += 1.0 / (double)(1 << lengths[nodes[i].sym]);
    }
    /* If kraft > 1, increase some lengths */
    while (kraft > 1.0) {
        /* Find shortest code and increase it */
        int min_idx = 0;
        for (int i = 1; i < active; i++) {
            if (lengths[nodes[i].sym] < lengths[nodes[min_idx].sym]) {
                min_idx = i;
            }
        }
        lengths[nodes[min_idx].sym]++;
        kraft = 0;
        for (int i = 0; i < active; i++) {
            kraft += 1.0 / (double)(1 << lengths[nodes[i].sym]);
        }
    }
}

/* Build canonical codes from lengths */
static void huff_build_codes(const uint8_t *lengths, huff_code_t *codes) {
    /* Count code lengths */
    int bl_count[16];
    memset(bl_count, 0, sizeof(bl_count));
    for (int i = 0; i < 256; i++) {
        if (lengths[i] > 0) bl_count[lengths[i]]++;
    }

    /* Generate starting codes for each length */
    uint32_t next_code[16];
    next_code[0] = 0;
    uint32_t code = 0;
    for (int bits = 1; bits <= 15; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    /* Assign codes */
    for (int i = 0; i < 256; i++) {
        codes[i].length = lengths[i];
        if (lengths[i] > 0) {
            codes[i].code = next_code[lengths[i]]++;
        } else {
            codes[i].code = 0;
        }
    }
}

/* ── Huffman Compress ────────────────────────────────────────────── */

nex_status_t nex_huffman_compress(const uint8_t *in, size_t in_size,
                                   nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;
    if (in_size == 0) {
        out->size = 0;
        return NEX_OK;
    }

    /* Count frequencies */
    uint32_t freq[256];
    memset(freq, 0, sizeof(freq));
    for (size_t i = 0; i < in_size; i++) freq[in[i]]++;

    /* Build code lengths and canonical codes */
    uint8_t lengths[256];
    huff_build_lengths(freq, 256, lengths);

    huff_code_t codes[256];
    huff_build_codes(lengths, codes);

    /* Calculate total bits needed */
    uint64_t total_bits = 0;
    for (int i = 0; i < 256; i++) {
        total_bits += (uint64_t)freq[i] * lengths[i];
    }
    uint32_t total_bits32 = (uint32_t)total_bits;
    size_t byte_count = (total_bits + 7) / 8;

    /* Allocate output: 4 + 4 + 256 + byte_count */
    size_t needed = 264 + byte_count;
    if (out->capacity < needed) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, needed);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = needed;
    }

    uint8_t *op = out->data;

    /* Header */
    uint32_t orig32 = (uint32_t)in_size;
    memcpy(op, &orig32, 4); op += 4;
    memcpy(op, &total_bits32, 4); op += 4;

    /* Code lengths table */
    memcpy(op, lengths, 256); op += 256;

    /* Encode bitstream */
    uint8_t *bitstream = op;
    memset(bitstream, 0, byte_count);

    uint64_t bit_pos = 0;
    for (size_t i = 0; i < in_size; i++) {
        huff_code_t c = codes[in[i]];
        /* Write bits MSB first */
        for (int b = c.length - 1; b >= 0; b--) {
            if (c.code & (1U << b)) {
                bitstream[bit_pos >> 3] |= (1 << (7 - (bit_pos & 7)));
            }
            bit_pos++;
        }
    }

    out->size = 264 + byte_count;
    return NEX_OK;
}

/* ── Huffman Decompress ──────────────────────────────────────────── */

nex_status_t nex_huffman_decompress(const uint8_t *in, size_t in_size,
                                     nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;
    if (in_size < 264) return NEX_ERR_CORRUPT;

    const uint8_t *ip = in;

    uint32_t orig_size, total_bits;
    memcpy(&orig_size, ip, 4); ip += 4;
    memcpy(&total_bits, ip, 4); ip += 4;

    /* Read code lengths */
    uint8_t lengths[256];
    memcpy(lengths, ip, 256); ip += 256;

    /* Build canonical codes for decoding */
    huff_code_t codes[256];
    huff_build_codes(lengths, codes);

    /* Allocate output */
    if (out->capacity < orig_size) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, orig_size);
        if (!new_data) return NEX_ERR_NOMEM;
        out->data = new_data;
        out->capacity = orig_size;
    }

    /* Build decode lookup: brute-force match codes */
    const uint8_t *bitstream = ip;
    uint64_t bit_pos = 0;
    size_t out_pos = 0;

    while (out_pos < orig_size && bit_pos < total_bits) {
        uint32_t accum = 0;
        int bits_read = 0;
        bool found = false;

        for (int len = 1; len <= 15 && !found; len++) {
            accum = (accum << 1);
            if (bitstream[bit_pos >> 3] & (1 << (7 - (bit_pos & 7)))) {
                accum |= 1;
            }
            bit_pos++;
            bits_read++;

            /* Search for matching code */
            for (int sym = 0; sym < 256; sym++) {
                if (codes[sym].length == bits_read &&
                    codes[sym].code == accum) {
                    out->data[out_pos++] = (uint8_t)sym;
                    found = true;
                    break;
                }
            }
        }

        if (!found) return NEX_ERR_CORRUPT;
    }

    out->size = out_pos;
    return NEX_OK;
}
