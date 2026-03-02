/*
 * NEX Compress — Next-Generation Hybrid Compression System
 * Public API Header
 *
 * Copyright (c) 2026. All rights reserved.
 * License: MIT
 */

#ifndef NEX_H
#define NEX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Version ──────────────────────────────────────────────────────── */
#define NEX_VERSION_MAJOR  1
#define NEX_VERSION_MINOR  0
#define NEX_VERSION_PATCH  0
#define NEX_VERSION_STRING "1.0.0"

/* ── Error Codes ─────────────────────────────────────────────────── */
typedef enum {
    NEX_OK              =  0,
    NEX_ERR_NOMEM       = -1,
    NEX_ERR_IO          = -2,
    NEX_ERR_CORRUPT     = -3,
    NEX_ERR_CHECKSUM    = -4,
    NEX_ERR_UNSUPPORTED = -5,
    NEX_ERR_OVERFLOW    = -6,
    NEX_ERR_PARAM       = -7,
    NEX_ERR_FORMAT      = -8,
    NEX_ERR_THREAD      = -9,
} nex_status_t;

/* ── Pipeline IDs ────────────────────────────────────────────────── */
typedef enum {
    NEX_PIPE_AUTO     = 0,  /* Auto-select best pipeline       */
    NEX_PIPE_MAX      = 1,  /* LZ optimal + rANS (best ratio)  */
    NEX_PIPE_BWT      = 2,  /* BWT + MTF + RLE + rANS (text)   */
    NEX_PIPE_BALANCED = 3,  /* LZ lazy + rANS (general)        */
    NEX_PIPE_FAST     = 4,  /* LZ greedy + Huffman (speed)     */
    NEX_PIPE_STORE    = 5,  /* Raw copy (incompressible)       */
    NEX_PIPE_COUNT    = 6,
} nex_pipeline_id_t;

/* ── Compression Level ───────────────────────────────────────────── */
#define NEX_LEVEL_MIN   1
#define NEX_LEVEL_MAX   9
#define NEX_LEVEL_DEFAULT 6

/* ── Buffer ──────────────────────────────────────────────────────── */
typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   capacity;
} nex_buffer_t;

/* ── Configuration ───────────────────────────────────────────────── */
typedef struct {
    int              level;        /* 1-9, default 6              */
    nex_pipeline_id_t pipeline;   /* NEX_PIPE_AUTO for auto      */
    int              threads;     /* 0 = auto-detect             */
    size_t           chunk_size;  /* default 1MB                 */
    size_t           mem_limit;   /* 0 = auto                    */
    bool             verbose;     /* print diagnostics           */
} nex_config_t;

/* ── Compression Statistics ──────────────────────────────────────── */
typedef struct {
    uint64_t original_size;
    uint64_t compressed_size;
    double   ratio;             /* compressed / original        */
    double   compress_time_ms;
    double   decompress_time_ms;
    double   compress_speed_mbs; /* MB/s                        */
    double   decompress_speed_mbs;
} nex_stats_t;

/* ── Public API ──────────────────────────────────────────────────── */

/* Initialize config with defaults */
void nex_config_init(nex_config_t *cfg);

/* Buffer operations */
nex_status_t nex_buffer_alloc(nex_buffer_t *buf, size_t capacity);
void         nex_buffer_free(nex_buffer_t *buf);

/* Compress in-memory */
nex_status_t nex_compress(const uint8_t *input, size_t input_size,
                          nex_buffer_t *output,
                          const nex_config_t *cfg,
                          nex_stats_t *stats);

/* Decompress in-memory */
nex_status_t nex_decompress(const uint8_t *input, size_t input_size,
                            nex_buffer_t *output,
                            nex_stats_t *stats);

/* File-based compress/decompress */
nex_status_t nex_compress_file(const char *input_path,
                               const char *output_path,
                               const nex_config_t *cfg,
                               nex_stats_t *stats);

nex_status_t nex_decompress_file(const char *input_path,
                                 const char *output_path,
                                 nex_stats_t *stats);

/* Error description */
const char *nex_strerror(nex_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* NEX_H */
