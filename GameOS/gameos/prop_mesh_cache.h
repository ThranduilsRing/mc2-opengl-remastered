#pragma once
// Type-local GPU geometry; no pointers, batcher state or graphics API dependencies.
#include "cook_cache.h"
#include "../../mclib/fast_hash.h"
#include <array>
#include <bit>
#include <optional>

namespace mc2_prop::cache {
inline constexpr uint32_t kVersion = 1, kVertexStride = 40;
// Bump for packing, dedupe, meshoptimizer version/options or material encoding changes.
inline constexpr uint32_t kProducer = 1; // meshoptimizer 1.3, cache/overdraw(1.05)/fetch
// 2048 types keeps roughly three mc2_01-sized sets; cap individual outlier types at 128 MiB.
inline constexpr mc2cache::CacheKind kKind{"prop-", ".pmb", 2048, 128ull << 20};
inline constexpr std::string_view kDirectory = "cache/prop-mesh";
struct Packet {
    uint32_t firstIndex, indexCount, textureSlot, materialFlags;
};
struct PropMeshBlob {
    std::vector<uint8_t> vertices;
    std::vector<uint32_t> indices;
    std::vector<Packet> packets;
    uint64_t triangles = 0;
    double missesBefore = 0, missesAfter = 0; // triangle-weighted ACMR, preserves accumulation
};
// Source identity is probabilistic (64-bit fastHash), NOT a collision-free guarantee.
// Stored separately from the payload checksum and compared to the requested source.
template<class Vertex, class Triangle>
uint64_t key(std::span<const Vertex> vertices, std::span<const Triangle> triangles,
             bool selfLitTwoSided, bool alphaTestOn) {
    std::vector<uint8_t> header;
    for (uint32_t v : {kVersion, kProducer, kVertexStride, static_cast<uint32_t>(selfLitTwoSided), static_cast<uint32_t>(alphaTestOn)})
        mc2cache::putLE(header, v, 4);
    mc2cache::putLE(header, vertices.size(), 8);
    mc2cache::putLE(header, triangles.size(), 8);
    auto hash = mc2::content::fastHash(header);
    for (const auto& v : vertices) {
        const std::array<uint32_t, 7> fields{std::bit_cast<uint32_t>(v.position.x), std::bit_cast<uint32_t>(v.position.y),
            std::bit_cast<uint32_t>(v.position.z), std::bit_cast<uint32_t>(v.normal.x), std::bit_cast<uint32_t>(v.normal.y),
            std::bit_cast<uint32_t>(v.normal.z), v.aRGBLight};
        hash = mc2::content::fastHash({reinterpret_cast<const uint8_t*>(fields.data()), sizeof(fields)}, hash);
    }
    for (const auto& t : triangles) {
        const std::array<uint32_t, 10> fields{t.Vertices[0], t.Vertices[1], t.Vertices[2], t.localTextureHandle,
            std::bit_cast<uint32_t>(t.uvdata.u0), std::bit_cast<uint32_t>(t.uvdata.v0),
            std::bit_cast<uint32_t>(t.uvdata.u1), std::bit_cast<uint32_t>(t.uvdata.v1),
            std::bit_cast<uint32_t>(t.uvdata.u2), std::bit_cast<uint32_t>(t.uvdata.v2)};
        hash = mc2::content::fastHash({reinterpret_cast<const uint8_t*>(fields.data()), sizeof(fields)}, hash);
    }
    return hash;
}
std::vector<uint8_t> encode(uint64_t source, const PropMeshBlob& blob);
std::optional<PropMeshBlob> decode(uint64_t source, std::span<const uint8_t> bytes);
// Validate the complete blob and destination bounds before any staging mutation.
bool canAppend(const PropMeshBlob& blob, size_t vertexBytes, size_t indexCount, size_t packetCount);
}
