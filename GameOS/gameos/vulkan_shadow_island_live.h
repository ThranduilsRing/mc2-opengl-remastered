// vulkan_shadow_island_live.h — VK-P8a SHADOW-INTEROP public API + caster-geometry
// feed contract. The whole feature is #ifdef MC2_VULKAN_ISLAND; these declarations
// are extern "C" so consumers (gos_postprocess.cpp, txmmgr.cpp, gameosmain.cpp,
// debug_state_dump.cpp) pull in NO Vulkan headers.
//
// STEP 0 caster-feed decision (.claude/VK-P8a-STEP0-CASTER-FEED-NOTE.md):
//   - instances = the CPU GpuStaticPropInstance vector (dynShadowSet, 112B each) the
//     GL path already builds each frame — memcpy'd into a VK SSBO.
//   - geometry  = the shared static-prop VBO/IBO — copied ONCE per map into VK-owned
//     buffers via the feed accessor below (P8a fallback (B): VK owns a copy).

#ifndef MC2_VULKAN_SHADOW_ISLAND_LIVE_H
#define MC2_VULKAN_SHADOW_ISLAND_LIVE_H

#ifdef MC2_VULKAN_ISLAND

#include <cstdint>

namespace mc2tex { struct TextureArrayCpu; }

#ifdef __cplusplus
extern "C" {
#endif

// --- Caster geometry feed (provided by gos_static_prop_batcher.cpp) ---------
// A per-type packet range into the shared VBO/IBO (subset of GpuStaticPropPacket).
struct Mc2ShadowPacket {
    uint32_t firstIndex;   // into shared IBO
    uint32_t indexCount;
    uint32_t baseVertex;   // into shared VBO
};

// One-shot geometry feed for the shadow and colour islands: the shared VBO/IBO byte sizes plus the packet / type ranges.
// The bytes themselves come from mc2_static_prop_geometry_cpu (backend-neutral; no GL readback); the sizes here let a
// reader reject a CPU copy that does not match. The packet / type arrays are batcher-owned and valid until onMapUnload().
struct Mc2ShadowGeomFeed {
    uint64_t       vboBytes;
    uint64_t       iboBytes;   // uint32 indices
    uint32_t       vertexStride;    // kVertexStride (position at offset 0, vec3 float)
    const Mc2ShadowPacket* packets; // per-packet ranges (length packetCount)
    uint32_t       packetCount;
    const uint32_t* typeFirstPacket; // [typeCount] firstPacket per typeID
    const uint32_t* typePacketCount; // [typeCount] packetCount per typeID
    uint32_t       typeCount;
};

// Fills *out from the finalized batcher geometry. Returns false if geometry is not
// yet finalized (island then declines the frame; GL path runs). Implemented in
// gos_static_prop_batcher.cpp. Buffers persist until onMapUnload().
int mc2_shadow_island_get_geometry_feed(Mc2ShadowGeomFeed* out);

// VK-P11 STATIC-PROP COLOR — per-frame COLOR instance feed. The color island reuses
// the SAME shared VBO/IBO + packet/type tables as the shadow feed above (identical
// geometry), but its per-frame instance set is the VISIBLE color-pass instances the
// GL GpuStaticPropBatcher::flush() draws (s_bucketsByType), NOT the shadow-caster
// set. This accessor flattens the live per-type buckets into a single
// GpuStaticPropInstance-layout (112B) array grouped by typeID and hands out the
// engine's world->clip matrix (gos_GetObjectFixBMVPMat4(), GL_FALSE) + the reverse-Z
// object depth bias. Called by the color island ONLY when the gate is ON (i.e. when
// the GL flush() was skipped, so the buckets are still populated for this frame).
// Returns false if geometry is not finalized or there are no visible instances.
//
// The instance blob pointer is owned by the batcher (a reused static scratch
// vector) and is valid until the next call. `instances112` points at count
// GpuStaticPropInstance records (112B each, same std430 POD).
struct Mc2ColorPropFeed {
    const void* instances112;   // count * GpuStaticPropInstance (112B), grouped by typeID
    uint32_t    count;
    float       worldToClipGL[16]; // gos_GetObjectFixBMVPMat4() — GL_FALSE, 16 linear floats
    float       objectDepthBias;   // reverse-Z surface bias (== OBJECT_DEPTH_BIAS)
};
int mc2_color_prop_island_get_frame_feed(Mc2ColorPropFeed* out);
// Batcher's real packed vertex stride (kVertexStride); valid before geometry upload.
uint32_t mc2_static_prop_vertex_stride(void);

// VK-P11 STATIC-PROP MATERIAL (MC2_VK_PROP_MATERIAL) — copy-once material feed.
// Hands the color island the change key of the alpha-OFF albedo array
// (the CPU group array — buildings live here; the parked Tree/Generic alpha-ON group is
// a SEPARATE coverage gap) plus a per-type group-relative albedo LAYER table and a
// per-type alpha-class flag. The island copy-once imports the array (glGetTexImage /
// glGetCompressedTexImage -> vkCmdCopyBufferToImage, layerCount=N, full mips) into a
// VK sampled array image and samples it in the Tier-1 FS at float(typeAlbedoLayer[
// typeID]). Also carries the terrain sun (MC2 world, toward-sun) for the shared
// computePBR directional term.
//
// `bucketed`  = 1 when MC2_STATICPROP_BC7 split the group into per-bucket sibling
//               arrays (bucket-relative layers): the single-array import is invalid
//               then, so the island fails soft to the flat teal (materials DEFERRED
//               for BC7). `arrayKeyOff` == 0 or !finalized -> return 0 (fail soft).
// `typeAlbedoLayer[t]` = group-relative albedo layer for type t in the alpha-OFF array, or
//               -1 if type t has no alpha-OFF texture (island keeps teal for that type).
// `typeAlphaClass[t]`  = 0 alpha-OFF, 1 alpha-ON. The island only textures alpha-OFF
//               types (class 0) from the alpha-OFF array; class 1 types keep teal.
// Buffers are batcher-owned and valid until onMapUnload(). Returns 0 if not finalized
// or the alpha-OFF array is unavailable.
struct Mc2ColorPropMaterialFeed {
    uint32_t        arrayKeyOff;     // change key of the alpha-OFF group array (0 = not built; new serial per build)
    uint32_t        bucketed;        // 1 = BC7/bucketed (island fails soft, keeps teal)
    const int32_t*  typeAlbedoLayer; // [typeCount] group-relative layer (-1 = none)
    const int32_t*  typeAlphaClass;  // [typeCount] 0=alpha-OFF, 1=alpha-ON
    uint32_t        typeCount;
    float           terrainSunMC2[3];// gos_GetTerrainLightDir (surface->sun, MC2 world)
    // VK-P11 STATIC-PROP MATERIAL alpha-ON: the key of the alpha-ON group array.
    // typeAlbedoLayer[t] is ALREADY group-relative for class-1 types (layer into
    // the alpha-ON array), so no second layer table is needed on the feed side. 0 = alpha-ON
    // array not built (island imports/routes class-1 types only when non-zero).
    uint32_t        arrayKeyOn;      // change key of the alpha-ON group array (0 = not built)
    // The CPU copies of the two group arrays (every level, the bytes GL uploaded); null when a group has none. The island imports
    // these; the keys above tell it the arrays were rebuilt.
    const mc2tex::TextureArrayCpu* albedoOff;
    const mc2tex::TextureArrayCpu* albedoOn;

    // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: per-PACKET (not per-type) albedo layer.
    // A single typeID can own multiple packets (sub-mesh ranges) that each target a
    // DIFFERENT texture within the same group array (e.g. a fence type's "post" and
    // "arch" sub-meshes) — GL's coalesce path samples per-DRAW (PerDrawEntry.
    // texArrayLayer), never per-type. The island's prior per-TYPE-only layer
    // (typeAlbedoLayer, "first-valid-wins") forced every packet of a multi-packet
    // type to reuse packet 0's layer, so any non-first packet sampled the WRONG
    // texture layer/UV combination — for BC7 this reliably lands on a black margin
    // block (opaque alpha, so no alpha-test discard masked it), which is the
    // observed "every-other-post is black" symptom. This array is indexed by the
    // GLOBAL packet index (same indexing as Mc2ShadowPacket / typeFirstPacket +
    // typePacketCount), length packetCount, group-relative (same array as
    // typeAlbedoLayer). -1 = no texture for this packet (island keeps teal for it).
    const int32_t*  packetAlbedoLayer;
    uint32_t        packetCount;

    // FENCE-UVSCALE-1: per-PACKET UV sub-region scale (same indexing as
    // packetAlbedoLayer / packetCount above). GL's coalesce path crops each
    // draw's sampled UV to (0..uvScaleX, 0..uvScaleY) inside the group's
    // max-dims texture-array layer (static_prop.frag:206
    // `fract(v_uv) * vec2(uvScaleX, uvScaleY)`); the island previously assumed
    // 1.0 (full layer), which reads panels narrower/gapped vs GL wherever a
    // packet's unique texture is smaller than the group's max dims (the fence
    // chain-link post/arch sub-meshes). 1.0 = no crop (fills the layer).
    const float*    packetUvScaleX;
    const float*    packetUvScaleY;

    // Per-type emission colours of the three hot-colour night tags (hot pink = lit windows, hot yellow = outside building lights,
    // hot green = building base lights / runway fixtures), 12 floats per type (three RGBA triples in that order, alpha 0), indexed
    // by type: the same table the GL prop shader reads. Without it the island cannot light a window or a fixture at night.
    const float*    typeHotColors;   // [typeCount * 12], or null when the type table is not ready
};
int mc2_color_prop_island_get_material_feed(Mc2ColorPropMaterialFeed* out);

// VK-PROP-AMBIENT-IBL-1: real ObjectLights ambient + IBL-SH feed for the Tier-1
// prop island (deferred item from VK-FLIP-FIELD-REPORT-2 washout fix). Mirrors the
// VK-MECH-MATERIAL-LIGHTING-1 pattern: LightsData GL SSBO name/size (binding 20,
// copy-per-frame into the island's own SSBO) + the static IBL-SH coefficient table
// (header-only constexpr, no GL round-trip needed) + the live iblShStrength slider
// value (0.0 when StaticPropIblSh is off -> shader short-circuits, byte-identical
// contribution). ambient_v1 hemisphere tints are NOT part of this feed (still a GL
// constant the island approximates locally) -- only the ObjectLights AMBIENT-type
// sum and the SH-L2 fill are real data now.
struct Mc2ColorPropLightFeed {
    // VK-PROP-LIGHTFEED-NATIVE-1 (mech item-9 pattern): the CPU LightsData array
    // (mcTextureManager lightData_, the byte source of the GL SSBO upload at
    // renderLists) is the per-frame lighting feed. VK-PERF-2 cleanup deleted the
    // GL SSBO name/size fields with the audit-gated readback they existed for
    // (MC2_VK_PROP_FEED_AUDIT banked a clean run: 3000 frames, mismatches=0).
    const void*  lightDataCpu;      // TG_HWLightsData[count] (3600B each) or null
    uint64_t     lightDataCpuBytes; // count * sizeof(GpuObjectLights)
    float        iblSh[9][3];       // RenderCore::kIblShCoeffs (or current mission's set)
    float        iblShStrength;     // g_iblShStrength if s_iblShEnabled else 0.0
    // The eye's night state: the hot-colour tags (windows, base lights, fixtures) are lit by it.
    int32_t      isNight;           // 0/1
    float        nightFactor;       // 0..1 day->night blend
    float        ambientV1Strength; // StaticPropAmbient option: 1.0 when the hemisphere ambient fill is on, else 0.0
    float        pbrV1Strength;     // StaticPropPbr option: the specular strength when it is on, else 0.0
    float        pbrV1RoughnessOverride; // the GL roughness override the specular uses (the tuning slider), or -1.0 when it is off
    float        cameraWorldPos[4]; // GL world eye position for the specular view vector
    // Night light pools (the GL prop pool term's inputs): the CPU pool array and its knobs.
    const void*  poolLights48;      // GpuTerrainLight[numPoolLights] (48B each) or null
    int32_t      numPoolLights;     // 0 = no pool term this frame
    int32_t      poolSoft;
    float        poolMoverDim;
    float        poolMoverScale;
    int32_t      poolMoverMax;
    int32_t      poolCompress;
    float        poolObjScale;
};
int mc2_color_prop_island_get_light_feed(Mc2ColorPropLightFeed* out);

// --- Island public API (implemented in vulkan_shadow_island_live.cpp) --------

// True when MC2_VK_SHADOW_INTEROP requests the VK shadow region.
int mc2_vk_shadow_island_enabled();

// Per-frame entry. Returns the GL texture handle of the VK-produced shared shadow
// map (>0) if the VK pass rendered+signalled this frame; 0 = fell back (GL path).
//   size          — square shadow map side (getDynamicShadowMapSize()).
//   lightSpace16  — the 16 raw floats GL uploads (GL_FALSE) for lightSpaceMatrix.
//   instances     — the CPU GpuStaticPropInstance vector (112B each).
//   instanceCount — caster instance count.
//   biasFactor/biasUnits — glPolygonOffset(shadowBiasFactor_, shadowBiasUnits_).
unsigned int mc2_vk_shadow_island_run(
    int size, const float* lightSpace16,
    const void* instances, unsigned int instanceCount,
    float biasFactor, float biasUnits);

// Ends the frame's shadow recording and submits it, once, with no wait on the GPU. mc2_vk_shadow_island_run / _run_csm_array_layer / _run_csm_fullmap only
// RECORD their passes into the frame's command buffer; this is called after the last of them and before any consumer reads a product (the accessors below
// report a product ready only after it). A frame whose passes all declined submits nothing.
void mc2_vk_shadow_island_submit_frame();

// VK-SHADOW-DIRECT-CONSUME-1: raw VK-side accessors for a same-process, same-device
// VK consumer (the terrain island) to bind the shadow island's depth image directly —
// no GL round-trip at all (neither the GL copy-to-VK-image the terrain island does
// today, nor a GL sample). Valid ONLY for the frame just produced by
// mc2_vk_shadow_island_run() (mc2_vk_shadow_island_dyn_shadow_ready()!=0 iff that
// call recorded this frame and the frame was submitted; consumers run later on the same queue and the render pass dependencies order their reads). The returned view/sampler are owned by
// the shadow island; the caller must NOT destroy them. Opaque void* to keep this
// header GL/Vulkan-type-free for non-VK TUs; real type is VkImageView / VkSampler,
// reinterpret_cast in the (VK-only) consumer TU. Same VkDevice as the terrain island
// (both borrow the one shared engine VulkanContext) — the handles are valid VK
// objects on that device already, no export/import ceremony (VK->VK, not VK->GL).
// Same-queue ordering: the shadow island's vkWaitForFences() inside run() fully
// retires its submit before returning, and the terrain island runs strictly later in
// the frame (after GL terrain draws) -- so by the time the terrain island samples
// this view, the depth write is already complete and visible; no extra barrier needed
// on the terrain island's read side beyond the descriptor's SHADER_READ_ONLY layout.
void* mc2_vk_shadow_island_get_dyn_shadow_view();
void* mc2_vk_shadow_island_get_dyn_shadow_sampler();
int   mc2_vk_shadow_island_dyn_shadow_ready();

// VK-CSM-FULLMAP-DIRECT-1: default-off companion producer for the CSM catch-all
// cascade. It reuses the shadow island's static-prop geometry/instance machinery,
// but renders the last cascade matrix into a separate exportable D32 image whose
// VkImageView can be bound directly by the terrain island at binding 9. The GL CSM
// path still renders normally; this path only deletes terrain's per-frame
// glGetTexImage+vkCmdCopyBufferToImage full-map import when
// MC2_VK_CSM_FULLMAP_INTEROP=1.
unsigned int mc2_vk_shadow_island_run_csm_fullmap(
    int size, const float* lightSpace16,
    const void* instances, unsigned int instanceCount,
    float biasFactor, float biasUnits);
void* mc2_vk_shadow_island_get_csm_fullmap_view();
void* mc2_vk_shadow_island_get_csm_fullmap_image();
int   mc2_vk_shadow_island_csm_fullmap_ready();

// VK-CSM-ARRAY-DIRECT-1: optional near-cascade companion. Renders each near CSM
// cascade into a layer of a VK-owned D32 2D array; terrain can bind the array view
// directly at binding 8 when all near layers were produced in the current frame.
unsigned int mc2_vk_shadow_island_run_csm_array_layer(
    int size, int layers, int layerIndex, const float* lightSpace16,
    const void* instances, unsigned int instanceCount,
    float biasFactor, float biasUnits);
void* mc2_vk_shadow_island_get_csm_array_view();
void* mc2_vk_shadow_island_get_csm_array_image();
int   mc2_vk_shadow_island_csm_array_ready();

// The world-fixed static sun shadow map (terrain + registry buildings), built once per engine static-map generation (map load, destroy rebake). The
// view is a D32 image in DEPTH_STENCIL_READ_ONLY; null and 0 until the map's first build has been submitted.
void*    mc2_vk_shadow_island_get_static_view();
uint64_t mc2_vk_shadow_island_static_generation();

// MC2_VK_SHADOW_MATRIX_DIAG (Trace gate, default OFF = zero cost): one-shot matrix/depth-map
// evidence for the GL-vs-VK shadow producer/consumer comparison. enabled() is a cached getenv.
int  mc2_vk_shadow_matrix_diag_enabled();
// Called once per frame after the dynamic+CSM caster passes (txmmgr.cpp). Logs
// [VK_SHADOW_MATRIX v1] at tick 600 and 1200; dumps GL+VK depth PNGs at tick 600.
void mc2_vk_shadow_matrix_diag_tick(const float* glDynMat, int dynSize, unsigned glDynTex,
                                    const float* glCascMats, int csmN, int arrSize,
                                    unsigned glArrTex, unsigned glFullTex, int fullSize,
                                    float fullTexelWorld, unsigned glStaticTex, int staticSize);
// Terrain island: returns 1 exactly once after a tick armed a log (consumes the latch).
int  mc2_vk_shadow_matrix_diag_take_terrain_arm();

// Health accessor (mirrors mc2_render_backend_water_region_health).
void mc2_render_backend_shadow_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* validationErrors, unsigned long* attempted, unsigned long* usedVulkan,
    const char** fallbackReason, unsigned long* vkDraws,
    int* interopActive, int* vk2glSemActive, unsigned long* usedInterop,
    unsigned long* instRecords, unsigned long* typeRuns, unsigned long* geomVerts,
    unsigned long* forbiddenCopyImgToBuf, unsigned long* forbiddenGetTexImage,
    unsigned long* forbiddenReadPixels,
    double* stageInstUs, double* drawUs, double* totalUs);

// Teardown (called from gameosmain.cpp before the shared device is destroyed).
void mc2_vk_shadow_island_destroy();
// Map boundary: drop VK-owned caster geometry (re-uploaded on next map).
void mc2_vk_shadow_island_invalidate_geometry();

#ifdef __cplusplus
}
#endif

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VULKAN_SHADOW_ISLAND_LIVE_H
