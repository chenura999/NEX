#!/usr/bin/env python3
# Industrial Benchmark for NEX Compression
# Tests performance across NEX, zstd, gzip, xz, lz4 on realistic source code

import subprocess
import os
import time

dataset = "/tmp/dataset.tar"

print(f"Creating realistic benchmark dataset (Linux headers)...")
subprocess.run(f"tar -cf {dataset} /usr/include", shell=True, stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
orig_size = os.path.getsize(dataset)
print(f"Dataset Size: {orig_size / (1024*1024):.2f} MB")
print()

tools = [
    ("NEX (Fast)", "./nexc -l 1 -c {0} -o /tmp/out.nex", "./nexc -d /tmp/out.nex -o /tmp/out.dec", "/tmp/out.nex"),
    ("NEX (Default)","./nexc -c {0} -o /tmp/out.nex", "./nexc -d /tmp/out.nex -o /tmp/out.dec", "/tmp/out.nex"),
    ("NEX (Max)", "./nexc -l 9 -c {0} -o /tmp/out.nex", "./nexc -d /tmp/out.nex -o /tmp/out.dec", "/tmp/out.nex"),
    ("Zstd (-1)", "zstd -1 -f {0} -o /tmp/out.zst", "zstd -d -f /tmp/out.zst -o /tmp/out.dec", "/tmp/out.zst"),
    ("Zstd (-3)", "zstd -3 -f {0} -o /tmp/out.zst", "zstd -d -f /tmp/out.zst -o /tmp/out.dec", "/tmp/out.zst"),
    ("Zstd (-19)", "zstd -19 -T0 -f {0} -o /tmp/out.zst", "zstd -d -f /tmp/out.zst -o /tmp/out.dec", "/tmp/out.zst"),
    ("Gzip (-6)", "gzip -6 -k -f {0}", "gzip -d -f {0}.gz", "{0}.gz"),
    ("XZ (-6)", "xz -6 -T0 -k -f {0}", "xz -d -f {0}.xz", "{0}.xz"),
    ("LZ4", "lz4 -f {0} /tmp/out.lz4", "lz4 -d -f /tmp/out.lz4 /tmp/out.dec", "/tmp/out.lz4")
]

print(f"{'Compressor':<15} | {'Ratio':<7} | {'Comp (MB/s)':<12} | {'Decomp (MB/s)':<12}")
print("-" * 55)

for name, comp_cmd, decomp_cmd, out_file in tools:
    # Format strings
    comp_cmd = comp_cmd.format(dataset)
    decomp_cmd = decomp_cmd.format(dataset)
    out_file = out_file.format(dataset)
    
    # Compress
    t0 = time.monotonic()
    subprocess.run(comp_cmd, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t1 = time.monotonic()
    
    if not os.path.exists(out_file):
        print(f"{name:<15} | ERROR   | {'N/A':<12} | {'N/A':<12}")
        continue
        
    comp_size = os.path.getsize(out_file)
    ratio = (comp_size / orig_size) * 100
    comp_speed = (orig_size / (1024*1024)) / (t1 - t0)
    
    # Decompress
    t2 = time.monotonic()
    subprocess.run(decomp_cmd, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t3 = time.monotonic()
    
    decomp_speed = (orig_size / (1024*1024)) / (t3 - t2)
    
    print(f"{name:<15} | {ratio:>6.1f}% | {comp_speed:>11.1f} | {decomp_speed:>11.1f}")

# Cleanup
subprocess.run("rm -f /tmp/dataset.tar*", shell=True)
subprocess.run("rm -f /tmp/out.*", shell=True)
print("\nDone.")
