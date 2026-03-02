#!/bin/bash
# NEX Compress — Round-Trip Test Script
# Tests compress → decompress → diff on multiple file types

set -e
cd "$(dirname "$0")/.."

NEXC=./nexc
PASS=0
FAIL=0
TMPDIR=$(mktemp -d)

cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

test_roundtrip() {
    local name="$1" file="$2" args="$3"
    
    "$NEXC" $args -c "$file" -o "$TMPDIR/test.nex" 2>/dev/null
    local rc1=$?
    if [ $rc1 -ne 0 ]; then
        echo "✗ FAIL: $name (compress exit=$rc1)"
        FAIL=$((FAIL + 1))
        return
    fi
    
    "$NEXC" -d "$TMPDIR/test.nex" -o "$TMPDIR/test.out" 2>/dev/null
    local rc2=$?
    if [ $rc2 -ne 0 ]; then
        echo "✗ FAIL: $name (decompress exit=$rc2)"
        FAIL=$((FAIL + 1))
        return
    fi
    
    if diff "$file" "$TMPDIR/test.out" > /dev/null 2>&1; then
        echo "✓ PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "✗ FAIL: $name (data mismatch!)"
        FAIL=$((FAIL + 1))
    fi
    
    rm -f "$TMPDIR/test.nex" "$TMPDIR/test.out"
}

echo "══════════════════════════════════════════════"
echo "  NEX Compress — Round-Trip Test Suite"
echo "══════════════════════════════════════════════"
echo ""

# Create test files
echo "Creating test data..."

# Small text
echo "Hello world! This is a test." > "$TMPDIR/small.txt"

# Repeated text
python3 -c "print('Test pattern. ' * 1000)" > "$TMPDIR/repeated.txt"

# Binary with patterns
python3 -c "
import sys
data = bytes(range(256)) * 20
sys.stdout.buffer.write(data)
" > "$TMPDIR/binary.bin"

# Random data
dd if=/dev/urandom of="$TMPDIR/random.bin" bs=1024 count=32 2>/dev/null

# Empty file
touch "$TMPDIR/empty.txt"

# Single byte
echo -n "X" > "$TMPDIR/single.txt"

echo ""
echo "--- Auto Pipeline ---"
test_roundtrip "Small text / auto"    "$TMPDIR/small.txt"
test_roundtrip "Repeated text / auto" "$TMPDIR/repeated.txt"
test_roundtrip "Binary / auto"        "$TMPDIR/binary.bin"
test_roundtrip "Random / auto"        "$TMPDIR/random.bin"
test_roundtrip "Single byte / auto"   "$TMPDIR/single.txt"

echo ""
echo "--- Balanced Pipeline ---"
test_roundtrip "Repeated / balanced"  "$TMPDIR/repeated.txt" "-p balanced"
test_roundtrip "Binary / balanced"    "$TMPDIR/binary.bin"    "-p balanced"

echo ""
echo "--- Fast Pipeline ---"
test_roundtrip "Repeated / fast"      "$TMPDIR/repeated.txt" "-p fast"
test_roundtrip "Binary / fast"        "$TMPDIR/binary.bin"    "-p fast"

echo ""
echo "--- Max Pipeline ---"
test_roundtrip "Repeated / max"       "$TMPDIR/repeated.txt" "-p max -l 9"
test_roundtrip "Binary / max"         "$TMPDIR/binary.bin"    "-p max -l 9"

echo ""
echo "--- Store Pipeline ---"
test_roundtrip "Repeated / store"     "$TMPDIR/repeated.txt" "-p store"
test_roundtrip "Random / store"       "$TMPDIR/random.bin"    "-p store"

echo ""
echo "--- Compression Levels ---"
test_roundtrip "Level 1"              "$TMPDIR/repeated.txt" "-l 1"
test_roundtrip "Level 5"              "$TMPDIR/repeated.txt" "-l 5"
test_roundtrip "Level 9"              "$TMPDIR/repeated.txt" "-l 9"

echo ""
echo "══════════════════════════════════════════════"
echo "  Results: $PASS passed, $FAIL failed"
echo "══════════════════════════════════════════════"

exit $FAIL
