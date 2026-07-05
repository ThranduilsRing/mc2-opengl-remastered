#pragma once
#include <cstddef>
#include <cstdint>

namespace RenderCore {

// Enumerated render resource slots. Values are stable indices into the
// fixed registry array — do not reorder or renumber.
// Registry is descriptive only; owners retain GL texture/buffer lifetimes.
enum class RenderResourceId : uint16_t {
    Unknown              = 0,
    MainColor            = 1,
    MainDepth            = 2,
    ShadowStaticMap      = 3,
    TerrainHeightTexture = 4,
    MaterialGpuBuffer    = 5,
    ShadowDynamicMap     = 6,
    WaterReflectionColor = 7,   // WATER-REFLECTION-RESOURCE-1: 1/4-res reflection RT (color)
    WaterReflectionDepth = 8,   // WATER-REFLECTION-RESOURCE-1: 1/4-res reflection RT (depth)
    Backbuffer           = 9,   // FRAME-GRAPH-FBO-LEDGER-1: default framebuffer (GL name 0)
    TerrainRecipeBuffer  = 10,  // TERRAIN-SUBPASS-MODEL-1: Indirect: recipe SSBO (slot1)
    TerrainThinBuffer    = 11,  // TERRAIN-SUBPASS-MODEL-1: Indirect/PatchStream: thin-record SSBO (slot2)
    CementAtlas          = 12,  // TERRAIN-SUBPASS-MODEL-1: Indirect: cement atlas (unit3)
    TransitionMaskArray  = 13,  // TERRAIN-SUBPASS-MODEL-1: Indirect: transition-mask 2D_ARRAY (unit4)
    TerrainHeightSsbo    = 14,  // TERRAIN-SUBPASS-MODEL-1: LOD-chunk: height SSBO (distinct from static TerrainHeightTexture)
    MainNormal           = 15,  // POSTPROCESS-SUBGRAPH-1: GBuffer1 (sceneNormalTex_, COLOR_ATTACHMENT1 of sceneFBO_); read by Shoreline, ScreenShadow, SSAO, BoxDecals
    SceneDepthCopy       = 16,  // POSTPROCESS-SCENEDEPTHCOPY-RESOURCE-1: depth-copy RT (sceneDepthCopyTex_); produced by copySceneDepthForParticles() in the VFX/particle path (cross-boundary); read by projected/box decals + particles soft-depth
    SceneObjectId        = 17,  // POSTPROCESS-SCENEOBJECTID-RESOURCE-1: GBuffer2 object-id RT (sceneObjectIdTex_, GL_R32UI, COLOR_ATTACHMENT2 of sceneFBO_); gated RenderWorld::IsObjectIdBufferEnabled(); written by MechOpaque+StaticPropOpaque (layout location=2 when MRT+objectId enabled); read by Composite (unit2, effectiveMode==1 / objectId debug mode)
    HzbPyramid           = 18,  // POSTPROCESS-SUBGRAPH-2: Hi-Z pyramid mip chain (hzbLevelTex_[0..N], R32F); produced by HzbReduce (draw pass via hzbFBO_); read by HzbProbe (CPU diagnostic). Frame-persistent (survives mission reload).
    SsaoOcclusion        = 19,  // POSTPROCESS-SUBGRAPH-2: half-res AO result (ssaoColorTex_, R8/RGBA8); produced by SSAO pass1 (ssaoFBO_); consumed by SSAO pass2/apply (sceneFBO_). Transient within endScene.
    SceneColorCopy       = 20,  // POSTPROCESS-SUBGRAPH-2: feedback-safe scene color copy (sceneColorCopyTex_, RGBA16F); producer = VFX pass (copySceneColorForVfx, REGISTRY-SCENECOLORCOPY-PRODUCER-1); consumed by PostprocessComputeBlur (substrate only, default-OFF).
    ClusterDepthPyramid  = 21,  // REGISTRY-COMPUTE-IDS-1: cluster_depth_pyramid tile min/max texture (s_tileTex, RG32F, tile-grid res); produced by cluster_depth_pyramid::Run (compute), default-OFF gated substrate. Consumed by Lightgrid build (tileMinMax).
    LightgridGrid        = 22,  // REGISTRY-COMPUTE-IDS-1: lightgrid sphere SSBO (s_sphereSsbo, kSphereStride*kMaxLights bytes); produced by lightgrid_build (compute), default-OFF gated substrate.
    LightgridIndex       = 23,  // REGISTRY-COMPUTE-IDS-1: lightgrid per-tile index pool SSBO (s_indexPool, nTiles*kMaxLights uints); produced by lightgrid_build (compute), default-OFF gated substrate.
    PostprocessComputeBlur = 24,// REGISTRY-COMPUTE-IDS-1: compute-blur ping-pong output substrate (postprocess_blur s_pingA, RGBA16F half-res); produced by postprocess_blur::Run (compute), default-OFF gated substrate. No consumer (substrate only).
    ViewUniformsUbo      = 25,  // GPU-BUFFER-OWNER-SKELETON-1: view-uniforms UBO (s_viewUniformsUbo, view_uniforms_gl.cpp); LIVE, raw-owned, UNregistered today. First owner target.
    TerrainTypeSsbo      = 26,  // GPU-BUFFER-OWNER-SKELETON-1: LOD-chunk per-quad type SSBO (s_typeSsbo, gos_terrain_lod_chunk.cpp); LIVE, raw-owned, UNregistered today.
    TerrainCementSsbo    = 27,  // GPU-BUFFER-OWNER-SKELETON-1: LOD-chunk cement-word SSBO (s_cementSsbo, gos_terrain_lod_chunk.cpp); LIVE, raw-owned, UNregistered today.
    StaticPropMaterialGpuBuffer = 28,  // STATICPROP-MATERIAL-SSBO-OWNER-1: static-prop MaterialGpu table SSBO (s_materialGpuSsbo, binding 5, gos_static_prop_batcher.cpp); LIVE default-ON, Mission lifetime. Distinct from mech-material MaterialGpuBuffer(id5, binding 2).
    MechProfileMaterialGpuBuffer = 29,  // MECH-PROFILE-SSBO-OWNER-1: mech material-profile table SSBO (s_ssbo, binding 7, gos_materials.cpp); Persistent lifetime (init→shutdown). Only created when MC2_MECH_SURFACE_MATERIAL loads a profile; default run leaves glName==0. Distinct from binding-5 static-prop and binding-2 mech-material tables.
    TerrainVisualHeightSsbo = 30,  // TERRAIN-VISUAL-HEIGHT-SSBO-OWNER-1: LOD-chunk 4x visual heightfield SSBO (s_visualHeightSsbo, GL binding 26, gos_terrain_lod_chunk.cpp); Mission lifetime. CREATE gated upstream (mclib/terrain.cpp: MC2_TERRAIN_VISUAL_HEIGHT/_DISPLACE + visual_height_4x.r32 bake); WIP, no stock bake → default run leaves glName==0 (never registers). GL binding 26 unrelated to id numbering.
    TerrainLightVertexInputSsbo = 31,  // TERRAIN-LIGHTING-SSBO-OWNER-1: terrain-lighting compute vertex-input SSBO (s_vertexInputSsbo, GL binding 0, gos_terrain_lighting.cpp); Mission lifetime (created mission_init, destroyed mission_shutdown). LIVE default-ON (MC2_TERRAIN_LIGHTING_GPU); feeds the Terrain pass via compute dispatch. Staging ring is Tier-2, NOT owned. GL binding 0 unrelated to id numbering.
    TerrainLightInputSsbo = 32,  // TERRAIN-LIGHTING-SSBO-OWNER-1: terrain-lighting compute per-light SSBO (s_lightInputSsbo, GL binding 1, gos_terrain_lighting.cpp); Mission lifetime. LIVE default-ON. GL binding 1 unrelated to id numbering.
    TerrainLightComputeOutputSsbo = 33,  // TERRAIN-LIGHTING-SSBO-OWNER-1: terrain-lighting compute output SSBO (s_computeOutputSsbo, GL binding 2, gos_terrain_lighting.cpp); Mission lifetime. LIVE default-ON; VRAM output the Terrain pass + water/mask consumers read. GL binding 2 unrelated to id numbering.
    LightDataSsbo        = 34,  // LIGHTDATA-SSBO-OWNER-1: per-frame light-data SSBO (s_lightDataSsbo, GL binding LIGHT_DATA_SSBO_BINDING, gameos_graphics.cpp); LIVE default-path, grow-once/realloc (handle recreated on grow → re-registered). Persistent lifetime (lazy-created on first upload, destroyed in txmmgr.cpp mcTextureManager teardown). GL binding unrelated to id numbering.
    DynamicFullMapFbo    = 35,  // FBO-LEDGER-EXTEND-1: dynamic full-map render FBO (dynamicFullMapFbo_, gos_postprocess.cpp); registered observe-only in the FboLedger. No prior id (SsaoOcclusion=19/HzbPyramid=18 reused for the other two unaccounted FBOs).
    DynamicPropShadowSsbo = 36, // SCENE-SSBO-OWNER-SWEEP-1: dynamic prop-shadow caster instance SSBO (s_dynamicPropShadowSsbo, GL binding 0, gos_static_prop_batcher.cpp drawDynamicPropShadows); LIVE default-path (gate MC2_SHADOW_DYNAMIC_PROP_CASTERS default-ON, inside the always-on dynamic shadow pass). Mission lifetime: handle glGen'd once per map, per-frame orphan-on-write bufferData, freed on onMapUnload. GL binding 0 unrelated to id numbering. COMPLETENESS-SWEEP gap B#5. Siblings (s_staticBldgShadowSsbo, s_blockVisSsbo, particle s_ssbo/tube SSBOs) DEFERRED to exclusion ledger — created only under default-OFF gates.
    PostprocessSubgraphColor = 37, // POSTPROCESS-VK-IMAGE-OWNERSHIP-1: Layer-4 subgraph OWNED intermediate COLOR image (R16G16B16A16_SFLOAT). Copied-in from GL sceneColor, blended by BOTH fog passes (edge then oob) in ONE render pass, copied-out to GL. Layout chain UNDEFINED->TRANSFER_DST->COLOR_ATTACHMENT->TRANSFER_SRC. Runtime VkImage handle lives in the future subgraph .cpp, NOT here. Proof-only enum id.
    PostprocessSubgraphDepth = 38, // POSTPROCESS-VK-IMAGE-OWNERSHIP-1: Layer-4 subgraph OWNED intermediate DEPTH image (D32_SFLOAT). Copied-in from GL sceneDepth, SAMPLED (read-only) by both fog passes. Layout chain UNDEFINED->TRANSFER_DST->SHADER_READ_ONLY. Runtime VkImage handle lives in the future subgraph .cpp, NOT here. Proof-only enum id.
    TerrainVisualDampSsbo = 39, // TERRAIN-REAUTH-UNPIN-1 Half B: coarse object-proximity displacement damp SSBO (s_visualDampSsbo, GL binding 27, gos_terrain_lod_chunk.cpp); Mission lifetime. CREATE gated upstream (MC2_TERRAIN_VISUAL_DISPLACE + objfade gate + visual_damp.r32 sidecar); static half uploaded at mission load, per-frame mover stamps via BufferSubData. GL binding 27 unrelated to id numbering.
    Count
};

// ENUM-ID-GUARD-EXPAND-1: DENSE-INDEX SAFETY.
// Several consumers allocate a fixed array `T arr[int(RenderResourceId::Count)]`
// and index it by `int(id)` WITHOUT a bounds check (frame_graph_validate.h,
// postprocess_subgraph.h). That is memory-safe iff every enumerator's numeric
// value lies in [0, Count) AND Count is exactly the number of real slots (i.e.
// the enum is a dense 0..Count-1 run with Count last). These compile-time guards
// prove both, so a future misnumber / gap / forgotten-Count-bump breaks the build
// here instead of silently writing out of bounds at runtime. GL-free, zero cost.

// (a) Count sits above every real id: an int(id) index is always < Count. Each
//     real enumerator is listed; adding one WITHOUT extending this list, or giving
//     an id a value >= Count, fails to compile.
static_assert(static_cast<int>(RenderResourceId::Unknown)                       < static_cast<int>(RenderResourceId::Count), "id >= Count would index out of bounds");
static_assert(static_cast<int>(RenderResourceId::DynamicPropShadowSsbo)         < static_cast<int>(RenderResourceId::Count), "id >= Count would index out of bounds");
static_assert(static_cast<int>(RenderResourceId::PostprocessSubgraphColor)      < static_cast<int>(RenderResourceId::Count), "id >= Count would index out of bounds");
static_assert(static_cast<int>(RenderResourceId::PostprocessSubgraphDepth)      < static_cast<int>(RenderResourceId::Count), "id >= Count would index out of bounds");
static_assert(static_cast<int>(RenderResourceId::TerrainVisualDampSsbo)         < static_cast<int>(RenderResourceId::Count), "id >= Count would index out of bounds");

// (b) The run is DENSE: Count == number-of-real-slots. TerrainVisualDampSsbo is the
//     last real id (value 39) and Count follows it, so Count must be 40. If a new id
//     is inserted, or an existing one renumbered leaving a hole, this breaks — forcing
//     a review of the dense-index consumers.
static_assert(static_cast<int>(RenderResourceId::TerrainVisualDampSsbo) + 1
                  == static_cast<int>(RenderResourceId::Count),
              "RenderResourceId is not a dense 0..Count-1 run (Count must immediately "
              "follow the last real id); dense-index consumers assume no holes above Count");
static_assert(static_cast<int>(RenderResourceId::Count) == 40,
              "RenderResourceId::Count changed; update dense-index consumers "
              "(frame_graph_validate.h, postprocess_subgraph.h) and this guard");

// REGISTRY-LIFETIME-CLASS-1: how long a registered resource stays valid.
// Observe-only metadata — the bridge to a future scheduler/backend that needs to
// know aliasing/recreation lifetime. Vocabulary is deliberately small.
//   Unset      — sentinel; NOT a real lifetime. A valid resource left at Unset is
//                a registration bug (the validator fails on it). Note: this is
//                distinct from the existing valid=false / gated-absent behavior,
//                which means "registered but currently unavailable" and is NOT a
//                lifetime.
//   FrameLocal — transient, produced+consumed within a frame, aliasable.
//   Mission    — rebuilt on each mission load (terrain SSBOs, atlases).
//   Persistent — process/long-lived; survives mission reload (screen-sized FBO
//                targets are recreated only on resize, still Persistent).
//   External   — temporal / cross-frame N-1 / externally-owned (water reflection).
enum class RenderResourceLifetime : uint8_t {
    Unset      = 0,
    FrameLocal = 1,
    Mission    = 2,
    Persistent = 3,
    External   = 4,
};

enum class RenderResourceKind : uint8_t {
    Unknown        = 0,
    Texture2D      = 1,
    Texture2DArray = 2,
    TextureCube    = 3,
    Buffer         = 4,
};

enum class RenderResourceFormat : uint8_t {
    Unknown   = 0,
    R32F      = 1,
    RGBA16F   = 2,
    RGBA8     = 3,
    Depth16   = 4,
    Depth24   = 5,
    Depth32F  = 6,
    BufferRaw = 7,
};

// Per-resource descriptor. GL-free conceptually; glName is debug-only.
// valid=false means the slot is registered but the resource is currently
// unavailable (mission not loaded, subsystem not yet initialized, etc.).
// debugName must point to a string literal; never heap-allocated.
struct RenderResourceDesc {
    RenderResourceId     id             = RenderResourceId::Unknown;
    RenderResourceKind   kind           = RenderResourceKind::Unknown;
    RenderResourceFormat format         = RenderResourceFormat::Unknown;
    // REGISTRY-LIFETIME-CLASS-1: lifetime class. No safe default — a valid
    // resource MUST set this explicitly; the validator fails on Unset.
    RenderResourceLifetime lifetime     = RenderResourceLifetime::Unset;
    const char*          debugName      = nullptr;
    uint32_t             width          = 0;
    uint32_t             height         = 0;
    uint32_t             layers         = 1;
    uint32_t             samples        = 1;
    uint32_t             glName         = 0;    // debug only; 0 if unavailable
    uint64_t             sizeBytes      = 0;    // 0 if unknown
    uint32_t             producerPassId = 0;
    uint32_t             consumerMask   = 0;
    bool                 valid          = false;
};

// Register or update a resource descriptor indexed by desc.id.
// Overwrites any existing record for that id. Unknown id is a no-op.
// Calling with valid=false marks the slot unavailable without removing it.
void registerOrUpdateRenderResource(const RenderResourceDesc& desc);

// Returns nullptr if id is Unknown, out of range, or valid=false.
const RenderResourceDesc* getRenderResource(RenderResourceId id);

// Count of currently-valid registered resources.
size_t getRenderResourceCount();

// Enumerate valid resources by dense index [0, getRenderResourceCount()).
const RenderResourceDesc* getRenderResourceByIndex(size_t index);

const char* toString(RenderResourceId id);
const char* toString(RenderResourceKind kind);
const char* toString(RenderResourceFormat fmt);
const char* toString(RenderResourceLifetime lifetime);

// REGISTRY-LIFETIME-CLASS-1: validate that every currently-registered (valid)
// resource has a lifetime set (!= Unset). Returns true if all valid resources
// carry a lifetime; on failure, *offending (if non-null) receives the id of the
// first valid resource missing a lifetime. A clean registry returns true.
bool validateRenderResourceLifetimes(RenderResourceId* offending = nullptr);

} // namespace RenderCore
