// vk_rt_scene.cpp -- the Vulkan half of the persistent ray-visible scene (see vk_rt_scene.h for the policy). Owns, in IslandSlot::RtScene: one compacted BLAS
// per static-prop type, one per terrain chunk, and the TLAS over the whole registry. Every build is a blocking one-shot; a replacement set is built into new
// owners and published only when the TLAS over it was built, then the replaced TLAS and BLASes retire through VulkanContext::retire.
#include "vk_rt_scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <deque>
#include <format>
#include <numeric>
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

// One skinned mover (a mech or a vehicle): its updatable BLAS over every draw run of the actor, rebuilt by refitAction.
struct Mover {
    uint32_t key = 0;          // feed.motionKeys (RenderWorld handle, or bit 31 + shape address)
    uint32_t slot = 0;         // custom index kMechIdFirst + slot
    AccelStructure blas;
    uint32_t refits = 0;       // since the last build
    uint64_t lastSeen = 0;     // the frame it was last drawn
    float anchor[3] = {};      // root bone, MC2 world, at lastSeen
};

class State final : public IslandState {
public:
    ScratchArena scratch;
    // Movers (part 2). The mover TLAS is rebuilt in the mech frame command buffer every frame over the static instances + every live mover.
    std::vector<VkAccelerationStructureInstanceKHR> pendingInst, staticInst;   // the static TLAS's instances (built / published)
    std::vector<Mover> movers;                      // sorted by key
    uint32_t nextSlot = 0;
    std::deque<uint32_t> freeSlots;                 // left slots, reused FIFO once nextSlot reached kMoverSlots
    std::vector<OwnedBuffer> moverInst;             // per frame slot: the TLAS build input (host-written; the slot's previous submit retired)
    std::vector<uint32_t> moverInstCap;             // instances per moverInst
    std::vector<VkAccelerationStructureInstanceKHR> instStage;
    AccelStructure moverTlas;
    ScratchArena moverScratch;
    uint64_t moverBuilds = 0, moverRefits = 0, moverRebuilds = 0, moverRetired = 0, moverLeft = 0, moverDropped = 0, moverFailed = 0;
    uint64_t lastLedgerFrame = 0, lastLiveFrame = 0, firstMoverFrame = 0;
    uint32_t lastLedgerLive = ~0u;
    uint32_t moverProbe = 0;                        // MC2_VK_RT_SELFTEST: 0 pending, 1 victim despawned, 2 done
    uint64_t moverProbeFrame = 0;
    uint32_t victimKey = 0, victimIndex = 0;
    float victimAnchor[3] = {};
    std::vector<uint32_t> suppressed;               // the self-test's despawned victim stays out
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
    s.pendingInst = inst;   // the per-frame mover TLAS carries the same static instances once this snapshot publishes
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

// A mover leaves: its BLAS retires frame-fenced (the TLAS of an in-flight frame may still reference it) and its slot becomes reusable.
void dropMover(VulkanContext& ctx, State& s, size_t i, std::string_view why) {
    Mover& m = s.movers[i];
    if (m.blas) {
        const VkDeviceSize bytes = m.blas.size();
        ctx.retire(std::move(m.blas), bytes);
        ++s.moverRetired;
    }
    s.freeSlots.push_back(m.slot);
    ++s.moverLeft;
    MC2_EVENT(Vk, "VK_RT_SCENE", 1, "mover leave key=0x{:x} slot={} why={}", m.key, m.slot, why);
    s.movers.erase(s.movers.begin() + static_cast<std::ptrdiff_t>(i));
}

void dropAllMovers(VulkanContext& ctx, State& s) {
    if (s.moverTlas) ctx.retire(std::move(s.moverTlas));
    while (!s.movers.empty()) dropMover(ctx, s, s.movers.size() - 1, "unload");
    s.moverScratch.retireAll(ctx);
    s.nextSlot = 0;
    s.freeSlots.clear();
    s.suppressed.clear();
}

bool takeMoverSlot(State& s, uint32_t& slot) {
    if (s.nextSlot < kMoverSlots) {
        slot = s.nextSlot++;
        return true;
    }
    if (s.freeSlots.empty()) return false;
    slot = s.freeSlots.front();
    s.freeSlots.pop_front();
    return true;
}

// Traces one downward ray at (x, y), continuing past occluders (up to 8): true when custom index `want` is found along it.
bool rayFinds(VulkanContext& ctx, VkAccelerationStructureKHR tlas, float x, float y, uint32_t want, bool& traced) {
    constexpr float kTop = 20000.0f;
    float z = kTop;
    traced = true;
    for (uint32_t pass = 0; pass < 9 && z > -kTop; ++pass) {
        const float ray[8] = {x, y, z, 0.0f, 0.0f, 0.0f, -1.0f, z + kTop};
        float r[4] = {};
        if (!traceRays(ctx, tlas, ray, r)) {
            traced = false;
            return false;
        }
        if (r[0] == 0.0f) return false;
        if (static_cast<uint32_t>(r[3]) == want) return true;
        z -= r[1] + 0.05f;
    }
    return false;
}

// MC2_VK_RT_SELFTEST, movers: (0) once the mover TLAS has run 30 frames, rays at each mover's root must find its custom index, then one victim is
// despawned; (1) a later frame's TLAS must not contain the victim anywhere along its ray. traceRays is a blocking one-shot on the published moverTlas,
// whose build was submitted in an earlier frame.
void probeMovers(VulkanContext& ctx, State& s, uint64_t frame) {
    if (s.moverProbe >= 2 || !s.moverTlas || !mc2::gate::get<"MC2_VK_RT_SELFTEST">()) return;
    if (s.moverProbe == 0) {
        if (s.movers.empty() || frame < s.firstMoverFrame + 30) return;
        uint32_t hit = 0, n = 0;
        for (const Mover& m : s.movers) {
            if (n >= 8 || !m.blas) continue;
            ++n;
            const uint32_t want = moverCustomIndex(m.slot).value_or(0);
            bool found = false, traced = true;
            for (const float d : {0.0f, 8.0f, -8.0f})
                if (!found) found = rayFinds(ctx, s.moverTlas.get(), m.anchor[0] + d, m.anchor[1] + d, want, traced);
            hit += found ? 1u : 0u;
            MC2_EVENT(Vk, "VK_RT_SCENE", 1, "probe mover key=0x{:x} slot={} anchor=({:.1f},{:.1f},{:.1f}) traced={} found={}", m.key, m.slot, m.anchor[0], m.anchor[1],
                      m.anchor[2], traced ? 1 : 0, found ? 1 : 0);
        }
        MC2_EVENT(Vk, "VK_RT_SCENE", 1, "probe movers={}/{}", hit, n);
        for (size_t i = 0; i < s.movers.size(); ++i)
            if (s.movers[i].blas) {
                s.victimKey = s.movers[i].key;
                s.victimIndex = moverCustomIndex(s.movers[i].slot).value_or(0);
                std::copy(s.movers[i].anchor, s.movers[i].anchor + 3, s.victimAnchor);
                s.suppressed.push_back(s.victimKey);
                dropMover(ctx, s, i, "selftest_despawn");
                s.moverProbe = 1;
                s.moverProbeFrame = frame;
                return;
            }
        s.moverProbe = 2;
        return;
    }
    if (frame < s.moverProbeFrame + 2) return;   // the next TLAS (built without the victim) has been submitted
    bool traced = true, found = false;
    for (const float d : {0.0f, 8.0f, -8.0f})
        if (!found) found = rayFinds(ctx, s.moverTlas.get(), s.victimAnchor[0] + d, s.victimAnchor[1] + d, s.victimIndex, traced);
    MC2_EVENT(Vk, "VK_RT_SCENE", 1, "probe despawn_clean={} key=0x{:x} index=0x{:x} traced={}", (!found && traced) ? 1 : 0, s.victimKey, s.victimIndex, traced ? 1 : 0);
    s.moverProbe = 2;
}

void logMoverLedger(const VulkanContext& ctx, State& s, uint64_t frame, uint32_t instances) {
    const uint32_t live = static_cast<uint32_t>(s.movers.size());
    if (live == s.lastLedgerLive && frame < s.lastLedgerFrame + 300) return;
    s.lastLedgerLive = live;
    s.lastLedgerFrame = frame;
    uint64_t blasBytes = 0, instBytes = 0;
    for (const Mover& m : s.movers) blasBytes += m.blas ? m.blas.size() : 0;
    for (size_t i = 0; i < s.moverInst.size(); ++i) instBytes += s.moverInst[i] ? static_cast<uint64_t>(s.moverInstCap[i]) * kInstanceBytes : 0;
    MC2_EVENT(Vk, "VK_RT_SCENE", 1, "movers live={} tlas_instances={} blas_bytes={} builds={} refits={} rebuilds={} retired={} left={} dropped={} failed={} "
              "pending_retire={} pending_retire_bytes={} tlas_bytes={} instance_bytes={} scratch_bytes={}", live, instances, blasBytes, s.moverBuilds, s.moverRefits,
              s.moverRebuilds, s.moverRetired, s.moverLeft, s.moverDropped, s.moverFailed, ctx.deferred().size(), ctx.deferred().bytes(),
              s.moverTlas ? s.moverTlas.size() : 0, instBytes, s.moverScratch.capacity());
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
    if (s.moverTlas) ctx->retire(std::move(s.moverTlas));   // it references the replaced BLASes; the next mover frame rebuilds it
    s.staticInst = std::move(s.pendingInst);
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
    s.staticInst = std::move(s.pendingInst);   // no published BLAS was replaced: the held mover TLAS stays valid until the next mover frame
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
    dropAllMovers(ctx, s);   // the mover TLAS references the prop BLASes too; the mech geometry goes away with the map
    s.staticInst.clear();
    s.pendingInst.clear();
    retireEach(ctx, s.propBlas);
    s.propTried.clear();
    s.instances.clear();
    s.objectIds.clear();
    s.propVbo = 0;
    s.propGen = s.propGenFailed = kNone;
    s.tlasInstances = s.tlasProps = s.dropped = 0;
    s.probed = false;
}

void recordMovers(VkCommandBuffer_T* cb, uint32_t slot, std::span<const MoverGeometry> geos) {
    VulkanContext* ctx = rayContext();
    if (!ctx || cb == nullptr) return;
    if (geos.empty() && !State::exists()) return;
    State& s = State::instance();
    if (geos.empty() && s.movers.empty()) {
        if (s.moverTlas) ctx->retire(std::move(s.moverTlas));   // it may reference movers that left
        return;
    }
    const VulkanContext::FrameContext fc = ctx->frameContext();
    const uint64_t frame = fc.frame;
    VkCommandBuffer cmd = cb;

    // The self-test reads the TLAS published by an earlier frame, before this frame records its replacement.
    probeMovers(*ctx, s, frame);

    // Group this frame's runs by actor (stable: the run order is the actor's geometry order).
    std::vector<size_t> order(geos.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return geos[a].key < geos[b].key; });
    struct Actor {
        uint32_t key;
        size_t start, count;   // into tris
        float anchor[3];
    };
    std::vector<TriangleGeometry> tris;
    std::vector<Actor> actors;
    tris.reserve(geos.size());
    for (size_t i : order) {
        const MoverGeometry& g = geos[i];
        if (g.key == 0 || g.vertices == 0 || g.indices == 0 || g.vertexCount == 0 || g.triangleCount == 0) continue;
        if (std::find(s.suppressed.begin(), s.suppressed.end(), g.key) != s.suppressed.end()) continue;
        if (actors.empty() || actors.back().key != g.key) actors.push_back({g.key, tris.size(), 0, {g.anchor[0], g.anchor[1], g.anchor[2]}});
        TriangleGeometry t;
        t.vertices = g.vertices;
        t.vertexStride = g.vertexStride;
        t.vertexCount = g.vertexCount;
        t.indices = g.indices;
        t.triangleCount = g.triangleCount;
        tris.push_back(t);
        ++actors.back().count;
    }

    // Pass 1: membership (insertions keep `movers` sorted; no pointer into it is taken yet).
    for (const Actor& a : actors) {
        auto it = std::lower_bound(s.movers.begin(), s.movers.end(), a.key, [](const Mover& m, uint32_t k) { return m.key < k; });
        if (it != s.movers.end() && it->key == a.key) continue;
        uint32_t moverSlot = 0;
        if (!takeMoverSlot(s, moverSlot)) {
            ++s.moverDropped;
            continue;
        }
        Mover m;
        m.key = a.key;
        m.slot = moverSlot;
        if (s.movers.empty() && s.firstMoverFrame == 0) s.firstMoverFrame = frame;
        s.movers.insert(it, std::move(m));
    }

    // Pass 2: one job per drawn actor. Refit while the topology and flags hold and fewer than kRefitsBeforeRebuild refits ran; otherwise rebuild.
    std::vector<BlasJob> jobs;
    std::vector<Mover*> jobMover;
    std::vector<GeometrySignature> sigs;
    jobs.reserve(actors.size());
    for (const Actor& a : actors) {
        auto it = std::lower_bound(s.movers.begin(), s.movers.end(), a.key, [](const Mover& m, uint32_t k) { return m.key < k; });
        if (it == s.movers.end() || it->key != a.key) continue;   // dropped: no slot
        Mover& m = *it;
        m.lastSeen = frame;
        std::copy(a.anchor, a.anchor + 3, m.anchor);
        const std::span<const TriangleGeometry> g(tris.data() + a.start, a.count);
        sigs.clear();
        for (const TriangleGeometry& t : g) sigs.push_back(signatureOf(t));
        const RefitAction act = refitAction(m.blas.signature(), m.blas ? m.blas.buildFlagBits() : 0u, sigs, kBuildAllowUpdate, m.refits);
        BlasJob j;
        j.geometries = g;
        j.blas = &m.blas;
        j.refit = act == RefitAction::Refit;
        if (!j.refit) {
            if (m.blas) {
                const VkDeviceSize bytes = m.blas.size();
                ctx->retire(std::move(m.blas), bytes);
                m.blas = AccelStructure{};
                ++s.moverRetired;
                ++s.moverRebuilds;
            } else {
                ++s.moverBuilds;
            }
            m.refits = 0;
        }
        jobs.push_back(j);
        jobMover.push_back(&m);
    }

    // This frame's skin writes, the previous frame's builds (WAW on the BLASes, scratch, TLAS) and its ray reads (WAR) -> this frame's builds.
    recordBarrier(*ctx, cmd, frameRebuildBarrier());
    if (!jobs.empty()) {
        BuildFlags flags;
        flags.allowUpdate = true;          // refit every frame
        flags.allowCompaction = false;     // never compacted (and forbidden with ALLOW_UPDATE on AMD proprietary: compactionAllowed)
        flags.preferFastTrace = false;     // PREFER_FAST_BUILD
        if (!recordBlasJobs(*ctx, cmd, jobs, flags, "rt_scene.mover", s.moverScratch)) {
            ++s.moverFailed;
            if (s.moverFailed == 1) MC2_EVENT(Vk, "VK_RT_SCENE", 1, "FAIL mover scratch reserve jobs={}", jobs.size());
        }
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (jobs[i].ok && jobs[i].refit) {
                ++jobMover[i]->refits;
                ++s.moverRefits;
            } else if (!jobs[i].ok) {
                ++s.moverFailed;
                jobMover[i]->refits = kRefitsBeforeRebuild;   // the held structure (if any) is stale for this pose: rebuild next frame
            }
        }
    }
    recordBarrier(*ctx, cmd, Phase::Blas, Phase::Tlas);

    // The frame TLAS: every static instance + every mover holding a BLAS (off-screen movers keep their last pose).
    s.instStage.assign(s.staticInst.begin(), s.staticInst.end());
    float xf[12];
    moverTransform(xf);
    for (const Mover& m : s.movers) {
        const std::optional<uint32_t> index = moverCustomIndex(m.slot);
        if (!m.blas || !index) continue;
        s.instStage.push_back(makeInstance(m.blas.address(), xf, *index, kMaskMover));
    }
    const uint32_t count = static_cast<uint32_t>(s.instStage.size());
    if (s.moverInst.size() < fc.framesInFlight) {
        s.moverInst.resize(fc.framesInFlight);
        s.moverInstCap.resize(fc.framesInFlight, 0);
    }
    const uint32_t fs = slot % static_cast<uint32_t>(s.moverInst.size());
    bool ok = count > 0;
    if (ok && s.moverInstCap[fs] < count) {
        if (s.moverInst[fs]) ctx->retire(std::move(s.moverInst[fs]));
        const uint32_t cap = std::max<uint32_t>(count + count / 2, 64);
        s.moverInstCap[fs] = 0;
        if (s.moverInst[fs].createHostWritable(ctx->allocator(), ctx->device(), static_cast<VkDeviceSize>(cap) * kInstanceBytes, kBuildInput, "rt_scene.mover_instances"))
            s.moverInstCap[fs] = cap;
        else
            ok = false;
    }
    if (ok) ok = s.moverInst[fs].upload(s.instStage.data(), static_cast<VkDeviceSize>(count) * kInstanceBytes);
    if (ok) {
        BuildFlags flags;
        flags.preferFastTrace = false;
        const uint32_t capacity = s.moverInstCap[fs];
        ok = recordTlasRebuild(*ctx, cmd, s.moverInst[fs].deviceAddress(), count, capacity, flags, "rt_scene.mover_tlas", s.moverTlas, s.moverScratch);
        if (!ok && s.moverTlas) {   // too small: a new one sized for the capacity
            ctx->retire(std::move(s.moverTlas));
            s.moverTlas = AccelStructure{};
            ok = recordTlasRebuild(*ctx, cmd, s.moverInst[fs].deviceAddress(), count, capacity, flags, "rt_scene.mover_tlas", s.moverTlas, s.moverScratch);
        }
        if (ok) recordBarrier(*ctx, cmd, Phase::Tlas, Phase::Consumer);
    }
    if (!ok && s.moverTlas) ctx->retire(std::move(s.moverTlas));   // never leave a TLAS whose build was not recorded this frame
    s.moverScratch.retireStale(*ctx);

    // Leave: a mech whose RenderWorld handle died, or (vehicles, high-generation handles) a mover unseen for kMoverUnseenFrames. Checked every 30 frames.
    if (frame >= s.lastLiveFrame + 30) {
        s.lastLiveFrame = frame;
        for (size_t i = s.movers.size(); i-- > 0;) {
            const Mover& m = s.movers[i];
            if (m.lastSeen == frame) continue;
            const bool handle = moverIsHandleKey(m.key);
            bool alive = false;
            if (handle) {
                RenderWorld::ObjectRecordView view;
                alive = RenderWorld::getObjectRecordView(RenderCore::RenderObjectHandle::make(m.key & 0xFFFFFu, m.key >> 20), &view) && view.valid;
            }
            if (moverLeaves(handle, alive, frame - m.lastSeen)) dropMover(*ctx, s, i, handle ? "handle_dead" : "unseen");
        }
    }
    logMoverLedger(*ctx, s, frame, ok ? count : 0);
}

}  // namespace mc2vk::rt::scene
