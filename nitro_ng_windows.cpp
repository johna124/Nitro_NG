#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <condition_variable>
#include <memory>
#include <optional>

// Windows Specific Headers
#include <windows.h>
#include <io.h>
#include <zlib.h>

#define TITLE "Nitro NG 1.05 HP Windows"
#define MIN_DECOMPRESS_SIZE 512ULL 
#define NITRO_CACHE_SIZE 65536
#define ZLIB_DICT_CACHE_SIZE 8192

using Clock = std::chrono::high_resolution_clock;
static Clock::time_point start_time_global;
static std::atomic<uint64_t> g_write_ptr{0};
static std::atomic<uint32_t> g_streams_found{0};
static std::atomic<bool> g_global_done{false};
static std::atomic<uint64_t> g_current_mem_usage{0};
static std::atomic<uint64_t> g_cache_hits{0};
static std::atomic<uint64_t> g_dict_reuse{0};
static std::atomic<uint64_t> g_dict_fallbacks{0};

// ==================== LOCK-FREE CACHE SYSTEM ====================
struct NitroCacheEntry {
    uint8_t matching_level;
    int8_t window_bits;
    std::atomic<bool> is_valid{false};
    
    NitroCacheEntry() : matching_level(0), window_bits(0) {}
    NitroCacheEntry(const NitroCacheEntry& o) {
        matching_level = o.matching_level;
        window_bits = o.window_bits;
        is_valid.store(o.is_valid.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
};

static NitroCacheEntry g_nitro_intel_cache[NITRO_CACHE_SIZE];

inline uint16_t generate_nitro_fingerprint(const uint8_t* compressed_data) {
    uint64_t h;
    std::memcpy(&h, compressed_data, sizeof(uint64_t));
    h ^= 0xcbf29ce484222325ULL;
    h *= 0x100000001b3ULL;
    return static_cast<uint16_t>(h & 0xFFFF);
}

// ==================== ROBUST ZLIB DICTIONARY CACHE ====================
struct ZlibDictSlot {
    z_stream stream;
    int8_t window_bits;
    uint8_t level;
    std::atomic<bool> in_use{false};
    std::atomic<uint64_t> last_used{0};
    
    ZlibDictSlot() : window_bits(0), level(0) {
        memset(&stream, 0, sizeof(stream));
    }
    
    ~ZlibDictSlot() {
        if (stream.state) deflateEnd(&stream);
    }
    
    ZlibDictSlot(const ZlibDictSlot&) = delete;
    ZlibDictSlot& operator=(const ZlibDictSlot&) = delete;
};

class ZlibDictCache {
private:
    static const size_t NUM_SHARDS = 16;
    static const size_t SLOTS_PER_SHARD = ZLIB_DICT_CACHE_SIZE / NUM_SHARDS;
    
    struct CacheShard {
        ZlibDictSlot slots[SLOTS_PER_SHARD];
        std::atomic<uint64_t> global_counter{0};
        char padding[64];
    };
    
    CacheShard shards[NUM_SHARDS];
    std::mutex fallback_mutex;
    
    size_t get_shard_index(int window_bits, uint8_t level) const {
        return ((window_bits * 31) ^ (level * 65521)) % NUM_SHARDS;
    }
    
public:
    ZlibDictCache() {
        for (size_t s = 0; s < NUM_SHARDS; ++s) {
            for (size_t i = 0; i < SLOTS_PER_SHARD; ++i) {
                memset(&shards[s].slots[i].stream, 0, sizeof(z_stream));
            }
        }
    }
    
    ~ZlibDictCache() {
        for (size_t s = 0; s < NUM_SHARDS; ++s) {
            for (size_t i = 0; i < SLOTS_PER_SHARD; ++i) {
                if (shards[s].slots[i].stream.state) {
                    deflateEnd(&shards[s].slots[i].stream);
                }
            }
        }
    }
    
    z_stream* acquire_stream(int window_bits, uint8_t level) {
        size_t shard_idx = get_shard_index(window_bits, level);
        CacheShard& shard = shards[shard_idx];
        
        for (size_t attempt = 0; attempt < SLOTS_PER_SHARD; ++attempt) {
            ZlibDictSlot& slot = shard.slots[attempt];
            bool expected = false;
            
            if (slot.in_use.compare_exchange_strong(expected, true,
                std::memory_order_acquire, std::memory_order_relaxed)) {
                
                if (slot.window_bits == window_bits && 
                    slot.level == level && 
                    slot.stream.state) {
                    deflateReset(&slot.stream);
                    slot.last_used.store(shard.global_counter.fetch_add(1), std::memory_order_release);
                    return &slot.stream;
                }
                
                if (slot.stream.state) {
                    deflateEnd(&slot.stream);
                    memset(&slot.stream, 0, sizeof(z_stream));
                }
                
                if (deflateInit2(&slot.stream, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
                    slot.window_bits = window_bits;
                    slot.level = level;
                    slot.last_used.store(shard.global_counter.fetch_add(1), std::memory_order_release);
                    return &slot.stream;
                }
                
                slot.in_use.store(false, std::memory_order_release);
                return nullptr;
            }
        }
        
        uint64_t oldest_time = UINT64_MAX;
        size_t oldest_idx = 0;
        
        for (size_t i = 0; i < SLOTS_PER_SHARD; ++i) {
            ZlibDictSlot& slot = shard.slots[i];
            uint64_t last_used = slot.last_used.load(std::memory_order_acquire);
            
            if (last_used < oldest_time) {
                bool expected = false;
                if (slot.in_use.compare_exchange_strong(expected, true,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                    if (slot.stream.state) {
                        deflateEnd(&slot.stream);
                        memset(&slot.stream, 0, sizeof(z_stream));
                    }
                    
                    if (deflateInit2(&slot.stream, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
                        slot.window_bits = window_bits;
                        slot.level = level;
                        slot.last_used.store(shard.global_counter.fetch_add(1), std::memory_order_release);
                        return &slot.stream;
                    }
                    
                    slot.in_use.store(false, std::memory_order_release);
                }
                oldest_time = last_used;
                oldest_idx = i;
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(fallback_mutex);
            g_dict_fallbacks++;
            
            ZlibDictSlot& slot = shard.slots[oldest_idx];
            bool expected = false;
            if (slot.in_use.compare_exchange_strong(expected, true,
                std::memory_order_acquire, std::memory_order_relaxed)) {
                
                if (slot.stream.state) {
                    deflateEnd(&slot.stream);
                    memset(&slot.stream, 0, sizeof(z_stream));
                }
                
                if (deflateInit2(&slot.stream, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
                    slot.window_bits = window_bits;
                    slot.level = level;
                    slot.last_used.store(shard.global_counter.fetch_add(1), std::memory_order_release);
                    return &slot.stream;
                }
            }
        }
        
        z_stream* emergency_stream = new z_stream();
        memset(emergency_stream, 0, sizeof(z_stream));
        
        if (deflateInit2(emergency_stream, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
            return emergency_stream;
        }
        
        delete emergency_stream;
        return nullptr;
    }
    
    void release_stream(z_stream* stream) {
        if (!stream) return;
        
        for (size_t s = 0; s < NUM_SHARDS; ++s) {
            for (size_t i = 0; i < SLOTS_PER_SHARD; ++i) {
                if (&shards[s].slots[i].stream == stream) {
                    std::atomic_thread_fence(std::memory_order_release);
                    shards[s].slots[i].in_use.store(false, std::memory_order_release);
                    return;
                }
            }
        }
        
        if (stream->state) {
            deflateEnd(stream);
        }
        delete stream;
    }
};

static ZlibDictCache g_dict_cache;

// ==================== OPTIMIZED BUFFER POOL ====================
class BufferPool {
private:
    std::vector<std::vector<uint8_t>> buffers;
    std::mutex mtx;
    
public:
    size_t default_size;
    
    BufferPool(size_t size, int count) : default_size(size) {
        buffers.reserve(count);
        for (int i = 0; i < count; ++i) {
            buffers.emplace_back(size);
        }
    }
    
    std::vector<uint8_t> acquire() {
        std::lock_guard<std::mutex> lock(mtx);
        if (!buffers.empty()) {
            auto buf = std::move(buffers.back());
            buffers.pop_back();
            return buf;
        }
        return std::vector<uint8_t>(default_size);
    }
    
    void release(std::vector<uint8_t>& buf) {
        if (buf.capacity() >= default_size) {
            buf.clear();
            std::lock_guard<std::mutex> lock(mtx);
            if (buffers.size() < 64) {
                buffers.push_back(std::move(buf));
            }
        }
    }
};

bool portable_pread(HANDLE hFile, void* buffer, uint32_t size, uint64_t offset) {
    OVERLAPPED overlapped = {0};
    overlapped.Offset = (DWORD)(offset & 0xFFFFFFFF);
    overlapped.OffsetHigh = (DWORD)(offset >> 32);
    DWORD bytesRead;
    return ReadFile(hFile, buffer, size, &bytesRead, &overlapped) && (bytesRead == size);
}

std::string format_time(double seconds) {
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
    while (true) {
        uint64_t current = g_write_ptr.load();
        double percent = (total > 0) ? (static_cast<double>(current) / total * 100.0) : 0.0;
        double elapsed = std::chrono::duration<double>(Clock::now() - start_time_global).count();
        double speed = (current > 0 && elapsed > 0) ? (double)current / elapsed : 0.0;
        double remaining = (speed > 0) ? (static_cast<double>(total - current) / speed) : 0;
        
        std::cout << "\r" << label << " " << std::fixed << std::setprecision(1) << (percent > 100.0 ? 100.0 : percent) << "%"
                  << " | Str: " << g_streams_found.load() 
                  << " | " << std::fixed << std::setprecision(2) << (speed / 1024.0 / 1024.0) << " MB/s"
                  << " | RAM: " << (g_current_mem_usage.load() / 1024 / 1024) << "MB"
                  << " | Cache: " << g_cache_hits.load()
                  << " | Dict: " << g_dict_reuse.load()
                  << " | " << format_time(elapsed) << " < " << format_time(remaining) << "    " << std::flush;
        
        if (g_global_done && (current >= total)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

#pragma pack(push, 1)
struct GeneralPrecompHeader {
    char     magic[4] = {'P','R','E','C'};
    uint8_t  version = 14; 
    uint64_t original_size = 0;
    uint32_t num_streams = 0;
    uint64_t expanded_size = 0;
};
struct StreamMetadata {
    uint64_t offset;
    uint64_t dec_size;
    uint32_t comp_size;
    uint8_t  level;
    int8_t   windowBits; 
};
#pragma pack(pop)

struct RingBufferSlot {
    std::atomic<bool> ready{false};
    uint64_t input_pos_id = 0;
    std::vector<uint8_t> payload;
    StreamMetadata meta;
};

// ==================== WINDOWS ENCODER V1.05 (PIPELINE STREAMING) ====================
void do_encode(const std::wstring& in_path, const std::wstring& out_path, int num_threads) {
    auto encode_start_time = Clock::now();
    
    HANDLE hFileIn = CreateFileW(in_path.c_str(), GENERIC_READ, FILE_SHARE_READ, 
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFileIn == INVALID_HANDLE_VALUE) {
        std::wcerr << L"Error: Cannot open input file: " << in_path << std::endl;
        return;
    }

    LARGE_INTEGER fileSize;
    GetFileSizeEx(hFileIn, &fileSize);
    uint64_t total_size = fileSize.QuadPart;

    HANDLE hMapping = CreateFileMapping(hFileIn, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMapping) {
        std::wcerr << L"Error: Cannot create file mapping" << std::endl;
        CloseHandle(hFileIn);
        return;
    }

    const uint8_t* m_in = (const uint8_t*)MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    if (!m_in) {
        std::wcerr << L"Error: Cannot map view of file" << std::endl;
        CloseHandle(hMapping);
        CloseHandle(hFileIn);
        return;
    }

    HANDLE hFileOut = CreateFileW(out_path.c_str(), GENERIC_WRITE, 0, 
                                   NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFileOut == INVALID_HANDLE_VALUE) {
        std::wcerr << L"Error: Cannot create output file: " << out_path << std::endl;
        UnmapViewOfFile(m_in);
        CloseHandle(hMapping);
        CloseHandle(hFileIn);
        return;
    }

    auto win32_write = [&](const void* data, uint64_t size) -> bool {
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        uint64_t remaining = size;
        while (remaining > 0) {
            DWORD to_write = (remaining > 0xFFFFFFFFULL) ? 0xFFFFFFFF : static_cast<DWORD>(remaining);
            DWORD written = 0;
            if (!WriteFile(hFileOut, ptr, to_write, &written, NULL) || written != to_write) {
                return false;
            }
            ptr += written;
            remaining -= written;
        }
        return true;
    };

    GeneralPrecompHeader h{}; 
    h.original_size = total_size;
    if (!win32_write(&h, sizeof(h))) {
        std::wcerr << L"Error: Failed to write header" << std::endl;
        CloseHandle(hFileOut);
        UnmapViewOfFile(m_in);
        CloseHandle(hMapping);
        CloseHandle(hFileIn);
        return;
    }

    start_time_global = Clock::now(); 
    g_global_done = false; g_write_ptr = 0; g_streams_found = 0; g_current_mem_usage = 0; 
    g_cache_hits = 0; g_dict_reuse = 0; g_dict_fallbacks = 0;
    
    std::optional<std::thread> prog;
    prog.emplace(progress_updater, "Nitro-E", total_size);

    std::vector<std::atomic<uint64_t>> worker_heads(num_threads);
    uint64_t chunk_size = (total_size + num_threads - 1) / num_threads;
    for(int i=0; i<num_threads; ++i) {
        uint64_t s = (uint64_t)i * chunk_size;
        worker_heads[i].store(s);
    }
    
    struct TargetStream {
        StreamMetadata meta;
        std::vector<uint8_t> payload;
    };
    std::map<uint64_t, TargetStream> pending_streams;
    std::mutex pipeline_mtx;
    std::condition_variable cv_pipeline;
    std::atomic<size_t> pipeline_mem_usage{0};
    
    std::vector<StreamMetadata> final_table;
    int max_level = 0;
    
    BufferPool vtmp_pool(16 * 1024 * 1024 + 65536, num_threads * 2);
    BufferPool inflate_pool(65536, num_threads * 4);

    auto worker = [&](int id, uint64_t start, uint64_t end) {
        uint64_t pos = start;
        std::vector<uint8_t> vtmp = vtmp_pool.acquire();
        std::vector<uint8_t> dec_buf(128 * 1024 * 1024);

        while (pos < end) {
            if (pos % (256 * 1024) == 0) {
                worker_heads[id].store(pos);
            }

            const uint8_t* compressed_data_start = m_in + pos;
            bool handled_stream = false;
            int detected_wbits = 0;
            
            if (pos + 2 < end && m_in[pos] == 0x78 && (m_in[pos+1] == 0x9C || m_in[pos+1] == 0xDA || m_in[pos+1] == 0x01)) {
                detected_wbits = 15;
            } 
            else if (pos + 6 < end && memcmp(m_in + pos, "stream", 6) == 0) {
                uint64_t search_pos = pos + 6;
                while(search_pos < end && search_pos < pos + 10 && (m_in[search_pos] == '\r' || m_in[search_pos] == '\n')) search_pos++;
                pos = search_pos;
                compressed_data_start = m_in + pos;
                detected_wbits = -15;
            }

            if (detected_wbits != 0) {
                std::vector<uint8_t> tmp_buf = inflate_pool.acquire();
                z_stream zs = {};
                
                if(inflateInit2(&zs, detected_wbits) == Z_OK) {
                    zs.next_in = const_cast<Bytef*>(m_in + pos);
                    zs.avail_in = (uInt)std::min<uint64_t>(total_size - pos, 16 * 1024 * 1024);
                    
                    zs.next_out = dec_buf.data();
                    zs.avail_out = (uInt)dec_buf.size();
                    
                    int ret = inflate(&zs, Z_NO_FLUSH);
                    size_t inflated_total = zs.total_out;
                    uint32_t consumed = (uint32_t)zs.total_in;
                    
                    inflateEnd(&zs);

                    if (ret == Z_STREAM_END && inflated_total >= MIN_DECOMPRESS_SIZE && inflated_total <= dec_buf.size()) {
                        uint16_t fp = generate_nitro_fingerprint(compressed_data_start);
                        NitroCacheEntry& cache_entry = g_nitro_intel_cache[fp];
                        bool compression_found = false;
                        uint8_t winning_level = 0;
                        
                        if (cache_entry.is_valid.load(std::memory_order_acquire) && 
                            cache_entry.window_bits == detected_wbits) {
                            
                            uint8_t cached_level = cache_entry.matching_level;
                            z_stream* vzs = g_dict_cache.acquire_stream(detected_wbits, cached_level);
                            if (vzs) {
                                if (vtmp.size() < consumed + 1024) vtmp.resize(consumed + 1024);
                                vzs->next_in = dec_buf.data(); 
                                vzs->avail_in = (uInt)inflated_total; 
                                vzs->next_out = vtmp.data(); 
                                vzs->avail_out = (uInt)vtmp.size();

                                if (deflate(vzs, Z_FINISH) == Z_STREAM_END && 
                                    vzs->total_out == (uLong)consumed && 
                                    memcmp(vtmp.data(), compressed_data_start, consumed) == 0) {
                                    
                                    compression_found = true;
                                    winning_level = cached_level;
                                    g_cache_hits++;
                                    g_dict_reuse++;
                                }
                                g_dict_cache.release_stream(vzs);
                            }
                            
                            if (!compression_found) {
                                cache_entry.is_valid.store(false, std::memory_order_release);
                            }
                        }
                        
                        if (!compression_found) {
                            for (uint8_t l : {6, 9, 1}) {
                                if (cache_entry.is_valid.load(std::memory_order_acquire) && 
                                    l == cache_entry.matching_level) continue;
                                
                                z_stream* vzs = g_dict_cache.acquire_stream(detected_wbits, l);
                                if (!vzs) {
                                    g_dict_fallbacks++;
                                    continue;
                                }
                                
                                if (vtmp.size() < consumed + 1024) vtmp.resize(consumed + 1024);
                                vzs->next_in = dec_buf.data(); 
                                vzs->avail_in = (uInt)inflated_total; 
                                vzs->next_out = vtmp.data(); 
                                vzs->avail_out = (uInt)vtmp.size();

                                int deflate_result = deflate(vzs, Z_FINISH);
                                if (deflate_result == Z_STREAM_END && 
                                    vzs->total_out == (uLong)consumed && 
                                    memcmp(vtmp.data(), compressed_data_start, consumed) == 0) {
                                    
                                    cache_entry.matching_level = l;
                                    cache_entry.window_bits = (int8_t)detected_wbits;
                                    cache_entry.is_valid.store(true, std::memory_order_release);
                                    
                                    compression_found = true;
                                    winning_level = l;
                                    g_dict_reuse++;
                                    g_dict_cache.release_stream(vzs);
                                    break;
                                }
                                g_dict_cache.release_stream(vzs);
                            }
                        }
                        
                        if (compression_found) {
                            if (winning_level > max_level) max_level = winning_level;
                            
                            std::vector<uint8_t> final_payload(dec_buf.begin(), dec_buf.begin() + inflated_total);
                            
                            {
                                std::lock_guard<std::mutex> lock(pipeline_mtx);
                                TargetStream ts;
                                ts.meta = {pos, inflated_total, consumed, winning_level, (int8_t)detected_wbits};
                                ts.payload = std::move(final_payload);
                                pending_streams[pos] = std::move(ts);
                                pipeline_mem_usage += inflated_total;
                                g_streams_found++;
                            }
                            cv_pipeline.notify_one();
                            
                            pos += consumed;
                            worker_heads[id].store(pos);
                            handled_stream = true;
                        }
                    }
                }
                inflate_pool.release(tmp_buf);
            }
            
            if (!handled_stream) {
                pos++;
            }
        }
        worker_heads[id].store(end);
        vtmp_pool.release(vtmp);
    };

    std::vector<std::thread> workers;
    for (int i = 0; i < num_threads; ++i) {
        uint64_t s = (uint64_t)i * chunk_size;
        uint64_t e = std::min(s + chunk_size, total_size);
        if (s < total_size) workers.emplace_back(worker, i, s, e);
    }

    uint64_t current_in_ptr = 0;
    uint64_t data_section_start = sizeof(h);
    uint64_t current_write_offset = sizeof(h);
    uint64_t total_payload_compressed = 0;
    uint64_t total_decompressed_streams_size = 0;

    while (current_in_ptr < total_size) {
        std::unique_lock<std::mutex> lock(pipeline_mtx);
        
        bool stream_ready = !pending_streams.empty() && 
                            pending_streams.begin()->first == current_in_ptr;
        
        if (stream_ready) {
            TargetStream res = std::move(pending_streams.begin()->second);
            uint64_t stream_pos = res.meta.offset;
            pending_streams.erase(pending_streams.begin());
            pipeline_mem_usage -= res.payload.size();
            lock.unlock();
            
            if (stream_pos > current_in_ptr) {
                uint64_t gap_size = stream_pos - current_in_ptr;
                win32_write(m_in + current_in_ptr, gap_size);
                current_write_offset += gap_size;
                current_in_ptr = stream_pos;
                g_write_ptr.store(current_in_ptr);
            }
            
            StreamMetadata sm = res.meta;
            sm.offset = current_write_offset - data_section_start;
            final_table.push_back(sm);
            
            win32_write(res.payload.data(), res.payload.size());
            current_write_offset += res.payload.size();
            total_payload_compressed += res.meta.comp_size;
            total_decompressed_streams_size += res.meta.dec_size;
            
            current_in_ptr += res.meta.comp_size;
            g_write_ptr.store(current_in_ptr);
            g_current_mem_usage = pipeline_mem_usage.load();
            continue;
        }
        
        int active_worker = std::min<int>(num_threads - 1, (int)(current_in_ptr / chunk_size));
        uint64_t next_chunk_start = std::min<uint64_t>((uint64_t)(active_worker + 1) * chunk_size, total_size);
        uint64_t safe_limit = std::min<uint64_t>(next_chunk_start, worker_heads[active_worker].load());
        
        if (!pending_streams.empty()) {
            safe_limit = std::min(safe_limit, pending_streams.begin()->first);
        }
        
        if (safe_limit > current_in_ptr) {
            uint64_t gap_size = safe_limit - current_in_ptr;
            lock.unlock();
            
            win32_write(m_in + current_in_ptr, gap_size);
            current_write_offset += gap_size;
            current_in_ptr = safe_limit;
            g_write_ptr.store(current_in_ptr);
            g_current_mem_usage = pipeline_mem_usage.load();
            continue;
        }
        
        bool all_done = true;
        for(int i=0; i<num_threads; ++i) {
            uint64_t expected_end = std::min((uint64_t)(i+1)*chunk_size, total_size);
            if(worker_heads[i].load() < expected_end) {
                all_done = false;
                break;
            }
        }
        
        if (all_done && pending_streams.empty()) {
            break;
        }
        
        cv_pipeline.wait_for(lock, std::chrono::milliseconds(2));
    }

    if (current_in_ptr < total_size) {
        uint64_t tail = total_size - current_in_ptr;
        win32_write(m_in + current_in_ptr, tail);
        current_write_offset += tail;
        current_in_ptr += tail;
        g_write_ptr.store(current_in_ptr);
    }

    for (auto& t : workers) t.join();

    h.num_streams = (uint32_t)final_table.size(); 
    h.expanded_size = current_write_offset - sizeof(h);
    
    win32_write(final_table.data(), final_table.size() * sizeof(StreamMetadata));
    
    OVERLAPPED ov = {0};
    DWORD written;
    WriteFile(hFileOut, &h, sizeof(h), &written, &ov);

    g_global_done = true; g_write_ptr.store(total_size);
    if (prog->joinable()) prog->join();
    
    CloseHandle(hFileOut);
    UnmapViewOfFile(m_in);
    CloseHandle(hMapping);
    CloseHandle(hFileIn);
    
    uint64_t total_payload_diff = h.expanded_size - total_decompressed_streams_size;
    double diff_ratio = (total_size > 0) ? (static_cast<double>(total_payload_diff) / total_size * 100.0) : 0.0;
    
    auto get_elapsed_seconds = [&]() {
        return std::chrono::duration<double>(Clock::now() - encode_start_time).count();
    };

  std::wcout << L"\n[OK] Precomp saved successfully! v1.05 Pipeline\n"
           << L"    Streams   : " << final_table.size() << L"\n"
           << L"    Original  : " << total_size << L" bytes (" << (total_size / 1024.0 / 1024.0) << L" MB)\n"
           << L"    Expanded  : " << h.expanded_size << L" bytes (" << (h.expanded_size / 1024.0 / 1024.0) << L" MB)\n"
           << L"    Diff/Res  : " << total_payload_diff << L" bytes (" << (total_payload_diff / 1024.0 / 1024.0) << L" MB) - " 
           << std::fixed << std::setprecision(1) << diff_ratio << L"% of original\n"
           << L"    Levels    : " << max_level << L"\n"
           << L"    Cache     : " << g_cache_hits.load() << L"/" << g_streams_found.load() 
           << L" (" << (g_streams_found.load() > 0 ? (g_cache_hits.load() * 100 / g_streams_found.load()) : 0) << L"%)\n"
           << L"    Dict      : " << g_dict_reuse.load() << L" reuse, " << g_dict_fallbacks.load() << L" fallbacks\n"
           << L"    Total time: " << std::fixed << std::setprecision(1) << get_elapsed_seconds() << L" seconds\n";

}

// ==================== WINDOWS DECODER V1.05 ====================
void do_decode(const std::wstring& in_path, const std::wstring& out_path, int num_threads) {
    auto decode_start_time = Clock::now();
    
    HANDLE hFileIn = CreateFileW(in_path.c_str(), GENERIC_READ, FILE_SHARE_READ, 
                                 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    
    if (hFileIn == INVALID_HANDLE_VALUE) {
        std::wcerr << L"CRITICAL: Cannot open input file: " << in_path << std::endl;
        return;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFileIn, &fileSize)) {
        std::wcerr << L"CRITICAL: Cannot query input file size." << std::endl;
        CloseHandle(hFileIn);
        return;
    }
    uint64_t actual_file_size = static_cast<uint64_t>(fileSize.QuadPart);

    GeneralPrecompHeader h;
    DWORD bytesRead;
    if (!ReadFile(hFileIn, &h, sizeof(GeneralPrecompHeader), &bytesRead, NULL) || bytesRead != sizeof(GeneralPrecompHeader)) {
        std::wcerr << L"CRITICAL: Failed to read PREC header." << std::endl;
        CloseHandle(hFileIn);
        return;
    }

    if (std::memcmp(h.magic, "PREC", 4) != 0 || h.version != 14) {
        std::wcerr << L"CRITICAL: Invalid magic bytes or unsupported format version (Requires PREC v14)." << std::endl;
        CloseHandle(hFileIn);
        return;
    }

    uint64_t expected_meta_size = static_cast<uint64_t>(h.num_streams) * sizeof(StreamMetadata);
    uint64_t total_expected_size = sizeof(GeneralPrecompHeader) + h.expanded_size + expected_meta_size;

    if (actual_file_size != total_expected_size) {
        std::wcerr << L"CRITICAL [PREC v13]: Integrity Check Failed!\n"
                   << L"  Expected size: " << total_expected_size << L" bytes\n"
                   << L"  Actual size:   " << actual_file_size << L" bytes" << std::endl;
        CloseHandle(hFileIn);
        return;
    }
    
    if (h.num_streams > 10000000) {
        std::wcerr << L"CRITICAL: Unreasonable stream count. File rejected." << std::endl;
        CloseHandle(hFileIn);
        return;
    }
    
    std::vector<StreamMetadata> streams(h.num_streams);
    if (h.num_streams > 0) {
        uint64_t metadata_offset = sizeof(GeneralPrecompHeader) + h.expanded_size;
        if (!portable_pread(hFileIn, streams.data(), h.num_streams * sizeof(StreamMetadata), metadata_offset)) {
            std::wcerr << L"CRITICAL: Cannot read stream metadata" << std::endl;
            CloseHandle(hFileIn);
            return;
        }
    }
    
    HANDLE hFileOut = CreateFileW(out_path.c_str(), GENERIC_WRITE, 0, 
                                  NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFileOut == INVALID_HANDLE_VALUE) {
        std::wcerr << L"CRITICAL: Cannot create output file: " << out_path << std::endl;
        CloseHandle(hFileIn);
        return;
    }

    auto win32_write = [&](const void* data, uint64_t size) -> bool {
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        uint64_t remaining = size;
        while (remaining > 0) {
            DWORD to_write = (remaining > 0xFFFFFFFFULL) ? 0xFFFFFFFF : static_cast<DWORD>(remaining);
            DWORD written = 0;
            if (!WriteFile(hFileOut, ptr, to_write, &written, NULL) || written != to_write) {
                return false;
            }
            ptr += written;
            remaining -= written;
        }
        return true;
    };
    
    start_time_global = Clock::now(); 
    g_global_done = false; g_write_ptr = 0; g_streams_found = 0;
    g_dict_reuse = 0; g_dict_fallbacks = 0;
    std::thread prog(progress_updater, "Nitro-D", h.original_size);

    auto ring_buffer = std::make_unique<RingBufferSlot[]>(256);
    std::mutex ring_mtx;
    std::condition_variable cv_ring;
    std::atomic<uint64_t> global_decode_ticket{0}; 
    uint64_t expected_ticket = 0;
    
    uint64_t data_section_start = sizeof(GeneralPrecompHeader);

    auto worker = [&]() {
        std::vector<uint8_t> dec_buf(128 * 1024 * 1024);

        while (true) {
            uint64_t my_task_id = global_decode_ticket.fetch_add(1, std::memory_order_relaxed);
            if (my_task_id >= h.num_streams) break;
            
            StreamMetadata sm = streams[my_task_id];
            
            uint64_t read_offset = data_section_start + sm.offset;
            size_t bytes_to_read = std::min((size_t)sm.dec_size, dec_buf.size());
            if (!portable_pread(hFileIn, dec_buf.data(), (uint32_t)bytes_to_read, read_offset)) {
                std::wcerr << L"Error reading stream " << my_task_id << L" at offset " << read_offset << std::endl;
                continue;
            }
            
            z_stream* zs = g_dict_cache.acquire_stream(sm.windowBits, sm.level);
            bool emergency_alloc = false;
            
            if (!zs) {
                zs = new z_stream();
                memset(zs, 0, sizeof(z_stream));
                if (deflateInit2(zs, sm.level, Z_DEFLATED, sm.windowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                    std::wcerr << L"Critical: Cannot initialize deflate for stream " << my_task_id << std::endl;
                    delete zs;
                    continue;
                }
                emergency_alloc = true;
                g_dict_fallbacks++;
            }
            
            std::vector<uint8_t> rec(sm.comp_size);
            
            zs->next_in = dec_buf.data(); 
            zs->avail_in = (uInt)bytes_to_read;
            zs->next_out = rec.data();
            zs->avail_out = (uInt)rec.size();
            
            int ret = deflate(zs, Z_FINISH);
            uLong actual_out = zs->total_out;
            
            if (ret != Z_STREAM_END || actual_out != sm.comp_size) {
                if (actual_out < sm.comp_size) {
                    std::memset(rec.data() + actual_out, 0, sm.comp_size - actual_out);
                }
            }
            
            if (emergency_alloc) {
                if (zs->state) deflateEnd(zs);
                delete zs;
            } else {
                g_dict_cache.release_stream(zs);
                g_dict_reuse++;
            }
            
            size_t slot_idx = my_task_id & 255;
            
            {
                std::unique_lock<std::mutex> lock(ring_mtx);
                cv_ring.wait(lock, [&]() { 
                    return !ring_buffer[slot_idx].ready.load(std::memory_order_acquire); 
                });
                
                ring_buffer[slot_idx].payload = std::move(rec);
                ring_buffer[slot_idx].meta = sm;
                ring_buffer[slot_idx].ready.store(true, std::memory_order_release);
            }
            cv_ring.notify_all();
        }
    };

    int worker_count = (h.num_streams > 0) ? num_threads : 0;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for(int i = 0; i < worker_count; ++i) {
        workers.emplace_back(worker);
    }

    uint64_t pcf_read_ptr = 0;
    std::vector<char> io_buf(4 * 1024 * 1024);
    
    while (expected_ticket < h.num_streams) {
        size_t slot_idx = expected_ticket & 255;
        
        {
            std::unique_lock<std::mutex> lock(ring_mtx);
            cv_ring.wait(lock, [&]() {
                return ring_buffer[slot_idx].ready.load(std::memory_order_acquire);
            });
        }
        
        StreamMetadata sm = ring_buffer[slot_idx].meta;
        std::vector<uint8_t>& rec = ring_buffer[slot_idx].payload;
        
        uint64_t gap = sm.offset - pcf_read_ptr;
        while(gap > 0) {
            uint32_t chunk = (uint32_t)std::min<uint64_t>(gap, io_buf.size());
            uint64_t read_pos = data_section_start + pcf_read_ptr;
            if (!portable_pread(hFileIn, io_buf.data(), chunk, read_pos)) {
                break;
            }
            win32_write(io_buf.data(), chunk);
            gap -= chunk; 
            pcf_read_ptr += chunk; 
            g_write_ptr += chunk;
        }
        
        win32_write(rec.data(), rec.size());
        pcf_read_ptr += sm.dec_size;
        g_write_ptr += rec.size();
        g_streams_found++;
        
        ring_buffer[slot_idx].ready.store(false, std::memory_order_release);
        ring_buffer[slot_idx].payload.clear();
        expected_ticket++;
        
        cv_ring.notify_all();
    }

    uint64_t tail = h.expanded_size - pcf_read_ptr;
    while(tail > 0) {
        uint32_t chunk = (uint32_t)std::min<uint64_t>(tail, io_buf.size());
        uint64_t read_pos = data_section_start + pcf_read_ptr;
        if (!portable_pread(hFileIn, io_buf.data(), chunk, read_pos)) {
            break;
        }
        win32_write(io_buf.data(), chunk);
        tail -= chunk; 
        pcf_read_ptr += chunk; 
        g_write_ptr += chunk;
    }

    for(auto& t : workers) t.join();
    
    g_global_done = true; 
    g_write_ptr = h.original_size; 
    if (prog.joinable()) prog.join();
    
    CloseHandle(hFileOut);
    CloseHandle(hFileIn);
    
    auto get_elapsed_seconds = [&]() {
        return std::chrono::duration<double>(Clock::now() - decode_start_time).count();
    };

    std::wcout << L"\n[OK] Precomp decoded successfully! v1.05\n"
               << L"    Streams   : " << expected_ticket << L"\n"
               << L"    Original  : " << h.original_size << L" bytes\n"
               << L"    Dict      : " << g_dict_reuse.load() << L" reuse, " << g_dict_fallbacks.load() << L" fallbacks\n"
               << L"    Total time: " << std::fixed << std::setprecision(1) << get_elapsed_seconds() << L" seconds\n";
}

// ==================== WINDOWS ENTRY POINT ====================
int wmain(int argc, wchar_t** argv) {
    std::wcout << L"============================" << std::endl;
    std::wcout << L" NITRO NG 1.05 HP - WINDOWS " << std::endl;
    std::wcout << L"============================" << std::endl;

    if (argc < 4) {
        std::wcout << L"Use:\n"
                   << L"  Encode : " << argv[0] << L" e <input> <output.pre> [-j threads]\n"
                   << L"  Decode  : " << argv[0] << L" d <input.pre> <restored> [-j threads]\n";
        return 1;
    }

    std::wstring mode = argv[1];
    std::wstring in_path = argv[2];
    std::wstring out_path = argv[3];
    int threads = 2;

    if (argc >= 6 && std::wstring(argv[4]) == L"-j") {
        threads = _wtoi(argv[5]);
    }

    if (mode == L"e") {
        do_encode(in_path, out_path, threads);
    } else if (mode == L"d") {
        do_decode(in_path, out_path, threads);
    } else {
        std::wcout << L"Error: Unknow Mode '" << mode << L"'" << std::endl;
        return 1;
    }

    return 0;
}
