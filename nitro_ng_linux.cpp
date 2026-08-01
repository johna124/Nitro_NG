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

// Linux / POSIX Standard Headers
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#define TITLE "============================\n" \
              " NITRO NG 1.05 HP - LINUX  \n" \
              "============================"
#define MIN_DECOMPRESS_SIZE 512ULL 
#define MAX_PENDING_RAM (1024ULL * 1024 * 1024 * 2) 
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

inline uint16_t generate_nitro_fingerprint(const uint8_t* compressed_data, size_t available_bytes) {
    if (available_bytes < 8 || !compressed_data) return 0;
    uint64_t h = *reinterpret_cast<const uint64_t*>(compressed_data);
    h ^= 0xcbf29ce484222325ULL;
    h *= 0x100000001b3ULL;
    return static_cast<uint16_t>((h ^ (h >> 32)) & (NITRO_CACHE_SIZE - 1));
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

bool portable_pread(int fd, void* buffer, uint32_t size, uint64_t offset) {
    return ::pread(fd, buffer, size, offset) == static_cast<ssize_t>(size);
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
                  << " | Fall: " << g_dict_fallbacks.load()
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

struct StreamResult {
    StreamMetadata meta;
    std::vector<uint8_t> payload;
};

// ==================== LINUX NATIVE ENCODER ====================
void do_encode(const std::string& in_path, const std::string& out_path, int num_threads) {
    auto encode_start_time = Clock::now();
    
    int fd = open(in_path.c_str(), O_RDONLY);
    if (fd == -1) {
        std::cerr << "Error: Cannot open input file: " << in_path << std::endl;
        return;
    }

    struct stat st;
    fstat(fd, &st);
    uint64_t total_size = st.st_size;

    const uint8_t* m_in = (const uint8_t*)mmap(nullptr, total_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m_in == MAP_FAILED) {
        std::cerr << "Error: Cannot mmap input file" << std::endl;
        close(fd);
        return;
    }
    
    madvise((void*)m_in, total_size, MADV_SEQUENTIAL);

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::cerr << "Error: Cannot create output file: " << out_path << std::endl;
        munmap((void*)m_in, total_size);
        close(fd);
        return;
    }

    GeneralPrecompHeader h{}; 
    h.original_size = total_size;
    out.write((char*)&h, sizeof(h));

    start_time_global = Clock::now(); 
    g_global_done = false; g_write_ptr = 0; g_streams_found = 0; g_current_mem_usage = 0; 
    g_cache_hits = 0; g_dict_reuse = 0; g_dict_fallbacks = 0;
    std::thread prog(progress_updater, "Nitro-Linux-E", total_size);

    std::map<uint64_t, StreamResult> pending_map;
    std::vector<std::atomic<uint64_t>> worker_heads(num_threads);
    for(int i=0; i<num_threads; ++i) worker_heads[i].store(0);
    
    std::mutex mtx; 
    std::condition_variable cv_writer;
    std::vector<StreamMetadata> final_table;
    std::vector<StreamResult> all_streams;
    uint64_t chunk_size = (total_size + num_threads - 1) / num_threads;
    int max_level = 0;
    
    BufferPool vtmp_pool(16 * 1024 * 1024 + 65536, num_threads * 2);
    BufferPool inflate_pool(65536, num_threads * 4);

    auto worker = [&](int id, uint64_t start, uint64_t end) {
        uint64_t pos = start;
        std::vector<uint8_t> vtmp = vtmp_pool.acquire();
        
        while (pos < end) {
            if (pos % (256 * 1024) == 0) {
                worker_heads[id].store(pos);
                cv_writer.notify_all();
            }

            if (g_current_mem_usage.load() > MAX_PENDING_RAM) { 
                std::this_thread::sleep_for(std::chrono::milliseconds(5)); 
                continue; 
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
                    std::vector<uint8_t> dec_buf; 
                    uint8_t* tmp = tmp_buf.data();
                    int ret;
                    
                    do { 
                        zs.next_out = tmp; 
                        zs.avail_out = (uInt)inflate_pool.default_size; 
                        ret = inflate(&zs, Z_NO_FLUSH);
                        size_t inflated = inflate_pool.default_size - zs.avail_out;
                        dec_buf.insert(dec_buf.end(), tmp, tmp + inflated);
                    } while (ret == Z_OK && dec_buf.size() < 128 * 1024 * 1024);

                    uint32_t consumed = (uint32_t)zs.total_in; 
                    inflateEnd(&zs);

                    if (ret == Z_STREAM_END && dec_buf.size() >= MIN_DECOMPRESS_SIZE) {
                        uint16_t fp = generate_nitro_fingerprint(compressed_data_start, std::min(consumed, (uint32_t)32));
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
                                vzs->avail_in = (uInt)dec_buf.size(); 
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
                                vzs->avail_in = (uInt)dec_buf.size(); 
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
                            
                            uint64_t d_sz = dec_buf.size();
                            { 
                                std::lock_guard<std::mutex> lock(mtx); 
                                pending_map[pos] = { {pos, d_sz, consumed, winning_level, (int8_t)detected_wbits}, std::move(dec_buf) }; 
                                g_streams_found++; g_current_mem_usage += d_sz; 
                            }
                            pos += consumed; 
                            worker_heads[id].store(pos); 
                            cv_writer.notify_all();
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
        cv_writer.notify_all();
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
    uint64_t total_payload = 0;
    
    while (current_in_ptr < total_size) {
        std::unique_lock<std::mutex> lock(mtx);
        auto it = pending_map.begin();
        if (it != pending_map.end() && it->first == current_in_ptr) {
            StreamResult res = std::move(it->second); 
            uint64_t p_size = res.payload.size(); 
            total_payload += p_size;
            all_streams.push_back(res);
            pending_map.erase(it); lock.unlock();
            StreamMetadata sm = res.meta; 
            sm.offset = (uint64_t)out.tellp() - data_section_start; 
            final_table.push_back(sm);
            out.write((char*)res.payload.data(), res.payload.size());
            current_in_ptr += res.meta.comp_size; 
            g_write_ptr.store(current_in_ptr); g_current_mem_usage -= p_size; 
            continue;
        }

        int active_worker = std::min<int>(num_threads - 1, (int)(current_in_ptr / chunk_size));
        uint64_t worker_limit = worker_heads[active_worker].load();
        uint64_t write_limit = (it != pending_map.end() && it->first < worker_limit) ? it->first : worker_limit;

        if (write_limit > current_in_ptr) {
            uint64_t to_write = write_limit - current_in_ptr; 
            lock.unlock();
            out.write(reinterpret_cast<const char*>(m_in + current_in_ptr), to_write); 
            current_in_ptr += to_write; g_write_ptr.store(current_in_ptr);
        } else {
            bool all_done = true;
            for(int i=0; i<num_threads; ++i) 
                if(worker_heads[i].load() < std::min((uint64_t)(i+1)*chunk_size, total_size)) all_done = false;
            if(all_done && pending_map.empty()) break;
            cv_writer.wait_for(lock, std::chrono::milliseconds(10));
        }
    }

    for (auto& t : workers) t.join();
    
    h.num_streams = (uint32_t)final_table.size(); 
    h.expanded_size = (uint64_t)out.tellp() - sizeof(h);
    out.write((char*)final_table.data(), final_table.size() * sizeof(StreamMetadata));
    out.seekp(0); out.write((char*)&h, sizeof(h)); out.close();

    g_global_done = true; g_write_ptr.store(total_size); 
    if (prog.joinable()) prog.join();
    
    munmap((void*)m_in, total_size);
    close(fd);
    
        uint64_t total_decompressed_streams_size = 0;
    for (const auto& sm : final_table) {
        total_decompressed_streams_size += sm.dec_size;
    }

    uint64_t total_payload_diff = h.expanded_size - total_decompressed_streams_size;
    double diff_ratio = (total_size > 0) ? (static_cast<double>(total_payload_diff) / total_size * 100.0) : 0.0;
    
    uint64_t total_payload_compressed = 0;
    for (const auto& sm : final_table) {
        total_payload_compressed += sm.comp_size;
    }
    
    auto get_elapsed_seconds = [&]() {
        return std::chrono::duration<double>(Clock::now() - encode_start_time).count();
    };

    std::cout << "\n[OK] Precomp saved successfully!\n"
              << "    Streams   : " << all_streams.size() << "\n"
              << "    Original  : " << total_size << " bytes (" << (total_size / 1024.0 / 1024.0) << " MB)\n"
              << "    Expanded  : " << h.expanded_size << " bytes (" << (h.expanded_size / 1024.0 / 1024.0) << " MB)\n"
              << "    Diff/Res  : " << total_payload_diff << " bytes (" << (total_payload_diff / 1024.0 / 1024.0) << " MB) - " 
              << std::fixed << std::setprecision(1) << diff_ratio << "% of original\n"
              << "    Levels    : " << max_level << "\n"
              << "    Cache     : " << g_cache_hits.load() << "/" << g_streams_found.load() 
              << " (" << (g_streams_found.load() > 0 ? (g_cache_hits.load() * 100 / g_streams_found.load()) : 0) << "%)\n"
              << "    Dict      : " << g_dict_reuse.load() << " reuse, " << g_dict_fallbacks.load() << " fallbacks\n"
              << "    Total time: " << std::fixed << std::setprecision(1) << get_elapsed_seconds() << " seconds\n";
}

// ==================== LINUX NATIVE DECODER ====================
void do_decode(const std::string& in_path, const std::string& out_path, int num_threads) {
    auto decode_start_time = Clock::now();
    
    int fd = open(in_path.c_str(), O_RDONLY);
    if (fd == -1) {
        std::cerr << "Error: Cannot open input file: " << in_path << std::endl;
        return;
    }

    GeneralPrecompHeader h; 
    if (!portable_pread(fd, &h, sizeof(h), 0)) {
        std::cerr << "Error: Cannot read header" << std::endl;
        close(fd);
        return;
    }
    
    if (memcmp(h.magic, "PREC", 4) != 0) {
        std::cerr << "Error: Invalid file format" << std::endl;
        close(fd);
        return;
    }
    
    std::vector<StreamMetadata> streams(h.num_streams);
    if (h.num_streams > 0) {
        uint64_t metadata_offset = sizeof(GeneralPrecompHeader) + h.expanded_size;
        if (!portable_pread(fd, streams.data(), h.num_streams * sizeof(StreamMetadata), metadata_offset)) {
            std::cerr << "Error: Cannot read stream metadata" << std::endl;
            close(fd);
            return;
        }
    }
    
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::cerr << "Error: Cannot create output file: " << out_path << std::endl;
        close(fd);
        return;
    }
    
    start_time_global = Clock::now(); 
    g_global_done = false; g_write_ptr = 0; g_streams_found = 0;
    g_dict_reuse = 0; g_dict_fallbacks = 0;
    std::thread prog(progress_updater, "Nitro-Linux-D", h.original_size);

    std::mutex mtx; 
    std::condition_variable cv;
    std::map<int, std::vector<uint8_t>> finished_streams;
    std::atomic<int> next_task_id{0};
    int write_stream_id = 0;
    uint64_t data_section_start = sizeof(GeneralPrecompHeader);

    auto worker = [&]() {
        std::vector<uint8_t> dec_buf;
        dec_buf.reserve(16 * 1024 * 1024);
        
        std::vector<uint8_t> out_buf;
        out_buf.reserve(16 * 1024 * 1024);

        while (true) {
            int task_id = next_task_id.fetch_add(1);
            if (task_id >= (int)h.num_streams) break;
            
            StreamMetadata sm = streams[task_id];
            
            dec_buf.resize(sm.dec_size);

            uint64_t read_offset = data_section_start + sm.offset;
            if (!portable_pread(fd, dec_buf.data(), (uint32_t)sm.dec_size, read_offset)) {
                std::cerr << "Error reading stream " << task_id << " at offset " << read_offset << std::endl;
                continue;
            }
            
            z_stream* zs = g_dict_cache.acquire_stream(sm.windowBits, sm.level);
            bool emergency_alloc = false;
            
            if (!zs) {
                zs = new z_stream();
                memset(zs, 0, sizeof(z_stream));
                if (deflateInit2(zs, sm.level, Z_DEFLATED, sm.windowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                    std::cerr << "Critical: Cannot initialize deflate for stream " << task_id << std::endl;
                    delete zs;
                    continue;
                }
                emergency_alloc = true;
                g_dict_fallbacks++;
            }
            
            size_t required_out = sm.comp_size + 65536;
            out_buf.resize(required_out);
            
            zs->next_in = dec_buf.data(); 
            zs->avail_in = (uInt)sm.dec_size;
            zs->next_out = out_buf.data(); 
            zs->avail_out = (uInt)out_buf.size();
            
            int ret = deflate(zs, Z_FINISH);
            uLong actual_out = zs->total_out;
            
            std::vector<uint8_t> rec;
            
            if (ret == Z_STREAM_END && actual_out == sm.comp_size) {
                rec.assign(out_buf.data(), out_buf.data() + actual_out);
            } else {
                std::cerr << "Warning: Stream " << task_id << " re-compression mismatch. "
                          << "Expected: " << sm.comp_size << " bytes, Got: " << actual_out 
                          << " bytes, Status: " << (ret == Z_STREAM_END ? "OK" : "ERROR") << std::endl;
                
                rec.resize(sm.comp_size);
                size_t copy_size = std::min((uLong)sm.comp_size, actual_out);
                if (copy_size > 0) {
                    memcpy(rec.data(), out_buf.data(), copy_size);
                }
                
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
            
            {
                std::lock_guard<std::mutex> lock(mtx);
                finished_streams[task_id] = std::move(rec);
            }
            cv.notify_all();
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
    
    while (write_stream_id < (int)h.num_streams) {
        StreamMetadata sm = streams[write_stream_id];
        
        uint64_t gap = sm.offset - pcf_read_ptr;
        while(gap > 0) {
            uint32_t chunk = (uint32_t)std::min<uint64_t>(gap, io_buf.size());
            uint64_t read_pos = data_section_start + pcf_read_ptr;
            if (!portable_pread(fd, io_buf.data(), chunk, read_pos)) {
                std::cerr << "Error reading gap data at offset " << read_pos << std::endl;
                break;
            }
            out.write(io_buf.data(), chunk);
            gap -= chunk; 
            pcf_read_ptr += chunk; 
            g_write_ptr += chunk;
        }
        
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&]{ return finished_streams.count(write_stream_id) > 0; });
        std::vector<uint8_t> rec = std::move(finished_streams[write_stream_id]);
        finished_streams.erase(write_stream_id);
        lock.unlock();
        
        out.write((char*)rec.data(), rec.size());
        pcf_read_ptr += sm.dec_size;
        g_write_ptr += rec.size();
        g_streams_found++; 
        write_stream_id++;
    }

    uint64_t tail = h.expanded_size - pcf_read_ptr;
    while(tail > 0) {
        uint32_t chunk = (uint32_t)std::min<uint64_t>(tail, io_buf.size());
        uint64_t read_pos = data_section_start + pcf_read_ptr;
        if (!portable_pread(fd, io_buf.data(), chunk, read_pos)) {
            std::cerr << "Error reading tail data at offset " << read_pos << std::endl;
            break;
        }
        out.write(io_buf.data(), chunk);
        tail -= chunk; 
        pcf_read_ptr += chunk; 
        g_write_ptr += chunk;
    }

    for(auto& t : workers) t.join();
    out.close();
    
    g_global_done = true; 
    g_write_ptr = h.original_size; 
    if (prog.joinable()) prog.join();
    close(fd);
    
    auto get_elapsed_seconds = [&]() {
        return std::chrono::duration<double>(Clock::now() - decode_start_time).count();
    };

    std::cout << "\n[OK] Precomp decoded successfully!\n"
              << "    Streams   : " << write_stream_id << "\n"
              << "    Original  : " << h.original_size << " bytes\n"
              << "    Dict      : " << g_dict_reuse.load() << " reuse, " << g_dict_fallbacks.load() << " fallbacks\n"
              << "    Total time: " << std::fixed << std::setprecision(1) << get_elapsed_seconds() << " seconds\n";
}

int main(int argc, char** argv) {
   std::cout << TITLE << "\n";
    if (argc < 4) { 
        std::cout << "Use: nitro_ng [e/d] [in] [out] -j [threads]\n"; 
        std::cout << "  e  : Encode (compress streams)\n";
        std::cout << "  d  : Decode (decompress streams)\n";
        std::cout << "  -j : Number of threads (default: hardware concurrency)\n";
        return 0; 
    }
    
    int threads = std::thread::hardware_concurrency();
    for (int i = 4; i < argc; ++i) {
        if (std::string(argv[i]) == "-j" && i+1 < argc) {
            threads = std::stoi(argv[++i]);
            if (threads < 1) threads = 1;
        }
    }

    if (argv[1][0] == 'e') {
        do_encode(argv[2], argv[3], threads);
    } else if (argv[1][0] == 'd') {
        do_decode(argv[2], argv[3], threads);
    } else {
        std::cerr << "Error: Unknown mode. Use 'e' for encode or 'd' for decode.\n";
    }
    
    return 0;
}
