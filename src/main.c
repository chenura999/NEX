/*
 * NEX Compress — CLI Entry Point
 * Command-line interface for compress/decompress/benchmark
 */

#include "nex_internal.h"
#include <getopt.h>
#include <time.h>
#include <unistd.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
        "NEX Compress v%s — Next-Generation Hybrid Compression System\n\n"
        "Usage: %s [options] <input> [-o output]\n\n"
        "Options:\n"
        "  -c, --compress         Compress (default)\n"
        "  -d, --decompress       Decompress\n"
        "  -l, --level 1-9        Compression level (default: %d)\n"
        "  -t, --threads N        Thread count (0 = auto, default: auto)\n"
        "  -p, --pipeline NAME    Force pipeline: max|bwt|balanced|fast|store\n"
        "  -b, --benchmark        Benchmark mode\n"
        "  -D, --dict FILE        Use dictionary file for compression/decompression\n"
        "  -v, --verbose          Verbose output\n"
        "  -h, --help             Show this help\n\n"
        "Pipelines:\n"
        "  max       LZ optimal parse + rANS    (best ratio)\n"
        "  bwt       BWT + MTF + RLE + rANS     (text data)\n"
        "  balanced  LZ lazy match + rANS       (general purpose)\n"
        "  fast      LZ4-style single-pass     (speed priority)\n"
        "  store     Raw copy                   (incompressible)\n\n"
        "Examples:\n"
        "  %s -c file.txt -o file.nex         Compress\n"
        "  %s -d file.nex -o file.txt         Decompress\n"
        "  %s -b file.txt                     Benchmark\n"
        "  %s -l 9 -p max file.txt -o out.nex Max compression\n"
        "  %s -l 1 -p fast file.txt -o out.nex Fast compression\n",
        NEX_VERSION_STRING, prog, NEX_LEVEL_DEFAULT,
        prog, prog, prog, prog, prog
    );
}

static nex_pipeline_id_t parse_pipeline(const char *name) {
    if (strcmp(name, "max") == 0)      return NEX_PIPE_MAX;
    if (strcmp(name, "bwt") == 0)      return NEX_PIPE_BWT;
    if (strcmp(name, "balanced") == 0) return NEX_PIPE_BALANCED;
    if (strcmp(name, "fast") == 0)     return NEX_PIPE_FAST;
    if (strcmp(name, "store") == 0)    return NEX_PIPE_STORE;
    if (strcmp(name, "auto") == 0)     return NEX_PIPE_AUTO;
    return NEX_PIPE_AUTO;
}

static void print_stats(const nex_stats_t *s, bool is_compress) {
    printf("  Original:     %'lu bytes (%.2f MB)\n",
           s->original_size, s->original_size / (1024.0 * 1024.0));
    printf("  Compressed:   %'lu bytes (%.2f MB)\n",
           s->compressed_size, s->compressed_size / (1024.0 * 1024.0));
    printf("  Ratio:        %.4f (%.1f%%)\n",
           s->ratio, s->ratio * 100.0);

    if (is_compress) {
        printf("  Compress:     %.2f ms (%.2f MB/s)\n",
               s->compress_time_ms, s->compress_speed_mbs);
    } else {
        printf("  Decompress:   %.2f ms (%.2f MB/s)\n",
               s->decompress_time_ms, s->decompress_speed_mbs);
    }
}

static void run_benchmark(const char *input_path, const nex_config_t *cfg) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║         NEX Compress Benchmark — v%s               ║\n",
           NEX_VERSION_STRING);
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
    printf("  Input: %s\n\n", input_path);

    /* Read file */
    FILE *f = fopen(input_path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", input_path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(f);
        fprintf(stderr, "Error: empty or unreadable file\n");
        return;
    }

    uint8_t *data = (uint8_t *)malloc((size_t)fsize);
    if (!data) { fclose(f); fprintf(stderr, "Error: out of memory\n"); return; }
    size_t data_size = fread(data, 1, (size_t)fsize, f);
    fclose(f);

    /* Analyze input */
    nex_profile_t profile;
    nex_analyze(data, data_size, &profile);

    const char *type_names[] = {
        "Unknown", "Text", "Binary", "Executable",
        "Structured", "Compressed", "Media"
    };
    printf("  Data type:    %s\n", type_names[profile.type]);
    printf("  Entropy:      %.3f bits/byte\n", profile.entropy);
    printf("  Text ratio:   %.1f%%\n", profile.text_ratio * 100.0);
    printf("  Compressible: %s\n\n", profile.is_compressible ? "Yes" : "No");

    /* Test each pipeline */
    printf("┌────────────┬────────┬────────────┬────────────┬────────────┐\n");
    printf("│ Pipeline   │ Ratio  │ Comp MB/s  │ Decomp MB/s│ Size       │\n");
    printf("├────────────┼────────┼────────────┼────────────┼────────────┤\n");

    nex_pipeline_id_t pipes[] = {
        NEX_PIPE_MAX, NEX_PIPE_BWT, NEX_PIPE_BALANCED,
        NEX_PIPE_FAST, NEX_PIPE_STORE
    };
    const char *pipe_names[] = {
        "max", "bwt", "balanced", "fast", "store"
    };

    for (int i = 0; i < 5; i++) {
        nex_config_t bench_cfg = *cfg;
        bench_cfg.pipeline = pipes[i];

        nex_buffer_t compressed = {0};
        nex_stats_t comp_stats = {0}, decomp_stats = {0};

        nex_status_t st = nex_compress(data, data_size, &compressed,
                                        &bench_cfg, &comp_stats);
        if (st != NEX_OK) {
            printf("│ %-10s │ FAIL   │ %-10s │ %-10s │ %-10s │\n",
                   pipe_names[i], nex_strerror(st), "-", "-");
            nex_buffer_free(&compressed);
            continue;
        }

        /* Decompress */
        nex_buffer_t decompressed = {0};
        st = nex_decompress(compressed.data, compressed.size,
                             &decompressed, cfg, &decomp_stats);

        if (st == NEX_OK) {
            /* Verify */
            bool match = (decompressed.size == data_size &&
                          memcmp(decompressed.data, data, data_size) == 0);

            printf("│ %-10s │ %5.1f%% │ %8.1f   │ %8.1f    │ %8lu B  │%s\n",
                   pipe_names[i],
                   comp_stats.ratio * 100.0,
                   comp_stats.compress_speed_mbs,
                   decomp_stats.decompress_speed_mbs,
                   comp_stats.compressed_size,
                   match ? "" : " ✗ MISMATCH!");
        } else {
            printf("│ %-10s │ %5.1f%% │ %8.1f   │ FAIL       │ %8lu B  │\n",
                   pipe_names[i],
                   comp_stats.ratio * 100.0,
                   comp_stats.compress_speed_mbs,
                   comp_stats.compressed_size);
        }

        nex_buffer_free(&compressed);
        nex_buffer_free(&decompressed);
    }

    printf("└────────────┴────────┴────────────┴────────────┴────────────┘\n");

    /* Auto-selected pipeline info */
    nex_pipeline_id_t auto_pipe = nex_select_pipeline(&profile, cfg->level);
    const char *auto_name = "unknown";
    for (int i = 0; i < 5; i++) {
        if (pipes[i] == auto_pipe) {
            auto_name = pipe_names[i];
            break;
        }
    }
    printf("\n  Auto-selected pipeline: %s (level %d)\n\n",
           auto_name, cfg->level);

    free(data);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* Parse options */
    nex_config_t cfg;
    nex_config_init(&cfg);

    bool decompress_mode = false;
    bool benchmark_mode = false;
    const char *output_path = NULL;
    const char *input_path = NULL;
    const char *dict_path = NULL;

    static struct option long_options[] = {
        {"compress",   no_argument,       0, 'c'},
        {"decompress", no_argument,       0, 'd'},
        {"level",      required_argument, 0, 'l'},
        {"threads",    required_argument, 0, 't'},
        {"pipeline",   required_argument, 0, 'p'},
        {"benchmark",  no_argument,       0, 'b'},
        {"output",     required_argument, 0, 'o'},
        {"dict",       required_argument, 0, 'D'},
        {"verbose",    no_argument,       0, 'v'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "cdl:t:p:bo:D:vh",
                               long_options, NULL)) != -1) {
        switch (opt) {
            case 'c': decompress_mode = false; break;
            case 'd': decompress_mode = true; break;
            case 'l':
                cfg.level = atoi(optarg);
                if (cfg.level < NEX_LEVEL_MIN) cfg.level = NEX_LEVEL_MIN;
                if (cfg.level > NEX_LEVEL_MAX) cfg.level = NEX_LEVEL_MAX;
                break;
            case 't': cfg.threads = atoi(optarg); break;
            case 'p': cfg.pipeline = parse_pipeline(optarg); break;
            case 'b': benchmark_mode = true; break;
            case 'o': output_path = optarg; break;
            case 'D': dict_path = optarg; break;
            case 'v': cfg.verbose = true; break;
            case 'h': print_usage(argv[0]); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }

    if (optind < argc) {
        input_path = argv[optind];
    }

    if (!input_path) {
        fprintf(stderr, "Error: no input file specified\n");
        print_usage(argv[0]);
        return 1;
    }

    if (dict_path) {
        FILE *df = fopen(dict_path, "rb");
        if (!df) {
            fprintf(stderr, "Error: cannot open dictionary file '%s'\n", dict_path);
            return 1;
        }
        fseek(df, 0, SEEK_END);
        long dsize = ftell(df);
        fseek(df, 0, SEEK_SET);
        if (dsize > 0) {
            uint8_t *dbuf = (uint8_t *)malloc(dsize);
            if (dbuf && fread(dbuf, 1, dsize, df) == (size_t)dsize) {
                cfg.dict_data = dbuf;
                cfg.dict_size = dsize;
            } else {
                free(dbuf);
                fprintf(stderr, "Error: failed to read dictionary\n");
                fclose(df);
                return 1;
            }
        }
        fclose(df);
    }

    /* Benchmark mode */
    if (benchmark_mode) {
        run_benchmark(input_path, &cfg);
        if (cfg.dict_data) free((void *)cfg.dict_data);
        return 0;
    }

    /* Generate output path if not specified */
    char auto_output[4096];
    if (!output_path) {
        if (decompress_mode) {
            /* Remove .nex extension */
            size_t len = strlen(input_path);
            if (len > 4 && strcmp(input_path + len - 4, ".nex") == 0) {
                snprintf(auto_output, sizeof(auto_output), "%.*s",
                        (int)(len - 4), input_path);
            } else {
                snprintf(auto_output, sizeof(auto_output), "%s.out",
                        input_path);
            }
        } else {
            snprintf(auto_output, sizeof(auto_output), "%s.nex", input_path);
        }
        output_path = auto_output;
    }

    nex_stats_t stats = {0};
    nex_status_t st;

    if (decompress_mode) {
        if (cfg.verbose) {
            printf("Decompressing: %s → %s\n", input_path, output_path);
        }
        st = nex_decompress_file(input_path, output_path, &cfg, &stats);

        if (st == NEX_OK) {
            if (cfg.verbose) print_stats(&stats, false);
            printf("Decompressed: %s → %s (%lu bytes)\n",
                   input_path, output_path, stats.original_size);
        }
    } else {
        if (cfg.verbose) {
            printf("Compressing: %s → %s (level %d)\n",
                   input_path, output_path, cfg.level);
            if (cfg.pipeline != NEX_PIPE_AUTO) {
                printf("Pipeline: %s\n",
                       nex_get_pipeline(cfg.pipeline)->name);
            }
        }
        st = nex_compress_file(input_path, output_path, &cfg, &stats);

        if (st == NEX_OK) {
            if (cfg.verbose) print_stats(&stats, true);
            printf("Compressed: %s → %s (%.1f%%, %.1f MB/s)\n",
                   input_path, output_path,
                   stats.ratio * 100.0, stats.compress_speed_mbs);
        }
    }

    if (st != NEX_OK) {
        fprintf(stderr, "Error: %s\n", nex_strerror(st));
        if (cfg.dict_data) free((void *)cfg.dict_data);
        return 1;
    }

    if (cfg.dict_data) free((void *)cfg.dict_data);
    return 0;
}
