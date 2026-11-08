// vk_rt_scene.cpp -- the Vulkan half of the persistent ray-visible scene (see vk_rt_scene.h for the policy). Owns, in IslandSlot::RtScene: one compacted BLAS
// per static-prop type, one per terrain chunk, and the TLAS over the whole registry. Every build is a blocking one-shot; a replacement set is built into new
// owners and published only when the TLAS over it was built, then the replaced TLAS and BLASes retire through VulkanContext::retire.
#include "vk_rt_scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include "Gates/gate.h"
#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"
#include "../../RenderWorld/RenderWorld.h"
#include "gos_static_prop_registry.h"
#include "utils/mc2log.h"
#include "vk_one_shot.h"
#include "vk_rt_acceleration.h"
#include "vk_rt_selftest.h"
#include "vulkan_color_prop_island.h"
#include "vulkan_context.h"
#include "vulkan_shadow_island_live.h"

namespace mc2vk::rt::scene {

namespace {

constexpr VkBufferUsageFlags kBuildInput = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
constexpr uint64_t kNone = ~0ull;

struct ProbePoint {
    float x = 0, y = 0, h = 0;
};

struct BuildStats {
    uint64_t builtBytes = 0;     // before compaction
    uint64_t finalBytes = 0;     // after
    uint32_t compacted = 0;
    uint32_t buildCalls = 0;     // vkCmdBuildAccelerationStructuresKHR calls for the BLASes (batched)
    uint64_t scratchSpan = 0;    // the largest batch's scratch span
};

class State final : public IslandState {
public:
    ScratchArena scratch;
    std::vector<AccelStructure> propBlas;           // by typeID; empty: no geometry (or not built yet)
    std::vector<uint8_t> propTried;                 // by typeID: a build was attempted with the current VBO
    std::vector<AccelStructure> terrainBlas;        // by chunk
    AccelStructure tlas;
    std::vector<GpuStaticPropInstance> instances;   // the registry snapshot the records point into
    std::vector<uint32_t> objectIds;                // per instance
    uint64_t propVbo = 0, propGen = kNone, propGenFailed = kNone;
    uint64_t terrainKeyLive = kNone, terrainKeyFailed = kNone;
    std::vector<ProbePoint> terrainProbe;
    uint32_t tlasInstances = 0, tlasProps = 0, dropped = 0;
    bool probed = false;

    static bool exists() {
        return mc2vk::engineVulkanContextValid() && mc2vk::getOrCreateEngineVulkanContext().islandSlot(IslandSlot::RtScene) != nullptr;
    }
    static State& instance() {
        auto& slot = mc2vk::getOrCreateEngineVulkanContext().islandSlot(IslandSlot::RtScene);
        if (!slot) slot = std::make_unique<State>();
        return static_cast<State&>(*slot);
    }
};

VulkanContext* rayContext() {
    if (!mc2vk::engineVulkanContextValid()) return nullptr;
    VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    return (ctx.device() != VK_NULL_HANDLE && ctx.rt().accel && ctx.rt().rayQuery) ? &ctx : nullptr;
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

template <class Seq>
void retireEach(VulkanContext& ctx, Seq& seq) {
    for (auto& a : seq)
        if (a) ctx.retire(std::move(a));
    seq.clear();
}

// Builds every geometry into `out` (same order) with PREFER_FAST_TRACE | ALLOW_COMPACTION, then compacts those that save >= 10%. All or nothing: on any
// failure `out` is retired and false returned. A geometry with no triangles yields an empty structure.
bool buildStatic(VulkanContext& ctx, State& s, const std::vector<TriangleGeometry>& geos, std::string_view name, std::vector<AccelStructure>& out,
                 BuildStats& st) {
    out.clear();
    out.resize(geos.size());
    if (geos.empty()) return true;
    BuildFlags flags;
    flags.allowCompaction = true;
    OwnedQueryPool qp = createCompactionQueryPool(ctx, static_cast<uint32_t>(geos.size()));
    if (!qp) return false;
    bool ok = true;
    const bool submitted = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
        recordBarrier(ctx, cmd, Phase::Upload, Phase::Blas);
        // One build call per scratch group, each BLAS on its own scratch sub-range (scratchBatchPlan), instead of one call and one barrier per BLAS.
        uint32_t calls = 0;
        VkDeviceSize span = 0;
        if (!recordBlasBuildBatch(ctx, cmd, geos, flags, name, out, s.scratch, &calls, &span)) {
            ok = false;
            return;
        }
        st.buildCalls += calls;
        st.scratchSpan = std::max<uint64_t>(st.scratchSpan, span);
        recordBarrier(ctx, cmd, Phase::Blas, Phase::Tlas);   // the compaction query below reads the built structures
        for (size_t i = 0; i < geos.size(); ++i)
            if (out[i]) recordCompactionQuery(ctx, cmd, qp.get(), static_cast<uint32_t>(i), out[i]);
    }, "rt_scene blas");
    s.scratch.retireStale(ctx);
    if (!submitted || !ok) {
        retireEach(ctx, out);
        return false;
    }
    std::vector<VkDeviceSize> sizes(geos.size(), 0);
    for (size_t i = 0; i < geos.size(); ++i) {
        if (!out[i]) continue;
        st.builtBytes += out[i].size();
        if (const std::optional<VkDeviceSize> c = readCompactedSize(ctx, qp.get(), static_cast<uint32_t>(i)); c && worthCompacting(out[i].size(), *c))
            sizes[i] = *c;
    }
    std::vector<AccelStructure> compactAs(geos.size());   // not "small": windows.h defines it as char
    bool copyOk = true;
    if (std::any_of(sizes.begin(), sizes.end(), [](VkDeviceSize v) { return v != 0; })) {
        const bool copied = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
            for (size_t i = 0; i < geos.size(); ++i)
                if (sizes[i] && !recordCompactCopy(ctx, cmd, out[i], sizes[i], name, compactAs[i])) sizes[i] = 0;   // keep the uncompacted one
            recordBarrier(ctx, cmd, Phase::Copy, Phase::Tlas);
        }, "rt_scene compact");
        copyOk = copied;
    }
    for (size_t i = 0; i < geos.size(); ++i) {
        if (copyOk && compactAs[i]) {
            ctx.retire(std::move(out[i]));
            out[i] = std::move(compactAs[i]);
            ++st.compacted;
        } else if (compactAs[i]) {
            ctx.retire(std::move(compactAs[i]));
        }
        if (out[i]) st.finalBytes += out[i].size();
    }
    return true;
}

// Builds the TLAS over `terrain` + the prop BLASes (`props` replaces s.propBlas entries where non-empty) into a new owner. Nothing is published here.
std::optional<AccelStructure> buildTlas(VulkanContext& ctx, State& s, const std::vector<AccelStructure>& terrain,
                                        const std::vector<std::pair<uint32_t, AccelStructure>>& added, uint32_t& instanceCount, uint32_t& propCount,
                                        uint32_t& dropped) {
    std::vector<PropPlacement> placements;
    placements.reserve(s.instances.size());
    for (size_t i = 0; i < s.instances.size(); ++i) placements.push_back({s.instances[i].typeID, s.objectIds[i], s.instances[i].modelMatrix});
    const std::vector<Record> records = buildRecords(placements, static_cast<uint32_t>(terrain.size()), &dropped);
    auto propAddress = [&](uint32_t type) -> VkDeviceAddress {
        for (const auto& [t, a] : added)
            if (t == type) return a.address();
        return (type < s.propBlas.size() && s.propBlas[type]) ? s.propBlas[type].address() : 0;
    };
    std::vector<VkAccelerationStructureInstanceKHR> inst;
    inst.reserve(records.size());
    propCount = 0;
    for (const Record& r : records) {
        VkDeviceAddress blas = 0;
        if (r.key.kind == Kind::Terrain) blas = (r.key.ordinal < terrain.size() && terrain[r.key.ordinal]) ? terrain[r.key.ordinal].address() : 0;
        else blas = propAddress(r.typeId);
        if (blas == 0) continue;   // a type or chunk with no triangles
        inst.push_back(makeInstance(blas, r.transform, customIndexOf(r.key), kMaskStatic));
        propCount += r.key.kind == Kind::StaticProp ? 1u : 0u;
    }
    instanceCount = static_cast<uint32_t>(inst.size());
    AccelStructure tlas;
    if (inst.empty()) return tlas;   // an empty scene: nothing to trace, still a valid (empty) snapshot
    OwnedBuffer buf;
    if (!buf.createFilled(ctx.allocator(), ctx.device(), inst.data(), inst.size() * sizeof(inst[0]), kBuildInput, "rt_scene.instances")) return std::nullopt;
    // Fable p34 section 3: the TLAS is PREFER_FAST_BUILD and fully rebuilt (never refit). The static scene rebuilds it on every membership change; the
    // per-frame rebuild in the frame command buffer arrives with movers (part 2), the first members that change every frame.
    BuildFlags flags;
    flags.preferFastTrace = false;
    bool ok = false;
    const bool submitted = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
        recordBarrier(ctx, cmd, Phase::Upload, Phase::Tlas);
        if (!recordTlasBuild(ctx, cmd, buf.deviceAddress(), instanceCount, flags, "rt_scene.tlas", tlas, s.scratch)) return;
        recordBarrier(ctx, cmd, Phase::Tlas, Phase::Consumer);
        ok = true;
    }, "rt_scene tlas");
    s.scratch.retireStale(ctx);
    ctx.retire(std::move(buf));
    if (!submitted || !ok) {
        if (tlas) ctx.retire(std::move(tlas));
        return std::nullopt;
    }
    return tlas;
}

void logPublish(const State& s, std::string_view why, const BuildStats& st, double ms) {
    uint64_t blasBytes = 0;
    uint32_t types = 0;
    for (const AccelStructure& a : s.terrainBlas) blasBytes += a ? a.size() : 0;
    for (const AccelStructure& a : s.propBlas)
        if (a) {
            blasBytes += a.size();
            ++types;
        }
    // Byte ledger: every resident RT byte by kind. instance_bytes is the TLAS build input (64 per instance, uploaded per build, retired after it).
    const uint64_t tlasBytes = s.tlas ? s.tlas.size() : 0, instanceBytes = static_cast<uint64_t>(s.tlasInstances) * kInstanceBytes;
    MC2_EVENT(Vk, "VK_RT_SCENE", 1, "publish why={} tlas_instances={} terrain_tiles={} props={} prop_types={} dropped={} blas_bytes={} built_bytes={} compacted={} "
         "compacted_bytes={} tlas_bytes={} scratch_bytes={} instance_bytes={} total_bytes={} blas_build_calls={} batch_scratch_bytes={} build_ms={:.1f}",
         why, s.tlasInstances, s.terrainBlas.size(), s.tlasProps, types, s.dropped, blasBytes, st.builtBytes, st.compacted, st.finalBytes, tlasBytes,
         s.scratch.capacity(), instanceBytes, blasBytes + tlasBytes + s.scratch.capacity() + instanceBytes, st.buildCalls, st.scratchSpan, ms);
}

// MC2_VK_RT_SELFTEST: once the scene holds terrain and props, fixed downward rays at known props and terrain vertices; logs every hit.
void probe(VulkanContext& ctx, State& s) {
    if (s.probed || !s.tlas || s.terrainBlas.empty() || s.tlasProps == 0 || !mc2::gate::get<"MC2_VK_RT_SELFTEST">()) return;
    s.probed = true;
    constexpr float kTop = 20000.0f;
    std::vector<float> rays;
    std::vector<uint32_t> expect;
    std::vector<float> expectT;
    const size_t step = std::max<size_t>(1, s.instances.size() / 8);
    for (size_t i = 0; i < s.instances.size() && expect.size() < 8; i += step) {
        if (s.objectIds[i] == kTerrainObjectId || !customIndexFits(s.objectIds[i])) continue;
        float t[12];
        propTransform(s.instances[i].modelMatrix, t);
        rays.insert(rays.end(), {t[3], t[7], kTop, 0.0f, 0.0f, 0.0f, -1.0f, 2.0f * kTop});
        expect.push_back(s.objectIds[i]);
        expectT.push_back(-1.0f);
    }
    const size_t props = expect.size();
    for (const ProbePoint& p : s.terrainProbe) {
        rays.insert(rays.end(), {p.x, p.y, kTop, 0.0f, 0.0f, 0.0f, -1.0f, 2.0f * kTop});
        expect.push_back(kTerrainObjectId);
        expectT.push_back(kTop - p.h);
    }
    const uint32_t n = static_cast<uint32_t>(expect.size());
    std::vector<float> out(static_cast<size_t>(n) * 4, 0.0f);
    if (n == 0 || !traceRays(ctx, s.tlas.get(), rays, out)) {
        MC2_EVENT(Vk, "VK_RT_SCENE", 1, "probe FAIL trace rays={}", n);
        return;
    }
    uint32_t propOk = 0, terrainOk = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const float* o = &out[static_cast<size_t>(i) * 4];
        const bool hit = o[0] != 0.0f;
        const uint32_t idx = static_cast<uint32_t>(o[3]);
        const bool isProp = i < props;
        bool match = hit && idx == expect[i] && (isProp || std::abs(o[1] - expectT[i]) < 1.0f);
        // A prop ray can be occluded by a taller neighbour at the same x,y (the ray starts far above the map). Continue the ray past each
        // occluder: finding the expected custom index behind it proves the instance mapping and only the occlusion is at fault; never
        // finding it is a wrong-instance fault. passes= is the number of occluders skipped.
        if (isProp && hit && !match) {
            const size_t base = static_cast<size_t>(i) * 8;
            float z = kTop - o[1] - 0.05f;
            uint32_t passes = 0;
            while (passes < 8 && z > -kTop) {
                ++passes;
                const float ray[8] = {rays[base], rays[base + 1], z, 0.0f, 0.0f, 0.0f, -1.0f, z + kTop};
                float r[4] = {};
                if (!traceRays(ctx, s.tlas.get(), ray, r) || r[0] == 0.0f) break;
                if (static_cast<uint32_t>(r[3]) == expect[i]) {
                    match = true;
                    break;
                }
                z -= r[1] + 0.05f;
            }
            MC2_EVENT(Vk, "VK_RT_SCENE", 1, "probe ray={} occluded_by=0x{:x} behind_found={} passes={}", i, idx, match ? 1 : 0, passes);
        }
        (isProp ? propOk : terrainOk) += match ? 1u : 0u;
        MC2_EVENT(Vk, "VK_RT_SCENE", 1, "probe ray={} kind={} origin=({:.1f},{:.1f}) hit={} t={:.2f} prim={} custom=0x{:x} expect=0x{:x} expect_t={:.2f} match={}", i,
             isProp ? "prop" : "terrain", rays[static_cast<size_t>(i) * 8], rays[static_cast<size_t>(i) * 8 + 1], hit ? 1 : 0, o[1], static_cast<uint32_t>(o[2]), idx, expect[i],
             expectT[i], match ? 1 : 0);
    }
    MC2_EVENT(Vk, "VK_RT_SCENE", 1, "probe result props={}/{} terrain={}/{} tlas_instances={}", propOk, props, terrainOk, n - props, s.tlasInstances);
}

}  // namespace

void noteTerrain(const TerrainSource& t) {
    VulkanContext* ctx = rayContext();
    if (!ctx || !terrainUsable(t)) return;
    const uint64_t key = terrainKey(t);
    State& s = State::instance();
    if (key == s.terrainKeyLive || key == s.terrainKeyFailed) return;
    const auto t0 = std::chrono::steady_clock::now();

    const uint32_t per = terrainTilesPerSide(t);
    std::vector<float> pos, chunkPos;
    std::vector<uint32_t> idx, chunkIdx;
    struct Range {
        uint32_t firstVertex, vertexCount, firstIndex, indexCount;
    };
    std::vector<Range> ranges;
    for (uint32_t cy = 0; cy < per; ++cy)
        for (uint32_t cx = 0; cx < per; ++cx) {
            buildTerrainTile(t, cx, cy, chunkPos, chunkIdx);   // record ordinal == terrainTileId(t, cx, cy)
            ranges.push_back({static_cast<uint32_t>(pos.size() / 3), static_cast<uint32_t>(chunkPos.size() / 3), static_cast<uint32_t>(idx.size()),
                              static_cast<uint32_t>(chunkIdx.size())});
            pos.insert(pos.end(), chunkPos.begin(), chunkPos.end());
            idx.insert(idx.end(), chunkIdx.begin(), chunkIdx.end());
        }
    OwnedBuffer vb, ib;
    std::vector<AccelStructure> built;
    BuildStats st;
    bool ok = !pos.empty() && vb.createFilled(ctx->allocator(), ctx->device(), pos.data(), pos.size() * sizeof(float), kBuildInput, "rt_scene.terrain_vb") &&
              ib.createFilled(ctx->allocator(), ctx->device(), idx.data(), idx.size() * sizeof(uint32_t), kBuildInput, "rt_scene.terrain_ib");
    if (ok) {
        std::vector<TriangleGeometry> geos;
        for (const Range& r : ranges) {
            TriangleGeometry g;
            g.vertices = vb.deviceAddress() + static_cast<uint64_t>(r.firstVertex) * 12;
            g.vertexCount = r.vertexCount;
            g.indices = ib.deviceAddress() + static_cast<uint64_t>(r.firstIndex) * 4;
            g.triangleCount = r.indexCount / 3;
            geos.push_back(g);
        }
        ok = buildStatic(*ctx, s, geos, "rt_scene.terrain", built, st);
    }
    if (vb) ctx->retire(std::move(vb));
    if (ib) ctx->retire(std::move(ib));
    uint32_t count = 0, props = 0, dropped = 0;
    std::optional<AccelStructure> tlas;
    if (ok) tlas = buildTlas(*ctx, s, built, {}, count, props, dropped);
    if (!tlas) {
        retireEach(*ctx, built);
        s.terrainKeyFailed = key;
        MC2_EVENT(Vk, "VK_RT_SCENE", 1, "FAIL terrain build tiles={} key=0x{:x}; keeping the last snapshot", ranges.size(), key);
        return;
    }
    // Publish: the old TLAS retires before the BLASes it referenced.
    if (s.tlas) ctx->retire(std::move(s.tlas));
    s.tlas = std::move(*tlas);
    retireEach(*ctx, s.terrainBlas);
    s.terrainBlas = std::move(built);
    s.terrainKeyLive = key;
    s.terrainKeyFailed = kNone;
    s.tlasInstances = count;
    s.tlasProps = props;
    s.dropped = dropped;
    s.terrainProbe.clear();
    const int32_t side = static_cast<int32_t>(terrainGridSide(t));
    for (const auto& [fx, fy] : {std::pair{1, 1}, std::pair{1, 3}, std::pair{3, 1}, std::pair{3, 3}}) {
        const int32_t gx = side * fx / 4, gy = side * fy / 4;
        s.terrainProbe.push_back({terrainGridX(t, gx), terrainGridY(t, gy), terrainGridHeight(t, gx, gy)});
    }
    s.probed = false;
    logPublish(s, terrainUsesFine(t) ? "terrain_fine" : "terrain_coarse", st, msSince(t0));
    probe(*ctx, s);
}

void updateProps(uint64_t propVbo) {
    VulkanContext* ctx = rayContext();
    if (!ctx || propVbo == 0) return;
    State& s = State::instance();
    if (propVbo != s.propVbo) {   // a new map's VBO: every prop BLAS was built from the old one
        invalidateProps();
        s.propVbo = propVbo;
    }
    const uint64_t gen = GpuStaticPropRegistry::getRegistryGeneration();
    if (gen == s.propGen || gen == s.propGenFailed) return;
    Mc2ShadowGeomFeed feed{};
    const void* vboCpu = nullptr;
    const void* iboCpu = nullptr;
    uint64_t vboBytes = 0, iboBytes = 0;
    if (!mc2_shadow_island_get_geometry_feed(&feed) || feed.vertexStride == 0 || !mc2_static_prop_geometry_cpu(&vboCpu, &vboBytes, &iboCpu, &iboBytes) ||
        iboCpu == nullptr || iboBytes != feed.iboBytes || vboBytes != feed.vboBytes)
        return;   // geometry not finalized yet: retried next frame
    const auto t0 = std::chrono::steady_clock::now();

    std::vector<GpuStaticPropInstance> inst, dyn;
    GpuStaticPropRegistry::getBuildingShadowInstances(inst);
    GpuStaticPropRegistry::getDynamicPropShadowInstances(dyn);
    inst.insert(inst.end(), dyn.begin(), dyn.end());
    std::vector<uint32_t> ids(inst.size(), 0);
    for (size_t i = 0; i < inst.size(); ++i) {
        const int32_t recipe = GpuStaticPropRegistry::getRecipeIndexForType(inst[i].typeID);
        ids[i] = recipe >= 0 ? RenderWorld::objectIdRawForStaticPropRecipe(recipe) : kTerrainObjectId;   // 0 = no recipe: dropped by buildRecords
    }

    // The types that still need a BLAS: one combined rebased IBO, one geometry per type over the shared VBO.
    if (s.propBlas.size() < feed.typeCount) {
        s.propBlas.resize(feed.typeCount);
        s.propTried.resize(feed.typeCount, 0);
    }
    const uint32_t vertexCount = static_cast<uint32_t>(feed.vboBytes / feed.vertexStride);
    const std::span<const uint32_t> ibo(static_cast<const uint32_t*>(iboCpu), static_cast<size_t>(iboBytes / 4));
    std::vector<uint32_t> types, indices, firstIndex;
    for (const GpuStaticPropInstance& g : inst) {
        const uint32_t ty = g.typeID;
        if (ty >= feed.typeCount || s.propTried[ty] || std::find(types.begin(), types.end(), ty) != types.end()) continue;
        types.push_back(ty);
    }
    std::vector<uint32_t> triangles;
    for (uint32_t ty : types) {
        firstIndex.push_back(static_cast<uint32_t>(indices.size()));
        const uint32_t first = feed.typeFirstPacket[ty], count = feed.typePacketCount[ty];
        std::vector<PacketRange> packets;
        for (uint32_t p = first; p < first + count && p < feed.packetCount; ++p) packets.push_back({feed.packets[p].firstIndex, feed.packets[p].indexCount, feed.packets[p].baseVertex});
        triangles.push_back(appendRebasedIndices(ibo, packets, vertexCount, indices));
    }
    OwnedBuffer ib;
    std::vector<AccelStructure> built;
    BuildStats st;
    bool ok = true;
    if (!indices.empty()) {
        ok = ib.createFilled(ctx->allocator(), ctx->device(), indices.data(), indices.size() * sizeof(uint32_t), kBuildInput, "rt_scene.prop_ib");
        if (ok) {
            std::vector<TriangleGeometry> geos;
            for (size_t i = 0; i < types.size(); ++i) {
                TriangleGeometry g;
                g.vertices = propVbo;
                g.vertexStride = feed.vertexStride;
                g.vertexCount = vertexCount;
                g.indices = ib.deviceAddress() + static_cast<uint64_t>(firstIndex[i]) * 4;
                g.triangleCount = triangles[i];
                geos.push_back(g);
            }
            ok = buildStatic(*ctx, s, geos, "rt_scene.prop", built, st);
        }
    }
    if (ib) ctx->retire(std::move(ib));
    std::vector<std::pair<uint32_t, AccelStructure>> added;
    for (size_t i = 0; i < built.size(); ++i) added.emplace_back(types[i], std::move(built[i]));

    // The TLAS reads the new snapshot; on failure the previous instances / records stay published.
    std::swap(s.instances, inst);
    std::swap(s.objectIds, ids);
    uint32_t count = 0, props = 0, dropped = 0;
    std::optional<AccelStructure> tlas;
    if (ok) tlas = buildTlas(*ctx, s, s.terrainBlas, added, count, props, dropped);
    if (!tlas) {
        std::swap(s.instances, inst);
        std::swap(s.objectIds, ids);
        for (auto& [ty, a] : added)
            if (a) ctx->retire(std::move(a));
        s.propGenFailed = gen;
        MC2_EVENT(Vk, "VK_RT_SCENE", 1, "FAIL prop build gen={} types={} instances={}; keeping the last snapshot", gen, types.size(), inst.size());
        return;
    }
    if (s.tlas) ctx->retire(std::move(s.tlas));
    s.tlas = std::move(*tlas);
    for (auto& [ty, a] : added) s.propBlas[ty] = std::move(a);   // new types only: no published BLAS is replaced
    for (uint32_t ty : types) s.propTried[ty] = 1;
    s.propGen = gen;
    s.propGenFailed = kNone;
    s.tlasInstances = count;
    s.tlasProps = props;
    s.dropped = dropped;
    logPublish(s, "props", st, msSince(t0));
    probe(*ctx, s);
}

void invalidateProps() {
    if (!State::exists()) return;
    VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    State& s = State::instance();
    // The TLAS references the prop BLASes: it retires first. Terrain BLASes stay; the next updateProps (or noteTerrain) republishes a TLAS.
    if (s.tlas) ctx.retire(std::move(s.tlas));
    retireEach(ctx, s.propBlas);
    s.propTried.clear();
    s.instances.clear();
    s.objectIds.clear();
    s.propVbo = 0;
    s.propGen = s.propGenFailed = kNone;
    s.tlasInstances = s.tlasProps = s.dropped = 0;
    s.probed = false;
}

}  // namespace mc2vk::rt::scene
