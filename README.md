<p align="center">
  <h1 align="center">⚡ NEX Compress</h1>
  <p align="center">
    <b>Next-Generation Hybrid Lossless Compression System</b>
    <br/>
    <i>Adaptive multi-pipeline architecture · BT4 + Viterbi optimal parsing · rANS entropy coding · Multi-threaded</i>
    <br/><br/>
    <a href="#quick-start"><img src="https://img.shields.io/badge/version-1.0.0-blue?style=flat-square" alt="Version"/></a>
    <a href="NEX/LICENSE"><img src="https://img.shields.io/badge/license-MIT-green?style=flat-square" alt="License"/></a>
    <a href="#building"><img src="https://img.shields.io/badge/language-C11-orange?style=flat-square" alt="Language"/></a>
    <a href="#benchmarks"><img src="https://img.shields.io/badge/status-experimental-yellow?style=flat-square" alt="Status"/></a>
  </p>
</p>

---

NEX is a lossless compression system written in pure C11 with **zero external dependencies** (only libc, pthreads, libm). It features an adaptive pipeline engine that automatically selects the optimal compression strategy based on real-time data analysis — combining LZ dictionary coding, BWT transforms, rANS/Huffman entropy coding, and BCJ filters into specialized pipelines for different data types.

> **⚠️ Experimental** — NEX is a research/learning project exploring modern compression techniques. Not yet production-hardened.

## ✨ Key Features

| Feature | Description |
|---|---|
| **Adaptive Pipeline Engine** | Auto-selects from 6 compression pipelines based on Shannon entropy, data type signatures, and text ratio analysis |
| **BT4 + Viterbi Optimal Parser** | Binary tree match finder with cost-model-driven optimal parsing at high levels, hash chain + lazy matching at lower levels |
| **Dual Entropy Coders** | 32-bit rANS (Asymmetric Numeral Systems) with O(1) table-driven decoding, plus canonical Huffman as a fast fallback |
| **BWT Pipeline** | Burrows-Wheeler Transform via prefix-doubling suffix array O(n log²n), combined with Move-to-Front + RLE |
| **BCJ Filter** | x86 CALL/JMP relative→absolute address conversion for improved executable compression |
| **Multi-threaded** | pthread-based thread pool with work queue; chunks processed in parallel (auto-detects CPU cores) |
| **AVX2/SSE2 SIMD** | Vectorized match extension using 32-byte AVX2 comparisons with automatic scalar fallback |
| **Dictionary Pre-training** | Optional external dictionary support for improved compression of small or domain-specific data |
| **Container Format** | Chunk-based `.nex` format with per-chunk XXH32 + full-file XXH64 integrity verification |
| **Micro-Container** | Optimized 12-byte header for single-chunk files (sub-1KB overhead) |
| **Arena Allocator** | Custom memory management with block-based arena allocation and 16-byte alignment |

## 🏗️ Architecture

```
                         ┌──────────────────┐
                         │   Input Data      │
                         └────────┬─────────┘
                                  │
                         ┌────────▼─────────┐
                         │    Analyzer       │  ← Shannon entropy, byte histogram,
                         │  (analyzer.c)     │    signature detection, text ratio
                         └────────┬─────────┘
                                  │ nex_profile_t
                         ┌────────▼─────────┐
                         │ Pipeline Selector │  ← Chooses optimal pipeline
                         │  (pipeline.c)     │    based on profile + level
                         └────────┬─────────┘
                                  │
              ┌───────────────────┼───────────────────┐
              │                   │                   │
     ┌────────▼──────┐  ┌────────▼──────┐  ┌────────▼──────┐
     │  MAX Pipeline  │  │ BWT Pipeline  │  │ FAST Pipeline │  ...
     │ LZ opt → rANS  │  │BWT→MTF+RLE→  │  │LZ greedy →   │
     │                │  │     rANS      │  │   Huffman     │
     └────────┬──────┘  └────────┬──────┘  └────────┬──────┘
              │                   │                   │
              └───────────────────┼───────────────────┘
                                  │
                         ┌────────▼─────────┐
                         │   Container       │  ← Header + chunk table +
                         │  (container.c)    │    compressed data + footer
                         └────────┬─────────┘
                                  │
                         ┌────────▼─────────┐
                         │   .nex File       │
                         └──────────────────┘
```

### Compression Pipelines

| Pipeline | Stages | Best For | Level |
|---|---|---|---|
| **MAX** | LZ optimal parse → rANS | Best ratio (general) | 8-9 |
| **BWT** | BWT → MTF+RLE → rANS | Sorted/text data | 3-7 |
| **BALANCED** | LZ lazy match → rANS | General purpose | 3-7 (default) |
| **FAST** | LZ greedy → Huffman | Speed priority | 1-2 |
| **EXEC** | BCJ x86 → LZ optimal → rANS | ELF/PE executables | Auto |
| **STORE** | Raw copy | Incompressible data | Auto |

### Match Finding Strategy (by level)

| Level | Match Finder | Hash Bits | Chain Length | Parser |
|---|---|---|---|---|
| 1-3 | Hash Chain | 16 | 16 | Greedy |
| 4-5 | Hash Chain | 16 | 64 | Lazy |
| 6-7 | BT4 Binary Tree | 20 | 128 | Viterbi Optimal |
| 8-9 | BT4 Binary Tree | 20 | 4096 | Viterbi Optimal |

## 🚀 Quick Start

### Building

```bash
# Standard optimized build
make

# Debug build (ASan + UBSan)
make DEBUG=1

# Clean
make clean
```

This produces:
- `nexc` — CLI compression tool
- `libnex.so` — Shared library for embedding

### Compressing & Decompressing

```bash
# Compress (auto-selects pipeline)
./nexc -c file.txt -o file.nex

# Decompress
./nexc -d file.nex -o file.txt

# Max compression (level 9, MAX pipeline)
./nexc -l 9 -p max file.txt -o file.nex

# Fast compression (level 1)
./nexc -l 1 -p fast file.txt -o file.nex

# Multi-threaded with 8 threads
./nexc -t 8 -c largefile.bin -o largefile.nex

# Use a pre-trained dictionary
./nexc -D dictionary.bin -c data.json -o data.nex

# Benchmark all pipelines on a file
./nexc -b testfile.bin
```

### CLI Options

```
Usage: nexc [options] <input> [-o output]

  -c, --compress         Compress (default)
  -d, --decompress       Decompress
  -l, --level 1-9        Compression level (default: 6)
  -t, --threads N        Thread count (0 = auto)
  -p, --pipeline NAME    Force pipeline: max|bwt|balanced|fast|store
  -b, --benchmark        Benchmark all pipelines
  -D, --dict FILE        Use dictionary file
  -v, --verbose          Verbose output with statistics
  -h, --help             Show help
```

## 📚 Library API

NEX exposes a clean C API via `include/nex.h`:

```c
#include "nex.h"

// Initialize configuration with defaults
nex_config_t cfg;
nex_config_init(&cfg);
cfg.level = 7;
cfg.pipeline = NEX_PIPE_AUTO;  // or NEX_PIPE_MAX, NEX_PIPE_BWT, etc.

// In-memory compression
nex_buffer_t output = {0};
nex_stats_t stats;
nex_status_t st = nex_compress(input_data, input_size, &output, &cfg, &stats);

if (st == NEX_OK) {
    printf("Ratio: %.1f%%, Speed: %.1f MB/s\n",
           stats.ratio * 100.0, stats.compress_speed_mbs);
}

// In-memory decompression
nex_buffer_t decompressed = {0};
st = nex_decompress(output.data, output.size, &decompressed, &cfg, NULL);

// File-based API
st = nex_compress_file("input.txt", "output.nex", &cfg, &stats);
st = nex_decompress_file("output.nex", "restored.txt", &cfg, NULL);

// Cleanup
nex_buffer_free(&output);
nex_buffer_free(&decompressed);
```

## 📦 Container Format (`.nex`)

### Standard Format

```
┌─────────────────────────────────────────────┐
│ Header (30 bytes)                           │
│   Magic:    "NEX\x01" (4B)                  │
│   Version:  uint16      (2B)                │
│   Flags:    uint16      (2B)                │
│   OrigSize: uint64      (8B)                │
│   Chunks:   uint32      (4B)                │
│   Checksum: uint64 XXH64 (8B)               │
│   Reserved:             (2B)                │
├─────────────────────────────────────────────┤
│ Chunk Table (N × 21 bytes)                  │
│   Per chunk:                                │
│     CompOffset: uint64  (8B)                │
│     CompSize:   uint32  (4B)                │
│     OrigSize:   uint32  (4B)                │
│     PipelineID: uint8   (1B)                │
│     Checksum:   uint32 XXH32 (4B)           │
├─────────────────────────────────────────────┤
│ Compressed Chunk Data [0..N]                │
├─────────────────────────────────────────────┤
│ Footer: "XEN\x01" (4B)                      │
└─────────────────────────────────────────────┘
```

### Micro-Container (single-chunk optimization)

For files that fit in a single chunk, NEX uses a compact 12-byte header:

```
Magic (4B) | Version (2B) | Flags+PipelineID (2B) | OrigSize (4B) | Data...
```

## 🧪 Testing

```bash
# Run unit tests (XXHash, Analyzer, LZ, BWT, Delta, rANS, Container, Pipelines, Full API)
make test

# Run comparative benchmark vs xz and zstd
python3 tests/benchmark.sh

# Run industrial benchmark (Linux headers corpus)
python3 tests/industrial_benchmark.py
```

The unit test suite covers **10 modules** with round-trip verification:

| Module | Tests |
|---|---|
| XXHash | Known test vectors, determinism, seed independence |
| Analyzer | Text/binary/random classification, entropy, pipeline selection |
| Memory | Arena allocation, cross-block growth, available memory |
| LZ Match | Repeated patterns, 4KB varied data round-trip |
| BWT | "banana", long text round-trip |
| Delta | Sequential, arbitrary data round-trip |
| rANS | Simple text, 2KB varied data round-trip |
| Container | Header read/write, chunk table serialization |
| Pipelines | balanced/fast/max/store round-trip on 4.5KB repeated text |
| Full API | End-to-end compress → decompress with checksum verification |

## 🔧 Source Tree

```
NEX/
├── include/
│   ├── nex.h              # Public API (129 lines)
│   └── nex_internal.h     # Internal types & module interfaces (233 lines)
├── src/
│   ├── main.c             # CLI entry point & benchmark runner
│   ├── analyzer.c         # Data profiling (entropy, signatures, classification)
│   ├── pipeline.c         # Pipeline registry & stage execution engine
│   ├── lz_match.c         # LZ matching: BT4, hash chain, Viterbi optimal parser
│   ├── entropy.c          # rANS encoder/decoder + Huffman codec
│   ├── transform.c        # BWT, MTF+RLE, Delta encoding, BCJ x86 filter
│   ├── container.c        # .nex file format reader/writer
│   ├── decompress.c       # Decompression driver, compress/decompress orchestration
│   ├── parallel.c         # pthread thread pool with work queue
│   ├── memory.c           # Arena allocator, aligned alloc, memory budget
│   └── xxhash.c           # XXH32/XXH64 hash implementation
├── tests/
│   ├── test_units.c       # Comprehensive unit test suite
│   ├── test_roundtrip.sh  # Shell-based round-trip tests
│   ├── test_corrupt.sh    # Corruption resilience tests
│   ├── benchmark.sh       # Comparative benchmark (NEX vs xz vs zstd)
│   └── industrial_benchmark.py  # Large-scale benchmark (Linux headers)
├── Makefile               # Build system (gcc, -O3, -march=native)
└── NEX/
    ├── LICENSE            # MIT License
    └── README.md          # Original stub
```

**Total: ~4,170 lines of C** (zero external dependencies)

## 🛡️ Build Hardening

The default build includes modern security hardening flags:

- `-fstack-protector-strong` — Stack canary protection
- `-D_FORTIFY_SOURCE=3` — Buffer overflow detection
- `-fPIE` / `-pie` — Position-independent executable (ASLR)
- `-Wl,-z,relro,-z,now` — Full RELRO (GOT protection)
- `-Wformat -Wformat-security -Werror=format-security` — Format string protection

## 📄 License

[MIT License](NEX/LICENSE) — Copyright (c) 2026 AloNeeXe
