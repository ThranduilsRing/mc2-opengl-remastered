// Native view of mc2-content-ir/1 after selecting one condition/state/LOD.
// TG corner UVs and vertex order survive lowering (stock animation and rigid skins depend on them).
// Frontends own CPU data only; runtime handles belong exclusively to the cooker/consumer.
#pragma once
#include "tgl.h"
#include "fast_hash.h"
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
    // Reserved recipe field; source-only loading leaves this zero.
    uint64_t verifiedCacheKey = 0;
};

// Validate CPU geometry in memory. No disk cache, simulation state or RNG.
bool cook(Asset& asset, std::string_view source);
bool loadCached(Asset& asset, std::string_view source);
// Digest of a source file's bytes (fastHash of the content; equals vfs::contentDigest for the same bytes).
// One [LOAD_LAPS v1] line per call (and resets the accumulators): where a load stage spent its time.
void reportLoadLaps(std::string_view stage, double stageMs, std::string_view mission);
uint64_t sourceBytesDigest(std::string_view sourcePath, std::span<const uint8_t> bytes);
}
