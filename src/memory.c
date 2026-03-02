/*
 * NEX Compress — Memory Management
 * Arena allocator, aligned alloc, memory budget
 */

#include "nex_internal.h"
#include <sys/sysinfo.h>

/* ── Arena Allocator ─────────────────────────────────────────────── */

void nex_arena_init(nex_arena_t *arena, size_t block_size) {
    arena->head = NULL;
    arena->default_block_size = block_size ? block_size : (256 * 1024);
}

static nex_arena_block_t *arena_new_block(size_t size) {
    nex_arena_block_t *blk = (nex_arena_block_t *)malloc(
        sizeof(nex_arena_block_t) + size);
    if (!blk) return NULL;
    blk->next = NULL;
    blk->size = size;
    blk->used = 0;
    return blk;
}

void *nex_arena_alloc(nex_arena_t *arena, size_t size) {
    /* Align to 16 bytes */
    size = NEX_ALIGN(size, 16);

    /* Try current head block */
    if (arena->head && (arena->head->size - arena->head->used) >= size) {
        void *ptr = arena->head->data + arena->head->used;
        arena->head->used += size;
        return ptr;
    }

    /* Need new block */
    size_t blk_size = NEX_MAX(arena->default_block_size, size);
    nex_arena_block_t *blk = arena_new_block(blk_size);
    if (!blk) return NULL;

    blk->next = arena->head;
    arena->head = blk;
    blk->used = size;
    return blk->data;
}

void nex_arena_reset(nex_arena_t *arena) {
    nex_arena_block_t *blk = arena->head;
    while (blk) {
        blk->used = 0;
        blk = blk->next;
    }
}

void nex_arena_destroy(nex_arena_t *arena) {
    nex_arena_block_t *blk = arena->head;
    while (blk) {
        nex_arena_block_t *next = blk->next;
        free(blk);
        blk = next;
    }
    arena->head = NULL;
}

/* ── Aligned Allocation ──────────────────────────────────────────── */

void *nex_aligned_alloc(size_t size, size_t alignment) {
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0) return NULL;
    return ptr;
}

void nex_aligned_free(void *ptr) {
    free(ptr);
}

/* ── Memory Budget ───────────────────────────────────────────────── */

size_t nex_available_memory(void) {
    struct sysinfo si;
    if (sysinfo(&si) != 0) return 256 * 1024 * 1024; /* fallback 256MB */
    uint64_t avail = (uint64_t)si.freeram * si.mem_unit;
    /* Use at most 75% of available memory */
    return (size_t)(avail * 3 / 4);
}
