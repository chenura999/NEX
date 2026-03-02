/*
 * NEX Compress — Input Analyzer
 * Data profiling and pipeline selection
 */

#include "nex_internal.h"
#include <math.h>

/* ── File Signatures ─────────────────────────────────────────────── */

typedef struct {
    const uint8_t *magic;
    size_t         magic_len;
    nex_data_type_t type;
} nex_signature_t;

static const uint8_t sig_png[]  = {0x89, 0x50, 0x4E, 0x47};
static const uint8_t sig_jpg[]  = {0xFF, 0xD8, 0xFF};
static const uint8_t sig_gif[]  = {0x47, 0x49, 0x46, 0x38};
static const uint8_t sig_zip[]  = {0x50, 0x4B, 0x03, 0x04};
static const uint8_t sig_gz[]   = {0x1F, 0x8B};
static const uint8_t sig_xz[]   = {0xFD, 0x37, 0x7A, 0x58, 0x5A};
static const uint8_t sig_zstd[] = {0x28, 0xB5, 0x2F, 0xFD};
static const uint8_t sig_bz2[]  = {0x42, 0x5A, 0x68};
static const uint8_t sig_elf[]  = {0x7F, 0x45, 0x4C, 0x46};
static const uint8_t sig_pe[]   = {0x4D, 0x5A};
static const uint8_t sig_pdf[]  = {0x25, 0x50, 0x44, 0x46};
static const uint8_t sig_mp4[]  = {0x00, 0x00, 0x00};  /* ftyp at +4 */
static const uint8_t sig_flac[] = {0x66, 0x4C, 0x61, 0x43};

static const nex_signature_t signatures[] = {
    { sig_png,  4, NEX_DATA_MEDIA      },
    { sig_jpg,  3, NEX_DATA_MEDIA      },
    { sig_gif,  4, NEX_DATA_MEDIA      },
    { sig_flac, 4, NEX_DATA_MEDIA      },
    { sig_zip,  4, NEX_DATA_COMPRESSED },
    { sig_gz,   2, NEX_DATA_COMPRESSED },
    { sig_xz,   5, NEX_DATA_COMPRESSED },
    { sig_zstd, 4, NEX_DATA_COMPRESSED },
    { sig_bz2,  3, NEX_DATA_COMPRESSED },
    { sig_elf,  4, NEX_DATA_EXEC       },
    { sig_pe,   2, NEX_DATA_EXEC       },
    { sig_pdf,  4, NEX_DATA_STRUCTURED },
};

#define NUM_SIGNATURES (sizeof(signatures) / sizeof(signatures[0]))

/* ── Entropy Calculation ─────────────────────────────────────────── */

static double compute_entropy(const uint32_t *histogram, size_t total) {
    if (total == 0) return 0.0;
    double entropy = 0.0;
    double inv_total = 1.0 / (double)total;

    for (int i = 0; i < 256; i++) {
        if (histogram[i] == 0) continue;
        double p = (double)histogram[i] * inv_total;
        entropy -= p * log2(p);
    }
    return entropy;
}

/* ── Byte Histogram ──────────────────────────────────────────────── */

static void compute_histogram(const uint8_t *data, size_t size,
                              uint32_t *histogram) {
    memset(histogram, 0, 256 * sizeof(uint32_t));

#ifdef NEX_HAS_AVX2
    /* Vectorized histogram for large inputs */
    if (size >= 256) {
        /* Process in scalar for simplicity — AVX2 histogram is complex.
         * The bottleneck is random memory access, not computation. */
    }
#endif

    for (size_t i = 0; i < size; i++) {
        histogram[data[i]]++;
    }
}

/* ── Data Analysis ───────────────────────────────────────────────── */

void nex_analyze(const uint8_t *data, size_t size, nex_profile_t *profile) {
    memset(profile, 0, sizeof(*profile));

    if (size == 0) {
        profile->type = NEX_DATA_UNKNOWN;
        profile->is_compressible = false;
        return;
    }

    /* Sample size: min(64KB, full data) */
    size_t sample_size = NEX_MIN(size, NEX_SAMPLE_SIZE);

    /* 1. Check file signatures */
    for (size_t i = 0; i < NUM_SIGNATURES; i++) {
        if (size >= signatures[i].magic_len &&
            memcmp(data, signatures[i].magic, signatures[i].magic_len) == 0) {
            profile->type = signatures[i].type;
            break;
        }
    }

    /* 2. Compute byte histogram over sample */
    compute_histogram(data, sample_size, profile->histogram);

    /* 3. Shannon entropy */
    profile->entropy = compute_entropy(profile->histogram, sample_size);

    /* 4. Text ratio (printable ASCII 0x20-0x7E + whitespace) */
    uint64_t text_count = 0;
    for (int c = 0x20; c <= 0x7E; c++) {
        text_count += profile->histogram[c];
    }
    text_count += profile->histogram['\t'];
    text_count += profile->histogram['\n'];
    text_count += profile->histogram['\r'];
    profile->text_ratio = (double)text_count / (double)sample_size;

    /* 5. Classify if not already identified by signature */
    if (profile->type == NEX_DATA_UNKNOWN) {
        if (profile->text_ratio > 0.85) {
            profile->type = NEX_DATA_TEXT;
        } else if (profile->entropy > 7.5) {
            profile->type = NEX_DATA_COMPRESSED; /* near-random */
        } else {
            profile->type = NEX_DATA_BINARY;
        }
    }

    /* 6. Estimate repetition (rough: count of unique byte values used) */
    int unique_bytes = 0;
    for (int i = 0; i < 256; i++) {
        if (profile->histogram[i] > 0) unique_bytes++;
    }
    profile->repetition = 1.0 - ((double)unique_bytes / 256.0);

    /* 7. Compressibility decision */
    profile->is_compressible = (profile->entropy < 7.5) &&
                                (profile->type != NEX_DATA_COMPRESSED) &&
                                (profile->type != NEX_DATA_MEDIA);
}

/* ── Pipeline Selection ──────────────────────────────────────────── */

nex_pipeline_id_t nex_select_pipeline(const nex_profile_t *profile, int level) {
    /* Incompressible data → store raw */
    if (!profile->is_compressible) {
        return NEX_PIPE_STORE;
    }

    /* Fast mode override */
    if (level <= 2) {
        return NEX_PIPE_FAST;
    }

    /* High compression request → MAX pipeline */
    if (level >= 8) {
        return NEX_PIPE_MAX;
    }

    /* Default → balanced LZ pipeline (works well on all data types)
     * Note: BWT pipeline is available via explicit -p bwt flag
     * and excels on small-to-medium text, but auto-select uses
     * BALANCED for reliability and consistent speed. */
    return NEX_PIPE_BALANCED;
}
