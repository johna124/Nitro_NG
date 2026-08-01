================================================================================
 _______  .________________________ ________     _______    ________ 
 \      \ |   \__    ___/\______   \\_____  \    \      \  /  _____/ 
 /   |   \|   | |    |    |       _/ /   |   \   /   |   \/   \  ___ 
/    |    \   | |    |    |    |   \/    |    \ /    |    \    \_\  \
\____|__  /___| |____|    |____|_  /\_______  / \____|__  /\______  /
        \/                       \/         \/          \/        \/ 
================================================================================
             --- THE SHARDED ENGINE OF THE CONCURRENT NORTH ---
================================================================================

[OVERVIEW]
In the halls where ancient silicon refuses to die, where old-school CPUs 
resist the passage of time, NITRO NG (Next Gen) is born. An asynchronous 
linear pre-compression engine, forged natively for Linux, designed with a 
single sacred purpose: to tear apart opaque containers (Unreal .pak, GOG setups), 
locate Zlib compression flows hidden in the stone, and inflate them to their 
expanded state with bit-exact mathematical precision.

While past engines collapsed under the weight of Preflate or drowned in the massive 
allocation of dynamic memory, Nitro NG cuts the Gordian knot. With a static executable 
of a mere 1.7 MB, it can fit entirely within the physical L2 cache of an 
Intel Core 2 Quad Q6600, running at lightning speed without touching the operating 
system heap.

This isn't generic software. This is classic hardware craftsmanship, data-oriented 
design pushed to the absolute limits of silicon.

--------------------------------------------------------------------------------
[THE PILLARS OF PERFORMANCE]
--------------------------------------------------------------------------------

1. LOCK-FREE SMART CACHE (65,536 SLOTS)
The scanner doesn't guess; it remembers. Using a single-step, 64-bit direct-read 
FNV-1a merging algorithm, the engine calculates the fingerprint of the first few 
bytes of the compressed stream. If the pattern has already been brute-forced 
(levels 6, 9, 1), subsequent iterations jump directly to the correct level, 
saving billions of redundant clock cycles.

2. PARALLEL ARCHITECTURE BY SHARDS (ANTI-FALSE SHARING)
To prevent CPU cores from fighting each other for control of the dictionary 
table, Zlib's global cache is divided into 16 independent shards. 
Each slot is shielded with 64 bytes of physical padding,creating an 
impenetrable barrier that prevents one core from invalidating the

L1/L2 cache of the adjacent core. Pure atomic concurrency (Acquire-Release)
with a slot stealing algorithm (LRU Stealing) that guarantees predictable
performance under extreme load.

3. ZERO-COPY & DMA OPTIMIZATION
Non-Zlib flat data blocks (gaps) are not copied into RAM.
The engine eliminates traditional "intermediate buffers."
Using the Linux kernel's native memory mapping (mmap),
data flows directly from physical storage to the file system via DMA,
emptying the memory bus and stretching the bandwidth of the NVMe/SATA SSD.


4. PERSISTENT BUFFER POOLS (ZERO MALLOC)
In both compression and decompression, the hardware threads reserve their 
working buffers (16MB for vtmp, 64KB for inflate) only once at startup.

During the hot data loop, unconditional resizes (.resize()) are executed, 
shrinking the logical size in 0 CPU cycles while retaining the physical capacity intact.

The Linux garbage collector does not operate; Nitro NG owns the memory.


5. The program uses the ideas of precomp classic and Xtool with its optimized 
    zlib library.
    The result is a lighter binary and its code is much smaller.
    In this case, we haven't modified the zlib library but rather played with the 
    compression level values.

6. This program is NOT designed for PNG, ZIP, or PDF files.
    It's for files with standard zlib compression, such as Unreal .pak files
    or GOG setup files, and it works perfectly.

--------------------------------------------------------------------------------
[COMPILATION & COMMANDS]
--------------------------------------------------------------------------------

To unleash the runic power of classic hardware (Core 2 Quad / POSIX Architecture),
 the static linker must surgically purge dead code:

$ g++ -O3 -march=core2 -mtune=core2 -fomit-frame-pointer -DNDEBUG -std=c++20 \
main.cpp -lz -lpthread -static -static-libgcc -static-libstdc++ \
-Wl,--gc-sections -s -o nitro_ng

Using the Engine:

Compression: ./nitro_ng e input.pak output.pre -j 2
Restoration: ./nitro_ng d output.pre restored.pak -j 2

================================================================================
        BIT-EXACT OR DEATH. THE NORTH REMEMBERS EVERY BYTE.
================================================================================

