#!/usr/bin/env python3
# NEX Compress — Comparative Benchmark vs XZ and Zstd
# Optimized for reliability and accurate timing

import subprocess
import os
import tempfile
import time
import json
import struct

def create_corpora(tmpdir):
    files = {}

    # 1. English text
    text = "The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs. How vexingly quick daft zebras jump! The five boxing wizards jump quickly. Sphinx of black quartz, judge my vow. " * 500
    path = os.path.join(tmpdir, "english_text.txt")
    with open(path, "w") as f: f.write(text)
    files["English Text"] = path

    # 2. Source code
    code = """#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int id;
    char name[64];
    double value;
} Node;

void process(Node *n) {
    if (!n) return;
    printf("%d %s %f\\n", n->id, n->name, n->value);
}
""" * 300
    path = os.path.join(tmpdir, "source_code.c")
    with open(path, "w") as f: f.write(code)
    files["C Source Code"] = path

    # 3. JSON data
    records = [{"id": i, "name": f"User {i}", "email": f"user{i}@example.com", "score": i * 3.14, "tags": ["a", "b", "c"]} for i in range(500)]
    path = os.path.join(tmpdir, "data.json")
    with open(path, "w") as f: json.dump(records, f, indent=2)
    files["JSON Data"] = path

    # 4. Binary with patterns
    data = b""
    for i in range(5000):
        data += struct.pack('<IHBd', i, i % 256, i % 128, i * 0.01)
    path = os.path.join(tmpdir, "binary.bin")
    with open(path, "wb") as f: f.write(data)
    files["Binary Patterns"] = path

    # 5. Random (incompressible)
    path = os.path.join(tmpdir, "random.bin")
    subprocess.run(["dd", "if=/dev/urandom", f"of={path}", "bs=1024", "count=100"], capture_output=True)
    files["Random Data"] = path

    # 6. Log file
    lines = []
    for i in range(2000):
        ts = f"2024-01-{(i%28)+1:02d} {(i%24):02d}:{(i%60):02d}:{(i%60):02d}"
        levels = ["INFO", "WARN", "ERROR", "DEBUG"]
        lines.append(f"[{ts}] [{levels[i%4]}] Request #{i} from 192.168.{i%256}.{(i*7)%256} path=/api/v1/res/{i%100} status={200 if i%5!=0 else 500} lat={i%999}ms")
    path = os.path.join(tmpdir, "server.log")
    with open(path, "w") as f: f.write("\n".join(lines))
    files["Server Log"] = path

    return files

def bench_file(name, path, nexc, tmpdir):
    orig_size = os.path.getsize(path)
    print(f"┌──────────────────────────────────────────────────────────────────┐")
    print(f"│ {name:<64} │")
    print(f"│ {orig_size:,} bytes {' ':<50} │")
    print(f"├──────────┬──────────┬──────────┬──────────┬──────────────────────┤")
    print(f"│ {'Tool':<8} │ {'Ratio':>8} │ {'Comp s':>8} │ {'Dec s':>8} │ {'Comp Size':>20} │")
    print(f"├──────────┼──────────┼──────────┼──────────┼──────────────────────┤")

    results = []

    # NEX
    out_nex = os.path.join(tmpdir, "out.nex")
    out_dec = os.path.join(tmpdir, "out.dec")
    
    t0 = time.monotonic()
    subprocess.run([nexc, "-c", path, "-o", out_nex], capture_output=True)
    t1 = time.monotonic()
    subprocess.run([nexc, "-d", out_nex, "-o", out_dec], capture_output=True)
    t2 = time.monotonic()
    
    nex_size = os.path.getsize(out_nex)
    nex_ok = subprocess.run(["diff", path, out_dec], capture_output=True).returncode == 0
    results.append(("NEX", nex_size, t1-t0, t2-t1, nex_ok))

    # XZ
    out_xz = os.path.join(tmpdir, "out.xz")
    t0 = time.monotonic()
    subprocess.run(f"xz -k -f -c '{path}' > {out_xz}", shell=True, capture_output=True)
    t1 = time.monotonic()
    subprocess.run(f"xz -d -c '{out_xz}' > {out_dec}", shell=True, capture_output=True)
    t2 = time.monotonic()
    results.append(("XZ", os.path.getsize(out_xz), t1-t0, t2-t1, True))

    # Zstd
    out_zst = os.path.join(tmpdir, "out.zst")
    t0 = time.monotonic()
    subprocess.run(["zstd", "-f", path, "-o", out_zst], capture_output=True)
    t1 = time.monotonic()
    subprocess.run(["zstd", "-d", "-f", out_zst, "-o", out_dec], capture_output=True)
    t2 = time.monotonic()
    results.append(("Zstd", os.path.getsize(out_zst), t1-t0, t2-t1, True))

    for tool, csz, tc, td, ok in results:
        ratio = (csz / orig_size) * 100
        ok_str = "✓" if ok else "✗"
        print(f"│ {tool:<8} │ {ratio:>7.1f}% │ {tc:>7.3f}s │ {td:>7.3f}s │ {csz:>20,} │")

    print(f"└──────────┴──────────┴──────────┴──────────┴──────────────────────┘")
    print()

def main():
    print("╔══════════════════════════════════════════════════════════════════╗")
    print("║        NEX Compress — Comparative Benchmark vs XZ and Zstd         ║")
    print("╚══════════════════════════════════════════════════════════════════╝")
    print()

    nexc = "./nexc"
    if not os.path.exists(nexc):
        print("Error: ./nexc not found. Build the project first.")
        return

    with tempfile.TemporaryDirectory() as tmpdir:
        print("Creating corpora...")
        files = create_corpora(tmpdir)
        print("Done.\n")
        
        for name, path in files.items():
            bench_file(name, path, nexc, tmpdir)

if __name__ == "__main__":
    main()
