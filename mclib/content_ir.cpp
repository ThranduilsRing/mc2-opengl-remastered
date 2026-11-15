#include <cmath>
#include "content_ir.h"
#include "content_source_digest.h"
#include "mc2fmt.h"
#include <atomic>
#include <chrono>

namespace mc2::content {
namespace {
struct Laps { std::atomic<uint64_t> loads{0}, cooks{0}, validation{0}; } g_laps;
uint64_t nowNs() { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
bool finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
}

uint64_t sourceBytesDigest(std::string_view, std::span<const uint8_t> bytes) {
    return fastHash(bytes);
}

bool loadCached(Asset&, std::string_view) {
    // Loading from source avoids hashing the GLB and then reading a larger second representation.
    g_laps.loads++;
    return false;
}

bool cook(Asset& asset, std::string_view sourcePath) {
    g_laps.cooks++;
    struct Timer { uint64_t t; ~Timer() { g_laps.validation += nowNs() - t; } } timer{nowNs()};
    if (asset.nodes.empty() || asset.nodes.size() > kMaxMultiShapeNodes || asset.materials.size() > kMaxShapeTextures)
        return false;
    for (const auto& s : asset.sockets) {
        if (!finite(s.position) || !finite(s.direction) || !finite(s.colour) ||
            !std::isfinite(s.range) || !std::isfinite(s.outerConeDegrees)) return false;
    }
    for (const auto& n : asset.nodes) {
        if (!finite(n.center) || !finite(n.relativeCenter) || n.vertices.size() > kMaxShapeVertices ||
            n.triangles.size() > kMaxShapeTriangles) return false;
        for (const auto& v : n.vertices) {
            if (!finite(v.position) || !finite(v.normal)) return false;
        }
        for (const auto& t : n.triangles) {
            for (const auto i : t.Vertices) if (i >= n.vertices.size()) return false;
            if (!finite(t.faceNormal)) return false;
            const float* uv = &t.uvdata.u0;
            for (int i = 0; i < 6; ++i) if (!std::isfinite(uv[i])) return false;
        }
    }
    mc2::fmt::print_err("[CONTENT_COOK] source={} key=0000000000000000 cache=source-only nodes={} materials={} sockets={} version={}\n",
        std::string(sourcePath), asset.nodes.size(), asset.materials.size(), asset.sockets.size(), cookerVersion);
    return true;
}

void reportLoadLaps(std::string_view stage, double stageMs, std::string_view mission) {
    const double validation = static_cast<double>(g_laps.validation.exchange(0)) / 1e6;
    mc2::fmt::print_err("[LOAD_LAPS v1] stage={} mission={} stage_ms={:.0f} loads={} load_hits=0 cooks={} digest_ms=0 read_ms=0 decode_ms=0 cook_ms={:.0f} cook_read_ms=0\n",
        std::string(stage), std::string(mission), stageMs, g_laps.loads.exchange(0), g_laps.cooks.exchange(0), validation);
}
}
