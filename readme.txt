================================================================================
_______  .________________________ ________     _______    ________
\      \ |   \__    ___/\______   \\_____  \    \      \  /  _____/
/   |   \|   | |    |    |       _/ /   |   \   /   |   \/   \  ___
/    |    \   | |    |    |    |   \/    |    \ /    |    \    \_\  \
\____|__  /___| |____|    |____|_  /\_______  / \____|__  /\______  /
        \/                       \/         \/          \/        \/
================================================================================
--- THE STREAMLINED ENGINE OF THE NORTH ---
================================================================================

[OVERVIEW]
Nitro NG v1.09 is a high-performance, low-memory, multi-threaded pre-compression
utility written in C++20 for Linux, Windows x64, and Win32-XP. It parses
containers (Unreal .pak, GOG installers, game archives) and expands zlib streams
with bit-exact precision (MD5 verified) to aid secondary heavy compression
(LZMA2/ZSTD/Oodle).

The engine combines lock-free atomic telemetry, streaming verification,
segmented I/O with backpressure, and robust cross-platform support, ensuring
peak RAM usage stays tightly bounded regardless of input file size or stream
density.

Static Binary Size:
* Windows x64: ~1.0 MB (1,042,432 bytes)
* Windows XP x86: ~1.1 MB (1,169,408 bytes)
* Linux x64: ~1.7 MB (1,776,088 bytes)

Licensed under GPLv3. Free and open source forever.

--------------------------------------------------------------------------------
[CORE ARCHITECTURAL PILLARS] (more info in the changelog)
--------------------------------------------------------------------------------
1. Streaming Verification & Dynamic Allocation (The Low-RAM Update)
*The game-changer for memory.* Scanner threads verify streams on-the-fly without 
storing decompressed data. Memory arenas adapt based on thread counts, utilizing 
immediate heap reclamation in Low-Thread Mode (<= 2 threads) and retaining allocation 
pools in High-Thread Mode (> 2 threads) to minimize allocator lock contention.

2. Segmented I/O with Core-Aware Backpressure
The input file is divided into segments with a sliding window for backpressure control. 
Output write chunks scale dynamically from **2 MB** for legacy mechanical hard drives 
(preventing physical disk thrashing) up to **16 MB** for high-throughput modern NVMe arrays.


3. FLEVEL Candidate Guidance (Introduced v1.08, Perfected v1.09)
Why guess when the file tells you? The zlib header's `FLEVEL` byte provides a strong 
hint about the original compression level. The encoder tries the most likely level first, 
remembering the last successful level per class, slashing failed verification attempts by 30-40%.

4. Lock-Free Atomic Telemetry & EMA ETA (v1.07+, Stabilized in v1.09)
Real-time statistics are sampled via relaxed-order atomics on a dedicated ticker thread 
(~10 Hz) with **zero mutex contention**. An Exponential Moving Average (EMA) smooths short 
bursts, preventing violent ETA oscillations.

5. Multi-Threaded Parallel Decoder (v1.07+, Optimized in v1.09)
Replaced the legacy sequential decoder with an active, task-based worker pool. 
Workers fetch tasks dynamically via atomic fetch-add, accelerating file restoration 
by 2x–3x on multi-core hardware while keeping memory strictly controlled via a bounded byte budget.

6. Windows Zero-Map Reader & XP Native Support (v1.07+, Hardened in v1.09)
Eliminated `MapViewOfFile` entirely. We now use direct overlapped I/O (`portable_pread`), 
preventing the Windows memory manager from aggressively caching massive `.pre` archives. 
Full cross-compilation support for 32-bit Windows XP (Subsystem 5.01) with Large Address Aware linking.

7. What's New in v1.09 (The Polish Release)
Fixed Console UX:** Resolved the visual bug where the "Levels" summary would only 
display the last level. Now correctly shows `Levels: 6 (x693), 7 (x1)`.

Optimized Build System:** Scripts now compile a local, **zlib 1.3.2** and link it 
statically. Replaced slow filesystem scans with instant compiler queries (`$CXX -print-file-name`) for toolchain detection.

Validated Metrics:** All performance claims are now backed by real-world, bit-exact tested benchmarks..

--------------------------------------------------------------------------------
[PERFORMANCE BENCHMARK (FULL CYCLE)]
--------------------------------------------------------------------------------

Summary of chronological cross-platform performance data using a dense stream distribution workload 
(**352.78 MB input, 694 zlib streams, setup_virtuaverse_1.38_(92629).exe):

Used DualCore with Wine 6.0.3

ENCODE PHASE (Compression Analysis)

Engine Version               Environment/Runtime   Peak RAM   Encode Time   Pipeline Architecture & Control
---------------------------------------------------------------------------------------------------------------------------
Nitro NG v1.03 (precomp_zlib) Windows x64 (Wine)    1183 MB    2m 18.754s    First Release / Unbounded Heap Allocation
Nitro NG v1.05                Windows x64 (Wine)    1647 MB    2m 33.438s    Concurrent Multi-thread / Unthrottled Allocation Bloat
Nitro NG v1.09                Windows x64 (Wine)     89 MB     1m 36.744s    Adaptive Backpressure & Aggressive Slot Release
Nitro NG v1.09                Linux Native (Raw)    104 MB     1m 26.949s    Linear Streaming / Direct Page Cache Write
Xtool 0.8.9 (Razor12911)      Windows x64 (Wine)    489 MB     2m 18.000s    High-Thread Buffer Aggregation
Precomp 0.48* (Christian S.)  Windows x64 (Wine)     80 MB     7m 16.821s    Monolithic Sequential / Intense Flags

*Note: Precomp 0.48 flags: -cn -intense -t-pzgnfjsmb3 | Recompressed streams: 1859/1885 (Nitro & Xtool match ~696)
**Note: Precomp 0.48 uses optimized compilation flags for cmake


DECODE PHASE (Restoration & I/O Target Analysis)

Tool & Configuration         Environment/Runtime   Peak RAM   Decode Time   Multi-Threading & I/O Profile
---------------------------------------------------------------------------------------------------------------------------
Precomp 0.48 (Classic)       Windows x64 (Wine)    33 MB      2m 59.043s    Hardcoded Monolithic Sequential Execution (3x Slower)
Xtool 0.8.9 (Modern)         Windows x64 (Wine)    353 MB     2m 49.000s    High-Thread Buffer Aggregation (HDD Thrashing)
Nitro NG v1.09     Windows x64 (Wine)    45 MB      2m 31.654s    Bounded Queue with 2 MB Core-Aware Disk Flushing
Nitro NG v1.09 (Real HDD)    Linux Native (Raw)    51 MB      2m 00.947s    Pure Uncached Mechanical Plato I/O (Dropped Caches)
Nitro NG v1.09 (Cached)      Linux Native (Raw)    46 MB      0m 46.000s    Native Linux Kernel Page-Cache Streaming Mode

Conclusion: Runtime-aware backpressure and conditional memory reclamation safely shrink 
historical multi-gigabyte memory leaks down to a baseline of 45-89 MB, transforming a 
flawed multithreading attempt into a hardware-respectful streaming engine.

--------------------------------------------------------------------------------
[HOST DEPENDENCIES & CROSS-COMPILATION]
--------------------------------------------------------------------------------
Debian/Ubuntu/Mint MinGW Win32/Win64 setup:
$ sudo apt-get update && sudo apt-get install -y mingw-w64-tools libz-mingw-w64-dev

32-bit MinGW additional packages (for XP x86 target):
$ sudo apt-get install -y binutils-mingw-w64-i686 gcc-mingw-w64-i686-posix \
  g++-mingw-w64-i686-posix

Linux Native Build:
$ ./compile_linux.sh

Windows x64 Cross-Compilation:
$ ./compile_windows.sh

Windows XP x86 Cross-Compilation:
$ ./compile_windows_xp.sh

All build scripts automatically compile a local optimized zlib 1.3.2 and link
it statically. No external dependencies required at runtime.

--------------------------------------------------------------------------------
[COMMAND REFERENCE]
--------------------------------------------------------------------------------
Usage:
Encode: nitro_ng e <input> <output.pre> [-j threads]
Decode: nitro_ng d <input.pre> <restored> [-j threads]

Options:
-j N          Number of worker threads.
              (Default: hardware concurrency)

Examples:
nitro_ng e input.bin output.pre -j 8
nitro_ng d output.pre restored.bin -j 8

--------------------------------------------------------------------------------
[VERIFICATION & PERFORMANCE PROFILE]
--------------------------------------------------------------------------------
* Integrity: 100% verified. Bit-for-bit restoration matches original input
  (MD5 confirmed).
* RAM Footprint: Bounded and deterministic (~45-89 MB peak on legacy hardware with 
  Low-Thread Profile). Scales dynamically with the segment window size and thread 
  allocation, not with the input file size or stream density.
  size, not with input file size.
* UX Stability: Single-line live progress with hidden console cursor and clean
  rendering. Works correctly on narrow terminals and Windows/Wine environments.

--------------------------------------------------------------------------------
[A NOTE OF RESPECT TO THE PIONEERS]
--------------------------------------------------------------------------------
Absolute respect to classic tools like Precomp by Christian Schneider and the 
incredible work of Razor12911 with Xtool; they were the absolute pioneers of 
this technology. This benchmark does not seek to compete in raw horsepower on 
modern workstations, but strictly to measure the efficiency of memory 
management within environments with severely limited hardware resources 
(hardware-constrained platforms).


--------------------------------------------------------------------------------
[LICENSING]
--------------------------------------------------------------------------------
Nitro NG is licensed under the GNU General Public License v3.0 (GPLv3).

This means:
* You can use it for any purpose, including commercial projects.
* You can modify the source code and distribute your changes.
* You must provide the source code when distributing binaries.
* You must preserve the license and copyright notices.

Nitro NG is free forever and open for the community to audit, extend, and
improve. If you want to add support for Oodle, LZ4, or Zstandard.

================================================================================
BIT-EXACT OR DEATH. THE NORTH REMEMBERS EVERY BYTE.
================================================================================
