// cook_cache.cpp -- see the header.
#include "cook_cache.h"
#include "mc2fmt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <system_error>
#include <utility>

namespace mc2cache {

std::filesystem::path entryPath(std::string_view dir, const CacheKind& kind, uint64_t key) {
    char hex[20];
    mc2::fmt::to_buf(hex, "{:016x}", key);
    std::string name(kind.prefix);
    name += hex;
    name += kind.ext;
    return std::filesystem::path(dir) / name;
}

std::optional<std::vector<uint8_t>> readEntry(std::string_view dir, const CacheKind& kind, uint64_t key) {
    if (dir.empty() || key == 0) return {};
    const auto path = entryPath(dir, kind, key);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || (kind.maxBytes != 0 && size > kind.maxBytes)) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return {};
    const bool read = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    if (!read) return {};
    return bytes;
}

bool writeEntry(std::string_view dir, const CacheKind& kind, uint64_t key, std::span<const uint8_t> bytes) {
    if (dir.empty() || bytes.empty()) return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dirPath{dir};
    fs::create_directories(dirPath, ec);
    const auto path = entryPath(dir, kind, key);
    static std::atomic<uint32_t> counter{0};
    auto tmp = path;
    tmp += ".tmp" + std::to_string(counter.fetch_add(1)) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) return false;
    const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) { fs::remove(tmp, ec); return false; }
    fs::rename(tmp, path, ec);   // atomic replace: a crash leaves only a .tmp file, never a partial entry
    if (ec) { fs::remove(tmp, ec); return false; }
    // Bound disk use per kind: keep the newest maxFiles entries, sweep this kind's stale temp files.
    std::vector<std::pair<fs::file_time_type, fs::path>> entries;
    for (const auto& e : fs::directory_iterator(dirPath, ec)) {
        if (e.path().filename().string().rfind(std::string(kind.prefix), 0) != 0) continue;
        std::error_code e2;
        const auto t = e.last_write_time(e2);
        if (e2) continue;
        if (e.path().extension() == kind.ext) entries.emplace_back(t, e.path());
        else if (fs::file_time_type::clock::now() - t > std::chrono::hours(1)) fs::remove(e.path(), e2);
    }
    if (entries.size() > kind.maxFiles) {
        // A successful write is newest by operation order, even when filesystem
        // timestamps tie or the wall clock moves backward.
        std::ranges::sort(entries, [&path](const auto& a, const auto& b) {
            const auto& [aTime, aPath] = a;
            const auto& [bTime, bPath] = b;
            const bool aNew = aPath == path;
            const bool bNew = bPath == path;
            if (aNew || bNew) return aNew && !bNew;
            return aTime == bTime ? aPath > bPath : aTime > bTime;
        });
        for (size_t i = kind.maxFiles; i < entries.size(); ++i)
            if (const auto& victim = std::get<1>(entries[i]); victim != path) fs::remove(victim, ec);
    }
    return true;
}

}  // namespace mc2cache
