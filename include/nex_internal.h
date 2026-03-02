/*
 * NEX Compress — Internal Header
 * Private types, macros, and module interfaces
 */

#ifndef NEX_INTERNAL_H
#define NEX_INTERNAL_H

#include "nex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef __SSE2__
#include <immintrin.h>
#define NEX_HAS_SSE2 1
#endif

#ifdef __AVX2__
#define NEX_HAS_AVX2 1
#endif

/* ── Constants ───────────────────────────────────────────────────── */
#define NEX_MAGIC         0x0158454EU  /* "NEX\x01" LE              */
#define NEX_FOOTER_MAGIC  0x014E4558U  /* "XEN\x01" LE              */
#define NEX_FORMAT_VER    1
#define NEX_FLAG_MICRO    0x0001       /* Sub-1KB micro header flag */

#define NEX_DEFAULT_CHUNK_SIZE  (1 << 20)   /* 1 MB                 */
#define NEX_MIN_CHUNK_SIZE      (1 << 12)   /* 4 KB                 */
#define NEX_MAX_CHUNK_SIZE      (1 << 27)   /* 128 MB               */

#define NEX_LZ_MIN_MATCH       3
#define NEX_LZ_MAX_MATCH       258
#define NEX_LZ_HASH_BITS       16
#define NEX_LZ_HASH_SIZE       (1 << NEX_LZ_HASH_BITS)
#define NEX_LZ_MAX_CHAIN       64

#define NEX_ENTROPY_ALPHABET   256
#define NEX_ANS_BITS           32
#define NEX_ANS_LOWER          (1U << 23)
#define NEX_ANS_UPPER          (1U << 31)
#define NEX_FREQ_BITS          12
#define NEX_FREQ_SUM           (1 << NEX_FREQ_BITS)

#define NEX_SAMPLE_SIZE        (64 * 1024)  /* 64 KB analysis sample */

/* ── Utility Macros ──────────────────────────────────────────────── */
#define NEX_MIN(a, b) ((a) < (b) ? (a) : (b))
#define NEX_MAX(a, b) ((a) > (b) ? (a) : (b))
#define NEX_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

#define NEX_LIKELY(x)   __builtin_expect(!!(x), 1)
#define NEX_UNLIKELY(x) __builtin_expect(!!(x), 0)

/* ── Data Profile (from analyzer) ────────────────────────────────── */
typedef enum {
    NEX_DATA_UNKNOWN    = 0,
    NEX_DATA_TEXT       = 1,
    NEX_DATA_BINARY     = 2,
    NEX_DATA_EXEC       = 3,
    NEX_DATA_STRUCTURED = 4,
    NEX_DATA_COMPRESSED = 5,
    NEX_DATA_MEDIA      = 6,
} nex_data_type_t;

typedef struct {
    nex_data_type_t type;
    double          entropy;        /* 0.0 - 8.0 bits/byte         */
    double          text_ratio;     /* % printable ASCII            */
    double          repetition;     /* estimated dictionary gain    */
    uint32_t        histogram[256]; /* byte frequency histogram     */
    bool            is_compressible;
} nex_profile_t;

/* ── LZ Token ────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  is_match;   /* 0 = literal, 1 = match              */
    uint8_t  literal;    /* literal byte (if is_match == 0)      */
    uint16_t length;     /* match length                         */
    uint32_t offset;     /* match offset (distance back)         */
} nex_lz_token_t;

typedef struct {
    nex_lz_token_t *tokens;
    size_t          count;
    size_t          capacity;
} nex_lz_sequence_t;

/* ── Chunk Metadata (in container) ───────────────────────────────── */
typedef struct {
    uint64_t compressed_offset;
    uint32_t compressed_size;
    uint32_t original_size;
    uint8_t  pipeline_id;
    uint32_t checksum;     /* XXH32 of original */
} nex_chunk_entry_t;

/* ── Container Header ────────────────────────────────────────────── */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint64_t original_size;
    uint32_t chunk_count;
    uint64_t checksum;     /* XXH64 of original */
} nex_header_t;

/* ── Arena Allocator ─────────────────────────────────────────────── */
typedef struct nex_arena_block {
    struct nex_arena_block *next;
    size_t  size;
    size_t  used;
    uint8_t data[];
} nex_arena_block_t;

typedef struct {
    nex_arena_block_t *head;
    size_t default_block_size;
} nex_arena_t;

/* ── Pipeline Stage Function Types ───────────────────────────────── */
typedef nex_status_t (*nex_stage_fn)(const uint8_t *in, size_t in_size,
                                     nex_buffer_t *out, int level,
                                     const uint8_t *dict, size_t dict_size);

typedef struct {
    nex_pipeline_id_t id;
    const char       *name;
    nex_stage_fn     *compress_stages;
    nex_stage_fn     *decompress_stages;
    int               num_stages;
} nex_pipeline_t;

/* ── Module APIs (internal) ──────────────────────────────────────── */

/* memory.c */
void         nex_arena_init(nex_arena_t *arena, size_t block_size);
void        *nex_arena_alloc(nex_arena_t *arena, size_t size);
void         nex_arena_reset(nex_arena_t *arena);
void         nex_arena_destroy(nex_arena_t *arena);
void        *nex_aligned_alloc(size_t size, size_t alignment);
void         nex_aligned_free(void *ptr);
size_t       nex_available_memory(void);

/* analyzer.c */
void         nex_analyze(const uint8_t *data, size_t size, nex_profile_t *profile);
nex_pipeline_id_t nex_select_pipeline(const nex_profile_t *profile, int level);

/* lz_match.c */
nex_status_t nex_lz_compress(const uint8_t *in, size_t in_size,
                              nex_buffer_t *out, int level,
                              const uint8_t *dict, size_t dict_size);
nex_status_t nex_lz_decompress(const uint8_t *in, size_t in_size,
                                nex_buffer_t *out, int level,
                                const uint8_t *dict, size_t dict_size);

/* lz_fast.c */
nex_status_t nex_lz_fast_compress(const uint8_t *in, size_t in_size,
                                   nex_buffer_t *out, int level,
                                   const uint8_t *dict, size_t dict_size);
nex_status_t nex_lz_fast_decompress(const uint8_t *in, size_t in_size,
                                     nex_buffer_t *out, int level,
                                     const uint8_t *dict, size_t dict_size);

/* transform.c */
nex_status_t nex_bwt_forward(const uint8_t *in, size_t in_size,
                              nex_buffer_t *out, int level,
                              const uint8_t *dict, size_t dict_size);
nex_status_t nex_bwt_inverse(const uint8_t *in, size_t in_size,
                              nex_buffer_t *out, int level,
                              const uint8_t *dict, size_t dict_size);
nex_status_t nex_delta_encode(const uint8_t *in, size_t in_size,
                               nex_buffer_t *out, int level,
                               const uint8_t *dict, size_t dict_size);
nex_status_t nex_delta_decode(const uint8_t *in, size_t in_size,
                               nex_buffer_t *out, int level,
                               const uint8_t *dict, size_t dict_size);
nex_status_t nex_mtf_rle_encode(const uint8_t *in, size_t in_size,
                                 nex_buffer_t *out, int level,
                                 const uint8_t *dict, size_t dict_size);
nex_status_t nex_mtf_rle_decode(const uint8_t *in, size_t in_size,
                                 nex_buffer_t *out, int level,
                                 const uint8_t *dict, size_t dict_size);
nex_status_t nex_bcj_x86_encode(const uint8_t *in, size_t in_size,
                                 nex_buffer_t *out, int level,
                                 const uint8_t *dict, size_t dict_size);
nex_status_t nex_bcj_x86_decode(const uint8_t *in, size_t in_size,
                                 nex_buffer_t *out, int level,
                                 const uint8_t *dict, size_t dict_size);

/* entropy.c */
nex_status_t nex_rans_compress(const uint8_t *in, size_t in_size,
                                nex_buffer_t *out, int level,
                                const uint8_t *dict, size_t dict_size);
nex_status_t nex_rans_decompress(const uint8_t *in, size_t in_size,
                                  nex_buffer_t *out, int level,
                                  const uint8_t *dict, size_t dict_size);
nex_status_t nex_huffman_compress(const uint8_t *in, size_t in_size,
                                   nex_buffer_t *out, int level,
                                   const uint8_t *dict, size_t dict_size);
nex_status_t nex_huffman_decompress(const uint8_t *in, size_t in_size,
                                     nex_buffer_t *out, int level,
                                     const uint8_t *dict, size_t dict_size);
nex_status_t nex_fse_compress(const uint8_t *in, size_t in_size,
                               nex_buffer_t *out, int level,
                               const uint8_t *dict, size_t dict_size);
nex_status_t nex_fse_decompress(const uint8_t *in, size_t in_size,
                                 nex_buffer_t *out, int level,
                                 const uint8_t *dict, size_t dict_size);

/* pipeline.c */
const nex_pipeline_t *nex_get_pipeline(nex_pipeline_id_t id);
nex_status_t nex_pipeline_compress(nex_pipeline_id_t id,
                                    const uint8_t *in, size_t in_size,
                                    nex_buffer_t *out, int level,
                                    const uint8_t *dict, size_t dict_size);
nex_status_t nex_pipeline_decompress(nex_pipeline_id_t id,
                                      const uint8_t *in, size_t in_size,
                                      nex_buffer_t *out, int level,
                                      const uint8_t *dict, size_t dict_size);

/* container.c */
nex_status_t nex_write_header(nex_buffer_t *out, const nex_header_t *hdr);
nex_status_t nex_read_header(const uint8_t *data, size_t size, nex_header_t *hdr);
nex_status_t nex_write_chunk_table(nex_buffer_t *out,
                                    const nex_chunk_entry_t *entries,
                                    uint32_t count);
nex_status_t nex_read_chunk_table(const uint8_t *data, size_t size,
                                   nex_chunk_entry_t **entries,
                                   uint32_t count);

/* xxhash.c */
uint32_t nex_xxh32(const void *data, size_t len, uint32_t seed);
uint64_t nex_xxh64(const void *data, size_t len, uint64_t seed);

/* parallel.c */
typedef struct nex_thread_pool nex_thread_pool_t;
nex_thread_pool_t *nex_pool_create(int num_threads);
void               nex_pool_destroy(nex_thread_pool_t *pool);
typedef void (*nex_task_fn)(void *arg);
nex_status_t nex_pool_submit(nex_thread_pool_t *pool, nex_task_fn fn, void *arg);
void         nex_pool_wait(nex_thread_pool_t *pool);

#endif /* NEX_INTERNAL_H */
