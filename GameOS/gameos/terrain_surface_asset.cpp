#include "terrain_surface_asset.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace mc2terrain {
namespace {
constexpr uint32_t kMagic = 0x31465354; // TSF1
constexpr size_t kHeaderBytes = 48;
constexpr uint64_t kPixelBytes = 16;
std::vector<uint8_t> missionCook;

bool validSize(uint32_t w, uint32_t h) {
    return w > 0 && h > 0 && w <= SurfaceAsset::kMaxSide && h <= SurfaceAsset::kMaxSide;
}

void put(std::vector<uint8_t>& bytes, uint64_t word, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<uint8_t>(word >> (8 * i)));
}
uint64_t get(std::span<const uint8_t> bytes, size_t offset, unsigned count) {
    uint64_t word = 0;
    for (unsigned i = 0; i < count; ++i) word |= uint64_t(bytes[offset + i]) << (8 * i);
    return word;
}
} // namespace

uint64_t surfaceHash(std::span<const uint8_t> bytes, uint64_t seed) {
    for (uint8_t byte : bytes) { seed ^= byte; seed *= 1099511628211ull; }
    return seed;
}

bool validSurfaceAsset(const SurfaceAsset& asset, uint64_t expectedSource) {
    if (!validSize(asset.width, asset.height) || asset.sourceHash != expectedSource || expectedSource == 0 ||
        asset.macroColor.size() != uint64_t(asset.width) * asset.height) return false;
    for (const auto& pixel : asset.macroColor)
        for (float channel : pixel)
            if (!std::isfinite(channel) || channel < 0.0f || channel > 1.0f) return false;
    return true;
}

std::optional<SurfaceAsset> decodeSurfaceAsset(std::span<const uint8_t> bytes, uint64_t expectedSource) {
    if (bytes.size() < kHeaderBytes || get(bytes, 0, 4) != kMagic ||
        get(bytes, 4, 4) != SurfaceAsset::kSchema || get(bytes, 8, 4) != SurfaceAsset::kEvaluator ||
        get(bytes, 20, 4) != SurfaceAsset::kLinear || get(bytes, 24, 8) != expectedSource || expectedSource == 0) return {};
    const auto width = static_cast<uint32_t>(get(bytes, 12, 4));
    const auto height = static_cast<uint32_t>(get(bytes, 16, 4));
    if (!validSize(width, height)) return {};
    const uint64_t payloadBytes = uint64_t(width) * height * kPixelBytes;
    if (get(bytes, 40, 8) != payloadBytes || bytes.size() != kHeaderBytes + payloadBytes ||
        surfaceHash(bytes.subspan(kHeaderBytes)) != get(bytes, 32, 8)) return {};
    SurfaceAsset candidate;
    candidate.width = width; candidate.height = height; candidate.sourceHash = expectedSource;
    candidate.macroColor.resize(size_t(width) * height);
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(candidate.macroColor.data(), bytes.data() + kHeaderBytes, size_t(payloadBytes));
    } else {
        size_t offset = kHeaderBytes;
        for (auto& pixel : candidate.macroColor)
            for (float& channel : pixel) {
                channel = std::bit_cast<float>(static_cast<uint32_t>(get(bytes, offset, 4)));
                offset += 4;
            }
    }
    if (!validSurfaceAsset(candidate, expectedSource)) return {};
    return candidate;
}

std::vector<uint8_t> encodeSurfaceAsset(const SurfaceAsset& asset) {
    if (!validSurfaceAsset(asset, asset.sourceHash)) return {};
    std::vector<uint8_t> bytes;
    bytes.reserve(kHeaderBytes + asset.macroColor.size() * kPixelBytes);
    put(bytes, kMagic, 4); put(bytes, SurfaceAsset::kSchema, 4); put(bytes, SurfaceAsset::kEvaluator, 4);
    put(bytes, asset.width, 4); put(bytes, asset.height, 4); put(bytes, SurfaceAsset::kLinear, 4);
    put(bytes, asset.sourceHash, 8); put(bytes, 0, 8); put(bytes, asset.macroColor.size() * kPixelBytes, 8);
    if constexpr (std::endian::native == std::endian::little) {
        bytes.resize(kHeaderBytes + asset.macroColor.size() * kPixelBytes);
        std::memcpy(bytes.data() + kHeaderBytes, asset.macroColor.data(), asset.macroColor.size() * kPixelBytes);
    } else {
        for (const auto& pixel : asset.macroColor)
            for (float channel : pixel) put(bytes, std::bit_cast<uint32_t>(channel), 4);
    }
    const uint64_t hash = surfaceHash(std::span<const uint8_t>(bytes).subspan(kHeaderBytes));
    for (unsigned i = 0; i < 8; ++i) bytes[32 + i] = static_cast<uint8_t>(hash >> (8 * i));
    return bytes;
}

namespace {
constexpr size_t kCacheMaxFiles = 6; // each entry is up to ~256 MiB
std::filesystem::path cacheFile(const std::string& dir, uint64_t source) {
    char name[40];
    std::snprintf(name, sizeof name, "macro-%016llx.tsf", static_cast<unsigned long long>(source));
    return std::filesystem::path(dir) / name;
}
}

std::optional<SurfaceAsset> loadCachedSurfaceAsset(const std::string& dir, uint64_t expectedSource) {
    if (dir.empty() || expectedSource == 0) return {};
    const auto path = cacheFile(dir, expectedSource);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < kHeaderBytes || size > SurfaceAsset::kMaxBytes) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return {};
    const bool read = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    if (!read) return {};
    return decodeSurfaceAsset(bytes, expectedSource);
}

bool storeCachedSurfaceAsset(const std::string& dir, const SurfaceAsset& asset) {
    if (dir.empty()) return false;
    const auto bytes = encodeSurfaceAsset(asset);
    if (bytes.empty()) return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto path = cacheFile(dir, asset.sourceHash);
    static std::atomic<uint32_t> counter{0};
    auto tmp = path;
    tmp += ".tmp" + std::to_string(counter.fetch_add(1)) + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) return false;
    const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) { fs::remove(tmp, ec); return false; }
    fs::rename(tmp, path, ec); // atomic replace: a crash leaves only a .tmp file, never a partial entry
    if (ec) { fs::remove(tmp, ec); return false; }
    // Bound disk use: keep the newest kCacheMaxFiles entries, sweep stale temp files.
    std::vector<std::pair<fs::file_time_type, fs::path>> entries;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().filename().string().rfind("macro-", 0) != 0) continue;
        std::error_code e2;
        const auto t = e.last_write_time(e2);
        if (e2) continue;
        if (e.path().extension() == ".tsf") entries.emplace_back(t, e.path());
        else if (fs::file_time_type::clock::now() - t > std::chrono::hours(1)) fs::remove(e.path(), e2);
    }
    if (entries.size() > kCacheMaxFiles) {
        std::sort(entries.begin(), entries.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        for (size_t i = kCacheMaxFiles; i < entries.size(); ++i)
            if (entries[i].second != path) fs::remove(entries[i].second, ec);
    }
    return true;
}

void setSurfaceMissionCook(std::vector<uint8_t> candidate) {
    missionCook = candidate.size() <= SurfaceAsset::kMaxBytes ? std::move(candidate) : std::vector<uint8_t>{};
}

std::span<const uint8_t> surfaceMissionCook() { return missionCook; }
} // namespace mc2terrain
