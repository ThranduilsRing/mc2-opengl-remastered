// GameOS/gameos/gos_mech_batcher.h
#pragma once

#include <cstdint>
#include <cstddef>    // offsetof
#include <vector>
#include <unordered_map>
#include "Stuff/Stuff.hpp"
#include "tgl.h"
#include "msl.h"
#include "mech3d.h"

// CPU-side vertex layout — packed 48 bytes for VBO storage.
// CHANGING THIS STRUCT REQUIRES CHANGING mech.vert ATTRIBUTE LAYOUT IN LOCKSTEP.
struct GpuMechVertex {
    float    position[3];    // 12B — a_position  (loc 0, GL_FLOAT)
    float    normal[3];      // 12B — a_normal     (loc 1, GL_FLOAT)
    float    uv[2];          //  8B — a_uv         (loc 2, GL_FLOAT)
    uint8_t  boneIndices[4]; //  4B — a_boneIndices (loc 3, GL_UNSIGNED_BYTE, IPointer -> uvec4)
    uint8_t  boneWeights[4]; //  4B — a_boneWeights (loc 4, GL_UNSIGNED_BYTE, GL_TRUE -> vec4 [0,1])
    int16_t  tangentOct[2];  //  4B — a_tangentOct  (loc 5, GL_SHORT, GL_TRUE -> vec2 [-1,1])
    uint32_t aRGBLight;      //  4B — a_aRGBLight   (loc 6, GL_UNSIGNED_INT, IPointer -> uint)
};                           // 48B total
static_assert(sizeof(GpuMechVertex) == 48, "GpuMechVertex must be 48 bytes");
static_assert(offsetof(GpuMechVertex, position)    ==  0, "position offset");
static_assert(offsetof(GpuMechVertex, normal)      == 12, "normal offset");
static_assert(offsetof(GpuMechVertex, uv)          == 24, "uv offset");
static_assert(offsetof(GpuMechVertex, boneIndices) == 32, "boneIndices offset");
static_assert(offsetof(GpuMechVertex, boneWeights) == 36, "boneWeights offset");
static_assert(offsetof(GpuMechVertex, tangentOct)  == 40, "tangentOct offset");
static_assert(offsetof(GpuMechVertex, aRGBLight)   == 44, "aRGBLight offset");

// Per-instance GPU record (64 B, std430). SINGLE SOURCE OF TRUTH is
// RenderCore/gpu_abi/GpuMechInstance.h; its generated asserts and the shaders
// (via the generated .hglsl) are derived from it.
#include "../../RenderCore/gpu_abi/GpuMechInstance.h"

// Bone matrix: 4 explicit rows to avoid GLSL column-major confusion.
// Upload rows as row0..row3; GLSL mat4(row0,row1,row2,row3) fills COLUMNS from
// these, making boneT the transpose of the Stuff LinearMatrix4D —
// boneT * vec4(pos,1) == row-vector math. Named boneT in shader; do NOT remove.
struct GpuMechBone {
    float row0[4], row1[4], row2[4], row3[4];
};
static_assert(sizeof(GpuMechBone) == 64, "GpuMechBone must be 64 bytes");

// Per mechType×LOD registration record (CPU-side only).
struct GpuMechTypeLodRecord {
    uint32_t firstBoneIndex;   // vertex-local bone index namespace base (normally 0)
    uint32_t numBones;         // count of SHAPE_NODE children; must be <= 255
    uint32_t firstPacket;      // into s_packets table
    uint32_t packetCount;
    uint32_t vertexCount;      // total triangle-soup vertices across all nodes
    const TG_TypeShape* sourceNode0; // for late-reg logging
    // BT2018-SKEL-GPU-PALETTE-PLACEMENT-1: non-null for an imported animated mech
    // type (the TG_TypeMultiShape*). submitActor sources this actor's bone palette
    // from shapeToWorld_root · mc2mechanim model-delta instead of node shapeToWorld.
    const void* importedGpuType = nullptr;
};

// Per-node per-texture-group draw record (CPU-side only, not uploaded).
struct GpuMechPacket {
    uint32_t firstIndex;         // index into shared IBO (in indices, not bytes)
    uint32_t indexCount;
    int32_t  baseVertex;         // signed; passed to glDrawElementsInstancedBaseVertex
    uint32_t textureSlot;        // index into owning TG_TypeShape::listOfTextures.
                                 // Slot 0 is per-actor (paint scheme); resolved from
                                 // GpuMechSubmitDesc::slot0TexHandle at submitActor time.
                                 // Slots 1+ are type-stable; resolved from owningTypeShape.
    uint32_t materialFlags;      // bit 0: ALPHA_TEST_BIT
    uint32_t owningTypeLodRecord;
    uint32_t nodeLocalIndex;     // which bone index within the type (index into listOfShapes)
    const TG_TypeShape* owningTypeShape; // type-level shape for slots 1+ texture resolution
};

// All per-actor context needed at submit / flush time.
struct GpuMechSubmitDesc {
    TG_MultiShape*              mechShape;      // live per-instance shape (shapeToWorld only)
    const Mech3DAppearanceType* mechType;       // stable type pointer
    int                         currentLOD;
    uint32_t                    slot0TexHandle; // mcTextureManager SLOT INDEX (NOT a gos handle)
                                               // for texture slot 0 (per-actor paint scheme;
                                               // localTextureHandle from Mech3DAppearance, set
                                               // by mcTextureManager->loadTexture). Resolved to
                                               // a live gos handle at flush time via
                                               // mcTextureManager->get_gosTextureHandle(slot)
                                               // per memory/mc2_texture_handle_is_live.md.
                                               // TG_TypeShape::listOfTextures is a shared type-
                                               // level cache mutated by TransformMultiShape across
                                               // all actors — do NOT read it for slot 0.
    uint32_t                    slot6TexHandle; // AO-1: AO mcTextureManager SLOT INDEX
                                               // (0 = no AO). Resolved to a gos handle at
                                               // flush + bound to unit 6 when HAS_AO.
    uint32_t                    slot7TexHandle; // NORMALS-1: normal-map mcTextureManager
                                               // SLOT INDEX (0 = no normal). Resolved at
                                               // flush + bound to unit 7 when HAS_NORMAL.
    uint32_t                    lightDataIndex; // Slice B1; 0 in Slice A
    uint32_t                    renderFlags;
    uint32_t                    highlightARGB;
    uint32_t                    fogARGB;
    // M2.5: RenderObjectHandle.raw() for this actor's mech handle
    // (M2 storage). 0 = Handle::invalid() = no ObjectID write at this
    // pixel (treated identically to legacy-path fallback). CPU-side
    // carrier is UNCONDITIONAL per Q3; the consumer is GLSL-macro-gated.
    // Source: mech3d.cpp submit site reads
    //   appearance.getRenderWorldHandle().raw()  [mech3d.h:487].
    uint32_t                    objectIdRaw;
    // GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: sanitized per-mech visual state,
    // sourced from Mech3DAppearance::getVisualState() at the mech3d.cpp submit
    // site. heat01 carried for the debug dump / forward-compat (always 0,
    // USEHEAT off); only damage01 + flags reach the GpuMechInstance SSBO.
    float                       heat01   = 0.0f;
    float                       damage01 = 0.0f;
    uint32_t                    visualFlags = 0u;
};

// Fallback accounting reasons (used in MC2_MECH_BATCHER_STATS output).
enum class GpuMechFallbackReason : uint8_t {
    UnregisteredType  = 0,
    U8BoneOverflow    = 1,
    RingOverflow      = 2,
    TglGpuUnsupported = 3,
    ShaderInitFailure = 4,
    // MECH-LIGHTROW-FRESHNESS-1: the actor's cached light-row index failed the
    // cachedFrame_ freshness test (or the range check) at mclib/mech3d.cpp, so
    // the GPU submit was declined and the mech took the CPU MLR draw instead.
    // Distinct from the failures above because nothing is broken -- the GPU path
    // is healthy and simply had no trustworthy light row for this actor this
    // frame. Appending here requires bumping the s_fallbacksThisFrame[] bound
    // and kFallbackNames[] in gos_mech_batcher.cpp (both sized to this enum).
    StaleLightRow     = 5,
};

class GpuMechBatcher {
public:
    static GpuMechBatcher& instance();

    void onMapLoad();
    void onMapUnload();

    // Register one Mech3DAppearanceType × LOD. Idempotent.
    // Reads mechType->mechShape[lod] (TG_TypeMultiShape*, stable type-level pointer).
    void registerTypeLod(const Mech3DAppearanceType* mechType, int lod);

    // Upload immutable VBO/IBO after all registerTypeLod() calls.
    void finalizeGeometry();

    // True once finalizeGeometry() has uploaded geometry; submitActor()
    // fast-rejects every actor while this is false. Used by the
    // [MECHRESTORE v1] probe to prove the savegame-load finalize gap.
    bool isFinalized() const;

    // Rebuild shared geometry to absorb types registered AFTER a prior
    // finalizeGeometry() (VPL-#11: campaign .fit resume spawns the player
    // force-group post-Mission::init-finalize). No-op unless a late type
    // was staged. Caller invokes once the late-spawn batch completes
    // (logistics.cpp SP force-group loop). NO render may occur between
    // the late registrations and this call (adversarial-review invariant).
    void finalizePending();

    // Caller-side accounting (called BEFORE any registration check).
    void recordEligibleActor();
    void recordCpuFallback(GpuMechFallbackReason reason);

    // Per-frame submission. Returns false on any failure; caller MUST
    // CPU-fallback (mechShape->Render(true)) this frame when false.
    [[nodiscard]] bool submitActor(const GpuMechSubmitDesc& desc);

    // APPLY-STATE-MECHOPAQUE-1: the frame-graph executor's record of the MechOpaque top-level pass (counts the pass in the
    // apply-state ledger; the pipeline itself is the Vulkan mech island's). Called from txmmgr renderLists() when
    // MC2_FRAMEGRAPH_EXECUTOR is ON.
    void executorApplyMechOpaqueState();

    // Per-frame flush: builds the instance, bone and run feeds the Vulkan mech island consumes (CPU only).
    void flush();

    bool wasLastFailureLateRegistration() const;

    static uint64_t getAllowedLateRegEventCount();
    static uint64_t getDisallowedLateRegEventCount();
};

// ---------------------------------------------------------------------------
// MECH-EXTRACTION-0: mech pending-submit snapshot API.
// Gate: MC2_SNAPSHOT_MECH_EXTRACT=1 (default OFF).
// Full type definitions in render_snapshot.h; caller must include it.
// gos_mech_batcher.cpp includes render_snapshot.h directly.
// ---------------------------------------------------------------------------

// VK-PARITY-MECH-1: the ONE producer of the default mech per-fragment shading
// knobs (shaders/include/mech_shading.hglsl). The GL uniform upload in flush()
// and the VK mech island frame feed both read it, so GL and VK cannot disagree.
// Values are the EFFECTIVE ones (gate-off terms already folded to 0).
struct GpuMechShadingParams {
    float ambientStrength;     // MECH-AMBIENT-1 (0 when MC2_MECH_AMBIENT_V1=0)
    float specularStrength;    // MECH-SPECULAR-V1 (0 when gate off / no ViewUniforms)
    float metalRoughness;
    float glassRoughness;
    float glassLumaThresh;
    float glassMaxChanThresh;
    int   specDebugMask;       // ImGui cockpit-mask view
    float importedGamma;       // BT2018-MECH-MATERIAL-GAMMA-1
    float importedAlbedoScale;
    float aoStrength;          // AO-1
    float normalStrength;      // NORMALS-1
};
void gpuMechShadingParams(GpuMechShadingParams* out);

// Forward declarations so this header compiles without render_snapshot.h.
struct ExtractedMechPacket;
struct RenderSnapshot;

// Returns count of pending submits for the current frame (after DoGameLogic, before flush).
uint32_t batcher_getMechPendingCount();

// V1A: per-frame mech submit count latched at flush() entry (before any guard clears
// s_pendingSubmits). GpuMechBatcher submits only — does NOT include MLR fallback draws.
// Returns 0 before the first flush (mission not yet loaded).
uint64_t batcher_getLastFlushSubmitCount();

// Fills *out with s_pendingSubmits[idx] fields. Returns false if idx out of range.
// materialIdx is always 0xFFFFFFFFu in v0 (not available pre-flush).
bool batcher_getMechPendingEntry(uint32_t idx, ExtractedMechPacket* out);

// Compares snap->mechPackets against live s_pendingSubmits.
// Fills snap->mechCountMismatch, mechHandleMismatch, mechObjectIdMismatch,
// mechTexHandleMismatch. mechMaterialIdxMismatch stays 0 (sentinel in v0).
// Called from ExtractRenderSnapshot() under MC2_SNAPSHOT_MECH_EXTRACT gate.
void batcher_compareMechSnapshot(RenderSnapshot* snap);
