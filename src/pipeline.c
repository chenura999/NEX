/*
 * NEX Compress — Pipeline Engine
 * Dynamic pipeline selection and execution
 */

#include "nex_internal.h"

/* ═══════════════════════════════════════════════════════════════════
 * Pipeline Definitions
 *
 * Each pipeline is a chain of transform stages. The compress stages
 * are executed in order; decompress stages are executed in reverse.
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Pipeline: MAX (LZ optimal → cascaded entropy) ──────────────── */
static nex_stage_fn pipe_max_compress[] = {
    nex_lz_compress,
    nex_cascaded_compress,
};
static nex_stage_fn pipe_max_decompress[] = {
    nex_cascaded_decompress,
    nex_lz_decompress,
};

/* ── Pipeline: BWT (BWT → MTF+RLE → rANS) ───────────────────────── */
static nex_stage_fn pipe_bwt_compress[] = {
    nex_bwt_forward,
    nex_mtf_rle_encode,
    nex_rans_compress,
};
static nex_stage_fn pipe_bwt_decompress[] = {
    nex_rans_decompress,
    nex_mtf_rle_decode,
    nex_bwt_inverse,
};

/* ── Pipeline: BALANCED (LZ lazy → cascaded entropy) ─────────────── */
static nex_stage_fn pipe_balanced_compress[] = {
    nex_lz_compress,
    nex_cascaded_compress,
};
static nex_stage_fn pipe_balanced_decompress[] = {
    nex_cascaded_decompress,
    nex_lz_decompress,
};

/* ── Pipeline: FAST (LZ4-style single-pass — no entropy stage) ───── */
static nex_stage_fn pipe_fast_compress[] = {
    nex_lz_fast_compress,
};
static nex_stage_fn pipe_fast_decompress[] = {
    nex_lz_fast_decompress,
};

/* ── Pipeline: EXEC (BCJ + LZ optimal + rANS) ─────────────────────── */
static nex_stage_fn pipe_exec_compress[] = {
    nex_bcj_x86_encode,
    nex_lz_compress,
    nex_rans_compress,
};
static nex_stage_fn pipe_exec_decompress[] = {
    nex_rans_decompress,
    nex_lz_decompress,
    nex_bcj_x86_decode,
};

/* ── Pipeline Registry ───────────────────────────────────────────── */

static nex_pipeline_t pipelines[] = {
    [NEX_PIPE_AUTO]     = { NEX_PIPE_AUTO, "auto", NULL, NULL, 0 },
    [NEX_PIPE_MAX]      = {
        NEX_PIPE_MAX, "max",
        pipe_max_compress, pipe_max_decompress, 2
    },
    [NEX_PIPE_BWT]      = {
        NEX_PIPE_BWT, "bwt",
        pipe_bwt_compress, pipe_bwt_decompress, 3
    },
    [NEX_PIPE_BALANCED] = {
        NEX_PIPE_BALANCED, "balanced",
        pipe_balanced_compress, pipe_balanced_decompress, 2
    },
    [NEX_PIPE_FAST]     = {
        NEX_PIPE_FAST, "fast",
        pipe_fast_compress, pipe_fast_decompress, 1
    },
    [NEX_PIPE_EXEC]     = {
        NEX_PIPE_EXEC, "exec",
        pipe_exec_compress, pipe_exec_decompress, 3
    },
    [NEX_PIPE_STORE]    = {
        NEX_PIPE_STORE, "store", NULL, NULL, 0
    },
};

const nex_pipeline_t *nex_get_pipeline(nex_pipeline_id_t id) {
    if (id >= NEX_PIPE_COUNT) return NULL;
    return &pipelines[id];
}

/* ── Pipeline Execution ──────────────────────────────────────────── */

nex_status_t nex_pipeline_compress(nex_pipeline_id_t id,
                                    const uint8_t *in, size_t in_size,
                                    nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    /* Store pipeline: just copy */
    if (id == NEX_PIPE_STORE) {
        if (out->capacity < in_size) {
            uint8_t *new_data = (uint8_t *)realloc(out->data, in_size);
            if (!new_data) return NEX_ERR_NOMEM;
            out->data = new_data;
            out->capacity = in_size;
        }
        memcpy(out->data, in, in_size);
        out->size = in_size;
        return NEX_OK;
    }

    const nex_pipeline_t *pipe = nex_get_pipeline(id);
    if (!pipe || pipe->num_stages == 0) return NEX_ERR_PARAM;

    /* Execute stages in sequence, using double-buffered temps */
    nex_buffer_t buf_a = {0}, buf_b = {0};
    const uint8_t *cur_in = in;
    size_t cur_size = in_size;

    for (int i = 0; i < pipe->num_stages; i++) {
        nex_buffer_t *cur_out = (i % 2 == 0) ? &buf_a : &buf_b;

        /* Allocate output buffer */
        size_t est = cur_size + cur_size / 8 + 4096;
        nex_status_t st = nex_buffer_alloc(cur_out, est);
        if (st != NEX_OK) {
            nex_buffer_free(&buf_a);
            nex_buffer_free(&buf_b);
            return st;
        }

        /* Determine effective level for this stage */
        int stage_level = level;
        if (id == NEX_PIPE_FAST) stage_level = NEX_MIN(level, 4);
        else if (id == NEX_PIPE_MAX) stage_level = NEX_MAX(level, 7);

        st = pipe->compress_stages[i](cur_in, cur_size, cur_out, stage_level, dict, dict_size);
        if (st != NEX_OK) {
            nex_buffer_free(&buf_a);
            nex_buffer_free(&buf_b);
            return st;
        }

        cur_in = cur_out->data;
        cur_size = cur_out->size;
    }

    /* Copy final result to output */
    if (out->capacity < cur_size) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, cur_size);
        if (!new_data) {
            nex_buffer_free(&buf_a);
            nex_buffer_free(&buf_b);
            return NEX_ERR_NOMEM;
        }
        out->data = new_data;
        out->capacity = cur_size;
    }
    memcpy(out->data, cur_in, cur_size);
    out->size = cur_size;

    nex_buffer_free(&buf_a);
    nex_buffer_free(&buf_b);
    return NEX_OK;
}

nex_status_t nex_pipeline_decompress(nex_pipeline_id_t id,
                                      const uint8_t *in, size_t in_size,
                                      nex_buffer_t *out, int level, const uint8_t *dict, size_t dict_size) {
    /* Store pipeline: just copy */
    if (id == NEX_PIPE_STORE) {
        if (out->capacity < in_size) {
            uint8_t *new_data = (uint8_t *)realloc(out->data, in_size);
            if (!new_data) return NEX_ERR_NOMEM;
            out->data = new_data;
            out->capacity = in_size;
        }
        memcpy(out->data, in, in_size);
        out->size = in_size;
        return NEX_OK;
    }

    const nex_pipeline_t *pipe = nex_get_pipeline(id);
    if (!pipe || pipe->num_stages == 0) return NEX_ERR_PARAM;

    /* Execute decompress stages (they are already in reverse order) */
    nex_buffer_t buf_a = {0}, buf_b = {0};
    const uint8_t *cur_in = in;
    size_t cur_size = in_size;

    for (int i = 0; i < pipe->num_stages; i++) {
        nex_buffer_t *cur_out = (i % 2 == 0) ? &buf_a : &buf_b;

        /* Estimate decompressed size (generous) */
        size_t est = cur_size * 8 + 4096;
        nex_status_t st = nex_buffer_alloc(cur_out, est);
        if (st != NEX_OK) {
            nex_buffer_free(&buf_a);
            nex_buffer_free(&buf_b);
            return st;
        }

        st = pipe->decompress_stages[i](cur_in, cur_size, cur_out, level, dict, dict_size);
        if (st != NEX_OK) {
            nex_buffer_free(&buf_a);
            nex_buffer_free(&buf_b);
            return st;
        }

        cur_in = cur_out->data;
        cur_size = cur_out->size;
    }

    /* Copy to output */
    if (out->capacity < cur_size) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, cur_size);
        if (!new_data) {
            nex_buffer_free(&buf_a);
            nex_buffer_free(&buf_b);
            return NEX_ERR_NOMEM;
        }
        out->data = new_data;
        out->capacity = cur_size;
    }
    memcpy(out->data, cur_in, cur_size);
    out->size = cur_size;

    nex_buffer_free(&buf_a);
    nex_buffer_free(&buf_b);
    return NEX_OK;
}
