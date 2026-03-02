#!/bin/bash
# NEX Compress — Comparative Benchmark vs XZ and Zstd
# Generates test corpora and compares ratio/speed

set -e
cd "$(dirname "$0")/.."

NEXC=./nexc
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║       NEX Compress — Comparative Benchmark                  ║"
echo "║       NEX vs XZ vs Zstd                                     ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# ── Create test corpora ───────────────────────────────────────
echo "Creating test corpora..."

# 1. English text (100KB repeated prose)
python3 -c "
text = '''The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs. How vexingly quick daft zebras jump! The five boxing wizards jump quickly. Sphinx of black quartz, judge my vow. '''
data = text * 200
with open('$TMPDIR/english_text.txt', 'w') as f:
    f.write(data)
print(f'  english_text.txt: {len(data)} bytes')
"

# 2. Source code (C header repeated)
python3 -c "
code = '''#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int id;
    char name[64];
    double value;
    struct Node *next;
} Node;

Node *create_node(int id, const char *name, double value) {
    Node *n = (Node *)malloc(sizeof(Node));
    if (!n) return NULL;
    n->id = id;
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->value = value;
    n->next = NULL;
    return n;
}

void free_list(Node *head) {
    while (head) {
        Node *tmp = head;
        head = head->next;
        free(tmp);
    }
}
'''
with open('$TMPDIR/source_code.c', 'w') as f:
    f.write(code * 150)
print(f'  source_code.c: {len(code * 150)} bytes')
"

# 3. JSON data
python3 -c "
import json
records = []
for i in range(500):
    records.append({
        'id': i,
        'name': f'User {i}',
        'email': f'user{i}@example.com',
        'score': i * 3.14,
        'active': i % 3 == 0,
        'tags': ['alpha', 'beta', 'gamma'] if i % 2 == 0 else ['delta']
    })
data = json.dumps(records, indent=2)
with open('$TMPDIR/data.json', 'w') as f:
    f.write(data)
print(f'  data.json: {len(data)} bytes')
"

# 4. Binary with patterns
python3 -c "
import struct, sys
data = b''
for i in range(5000):
    data += struct.pack('<IHBd', i, i % 256, i % 128, i * 0.01)
with open('$TMPDIR/binary.bin', 'wb') as f:
    f.write(data)
print(f'  binary.bin: {len(data)} bytes')
"

# 5. Random (incompressible)
dd if=/dev/urandom of="$TMPDIR/random.bin" bs=1024 count=100 2>/dev/null
echo "  random.bin: 102400 bytes"

# 6. Log file
python3 -c "
import datetime
lines = []
for i in range(2000):
    ts = f'2024-01-{(i%28)+1:02d} {(i%24):02d}:{(i%60):02d}:{(i%60):02d}'
    levels = ['INFO', 'WARN', 'ERROR', 'DEBUG']
    lines.append(f'[{ts}] [{levels[i%4]}] Request #{i} from 192.168.{i%256}.{(i*7)%256} path=/api/v1/resource/{i%100} status={200 if i%5!=0 else 500} latency={i%999}ms')
with open('$TMPDIR/server.log', 'w') as f:
    f.write('\n'.join(lines))
print(f'  server.log: {sum(len(l) for l in lines)} bytes')
"

echo ""

# ── Benchmark function ────────────────────────────────────────

bench_file() {
    local name="$1" file="$2"
    local orig_size=$(wc -c < "$file" | tr -d ' ')
    
    printf "┌─────────────────────────────────────────────────────────────┐\n"
    printf "│ %-59s │\n" "$name ($orig_size bytes)"
    printf "├──────────┬──────────┬──────────┬──────────┬────────────────┤\n"
    printf "│ %-8s │ %-8s │ %-8s │ %-8s │ %-14s │\n" \
           "Tool" "Ratio" "Comp s" "Dec s" "Comp Size"
    printf "├──────────┼──────────┼──────────┼──────────┼────────────────┤\n"

    # NEX (balanced - default)
    local t0=$(date +%s%N)
    $NEXC -c "$file" -o "$TMPDIR/out.nex" 2>/dev/null
    local t1=$(date +%s%N)
    $NEXC -d "$TMPDIR/out.nex" -o "$TMPDIR/out.dec" 2>/dev/null
    local t2=$(date +%s%N)
    local nex_size=$(wc -c < "$TMPDIR/out.nex" | tr -d ' ')
    local nex_ct=$(echo "scale=3; ($t1 - $t0) / 1000000000" | python3 -c "import sys; print(f'{eval(sys.stdin.read()):.3f}')")
    local nex_dt=$(echo "scale=3; ($t2 - $t1) / 1000000000" | python3 -c "import sys; print(f'{eval(sys.stdin.read()):.3f}')")
    local nex_ratio=$(python3 -c "print(f'{$nex_size/$orig_size*100:.1f}%')")
    diff "$file" "$TMPDIR/out.dec" > /dev/null 2>&1 || nex_ratio="FAIL"
    printf "│ %-8s │ %8s │ %8s │ %8s │ %14s │\n" \
           "NEX" "$nex_ratio" "${nex_ct}s" "${nex_dt}s" "$nex_size"
    rm -f "$TMPDIR/out.nex" "$TMPDIR/out.dec"

    # XZ (default level 6)
    t0=$(date +%s%N)
    xz -k -f "$file" -c > "$TMPDIR/out.xz" 2>/dev/null
    t1=$(date +%s%N)
    xz -d -k "$TMPDIR/out.xz" -c > "$TMPDIR/out.dec" 2>/dev/null
    t2=$(date +%s%N)
    local xz_size=$(wc -c < "$TMPDIR/out.xz" | tr -d ' ')
    local xz_ct=$(echo "scale=3; ($t1 - $t0) / 1000000000" | python3 -c "import sys; print(f'{eval(sys.stdin.read()):.3f}')")
    local xz_dt=$(echo "scale=3; ($t2 - $t1) / 1000000000" | python3 -c "import sys; print(f'{eval(sys.stdin.read()):.3f}')")
    local xz_ratio=$(python3 -c "print(f'{$xz_size/$orig_size*100:.1f}%')")
    printf "│ %-8s │ %8s │ %8s │ %8s │ %14s │\n" \
           "XZ" "$xz_ratio" "${xz_ct}s" "${xz_dt}s" "$xz_size"
    rm -f "$TMPDIR/out.xz" "$TMPDIR/out.dec"

    # Zstd (default level 3)
    t0=$(date +%s%N)
    zstd -f "$file" -o "$TMPDIR/out.zst" 2>/dev/null
    t1=$(date +%s%N)
    zstd -d -f "$TMPDIR/out.zst" -o "$TMPDIR/out.dec" 2>/dev/null
    t2=$(date +%s%N)
    local zst_size=$(wc -c < "$TMPDIR/out.zst" | tr -d ' ')
    local zst_ct=$(echo "scale=3; ($t1 - $t0) / 1000000000" | python3 -c "import sys; print(f'{eval(sys.stdin.read()):.3f}')")
    local zst_dt=$(echo "scale=3; ($t2 - $t1) / 1000000000" | python3 -c "import sys; print(f'{eval(sys.stdin.read()):.3f}')")
    local zst_ratio=$(python3 -c "print(f'{$zst_size/$orig_size*100:.1f}%')")
    printf "│ %-8s │ %8s │ %8s │ %8s │ %14s │\n" \
           "Zstd" "$zst_ratio" "${zst_ct}s" "${zst_dt}s" "$zst_size"
    rm -f "$TMPDIR/out.zst" "$TMPDIR/out.dec"

    printf "└──────────┴──────────┴──────────┴──────────┴────────────────┘\n"
    echo ""
}

# ── Run benchmarks ────────────────────────────────────────────
bench_file "English Text"    "$TMPDIR/english_text.txt"
bench_file "C Source Code"   "$TMPDIR/source_code.c"
bench_file "JSON Data"       "$TMPDIR/data.json"
bench_file "Binary Patterns" "$TMPDIR/binary.bin"
bench_file "Random Data"     "$TMPDIR/random.bin"
bench_file "Server Log"      "$TMPDIR/server.log"

echo "Done."
