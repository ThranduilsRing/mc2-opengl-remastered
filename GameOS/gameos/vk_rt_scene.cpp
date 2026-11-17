// vk_rt_scene.cpp -- the Vulkan half of the persistent ray-visible scene (see vk_rt_scene.h for the policy). Owns, in IslandSlot::RtScene: one compacted BLAS
// per static-prop type, one per terrain chunk, and the TLAS over the whole registry. Map-load builds block; prop updates use the frame queue. Replacements use new
// owners and published only when the TLAS over it was built, then the replaced TLAS and BLASes retire through VulkanContext::retire.
#include "vk_rt_scene.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <cstring>
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
#include "vk_gpu_profile.h"
#include <tracy/Tracy.hpp>
#include "vk_rt_acceleration.h"
#include "vk_rt_selftest.h"
#include "vulkan_color_prop_island.h"
#include "vulkan_context.h"
#include "vulkan_shadow_island_live.h"

namespace mc2vk::rt::scene {

namespace {

constexpr VkBufferUsageFlags kBuildInput = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
constexpr uint64_t kNone = ~0ull;
constexpr uint32_t kNoRow = ~0u;
constexpr uint32_t kMatWaitCalls = 120;        // updateProps calls a new type waits for the colour-prop material feed before building without materials
constexpr uint32_t kMatProbeWaitCalls = 600;   // MC2_VK_RT_SELFTEST: calls the material probe waits for the island's albedo arrays before it FAILs
constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

struct ProbePoint {
    float x = 0, y = 0, h = 0;   // a grid vertex (the object-ID probe)
    float mx = 0, my = 0;        // the centre of the cell right of / above it, inside `tile` (the material probe)
    uint32_t tile = 0;
};

// MC2_VK_RT_SELFTEST material probe target: one triangle of a prop type, local space.
struct Target {
    float v[9] = {};
    uint32_t row = 0;            // local prop row (MaterialRow index - kPropRowBase)
    uint32_t prim = 0;           // primitive index inside that row's geometry
    float area = 0.0f;
    bool cutout = false;
    bool valid = false;
};
constexpr size_t kTargetCandidates = 8;   // per type and class, largest first, none with a coplanar twin

// CPU copy of the traced terrain grid (MC2_VK_RT_SELFTEST only): the probe keeps its targets above the ground.
struct TerrainGrid {
    int32_t side = 0;
    float step = 128.0f, halfMap = 0.0f;
    std::vector<float> h;
    // The highest of the four grid corners around world (x, y): a conservative terrain height.
    float maxAt(float x, float y) const {
        if (side < 2) return 0.0f;
        const int32_t gx = std::clamp(static_cast<int32_t>(std::floor((x + halfMap) / step)), 0, side - 2);
        const int32_t gy = std::clamp(static_cast<int32_t>(std::floor((halfMap - y) / step)), 0, side - 2);
        const size_t i = static_cast<size_t>(gx) + static_cast<size_t>(gy) * static_cast<size_t>(side);
        return std::max({h[i], h[i + 1], h[i + static_cast<size_t>(side)], h[i + static_cast<size_t>(side) + 1]});
    }
};

// Plane data of a triangle (local space).
struct TriPlane {
    float n[3], c[3], radius;
    bool ok;
};
TriPlane triPlane(const float v[9]) {
    TriPlane p{};
    const float e1[3] = {v[3] - v[0], v[4] - v[1], v[5] - v[2]}, e2[3] = {v[6] - v[0], v[7] - v[1], v[8] - v[2]};
    p.n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    p.n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    p.n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    const float len = std::sqrt(p.n[0] * p.n[0] + p.n[1] * p.n[1] + p.n[2] * p.n[2]);
    p.ok = len > 1e-6f;
    if (!p.ok) return p;
    for (float& c : p.n) c /= len;
    for (int r = 0; r < 3; ++r) p.c[r] = (v[r] + v[3 + r] + v[6 + r]) / 3.0f;
    for (int k = 0; k < 3; ++k) {
        float d2 = 0.0f;
        for (int r = 0; r < 3; ++r) d2 += (v[3 * k + r] - p.c[r]) * (v[3 * k + r] - p.c[r]);
        p.radius = std::max(p.radius, std::sqrt(d2));
    }
    return p;
}

// The candidate targets of one class: the largest triangles with no coplanar, overlapping twin (a ground plate or a doubled face would win the ray
// instead of the target).
std::vector<Target> pickCandidates(const std::vector<Target>& all, bool cutout) {
    constexpr float kEps = 0.05f;
    constexpr size_t kExamine = 64;
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < all.size(); ++i)
        if (all[i].cutout == cutout) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return all[a].area > all[b].area; });
    std::vector<Target> out;
    for (size_t k = 0; k < order.size() && k < kExamine && out.size() < kTargetCandidates; ++k) {
        const Target& tg = all[order[k]];
        const TriPlane p = triPlane(tg.v);
        if (!p.ok) continue;
        bool twin = false;
        for (uint32_t j = 0; j < all.size() && !twin; ++j) {
            if (j == order[k]) continue;
            const TriPlane q = triPlane(all[j].v);
            if (!q.ok) continue;
            const float dot = p.n[0] * q.n[0] + p.n[1] * q.n[1] + p.n[2] * q.n[2];
            if (std::abs(dot) < 0.999f) continue;
            bool inPlane = true;
            for (int v = 0; v < 3 && inPlane; ++v) {
                float dist = 0.0f;
                for (int r = 0; r < 3; ++r) dist += (all[j].v[3 * v + r] - p.c[r]) * p.n[r];
                inPlane = std::abs(dist) < kEps;
            }
            if (!inPlane) continue;
            float apart = 0.0f;
            for (int r = 0; r < 3; ++r) apart += (q.c[r] - p.c[r]) * (q.c[r] - p.c[r]);
            twin = std::sqrt(apart) < p.radius + q.radius;
        }
        if (!twin) out.push_back(tg);
    }
    return out;
}

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
    VkCommandBuffer frameCmd = VK_NULL_HANDLE;
    bool frameWork = false;
    uint64_t queuedVbo = 0;
    bool compactedDirty = false;
    struct Compaction {
        OwnedQueryPool query;
        uint64_t frame = 0;
        std::vector<uint32_t> types;
        std::vector<VkDeviceAddress> addresses;
    };
    std::vector<Compaction> compactions;
    std::vector<std::pair<uint32_t, AccelStructure>> compacted;
    // Movers (part 2). The mover TLAS is rebuilt in the mech frame command buffer every frame over the static instances + every live mover.
    std::vector<VkAccelerationStructureInstanceKHR> pendingInst, staticInst;   // the static TLAS's instances (built / published)
    std::vector<Mover> movers;                      // sorted by key
    uint32_t nextSlot = 0;
    std::deque<uint32_t> freeSlots;                 // left slots, reused FIFO once nextSlot reached kMoverSlots
    std::vector<OwnedBuffer> moverInst;             // per frame slot: the TLAS build input (host-written; the slot's previous submit retired)
    std::vector<uint32_t> moverInstCap;             // instances per moverInst
    std::vector<VkAccelerationStructureInstanceKHR> instStage;
    AccelStructure moverTlas;
    bool moverTlasDirty = true;
    ScratchArena moverScratch;
    uint64_t moverBuilds = 0, moverRefits = 0, moverRebuilds = 0, moverRetired = 0, moverLeft = 0, moverDropped = 0, moverFailed = 0;
    uint64_t lastLedgerFrame = 0, lastLiveFrame = 0, firstMoverFrame = 0;
    uint32_t lastLedgerLive = ~0u;
    uint32_t moverProbe = 0;                        // MC2_VK_RT_SELFTEST: 0 pending, 1 victim despawned, 2 done
    uint32_t moverTlasFailed = 0;                   // frame TLAS build failures (first three logged)
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
    // Materials (part 3): published with the TLAS (row layout in vk_rt_scene.h MATERIALS).
    std::vector<MaterialRow> propRows;              // local prop rows (row - kPropRowBase), type by type in build order
    std::vector<uint32_t> typeRowBase;              // by typeID: the type's first local prop row, kNoRow until built
    std::vector<uint32_t> uvIndices;                // every prop geometry's rebased indices (MaterialRow::firstIndex), CPU copy of uvIndex
    OwnedBuffer materials, uvIndex, pendingMat, pendingUv;
    uint64_t materialBytes = 0, pendingMaterialBytes = 0;
    std::vector<std::array<std::vector<Target>, 2>> typeTarget;  // by typeID: [0] opaque, [1] cutout candidates (pickCandidates)
    TerrainGrid terrainGrid;                        // MC2_VK_RT_SELFTEST only
    uint32_t propStride = 0;
    uint32_t matWait = 0, matProbeWait = 0, propsWithoutMaterial = 0;
    bool matProbed = false;

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

Demand& consumers() {
    static Demand demand = [] {
        Demand d;
        d.set(Consumer::SelfTest, mc2::gate::get<"MC2_VK_RT_SELFTEST">());
        return d;
    }();
    return demand;
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

// Builds one BLAS per geometry list into `out` (same order) with PREFER_FAST_TRACE | ALLOW_COMPACTION, then compacts those that save >= 10%. All or
// nothing: on any failure `out` is retired and false returned. An empty list yields an empty structure (every listed geometry must have triangles).
const char* g_failStage = "none";   // the last scene-build failure point, printed on the FAIL lines
std::string g_failWhy;   // the failed check and its values (takeLastRecordFailure), printed after the stage

bool buildStatic(VulkanContext& ctx, State& s, const std::vector<std::vector<TriangleGeometry>>& geos, std::string_view name, std::vector<AccelStructure>& out,
                 BuildStats& st, std::span<const uint32_t> types = {}) {
    out.clear();
    out.resize(geos.size());
    if (std::all_of(geos.begin(), geos.end(), [](const std::vector<TriangleGeometry>& g) { return g.empty(); })) return true;
    std::vector<BlasJob> jobs(geos.size());
    for (size_t i = 0; i < geos.size(); ++i) {
        jobs[i].geometries = geos[i];
        jobs[i].blas = &out[i];
    }
    BuildFlags flags;
    flags.allowCompaction = true;
    OwnedQueryPool qp = createCompactionQueryPool(ctx, static_cast<uint32_t>(geos.size()));
    if (!qp) { g_failStage = "query_pool"; return false; }
    bool ok = true;
    auto record = [&](VkCommandBuffer cmd) {
        recordBarrier(ctx, cmd, Phase::Upload, Phase::Blas);
        // One build call per scratch group, each BLAS on its own scratch sub-range (scratchBatchPlan), instead of one call and one barrier per BLAS.
        uint32_t calls = 0;
        VkDeviceSize span = 0;
        if (!recordBlasJobs(ctx, cmd, jobs, flags, name, s.scratch, &calls, &span)) {
            g_failStage = "blas_record";
            ok = false;
            return;
        }
        for (size_t i = 0; i < geos.size(); ++i)
            if (!geos[i].empty() && !jobs[i].ok) { g_failStage = "blas_job"; ok = false; }   // recorded builds of the others still complete; everything retires below
        st.buildCalls += calls;
        st.scratchSpan = std::max<uint64_t>(st.scratchSpan, span);
        recordBarrier(ctx, cmd, Phase::Blas, Phase::Tlas);   // the compaction query below reads the built structures
        for (size_t i = 0; i < geos.size(); ++i)
            if (out[i]) recordCompactionQuery(ctx, cmd, qp.get(), static_cast<uint32_t>(i), out[i]);
    };
    if (s.frameCmd) {
        s.frameWork = true;
        record(s.frameCmd);
        State::Compaction pending;
        pending.query = std::move(qp);
        pending.frame = ctx.frameContext().frame;
        pending.types.assign(types.begin(), types.end());
        for (const auto& a : out) {
            pending.addresses.push_back(a ? a.address() : 0);
            st.builtBytes += a ? a.size() : 0;
        }
        st.finalBytes = st.builtBytes;
        s.compactions.push_back(std::move(pending));
        s.scratch.retireStale(ctx);
        if (!ok) retireEach(ctx, out);
        return ok;
    }
    const bool submitted = submitOneShot(ctx, record, "rt_scene blas");
    s.scratch.retireStale(ctx);
    if (!submitted || !ok) {
        if (!submitted) g_failStage = "blas_submit";
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
// Also builds the material table (mover row, `propRows`, one row per terrain tile) and the uv-index buffer into s.pendingMat / s.pendingUv: the caller
// publishes them with the TLAS (publishMaterials) or retires them.
void retirePending(VulkanContext& ctx, State& s) {
    if (s.pendingMat) ctx.retire(std::move(s.pendingMat));
    if (s.pendingUv) ctx.retire(std::move(s.pendingUv));
    s.pendingMaterialBytes = 0;
}

void publishMaterials(VulkanContext& ctx, State& s) {
    if (s.pendingMat) {
        if (s.materials) ctx.retire(std::move(s.materials));
        s.materials = std::move(s.pendingMat);
    }
    if (s.pendingUv) {
        if (s.uvIndex) ctx.retire(std::move(s.uvIndex));
        s.uvIndex = std::move(s.pendingUv);
    }
    s.materialBytes = s.pendingMaterialBytes;
    s.pendingMaterialBytes = 0;
}

std::optional<AccelStructure> buildTlas(VulkanContext& ctx, State& s, const std::vector<AccelStructure>& terrain,
                                        const std::vector<std::pair<uint32_t, AccelStructure>>& added, const std::vector<MaterialRow>& propRows,
                                        const std::vector<uint32_t>& typeRowBase, const std::vector<uint32_t>& uv, uint32_t& instanceCount,
                                        uint32_t& propCount, uint32_t& dropped) {
    retirePending(ctx, s);
    const size_t rowCount = 1 + propRows.size() + terrain.size();
    const bool sameRows = s.materials && propRows == s.propRows && terrain.size() == s.terrainBlas.size();
    std::vector<MaterialRow> rows;
    if (!sameRows) {
        rows.reserve(rowCount);
        rows.push_back(moverMaterialRow());
        rows.insert(rows.end(), propRows.begin(), propRows.end());
        for (uint32_t tile = 0; tile < terrain.size(); ++tile) rows.push_back(terrainMaterialRow(tile));
    }
    const uint32_t terrainBase = terrainRowBase(static_cast<uint32_t>(propRows.size()));
    // No padding: every buffer whose address the build needs aligned asks VMA for it (rt_scene.instances below, AS storage, scratch).
    const uint32_t zero = 0;
    const std::span<const uint32_t> uvOrZero = uv.empty() ? std::span<const uint32_t>(&zero, 1) : std::span<const uint32_t>(uv);
    const VkDeviceSize uvBytes = uvOrZero.size() * sizeof(uint32_t);
    const bool sameUv = s.uvIndex && uv == s.uvIndices;
    g_failStage = !customIndexFits(static_cast<uint32_t>(rowCount)) ? "rows_fit" : "material_buffers";
    if (!customIndexFits(static_cast<uint32_t>(rowCount)) ||
        (!sameRows && !s.pendingMat.createFilled(ctx.allocator(), ctx.device(), rows.data(), rows.size() * sizeof(MaterialRow), kStorage, "rt_scene.materials")) ||
        (!sameUv && !s.pendingUv.createFilled(ctx.allocator(), ctx.device(), uvOrZero.data(), uvBytes, kStorage, "rt_scene.uv_index"))) {
        retirePending(ctx, s);
        return std::nullopt;
    }
    s.pendingMaterialBytes = rowCount * sizeof(MaterialRow) + uvBytes;
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
        // The instance's first material row (ray query: row = instanceShaderBindingTableRecordOffset + geometry index).
        uint32_t sbt = kMoverRow;
        if (r.key.kind == Kind::Terrain) sbt = terrainBase + r.key.ordinal;
        else if (r.typeId < typeRowBase.size() && typeRowBase[r.typeId] != kNoRow) sbt = kPropRowBase + typeRowBase[r.typeId];
        inst.push_back(makeInstance(blas, r.transform, customIndexOf(r.key), kMaskStatic, sbt));
        propCount += r.key.kind == Kind::StaticProp ? 1u : 0u;
    }
    instanceCount = static_cast<uint32_t>(inst.size());
    s.pendingInst = inst;   // the per-frame mover TLAS carries the same static instances once this snapshot publishes
    AccelStructure tlas;
    if (inst.empty()) return tlas;   // an empty scene: nothing to trace, still a valid (empty) snapshot
    OwnedBuffer buf;
    if (!buf.createFilled(ctx.allocator(), ctx.device(), inst.data(), inst.size() * sizeof(inst[0]), kBuildInput, "rt_scene.instances", kInstanceAddressAlignment)) {
        g_failStage = "instance_buffer";
        retirePending(ctx, s);
        return std::nullopt;
    }
    // Membership changes always use BUILD, including the static publish on mover-less frames.
    BuildFlags flags;
    flags.preferFastTrace = false;
    bool ok = false;
    auto record = [&](VkCommandBuffer cmd) {
        recordBarrier(ctx, cmd, Phase::Upload, Phase::Tlas);
        if (!recordTlasBuild(ctx, cmd, buf.deviceAddress(), instanceCount, flags, "rt_scene.tlas", tlas, s.scratch)) { g_failStage = "tlas_record"; g_failWhy = takeLastRecordFailure(); return; }
        recordBarrier(ctx, cmd, Phase::Tlas, Phase::Consumer);
        ok = true;
    };
    bool submitted = true;
    if (s.frameCmd) { s.frameWork = true; record(s.frameCmd); }
    else submitted = submitOneShot(ctx, record, "rt_scene tlas");
    s.scratch.retireStale(ctx);
    ctx.retire(std::move(buf));
    if (!submitted || !ok) {
        if (!submitted) g_failStage = "tlas_submit";
        if (tlas) ctx.retire(std::move(tlas));
        retirePending(ctx, s);
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
    // material_bytes: the material table + the uv-index buffer (resident with the TLAS).
    const uint64_t tlasBytes = s.tlas ? s.tlas.size() : 0, instanceBytes = static_cast<uint64_t>(s.tlasInstances) * kInstanceBytes;
    uint32_t cutouts = 0;
    for (const MaterialRow& r : s.propRows) cutouts += (r.flags & kMatCutout) ? 1u : 0u;
    MC2_EVENT(Vk, "VK_RT_SCENE", 1, "publish why={} tlas_instances={} terrain_tiles={} props={} prop_types={} dropped={} blas_bytes={} built_bytes={} compacted={} "
         "compacted_bytes={} tlas_bytes={} scratch_bytes={} instance_bytes={} material_bytes={} material_rows={} cutout_rows={} total_bytes={} blas_build_calls={} "
         "batch_scratch_bytes={} build_ms={:.1f}",
         why, s.tlasInstances, s.terrainBlas.size(), s.tlasProps, types, s.dropped, blasBytes, st.builtBytes, st.compacted, st.finalBytes, tlasBytes,
         s.scratch.capacity(), instanceBytes, s.materialBytes, 1 + s.propRows.size() + s.terrainBlas.size(), cutouts,
         blasBytes + tlasBytes + s.scratch.capacity() + instanceBytes + s.materialBytes, st.buildCalls, st.scratchSpan, ms);
}

bool terrainOccluder(const State& s, uint32_t id, uint32_t prim, float x, float y, float z);

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
        if (!isProp && hit && !match && idx != kTerrainObjectId) {
            const ProbePoint& p = s.terrainProbe[i - props];
            // Only move below an occluder independently confirmed in the CPU source geometry at the reported GPU hit.
            const bool correct = terrainOccluder(s, idx, static_cast<uint32_t>(o[2]), p.x, p.y, kTop - o[1]);
            float r[4] = {};
            const float ray[8] = {p.x, p.y, p.h + 1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 2.0f};
            if (correct && kTop - o[1] > p.h + 1.0f && traceRays(ctx, s.tlas.get(), ray, r))
                match = r[0] != 0.0f && static_cast<uint32_t>(r[3]) == kTerrainObjectId && std::abs(r[1] - 1.0f) < 0.1f;
            MC2_EVENT(Vk, "VK_RT_SCENE", 1, "probe terrain ray={} occluder_correct={} moved_origin_z={:.3f} hit={} t={:.3f} custom=0x{:x} match={}",
                      i, correct ? 1 : 0, p.h + 1.0f, r[0], r[1], static_cast<uint32_t>(r[3]), match ? 1 : 0);
        }
        (isProp ? propOk : terrainOk) += match ? 1u : 0u;
        MC2_EVENT(Vk, "VK_RT_SCENE", 1, "probe ray={} kind={} origin=({:.1f},{:.1f}) hit={} t={:.2f} prim={} custom=0x{:x} expect=0x{:x} expect_t={:.2f} match={}", i,
             isProp ? "prop" : "terrain", rays[static_cast<size_t>(i) * 8], rays[static_cast<size_t>(i) * 8 + 1], hit ? 1 : 0, o[1], static_cast<uint32_t>(o[2]), idx, expect[i],
             expectT[i], match ? 1 : 0);
    }
    MC2_EVENT(Vk, "VK_RT_SCENE", 1, "probe result props={}/{} terrain={}/{} tlas_instances={}", propOk, props, terrainOk, n - props, s.tlasInstances);
}

struct WorldTri {
    float a[3], b[3], c[3], n[3];
};

// The target triangle through the instance's 3x4 transform; false when degenerate.
bool worldTri(const float t[12], const float v[9], WorldTri& w) {
    float* dst[3] = {w.a, w.b, w.c};
    for (int k = 0; k < 3; ++k)
        for (int r = 0; r < 3; ++r) dst[k][r] = t[4 * r] * v[3 * k] + t[4 * r + 1] * v[3 * k + 1] + t[4 * r + 2] * v[3 * k + 2] + t[4 * r + 3];
    const float e1[3] = {w.b[0] - w.a[0], w.b[1] - w.a[1], w.b[2] - w.a[2]}, e2[3] = {w.c[0] - w.a[0], w.c[1] - w.a[1], w.c[2] - w.a[2]};
    w.n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    w.n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    w.n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    const float len = std::sqrt(w.n[0] * w.n[0] + w.n[1] * w.n[1] + w.n[2] * w.n[2]);
    if (!(len > 1e-4f)) return false;
    for (float& c : w.n) c /= len;
    return true;
}

// Validate the original hit against CPU vertices and current placements, independently of BLAS/TLAS contents.
bool terrainOccluder(const State& s, uint32_t id, uint32_t prim, float x, float y, float z) {
    Mc2ShadowGeomFeed feed{};
    const void *vb = nullptr, *ib = nullptr;
    uint64_t vbBytes = 0, ibBytes = 0;
    if (!mc2_shadow_island_get_geometry_feed(&feed) || !feed.vertexStride ||
        !mc2_static_prop_geometry_cpu(&vb, &vbBytes, &ib, &ibBytes) || !vb) return false;
    for (size_t i = 0; i < s.instances.size(); ++i) {
        const uint32_t ty = s.instances[i].typeID;
        if (s.objectIds[i] != id || ty >= s.typeRowBase.size() || ty >= feed.typeCount) continue;
        float t[12];
        propTransform(s.instances[i].modelMatrix, t);
        for (size_t row = s.typeRowBase[ty]; row < s.propRows.size(); ++row) {
            const MaterialRow& m = s.propRows[row];
            if (m.packet >= feed.typeFirstPacket[ty] + feed.typePacketCount[ty]) break;
            if (m.packet >= feed.packetCount || prim >= feed.packets[m.packet].indexCount / 3) continue;
            const size_t at = static_cast<size_t>(m.firstIndex) + 3ull * prim;
            if (at + 2 >= s.uvIndices.size()) continue;
            float v[9];
            bool valid = true;
            for (size_t k = 0; k < 3; ++k) {
                const size_t offset = static_cast<size_t>(s.uvIndices[at + k]) * feed.vertexStride;
                if (offset + 12 > vbBytes) { valid = false; break; }
                std::memcpy(v + 3 * k, static_cast<const uint8_t*>(vb) + offset, 12);
            }
            WorldTri w{};
            if (!valid || !worldTri(t, v, w)) continue;
            const float bx = w.b[0] - w.a[0], by = w.b[1] - w.a[1], cx = w.c[0] - w.a[0], cy = w.c[1] - w.a[1];
            const float det = bx * cy - by * cx;
            if (std::abs(det) < 1e-6f) continue;
            const float u = ((x - w.a[0]) * cy - (y - w.a[1]) * cx) / det;
            const float vWeight = (bx * (y - w.a[1]) - by * (x - w.a[0])) / det;
            const float cpuZ = w.a[2] + u * (w.b[2] - w.a[2]) + vWeight * (w.c[2] - w.a[2]);
            if (u < 0 || vWeight < 0 || u + vWeight > 1 || std::abs(cpuZ - z) > 0.1f) continue;
            MC2_EVENT(Vk, "VK_RT_SCENE", 1, "probe occluder CPU instance={} type={} row={} prim={} xy=({:.3f},{:.3f}) bary=({:.6f},{:.6f}) z={:.3f} gpu_z={:.3f}",
                      i, ty, row, prim, x, y, u, vWeight, cpuZ, z);
            return true;
        }
    }
    return false;
}

// A ray along -normal through barycentric (u, v) of `w`, starting 2 units off the surface: no neighbour above can occlude it (the 1/8 object-ID miss).
// The ray starts kProbeLift above the target triangle along its normal: short, so terrain or another part of the same mesh rarely sits between
// (a 2-unit lift let the terrain hide buried props on mc2_10 and other triangles of the mesh win on mc2_01).
constexpr float kProbeLift = 0.25f;
constexpr float kProbeClear = 0.1f;   // a target's ray must start and end at least this far above the (conservative) terrain height
void pushTriRay(std::vector<float>& rays, const WorldTri& w, float u, float v) {
    const float k = 1.0f - u - v;
    float p[3];
    for (int r = 0; r < 3; ++r) p[r] = w.a[r] * k + w.b[r] * u + w.c[r] * v + w.n[r] * kProbeLift;
    rays.insert(rays.end(), {p[0], p[1], p[2], 0.0f, -w.n[0], -w.n[1], -w.n[2], 2.0f * kProbeLift});
}

struct ProbeHit {
    bool hit;
    float t;
    uint32_t prim, custom, row;
    int32_t layer, packet, flags;
};
ProbeHit hitAt(const std::vector<float>& out, size_t i) {
    const float* o = &out[i * 8];
    return {o[0] != 0.0f, o[1], static_cast<uint32_t>(o[2]), static_cast<uint32_t>(o[3]), static_cast<uint32_t>(o[4]), static_cast<int32_t>(o[5]),
            static_cast<int32_t>(o[6]), static_cast<int32_t>(o[7])};
}
bool sameHit(const ProbeHit& a, const ProbeHit& b) {
    if (a.hit != b.hit) return false;
    return !a.hit || (a.custom == b.custom && a.row == b.row && a.prim == b.prim && std::abs(a.t - b.t) < 1e-3f);
}

// MC2_VK_RT_SELFTEST part 3: material rows, albedo layers and alpha cutouts through rt_scene_probe.comp.slang, every ray traced twice (alpha-tested and every
// candidate committed). props / vegetation: rays along the normal of the type's largest opaque / cutout triangle that has no coplanar twin and sits above
// the terrain (pickTarget) must report that instance, row, layer and packet (a different triangle of the same row passes; `material info` lines). terrain: an upward ray from below each probe cell must report the tile's row. holes: a 16x16 barycentric grid over each
// vegetation triangle; a hole is a ray the opaque trace lands on the triangle and the alpha trace passes through. false_holes: grid rays whose opaque
// hit is a non-cutout row and whose alpha trace differs; opaque_diffs: prop / terrain rays whose two traces differ. FAIL unless every check passes,
// at least one hole was found and both difference counts are 0.
void probeMaterials(VulkanContext& ctx, State& s) {
    if (s.matProbed || !s.tlas || s.terrainBlas.empty() || s.tlasProps == 0 || !s.materials || !s.uvIndex || !mc2::gate::get<"MC2_VK_RT_SELFTEST">()) return;
    bool needOn = false, needOff = false;
    for (const MaterialRow& r : s.propRows)
        if (r.albedoLayer >= 0) ((r.flags & kMatAlphaOn) ? needOn : needOff) = true;
    void* offViews[mc2::prop_buckets::kMaxBuckets]{};
    void* onViews[mc2::prop_buckets::kMaxBuckets]{};
    uint32_t gen = 0;
    const bool off = mc2_color_prop_island_get_albedo_off_arrays(offViews, &gen) != 0;
    const bool on = mc2_color_prop_island_get_alpha_arrays(onViews, &gen) != 0;
    const uint32_t rowCount = static_cast<uint32_t>(1 + s.propRows.size() + s.terrainBlas.size());
    if ((needOff && !off) || (needOn && !on) || (!off && !on)) {
        if (++s.matProbeWait < kMatProbeWaitCalls) return;
        s.matProbed = true;
        MC2_EVENT(Vk, "VK_RT_SCENE", 1, "material result=FAIL reason=albedo_arrays off={} on={} need_off={} need_on={} rows={}", off ? 1 : 0, on ? 1 : 0,
                  needOff ? 1 : 0, needOn ? 1 : 0, rowCount);
        return;
    }
    s.matProbed = true;

    struct Expect {
        uint32_t custom, row, prim, packet;
        int32_t layer;
    };
    std::vector<float> rays;
    std::vector<Expect> propX, vegX, terrX;
    std::vector<size_t> vegGridFirst, vegGridCount;
    auto expectOf = [&](uint32_t id, const Target& tg) {
        const MaterialRow& r = s.propRows[tg.row];
        return Expect{id, kPropRowBase + tg.row, tg.prim, r.packet, r.albedoLayer};
    };
    // The first candidate of the type (largest first, no coplanar twin) whose centroid, and the ray's whole 0.5 span, sits clearly above the terrain at
    // that point: otherwise the ray would land on the ground or on a plate in the terrain plane.
    auto pickTarget = [&](size_t inst, uint32_t ty, int cls, WorldTri& w, Target& out) {
        float t[12];
        propTransform(s.instances[inst].modelMatrix, t);
        for (const Target& tg : s.typeTarget[ty][cls]) {
            if (!worldTri(t, tg.v, w)) continue;
            const float cx = (w.a[0] + w.b[0] + w.c[0]) / 3.0f, cy = (w.a[1] + w.b[1] + w.c[1]) / 3.0f, cz = (w.a[2] + w.b[2] + w.c[2]) / 3.0f;
            if (cz - kProbeLift * std::abs(w.n[2]) < s.terrainGrid.maxAt(cx, cy) + kProbeClear) continue;
            out = tg;
            return true;
        }
        return false;
    };
    // Props: up to 8 instances spread over the registry (a second pass over the rest when targets were skipped); vegetation: up to 4 instances of
    // distinct cutout types.
    const size_t step = std::max<size_t>(1, s.instances.size() / 64);
    std::vector<size_t> propInst;
    auto tryProp = [&](size_t i) {
        const uint32_t ty = s.instances[i].typeID, id = s.objectIds[i];
        if (id == kTerrainObjectId || !customIndexFits(id) || ty >= s.typeTarget.size() || std::find(propInst.begin(), propInst.end(), i) != propInst.end()) return;
        WorldTri w;
        Target tg;
        if (!pickTarget(i, ty, 0, w, tg)) return;
        pushTriRay(rays, w, 1.0f / 3.0f, 1.0f / 3.0f);
        propX.push_back(expectOf(id, tg));
        propInst.push_back(i);
    };
    for (size_t i = 0; i < s.instances.size() && propX.size() < 8; i += step) tryProp(i);
    for (size_t i = 0; i < s.instances.size() && propX.size() < 8; ++i) tryProp(i);
    const size_t vegFirst = propX.size();
    std::vector<uint32_t> vegTypes;
    std::vector<size_t> vegInst;
    std::vector<Target> vegTarget;
    for (size_t i = 0; i < s.instances.size() && vegX.size() < 4; ++i) {
        const uint32_t ty = s.instances[i].typeID, id = s.objectIds[i];
        if (id == kTerrainObjectId || !customIndexFits(id) || ty >= s.typeTarget.size() || std::find(vegTypes.begin(), vegTypes.end(), ty) != vegTypes.end()) continue;
        WorldTri w;
        Target tg;
        if (!pickTarget(i, ty, 1, w, tg)) continue;
        vegTypes.push_back(ty);
        vegInst.push_back(i);
        vegTarget.push_back(tg);
        pushTriRay(rays, w, 1.0f / 3.0f, 1.0f / 3.0f);
        vegX.push_back(expectOf(id, tg));
    }
    const size_t gridFirst = vegFirst + vegX.size();
    {   // the hole grids, after the centroid rays so the indices stay contiguous
        size_t at = gridFirst;
        for (size_t k = 0; k < vegX.size(); ++k) {
            float t[12];
            propTransform(s.instances[vegInst[k]].modelMatrix, t);
            WorldTri w;
            worldTri(t, vegTarget[k].v, w);
            vegGridFirst.push_back(at);
            size_t n = 0;
            for (int gy = 0; gy < 16; ++gy)
                for (int gx = 0; gx < 16; ++gx) {
                    const float u = (gx + 0.5f) / 16.0f, v = (gy + 0.5f) / 16.0f;
                    if (u + v >= 1.0f) continue;
                    pushTriRay(rays, w, u, v);
                    ++n;
                }
            vegGridCount.push_back(n);
            at += n;
        }
    }
    const size_t terrFirst = rays.size() / 8;
    const uint32_t terrainBase = terrainRowBase(static_cast<uint32_t>(s.propRows.size()));
    for (const ProbePoint& p : s.terrainProbe) {
        rays.insert(rays.end(), {p.mx, p.my, p.h - 2000.0f, 0.0f, 0.0f, 0.0f, 1.0f, 4000.0f});   // from below: nothing can occlude the terrain
        terrX.push_back({kTerrainObjectId, terrainBase + p.tile, 0, p.tile, -1});
    }
    const size_t n = rays.size() / 8;
    std::vector<float> outA(n * 8, 0.0f), outO(n * 8, 0.0f);
    SceneProbeInputs in;
    in.tlas = s.tlas.get();
    in.materials = s.materials.get();
    in.uvIndex = s.uvIndex.get();
    in.vbo = s.propVbo;
    in.stride = s.propStride;
    for (uint32_t b = 0; b < mc2::prop_buckets::kMaxBuckets; ++b) {
        in.alphaOff[b] = off ? reinterpret_cast<VkImageView>(offViews[b]) : VK_NULL_HANDLE;
        in.alphaOn[b] = on ? reinterpret_cast<VkImageView>(onViews[b]) : VK_NULL_HANDLE;
    }
    if (n == 0 || !traceSceneRays(ctx, in, rays, outA, false) || !traceSceneRays(ctx, in, rays, outO, true)) {
        MC2_EVENT(Vk, "VK_RT_SCENE", 1, "material result=FAIL reason=trace rays={} rows={}", n, rowCount);
        return;
    }
    uint32_t propOk = 0, vegOk = 0, terrOk = 0, holes = 0, falseHoles = 0, opaqueDiffs = 0, logged = 0, primDiffs = 0;
    // The triangle index is information only: another triangle of the same mesh row may sit coplanar with or just in front of the target.
    auto primInfo = [&](std::string_view kind, size_t i, const ProbeHit& h, const Expect& e) {
        ++primDiffs;
        MC2_EVENT(Vk, "VK_RT_SCENE", 1, "material info kind={} ray={} prim={}/{} t={:.3f} custom=0x{:x} row={}", kind, i, h.prim, e.prim, h.t, h.custom, h.row);
    };
    auto report = [&](std::string_view kind, size_t i, const ProbeHit& h, const Expect& e) {
        if (logged++ < 12)
            MC2_EVENT(Vk, "VK_RT_SCENE", 1, "material miss kind={} ray={} hit={} t={:.3f} custom=0x{:x}/0x{:x} row={}/{} prim={}/{} layer={}/{} packet={}/{} flags={}", kind, i,
                      h.hit ? 1 : 0, h.t, h.custom, e.custom, h.row, e.row, h.prim, e.prim, h.layer, e.layer, h.packet, e.packet, h.flags);
    };
    for (size_t i = 0; i < propX.size(); ++i) {
        const ProbeHit a = hitAt(outA, i), o = hitAt(outO, i);
        const Expect& e = propX[i];
        const bool ok = a.hit && a.custom == e.custom && a.row == e.row && a.layer == e.layer && a.packet == static_cast<int32_t>(e.packet) &&
                        (a.flags & static_cast<int32_t>(kMatProp)) != 0;
        if (ok) {
            ++propOk;
            if (a.prim != e.prim) primInfo("prop", i, a, e);
        } else {
            report("prop", i, a, e);
        }
        opaqueDiffs += sameHit(a, o) ? 0u : 1u;
    }
    for (size_t k = 0; k < vegX.size(); ++k) {
        const size_t i = vegFirst + k;
        const ProbeHit o = hitAt(outO, i);
        const Expect& e = vegX[k];
        const bool ok = o.hit && o.custom == e.custom && o.row == e.row && o.layer == e.layer && o.packet == static_cast<int32_t>(e.packet) &&
                        (o.flags & static_cast<int32_t>(kMatCutout)) != 0;
        if (ok) {
            ++vegOk;
            if (o.prim != e.prim) primInfo("vegetation", i, o, e);
        } else {
            report("vegetation", i, o, e);
        }
        for (size_t g = vegGridFirst[k]; g < vegGridFirst[k] + vegGridCount[k]; ++g) {
            const ProbeHit ga = hitAt(outA, g), go = hitAt(outO, g);
            const bool onTri = go.hit && go.custom == e.custom && go.row == e.row && go.prim == e.prim;
            const bool alphaOnTri = ga.hit && ga.custom == e.custom && ga.row == e.row && ga.prim == e.prim;
            if (onTri && !alphaOnTri) ++holes;
            if (go.hit && go.flags >= 0 && (go.flags & static_cast<int32_t>(kMatCutout)) == 0 && !sameHit(ga, go)) ++falseHoles;
        }
    }
    for (size_t k = 0; k < terrX.size(); ++k) {
        const size_t i = terrFirst + k;
        const ProbeHit a = hitAt(outA, i), o = hitAt(outO, i);
        const Expect& e = terrX[k];
        const bool ok = a.hit && a.custom == e.custom && a.row == e.row && a.packet == static_cast<int32_t>(e.packet) && a.flags >= 0 &&
                        (a.flags & static_cast<int32_t>(kMatTerrain)) != 0;
        if (ok) ++terrOk;
        else report("terrain", i, a, e);
        opaqueDiffs += sameHit(a, o) ? 0u : 1u;
    }
    const bool pass = !propX.empty() && propOk == propX.size() && !vegX.empty() && vegOk == vegX.size() && !terrX.empty() && terrOk == terrX.size() &&
                      holes > 0 && falseHoles == 0 && opaqueDiffs == 0;
    MC2_EVENT(Vk, "VK_RT_SCENE", 1, "material result={} props={}/{} vegetation={}/{} terrain={}/{} holes={} false_holes={} opaque_diffs={} prim_info={} rows={} rays={}",
              pass ? "PASS" : "FAIL", propOk, propX.size(), vegOk, vegX.size(), terrOk, terrX.size(), holes, falseHoles, opaqueDiffs, primDiffs, rowCount, n);
}

// A mover leaves: its BLAS retires frame-fenced (the TLAS of an in-flight frame may still reference it) and its slot becomes reusable.
void dropMover(VulkanContext& ctx, State& s, size_t i, std::string_view why) {
    s.moverTlasDirty = true;
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
    s.moverTlasDirty = true;
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

bool demanded() { return consumers().needed(rayContext() != nullptr); }

void setConsumer(Consumer consumer, bool registered) {
    if (!consumers().set(consumer, registered)) return;
    invalidateProps();
    if (State::exists()) {
        // invalidateProps retains terrain BLASes, but may have retired their
        // only TLAS. Resample even an unchanged surface when demand returns.
        State& s = State::instance();
        s.terrainKeyLive = s.terrainKeyFailed = kNone;
    }
}

void noteTerrain(const TerrainSource& t) {
    if (!demanded()) return;
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
        std::vector<std::vector<TriangleGeometry>> geos;
        for (const Range& r : ranges) {
            TriangleGeometry g;
            g.vertices = vb.deviceAddress() + static_cast<uint64_t>(r.firstVertex) * 12;
            g.vertexCount = r.vertexCount;
            g.indices = ib.deviceAddress() + static_cast<uint64_t>(r.firstIndex) * 4;
            g.triangleCount = r.indexCount / 3;
            geos.push_back(g.triangleCount ? std::vector<TriangleGeometry>{g} : std::vector<TriangleGeometry>{});
        }
        ok = buildStatic(*ctx, s, geos, "rt_scene.terrain", built, st);
    }
    if (vb) ctx->retire(std::move(vb));
    if (ib) ctx->retire(std::move(ib));
    uint32_t count = 0, props = 0, dropped = 0;
    std::optional<AccelStructure> tlas;
    if (ok) tlas = buildTlas(*ctx, s, built, {}, s.propRows, s.typeRowBase, s.uvIndices, count, props, dropped);
    if (!tlas) {
        retireEach(*ctx, built);
        s.terrainKeyFailed = key;
        MC2_EVENT(Vk, "VK_RT_SCENE", 1, "FAIL terrain build tiles={} key=0x{:x} stage={} why={}; keeping the last snapshot", ranges.size(), key, g_failStage, g_failWhy);
        return;
    }
    // Publish: the old TLAS retires before the BLASes it referenced.
    if (s.tlas) ctx->retire(std::move(s.tlas));
    s.tlas = std::move(*tlas);
    if (s.moverTlas) ctx->retire(std::move(s.moverTlas));   // it references the replaced BLASes; the next mover frame rebuilds it
    s.staticInst = std::move(s.pendingInst);
    s.moverTlasDirty = true;
    publishMaterials(*ctx, s);
    retireEach(*ctx, s.terrainBlas);
    s.terrainBlas = std::move(built);
    s.terrainKeyLive = key;
    s.terrainKeyFailed = kNone;
    s.tlasInstances = count;
    s.tlasProps = props;
    s.dropped = dropped;
    s.terrainProbe.clear();
    if (mc2::gate::get<"MC2_VK_RT_SELFTEST">()) {
        TerrainGrid& g = s.terrainGrid;
        g.side = static_cast<int32_t>(terrainGridSide(t));
        g.step = terrainUsesFine(t) ? 32.0f : 128.0f;
        g.halfMap = t.halfMap;
        g.h.resize(static_cast<size_t>(g.side) * static_cast<size_t>(g.side));
        for (int32_t gy = 0; gy < g.side; ++gy)
            for (int32_t gx = 0; gx < g.side; ++gx) g.h[static_cast<size_t>(gx) + static_cast<size_t>(gy) * static_cast<size_t>(g.side)] = terrainGridHeight(t, gx, gy);
    }
    const int32_t side = static_cast<int32_t>(terrainGridSide(t));
    const uint32_t tileCells = terrainTileCells(t);
    for (const auto& [fx, fy] : {std::pair{1, 1}, std::pair{1, 3}, std::pair{3, 1}, std::pair{3, 3}}) {
        const int32_t gx = side * fx / 4, gy = side * fy / 4;
        ProbePoint p{terrainGridX(t, gx), terrainGridY(t, gy), terrainGridHeight(t, gx, gy)};
        p.mx = 0.5f * (terrainGridX(t, gx) + terrainGridX(t, gx + 1));
        p.my = 0.5f * (terrainGridY(t, gy) + terrainGridY(t, gy + 1));
        p.tile = terrainTileId(t, std::clamp<uint32_t>(static_cast<uint32_t>(gx) / tileCells, 0u, per - 1), std::clamp<uint32_t>(static_cast<uint32_t>(gy) / tileCells, 0u, per - 1));
        s.terrainProbe.push_back(p);
    }
    s.probed = false;
    s.matProbed = false;
    s.matProbeWait = 0;
    logPublish(s, terrainUsesFine(t) ? "terrain_fine" : "terrain_coarse", st, msSince(t0));
    probe(*ctx, s);
    probeMaterials(*ctx, s);
}

static void updatePropsNow(uint64_t propVbo) {
    if (!demanded()) return;
    VulkanContext* ctx = rayContext();
    if (!ctx || propVbo == 0) return;
    State& s = State::instance();
    if (propVbo != s.propVbo) {   // a new map's VBO: every prop BLAS was built from the old one
        invalidateProps();
        s.propVbo = propVbo;
    }
    const uint64_t gen = GpuStaticPropRegistry::getRegistryGeneration();
    if (!s.compactedDirty && (gen == s.propGen || gen == s.propGenFailed)) {
        if (!s.frameCmd) probeMaterials(*ctx, s);   // frame builds must be submitted before a blocking diagnostic trace
        return;
    }
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

    // The types that still need a BLAS: one combined rebased IBO, one BLAS per type with one geometry per non-empty packet over the shared VBO.
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
    bool sameInstances = ids == s.objectIds && inst.size() == s.instances.size();
    for (size_t i = 0; sameInstances && i < inst.size(); ++i)
        sameInstances = samePlacement({inst[i].typeID, ids[i], inst[i].modelMatrix},
                                     {s.instances[i].typeID, s.objectIds[i], s.instances[i].modelMatrix});
    if (!updateNeeded(s.propGen != kNone, s.compactedDirty || !types.empty() || !sameInstances)) {
        s.propGen = gen;   // registry light/fog writes are irrelevant to RT
        s.propGenFailed = kNone;
        if (!s.frameCmd) probeMaterials(*ctx, s);
        return;
    }
    // Materials: the colour-prop island's packet routing (layer per packet, alpha class per type, uv crop). A new type waits a bounded number of calls
    // for it, then builds untextured (every geometry opaque).
    Mc2ColorPropMaterialFeed mf{};
    const bool haveMat = mc2_color_prop_island_get_material_feed(&mf) && mf.packetAlbedoLayer && mf.typeAlphaClass && mf.packetCount > 0;
    if (!haveMat && !types.empty() && s.matWait < kMatWaitCalls) {
        ++s.matWait;
        return;
    }
    auto packetMaterial = [&](uint32_t ty, uint32_t pk) {
        PacketMaterial m;
        m.alphaTest = mc2_static_prop_packet_alpha_test(pk);
        if (!haveMat || pk >= mf.packetCount || ty >= mf.typeCount) return m;
        (mf.typeAlphaClass[ty] == 1 ? m.layerOn : m.layerOff) = mf.packetAlbedoLayer[pk];
        if (mf.packetUvScaleX && mf.packetUvScaleY) {
            m.uvX = mf.packetUvScaleX[pk];
            m.uvY = mf.packetUvScaleY[pk];
        }
        if (mf.packetMaterialIds) m.materialId = mf.packetMaterialIds[pk];
        return m;
    };
    struct Geo {
        uint32_t firstIndex, triangles;
        bool opaque;
    };
    std::vector<MaterialRow> newRows;
    std::vector<uint32_t> newRowBase;
    if (!types.empty()) {
        newRows = s.propRows;
        newRowBase = s.typeRowBase;
        newRowBase.resize(feed.typeCount, kNoRow);
    }
    auto& rows = types.empty() ? s.propRows : newRows;
    auto& rowBase = types.empty() ? s.typeRowBase : newRowBase;
    const uint32_t uvBase = static_cast<uint32_t>(s.uvIndices.size());
    std::vector<std::vector<Geo>> perType(types.size());
    std::vector<std::array<std::vector<Target>, 2>> targets(types.size());
    std::vector<Target> allTris;
    const bool selftest = mc2::gate::get<"MC2_VK_RT_SELFTEST">();
    auto position = [&](uint32_t v, float* o) { std::memcpy(o, static_cast<const uint8_t*>(vboCpu) + static_cast<size_t>(v) * feed.vertexStride, 12); };
    for (size_t k = 0; k < types.size(); ++k) {
        const uint32_t ty = types[k];
        rowBase[ty] = static_cast<uint32_t>(rows.size());
        allTris.clear();
        const uint32_t first = feed.typeFirstPacket[ty], count = feed.typePacketCount[ty];
        for (uint32_t p = first; p < first + count && p < feed.packetCount; ++p) {
            const uint32_t at = static_cast<uint32_t>(indices.size());
            const PacketRange pr{feed.packets[p].firstIndex, feed.packets[p].indexCount, feed.packets[p].baseVertex};
            const uint32_t tris = appendRebasedIndices(ibo, std::span<const PacketRange>(&pr, 1), vertexCount, indices);
            if (tris == 0) continue;
            const MaterialRow row = propMaterialRow(packetMaterial(ty, p), uvBase + at, p);
            const bool cutout = (row.flags & kMatCutout) != 0;
            perType[k].push_back({at, tris, !cutout});
            // MC2_VK_RT_SELFTEST targets: every triangle of the type, narrowed to candidates after the packet loop.
            for (uint32_t tri = 0; selftest && tri < tris; ++tri) {
                float v[9];
                for (int j = 0; j < 3; ++j) position(indices[at + 3 * tri + j], &v[3 * j]);
                const float e1[3] = {v[3] - v[0], v[4] - v[1], v[5] - v[2]}, e2[3] = {v[6] - v[0], v[7] - v[1], v[8] - v[2]};
                const float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
                const float area = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
                if (area > 0.0f) {
                    Target tg;
                    std::memcpy(tg.v, v, sizeof(v));
                    tg.row = static_cast<uint32_t>(rows.size());
                    tg.prim = tri;
                    tg.area = area;
                    tg.cutout = cutout;
                    tg.valid = true;
                    allTris.push_back(tg);
                }
            }
            rows.push_back(row);
        }
        if (!allTris.empty()) {
            targets[k][0] = pickCandidates(allTris, false);
            targets[k][1] = pickCandidates(allTris, true);
        }
    }
    std::vector<uint32_t> newUv;
    if (!indices.empty()) {
        newUv = s.uvIndices;
        newUv.insert(newUv.end(), indices.begin(), indices.end());
    }
    const auto& uv = indices.empty() ? s.uvIndices : newUv;
    OwnedBuffer ib;
    std::vector<AccelStructure> built;
    BuildStats st;
    bool ok = true;
    if (!indices.empty()) {
        ok = ib.createFilled(ctx->allocator(), ctx->device(), indices.data(), indices.size() * sizeof(uint32_t), kBuildInput, "rt_scene.prop_ib");
        if (ok) {
            std::vector<std::vector<TriangleGeometry>> geos(types.size());
            for (size_t i = 0; i < types.size(); ++i)
                for (const Geo& pg : perType[i]) {
                    TriangleGeometry g;
                    g.vertices = propVbo;
                    g.vertexStride = feed.vertexStride;
                    g.vertexCount = vertexCount;
                    g.indices = ib.deviceAddress() + static_cast<uint64_t>(pg.firstIndex) * 4;
                    g.triangleCount = pg.triangles;
                    g.opaque = pg.opaque;
                    geos[i].push_back(g);
                }
            ok = buildStatic(*ctx, s, geos, "rt_scene.prop", built, st, types);
        }
    }
    if (ib) ctx->retire(std::move(ib));
    std::vector<std::pair<uint32_t, AccelStructure>> added;
    added.swap(s.compacted);
    for (size_t i = 0; i < built.size(); ++i) added.emplace_back(types[i], std::move(built[i]));

    // The TLAS reads the new snapshot; on failure the previous instances / records stay published.
    std::swap(s.instances, inst);
    std::swap(s.objectIds, ids);
    uint32_t count = 0, props = 0, dropped = 0;
    std::optional<AccelStructure> tlas;
    if (ok) tlas = buildTlas(*ctx, s, s.terrainBlas, added, rows, rowBase, uv, count, props, dropped);
    if (!tlas) {
        std::swap(s.instances, inst);
        std::swap(s.objectIds, ids);
        for (auto& [ty, a] : added)
            if (a) ctx->retire(std::move(a));
        s.propGenFailed = gen;
        s.compactedDirty = false;
        MC2_EVENT(Vk, "VK_RT_SCENE", 1, "FAIL prop build gen={} types={} instances={} stage={} why={}; keeping the last snapshot", gen, types.size(), inst.size(), g_failStage, g_failWhy);
        return;
    }
    if (s.tlas) ctx->retire(std::move(s.tlas));
    s.tlas = std::move(*tlas);
    // The retained mover TLAS has the old membership, BLAS addresses and terrain row offsets.
    if (s.moverTlas)
        ctx->retire(std::move(s.moverTlas));
    s.staticInst = std::move(s.pendingInst);
    s.moverTlasDirty = true;
    publishMaterials(*ctx, s);
    for (auto& [ty, a] : added) {
        if (s.propBlas[ty]) ctx->retire(std::move(s.propBlas[ty]));
        s.propBlas[ty] = std::move(a);
    }
    if (!types.empty() && !haveMat) ++s.propsWithoutMaterial;
    if (!types.empty()) {
        s.propRows = std::move(newRows);
        s.typeRowBase = std::move(newRowBase);
    }
    if (!indices.empty()) s.uvIndices = std::move(newUv);
    s.propStride = feed.vertexStride;
    if (s.typeTarget.size() < feed.typeCount) s.typeTarget.resize(feed.typeCount);
    for (size_t k = 0; k < types.size(); ++k) s.typeTarget[types[k]] = targets[k];
    for (uint32_t ty : types) s.propTried[ty] = 1;
    s.propGen = gen;
    s.propGenFailed = kNone;
    s.tlasInstances = count;
    s.tlasProps = props;
    s.dropped = dropped;
    s.compactedDirty = false;
    s.probed = s.matProbed = false;
    logPublish(s, haveMat || types.empty() ? "props" : "props_untextured", st, msSince(t0));
    if (!s.frameCmd) {
        probe(*ctx, s);
        probeMaterials(*ctx, s);
    }
}

void updateProps(uint64_t propVbo) {
    if (!demanded() || !propVbo) return;
    State& s = State::instance();
    if (!deferPropBuild(s.propGen != kNone, propVbo == s.propVbo)) updatePropsNow(propVbo);
    else if (const uint64_t gen = GpuStaticPropRegistry::getRegistryGeneration(); gen != s.propGen && gen != s.propGenFailed) s.queuedVbo = propVbo;
    else probeMaterials(*rayContext(), s);
}

// Called by the scene flush even when the mech island draws nothing. The pool belongs to the frame slot; no one-shot wait or private queue.
void recordFrame() {
    if (!demanded() || !State::exists()) return;
    auto& ctx = *rayContext();
    State& s = State::instance();
    if (!s.queuedVbo && s.compactions.empty() && !s.compactedDirty) return;
    const VkCommandBuffer cmd = ctx.acquireRecordCommandBuffer();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!cmd || vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return;
    s.frameCmd = cmd;
    s.frameWork = false;
    recordBarrier(ctx, cmd, frameRebuildBarrier());
    if (!s.compactions.empty()) {
        ZoneScopedN("RT.AsyncCompaction.Record");
        std::vector<std::pair<uint32_t, VkDeviceSize>> copies;
        for (size_t b = 0; b < s.compactions.size();) {
            auto& batch = s.compactions[b];
            bool ready = ctx.frameContext().frame > batch.frame;
            for (size_t i = 0; ready && i < batch.types.size(); ++i) {
                if (!batch.addresses[i]) continue;
                VkDeviceSize bytes = 0;
                const VkResult r = vkGetQueryPoolResults(ctx.device(), batch.query.get(), static_cast<uint32_t>(i), 1, sizeof(bytes), &bytes, sizeof(bytes), VK_QUERY_RESULT_64_BIT);
                if (r == VK_NOT_READY) { ready = false; break; }
                const uint32_t type = batch.types[i];
                if (r == VK_SUCCESS && type < s.propBlas.size() && s.propBlas[type].address() == batch.addresses[i] && worthCompacting(s.propBlas[type].size(), bytes))
                    copies.emplace_back(type, bytes);
                batch.addresses[i] = 0;
            }
            if (!ready) { ++b; continue; }
            ctx.retire(std::move(batch.query));
            s.compactions.erase(s.compactions.begin() + b);
        }
        if (!copies.empty()) {
            MC2_VK_GPU_SCOPE(compactionScope, cmd, "RT.AsyncCompaction");
            // A profiler query must be submitted even if every copy allocation fails.
            s.frameWork |= mc2vk::gpuprof::active();
            for (const auto& [type, bytes] : copies) {
                AccelStructure compact;
                if (recordCompactCopy(ctx, cmd, s.propBlas[type], bytes, "rt_scene.prop", compact)) {
                    s.frameWork = true;
                    s.compacted.emplace_back(type, std::move(compact));
                    s.compactedDirty = true;
                }
            }
        }
    }
    recordBarrier(ctx, cmd, Phase::Copy, Phase::Tlas);
    if (s.queuedVbo || s.compactedDirty) updatePropsNow(s.queuedVbo ? s.queuedVbo : s.propVbo);
    s.queuedVbo = 0;
    s.frameCmd = VK_NULL_HANDLE;
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) { invalidateProps(); return; }
    if (!s.frameWork) return;   // raster-only registry writes need no submit
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (ctx.submit(si, VK_NULL_HANDLE) != VK_SUCCESS) { invalidateProps(); return; }
    probe(ctx, s);
    probeMaterials(ctx, s);
}

void invalidateProps() {
    if (!State::exists()) return;
    VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    State& s = State::instance();
    // The TLAS references the prop BLASes: it retires first. Terrain BLASes stay; the next updateProps (or noteTerrain) republishes a TLAS.
    for (auto& pending : s.compactions) ctx.retire(std::move(pending.query));
    s.compactions.clear();
    for (auto& [ty, a] : s.compacted) if (a) ctx.retire(std::move(a));
    s.compacted.clear();
    s.queuedVbo = 0;
    s.compactedDirty = false;
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
    // The material table references the prop rows: it goes with the TLAS.
    retirePending(ctx, s);
    if (s.materials) ctx.retire(std::move(s.materials));
    if (s.uvIndex) ctx.retire(std::move(s.uvIndex));
    s.materialBytes = 0;
    s.propRows.clear();
    s.typeRowBase.clear();
    s.uvIndices.clear();
    s.typeTarget.clear();
    s.propStride = 0;
    s.matWait = s.matProbeWait = 0;
    s.matProbed = false;
}

void recordMovers(VkCommandBuffer_T* cb, uint32_t slot, std::span<const MoverGeometry> geos) {
    if (!demanded()) return;
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

    // Leaves must be resolved before jobs take pointers into the mover vector.
    // Drawn actors stay even though lastSeen is updated by pass 2 below.
    if (frame >= s.lastLiveFrame + 30) {
        s.lastLiveFrame = frame;
        for (size_t i = s.movers.size(); i-- > 0;) {
            const Mover& m = s.movers[i];
            const auto drawn = std::lower_bound(actors.begin(), actors.end(), m.key, [](const Actor& a, uint32_t key) { return a.key < key; });
            if (drawn != actors.end() && drawn->key == m.key) continue;
            const bool handle = moverIsHandleKey(m.key);
            bool alive = false;
            if (handle) {
                RenderWorld::ObjectRecordView view;
                alive = RenderWorld::getObjectRecordView(RenderCore::RenderObjectHandle::make(m.key & 0xFFFFFu, m.key >> 20), &view) && view.valid;
            }
            if (moverLeaves(handle, alive, frame - m.lastSeen)) dropMover(*ctx, s, i, handle ? "handle_dead" : "unseen");
        }
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

    if (!updateNeeded(static_cast<bool>(s.moverTlas), s.moverTlasDirty || !jobs.empty())) {
        logMoverLedger(*ctx, s, frame, static_cast<uint32_t>(s.staticInst.size() + s.movers.size()));
        return;
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
        // Allocated on a 16-byte address (kInstanceAddressAlignment): the driver reports 4 for a host buffer on the RX 7900 XTX.
        if (s.moverInst[fs].createHostWritable(ctx->allocator(), ctx->device(), static_cast<VkDeviceSize>(cap) * kInstanceBytes, kBuildInput,
                                               "rt_scene.mover_instances", -1, -1, kInstanceAddressAlignment))
            s.moverInstCap[fs] = cap;
        else
            ok = false;
    }
    const VkDeviceAddress instBase = ok ? s.moverInst[fs].deviceAddress() : 0;
    if (ok) ok = s.moverInst[fs].upload(s.instStage.data(), static_cast<VkDeviceSize>(count) * kInstanceBytes);
    if (ok) {
        BuildFlags flags;
        flags.preferFastTrace = false;
        const uint32_t capacity = s.moverInstCap[fs];
        ok = recordTlasRebuild(*ctx, cmd, instBase, count, capacity, flags, "rt_scene.mover_tlas", s.moverTlas, s.moverScratch);
        if (!ok && s.moverTlas) {   // too small: a new one sized for the capacity
            ctx->retire(std::move(s.moverTlas));
            s.moverTlas = AccelStructure{};
            ok = recordTlasRebuild(*ctx, cmd, instBase, count, capacity, flags, "rt_scene.mover_tlas", s.moverTlas, s.moverScratch);
        }
        if (ok) recordBarrier(*ctx, cmd, Phase::Tlas, Phase::Consumer);
    }
    if (!ok && count > 0 && ++s.moverTlasFailed <= 3)
        MC2_EVENT(Vk, "VK_RT_SCENE", 1, "FAIL mover tlas count={} cap={} inst_addr=0x{:x} tlas_bytes={} scratch_bytes={}", count, s.moverInstCap[fs],
                  instBase, s.moverTlas ? s.moverTlas.size() : 0, s.moverScratch.capacity());
    if (!ok && s.moverTlas) ctx->retire(std::move(s.moverTlas));   // never leave a TLAS whose build was not recorded this frame
    s.moverTlasDirty = !ok;
    s.moverScratch.retireStale(*ctx);
    logMoverLedger(*ctx, s, frame, ok ? count : 0);
}

}  // namespace mc2vk::rt::scene
