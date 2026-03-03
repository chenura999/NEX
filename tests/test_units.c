/*
 * NEX Compress — Unit Tests
 * Tests for individual modules
 */

#include "../include/nex_internal.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "  ✗ FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        printf("  ✓ PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)

/* ── XXHash Tests ────────────────────────────────────────────────── */
static void test_xxhash(void) {
    printf("\n=== XXHash Tests ===\n");

    /* Known test vectors */
    uint32_t h32 = nex_xxh32("", 0, 0);
    ASSERT(h32 == 0x02CC5D05, "XXH32 empty string");

    h32 = nex_xxh32("a", 1, 0);
    ASSERT(h32 != 0, "XXH32 single char non-zero");

    /* Determinism */
    uint8_t data[256];
    for (int i = 0; i < 256; i++) data[i] = (uint8_t)i;
    uint32_t h1 = nex_xxh32(data, 256, 0);
    uint32_t h2 = nex_xxh32(data, 256, 0);
    ASSERT(h1 == h2, "XXH32 deterministic");

    uint64_t h64_1 = nex_xxh64(data, 256, 0);
    uint64_t h64_2 = nex_xxh64(data, 256, 0);
    ASSERT(h64_1 == h64_2, "XXH64 deterministic");

    /* Different seeds produce different hashes */
    uint32_t h3 = nex_xxh32(data, 256, 1);
    ASSERT(h1 != h3, "XXH32 different seeds differ");
}

/* ── Analyzer Tests ──────────────────────────────────────────────── */
static void test_analyzer(void) {
    printf("\n=== Analyzer Tests ===\n");

    /* Text data */
    const char *text = "Hello, World! This is a test string with text content.";
    nex_profile_t profile;
    nex_analyze((const uint8_t *)text, strlen(text), &profile);
    ASSERT(profile.type == NEX_DATA_TEXT, "Text detection");
    ASSERT(profile.text_ratio > 0.9, "Text ratio > 90%");
    ASSERT(profile.is_compressible, "Text is compressible");

    /* Random-ish data (high entropy) */
    uint8_t random_data[1024];
    srand(42);
    for (int i = 0; i < 1024; i++) random_data[i] = (uint8_t)(rand() & 0xFF);
    nex_analyze(random_data, 1024, &profile);
    ASSERT(profile.entropy > 6.0, "Random data high entropy");

    /* Uniform data (low entropy) */
    uint8_t zeros[1024];
    memset(zeros, 0, 1024);
    nex_analyze(zeros, 1024, &profile);
    ASSERT(profile.entropy < 0.01, "All-zero data near-zero entropy");
    ASSERT(profile.is_compressible, "Zero data is compressible");

    /* Pipeline selection */
    nex_pipeline_id_t pipe;

    nex_analyze((const uint8_t *)text, strlen(text), &profile);
    pipe = nex_select_pipeline(&profile, 6);
    ASSERT(pipe == NEX_PIPE_BWT, "Text level 6 → bwt");

    pipe = nex_select_pipeline(&profile, 1);
    ASSERT(pipe == NEX_PIPE_FAST, "Level 1 → fast");
}

/* ── LZ Round-Trip Tests ─────────────────────────────────────────── */
static void test_lz(void) {
    printf("\n=== LZ Matching Tests ===\n");

    /* Simple repeated pattern */
    const char *pattern = "ABCABCABCABCABC";
    size_t len = strlen(pattern);

    nex_buffer_t compressed = {0}, decompressed = {0};
    nex_status_t st;

    st = nex_lz_compress((const uint8_t *)pattern, len, &compressed, 6, NULL, 0);
    ASSERT(st == NEX_OK, "LZ compress ok");

    st = nex_lz_decompress(compressed.data, compressed.size, &decompressed, 6, NULL, 0);
    ASSERT(st == NEX_OK, "LZ decompress ok");
    ASSERT(decompressed.size == len, "LZ size matches");
    ASSERT(memcmp(decompressed.data, pattern, len) == 0, "LZ data matches");

    nex_buffer_free(&compressed);
    nex_buffer_free(&decompressed);

    /* Larger data with varied patterns */
    uint8_t large[4096];
    for (int i = 0; i < 4096; i++) large[i] = (uint8_t)(i % 97);  /* prime mod */

    memset(&compressed, 0, sizeof(compressed));
    memset(&decompressed, 0, sizeof(decompressed));

    st = nex_lz_compress(large, 4096, &compressed, 6, NULL, 0);
    ASSERT(st == NEX_OK, "LZ compress 4KB ok");

    st = nex_lz_decompress(compressed.data, compressed.size, &decompressed, 6, NULL, 0);
    ASSERT(st == NEX_OK, "LZ decompress 4KB ok");
    ASSERT(decompressed.size == 4096, "LZ 4KB size matches");
    ASSERT(memcmp(decompressed.data, large, 4096) == 0, "LZ 4KB data matches");

    nex_buffer_free(&compressed);
    nex_buffer_free(&decompressed);
}

/* ── BWT Round-Trip Tests ────────────────────────────────────────── */
static void test_bwt(void) {
    printf("\n=== BWT Tests ===\n");

    const char *text = "banana";
    size_t len = strlen(text);

    nex_buffer_t transformed = {0}, restored = {0};
    nex_status_t st;

    st = nex_bwt_forward((const uint8_t *)text, len, &transformed, 6, NULL, 0);
    ASSERT(st == NEX_OK, "BWT forward ok");

    st = nex_bwt_inverse(transformed.data, transformed.size, &restored, 6, NULL, 0);
    ASSERT(st == NEX_OK, "BWT inverse ok");
    ASSERT(restored.size == len, "BWT size matches");
    ASSERT(memcmp(restored.data, text, len) == 0, "BWT round-trip matches");

    nex_buffer_free(&transformed);
    nex_buffer_free(&restored);

    /* Longer text */
    const char *longer = "mississippi is a state in the southern united states";
    len = strlen(longer);

    memset(&transformed, 0, sizeof(transformed));
    memset(&restored, 0, sizeof(restored));

    st = nex_bwt_forward((const uint8_t *)longer, len, &transformed, 6, NULL, 0);
    ASSERT(st == NEX_OK, "BWT forward longer ok");

    st = nex_bwt_inverse(transformed.data, transformed.size, &restored, 6, NULL, 0);
    ASSERT(st == NEX_OK, "BWT inverse longer ok");
    ASSERT(restored.size == len, "BWT longer size matches");
    ASSERT(memcmp(restored.data, longer, len) == 0, "BWT longer round-trip");

    nex_buffer_free(&transformed);
    nex_buffer_free(&restored);
}

/* ── Delta Encoding Tests ────────────────────────────────────────── */
static void test_delta(void) {
    printf("\n=== Delta Encoding Tests ===\n");

    uint8_t data[] = {10, 12, 14, 16, 18, 20, 15, 10};
    size_t len = sizeof(data);

    nex_buffer_t encoded = {0}, decoded = {0};

    nex_status_t st = nex_delta_encode(data, len, &encoded, 6, NULL, 0);
    ASSERT(st == NEX_OK, "Delta encode ok");

    st = nex_delta_decode(encoded.data, encoded.size, &decoded, 6, NULL, 0);
    ASSERT(st == NEX_OK, "Delta decode ok");
    ASSERT(decoded.size == len, "Delta size matches");
    ASSERT(memcmp(decoded.data, data, len) == 0, "Delta round-trip matches");

    nex_buffer_free(&encoded);
    nex_buffer_free(&decoded);
}

/* ── rANS Entropy Tests ──────────────────────────────────────────── */
static void test_rans(void) {
    printf("\n=== rANS Entropy Tests ===\n");

    /* Simple data */
    const char *text = "aaabbbccc";
    size_t len = strlen(text);

    nex_buffer_t compressed = {0}, decompressed = {0};

    nex_status_t st = nex_rans_compress((const uint8_t *)text, len,
                                         &compressed, 6, NULL, 0);
    ASSERT(st == NEX_OK, "rANS compress ok");

    st = nex_rans_decompress(compressed.data, compressed.size,
                              &decompressed, 6, NULL, 0);
    ASSERT(st == NEX_OK, "rANS decompress ok");
    ASSERT(decompressed.size == len, "rANS size matches");
    ASSERT(memcmp(decompressed.data, text, len) == 0, "rANS round-trip");

    nex_buffer_free(&compressed);
    nex_buffer_free(&decompressed);

    /* Larger varied data */
    uint8_t data[2048];
    for (int i = 0; i < 2048; i++) data[i] = (uint8_t)(i % 128);

    memset(&compressed, 0, sizeof(compressed));
    memset(&decompressed, 0, sizeof(decompressed));

    st = nex_rans_compress(data, 2048, &compressed, 6, NULL, 0);
    ASSERT(st == NEX_OK, "rANS compress 2KB ok");

    st = nex_rans_decompress(compressed.data, compressed.size,
                              &decompressed, 6, NULL, 0);
    ASSERT(st == NEX_OK, "rANS decompress 2KB ok");
    ASSERT(decompressed.size == 2048, "rANS 2KB size matches");
    ASSERT(memcmp(decompressed.data, data, 2048) == 0, "rANS 2KB round-trip");

    nex_buffer_free(&compressed);
    nex_buffer_free(&decompressed);
}

/* ── Container Tests ─────────────────────────────────────────────── */
static void test_container(void) {
    printf("\n=== Container Tests ===\n");

    /* Write and read header */
    nex_header_t hdr = {
        .magic = NEX_MAGIC,
        .version = NEX_FORMAT_VER,
        .flags = 0,
        .original_size = 12345,
        .chunk_count = 3,
        .checksum = 0xDEADBEEFCAFEULL,
    };

    nex_buffer_t buf = {0};
    nex_status_t st = nex_write_header(&buf, &hdr);
    ASSERT(st == NEX_OK, "Write header ok");
    ASSERT(buf.size == 30, "Header is 30 bytes");

    nex_header_t hdr2;
    st = nex_read_header(buf.data, buf.size, &hdr2);
    ASSERT(st == NEX_OK, "Read header ok");
    ASSERT(hdr2.version == hdr.version, "Version matches");
    ASSERT(hdr2.original_size == hdr.original_size, "Size matches");
    ASSERT(hdr2.chunk_count == hdr.chunk_count, "Chunk count matches");
    ASSERT(hdr2.checksum == hdr.checksum, "Checksum matches");

    nex_buffer_free(&buf);

    /* Chunk table */
    nex_chunk_entry_t entries[2] = {
        { .compressed_offset = 100, .compressed_size = 50,
          .original_size = 1024, .pipeline_id = NEX_PIPE_BALANCED,
          .checksum = 0x12345678 },
        { .compressed_offset = 150, .compressed_size = 60,
          .original_size = 2048, .pipeline_id = NEX_PIPE_FAST,
          .checksum = 0xABCDEF00 },
    };

    memset(&buf, 0, sizeof(buf));
    st = nex_write_chunk_table(&buf, entries, 2);
    ASSERT(st == NEX_OK, "Write chunk table ok");
    ASSERT(buf.size == 42, "Chunk table is 42 bytes (2 × 21)");

    nex_chunk_entry_t *read_entries = NULL;
    st = nex_read_chunk_table(buf.data, buf.size, &read_entries, 2);
    ASSERT(st == NEX_OK, "Read chunk table ok");
    ASSERT(read_entries[0].compressed_size == 50, "Entry 0 size matches");
    ASSERT(read_entries[1].pipeline_id == NEX_PIPE_FAST, "Entry 1 pipeline matches");

    free(read_entries);
    nex_buffer_free(&buf);
}

/* ── Pipeline Round-Trip Tests ───────────────────────────────────── */
static void test_pipelines(void) {
    printf("\n=== Pipeline Tests ===\n");

    /* Test data: repeated text */
    const char *text = "The quick brown fox jumps over the lazy dog. ";
    size_t repeat = 100;
    size_t len = strlen(text) * repeat;
    uint8_t *data = (uint8_t *)malloc(len);
    for (size_t i = 0; i < repeat; i++) {
        memcpy(data + i * strlen(text), text, strlen(text));
    }

    nex_pipeline_id_t pipes[] = {
        NEX_PIPE_BALANCED, NEX_PIPE_FAST, NEX_PIPE_MAX, NEX_PIPE_STORE
    };
    const char *names[] = {"balanced", "fast", "max", "store"};

    for (int p = 0; p < 4; p++) {
        nex_buffer_t comp = {0}, decomp = {0};

        nex_status_t st = nex_pipeline_compress(pipes[p], data, len,
                                                 &comp, 6, NULL, 0);
        char msg[128];
        snprintf(msg, sizeof(msg), "Pipeline %s compress ok", names[p]);
        ASSERT(st == NEX_OK, msg);

        st = nex_pipeline_decompress(pipes[p], comp.data, comp.size,
                                      &decomp, 6, NULL, 0);
        snprintf(msg, sizeof(msg), "Pipeline %s decompress ok", names[p]);
        ASSERT(st == NEX_OK, msg);

        snprintf(msg, sizeof(msg), "Pipeline %s round-trip", names[p]);
        ASSERT(decomp.size == len && memcmp(decomp.data, data, len) == 0, msg);

        nex_buffer_free(&comp);
        nex_buffer_free(&decomp);
    }

    free(data);
}

/* ── Full Compress/Decompress API Test ───────────────────────────── */
static void test_full_api(void) {
    printf("\n=== Full API Tests ===\n");

    const char *text = "NEX Compress full API test. Repeated data for compression. ";
    size_t repeat = 500;
    size_t len = strlen(text) * repeat;
    uint8_t *data = (uint8_t *)malloc(len);
    for (size_t i = 0; i < repeat; i++) {
        memcpy(data + i * strlen(text), text, strlen(text));
    }

    nex_config_t cfg;
    nex_config_init(&cfg);
    cfg.threads = 1; /* single-threaded for determinism */

    nex_buffer_t comp = {0};
    nex_stats_t stats = {0};

    nex_status_t st = nex_compress(data, len, &comp, &cfg, &stats);
    ASSERT(st == NEX_OK, "Full compress ok");
    ASSERT(stats.original_size == len, "Stats original size");
    ASSERT(stats.compressed_size == comp.size, "Stats compressed size");
    ASSERT(stats.ratio > 0 && stats.ratio <= 1.2, "Stats ratio reasonable");

    /* Decompress */
    nex_buffer_t decomp = {0};
    nex_stats_t dstats = {0};

    st = nex_decompress(comp.data, comp.size, &decomp, &cfg, &dstats);
    ASSERT(st == NEX_OK, "Full decompress ok");
    ASSERT(decomp.size == len, "Decompressed size matches");
    ASSERT(memcmp(decomp.data, data, len) == 0, "Decompressed data matches");

    nex_buffer_free(&comp);
    nex_buffer_free(&decomp);
    free(data);
}

/* ── Memory Tests ────────────────────────────────────────────────── */
static void test_memory(void) {
    printf("\n=== Memory Tests ===\n");

    nex_arena_t arena;
    nex_arena_init(&arena, 1024);

    void *p1 = nex_arena_alloc(&arena, 100);
    ASSERT(p1 != NULL, "Arena alloc 100 bytes");

    void *p2 = nex_arena_alloc(&arena, 200);
    ASSERT(p2 != NULL, "Arena alloc 200 bytes");

    void *p3 = nex_arena_alloc(&arena, 2000); /* exceeds block, new block */
    ASSERT(p3 != NULL, "Arena alloc 2000 bytes (new block)");

    nex_arena_destroy(&arena);

    size_t avail = nex_available_memory();
    ASSERT(avail > 0, "Available memory > 0");

    printf("  Available memory: %zu MB\n", avail / (1024 * 1024));
}

/* ── Main ────────────────────────────────────────────────────────── */
int main(void) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║        NEX Compress — Unit Test Suite        ║\n");
    printf("╚══════════════════════════════════════════════╝\n");

    test_xxhash();
    test_analyzer();
    test_memory();
    test_lz();
    test_bwt();
    test_delta();
    test_rans();
    test_container();
    test_pipelines();
    test_full_api();

    printf("\n══════════════════════════════════════════════\n");
    printf("  Results: %d/%d passed\n", tests_passed, tests_run);
    printf("══════════════════════════════════════════════\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
