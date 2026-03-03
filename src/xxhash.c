/*
 * NEX Compress — XXHash Implementation
 * Fast non-cryptographic hash for checksums
 * Based on xxHash by Yann Collet (BSD-2-Clause)
 */

#include "nex_internal.h"

/* ── XXH32 ───────────────────────────────────────────────────────── */

#define XXH_PRIME32_1  0x9E3779B1U
#define XXH_PRIME32_2  0x85EBCA77U
#define XXH_PRIME32_3  0xC2B2AE3DU
#define XXH_PRIME32_4  0x27D4EB2FU
#define XXH_PRIME32_5  0x165667B1U

static inline uint32_t xxh32_rotl(uint32_t x, int r) {
    return (x << r) | (x >> (32 - r));
}

static inline uint32_t xxh32_round(uint32_t acc, uint32_t input) {
    acc += input * XXH_PRIME32_2;
    acc = xxh32_rotl(acc, 13);
    acc *= XXH_PRIME32_1;
    return acc;
}

static inline uint32_t xxh32_read32(const void *ptr) {
    uint32_t val;
    memcpy(&val, ptr, 4);
    return val;
}

uint32_t nex_xxh32(const void *data, size_t len, uint32_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint32_t h32;

    if (len >= 16) {
        const uint8_t *limit = end - 16;
        uint32_t v1 = seed + XXH_PRIME32_1 + XXH_PRIME32_2;
        uint32_t v2 = seed + XXH_PRIME32_2;
        uint32_t v3 = seed;
        uint32_t v4 = seed - XXH_PRIME32_1;

        do {
            v1 = xxh32_round(v1, xxh32_read32(p));  p += 4;
            v2 = xxh32_round(v2, xxh32_read32(p));  p += 4;
            v3 = xxh32_round(v3, xxh32_read32(p));  p += 4;
            v4 = xxh32_round(v4, xxh32_read32(p));  p += 4;
        } while (p <= limit);

        h32 = xxh32_rotl(v1, 1) + xxh32_rotl(v2, 7) +
              xxh32_rotl(v3, 12) + xxh32_rotl(v4, 18);
    } else {
        h32 = seed + XXH_PRIME32_5;
    }

    h32 += (uint32_t)len;

    while (p + 4 <= end) {
        h32 += xxh32_read32(p) * XXH_PRIME32_3;
        h32 = xxh32_rotl(h32, 17) * XXH_PRIME32_4;
        p += 4;
    }

    while (p < end) {
        h32 += (*p) * XXH_PRIME32_5;
        h32 = xxh32_rotl(h32, 11) * XXH_PRIME32_1;
        p++;
    }

    h32 ^= h32 >> 15;
    h32 *= XXH_PRIME32_2;
    h32 ^= h32 >> 13;
    h32 *= XXH_PRIME32_3;
    h32 ^= h32 >> 16;

    return h32;
}

/* ── XXH64 ───────────────────────────────────────────────────────── */

#define XXH_PRIME64_1  0x9E3779B185EBCA87ULL
#define XXH_PRIME64_2  0xC2B2AE3D27D4EB4FULL
#define XXH_PRIME64_3  0x165667B19E3779F9ULL
#define XXH_PRIME64_4  0x85EBCA77C2B2AE63ULL
#define XXH_PRIME64_5  0x27D4EB2F165667C5ULL

static inline uint64_t xxh64_rotl(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t xxh64_round(uint64_t acc, uint64_t input) {
    acc += input * XXH_PRIME64_2;
    acc = xxh64_rotl(acc, 31);
    acc *= XXH_PRIME64_1;
    return acc;
}

static inline uint64_t xxh64_merge_round(uint64_t acc, uint64_t val) {
    val = xxh64_round(0, val);
    acc ^= val;
    acc = acc * XXH_PRIME64_1 + XXH_PRIME64_4;
    return acc;
}

static inline uint64_t xxh64_read64(const void *ptr) {
    uint64_t val;
    memcpy(&val, ptr, 8);
    return val;
}

uint64_t nex_xxh64(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint64_t h64;

    if (len >= 32) {
        const uint8_t *limit = end - 32;
        uint64_t v1 = seed + XXH_PRIME64_1 + XXH_PRIME64_2;
        uint64_t v2 = seed + XXH_PRIME64_2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - XXH_PRIME64_1;

        do {
            v1 = xxh64_round(v1, xxh64_read64(p));  p += 8;
            v2 = xxh64_round(v2, xxh64_read64(p));  p += 8;
            v3 = xxh64_round(v3, xxh64_read64(p));  p += 8;
            v4 = xxh64_round(v4, xxh64_read64(p));  p += 8;
        } while (p <= limit);

        h64 = xxh64_rotl(v1, 1) + xxh64_rotl(v2, 7) +
              xxh64_rotl(v3, 12) + xxh64_rotl(v4, 18);

        h64 = xxh64_merge_round(h64, v1);
        h64 = xxh64_merge_round(h64, v2);
        h64 = xxh64_merge_round(h64, v3);
        h64 = xxh64_merge_round(h64, v4);
    } else {
        h64 = seed + XXH_PRIME64_5;
    }

    h64 += (uint64_t)len;

    while (p + 8 <= end) {
        uint64_t k1 = xxh64_round(0, xxh64_read64(p));
        h64 ^= k1;
        h64 = xxh64_rotl(h64, 27) * XXH_PRIME64_1 + XXH_PRIME64_4;
        p += 8;
    }

    if (p + 4 <= end) {
        uint32_t v32;
        memcpy(&v32, p, 4);
        h64 ^= (uint64_t)v32 * XXH_PRIME64_1;
        h64 = xxh64_rotl(h64, 23) * XXH_PRIME64_2 + XXH_PRIME64_3;
        p += 4;
    }

    while (p < end) {
        h64 ^= (*p) * XXH_PRIME64_5;
        h64 = xxh64_rotl(h64, 11) * XXH_PRIME64_1;
        p++;
    }

    h64 ^= h64 >> 33;
    h64 *= XXH_PRIME64_2;
    h64 ^= h64 >> 29;
    h64 *= XXH_PRIME64_3;
    h64 ^= h64 >> 32;

    return h64;
}

/* ── XXH64 Streaming API ─────────────────────────────────────────── */

void nex_xxh64_init(nex_xxh64_state_t *state, uint64_t seed) {
    memset(state, 0, sizeof(*state));
    state->v1 = seed + XXH_PRIME64_1 + XXH_PRIME64_2;
    state->v2 = seed + XXH_PRIME64_2;
    state->v3 = seed;
    state->v4 = seed - XXH_PRIME64_1;
    state->seed = seed;
}

void nex_xxh64_update(nex_xxh64_state_t *state, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;

    state->total_len += len;

    if (state->memsize + len < 32) {
        memcpy(state->mem + state->memsize, p, len);
        state->memsize += (uint32_t)len;
        return;
    }

    if (state->memsize > 0) {
        size_t fill = 32 - state->memsize;
        memcpy(state->mem + state->memsize, p, fill);
        state->v1 = xxh64_round(state->v1, xxh64_read64(state->mem));
        state->v2 = xxh64_round(state->v2, xxh64_read64(state->mem + 8));
        state->v3 = xxh64_round(state->v3, xxh64_read64(state->mem + 16));
        state->v4 = xxh64_round(state->v4, xxh64_read64(state->mem + 24));
        p += fill;
        state->memsize = 0;
    }

    while (p + 32 <= end) {
        state->v1 = xxh64_round(state->v1, xxh64_read64(p));
        state->v2 = xxh64_round(state->v2, xxh64_read64(p + 8));
        state->v3 = xxh64_round(state->v3, xxh64_read64(p + 16));
        state->v4 = xxh64_round(state->v4, xxh64_read64(p + 24));
        p += 32;
    }

    if (p < end) {
        memcpy(state->mem, p, end - p);
        state->memsize = (uint32_t)(end - p);
    }
}

uint64_t nex_xxh64_digest(const nex_xxh64_state_t *state) {
    uint64_t h64;

    if (state->total_len >= 32) {
        h64 = xxh64_rotl(state->v1, 1) + xxh64_rotl(state->v2, 7) +
              xxh64_rotl(state->v3, 12) + xxh64_rotl(state->v4, 18);

        h64 = xxh64_merge_round(h64, state->v1);
        h64 = xxh64_merge_round(h64, state->v2);
        h64 = xxh64_merge_round(h64, state->v3);
        h64 = xxh64_merge_round(h64, state->v4);
    } else {
        h64 = state->seed + XXH_PRIME64_5;
    }

    h64 += state->total_len;

    const uint8_t *p = state->mem;
    const uint8_t *end = p + state->memsize;

    while (p + 8 <= end) {
        uint64_t k1 = xxh64_round(0, xxh64_read64(p));
        h64 ^= k1;
        h64 = xxh64_rotl(h64, 27) * XXH_PRIME64_1 + XXH_PRIME64_4;
        p += 8;
    }

    if (p + 4 <= end) {
        uint32_t v32;
        memcpy(&v32, p, 4);
        h64 ^= (uint64_t)v32 * XXH_PRIME64_1;
        h64 = xxh64_rotl(h64, 23) * XXH_PRIME64_2 + XXH_PRIME64_3;
        p += 4;
    }

    while (p < end) {
        h64 ^= (*p) * XXH_PRIME64_5;
        h64 = xxh64_rotl(h64, 11) * XXH_PRIME64_1;
        p++;
    }

    h64 ^= h64 >> 33;
    h64 *= XXH_PRIME64_2;
    h64 ^= h64 >> 29;
    h64 *= XXH_PRIME64_3;
    h64 ^= h64 >> 32;

    return h64;
}
