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
    // Set by loadCached on a digest-verified cache hit: the cache key this payload was read under. cook() skips
    // re-serializing, re-hashing and re-reading the entry it just came from when it still derives the same key.
    uint64_t verifiedCacheKey = 0;
};

// Validate and publish a content-addressed CPU recipe. Input identity includes resolved
// source bytes, the complete IR payload and the cooker version. No simulation state/RNG.
bool cook(Asset& asset, std::string_view source);
bool loadCached(Asset& asset, std::string_view source);
// Digest of a source file's bytes (fastHash of the content; equals vfs::contentDigest for the same bytes).
// One [LOAD_LAPS v1] line per call (and resets the accumulators): where a load stage spent its time.
void reportLoadLaps(std::string_view stage, double stageMs, std::string_view mission);
// TEMP sub-laps: kind 0=tree 1=terrain object 2=building; phase 0=getAppearance 1=appearance->init 2=rest.
void tempLapAdd(int kind, int phase, uint64_t ns);
uint64_t tempLapNow();
uint64_t sourceBytesDigest(std::string_view sourcePath, std::span<const uint8_t> bytes);
}
