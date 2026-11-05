// Native view of mc2-content-ir/1 after selecting one condition/state/LOD.
// TG corner UVs and vertex order survive lowering (stock animation and rigid skins depend on them).
// Frontends own CPU data only; runtime handles belong exclusively to the cooker/consumer.
#pragma once
#include "tgl.h"
#include "content_socket.h"
#include <string>
#include <span>
#include <string_view>
#include <vector>

namespace mc2::content {
inline constexpr uint32_t cookerVersion = 5;  // 5: fastHash replaces byte-wise FNV in keys and digests
struct Material {
    std::string textureName;
    bool alpha = false;
};
struct Node {
    std::string name, parent = "None";
    glm::vec3 center{0}, relativeCenter{0};
    bool helper = false;
    std::vector<TG_TypeVertex> vertices;
    std::vector<TG_TypeTriangle> triangles;
};
struct Asset {
    std::vector<Node> nodes;
    std::vector<Material> materials;
    std::vector<Socket> sockets;
    // Compatibility bounding policy is independent of the render override: pathing/LOS
    // continue to consume the original stock payload, never replacement triangles.
    bool legacyBounds = false;
    int ground = 0;
    std::string parameters;
};

// Validate and publish a content-addressed CPU recipe. Input identity includes resolved
// source bytes, the complete IR payload and the cooker version. No simulation state/RNG.
bool cook(Asset& asset, std::string_view source);
bool loadCached(Asset& asset, std::string_view source);
// Fast 8-byte-word hash used for cache keys and source digests (replaces byte-wise FNV-1a).
uint64_t fastHash(std::span<const uint8_t> bytes, uint64_t seed = 14695981039346656037ull);
// Digest of a source file's bytes, memoised per path+size so cache lookup, cook and the
// importer's IO layer hash each file once per session.
uint64_t sourceBytesDigest(std::string_view sourcePath, std::span<const uint8_t> bytes);
}
