#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <atomic>
#include <vector>
#include <unordered_map>
#include "Stuff/Stuff.hpp"
#include "tgl.h"
#include "msl.h"
#include "texture_decode.h"
#include "../../RenderCore/RenderDebugView.h"  // DEBUG-VIEW-REGISTRY-1: StaticPropViewToShaderMode / StaticPropShaderModeToView

// MaterialGpu-3: forward declare RenderCore::MaterialGpu so batcher_getMaterialGpuEntry
// can be declared without pulling in the full MaterialGpu.h into every TU that includes this header.
namespace RenderCore { struct MaterialGpu; }
namespace RenderCore { struct StaticPropTypeDesc; }

#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"

struct PerDrawEntry {
    int32_t packetID;          //  0 — index into s_packets[]
    int32_t materialFlags;     //  4 — 0 or STATIC_PROP_FLAG_ALPHA_TEST
    int32_t maxLocalVertexID;  //  8 — type.vertexCount - 1
    int32_t texArrayLayer;     // 12 — legacy texture array layer; compare authority until
                               //      MaterialGpu fully retired (must equal materials[materialIdx].albedoTex)
    float   uvScaleX;          // 16 — 1.0f for Stage A
    float   uvScaleY;          // 20 — 1.0f for Stage A
    int32_t  objectIdRaw;      // 24 — M1.5: handle.raw() when MC2_OBJECT_ID_BUFFER=1, else 0
    uint32_t materialIdx;      // 28 — index into MaterialGpu table (s_materialGpuTable[]).
                               //      albedoTex at this index must equal texArrayLayer (compare invariant).
};
static_assert(sizeof(PerDrawEntry) == 32, "PerDrawEntry std430 size");
static_assert(offsetof(PerDrawEntry, packetID)         ==  0, "packetID offset");
static_assert(offsetof(PerDrawEntry, materialFlags)    ==  4, "materialFlags offset");
static_assert(offsetof(PerDrawEntry, maxLocalVertexID) ==  8, "maxLocalVertexID offset");
static_assert(offsetof(PerDrawEntry, texArrayLayer)    == 12, "texArrayLayer offset");
static_assert(offsetof(PerDrawEntry, uvScaleX)         == 16, "uvScaleX offset");
static_assert(offsetof(PerDrawEntry, uvScaleY)         == 20, "uvScaleY offset");
static_assert(offsetof(PerDrawEntry, objectIdRaw)      == 24, "objectIdRaw offset");
static_assert(offsetof(PerDrawEntry, materialIdx)      == 28, "materialIdx offset");

struct GpuStaticPropPacket {
    uint32_t firstIndex;     // into shared IBO
    uint32_t indexCount;
    int32_t  baseVertex;     // into shared VBO
    uint32_t textureSlot;    // index into owning TG_TypeShape::listOfTextures.
                             // Resolved at draw time because MC2 mutates the
                             // handle each frame via SetTextureHandle (see
                             // msl.cpp:1321 TransformMultiShape).
    uint32_t materialFlags;  // bit 0: ALPHA_TEST_BIT
    uint32_t owningTypeID;
};

// Ring depth of the batcher's s_fence ring, exported so GpuMechBatcher can
// static_assert it shares the same fence semantics. The .cpp defines its
// private `RING_FRAMES` separately and MUST match this value.
constexpr uint32_t STATIC_PROP_RING_FRAMES = 3u;

constexpr uint32_t STATIC_PROP_FLAG_ALPHA_TEST = 1u << 0;

// Slice 2 (object-offload): free-function form of the eligibility check used
// by the addRenderShape gate in tgl.cpp:2522 (the bShadersDrawPathEnabled
// block). Cheap, side-effect-free. Returns true iff `shape` belongs to a
// registered TG_TypeShape AND g_useGpuObjects is enabled. tgl.cpp gets this
// via a forward declaration (see tgl.cpp near the gate site) to avoid
// pulling the rest of batcher.h into the legacy translation unit.
//
// Stage 2.A: declared and defined. Under default `MC2_GPU_OBJECTS=0` (i.e.
// g_useGpuObjects==false) this returns false, so the addRenderShape gate
// becomes false-equivalent and the legacy path is byte-for-byte unchanged.
bool eligibleForGpuObjects(class TG_Shape* shape);

// GPU-INSTANCE-SKIP-POOLS-1: true iff MC2_LEGACY_INSTANCE_POOLS is set (escape
// hatch forcing the legacy per-instance frame-pool path). Default false → the
// GPU-only zero-pool path. Read once at process start.
bool gos_StaticPropLegacyInstancePools();

// Population tag — passed by caller so the batcher can split per-population
// counts in the [OBJBATCHER v1] summary. Not stored; consumed inside submit
// only.
//
// Legacy is the slice-1 fallback branch (g_useGpuStaticProps && !g_useGpuObjects)
// in *Appearance::render. Counted separately so Gate F's fallback-rate
// computation uses only slice-1 populations (Building/Tree/Generic).
enum class GpuStaticPropPopulation : uint8_t {
    Building = 0,
    Tree     = 1,
    Generic  = 2,
    Legacy   = 3,
};

// Per-type descriptor: range of packets + vertex count (for color block sizing).
//
// Substrate-coalesce fields (instanceCap, coalesceByteOffsetWithinGroup,
// lastSeenGosHandle, alphaClass) are populated by finalizeGeometry() and
// are UNDEFINED before then. Step group 1.1 of plan v3.8.
struct GpuStaticPropType {
    uint32_t firstPacket;
    uint32_t packetCount;
    uint32_t vertexCount;    // number of vertices in the owning TG_TypeShape
    const TG_TypeShape* source;
    uint32_t instanceCap;                    // §5.1 per-type capacity (2× avg)
    uint32_t coalesceByteOffsetWithinGroup;  // §5.1b group-relative byte offset
    uint32_t lastSeenGosHandle;              // §5.4 eviction-detect snapshot
    uint8_t  alphaClass;                     // §CRITICAL-C 0=alpha-OFF, 1=alpha-ON
    // MODEL-OVERRIDE-GPU-BATCHER-SEAM: set true for types registered from a
    // modder glTF render-override multishape. Override geometry imports with
    // unresolved "NULLTXM" texture handles (nodeIdx=0xFFFFFFFF), so its packets
    // would get layerForPacket=-1 and be culled from the coalesce multidraw
    // (the bdactor.cpp damage-shape skip rule). When this flag is set the
    // batcher routes such packets to a valid default texture layer so the
    // override RASTERIZES (flat/wrong-texture box) instead of vanishing.
    bool     isOverride;
};

// Forward declaration needed for flush() signature (v2.3).
// Full definition in render_snapshot.h; batcher.cpp includes it directly.
// Phase-helper state of GpuStaticPropBatcher::flush (defined in gos_static_prop_batcher_flush.cpp).
struct StaticPropFinalizeFrame;
struct StaticPropDispatchMeta;
namespace RenderCore { struct DrawPacket; }
struct RenderSnapshot;

class GpuStaticPropBatcher {
public:
    static GpuStaticPropBatcher& instance();

    // Called from gameosmain at map load / unload.
    void onMapLoad();
    void onMapUnload();

    // V-IBL-STATIC-2: select the SH coefficient set for the loaded mission.
    // Called from Mission::init right after onMapLoad(). Safe to call with
    // nullptr/empty (-> "default"). Per-mission registry lives
    // in RenderCore/IblShRegistry.h (kMissionShMap).
    static void setMissionForIbl(const char* missionName);

    // V-IBL-STATIC-2: inspector accessor. Returns the active SH set name
    // (e.g. "default"). Never returns nullptr.
    static const char* getCurrentShSetName();

    // Register one TG_TypeShape (idempotent). Builds packet table entries
    // and appends geometry to the in-progress VBO/IBO staging.
    // Called during onMapLoad for every static-prop type + its damage variants.
    // multiShape is the parent type-multi-shape, used to look up texture
    // names for register-time alpha-test material classification (Path 4).
    void registerType(TG_TypeShape* typeShape, TG_TypeMultiShape* multiShape);

    // Convenience wrapper: iterate a multishape's listOfTypeShapes and call
    // registerType on each SHAPE_NODE leaf. Safe to call with NULL (no-op).
    // isOverride: tag every leaf type registered here as a modder glTF
    // render-override (see GpuStaticPropType::isOverride). Default false keeps
    // the stock call sites byte-identical.
    void registerMultiShape(TG_TypeMultiShape* multiShape, bool isOverride = false);

    // Called at end of registration to upload the immutable VBO/IBO.
    void finalizeGeometry();

private:
    // GpuStaticPropBatcher::finalizeGeometry phases (gos_static_prop_batcher_finalize*.cpp); callers use finalizeGeometry().
    static bool packetHasAlpha(const GpuStaticPropPacket& pkt, const TG_TypeShape* src);
    void finalizePrologue();
    void publishGeometry();
    bool prepareMaterialLayout(StaticPropFinalizeFrame& f);
    bool buildTextureArrays(StaticPropFinalizeFrame& f);
    void buildPerPacketDrawTables(StaticPropFinalizeFrame& f);
    bool reportMaterialCache(StaticPropFinalizeFrame& f);
    void armCoalesce(StaticPropFinalizeFrame& f);
    std::array<mc2tex::TextureArrayCpu, 2> albedoArrays_;
    // Per group: 0 until the group array of this map is built, then a serial that is new for every build (the change key of the island's copy).
    std::array<uint32_t, 2> albedoKey_{};
public:
    uint32_t albedoKey(uint32_t group) const { return albedoKey_[group & 1u]; }
    const mc2tex::TextureArrayCpu& albedoArray(uint32_t group) const { return albedoArrays_[group & 1u]; }
    bool packetAlphaTest(uint32_t packet) const;

    // Per-frame submission. shape->listOfColors must be fresh (set by
    // TransformMultiShape earlier in the frame).
    // Returns true if the instance was accepted. Returns false (Layer B
    // safety net) when the type was never registered; in that case the
    // caller MUST render the shape via the old CPU path this frame.
    //
    // Slice 2 (object-offload) — Stage 2.C: lightDataIndex parameter is
    // the dedup-cache index returned by TG_Shape::GatherGpuObjectLightDataOnly()
    // for the OWNING multishape (per-actor, not per-leaf — Recon Section 9
    // Item 5 confirmed all leaves see identical lightData_). Caller hoists
    // the gather call between submitMultiShape's two for-loops and
    // broadcasts the index into every leaf's submit() in the second loop.
    [[nodiscard]] bool submit(TG_Shape* shape,
                              const glm::mat4& shapeToWorld,
                              uint32_t highlightARGB,
                              uint32_t fogARGB,
                              uint32_t flags,
                              uint32_t lightDataIndex);

    // Iterate a multishape's children and submit each SHAPE_NODE leaf using
    // the child's own listOfShapes[i].shapeToWorld. Per-child highlight/fog/
    // flags are pulled from the TG_Shape node itself. Returns false if ANY
    // child fails registration — caller MUST CPU-fallback the whole
    // multishape for this frame to keep the visual self-consistent.
    // Caller-side accounting. recordEligibleActor() is called by
    // *Appearance::render BEFORE submit so caller-side bypasses
    // (e.g., null shape) still count toward eligible_actors.
    // recordCpuFallback() is called when no submit succeeded.
    void recordEligibleActor(GpuStaticPropPopulation pop);
    void recordCpuFallback(GpuStaticPropPopulation pop);

    // Slice 2 (object-offload) — Stage 2.C+ instrumentation: optional
    // callerName (e.g., appearType->name) is included in the
    // [OBJBATCHER v1] event=late_register log line when a leaf type fails
    // registration. Pass nullptr if no caller-side name is available
    // (legacy path); the log will fall back to the bare TG_TypeShape
    // pointer. Helps identify which actor classes own the unregistered
    // type so the allowlist / registration walk can be repaired.
    [[nodiscard]] bool submitMultiShape(TG_MultiShape* multi,
                                        GpuStaticPropPopulation pop,
                                        const char* callerName = nullptr);

    // Slice 2 (object-offload): cheap, side-effect-free eligibility query.
    // Returns true iff every leaf SHAPE_NODE under `multi` is currently
    // registered in the batcher AND g_useGpuObjects is enabled. Mirrors slice
    // 1's render-time per-child gates EXCEPT the late-registration case
    // (Recon Section 9 Item 4). Stage 2.A: declared and defined; Stage 2.B
    // wires it into BldgAppearance/TreeAppearance/GenericAppearance::update
    // inside their existing cull gates. MUST NOT mutate any batcher state,
    // emit any logs, or increment any counters — this is a read-only check
    // used by the addRenderShape gate at tgl.cpp:2522 in the legacy path.
    bool isMultiShapeEligibleForGpuObjects(const TG_MultiShape* multi) const;

    // Per-frame dispatch.
    // Main color pass. snap: optional prior-frame RenderSnapshot the DrawPacket v6 dispatch builds its
    // packet+meta arrays from. snap pointer is not stored; read only during this call.
    void flush(const RenderSnapshot* snap = nullptr);

    // Debug: color-address validation mode. 0=off, 1=gradient, 2=hash.
    void setDebugAddrMode(int mode);
    int  getDebugAddrMode() const { return debugAddrMode_; }

    // Slice 2 (Stage 2.A) — late-registration signal for the caller.
    // Returns true iff the most recent submitMultiShape() call returned false
    // specifically because a SHAPE_NODE child type was not registered at
    // submission time (the late-registration branch, not some other failure
    // such as a fatal GPU error or program-load failure).
    //
    // Stage 2.B wires the caller: *Appearance::render queries this after a
    // false return from submitMultiShape and sets needsFullBakeNextFrame=true
    // on the owning actor so the map-load registration walk picks it up.
    //
    // The flag is cleared (reset to false) at the TOP of every
    // submitMultiShape() call so a stale "true" from a prior call never
    // masquerades as a signal for the current call.
    bool wasLastFailureLateRegistration() const;

    // Slice 2 (Stage 2.D.3) — late-registration event counters.
    //
    // Diagnostic aggregates of the late-reg events. Both are monotonic since
    // process start.
    //
    //   getAllowedLateRegEventCount():   count of allowed=1 events
    //                                    (allowlisted nodeId, e.g. skybox)
    //   getDisallowedLateRegEventCount(): count of allowed=0 events
    //                                    (unallowlisted; non-zero is a STOP)
    //
    // Both increment per occurrence inside submitMultiShape's late-reg
    // branch (NOT once-per-type — the dedup-print logic stays the same,
    // but every event ticks the counter so the true frequency is visible).
    static uint64_t getAllowedLateRegEventCount();
    static uint64_t getDisallowedLateRegEventCount();

    // Stage 3.C: snapshot of all leaf instances built by the most recent
    // successful submitMultiShape() call. Valid only until the next
    // submitMultiShape() call. Used by GpuStaticPropRegistry::registerRecipe()
    // to snapshot the registration batch immediately after submission.
    const std::vector<GpuStaticPropInstance>& getLastBuiltBatch() const;

    void submitCachedInstance(const GpuStaticPropInstance& inst);

    // 2b Stage 2 (Mechanism B-reinject): persistent static instance store.
    // Registry drives: clear+append on a dirty generation, reinject every frame.
    uint64_t persistentStaticGen();
    void     setPersistentStaticGen(uint64_t g);
    void     clearPersistentStatic();
    void     appendPersistentStaticRange(const GpuStaticPropInstance* arr, uint32_t count);
    void     reinjectPersistentStatic();

    [[nodiscard]] bool buildRecipeFromShape(
            TG_Shape* shape,
            const glm::mat4& shapeToWorld,
            uint32_t highlightARGB,
            uint32_t fogARGB,
            uint32_t flags,
            GpuStaticPropInstance* outRecipe) const;

private:
    GpuStaticPropBatcher() = default;

    // Declared here so the whole batcher state is visible for review;
    // implementation details live in .cpp.
    struct Impl;
    // State is file-static in .cpp to keep this header light; singleton
    // method bodies there forward to those statics.
    int debugAddrMode_ = 0;
};

// ---------------------------------------------------------------------------
// C1b free-function accessors (Track C slice C1b).
// These expose the immutable per-type geometry table so gpu_cull_compute.cpp
// can build the DrawElementsIndirectCommand buffer at mission load without
// pulling the batcher's internal anonymous namespace into another TU.
// All functions are valid only after GpuStaticPropBatcher::finalizeGeometry().
// ---------------------------------------------------------------------------

// Returns the number of registered types (= bucket count for indirect draw).
// Returns 0 if geometry is not yet finalized.
uint32_t batcher_getTypeCount();

// [PLACE_TYPE_TEST] DIAG: query whether a leaf TG_TypeShape* is registered /
// late-dropped in the batcher type table. See gos_static_prop_batcher.cpp.
void batcher_queryTypeShape(const void* typeShape, int* outRegistered, int* outFailed);

// EDITOR-STATIC-RECIPE-FROM-TYPE-1: resolve a leaf TG_TypeShape* to its finalized
// typeID. Returns false if not registered. See gos_static_prop_batcher.cpp.
bool batcher_typeIdForTypeShape(const void* typeShape, uint32_t* outTypeId);

// Per-type geometry info for indirect draw command construction.
// typeID:          index in [0, batcher_getTypeCount())
// outIndexCount:   total index count across all packets for this type
// outFirstIndex:   firstIndex of the first packet (IBO offset in indices)
// outBaseVertex:   baseVertex of the first packet (VBO offset in vertices)
// outInstanceCap:  per-frame instance capacity (safe upper bound for visibleIds[])
// Returns false if typeID is out of range or geometry not finalized.
bool batcher_getTypeDrawInfo(uint32_t  typeID,
                              uint32_t* outIndexCount,
                              uint32_t* outFirstIndex,
                              int32_t*  outBaseVertex,
                              uint32_t* outInstanceCap);

// ---------------------------------------------------------------------------
// §5.6b accessors — substrate-coalesce path (plan v3.8 Step 1.2).
// All return safe sentinels (0 / nullptr / false) before
// GpuStaticPropBatcher::finalizeGeometry() runs.
// ---------------------------------------------------------------------------

// Sorted type-order array (alpha-OFF group first, alpha-ON second).
// Length = batcher_getTypeCount(); valid until onMapUnload().
const uint32_t* batcher_getSortedTypeOrder();

// Counts per alpha group. Sum = batcher_getTypeCount().
uint32_t batcher_getAlphaOffCount();
uint32_t batcher_getAlphaOnCount();

// 2026-05-11 per-packet rework. Sorted-packet order: global packet indices
// in [alpha-OFF | alpha-ON] order. Length = batcher_getSortedPacketCount().
const uint32_t* batcher_getSortedPacketOrder();
uint32_t batcher_getSortedPacketCount();
uint32_t batcher_getAlphaOffCmdCount();   // packet count in alpha-OFF group
uint32_t batcher_getAlphaOnCmdCount();    // packet count in alpha-ON group
// Per-packet draw info (for the per-packet indirect cmd build).
bool batcher_getPacketDrawInfo(uint32_t globalPacketIdx,
                                uint32_t* outIndexCount,
                                uint32_t* outFirstIndex,
                                int32_t*  outBaseVertex,
                                uint32_t* outOwningTypeID);

// Per-packet material flags. Returns false for out-of-range index.
// Does NOT expose GpuStaticPropPacket outside the batcher TU.
bool batcher_getPacketMaterialFlags(uint32_t globalPacketIdx,
                                     uint32_t* outMaterialFlags);

// Per-type instance capacity (§5.1 formula). 0 if typeID out of range.
uint32_t batcher_getInstanceCap(uint32_t typeID);

// Per-type LIVE instance count from the last upload (= the frozen draw-pool
// span the indirect draw renders from for this type). Captured at
// uploadAllBucketsIfNeeded; for frozen static props this equals the current
// frame's pool span (1-frame lag only across a dirty rebuild). Used by the
// GPU-cull cut-off upper-bound oracle. 0 if typeID has no uploaded range.
uint32_t batcher_getTypeUploadedInstanceCount(uint32_t typeID);

// M1 FROZEN-STATIC-CULL-RECORDS: per-type global instance-pool base (alpha-group
// prefix-sum), the binding-0 slot where typeID's instances start. Valid after
// batcher_prepareBaseInstanceTable() in global-pool armed mode (see
// batcher_isBaseInstanceTableReady). global_slot(typeID, rank) = base + rank.
uint32_t batcher_getBaseInstanceForType(uint32_t typeID);

uint32_t batcher_getStaticBaseInstanceForType(uint32_t typeID);

// True when the per-type base table is valid this frame (global-pool armed and
// prepareBaseInstanceTable has run). The golden static cull-record build gates
// on this so it only scatters when the binding-0 slot layout is well-defined.
bool batcher_isBaseInstanceTableReady();

// Total instance bytes for ONE ring frame (off-group + on-group). size_t so
// sub-4GB ring totals carry without truncation.
size_t batcher_getCoalescePerFrameInstanceBytes();

// State-machine flags (§7).
bool batcher_isCoalesceLayoutReady();
bool batcher_isCoalesceArmed();

// Returns the highest admitted instance count ever seen for typeID since
// mission load. Slice-1 NOT consumed (foundation for slice 2 GPU-emit).
uint32_t batcher_getPerTypePeakCount(uint32_t typeID);

bool batcher_isGlobalPoolLegacy();
// Per-ring-frame instance capacity for the global pool (slice 1).
uint32_t batcher_getGlobalInstanceCap();
// Current coalesce ring frame slot (diagnostic only; NOT the legacy s_frameSlot).
uint32_t batcher_getCoalesceFrameSlot();

// Called from txmmgr.cpp between GpuStaticPropRegistry::flush() and compute_dispatch().
// Non-legacy mode: advances s_coalesceFrameSlot, waits prior-frame fence, computes
// per-cmd baseInstance prefix-sum, writes to s_baseInstanceByCmdSsbo.
// Legacy mode: returns immediately (s_coalesceFrameSlot mirrors s_frameSlot at :2644).
void batcher_prepareBaseInstanceTable();

extern int g_lightProbeSetupPath;  // [GPUPROPS v1]

// LIGHT-BILLBOARD-GPU-1 (master-FAIL #8): MC2_LIGHT_BILLBOARD_FACING mode.
//   0 = OFF (DEFAULT; killswitch — no instance carries the flag bit ->
//       byte-identical stock. RUNWAY-MARKER-LIGHTS-1 flipped the default here
//       from ON after the user rejected the shipped look.)
//   1 = ON  (opt-in) — beacon/runway fixtures are camera-facing billboards
//       with a CAPPED minimum screen size (see static_prop.vert kLbMaxScale)
//   2 = ON + exaggerated card scale (verify-by-exaggeration only)
// Read once (process-lifetime static). Shared by BOTH static-prop instance
// flag-emission sites: the batcher's submitMultiShape() and the persistent
// recipe path in mclib/bdactor.cpp — they must agree or the population splits.
int gos_lightBillboardFacingMode();

// MaterialGpu-3: look up an entry from the per-mission material table by index.
// Returns true and fills *out if MC2_MATERIAL_GPU=1, geometry is finalized,
// and index < table size. Returns false otherwise (out is not modified).
bool batcher_getMaterialGpuEntry(uint32_t index, RenderCore::MaterialGpu* out);

// V-MATERIAL-STATIC-0 — read-only inventory of the StaticPropOpaque MaterialGpu
// table. Populated at finalizeGeometry() in lockstep with s_materialGpuTable;
// cleared in onMapUnload(). NEVER modified at draw time. The inventory captures
// the source-texture identity (nodeIdx / human-readable name / dimensions) that
// the MaterialGpu row's albedoTex layer was built from — information that is
// otherwise local to the per-group texture-array build loop.
//
// One entry per row in s_materialGpuTable. Indexed 0..count-1. Materials are
// deduplicated by texArrayLayer at sidecar-build time, so duplicate texture
// names are possible only when the same source texture lands in both alpha
// groups (alphaGroup distinguishes them).
//
// Inspector consumer: GuiRuntime/EditorInspector.cpp "Material Inventory" panel.
// Source: gos_static_prop_batcher.cpp s_materialInventory.
struct StaticPropMaterialInventoryEntry {
    uint32_t materialIdx;         // index into s_materialGpuTable (== this entry's position)
    uint32_t albedoTexLayer;      // texture-array layer (same as MaterialGpu::albedoTex for static-props)
    uint32_t alphaGroup;          // 0 = alpha-OFF group, 1 = alpha-ON group
    uint32_t flags;               // mirror of MaterialGpu::flags at build time
    uint32_t nodeIdx;             // mcTextureManager nodeIdx for the source texture; 0xFFFFFFFF if absent
    uint32_t textureWidth;        // pixels, from mcTextureManager metadata; 0 if unavailable
    uint32_t textureHeight;       // pixels; equals width for MC2 (square only)
    uint32_t usageCount;          // number of draw slots (PerDrawEntry rows) that reference this material
    char     textureName[64];     // mcTextureManager nodeName; "(unnamed)" / "(absent)" sentinels otherwise
    bool     placeholder;         // true when nodeIdx==0xFFFFFFFF OR name is null/empty OR dims are zero
    // V-MATERIAL-PBR-1: mirror of MaterialGpu PBR scalars for inspector display.
    // Static-prop default: metallic=0.0 (dielectric), roughness=1.0 (fully rough).
    float    metallicFactor;
    float    roughnessFactor;
};

static_assert(sizeof(StaticPropMaterialInventoryEntry) == 108,
              "StaticPropMaterialInventoryEntry size changed; update "
              "GuiRuntime/EditorInspector.cpp forward-decl in lockstep.");
static_assert(offsetof(StaticPropMaterialInventoryEntry, materialIdx) == 0, "materialIdx offset");
static_assert(offsetof(StaticPropMaterialInventoryEntry, flags) == 12, "flags offset");
static_assert(offsetof(StaticPropMaterialInventoryEntry, textureName) == 32, "textureName offset");
static_assert(offsetof(StaticPropMaterialInventoryEntry, placeholder) == 96, "placeholder offset");
static_assert(offsetof(StaticPropMaterialInventoryEntry, metallicFactor) == 100, "metallicFactor offset");
static_assert(offsetof(StaticPropMaterialInventoryEntry, roughnessFactor) == 104, "roughnessFactor offset");

// Number of inventory entries. Equals s_materialGpuTable size after finalizeGeometry.
// Returns 0 before finalize, after onMapUnload, or when MC2_MATERIAL_GPU is disabled.
uint32_t batcher_getStaticPropMaterialInventoryCount();

// Copy one inventory entry by index. Returns false if idx >= count or out==nullptr.
// Read-only snapshot — never mutates batcher state.
bool batcher_getStaticPropMaterialInventoryEntry(uint32_t idx,
                                                  StaticPropMaterialInventoryEntry* out);

// v2 extraction: per-packet texArrayLayer sidecar accessor.
// Returns true + fills *out if globalPacketIdx is within the sidecar range.
// Returns false + *out=-1 if out of range (sidecar empty or packet index invalid).
// Sidecar is populated at finalizeGeometry() and valid until onMapUnload().
bool batcher_getPacketTexArrayLayer(uint32_t globalPacketIdx, int32_t* out);

// v2.1 extraction: full definition in render_snapshot.h.
// batcher_getDrawSlotEntry requires the caller to include render_snapshot.h for the complete type.
// batcher.cpp includes render_snapshot.h directly so it can write struct members.
struct ExtractedStaticPropPacket;

// v2.1 extraction: draw-slot accessors.
// "Draw slot" = position i in s_sortedPacketOrder; [0, alphaOffCmdCount) = opaque,
// [alphaOffCmdCount, totalCmds) = alpha-test.
// Populated once per map in finalizeGeometry(); 0 before first finalizeGeometry().
uint32_t batcher_getDrawSlotCount();

// Fills *out with per-slot data. Returns false if slot >= getDrawSlotCount() or out==nullptr.
// pipelineId: RenderCore::PipelineId cast to uint32_t; 1=StaticPropOpaque, 2=StaticPropAlphaTest.
// instanceCount: previous-frame value (same prior-frame semantics as hasCullRecord);
//   0 on frame 1 or if the type had no visible instances last frame.
// materialIdx: 0xFFFFFFFFu if MC2_MATERIAL_GPU sidecar was not valid at finalizeGeometry().
bool batcher_getDrawSlotEntry(uint32_t slot, ExtractedStaticPropPacket* out);

// v2.2 extraction: dispatch-fact compare (extraction-time facts only; baseInstance deferred).
// Compares snap->staticPropPackets[] against live batcher state (sortedPacketOrder,
// pipelineId via RenderCore::PipelineId, materialIdx sidecar, texArrayLayer vs albedoTex).
// Fills snap->spCompare* fields. Call after v2.1 snapshot population, before ok computation.
// snap->ok from the current extraction must NOT yet be finalized — this fills its inputs.
// Handles null snap->staticPropPackets.data gracefully (still fills spCompareLiveCount).
// RenderSnapshot full definition is in render_snapshot.h; batcher.cpp includes it directly.
struct RenderSnapshot;
void batcher_compareSnapshotPackets(RenderSnapshot* snap);

// v3 snapshot build: read counters written by the most recent flush().
// All output pointers may be nullptr (individual fields skipped).
// attempted:      1 if snapshot build gate check ran this flush.
// countMismatch:  snap.count != totalCmds.
// packetMismatch: DrawPacket field divergence count (accumulated per-slot).
// metaMismatch:   DispatchMeta field divergence count (accumulated per-slot).
// fallback:       gate enabled/attempted but snapshot arrays not dispatched this flush.
void batcher_getSnapshotBuildStats(uint32_t* attempted, uint32_t* countMismatch,
                                   uint32_t* packetMismatch, uint32_t* metaMismatch,
                                   uint32_t* fallback);

// DEBUG-STATE-DUMP-1: read-only StaticPropOpaque visual/debug state.
struct StaticPropOpaqueDebugState {
    bool snapshotDispatchDefault = true;
    bool legacyDispatch = false;
    bool materialGpuEnabled = false;
    bool materialGpuSample = false;
    bool iblShEnabled = false;
    float iblShStrength = 0.0f;
    const char* iblShSet = "default";
    bool pbrEnabled = false;
    float pbrStrength = 0.0f;
    bool pbrRoughnessOverrideEnabled = false;
    float pbrRoughnessOverride = -1.0f;
    int debugMaterialMode = 0;
    // Draw dispatch counts (last completed frame)
    uint32_t spV6DrawCalls = 0;        // individual draw calls issued in v6 path
    uint32_t spAlphaOffPackets = 0;    // alpha-off packet count (multi-draw batch size)
    // Material table sizes
    uint32_t materialGpuTableSize = 0; // deduplicated GPU material entries
    uint32_t materialInventorySize = 0;// per-packet material inventory entries
};

void batcher_getStaticPropOpaqueDebugState(StaticPropOpaqueDebugState* out);

// Number of entries in the type desc table (equals batcher_getTypeCount() post-finalize).
// Returns 0 if geometry not yet finalized.
uint32_t batcher_getStaticPropTypeDescCount();

// Copy one descriptor by typeId. typeId must be in [0, batcher_getStaticPropTypeDescCount()).
// Returns false if typeId is out of range or the table is empty.
// Returns the desc as-is regardless of packetCount (caller validates).
// out must not be null.
bool batcher_getStaticPropTypeDesc(uint32_t typeId, RenderCore::StaticPropTypeDesc* out);

// Direct read-only pointer to the entire table. outCount receives the entry count.
// Pointer is valid until the next onMapUnload(). outCount must not be null.
// Returns nullptr if geometry not finalized or table is empty.
const RenderCore::StaticPropTypeDesc* batcher_getStaticPropTypeDescTable(uint32_t* outCount);

void batcher_buildCandidateLog();

// DEBUG-VIEW-REGISTRY-1: runtime getter/setter for the static-prop fragment debug mode.
// shaderMode maps directly to s_staticPropDebugMaterialMode (shader branch 0..6).
// 0=Final (off), 1=Albedo, 2=MaterialIdx, 3=Normal, 4=TexArrayLayer, 5=Roughness, 6=Metallic.
// Setter clamps input to 0..6. The initial value is 0 (Final).
// ImGui can override at runtime via these functions without restarting the process.
// Canonical label/description strings are in RenderCore/RenderDebugView.h.
void batcher_setDebugMaterialMode(int shaderMode);
int  batcher_getDebugMaterialMode();

// Convert between the canonical RenderDebugView enum and the static-prop shader branch
// integer (0..6). Single authoritative table — avoids duplicate switch statements.
int             StaticPropViewToShaderMode(RenderDebugView view);
RenderDebugView StaticPropShaderModeToView(int shaderMode);
