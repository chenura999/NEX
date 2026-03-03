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

    /* Sanity check: reject absurdly large sizes (DoS prevention) */
    if (orig_size > (256 * 1024 * 1024)) return NEX_ERR_CORRUPT;

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

    /* Sanity check: reject absurdly large sizes (DoS prevention) */
    if (orig_size > (256 * 1024 * 1024)) return NEX_ERR_CORRUPT;

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

/* ═══════════════════════════════════════════════════════════════════
 * FSE (Finite State Entropy) — tANS Encoder/Decoder
 *
 * Division-free entropy coding using precomputed lookup tables.
 * Same technique as Zstd's FSE implementation.
 *
 * Table size: 2048 entries (11-bit log), giving good compression
 * while keeping tables L1-cache friendly (~16KB encode, ~8KB decode).
 *
 * Stream format (compact):
 *   [4 bytes]    original size
 *   [4 bytes]    compressed data size
 *   [1 byte]     0xFE format marker
 *   [1 byte]     maxSymbolValue (M)
 *   [(M+1)*2 B]  normalized freq table (symbols 0..M)
 *   [1 byte]     pad_bits (0-7)
 *   [N bytes]    FSE-encoded bitstream
 * ═══════════════════════════════════════════════════════════════════ */

#define FSE_TABLE_LOG   11
#define FSE_TABLE_SIZE  (1 << FSE_TABLE_LOG)  /* 2048 */

typedef struct {
    uint16_t new_state;   /* next state after encoding this symbol */
    uint8_t  nb_bits;     /* number of bits to write */
    uint16_t bits_out;    /* bits to output */
} fse_enc_entry_t;

typedef struct {
    uint8_t  symbol;      /* decoded symbol */
    uint8_t  nb_bits;     /* bits to read for next state */
    uint16_t base;        /* base value for next state */
} fse_dec_entry_t;

/* ── Build normalized frequencies ────────────────────────────────── */

static void fse_normalize_freqs(const uint8_t *data, size_t size,
                                 uint16_t *norm_freq) {
    uint32_t raw[256];
    memset(raw, 0, sizeof(raw));
    for (size_t i = 0; i < size; i++) raw[data[i]]++;

    /* Normalize to FSE_TABLE_SIZE */
    uint32_t total = 0;
    for (int i = 0; i < 256; i++) {
        if (raw[i] > 0) {
            norm_freq[i] = (uint16_t)NEX_MAX(1,
                (uint32_t)((uint64_t)raw[i] * FSE_TABLE_SIZE / size));
            total += norm_freq[i];
        } else {
            norm_freq[i] = 0;
        }
    }

    /* Correct sum to exactly FSE_TABLE_SIZE */
    while (total != FSE_TABLE_SIZE) {
        int best = -1;
        uint32_t best_raw = 0;
        for (int i = 0; i < 256; i++) {
            if (norm_freq[i] > 1 && raw[i] > best_raw) {
                best = i; best_raw = raw[i];
            }
        }
        if (best < 0) break;
        if (total > FSE_TABLE_SIZE) { norm_freq[best]--; total--; }
        else { norm_freq[best]++; total++; }
    }
}

/* ── Build decode table (classic tANS) ───────────────────────────── */

static void fse_build_decode_table(const uint16_t *norm_freq,
                                    fse_dec_entry_t *decode_table) {
    /* Spread symbols across the table using a step function.
     * This is the standard tANS symbol spread from FSE/Zstd. */
    uint16_t pos = 0;
    uint16_t step = (FSE_TABLE_SIZE >> 1) + (FSE_TABLE_SIZE >> 3) + 3;
    uint16_t mask = FSE_TABLE_SIZE - 1;
    uint8_t symbol_table[FSE_TABLE_SIZE];

    for (int sym = 0; sym < 256; sym++) {
        for (uint16_t i = 0; i < norm_freq[sym]; i++) {
            symbol_table[pos] = (uint8_t)sym;
            pos = (pos + step) & mask;
        }
    }

    /* Build decode entries: for each table position, compute nb_bits and baseline.
     * next_state[sym] tracks the sequential allocation within each symbol.
     * For the j-th occurrence of symbol sym, next_state starts at freq
     * and increments. nb_bits = TABLE_LOG - highbit(next_state),
     * baseline = (next_state << nb_bits) - TABLE_SIZE.
     * This ensures the decoded state is always in [0, TABLE_SIZE). */
    uint16_t next_state[256];
    for (int i = 0; i < 256; i++) next_state[i] = norm_freq[i];

    for (int i = 0; i < FSE_TABLE_SIZE; i++) {
        uint8_t sym = symbol_table[i];
        uint16_t freq = norm_freq[sym];
        if (freq == 0) continue;

        uint16_t ns = next_state[sym];
        int high_bit = 31 - __builtin_clz((unsigned)ns);
        int nb_bits = FSE_TABLE_LOG - high_bit;
        if (nb_bits < 0) nb_bits = 0;

        decode_table[i].symbol = sym;
        decode_table[i].nb_bits = (uint8_t)nb_bits;
        decode_table[i].base = (uint16_t)(((uint32_t)ns << nb_bits) - FSE_TABLE_SIZE);

        next_state[sym]++;
    }
}

/* ── FSE Compress (production-grade classic tANS) ────────────────── */
/*
 * Implementation follows the standard tANS algorithm used by Zstd/FSE.
 *
 * Encoder state machine:
 *   - State ∈ [TABLE_SIZE, 2*TABLE_SIZE)
 *   - For each symbol (backward), compute nbBitsOut using the packed
 *     deltaNbBits formula from Yann Collet's reference FSE:
 *       nbBitsOut = (state + deltaNbBits[sym]) >> 16
 *     Output nbBitsOut low bits, shift state, look up new state.
 *
 * Two-pass encoding:
 *   Pass 1: Run tANS backward, collect (nb_bits, bits) pairs.
 *   Pass 2: Write pairs forward into a flat bitstream.
 *   This eliminates bit-ordering ambiguity between encoder/decoder.
 *
 * Decoder state machine:
 *   - State is a table index ∈ [0, TABLE_SIZE)
 *   - For each symbol, look up {symbol, nb_bits, baseline} from decode_table
 *   - Read nb_bits from bitstream, next_state = baseline + bits
 */

nex_status_t nex_fse_compress(const uint8_t *in, size_t in_size,
                               nex_buffer_t *out, int level,
                               const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;

    if (in_size == 0) { out->size = 0; return NEX_OK; }
    if (in_size < 64) {
        return nex_rans_compress(in, in_size, out, level, dict, dict_size);
    }

    /* Build frequency table */
    uint16_t norm_freq[256];
    fse_normalize_freqs(in, in_size, norm_freq);

    /* Build decode table */
    fse_dec_entry_t decode_table[FSE_TABLE_SIZE];
    fse_build_decode_table(norm_freq, decode_table);

    /* Build sorted_table: for each (symbol, j-th occurrence) → table position.
     * This inverts the decode table's symbol→position mapping. */
    uint16_t cum_freq[257];
    cum_freq[0] = 0;
    for (int i = 0; i < 256; i++)
        cum_freq[i + 1] = cum_freq[i] + norm_freq[i];

    uint16_t sorted_table[FSE_TABLE_SIZE];
    uint16_t sym_occ[256];
    memset(sym_occ, 0, sizeof(sym_occ));
    for (int i = 0; i < FSE_TABLE_SIZE; i++) {
        uint8_t sym = decode_table[i].symbol;
        sorted_table[cum_freq[sym] + sym_occ[sym]] = (uint16_t)i;
        sym_occ[sym]++;
    }

    /* Build per-symbol encode parameters.
     * Reference deltaNbBits/deltaFindState from Yann Collet's FSE:
     *   For freq f > 1:
     *     maxBitsOut = TL - highbit(f-1)
     *     minStatePlus = f << maxBitsOut
     *     deltaNbBits = (maxBitsOut << 16) - minStatePlus
     *     deltaFindState = cumFreq[s] - f
     *   For freq f == 1:
     *     deltaNbBits = (TL << 16) - TABLE_SIZE
     *     deltaFindState = cumFreq[s] - 1
     *   For freq f == 0:
     *     deltaNbBits = ((TL+1) << 16) - TABLE_SIZE  (always too large → fallback)
     */
    uint32_t delta_nb_bits[256];
    int32_t delta_find_state[256];
    for (int s = 0; s < 256; s++) {
        uint16_t f = norm_freq[s];
        if (f == 0) {
            delta_nb_bits[s] = ((FSE_TABLE_LOG + 1) << 16) - FSE_TABLE_SIZE;
            delta_find_state[s] = 0;
        } else if (f == 1) {
            delta_nb_bits[s] = ((uint32_t)FSE_TABLE_LOG << 16) - FSE_TABLE_SIZE;
            delta_find_state[s] = (int32_t)cum_freq[s] - 1;
        } else {
            int max_bits_out = FSE_TABLE_LOG - (31 - __builtin_clz((unsigned)(f - 1)));
            uint32_t min_state_plus = (uint32_t)f << max_bits_out;
            delta_nb_bits[s] = ((uint32_t)max_bits_out << 16) - min_state_plus;
            delta_find_state[s] = (int32_t)cum_freq[s] - (int32_t)f;
        }
    }

    /* Pass 1: backward tANS, collect (nb_bits, bits_value) ops */
    typedef struct { uint8_t nb; uint16_t bits; } fse_op_t;
    fse_op_t *ops = (fse_op_t *)malloc((in_size + 1) * sizeof(fse_op_t));
    if (!ops) return NEX_ERR_NOMEM;

    uint32_t state = FSE_TABLE_SIZE;  /* initial encoder state */
    size_t n_ops = 0;

    for (size_t i = in_size; i > 0; i--) {
        uint8_t sym = in[i - 1];
        uint16_t freq = norm_freq[sym];
        if (freq == 0) {
            free(ops);
            return nex_rans_compress(in, in_size, out, level, dict, dict_size);
        }

        uint32_t nbo = (state + delta_nb_bits[sym]) >> 16;
        ops[n_ops].nb   = (uint8_t)nbo;
        ops[n_ops].bits = (uint16_t)(state & ((1U << nbo) - 1));
        n_ops++;

        state >>= nbo;
        /* state now ∈ [freq, 2*freq). Index into sorted_table. */
        state = sorted_table[state + delta_find_state[sym]] + FSE_TABLE_SIZE;
    }

    /* Store final state as TABLE_LOG bits */
    ops[n_ops].nb   = FSE_TABLE_LOG;
    ops[n_ops].bits = (uint16_t)(state - FSE_TABLE_SIZE);
    n_ops++;

    /* Pass 2: write ops in REVERSE order (so decoder reads forward) */
    size_t bs_cap = in_size * 2 + 256;
    uint8_t *bs_buf = (uint8_t *)malloc(bs_cap);
    if (!bs_buf) { free(ops); return NEX_ERR_NOMEM; }

    uint64_t bit_buf = 0;
    int bit_pos = 0;
    size_t bs_len = 0;

    for (size_t j = n_ops; j > 0; j--) {
        uint32_t nb = ops[j - 1].nb;
        uint32_t bits = ops[j - 1].bits;
        bit_buf = (bit_buf << nb) | bits;
        bit_pos += (int)nb;
        while (bit_pos >= 8) {
            bit_pos -= 8;
            if (bs_len >= bs_cap) {
                free(ops); free(bs_buf);
                return nex_rans_compress(in, in_size, out, level, dict, dict_size);
            }
            bs_buf[bs_len++] = (uint8_t)(bit_buf >> bit_pos);
            bit_buf &= (1ULL << bit_pos) - 1;
        }
    }

    uint8_t pad_bits = 0;
    if (bit_pos > 0) {
        pad_bits = (uint8_t)(8 - bit_pos);
        bs_buf[bs_len++] = (uint8_t)(bit_buf << pad_bits);
    }
    free(ops);

    /* Find maxSymbolValue for compact header */
    int max_sym = 255;
    while (max_sym > 0 && norm_freq[max_sym] == 0) max_sym--;
    size_t freq_bytes = (size_t)(max_sym + 1) * 2;

    /* Write output */
    size_t max_out = 8 + 1 + 1 + freq_bytes + 1 + bs_len + 16;
    if (out->capacity < max_out) {
        uint8_t *nd = (uint8_t *)realloc(out->data, max_out);
        if (!nd) { free(bs_buf); return NEX_ERR_NOMEM; }
        out->data = nd; out->capacity = max_out;
    }

    uint8_t *p = out->data;
    uint32_t orig_size = (uint32_t)in_size;
    memcpy(p, &orig_size, 4); p += 4;
    uint8_t *csz_ptr = p; p += 4;

    *p++ = 0xFE;  /* FSE format marker */
    *p++ = (uint8_t)max_sym;  /* compact: only write freq[0..maxSym] */
    memcpy(p, norm_freq, freq_bytes); p += freq_bytes;
    *p++ = pad_bits;
    memcpy(p, bs_buf, bs_len); p += bs_len;
    free(bs_buf);

    uint32_t cds = (uint32_t)(p - out->data - 8);
    memcpy(csz_ptr, &cds, 4);
    out->size = (size_t)(p - out->data);

    /* If FSE didn't compress well, fall back to rANS */
    if (out->size >= in_size) {
        return nex_rans_compress(in, in_size, out, level, dict, dict_size);
    }
    return NEX_OK;
}

/* ── FSE Decompress (production-grade with forward bit reader) ───── */

nex_status_t nex_fse_decompress(const uint8_t *in, size_t in_size,
                                 nex_buffer_t *out, int level,
                                 const uint8_t *dict, size_t dict_size) {
    (void)level; (void)dict; (void)dict_size;

    if (in_size < 8) return NEX_ERR_CORRUPT;
    const uint8_t *p = in;
    const uint8_t *const in_end = in + in_size;

    uint32_t orig_size; memcpy(&orig_size, p, 4); p += 4;
    uint32_t comp_data_size; memcpy(&comp_data_size, p, 4); p += 4;

    if (orig_size > (256 * 1024 * 1024)) return NEX_ERR_CORRUPT;

    /* Check for FSE format marker (0xFE) — if absent, fall back to rANS */
    if (comp_data_size > in_size - 8 || p >= in_end || *p != 0xFE) {
        return nex_rans_decompress(in, in_size, out, level, dict, dict_size);
    }
    p++;

    if (p >= in_end) return NEX_ERR_CORRUPT;
    int max_sym = *p++;
    if (max_sym > 255) return NEX_ERR_CORRUPT;
    size_t freq_bytes = (size_t)(max_sym + 1) * 2;
    if (p + freq_bytes > in_end) return NEX_ERR_CORRUPT;

    uint16_t norm_freq[256];
    memset(norm_freq, 0, sizeof(norm_freq));
    memcpy(norm_freq, p, freq_bytes); p += freq_bytes;

    if (p >= in_end) return NEX_ERR_CORRUPT;
    uint8_t pad_bits = *p++;
    if (pad_bits >= 8) return NEX_ERR_CORRUPT;

    /* Build decode table */
    fse_dec_entry_t decode_table[FSE_TABLE_SIZE];
    fse_build_decode_table(norm_freq, decode_table);

    /* Ensure output buffer */
    if (out->capacity < orig_size) {
        uint8_t *nd = (uint8_t *)realloc(out->data, orig_size);
        if (!nd) return NEX_ERR_NOMEM;
        out->data = nd; out->capacity = orig_size;
    }

    /* Forward bit reader (MSB-first packing) */
    const uint8_t *bs = p;
    size_t bs_len = (size_t)(in_end - bs);
    if (bs_len == 0) return NEX_ERR_CORRUPT;

    uint64_t bit_buf = 0;
    int bit_count = 0;
    size_t bs_pos = 0;

    #define FSE_REFILL() do { \
        while (bit_count < 56 && bs_pos < bs_len) { \
            bit_buf = (bit_buf << 8) | bs[bs_pos++]; \
            bit_count += 8; \
        } \
    } while(0)

    #define FSE_READ_BITS(n) ({ \
        FSE_REFILL(); \
        bit_count -= (int)(n); \
        (uint32_t)((bit_buf >> bit_count) & ((1U << (n)) - 1)); \
    })

    /* Read initial state as table index [0, TABLE_SIZE) */
    uint32_t state = FSE_READ_BITS(FSE_TABLE_LOG);

    /* Decode loop */
    for (uint32_t i = 0; i < orig_size; i++) {
        if (state >= FSE_TABLE_SIZE) { out->size = i; return NEX_ERR_CORRUPT; }

        fse_dec_entry_t entry = decode_table[state];
        out->data[i] = entry.symbol;

        uint32_t nb = entry.nb_bits;
        uint32_t bits = (nb > 0) ? FSE_READ_BITS(nb) : 0;
        state = (uint32_t)entry.base + bits;
    }

    #undef FSE_READ_BITS
    #undef FSE_REFILL

    out->size = orig_size;
    return NEX_OK;
}


