#include "prop_mesh_cache.h"
#include <cmath>
#include <limits>

namespace mc2_prop::cache {
namespace {
constexpr size_t kHeaderBytes = 80;
bool valid(const PropMeshBlob& b) {
    if (b.vertices.size() % kVertexStride || b.indices.size() % 3 || b.triangles != b.indices.size() / 3 ||
        !std::isfinite(b.missesBefore) || !std::isfinite(b.missesAfter) || b.missesBefore < 0 || b.missesAfter < 0) return false;
    const size_t vertices = b.vertices.size() / kVertexStride;
    for (auto i : b.indices) if (i >= vertices) return false;
    size_t end = 0;
    for (const auto& p : b.packets) {
        if (p.firstIndex != end || p.indexCount % 3 || p.indexCount > b.indices.size() - end || p.materialFlags > 1) return false;
        end += p.indexCount;
    }
    return end == b.indices.size();
}
}
bool canAppend(const PropMeshBlob& b, size_t vertexBytes, size_t indexCount, size_t packetCount) {
    constexpr auto u32 = std::numeric_limits<uint32_t>::max();
    constexpr auto i32 = std::numeric_limits<int32_t>::max();
    return valid(b) && vertexBytes % kVertexStride == 0 && vertexBytes / kVertexStride <= static_cast<size_t>(i32) &&
        b.vertices.size() / kVertexStride <= static_cast<size_t>(i32) - vertexBytes / kVertexStride &&
        indexCount <= u32 && b.indices.size() <= u32 - indexCount &&
        packetCount <= u32 && b.packets.size() <= u32 - packetCount;
}
std::vector<uint8_t> encode(uint64_t source, const PropMeshBlob& b) {
    if (!canAppend(b, 0, 0, 0)) return {};
    const uint64_t size = kHeaderBytes + b.vertices.size() + b.indices.size() * 4ull + b.packets.size() * 16ull;
    if (size > kKind.maxBytes) return {};
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(size));
    for (uint32_t x : {0x424d5050u, kVersion, kProducer, kVertexStride}) mc2cache::putLE(out, x, 4);
    for (uint64_t x : {source, static_cast<uint64_t>(0), static_cast<uint64_t>(b.vertices.size()), static_cast<uint64_t>(b.indices.size()),
                      static_cast<uint64_t>(b.packets.size()), b.triangles, std::bit_cast<uint64_t>(b.missesBefore),
                      std::bit_cast<uint64_t>(b.missesAfter)}) mc2cache::putLE(out, x, 8);
    out.insert(out.end(), b.vertices.begin(), b.vertices.end());
    for (auto i : b.indices) mc2cache::putLE(out, i, 4);
    for (const auto& p : b.packets)
        for (auto x : {p.firstIndex, p.indexCount, p.textureSlot, p.materialFlags}) mc2cache::putLE(out, x, 4);
    const auto hash = mc2::content::fastHash(std::span<const uint8_t>(out).subspan(kHeaderBytes));
    for (unsigned i = 0; i < 8; ++i) out[24 + i] = static_cast<uint8_t>(hash >> (8 * i));
    return out;
}
std::optional<PropMeshBlob> decode(uint64_t source, std::span<const uint8_t> bytes) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kKind.maxBytes) return {};
    auto get = [&](size_t offset, unsigned count = 8) { return mc2cache::getLE(bytes, offset, count); };
    if (get(0, 4) != 0x424d5050u || get(4, 4) != kVersion || get(8, 4) != kProducer ||
        get(12, 4) != kVertexStride || get(16) != source ||
        get(24) != mc2::content::fastHash(bytes.subspan(kHeaderBytes))) return {};
    const auto nv = get(32), ni = get(40), np = get(48);
    // Check every count against remaining bytes BEFORE any allocation (including overflow).
    size_t remaining = bytes.size() - kHeaderBytes;
    if (nv % kVertexStride || nv > remaining) return {};
    remaining -= static_cast<size_t>(nv);
    if (ni > remaining / 4) return {};
    remaining -= static_cast<size_t>(ni) * 4;
    if (np > remaining / 16 || np * 16 != remaining) return {};
    PropMeshBlob b;
    b.triangles = get(56);
    b.missesBefore = std::bit_cast<double>(get(64)); b.missesAfter = std::bit_cast<double>(get(72));
    size_t offset = kHeaderBytes;
    b.vertices.assign(bytes.begin() + offset, bytes.begin() + offset + static_cast<size_t>(nv));
    offset += static_cast<size_t>(nv);
    b.indices.resize(static_cast<size_t>(ni));
    for (auto& i : b.indices) { i = static_cast<uint32_t>(get(offset, 4)); offset += 4; }
    b.packets.resize(static_cast<size_t>(np));
    for (auto& p : b.packets) {
        p = {static_cast<uint32_t>(get(offset, 4)), static_cast<uint32_t>(get(offset + 4, 4)),
             static_cast<uint32_t>(get(offset + 8, 4)), static_cast<uint32_t>(get(offset + 12, 4))};
        offset += 16;
    }
    if (!canAppend(b, 0, 0, 0)) return {};
    return b;
}
}
