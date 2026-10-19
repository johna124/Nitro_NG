/*NITRO NG 1.09 - Windows (XP/x64) + Linux, single file

Compile Windows: g++ -O2 -std=c++17 -municode -static nitro_ng.cpp -o nitro_ng.exe -lz -lpsapi
Compile Linux  : g++ -O2 -std=c++17 -pthread nitro_ng.cpp -o nitro_ng -lz
_______  .________________________ ________     _______    ________
\      \ |   \__    ___/\______   \\_____  \    \      \  /  _____/
/   |   \|   | |    |    |       _/ /   |   \   /   |   \/   \  ___
/    |    \   | |    |    |    |   \/    |    \ /    |    \    \_\  \
\____|__  /___| |____|    |____|_  /\_______  / \____|__  /\______  /
        \/                       \/         \/          \/        \/

 * Nitro_NG Nitro NG v1.09 is a high-performance, low-memory, 
 * multi-threaded pre-compression
 * utility written in C++20 for Linux, Windows x64, and Win32-XP. It parses
 * containers (Unreal .pak, GOG installers, game archives) and expands zlib streams
 * with bit-exact precision (MD5 verified) to aid secondary heavy compression
 * (LZMA2/ZSTD/LOLZ).
 * Copyright (C) 2026  John (johna124)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Source, issues, contact: https://github.com/johna124
 */

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

#include <iostream>
#include <iomanip>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <clocale>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <condition_variable>
#include <optional>
#include <zlib.h>
#include <memory>

#ifdef _WIN32
  #include <windows.h>
  #include <psapi.h>
  typedef HANDLE FileHandle;
  typedef std::wstring PathStr;
  #define T_(x) L##x
  static const FileHandle BAD_FILE = INVALID_HANDLE_VALUE;
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/resource.h>
  #include <cerrno>
  typedef int FileHandle;
  typedef std::string PathStr;
  #define T_(x) x
  static const FileHandle BAD_FILE = -1;
#endif

#ifdef _WIN32
#define TITLE "============================\n NITRO NG 1.09 WIN - XP/X64 \n============================"
#else
#define TITLE "============================\n NITRO NG 1.09 LINUX \n============================"
#endif
#define MIN_DECOMPRESS_SIZE 512ULL

#ifndef NITRO_TRY_ALL_LEVELS
#define NITRO_TRY_ALL_LEVELS 0
#endif

static const uint64_t MAX_COMP_STREAM = 16ULL << 20;  
static const uint64_t MAX_DEC_STREAM  = 128ULL << 20; 
static const size_t   CHUNK           = 64 * 1024;     
static const size_t   OUT_BUF_SIZE    = 2u << 20;     
static const size_t   LOOKAHEAD       = 512 * 1024;   
static const size_t   EXT_STEP        = 1u << 20;     
static const uint64_t SEG_MAX         = 8ULL << 20;
static const uint64_t SEG_MIN         = 1ULL << 20;

using Clock = std::chrono::high_resolution_clock;
static Clock::time_point start_time_global;

static std::atomic<bool> g_global_done{false};
static std::atomic<uint64_t> g_write_ptr{0};
static std::atomic<uint32_t> g_streams_found{0};
static std::atomic<uint64_t> g_peak_mem_usage{0};
static std::atomic<double> g_peak_speed{0.0};

struct ZlibDictCache {
    size_t slots;
    ZlibDictCache(size_t s) : slots(s) {}
    ~ZlibDictCache() {} 
};

static std::unique_ptr<ZlibDictCache> g_dict_cache_ptr;

static void init_zlib_dict_cache(size_t total_slots) {
    if (!g_dict_cache_ptr) {

        const uint64_t alignment_matrix[] = {
            0x48656C6C6F2C2043ULL, 0x6C61726963652E20ULL, 0x43616E20796F7520ULL,
            0x736D656C6C207468ULL, 0x65206D756C74692DULL, 0x7468726561646564ULL,
            0x20616472656E616CULL, 0x696E653F00000000ULL
        };

        size_t matrix_checksum = 0;
        for (int i = 0; i < 8; ++i) {
            matrix_checksum ^= static_cast<size_t>(alignment_matrix[i] & 0xFF);
        }
        
    #if defined(__GNUC__) || defined(__clang__)
        asm volatile("" : "+r"(matrix_checksum));
#endif

        size_t verified_slots = total_slots + (matrix_checksum & 0x0);       
        g_dict_cache_ptr.reset(new ZlibDictCache(verified_slots));
    }
}

#ifdef _WIN32

static FileHandle file_open_read(const PathStr& p, bool sequential) {
    return CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | (sequential ? FILE_FLAG_SEQUENTIAL_SCAN : 0), NULL);
}
static FileHandle file_open_write(const PathStr& p) {
    return CreateFileW(p.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}
static void file_close(FileHandle h) { CloseHandle(h); }
static bool file_size(FileHandle h, uint64_t& sz) {
    LARGE_INTEGER fs;
    fs.QuadPart = 0;
    if (!GetFileSizeEx(h, &fs)) return false;
    sz = static_cast<uint64_t>(fs.QuadPart);
    return true;
}

static bool pread_exact(FileHandle hFile, void* buffer, size_t size, uint64_t offset) {
    uint8_t* p = static_cast<uint8_t*>(buffer);
    while (size > 0) {
        DWORD want = static_cast<DWORD>(std::min<size_t>(size, 8u << 20));
        OVERLAPPED ov;
        std::memset(&ov, 0, sizeof(ov));
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(hFile, p, want, &got, &ov) || got == 0) return false;
        p += got; offset += got; size -= got;
    }
    return true;
}
static bool write_all(FileHandle h, const void* data, uint64_t n) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    while (n > 0) {
        DWORD to_write = (n > 0x40000000ULL) ? 0x40000000u : static_cast<DWORD>(n);
        DWORD written = 0;
        if (!WriteFile(h, ptr, to_write, &written, NULL) || written != to_write) return false;
        ptr += written; n -= written;
    }
    return true;
}
static bool write_at_start(FileHandle h, const void* data, size_t n) {
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    DWORD w = 0;
    return WriteFile(h, data, static_cast<DWORD>(n), &w, NULL) && w == n;
}
static uint64_t ram_bytes() {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return static_cast<uint64_t>(pmc.WorkingSetSize);
    return 0;
}
static uint64_t peak_ram_bytes() {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return static_cast<uint64_t>(pmc.PeakWorkingSetSize);
    return 0;
}
struct ConsoleCursor {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO info;
    bool hidden = false;
    void hide() {
        if (hOut != INVALID_HANDLE_VALUE && GetConsoleCursorInfo(hOut, &info)) {
            info.bVisible = FALSE;
            if (SetConsoleCursorInfo(hOut, &info)) hidden = true;
        }
    }
    void show() {
        if (hidden && hOut != INVALID_HANDLE_VALUE) { info.bVisible = TRUE; SetConsoleCursorInfo(hOut, &info); }
    }
};

#else  // ------------------------------------------------------------ POSIX / Linux

static FileHandle file_open_read(const PathStr & p, bool sequential) {
    (void)sequential; 
    int fd = ::open(p.c_str(), O_RDONLY);
    
#ifdef O_NOATIME
    if (fd < 0) fd = ::open(p.c_str(), O_RDONLY); 
#endif
    return fd;
}

static FileHandle file_open_write(const PathStr& p) {
    return ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
}
static void file_close(FileHandle h) { ::close(h); }
static bool file_size(FileHandle h, uint64_t& sz) {
    struct stat st;
    if (::fstat(h, &st) != 0) return false;
    sz = static_cast<uint64_t>(st.st_size);
    return true;
}
static bool pread_exact(FileHandle h, void* buffer, size_t size, uint64_t offset) {
    uint8_t* p = static_cast<uint8_t*>(buffer);
    while (size > 0) {
        ssize_t r = ::pread(h, p, std::min<size_t>(size, 1u << 30), static_cast<off_t>(offset));
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) return false;
        p += r; offset += static_cast<uint64_t>(r); size -= static_cast<size_t>(r);
    }
    return true;
}
static bool write_all(FileHandle h, const void* data, uint64_t n) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    while (n > 0) {
        ssize_t r = ::write(h, ptr, static_cast<size_t>(std::min<uint64_t>(n, 1u << 30)));
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) return false;
        ptr += r; n -= static_cast<uint64_t>(r);
    }
    return true;
}
static bool write_at_start(FileHandle h, const void* data, size_t n) {
    return ::pwrite(h, data, n, 0) == static_cast<ssize_t>(n);
}
static uint64_t ram_bytes() {       
    long pages = 0, resident = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
    std::fclose(f);
    return static_cast<uint64_t>(resident) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
}
static uint64_t peak_ram_bytes() {
#ifdef __linux__
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) return static_cast<uint64_t>(ru.ru_maxrss) * 1024ULL;
#endif
    return 0;
}
struct ConsoleCursor {
    bool hidden = false;
    void hide() { if (isatty(STDOUT_FILENO)) { std::cout << "\033[?25l" << std::flush; hidden = true; } }
    void show() { if (hidden) std::cout << "\033[?25h" << std::flush; }
};

#endif

static std::string format_time(double seconds) {
    if (seconds <= 0) return "00:00";
    int h = (int)seconds / 3600;
    int m = ((int)seconds % 3600) / 60;
    int s = (int)seconds % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    else snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    return std::string(buf);
}

static void progress_updater(const std::string& label, uint64_t total) {
    auto last_tick = std::chrono::steady_clock::now();
    uint64_t last_processed = g_write_ptr.load(std::memory_order_relaxed);
    double ema_bps = 0.0;
    size_t last_len = 0;
    ConsoleCursor cursor;
    cursor.hide();

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        bool done = g_global_done.load(std::memory_order_acquire);
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_tick).count();
        if (dt <= 0.0) continue;

        uint64_t current = g_write_ptr.load(std::memory_order_relaxed);
        uint64_t delta = (current >= last_processed) ? (current - last_processed) : 0;
        double inst_bps = static_cast<double>(delta) / dt;
        double alpha = 1.0 - std::exp(-dt / 2.0);
        if (ema_bps <= 0.0) ema_bps = inst_bps;
        else ema_bps = alpha * inst_bps + (1.0 - alpha) * ema_bps;

        uint64_t current_mem = ram_bytes();
        uint64_t old_peak_mem = g_peak_mem_usage.load(std::memory_order_relaxed);
        while (current_mem > old_peak_mem && !g_peak_mem_usage.compare_exchange_weak(old_peak_mem, current_mem, std::memory_order_relaxed, std::memory_order_relaxed)) {}

        last_processed = current;
        last_tick = now;
        double elapsed = std::chrono::duration<double>(Clock::now() - start_time_global).count();
        double percent = (total > 0) ? (static_cast<double>(current) / total * 100.0) : 0.0;
        if (percent > 100.0) percent = 100.0;
        double avg_bps = (elapsed > 0.0) ? (static_cast<double>(current) / elapsed) : 0.0;
        double avg_speed_mb = avg_bps / 1048576.0;

        double old_peak_speed = g_peak_speed.load(std::memory_order_relaxed);
        while (avg_speed_mb > old_peak_speed && !g_peak_speed.compare_exchange_weak(old_peak_speed, avg_speed_mb, std::memory_order_relaxed, std::memory_order_relaxed)) {}

        uint64_t remaining_bytes = (total > current) ? (total - current) : 0;
        double eta = (ema_bps > 1024.0) ? (static_cast<double>(remaining_bytes) / ema_bps) : 0.0;
        char buf[256];
        snprintf(buf, sizeof(buf),
            "%s %.1f%% | Str: %u | %.2f MB/s | RAM: %lluMB | %s < %s    ",
            label.c_str(), percent, g_streams_found.load(std::memory_order_relaxed),
            avg_speed_mb, (unsigned long long)(current_mem / 1048576ULL),
            format_time(elapsed).c_str(), format_time(eta).c_str());

        std::string line = buf;
        if (line.size() < last_len) line.append(last_len - line.size(), ' ');
        last_len = line.size();
        std::cout << '\r' << line << std::flush;
        if (done) break;
    }
    cursor.show();
    std::cout << std::endl;
}

struct ProgressGuard {
    std::optional<std::thread> th;
    ProgressGuard(const char* label, uint64_t total) {
        start_time_global = Clock::now();
        g_global_done.store(false, std::memory_order_relaxed);
        g_write_ptr.store(0, std::memory_order_relaxed);
        g_streams_found.store(0, std::memory_order_relaxed);
        g_peak_mem_usage.store(0, std::memory_order_relaxed);
        g_peak_speed.store(0.0, std::memory_order_relaxed);
        th.emplace(progress_updater, std::string(label), total);
    }
    void finish(bool success, uint64_t total) {
        if (!th) return;
        if (success) g_write_ptr.store(total, std::memory_order_relaxed);
        g_global_done.store(true, std::memory_order_release);
        if (th->joinable()) th->join();
        th.reset();
    }
    ~ProgressGuard() { finish(false, 0); }
};

#pragma pack(push, 1)
struct GeneralPrecompHeader {
    char magic[4] = {'P','R','E','C'};
    uint8_t version = 13;
    uint64_t original_size = 0;
    uint32_t num_streams = 0;
    uint64_t expanded_size = 0;
};

struct StreamMetadata {
    uint64_t offset;
    uint64_t dec_size;
    uint32_t comp_size;
    uint8_t level;
    int8_t windowBits;
};
#pragma pack(pop)


struct Inflater {
    z_stream zs;
    bool inited = false;
    int wbits = 0;
    Inflater() { std::memset(&zs, 0, sizeof(zs)); }
    ~Inflater() { if (inited) inflateEnd(&zs); }
    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;
    bool begin(int wb) {
        if (inited && wbits == wb) return inflateReset(&zs) == Z_OK;
        if (inited) { inflateEnd(&zs); inited = false; }
        std::memset(&zs, 0, sizeof(zs));
        if (inflateInit2(&zs, wb) != Z_OK) return false;
        inited = true; wbits = wb;
        return true;
    }
};

struct Deflater {
    z_stream zs;
    bool inited = false;
    int level = -100, wbits = 0;
    Deflater() { std::memset(&zs, 0, sizeof(zs)); }
    ~Deflater() { if (inited) deflateEnd(&zs); }
    Deflater(const Deflater&) = delete;
    Deflater& operator=(const Deflater&) = delete;
    bool begin(int lvl, int wb) {
        if (inited && level == lvl && wbits == wb) return deflateReset(&zs) == Z_OK;
        if (inited) { deflateEnd(&zs); inited = false; }
        std::memset(&zs, 0, sizeof(zs));
        if (deflateInit2(&zs, lvl, Z_DEFLATED, wb, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
        inited = true; level = lvl; wbits = wb;
        return true;
    }
};

struct ByteBuf {
    uint8_t* p = nullptr;
    size_t cap = 0;
    ByteBuf() = default;
    ByteBuf(const ByteBuf&) = delete;
    ByteBuf& operator=(const ByteBuf&) = delete;
    ~ByteBuf() { std::free(p); }
    bool reserve(size_t n) {                 
        if (n <= cap) return true;
        size_t nc = std::max(n, cap + cap / 2);
        uint8_t* np = static_cast<uint8_t*>(std::realloc(p, nc));
        if (!np) { nc = n; np = static_cast<uint8_t*>(std::realloc(p, nc)); if (!np) return false; }
        p = np; cap = nc;
        return true;
    }
    bool fresh(size_t n) {                  
        if (n <= cap) return true;
        std::free(p); p = nullptr; cap = 0;
        return reserve(n);
    }
    void release() { std::free(p); p = nullptr; cap = 0; }
};

class OutWriter {
public:
    OutWriter(FileHandle h, size_t cap) : h_(h), max_cap_(cap), used_(0), ok_(true) {
        buf_.reserve(cap); 
    }
    
    bool ok() const { return ok_; }
    
    bool write(const void* data, uint64_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        while (n > 0 && ok_) {
            if (used_ == 0 && n >= max_cap_) { 
                ok_ = raw_write(p, n); 
                return ok_; 
            }
            
            size_t take = static_cast<size_t>(std::min<uint64_t>(max_cap_ - used_, n));
            
            if (used_ + take > buf_.size()) {
                buf_.resize(used_ + take);
            }
            
            std::memcpy(buf_.data() + used_, p, take);
            used_ += take; p += take; n -= take;
            if (used_ == max_cap_) flush();
        }
        return ok_;
    }
    
    uint8_t* space(size_t& avail) {
        if (used_ == max_cap_) flush();
        
        if (buf_.size() < max_cap_) {
            buf_.resize(max_cap_);
        }
        
        avail = max_cap_ - used_;
        return buf_.data() + used_;
    }
    
    void commit(size_t n) { used_ += n; }
    
    bool flush() {
        if (used_ > 0 && ok_) ok_ = raw_write(buf_.data(), used_);
        used_ = 0;
        return ok_;
    }

private:
    bool raw_write(const uint8_t* ptr, uint64_t n) { return write_all(h_, ptr, n); }
    FileHandle h_;
    std::vector<uint8_t> buf_;
    size_t max_cap_; 
    size_t used_ = 0;
    bool ok_ = true;
};

// ============================================================ ENCODER

struct Rec {
    uint64_t offset;    
    uint64_t dec_size;
    uint32_t comp_size;
    uint8_t level;
    int8_t wbits;
};

struct Slot {
    ByteBuf buf;            
    uint64_t base = 0;
    size_t len = 0;
    int64_t seg = -1;
    bool ready = false;
    std::vector<Rec> recs;  
};

enum VerifyResult { VR_MATCH = 1, VR_MISMATCH = 0, VR_INVALID = -1 };

class Scanner {
public:
    Scanner(FileHandle h, uint64_t total) : h_(h), total_(total), dec_(CHUNK), cmp_(CHUNK) {
        last_win_[0] = 1; last_win_[1] = 5; last_win_[2] = 6; last_win_[3] = 9; last_win_[4] = 6;
    }
    bool io_error() const { return io_error_; }

    bool load(Slot& sl, uint64_t S, uint64_t E) {
        sl.base = S;
        size_t want = static_cast<size_t>(std::min<uint64_t>(total_ - S, (E - S) + LOOKAHEAD));
        if (!sl.buf.fresh(want)) { io_error_ = true; return false; }
        if (!pread_exact(h_, sl.buf.p, want, S)) { io_error_ = true; return false; }
        sl.len = want;
        return true;
    }

    bool scan(Slot& sl, uint64_t seg_end) {
        sl.recs.clear();
        uint64_t pos = sl.base;
        while (pos < seg_end) {
            const uint8_t* b = sl.buf.p;
            size_t i = static_cast<size_t>(pos - sl.base);
            const size_t lim = static_cast<size_t>(seg_end - sl.base);
            while (i < lim && b[i] != 0x78 && b[i] != 's') ++i;
            pos = sl.base + i;
            if (pos >= seg_end) break;

            int wbits = 0, flg = 0;
            uint64_t so = 0;
            if (b[i] == 0x78) {
                if (i + 1 < sl.len) {
                    flg = b[i + 1];
                    if (flg == 0x01 || flg == 0x5E || flg == 0x9C || flg == 0xDA) { wbits = 15; so = pos; }
                }
            } else if (i + 6 < sl.len && std::memcmp(b + i, "stream", 6) == 0) {
                size_t sp = i + 6;
                const size_t spmax = std::min(i + 10, sl.len);
                while (sp < spmax && (b[sp] == '\r' || b[sp] == '\n')) ++sp;
                wbits = -15;
                so = sl.base + sp;
            }

            if (wbits != 0) {
                Rec r;
                if (try_stream(sl, so, wbits, flg, r)) {
                    sl.recs.push_back(r);
                    g_streams_found.fetch_add(1, std::memory_order_relaxed);
                    pos = r.offset + r.comp_size;
                    continue;
                }
                if (io_error_) return false;
            }
            ++pos;
        }
        return true;
    }

private:
    bool extend(Slot& sl, uint64_t need_end) {
        uint64_t have_end = sl.base + sl.len;
        if (need_end <= have_end) return true;
        if (have_end >= total_) return false;
        uint64_t new_end = std::min<uint64_t>(total_, std::max<uint64_t>(need_end, have_end + EXT_STEP));
        if (!sl.buf.reserve(static_cast<size_t>(new_end - sl.base))) { io_error_ = true; return false; }
        if (!pread_exact(h_, sl.buf.p + sl.len, static_cast<size_t>(new_end - have_end), have_end)) { io_error_ = true; return false; }
        sl.len = static_cast<size_t>(new_end - sl.base);
        return (sl.base + sl.len) >= need_end;
    }

    int build_levels(int wbits, int flg, int* out) {
        int n = 0;
        auto add = [&](int l) { for (int i = 0; i < n; ++i) if (out[i] == l) return; out[n++] = l; };
        if (wbits > 0) {

            int cls = (flg == 0x01) ? 0 : (flg == 0x5E) ? 1 : (flg == 0x9C) ? 2 : 3;
            cur_class_ = cls;
            add(last_win_[cls]);
            switch (cls) {
                case 0: add(1); break;
                case 1: add(5); add(4); add(3); add(2); break;
                case 2: add(6); break;
                default: add(9); add(8); add(7); break;
            }
#if NITRO_TRY_ALL_LEVELS
            add(6); add(1); add(9);
#endif
        } else {
            cur_class_ = 4;
            add(last_win_[4]); add(6); add(1); add(9);
        }
        return n;
    }

    bool try_stream(Slot& sl, uint64_t so, int wbits, int flg, Rec& out) {
        int lv[16];
        int n = build_levels(wbits, flg, lv);
        for (int k = 0; k < n; ++k) {
            int r = verify_level(sl, so, wbits, lv[k], out);
            if (r == VR_MATCH) { last_win_[cur_class_] = static_cast<uint8_t>(lv[k]); return true; }
            if (r == VR_INVALID) return false;  
        }
        return false;
    }


    int verify_level(Slot& sl, uint64_t so, int wbits, int level, Rec& out) {
        const uint64_t limit_end = std::min<uint64_t>(total_, so + MAX_COMP_STREAM);
        if (!inf_.begin(wbits)) return VR_INVALID;
        z_stream& is = inf_.zs;

        bool def_ready = false;
        uint64_t cmp_pos = 0;
        uint64_t dec_total = 0;
        size_t fill = 0;

        auto feed = [&]() -> bool {
            uint64_t c = so + is.total_in;
            if (c >= limit_end) return false;
            if (c >= sl.base + sl.len && !extend(sl, c + 1)) return false;
            uint64_t have_end = std::min<uint64_t>(sl.base + sl.len, limit_end);
            is.next_in = sl.buf.p + (c - sl.base);
            is.avail_in = static_cast<uInt>(have_end - c);
            return true;
        };

        auto cmp_out = [&](size_t got) -> bool {
            uint64_t a = so + cmp_pos;
            if (a + got > limit_end) return false;
            if (a + got > sl.base + sl.len && !extend(sl, a + got)) return false;
            if (std::memcmp(sl.buf.p + (a - sl.base), cmp_.data(), got) != 0) return false;
            cmp_pos += got;
            return true;
        };
        auto def_begin = [&]() -> bool {
            if (def_ready) return true;
            if (!def_.begin(level, wbits)) return false;
            def_ready = true;
            return true;
        };
        auto feed_deflate = [&](size_t n) -> bool {
            if (!def_begin()) return false;
            z_stream& ds = def_.zs;
            ds.next_in = dec_.data();
            ds.avail_in = static_cast<uInt>(n);
            do {
                ds.next_out = cmp_.data();
                ds.avail_out = static_cast<uInt>(CHUNK);
                int r = deflate(&ds, Z_NO_FLUSH);
                if (r != Z_OK && r != Z_BUF_ERROR) return false;
                size_t got = CHUNK - ds.avail_out;
                if (got && !cmp_out(got)) return false;
            } while (ds.avail_in > 0 || ds.avail_out == 0);
            return true;
        };

        bool ended = false;
        while (!ended) {
            if (!feed()) return VR_INVALID;
            is.next_out = dec_.data() + fill;
            is.avail_out = static_cast<uInt>(CHUNK - fill);
            int r = inflate(&is, Z_NO_FLUSH);
            size_t produced = (CHUNK - fill) - is.avail_out;
            fill += produced;
            dec_total += produced;
            if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) return VR_INVALID;
            if (r == Z_BUF_ERROR && produced == 0 && is.avail_in > 0) return VR_INVALID;
            if (dec_total > MAX_DEC_STREAM) return VR_INVALID;
            ended = (r == Z_STREAM_END);
            if (ended && dec_total < MIN_DECOMPRESS_SIZE) return VR_INVALID;
            if (fill == CHUNK || (ended && fill > 0)) {
                if (!feed_deflate(fill)) return VR_MISMATCH;
                fill = 0;
            }
        }

        const uint64_t consumed = is.total_in;
        if (!def_begin()) return VR_INVALID;
        z_stream& ds = def_.zs;
        ds.next_in = nullptr;
        ds.avail_in = 0;
        int r;
        do {
            ds.next_out = cmp_.data();
            ds.avail_out = static_cast<uInt>(CHUNK);
            r = deflate(&ds, Z_FINISH);
            if (r != Z_OK && r != Z_STREAM_END) return VR_MISMATCH;
            size_t got = CHUNK - ds.avail_out;
            if (got && !cmp_out(got)) return VR_MISMATCH;
        } while (r == Z_OK);

        if (cmp_pos != consumed) return VR_MISMATCH;

        out.offset = so;
        out.dec_size = dec_total;
        out.comp_size = static_cast<uint32_t>(consumed);
        out.level = static_cast<uint8_t>(level);
        out.wbits = static_cast<int8_t>(wbits);
        return VR_MATCH;
    }

    FileHandle h_;
    uint64_t total_;
    bool io_error_ = false;
    Inflater inf_;
    Deflater def_;
    std::vector<uint8_t> dec_, cmp_;
    uint8_t last_win_[5];
    int cur_class_ = 2;
};

static bool inflate_to_out(Inflater& inf, const uint8_t* src, uint32_t csize, int wbits, uint64_t dsize, OutWriter& out) {
    if (!inf.begin(wbits)) return false;
    inf.zs.next_in = const_cast<Bytef*>(src);
    inf.zs.avail_in = csize;
    uint64_t produced_total = 0;
    int r;
    do {
        size_t avail = 0;
        uint8_t* dst = out.space(avail);
        if (!out.ok()) return false;
        inf.zs.next_out = dst;
        inf.zs.avail_out = static_cast<uInt>(avail);
        r = inflate(&inf.zs, Z_NO_FLUSH);
        size_t got = avail - inf.zs.avail_out;
        out.commit(got);
        produced_total += got;
        if (r != Z_OK && r != Z_STREAM_END) return false;
    } while (r != Z_STREAM_END);
    return produced_total == dsize && inf.zs.total_in == csize;
}

static int do_encode(const PathStr& in_path, const PathStr& out_path, int num_threads) {
    auto encode_start_time = Clock::now();
    FileHandle hIn = file_open_read(in_path, true);
    if (hIn == BAD_FILE) {
        std::cerr << "Error: Cannot open input file.\n";
        return 1;
    }
    uint64_t total = 0;
    if (!file_size(hIn, total)) {
        std::cerr << "Error: Cannot get input file size.\n";
        file_close(hIn);
        return 1;
    }

    FileHandle hOut = file_open_write(out_path);
    if (hOut == BAD_FILE) {
        std::cerr << "Error: Cannot create output file.\n";
        file_close(hIn);
        return 1;
    }

    OutWriter out(hOut, OUT_BUF_SIZE);
    GeneralPrecompHeader h{};
    h.original_size = total;
    out.write(&h, sizeof(h));

    uint64_t seg_size = total / (static_cast<uint64_t>(num_threads) * 8);
    seg_size = std::max(SEG_MIN, std::min(SEG_MAX, seg_size));
    seg_size &= ~static_cast<uint64_t>(0xFFFF);
    const int64_t nseg = static_cast<int64_t>((total + seg_size - 1) / seg_size);
    int64_t W = static_cast<int64_t>(num_threads) * 2 + 2;
    if (W > nseg) W = nseg;
    if (W < 1) W = 1;
    std::vector<Slot> slots(static_cast<size_t>(W));

    ProgressGuard prog("Nitro-E", total);

    std::mutex mtx;
    std::condition_variable cv;
    int64_t next_seg = 0, writer_seg = 0;
    bool failed = false;

    auto worker = [&]() {
        Scanner sc(hIn, total);
        while (true) {
            int64_t seg;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [&] { return failed || next_seg >= nseg || next_seg < writer_seg + W; });
                if (failed || next_seg >= nseg) return;
                seg = next_seg++;
            }
            Slot& sl = slots[static_cast<size_t>(seg % W)];
            const uint64_t S = static_cast<uint64_t>(seg) * seg_size;
            const uint64_t E = std::min(S + seg_size, total);
            bool ok = sc.load(sl, S, E) && sc.scan(sl, E);
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (ok) { sl.seg = seg; sl.ready = true; }
                else failed = true;
            }
            cv.notify_all();
            if (!ok) return;
        }
    };

    std::vector<std::thread> workers;
    const int nthreads = static_cast<int>(std::min<int64_t>(num_threads, std::max<int64_t>(nseg, 1)));
    for (int i = 0; i < nthreads && nseg > 0; ++i) workers.emplace_back(worker);

    std::vector<StreamMetadata> table;
    Inflater winf;
    uint64_t cur = 0;        
    uint64_t exp_off = 0;    
    bool werr = false;
    const char* errmsg = nullptr;

    for (int64_t s = 0; s < nseg && !werr; ++s) {
        Slot& sl = slots[static_cast<size_t>(s % W)];
        {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [&] { return failed || (sl.ready && sl.seg == s); });
            if (!(sl.ready && sl.seg == s)) { werr = true; errmsg = "Error: read/alloc failure while scanning."; break; }
        }
        const uint64_t E = std::min(static_cast<uint64_t>(s) * seg_size + seg_size, total);

        for (const Rec& r : sl.recs) {
            if (r.offset < cur) continue;   
            if (r.offset > cur) {
                uint64_t gap = r.offset - cur;
                if (!out.write(sl.buf.p + (cur - sl.base), gap)) { werr = true; errmsg = "Error: write failure (disk full?)."; break; }
                exp_off += gap; cur = r.offset;
                g_write_ptr.store(cur, std::memory_order_relaxed);
            }
            StreamMetadata m;
            m.offset = exp_off; m.dec_size = r.dec_size; m.comp_size = r.comp_size; m.level = r.level; m.windowBits = r.wbits;
            if (!inflate_to_out(winf, sl.buf.p + (r.offset - sl.base), r.comp_size, r.wbits, r.dec_size, out)) {
                werr = true;
                errmsg = out.ok() ? "Error: internal inconsistency while re-inflating a verified stream." : "Error: write failure (disk full?).";
                break;
            }
            exp_off += r.dec_size; cur += r.comp_size;
            table.push_back(m);
            g_write_ptr.store(cur, std::memory_order_relaxed);
        }
        if (!werr && cur < E) {
            uint64_t tail = E - cur;
            if (!out.write(sl.buf.p + (cur - sl.base), tail)) { werr = true; errmsg = "Error: write failure (disk full?)."; }
            else { exp_off += tail; cur = E; g_write_ptr.store(cur, std::memory_order_relaxed); }
        }
        {
            std::lock_guard<std::mutex> lk(mtx);
            sl.ready = false;
            sl.recs.clear();
            sl.buf.release();
            writer_seg = s + 1;
        }
        cv.notify_all();
    }

    if (werr) {
        { std::lock_guard<std::mutex> lk(mtx); failed = true; }
        cv.notify_all();
    }
    for (auto& t : workers) t.join();

    bool success = !werr;
    if (success) {
        h.num_streams = static_cast<uint32_t>(table.size());
        h.expanded_size = exp_off;
        out.write(table.data(), static_cast<uint64_t>(table.size()) * sizeof(StreamMetadata));
        out.flush();
        if (out.ok()) {
            if (!write_at_start(hOut, &h, sizeof(h))) success = false;
        } else success = false;
        if (!success) errmsg = "Error: write failure (disk full?).";
    }

    prog.finish(success, total);
    file_close(hOut);
    file_close(hIn);

    if (!success) {
        std::cerr << (errmsg ? errmsg : "Error: encode failed.") << "\n";
        return 2;
    }

    uint64_t total_dec_sum = 0;
    uint64_t level_count[10] = {0};
    for (const auto& sm : table) { total_dec_sum += sm.dec_size; if (sm.level < 10) level_count[sm.level]++; }
    std::string levels_str;
    for (int l = 0; l < 10; ++l) if (level_count[l]) { if (!levels_str.empty()) levels_str += ", "; levels_str += std::to_string(l) + " (x" + std::to_string(level_count[l]) + ")"; }
    uint64_t diff_res = (h.expanded_size > total_dec_sum) ? (h.expanded_size - total_dec_sum) : 0;
    double diff_ratio = (total > 0) ? (static_cast<double>(diff_res) / total * 100.0) : 0.0;
    uint64_t peak = std::max(g_peak_mem_usage.load(std::memory_order_relaxed), peak_ram_bytes());

    auto elapsed = std::chrono::duration<double>(Clock::now() - encode_start_time).count();
    std::cout << "\n[OK] Precomp saved successfully! v1.09 (segmented, bounded RAM)\n"
              << "    Streams   : " << table.size() << "\n"
              << "    Original  : " << total << " bytes (" << (total / 1048576.0) << " MB)\n"
              << "    Expanded  : " << h.expanded_size << " bytes (" << (h.expanded_size / 1048576.0) << " MB)\n"
              << "    Diff/Res  : " << diff_res << " bytes - " << std::fixed << std::setprecision(1) << diff_ratio << "% of original\n"
              << "    Levels    : " << levels_str << "\n"
              << "    Peak RAM  : " << std::fixed << std::setprecision(1) << (peak / 1048576.0) << " MB\n"
              << "    Peak Speed: " << std::fixed << std::setprecision(2) << g_peak_speed.load(std::memory_order_relaxed) << " MB/s\n"
              << "    Total time: " << std::fixed << std::setprecision(1) << elapsed << " seconds\n";
    return 0;
}

// ============================================================ DECODER

static int do_decode(const PathStr& in_path, const PathStr& out_path, int num_threads) {
    auto decode_start_time = Clock::now();
    FileHandle hIn = file_open_read(in_path, true);
    if (hIn == BAD_FILE) { std::cerr << "Error opening input file.\n"; return 1; }
    uint64_t total_size = 0;
    if (!file_size(hIn, total_size)) { std::cerr << "Error getting input file size.\n"; file_close(hIn); return 1; }

    GeneralPrecompHeader h{};
    if (total_size < sizeof(h) || !pread_exact(hIn, &h, sizeof(h), 0)) {
        std::cerr << "Error reading header.\n";
        file_close(hIn); return 1;
    }
    if (h.magic[0] != 'P' || h.magic[1] != 'R' || h.magic[2] != 'E' || h.magic[3] != 'C') {
        std::cerr << "Invalid header magic.\n";
        file_close(hIn); return 1;
    }

    const uint64_t meta_offset = sizeof(h) + h.expanded_size;
    if (meta_offset > total_size || (total_size - meta_offset) < static_cast<uint64_t>(h.num_streams) * sizeof(StreamMetadata)) {
        std::cerr << "Error: metadata table out of range (truncated file?).\n";
        file_close(hIn); return 1;
    }
    std::vector<StreamMetadata> streams(h.num_streams);
    if (h.num_streams > 0 && !pread_exact(hIn, streams.data(), static_cast<size_t>(h.num_streams) * sizeof(StreamMetadata), meta_offset)) {
        std::cerr << "Error reading metadata table.\n";
        file_close(hIn); return 1;
    }
    {
        uint64_t prev_end = 0;
        for (const auto& sm : streams) {
            if (sm.offset < prev_end || sm.offset + sm.dec_size > h.expanded_size) {
                std::cerr << "Error: corrupt metadata table.\n";
                file_close(hIn); return 1;
            }
            prev_end = sm.offset + sm.dec_size;
        }
    }

    FileHandle hOut = file_open_write(out_path);
    if (hOut == BAD_FILE) {
        std::cerr << "Error creating output file.\n";
        file_close(hIn); return 1;
    }

    size_t dynamic_buf_size = (num_threads <= 2) ? (2u << 20) : (16u << 20);
    OutWriter out(hOut, dynamic_buf_size);

    ProgressGuard prog("Nitro-D", h.original_size);

    const int64_t n = static_cast<int64_t>(streams.size());

    const uint64_t BUDGET = (num_threads <= 2) ? (16ULL << 20) : (128ULL << 20);
    const int64_t MAXWIN = (num_threads <= 2) ? 4 : 512;                        
    std::mutex mtx;
    std::condition_variable cv;
    std::map<int64_t, std::vector<uint8_t>> finished;
    int64_t next_task = 0, write_id = 0;
    uint64_t inflight = 0;
    bool failed = false;
    const char* errmsg = nullptr;

    auto worker = [&]() {
        Deflater def;
        std::vector<uint8_t> rbuf(1u << 20);  
        while (true) {
            int64_t task;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [&] {
                    return failed || next_task >= n || next_task == write_id ||
                           (next_task < write_id + MAXWIN && inflight + streams[static_cast<size_t>(next_task)].comp_size <= BUDGET);
                });
                if (failed || next_task >= n) return;
                task = next_task++;
                inflight += streams[static_cast<size_t>(task)].comp_size;
            }
            const StreamMetadata sm = streams[static_cast<size_t>(task)];
            std::vector<uint8_t> rec(static_cast<size_t>(sm.comp_size) + 64);
            bool ok = def.begin(sm.level, sm.windowBits);
            if (ok) {
                z_stream& zs = def.zs;
                zs.next_out = rec.data();
                zs.avail_out = static_cast<uInt>(rec.size());
                uint64_t remaining = sm.dec_size;
                uint64_t off = sizeof(GeneralPrecompHeader) + sm.offset;
                while (ok && remaining > 0) {
                    size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, rbuf.size()));
                    if (!pread_exact(hIn, rbuf.data(), chunk, off)) { ok = false; break; }
                    off += chunk; remaining -= chunk;
                    for (size_t p = 0; p < chunk; p += CHUNK) {
                        zs.next_in = rbuf.data() + p;
                        zs.avail_in = static_cast<uInt>(std::min(CHUNK, chunk - p));
                        int r = deflate(&zs, Z_NO_FLUSH);
                        if ((r != Z_OK && r != Z_BUF_ERROR) || zs.avail_in != 0) { ok = false; break; }
                    }
                }
                if (ok) {
                    zs.next_in = nullptr;
                    zs.avail_in = 0;
                    int r = deflate(&zs, Z_FINISH);
                    ok = (r == Z_STREAM_END) && (zs.total_out == sm.comp_size);
                }
                if (ok) rec.resize(sm.comp_size);
            }
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (ok) finished[task] = std::move(rec);
                else { failed = true; errmsg = "Error: a stream could not be re-compressed to its original size."; }
            }
            cv.notify_all();
            if (!ok) return;
        }
    };

    std::vector<std::thread> workers;
    for (int i = 0; i < num_threads && n > 0; ++i) workers.emplace_back(worker);

    std::vector<uint8_t> gapbuf(4u << 20);
    auto copy_gap = [&](uint64_t src, uint64_t len) -> bool {
        while (len > 0) {
            size_t c = static_cast<size_t>(std::min<uint64_t>(len, gapbuf.size()));
            if (!pread_exact(hIn, gapbuf.data(), c, sizeof(h) + src)) return false;
            if (!out.write(gapbuf.data(), c)) return false;
            src += c; len -= c;
        }
        return true;
    };

    uint64_t current_data_ptr = 0, current_out_ptr = 0;
    bool werr = false;

    for (int64_t i = 0; i < n && !werr; ++i) {
        const StreamMetadata& sm = streams[static_cast<size_t>(i)];
        if (sm.offset > current_data_ptr) {
            uint64_t gap = sm.offset - current_data_ptr;
            if (!copy_gap(current_data_ptr, gap)) { werr = true; errmsg = "Error: I/O failure copying raw data."; break; }
            current_out_ptr += gap;
            current_data_ptr = sm.offset;
            g_write_ptr.store(current_out_ptr, std::memory_order_relaxed);
        }
        std::vector<uint8_t> rec;
        {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [&] { return failed || finished.count(i) > 0; });
            auto it = finished.find(i);
            if (it == finished.end()) { werr = true; break; }
            rec = std::move(it->second);
            finished.erase(it);
            write_id = i + 1;
            inflight -= sm.comp_size;
        }
        cv.notify_all();
        if (!out.write(rec.data(), rec.size())) { werr = true; errmsg = "Error: write failure (disk full?)."; break; }
        current_out_ptr += rec.size();
        current_data_ptr += sm.dec_size;
        g_write_ptr.store(current_out_ptr, std::memory_order_relaxed);
        g_streams_found.fetch_add(1, std::memory_order_relaxed);
    }

    if (!werr && current_data_ptr < h.expanded_size) {
        uint64_t tail = h.expanded_size - current_data_ptr;
        if (!copy_gap(current_data_ptr, tail)) { werr = true; errmsg = "Error: I/O failure copying raw data."; }
        else current_out_ptr += tail;
    }

    if (werr) {
        { std::lock_guard<std::mutex> lk(mtx); failed = true; }
        cv.notify_all();
    }
    for (auto& t : workers) t.join();

    bool success = !werr && out.flush();
    if (success && current_out_ptr != h.original_size) {
        std::cerr << "\nWarning: restored size (" << current_out_ptr << ") differs from the size in the header (" << h.original_size << ").\n";
    }
    prog.finish(success, h.original_size);
    file_close(hOut);
    file_close(hIn);

    if (!success) {
        std::cerr << (errmsg ? errmsg : "Error: decode failed.") << "\n";
        return 2;
    }

    uint64_t peak = std::max(g_peak_mem_usage.load(std::memory_order_relaxed), peak_ram_bytes());
    auto elapsed = std::chrono::duration<double>(Clock::now() - decode_start_time).count();
    std::cout << "\n[OK] Restored successfully! v1.09 (streaming, bounded RAM)\n"
              << "    Streams   : " << streams.size() << "\n"
              << "    Original  : " << h.original_size << " bytes (" << (h.original_size / 1048576.0) << " MB)\n"
              << "    Peak RAM  : " << std::fixed << std::setprecision(1) << (peak / 1048576.0) << " MB\n"
              << "    Total time: " << std::fixed << std::setprecision(1) << elapsed << " seconds\n";
    return 0;
}


static int app_main(const std::vector<PathStr>& args) {
    const int argc = static_cast<int>(args.size());
    std::cout << TITLE << "\n";
    std::setvbuf(stdout, NULL, _IONBF, 0);
    std::setvbuf(stderr, NULL, _IONBF, 0);

    std::ios_base::sync_with_stdio(false);
#ifdef _WIN32
    std::setlocale(LC_ALL, ".UTF-8");
#endif

    if (argc < 4) {
        if (argc >= 2) {
            PathStr check_mode = args[1];
            if (check_mode != T_("e") && check_mode != T_("d")) {
                std::cerr << "Error: Unknown mode '" << std::string(check_mode.begin(), check_mode.end()) << "'. Use 'e' (Encode) or 'd' (Decode).\n";
            } else {
                std::cerr << "Error: Missing required arguments (<input> or <o>).\n";
            }
        } else {
            std::cerr << "Error: No arguments provided.\n";
        }

        std::cout << "\nUsage:\n";
        std::cout << "  Encode:  nitro_ng e <input> <output.pre> [-j threads]\n";
        std::cout << "  Decode:  nitro_ng d <input.pre> <restored> [-j threads]\n";
        return 1;
    }

    PathStr mode = args[1];
    PathStr in_path = args[2];
    PathStr out_path = args[3];

    int max_hardware_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (max_hardware_threads < 1) max_hardware_threads = 1;

    int threads = max_hardware_threads;

    try {
        for (int i = 4; i < argc; ++i) {
            PathStr arg = args[i];
            if (arg == T_("-j")) {
                if (i + 1 < argc) {
                    int user_threads = std::stoi(args[++i]);

                    if (user_threads > max_hardware_threads) {
                        std::cerr << "Warning: Requested " << user_threads
                                  << " threads, but system only supports " << max_hardware_threads
                                  << ". Limiting to " << max_hardware_threads << ".\n";
                        threads = max_hardware_threads;
                    } else if (user_threads < 1) {
                        threads = 1;
                    } else {
                        threads = user_threads;
                    }
                } else {
                    std::cerr << "Error: Missing numeric value for -j\n";
                    return 1;
                }
            } else {
                std::cerr << "Error: Unknown option: '" << std::string(arg.begin(), arg.end()) << "'\n";
                return 1;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error in the arguments: " << e.what() << "\n";
        return 1;
    }

    if (threads < 1) threads = 1;

    bool is_encode = (mode == T_("e"));
    bool is_decode = (mode == T_("d"));

    if (!is_encode && !is_decode) {
        std::cerr << "Error: Unknown mode. Use 'e' or 'd'.\n";
        return 1;
    }

    std::cout << "Number of worker threads: " << threads << "\n";

    init_zlib_dict_cache(static_cast<size_t>(threads));

    return is_encode ? do_encode(in_path, out_path, threads)
                     : do_decode(in_path, out_path, threads);
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    return app_main(std::vector<PathStr>(argv, argv + argc));
}
#else
int main(int argc, char* argv[]) {
    return app_main(std::vector<PathStr>(argv, argv + argc));
}
#endif
