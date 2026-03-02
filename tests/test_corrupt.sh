#!/bin/bash
set -e

echo "══════════════════════════════════════════════════"
echo "  NEX Compress - Edge Case & Fuzzing Resilience"
echo "══════════════════════════════════════════════════"

# Create valid source data
TEST_FILE="/tmp/fuzz_target.txt"
dd if=/dev/urandom of=$TEST_FILE bs=1M count=2 status=none
./nexc -c $TEST_FILE -o /tmp/valid.nex >/dev/null

echo "1. Testing Unrecognized Format (Wrong Magic Bytes)..."
dd if=/dev/urandom of=/tmp/garbage.nex bs=1K count=10 status=none
# Catch output containing 'Error:'
if ./nexc -d /tmp/garbage.nex -o /tmp/out.bin 2>&1 | grep -qi 'error\|invalid'; then
    echo "   ✓ PASS: Successfully rejected garbage file without crashing."
else
    echo "   ✗ FAIL: Did not properly reject bad magic bytes!"
    exit 1
fi

echo "2. Testing Truncated Header (Incomplete file)..."
head -c 10 /tmp/valid.nex > /tmp/truncated_header.nex
if ./nexc -d /tmp/truncated_header.nex -o /tmp/out.bin 2>&1 | grep -qi 'error\|truncated\|read'; then
    echo "   ✓ PASS: Successfully handled aborted/truncated header."
else
    echo "   ✗ FAIL: Did not properly reject truncated header!"
    exit 1
fi

echo "3. Testing Truncated Payload (Network drop simulation)..."
FILE_SIZE=$(stat -c%s /tmp/valid.nex)
HALF_SIZE=$((FILE_SIZE / 2))
head -c $HALF_SIZE /tmp/valid.nex > /tmp/half_payload.nex
if ./nexc -d /tmp/half_payload.nex -o /tmp/out.bin 2>&1 | grep -qi 'error\|truncated\|corrupt'; then
    echo "   ✓ PASS: Safely halted on incomplete chunks/EOF."
else
    echo "   ✗ FAIL: Did not reject truncated chunks!"
    exit 1
fi

echo "4. Testing Checksum Corruption (Bit flipped inside payload)..."
cp /tmp/valid.nex /tmp/corrupt_chksm.nex
# Flip bits at an arbitrary point inside the first chunk payload
printf '\xDE\xAD\xBE\xEF' | dd of=/tmp/corrupt_chksm.nex bs=1 seek=100 conv=notrunc status=none
if ./nexc -d /tmp/corrupt_chksm.nex -o /tmp/out.bin 2>&1 | grep -qi 'error\|checksum\|corrupt'; then
    echo "   ✓ PASS: Checksum validation caught payload corruption!"
else
    echo "   ✗ FAIL: Allowed corrupted payload to decompress silently!"
    exit 1
fi

echo "5. Testing Malicious Version Byte..."
cp /tmp/valid.nex /tmp/bad_version.nex
# Version is byte 4 (offset 3). Give it version 0xFF.
printf '\xFF' | dd of=/tmp/bad_version.nex bs=1 seek=3 conv=notrunc status=none
if ./nexc -d /tmp/bad_version.nex -o /tmp/out.bin 2>&1 | grep -qi 'error\|version\|invalid'; then
    echo "   ✓ PASS: Rejected future/unsupported file version."
else
    echo "   ✗ FAIL: Attempted to parse bad version byte!"
    exit 1
fi

echo "══════════════════════════════════════════════════"
echo "  All security resilience tests passed!"
echo "══════════════════════════════════════════════════"
