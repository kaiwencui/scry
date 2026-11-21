#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace scry {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;

using DocId = u32;
using Trigram = u32; // Packed 24-bit: (b0 << 16) | (b1 << 8) | b2

constexpr DocId INVALID_DOC_ID = UINT32_MAX;

inline constexpr Trigram pack_trigram(u8 a, u8 b, u8 c) noexcept {
    return (static_cast<u32>(a) << 16) | (static_cast<u32>(b) << 8) | static_cast<u32>(c);
}

inline constexpr u8 ascii_lower(u8 c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<u8>(c + 32) : c;
}

inline constexpr bool is_ascii_upper(u8 c) noexcept {
    return c >= 'A' && c <= 'Z';
}

inline constexpr Trigram lower_trigram(Trigram t) noexcept {
    u8 a = ascii_lower(static_cast<u8>((t >> 16) & 0xFF));
    u8 b = ascii_lower(static_cast<u8>((t >> 8) & 0xFF));
    u8 c = ascii_lower(static_cast<u8>(t & 0xFF));
    return pack_trigram(a, b, c);
}

inline constexpr bool trigram_has_upper(Trigram t) noexcept {
    return is_ascii_upper(static_cast<u8>((t >> 16) & 0xFF)) ||
           is_ascii_upper(static_cast<u8>((t >> 8) & 0xFF)) ||
           is_ascii_upper(static_cast<u8>(t & 0xFF));
}

inline constexpr u64 align_up(u64 val, u64 alignment) noexcept {
    return (val + alignment - 1) & ~(alignment - 1);
}

// Fast 64-bit hash (Wyhash / SplitMix64 inspired, zero-dep, great avalanche)
inline u64 hash_bytes64(const void* data, size_t len, u64 seed = 0x9E3779B97F4A7C15ULL) noexcept {
    const auto* p = static_cast<const u8*>(data);
    u64 h = seed ^ (len * 0xBF58476D1CE4E5B9ULL);
    size_t i = 0;
    while (i + 8 <= len) {
        u64 k;
        std::memcpy(&k, p + i, 8);
        k *= 0x94D049BB133111EBULL;
        k ^= k >> 31;
        k *= 0xBF58476D1CE4E5B9ULL;
        h ^= k;
        h = (h << 27) | (h >> 37);
        h = h * 5 + 0x52DCE729ULL;
        i += 8;
    }
    u64 tail = 0;
    size_t rem = len - i;
    if (rem > 0) {
        std::memcpy(&tail, p + i, rem);
        tail *= 0x94D049BB133111EBULL;
        tail ^= tail >> 31;
        h ^= tail;
    }
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;
    return h;
}

inline u64 hash_sv(std::string_view sv) noexcept {
    return hash_bytes64(sv.data(), sv.size());
}

inline std::string hex_u64(u64 v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return std::string(buf, 16);
}

// High-resolution monotonic timer
struct Stopwatch {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start_tp;

    Stopwatch() noexcept : start_tp(Clock::now()) {}
    void reset() noexcept { start_tp = Clock::now(); }

    double elapsed_us() const noexcept {
        auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_tp);
        return static_cast<double>(d.count()) / 1000.0;
    }
    double elapsed_ms() const noexcept {
        return elapsed_us() / 1000.0;
    }
    double elapsed_s() const noexcept {
        return elapsed_us() / 1e6;
    }
};

// Zero-copy read-only mmap RAII wrapper
class MmapFile {
public:
    MmapFile() = default;
    ~MmapFile() { close(); }

    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;

    MmapFile(MmapFile&& other) noexcept
        : data_(other.data_), size_(other.size_), fd_(other.fd_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
    }
    MmapFile& operator=(MmapFile&& other) noexcept {
        if (this != &other) {
            close();
            data_ = other.data_;
            size_ = other.size_;
            fd_ = other.fd_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.fd_ = -1;
        }
        return *this;
    }

    bool open(const std::string& path, bool random_access = false, bool willneed = false) {
        close();
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) return false;

        struct stat st {};
        if (::fstat(fd_, &st) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        if (!S_ISREG(st.st_mode)) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        size_ = static_cast<size_t>(st.st_size);
        if (size_ == 0) {
            ::close(fd_);
            fd_ = -1;
            data_ = nullptr;
            return true;
        }

        void* ptr = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (ptr == MAP_FAILED) {
            ::close(fd_);
            fd_ = -1;
            size_ = 0;
            return false;
        }
        data_ = static_cast<const u8*>(ptr);
        if (random_access) {
            ::madvise(ptr, size_, MADV_RANDOM);
        }
        if (willneed) {
            ::madvise(ptr, size_, MADV_WILLNEED);
        }
        // Can close fd after mmap; mapping remains valid.
        ::close(fd_);
        fd_ = -1;
        return true;
    }

    void close() noexcept {
        if (data_ && size_ > 0) {
            ::munmap(const_cast<u8*>(data_), size_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
        data_ = nullptr;
        size_ = 0;
        fd_ = -1;
    }

    bool is_open() const noexcept { return data_ != nullptr || size_ == 0; }
    bool valid() const noexcept { return data_ != nullptr; }
    const u8* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    std::string_view sv() const noexcept {
        return data_ ? std::string_view(reinterpret_cast<const char*>(data_), size_)
                     : std::string_view{};
    }

private:
    const u8* data_ = nullptr;
    size_t size_ = 0;
    int fd_ = -1;
};

// Stat helper returning mtime in nanoseconds
inline i64 stat_mtime_ns(const struct stat& st) noexcept {
#if defined(__APPLE__)
    return static_cast<i64>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    return static_cast<i64>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

// Compute cache/index storage directory for a given repo root
inline std::filesystem::path default_cache_root() {
    if (const char* env = std::getenv("SCRY_CACHE_DIR"); env && *env) {
        return std::filesystem::path(env);
    }
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "scry";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".cache" / "scry";
    }
    return std::filesystem::path("/tmp/scry-cache");
}

inline std::filesystem::path index_dir_for_root(const std::filesystem::path& canonical_root) {
    std::string s = canonical_root.string();
    u64 h1 = hash_sv(s);
    u64 h2 = hash_bytes64(s.data(), s.size(), 0x517CC1B727220A95ULL);
    return default_cache_root() / (hex_u64(h1) + hex_u64(h2));
}

inline std::filesystem::path default_config_root() {
    if (const char* env = std::getenv("SCRY_CONFIG_DIR"); env && *env) {
        return std::filesystem::path(env);
    }
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "scry";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "scry";
    }
    return std::filesystem::path("/tmp/scry-config");
}

inline std::filesystem::path daemon_repos_config_path() {
    return default_config_root() / "repos.txt";
}

inline std::filesystem::path daemon_socket_path() {
    if (const char* env = std::getenv("SCRY_SOCKET"); env && *env) {
        return std::filesystem::path(env);
    }
    if (const char* xdg_rt = std::getenv("XDG_RUNTIME_DIR"); xdg_rt && *xdg_rt) {
        return std::filesystem::path(xdg_rt) / "scryd.sock";
    }
    return std::filesystem::path("/tmp") / ("scryd-" + std::to_string(::getuid()) + ".sock");
}

// Read private dirty RSS in KiB on Linux (/proc/self/smaps_rollup)
inline u64 current_private_dirty_kib() {
    FILE* f = std::fopen("/proc/self/smaps_rollup", "r");
    if (!f) return 0;
    char line[256];
    u64 kb = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "Private_Dirty:", 14) == 0) {
            unsigned long long val = 0;
            if (std::sscanf(line + 14, "%llu", &val) == 1) {
                kb = static_cast<u64>(val);
            }
            break;
        }
    }
    std::fclose(f);
    return kb;
}

inline u64 current_rss_kib() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long long size_p = 0, rss_p = 0;
    if (std::fscanf(f, "%llu %llu", &size_p, &rss_p) != 2) rss_p = 0;
    std::fclose(f);
    long ps = ::sysconf(_SC_PAGESIZE);
    return static_cast<u64>(rss_p) * static_cast<u64>(ps > 0 ? ps : 4096) / 1024;
}

} // namespace scry
