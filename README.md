# ⚡ NITRO NG (Next Gen) 1.05 HP

![Version](https://shields.io)
![Format](https://shields.io)
![Platform](https://shields.io)
![License](https://shields.io)

> **BIT-EXACT OR DEATH. THE NORTH REMEMBERS EVERY BYTE.**

---

## 🏛️ Overview

Where ancient silicon refuses to yield, and old-guard CPUs withstand the passage of time, **Nitro NG** is forged. 

Nitro NG is a highly parallel, hardware-aware pre-compression tool engineered natively in **C++20** for Linux (POSIX) and Win32. Its sole, sacred purpose is to cut through opaque game files (such as Unreal Engine `.pak` containers or GOG setups), locate buried `zlib`/`deflate` compressed layers, and inflate them into their raw, expanded state with bit-exact mathematical precision. 

By restructuring the layout of your data into expanded streams, Nitro NG conditions game assets so secondary solid compression heavyweights (like **LZMA2** or **ZSTD**) can squeeze them down to absolute physical limits.

With a minimal static footprint of **~1.7 MB**, Nitro NG is built using strict **Data-Oriented Design (DOD)** principles, keeping hot working data inside the physical L2/L3 cache blocks of your CPU and avoiding expensive operating system heap allocations entirely.

---

## ⚔️ The Pillars of Performance

### 1. Lock-Free Fingerprint Cache Injection (99.8% Hits)
The scanner does not guess; it remembers. Using a single-step **FNV-1a 64-bit mixing algorithm** coupled with safe byte-copying (`std::memcpy`), Nitro NG hashes the very first bytes of an encountered compressed layer. If that pattern has already been solved by brute-force loops across Zlib compression levels `{6, 9, 1}`, subsequent threads bypass evaluation entirely, achieving a **99.85% cache hit rate** and saving billions of CPU clock cycles.

### 2. Sharded Dictionary Cache (Anti-False Sharing)
To prevent hardware cores from fighting over a global lookup table (*lock contention*), the Zlib dictionary cache is split symmetrically into **16 independent shards**. Every individual slot is isolated with an immutable **64-byte structural padding block**, completely neutralizing *False Sharing* by preventing neighboring CPU cores from cross-invalidating each other's L1/L2 cache lines.

### 3. Asynchronous Pipeline Streaming (v1.05 Architecture)
Nitro NG strips away volatile thread-local vector buffering models that cause system RAM spikes. Workers stream data in real-time to an optimized, concurrent priority queue (`std::map`). A dedicated sequential consumer thread dumps blocks directly to disk using an optimized lock-reacquisition scope, dropping the live memory footprint to a flat, predictable **<64MB** regardless of the size of the target archive.

### 4. Purist Bare-Metal I/O (Zero-Copy & DMA)
All overhead from the standard C++ runtime library (`std::ofstream`/`std::ifstream`) has been surgically excised. Nitro NG speaks directly to operating system kernels:
*   **Linux:** Utilizes high-frequency `mmap` mapping and thread-safe `pread` calls.
*   **Windows:** Leverages native wide-character Unicode `CreateFileW`, sequential chunked `WriteFile` boundaries, and explicit asynchronous `OVERLAPPED` cursor jumps. Uncompressed raw sections (*gaps*) bypass system RAM entirely via **Direct Memory Access (DMA)** straight to the storage host controller.

---

## 📦 The PREC Container Format (v14 Specs)

Nitro NG serializes files into a sequential, linear `.pre` (PREC v14) container. It isolates metadata tracking to the tail end of the file, allowing continuous, unobstructed sequential disk streaming during runtime.

### Physical Disk Layout

  +-------------------------------------------------------+
  |  GeneralPrecompHeader (Packed, Fixed 25 Bytes)        |
  +-------------------------------------------------------+
  |  EXPANDED PAYLOAD DATA                                |
  |  -> Sequential blocks of Raw Gaps + Inflated Streams  |
  |  -> Total exact length = h.expanded_size bytes        |
  +-------------------------------------------------------+
  |  METADATA DESCRIPTOR TABLE                            |
  |  -> Array of [StreamMetadata] blocks per stream       |
  |  -> Location offset = sizeof(Header) + h.expanded_size|
  +-------------------------------------------------------+

### 🛡️ Strict Cryptographic Hardening
To prevent Denial of Service (DoS) attacks or process crashes due to corrupt headers, the decoder enforces a strict validation check on boot:
Expected Size == 25 + h.expanded_size + (h.num_streams * 26)

Any malicious file declaring phantom allocations (e.g., num_streams > 10,000,000) is violently dropped by the Win32 kernel before a single byte of memory is reserved on the heap.

---

## 📈 Performance Benchmarks

### Test Environment
*   **CPU:** Intel Core 2 Quad Q6600 (4 Cores / 4 Threads @ 2.40 GHz - Launched 2007)
*   **Architecture:** Kentsfield MCM (Dual-die, 2x4MB shared L2 cache, 1066 MHz FSB)
*   **OS:** Windows 10 x64 / Linux x64 Kernel
*   **Target Asset:** 337.93 MB Game Archive GOG INSTALLER (containing 671 target Zlib streams)

### Execution Metrics

| Operation Mode | Allocated Threads | Target Speed | Cache Performance | Heap Fallbacks | Process Wall-Time |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **v1.03 Baseline Encode** | `-j 1` (Linear) | ~6.91 MB/s | 0% (Brute force) | Constant malloc | using wine 50.9 seconds |
| **v1.05 HP Encode (Definitive)** | `-j 4` (Parallel) | **12.81 MB/s** | **99.85% (670/671)** | **0 Fallbacks** | **35.6 seconds** |
| **v1.05 HP Decode (Definitive)** | `-j 4` (Parallel) | **16.24 MB/s** | *N/A (Symmetric)* | **0 Fallbacks** | **23.6 seconds** |

*Resulting Payload Details:* Expanded to **1,815,238,228 bytes** (1.73 GB). Residual gaps preserved at **14,364,389 bytes** (exactly 4.1% of the original archive size). Unlocked true scalable parallel throughput across separate CPU dies on modern storage controllers.

---

## 🛠️ Compilation Guideliness

To awaken the full power of Nitro NG on vintage or modern architectures, build pipelines must explicitly strip dead code sections and release stack pointers.

### 🐧 Linux Native (G++)
```bash
g++ -O3 -march=core2 -mtune=core2 -fomit-frame-pointer -DNDEBUG -std=c++20 \
    main.cpp -lz -lpthread -static -static-libgcc -static-libstdc++ \
    -Wl,--gc-sections -s -o nitro_ng
```

### 🪟 Windows Cross-Compilation (MinGW-w64 on Linux)
To handle wide-character UTF-16 Unicode paths natively via the NTFS kernel layer, you must force compiler frontend swaps using `-municode`:
```bash
x86_64-w64-mingw32-g++ -O3 -std=c++20 \
    nitro_ng_windows.cpp \
    -o nitro_ng.exe \
    -municode \
    -mconsole \
    -l:libz.a \
    -lpthread \
    -static -static-libgcc -static-libstdc++ \
    -Wl,--gc-sections \
    -s \
    -Wl,--stack,4194304
```

---

## 🕹️ Command Reference

Nitro NG implements an explicit console command loop layout.

```bash
# Pre-compress / Inflate an archive
./nitro_ng e <input.pak> <output.pre> -j 4

# Restore / Re-compress back to original layout (Bit-Exact)
./nitro_ng d <output.pre> <restored.pak> -j 4
```

---

## 📜 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details. Built upon the architectural lineage of `precomp classic` and `Xtool` mechanics.

