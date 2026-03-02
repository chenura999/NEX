/*
 * NEX Compress — Decompression Driver
 * Orchestrates container reading, chunk decompression, and verification
 */

#include "nex_internal.h"
#include <time.h>
#include <unistd.h>

/* Forward declaration */
nex_status_t nex_write_footer(nex_buffer_t *out);

/* ═══════════════════════════════════════════════════════════════════
 * Chunk Compression Task (used by both compress and decompress)
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    const uint8_t    *input;
    size_t            input_size;
    nex_buffer_t      output;
    nex_pipeline_id_t pipeline;
    int               level;
    uint32_t          original_checksum;
    nex_status_t      status;
    bool              is_compress; /* true = compress, false = decompress */
    
    /* V2 Upgrade: Dictionary */
    const uint8_t    *dict_data;
    size_t            dict_size;
} nex_chunk_task_t;

static void chunk_compress_fn(void *arg) {
    nex_chunk_task_t *task = (nex_chunk_task_t *)arg;
    memset(&task->output, 0, sizeof(task->output));

    task->status = nex_pipeline_compress(task->pipeline,
                                          task->input, task->input_size,
                                          &task->output, task->level,
                                          task->dict_data, task->dict_size);
}

static void chunk_decompress_fn(void *arg) {
    nex_chunk_task_t *task = (nex_chunk_task_t *)arg;
    memset(&task->output, 0, sizeof(task->output));

    task->status = nex_pipeline_decompress(task->pipeline,
                                            task->input, task->input_size,
                                            &task->output, task->level,
                                            task->dict_data, task->dict_size);
}

/* ═══════════════════════════════════════════════════════════════════
 * Public API: nex_buffer operations
 * ═══════════════════════════════════════════════════════════════════ */

nex_status_t nex_buffer_alloc(nex_buffer_t *buf, size_t capacity) {
    buf->data = (uint8_t *)malloc(capacity);
    if (!buf->data) return NEX_ERR_NOMEM;
    buf->size = 0;
    buf->capacity = capacity;
    return NEX_OK;
}

void nex_buffer_free(nex_buffer_t *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;
}

void nex_config_init(nex_config_t *cfg) {
    cfg->level = NEX_LEVEL_DEFAULT;
    cfg->pipeline = NEX_PIPE_AUTO;
    cfg->threads = 0; /* auto */
    cfg->chunk_size = NEX_DEFAULT_CHUNK_SIZE;
    cfg->mem_limit = 0; /* auto */
    cfg->verbose = false;
    cfg->dict_data = NULL;
    cfg->dict_size = 0;
}

const char *nex_strerror(nex_status_t status) {
    switch (status) {
        case NEX_OK:              return "OK";
        case NEX_ERR_NOMEM:       return "Out of memory";
        case NEX_ERR_IO:          return "I/O error";
        case NEX_ERR_CORRUPT:     return "Data corrupt";
        case NEX_ERR_CHECKSUM:    return "Checksum mismatch";
        case NEX_ERR_UNSUPPORTED: return "Unsupported format version";
        case NEX_ERR_OVERFLOW:    return "Buffer overflow";
        case NEX_ERR_PARAM:       return "Invalid parameter";
        case NEX_ERR_FORMAT:      return "Invalid file format";
        case NEX_ERR_THREAD:      return "Thread error";
        default:                  return "Unknown error";
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * In-Memory Compress
 * ═══════════════════════════════════════════════════════════════════ */

nex_status_t nex_compress(const uint8_t *input, size_t input_size,
                          nex_buffer_t *output,
                          const nex_config_t *cfg,
                          nex_stats_t *stats) {
    if (!input || !output || !cfg) return NEX_ERR_PARAM;
    if (input_size == 0) {
        output->size = 0;
        return NEX_OK;
    }

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    /* Determine chunk size */
    size_t chunk_size = cfg->chunk_size;
    
    /* V2 Industrial Upgrade: Dynamic Macro-Scale Windows */
    if (chunk_size == NEX_DEFAULT_CHUNK_SIZE) {
        if (cfg->level <= 3) {
            chunk_size = 4 * 1024 * 1024;       /* 4MB — max parallelism */
        } else if (cfg->level <= 6) {
            chunk_size = 8 * 1024 * 1024;       /* 8MB */
        } else {
            chunk_size = 16 * 1024 * 1024;      /* 16MB — balance ratio + threads */
        }
    }

    if (chunk_size < NEX_MIN_CHUNK_SIZE) chunk_size = NEX_MIN_CHUNK_SIZE;
    if (chunk_size > NEX_MAX_CHUNK_SIZE) chunk_size = NEX_MAX_CHUNK_SIZE;

    /* Calculate number of chunks */
    uint32_t num_chunks = (uint32_t)((input_size + chunk_size - 1) / chunk_size);

    /* Allocate chunk tasks */
    nex_chunk_task_t *tasks = (nex_chunk_task_t *)calloc(
        num_chunks, sizeof(nex_chunk_task_t));
    if (!tasks) return NEX_ERR_NOMEM;

    nex_chunk_entry_t *entries = (nex_chunk_entry_t *)calloc(
        num_chunks, sizeof(nex_chunk_entry_t));
    if (!entries) { free(tasks); return NEX_ERR_NOMEM; }

    /* Analyze and prepare chunks */
    for (uint32_t i = 0; i < num_chunks; i++) {
        size_t offset = (size_t)i * chunk_size;
        size_t remaining = input_size - offset;
        size_t this_chunk = NEX_MIN(remaining, chunk_size);

        tasks[i].input = input + offset;
        tasks[i].input_size = this_chunk;
        tasks[i].level = cfg->level;
        tasks[i].is_compress = true;
        tasks[i].dict_data = cfg->dict_data;
        tasks[i].dict_size = cfg->dict_size;

        /* Pipeline selection */
        if (cfg->pipeline != NEX_PIPE_AUTO) {
            tasks[i].pipeline = cfg->pipeline;
        } else {
            nex_profile_t profile;
            nex_analyze(tasks[i].input, tasks[i].input_size, &profile);
            tasks[i].pipeline = nex_select_pipeline(&profile, cfg->level);
        }

        /* Compute chunk checksum */
        tasks[i].original_checksum = nex_xxh32(tasks[i].input,
                                                tasks[i].input_size, 0);
    }

    /* Compress chunks (parallel or serial) */
    int num_threads = cfg->threads;
    if (num_threads == 0) {
        long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
        num_threads = (ncpus > 0) ? (int)ncpus : 1;
    }

    if (num_threads > 1 && num_chunks > 1) {
        /* Parallel compression */
        nex_thread_pool_t *pool = nex_pool_create(num_threads);
        if (!pool) {
            /* Fallback to serial */
            num_threads = 1;
        } else {
            for (uint32_t i = 0; i < num_chunks; i++) {
                nex_pool_submit(pool, chunk_compress_fn, &tasks[i]);
            }
            nex_pool_wait(pool);
            nex_pool_destroy(pool);
        }
    }

    if (num_threads <= 1 || num_chunks <= 1) {
        /* Serial compression */
        for (uint32_t i = 0; i < num_chunks; i++) {
            chunk_compress_fn(&tasks[i]);
        }
    }

    /* Check for errors */
    for (uint32_t i = 0; i < num_chunks; i++) {
        if (tasks[i].status != NEX_OK) {
            nex_status_t err = tasks[i].status;
            for (uint32_t j = 0; j < num_chunks; j++) {
                nex_buffer_free(&tasks[j].output);
            }
            free(tasks);
            free(entries);
            return err;
        }
    }

    /* Build container */
    
    /* V2 Upgrade: Check if micro-container format applies */
    bool use_micro = false;
    if (num_chunks == 1 && input_size <= 0xFFFFFFFFULL) {
        use_micro = true;
    }

    if (use_micro) {
        size_t header_size = 12;
        size_t total_compressed = tasks[0].output.size;
        size_t total_output = header_size + total_compressed;

        if (output->capacity < total_output) {
            uint8_t *new_data = (uint8_t *)realloc(output->data, total_output);
            if (!new_data) {
                nex_buffer_free(&tasks[0].output);
                free(tasks);
                free(entries);
                return NEX_ERR_NOMEM;
            }
            output->data = new_data;
            output->capacity = total_output;
        }
        output->size = 0;

        uint8_t *p = output->data;
        uint32_t magic = NEX_MAGIC;
        memcpy(p, &magic, 4); p += 4;
        
        uint16_t version = NEX_FORMAT_VER;
        memcpy(p, &version, 2); p += 2;
        
        uint16_t flags = NEX_FLAG_MICRO | (tasks[0].pipeline << 8);
        memcpy(p, &flags, 2); p += 2;
        
        uint32_t orig = (uint32_t)input_size;
        memcpy(p, &orig, 4); p += 4;
        
        memcpy(p, tasks[0].output.data, total_compressed);
        output->size = total_output;

    } else {
        /* Standard Container */
        size_t header_size = 30;
        size_t table_size = (size_t)num_chunks * 21;
        size_t data_offset = header_size + table_size;

        size_t total_compressed_data = 0;
        for (uint32_t i = 0; i < num_chunks; i++) {
            total_compressed_data += tasks[i].output.size;
        }

        size_t total_output = data_offset + total_compressed_data + 4; /* +footer */

        if (output->capacity < total_output) {
            uint8_t *new_data = (uint8_t *)realloc(output->data, total_output);
            if (!new_data) {
                for (uint32_t j = 0; j < num_chunks; j++) nex_buffer_free(&tasks[j].output);
                free(tasks);
                free(entries);
                return NEX_ERR_NOMEM;
            }
            output->data = new_data;
            output->capacity = total_output;
        }
        output->size = 0;

        nex_header_t hdr = {
            .magic = NEX_MAGIC,
            .version = NEX_FORMAT_VER,
            .flags = 0,
            .original_size = input_size,
            .chunk_count = num_chunks,
            .checksum = nex_xxh64(input, input_size, 0),
        };
        nex_write_header(output, &hdr);

        uint64_t cur_offset = data_offset;
        for (uint32_t i = 0; i < num_chunks; i++) {
            entries[i].compressed_offset = cur_offset;
            entries[i].compressed_size = (uint32_t)tasks[i].output.size;
            entries[i].original_size = (uint32_t)tasks[i].input_size;
            entries[i].pipeline_id = (uint8_t)tasks[i].pipeline;
            entries[i].checksum = tasks[i].original_checksum;
            cur_offset += tasks[i].output.size;
        }
        nex_write_chunk_table(output, entries, num_chunks);

        for (uint32_t i = 0; i < num_chunks; i++) {
            memcpy(output->data + output->size, tasks[i].output.data,
                   tasks[i].output.size);
            output->size += tasks[i].output.size;
        }

        nex_write_footer(output);
    }

    /* Stats */
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    if (stats) {
        double elapsed = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0 +
                          (ts_end.tv_nsec - ts_start.tv_nsec) / 1e6;
        stats->original_size = input_size;
        stats->compressed_size = output->size;
        stats->ratio = (double)output->size / (double)input_size;
        stats->compress_time_ms = elapsed;
        stats->compress_speed_mbs = (input_size / (1024.0 * 1024.0)) /
                                     (elapsed / 1000.0);
    }

    /* Cleanup */
    for (uint32_t i = 0; i < num_chunks; i++) {
        nex_buffer_free(&tasks[i].output);
    }
    free(tasks);
    free(entries);

    return NEX_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * In-Memory Decompress
 * ═══════════════════════════════════════════════════════════════════ */

nex_status_t nex_decompress(const uint8_t *input, size_t input_size,
                            nex_buffer_t *output,
                            const nex_config_t *cfg,
                            nex_stats_t *stats) {
    if (!input || !output || !cfg) return NEX_ERR_PARAM;

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    if (input_size < 12) return NEX_ERR_FORMAT;
    uint32_t magic;
    memcpy(&magic, input, 4);
    if (magic != NEX_MAGIC) return NEX_ERR_FORMAT;

    uint16_t flags;
    memcpy(&flags, input + 6, 2);

    if (flags & NEX_FLAG_MICRO) {
        /* Sub-1KB Micro-Container Path */
        uint32_t orig_size;
        memcpy(&orig_size, input + 8, 4);
        
        uint8_t pipeline_id = (flags >> 8) & 0xFF;
        const uint8_t *comp_data = input + 12;
        size_t comp_size = input_size - 12;

        if (output->capacity < orig_size) {
            uint8_t *new_data = (uint8_t *)realloc(output->data, orig_size);
            if (!new_data) return NEX_ERR_NOMEM;
            output->data = new_data;
            output->capacity = orig_size;
        }

        nex_status_t st = nex_pipeline_decompress((nex_pipeline_id_t)pipeline_id,
                                                   comp_data, comp_size, output,
                                                   NEX_LEVEL_DEFAULT,
                                                   cfg->dict_data, cfg->dict_size);
        
        if (st == NEX_OK && output->size != orig_size) return NEX_ERR_CORRUPT;
        
        clock_gettime(CLOCK_MONOTONIC, &ts_end);
        if (stats) {
            double elapsed = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0 +
                              (ts_end.tv_nsec - ts_start.tv_nsec) / 1e6;
            stats->original_size = orig_size;
            stats->compressed_size = input_size;
            stats->ratio = (double)input_size / (double)orig_size;
            stats->decompress_time_ms = elapsed;
            stats->decompress_speed_mbs = (orig_size / (1024.0 * 1024.0)) /
                                           (elapsed / 1000.0);
        }
        return st;
    }
    
    /* Read header (Standard format) */
    nex_header_t hdr;
    nex_status_t st = nex_read_header(input, input_size, &hdr);
    if (st != NEX_OK) return st;

    /* Read chunk table */
    nex_chunk_entry_t *entries = NULL;
    st = nex_read_chunk_table(input + 30, input_size - 30,
                               &entries, hdr.chunk_count);
    if (st != NEX_OK) return st;

    /* Allocate output */
    if (output->capacity < hdr.original_size) {
        uint8_t *new_data = (uint8_t *)realloc(output->data,
                                                (size_t)hdr.original_size);
        if (!new_data) { free(entries); return NEX_ERR_NOMEM; }
        output->data = new_data;
        output->capacity = (size_t)hdr.original_size;
    }

    /* Decompress each chunk */
    nex_chunk_task_t *tasks = (nex_chunk_task_t *)calloc(
        hdr.chunk_count, sizeof(nex_chunk_task_t));
    if (!tasks) { free(entries); return NEX_ERR_NOMEM; }

    for (uint32_t i = 0; i < hdr.chunk_count; i++) {
        nex_chunk_entry_t *e = &entries[i];

        if (e->compressed_offset + e->compressed_size > input_size) {
            free(entries);
            free(tasks);
            return NEX_ERR_CORRUPT;
        }

        tasks[i].input = input + e->compressed_offset;
        tasks[i].input_size = e->compressed_size;
        tasks[i].pipeline = (nex_pipeline_id_t)e->pipeline_id;
        tasks[i].level = NEX_LEVEL_DEFAULT;
        tasks[i].original_checksum = e->checksum;
        tasks[i].is_compress = false;
        tasks[i].dict_data = cfg->dict_data;
        tasks[i].dict_size = cfg->dict_size;
    }

    /* Decompress (parallel if multiple chunks) */
    if (hdr.chunk_count > 1) {
        nex_thread_pool_t *pool = nex_pool_create(0);
        if (pool) {
            for (uint32_t i = 0; i < hdr.chunk_count; i++) {
                nex_pool_submit(pool, chunk_decompress_fn, &tasks[i]);
            }
            nex_pool_wait(pool);
            nex_pool_destroy(pool);
        } else {
            for (uint32_t i = 0; i < hdr.chunk_count; i++) {
                chunk_decompress_fn(&tasks[i]);
            }
        }
    } else {
        for (uint32_t i = 0; i < hdr.chunk_count; i++) {
            chunk_decompress_fn(&tasks[i]);
        }
    }

    /* Assemble output and verify checksums */
    size_t out_pos = 0;
    for (uint32_t i = 0; i < hdr.chunk_count; i++) {
        if (tasks[i].status != NEX_OK) {
            st = tasks[i].status;
            for (uint32_t j = 0; j < hdr.chunk_count; j++) {
                nex_buffer_free(&tasks[j].output);
            }
            free(tasks);
            free(entries);
            return st;
        }

        /* Verify chunk checksum */
        uint32_t computed = nex_xxh32(tasks[i].output.data,
                                       tasks[i].output.size, 0);
        if (computed != tasks[i].original_checksum) {
            for (uint32_t j = 0; j < hdr.chunk_count; j++) {
                nex_buffer_free(&tasks[j].output);
            }
            free(tasks);
            free(entries);
            return NEX_ERR_CHECKSUM;
        }

        /* Copy to output */
        memcpy(output->data + out_pos, tasks[i].output.data,
               tasks[i].output.size);
        out_pos += tasks[i].output.size;
    }

    output->size = out_pos;

    /* Verify full-file checksum */
    uint64_t full_check = nex_xxh64(output->data, output->size, 0);
    if (full_check != hdr.checksum) {
        for (uint32_t i = 0; i < hdr.chunk_count; i++) {
            nex_buffer_free(&tasks[i].output);
        }
        free(tasks);
        free(entries);
        return NEX_ERR_CHECKSUM;
    }

    /* Stats */
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    if (stats) {
        double elapsed = (ts_end.tv_sec - ts_start.tv_sec) * 1000.0 +
                          (ts_end.tv_nsec - ts_start.tv_nsec) / 1e6;
        stats->original_size = hdr.original_size;
        stats->compressed_size = input_size;
        stats->ratio = (double)input_size / (double)hdr.original_size;
        stats->decompress_time_ms = elapsed;
        stats->decompress_speed_mbs = (hdr.original_size / (1024.0 * 1024.0)) /
                                       (elapsed / 1000.0);
    }

    /* Cleanup */
    for (uint32_t i = 0; i < hdr.chunk_count; i++) {
        nex_buffer_free(&tasks[i].output);
    }
    free(tasks);
    free(entries);

    return NEX_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * File-based Compress/Decompress
 * ═══════════════════════════════════════════════════════════════════ */

nex_status_t nex_compress_file(const char *input_path,
                               const char *output_path,
                               const nex_config_t *cfg,
                               nex_stats_t *stats) {
    /* Read input file */
    FILE *fin = fopen(input_path, "rb");
    if (!fin) return NEX_ERR_IO;

    fseek(fin, 0, SEEK_END);
    long file_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    if (file_size < 0) { fclose(fin); return NEX_ERR_IO; }

    uint8_t *input_data = NULL;
    size_t input_size = (size_t)file_size;

    if (input_size > 0) {
        input_data = (uint8_t *)malloc(input_size);
        if (!input_data) { fclose(fin); return NEX_ERR_NOMEM; }

        if (fread(input_data, 1, input_size, fin) != input_size) {
            free(input_data);
            fclose(fin);
            return NEX_ERR_IO;
        }
    }
    fclose(fin);

    /* Compress */
    nex_buffer_t output = {0};
    nex_status_t st = nex_compress(input_data, input_size, &output, cfg, stats);

    free(input_data);

    if (st != NEX_OK) {
        nex_buffer_free(&output);
        return st;
    }

    /* Write output file */
    FILE *fout = fopen(output_path, "wb");
    if (!fout) { nex_buffer_free(&output); return NEX_ERR_IO; }

    if (fwrite(output.data, 1, output.size, fout) != output.size) {
        fclose(fout);
        nex_buffer_free(&output);
        return NEX_ERR_IO;
    }

    fclose(fout);
    nex_buffer_free(&output);
    return NEX_OK;
}

nex_status_t nex_decompress_file(const char *input_path,
                                 const char *output_path,
                                 const nex_config_t *cfg,
                                 nex_stats_t *stats) {
    /* Read input file */
    FILE *fin = fopen(input_path, "rb");
    if (!fin) return NEX_ERR_IO;

    fseek(fin, 0, SEEK_END);
    long file_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    if (file_size <= 0) { fclose(fin); return NEX_ERR_IO; }

    size_t input_size = (size_t)file_size;
    uint8_t *input_data = (uint8_t *)malloc(input_size);
    if (!input_data) { fclose(fin); return NEX_ERR_NOMEM; }

    if (fread(input_data, 1, input_size, fin) != input_size) {
        free(input_data);
        fclose(fin);
        return NEX_ERR_IO;
    }
    fclose(fin);

    /* Decompress */
    nex_buffer_t output = {0};
    nex_status_t st = nex_decompress(input_data, input_size, &output, cfg, stats);

    free(input_data);

    if (st != NEX_OK) {
        nex_buffer_free(&output);
        return st;
    }

    /* Write output */
    FILE *fout = fopen(output_path, "wb");
    if (!fout) { nex_buffer_free(&output); return NEX_ERR_IO; }

    if (fwrite(output.data, 1, output.size, fout) != output.size) {
        fclose(fout);
        nex_buffer_free(&output);
        return NEX_ERR_IO;
    }

    fclose(fout);
    nex_buffer_free(&output);
    return NEX_OK;
}
