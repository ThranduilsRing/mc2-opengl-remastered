#include <cmath>
#include "content_ir.h"
#include "content_source_digest.h"
#include "file.h"
#include "vfs.h"
#include "../GameOS/gameos/cook_cache.h"
#include "../GameOS/gameos/diagnostic_trace.h"
#include "mc2fmt.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <unordered_map>
#include <atomic>
#include <chrono>

namespace mc2::content {
namespace {
// [LOAD_LAPS v1] accumulators (nanoseconds), summed over every shape load in a mission load.
struct Laps { std::atomic<uint64_t> digest{0}, read{0}, decode{0}, cookTotal{0}, cookRead{0}, loads{0}, hits{0}, cooks{0}; } g_laps;
uint64_t nowNs() { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

constexpr mc2cache::CacheKind kind{"model-", ".mci", 4096, 128ull << 20};
constexpr std::string_view directory = "cache/content-model";
uint64_t hash(std::span<const uint8_t> bytes, uint64_t h = 14695981039346656037ull) { return fastHash(bytes, h); }
template<class T> void append(std::vector<uint8_t>& bytes, const T& value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(T));
}
void text(std::vector<uint8_t>& bytes, std::string_view s) {
    append(bytes, static_cast<uint32_t>(s.size()));
    bytes.insert(bytes.end(), s.begin(), s.end());
}
bool finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

uint64_t sourceKey(const Asset& asset, std::string_view sourcePath) {
    const std::string sourceStorage(sourcePath);
    const char* source = sourceStorage.empty() ? nullptr : sourceStorage.c_str();
    if (!source) return 0;
    std::vector<uint8_t> params;
    append(params, cookerVersion); append(params, asset.legacyBounds); append(params, asset.ground);
    text(params, asset.parameters);
    uint64_t key = hash(params);
    // Content identity comes from the VFS: the digest of the bytes the winning candidate serves, memoised by
    // (path, size, mtime) and cleared on every VFS rebuild; never by path+size alone.
    const uint64_t digest = mc2::vfs::contentDigest(sourceStorage);
    if (!digest) return 0;
    key = hash(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&digest), sizeof(digest)), key);
    return key ? key : 1;
}

// Bounded reader for this cooker's private cache; never deserialize native pointers/handles.
struct Reader {
    std::span<const uint8_t> data;
    size_t pos = 0;
    bool ok = true;
    template<class T> T get() {
        T v{};
        if (!ok || sizeof(T) > data.size() - pos) { ok = false; return v; }
        std::memcpy(&v, data.data() + pos, sizeof(T)); pos += sizeof(T); return v;
    }
    std::string string() {
        const auto count = get<uint32_t>();
        if (!ok || count > 8192 || count > data.size() - pos) { ok = false; return {}; }
        std::string value(reinterpret_cast<const char*>(data.data() + pos), count); pos += count; return value;
    }
};
}

uint64_t sourceBytesDigest(std::string_view, std::span<const uint8_t> bytes) {
    // The bytes are already in memory: hash them (a path+size memo here was a stale-digest hazard for a
    // same-size edit). Same value as vfs::contentDigest for the same bytes.
    return fastHash(bytes);
}

bool loadCached(Asset& asset, std::string_view sourcePath) {
    const std::string sourceStorage(sourcePath);
    const char* source = sourceStorage.empty() ? nullptr : sourceStorage.c_str();
    // glTF may have external buffers: its decoded IR contributes to the key after
    // resolving them. The self-contained formats can bypass decoding on a cache hit.
    if (!source || std::string_view(source).ends_with(".gltf") || std::string_view(source).ends_with(".fbx")) return false;
    g_laps.loads++;
    const uint64_t t0 = nowNs();
    const auto key = sourceKey(asset, sourcePath);
    const uint64_t t1 = nowNs();
    g_laps.digest += t1 - t0;
    if (!key) return false;
    const auto bytes = mc2cache::readEntry(directory, kind, key);
    const uint64_t t2 = nowNs();
    g_laps.read += t2 - t1;
    if (!bytes || bytes->size() < 8) return false;
    struct DecodeTimer { uint64_t t; ~DecodeTimer() { g_laps.decode += nowNs() - t; } } decodeTimer{t2};
    Reader r{*bytes};
    const auto digest = r.get<uint64_t>();
    if (hash(r.data.subspan(8)) != digest || r.get<uint32_t>() != cookerVersion ||
        r.get<uint32_t>() != sizeof(TG_TypeVertex) || r.get<uint32_t>() != sizeof(TG_TypeTriangle)) return false;
    const auto nodes = r.get<uint32_t>(), materials = r.get<uint32_t>();
    const auto sockets = r.get<uint32_t>();
    if (!nodes || nodes > kMaxMultiShapeNodes || materials > kMaxShapeTextures || sockets > kMaxMultiShapeNodes) return false;
    Asset decoded;
    decoded.legacyBounds = r.get<bool>(); decoded.ground = r.get<int>(); decoded.parameters = asset.parameters;
    for (uint32_t i = 0; i < materials && r.ok; ++i) {
        Material m; m.textureName = r.string(); m.alpha = r.get<bool>(); decoded.materials.push_back(std::move(m));
    }
    for (uint32_t i = 0; i < sockets && r.ok; ++i) {
        Socket s; s.name = r.string(); s.kind = r.string(); s.parent = r.string();
        s.type = r.string(); s.intensityClass = r.string();
        s.position = r.get<glm::vec3>(); s.direction = r.get<glm::vec3>(); s.colour = r.get<glm::vec3>();
        s.range = r.get<float>(); s.outerConeDegrees = r.get<float>();
        s.active = r.get<bool>(); s.lightsOutOnDestroy = r.get<bool>();
        decoded.sockets.push_back(std::move(s));
    }
    for (uint32_t i = 0; i < nodes && r.ok; ++i) {
        Node n; n.name = r.string(); n.parent = r.string();
        n.center = r.get<glm::vec3>(); n.relativeCenter = r.get<glm::vec3>(); n.helper = r.get<bool>();
        const auto nv = r.get<uint32_t>(), nt = r.get<uint32_t>();
        if (nv > kMaxShapeVertices || nt > kMaxShapeTriangles || static_cast<uint64_t>(nv) * 28 + static_cast<uint64_t>(nt) * 56 > r.data.size() - r.pos)
            return false;
        n.vertices.resize(nv); n.triangles.resize(nt);
        for (auto& v : n.vertices) { v.position = r.get<glm::vec3>(); v.normal = r.get<glm::vec3>(); v.aRGBLight = r.get<DWORD>(); }
        for (auto& t : n.triangles) {
            for (auto& v : t.Vertices) v = r.get<DWORD>();
            t.localTextureHandle = r.get<DWORD>(); t.renderStateFlags = r.get<DWORD>();
            t.uvdata = r.get<TG_UVData>(); t.faceNormal = r.get<glm::vec3>();
        }
        decoded.nodes.push_back(std::move(n));
    }
    if (!r.ok || r.pos != r.data.size()) return false;
    asset = std::move(decoded);
    g_laps.hits++;
    return true;
}

bool cook(Asset& asset, std::string_view sourcePath) {
    g_laps.cooks++;
    struct CookTimer { uint64_t t; ~CookTimer() { g_laps.cookTotal += nowNs() - t; } } cookTimer{nowNs()};
    const std::string sourceStorage(sourcePath);
    const char* source = sourceStorage.empty() ? nullptr : sourceStorage.c_str();
    if (asset.nodes.empty() || asset.nodes.size() > kMaxMultiShapeNodes || asset.materials.size() > kMaxShapeTextures)
        return false;
    std::vector<uint8_t> recipe;
    append(recipe, cookerVersion);
    append(recipe, static_cast<uint32_t>(sizeof(TG_TypeVertex)));
    append(recipe, static_cast<uint32_t>(sizeof(TG_TypeTriangle)));
    append(recipe, static_cast<uint32_t>(asset.nodes.size()));
    append(recipe, static_cast<uint32_t>(asset.materials.size()));
    append(recipe, static_cast<uint32_t>(asset.sockets.size()));
    append(recipe, asset.legacyBounds);
    append(recipe, asset.ground);
    for (const auto& m : asset.materials) { text(recipe, m.textureName); append(recipe, m.alpha); }
    for (const auto& s : asset.sockets) {
        if (!finite(s.position) || !finite(s.direction) || !finite(s.colour) ||
            !std::isfinite(s.range) || !std::isfinite(s.outerConeDegrees)) return false;
        text(recipe, s.name); text(recipe, s.kind); text(recipe, s.parent);
        text(recipe, s.type); text(recipe, s.intensityClass);
        append(recipe, s.position); append(recipe, s.direction); append(recipe, s.colour);
        append(recipe, s.range); append(recipe, s.outerConeDegrees);
        append(recipe, s.active); append(recipe, s.lightsOutOnDestroy);
    }
    for (const auto& n : asset.nodes) {
        if (!finite(n.center) || !finite(n.relativeCenter) || n.vertices.size() > kMaxShapeVertices ||
            n.triangles.size() > kMaxShapeTriangles) return false;
        text(recipe, n.name); text(recipe, n.parent);
        append(recipe, n.center); append(recipe, n.relativeCenter); append(recipe, n.helper);
        append(recipe, static_cast<uint32_t>(n.vertices.size()));
        append(recipe, static_cast<uint32_t>(n.triangles.size()));
        for (const auto& v : n.vertices) {
            if (!finite(v.position) || !finite(v.normal)) return false;
            // Field serialization excludes native padding from content identity.
            append(recipe, v.position); append(recipe, v.normal); append(recipe, v.aRGBLight);
        }
        for (const auto& t : n.triangles) {
            for (const auto i : t.Vertices) if (i >= n.vertices.size()) return false;
            if (!finite(t.faceNormal)) return false;
            const float* uv = &t.uvdata.u0;
            for (int i = 0; i < 6; ++i) if (!std::isfinite(uv[i])) return false;
            for (const auto i : t.Vertices) append(recipe, i);
            append(recipe, t.localTextureHandle); append(recipe, t.renderStateFlags);
            append(recipe, t.uvdata); append(recipe, t.faceNormal);
        }
    }
    uint64_t key = sourceKey(asset, sourcePath);
    if (!key) key = hash(recipe);
    if (source && (std::string_view(source).ends_with(".gltf") || std::string_view(source).ends_with(".fbx")))
        key = hash(recipe, key);
    std::vector<uint8_t> blob;
    append(blob, hash(recipe)); blob.insert(blob.end(), recipe.begin(), recipe.end());
    const uint64_t tr = nowNs();
    const auto cached = mc2cache::readEntry(directory, kind, key);
    g_laps.cookRead += nowNs() - tr;
    const bool hit = cached && *cached == blob;
    if (!hit && !mc2cache::writeEntry(directory, kind, key, blob)) {
        mc2::fmt::print_err("[CONTENT_COOK_ERROR] source={} key={:016x} cache write failed\n",
            source ? source : "", key);
        return false;
    }
    mc2::fmt::print_err("[CONTENT_COOK] source={} key={:016x} cache={} nodes={} materials={} sockets={} version={}\n",
        source ? source : "", key, hit ? "hit" : "miss", asset.nodes.size(), asset.materials.size(), asset.sockets.size(), cookerVersion);
    return true;
}

void reportLoadLaps(std::string_view stage, double stageMs, std::string_view mission) {
    if (!mc2_diag::tagRequested("LOAD_LAPS")) return;   // MC2_DIAG_TAGS=LOAD_LAPS; accumulators are cheap and never reset when off
    const auto ms = [](std::atomic<uint64_t>& a) { return static_cast<double>(a.exchange(0)) / 1e6; };
    const auto n = [](std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.exchange(0)); };
    const double digest = ms(g_laps.digest), read = ms(g_laps.read), decode = ms(g_laps.decode);
    const double cookTotal = ms(g_laps.cookTotal), cookRead = ms(g_laps.cookRead);
    mc2::fmt::print_err("[LOAD_LAPS v1] stage={} mission={} stage_ms={:.0f} loads={} load_hits={} cooks={} digest_ms={:.0f} read_ms={:.0f} decode_ms={:.0f} cook_ms={:.0f} cook_read_ms={:.0f}\n",
        std::string(stage), std::string(mission), stageMs, n(g_laps.loads), n(g_laps.hits), n(g_laps.cooks), digest, read, decode, cookTotal, cookRead);
}
}
