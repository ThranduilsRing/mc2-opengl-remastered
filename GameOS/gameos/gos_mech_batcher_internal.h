// Shared surface of the gos_mech_batcher.cpp translation units (gos_mech_batcher.cpp, gos_mech_batcher_{geometry,flush,api}.cpp).
//
// gos_mech_batcher.cpp keeps the file-scope state (GL names, program locations, rings, tuning values), program loading, ring
// capacity, map load / unload and flushShadow; the rest of GpuMechBatcher is split by responsibility (see DOC in each piece).
#ifndef GOS_MECH_BATCHER_INTERNAL_H
#define GOS_MECH_BATCHER_INTERNAL_H

#include "gos_mech_batcher.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "vk_feed_dump.h"        // VK-OFFLINE-PROOFS-1: MC2_VK_FEED_DUMP golden dump
#include "gos_materials.h"       // Slice C1: named material profile SSBO (binding 7)
#include "ibl_sh_runtime.h"     // PBR-IBL-MECH-1: g_iblShStrength (shared with static props)
#include "../../RenderCore/IblShCoeffs.h"  // PBR-IBL-MECH-1: SH-L2 coefficients
#include "gos_gpu_sync.h"      // GPU-SYNC-CONTRACT typed barrier helper
#include "render_snapshot.h"  // MECH-EXTRACTION-0: ExtractedMechPacket, RenderSnapshot
#include "../../RenderCore/RenderDebugView.h"  // MECH-DEBUG-VIEWS-1
#include "../../RenderCore/PipelineRegistry.h"  // MECH-PIPELINEDESC-1
#include "pipeline_binder.h"                     // MECH-PIPELINEDESC-1 applyPipeline
#include "../../RenderCore/frame_executor.h"     // APPLY-STATE-MECHOPAQUE-1: ApplyPassId
#include "../../RenderCore/RenderResourceRegistry.h"  // REGISTRY-MATERIAL-SSBO-1: MaterialGpuBuffer
#include "../../RenderCore/GpuBufferOwner.h"  // VK-SHADOW-RING-OWNER-1: ring SSBO owner records
// M2.5: IsObjectIdBufferEnabled() drives the GLSL prefix that gates the
// mech.frag layout(location=2) write. Mirrors the include shipped by M1.5
// at gos_static_prop_batcher.cpp:3. GameOS/ is outside the firewall
// SCOPE_DIRS (scripts/check-include-firewall.sh:22) so this include is
// not policed by the firewall script; reviewer-discipline gate only.
// RenderWorld/RenderWorld.h is the PUBLIC header (no legacy/* reach).
#include "../../RenderWorld/RenderWorld.h"
#include "gos_mech_killswitch.h"
#include "gos_static_prop_batcher.h"  // for STATIC_PROP_RING_FRAMES cross-check
#include "../../mclib/mech_anim_runtime.h"  // 1B-GPU: imported-mech model-delta skinning
#include "gameos.hpp"                 // gos_InvalidateRenderStateCache
#include "utils/shader_builder.h"
#include "gos_postprocess.h"           // getGosPostProcess()->getDynamicLightSpaceMatrix()
#include "gos_terrain_lighting.h"      // POOL-CONSUMERS-1: UploadObjectLightPoolUniforms
#include "view_uniforms_gl.h"          // VK-PARITY-MECH-1: RenderCore::getCurrentView (cameraWorldPos)
#include "../../mclib/txmmgr.h"       // mcTextureManager->get_gosTextureHandle (live resolve)
#include "../../mclib/terrain_depth_bias.h"  // VK-P14 MECH: OBJECT_DEPTH_BIAS feed
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_mech_island.h"       // VK-P14 MECH: island feed contract (Vulkan-free POD)
#endif
#include <GL/glew.h>
#include "gl_state_guard.h"            // GlStateGuard adoption: mc2gl::GlScopedSsboBinding
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <unordered_map>
#include <array>
#include <cmath>
#include <optional>            // GLSTATE-GUARD-ADOPTION-3: deferred-destruct SSBO guards in flush()
#include "gos_object_draw_mvp.h"  // RENDER-VIEW-CURRENCY-1 currency-checked accessor
#include "Gates/gate.h"
#include "../../RenderCore/MaterialGpu.h"
#include "../../RenderCore/GpuBufferOwner.h"
#ifdef MC2_VULKAN_ISLAND
#include "vk_shared_gbuffer_gate.h"  // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
#endif

// Key: (Mech3DAppearanceType*, lod) -> s_typeLodRecords index
struct TypeLodKey {
    const Mech3DAppearanceType* type;
    int lod;
    bool operator==(const TypeLodKey& o) const { return type == o.type && lod == o.lod; }
};
struct TypeLodKeyHash {
    size_t operator()(const TypeLodKey& k) const {
        return std::hash<const void*>()(k.type) ^ (std::hash<int>()(k.lod) * 2654435761u);
    }
};

// Per-frame pending submit list.
struct PendingSubmit {
    GpuMechSubmitDesc        desc;
    std::vector<GpuMechBone> bones;          // staged from listOfShapes[i].shapeToWorld
    std::vector<uint32_t>    packetTexHandles; // per-packet live gosHandle captured at submit
    uint32_t                 typeLodIdx;
};

// Per-frame draw-call snapshot persisted at the END of flush() for use by
// flushShadow() on the NEXT frame.  flushShadow() runs before this frame's
// flush(), so it reads last frame's already-fenced SSBO slot (s_frameSlot)
// together with the drawCalls that were built for that same slot.
struct ShadowDrawEntry {
    uint32_t globalPacketIdx;
    uint32_t instanceBase;
    uint32_t instanceCount;
};
// MECH-LIGHTROW-FRESHNESS-1: bound is (int)GpuMechFallbackReason::StaleLightRow + 1.
// Sized from the enum rather than a bare literal so a future append cannot
// silently write past the end -- the previous bare 5 was exactly that hazard.
static constexpr int kGpuMechFallbackReasonCount =
    static_cast<int>(GpuMechFallbackReason::StaleLightRow) + 1;

// Globals defined in gos_mech_batcher.cpp (or elsewhere in the engine) that the split pieces share.
extern "C" uint64_t consumeAndResetMlrMechDraws();
extern "C" void consumeAndResetMechLightRowCounters(uint64_t* staleOut,
                                                    uint64_t* oobOut);
extern "C" void mc2_framegraph_executor_bump_apply_state(unsigned id);
extern bool g_drawMechs;
extern bool g_useGpuMechs;
extern int g_mechPreviewRenderDepth;
extern bool s_programLoadTried;
extern bool s_programLoadFailed;
extern GLuint s_mechProgram;
extern glsl_program* s_mechProgramObj;
extern int s_mechDebugMode;
extern int s_mechNormalsMode;
extern float s_mechNormalsSmoothDeg;
extern std::vector<std::pair<size_t,size_t>> s_nodeVboRanges;
extern bool s_mechAmbientV1;
extern float s_mechAmbientStrength;
extern bool s_mechSpecularV1;
extern float s_mechSpecularStrength;
extern float s_mechMetalRoughness;
extern float s_mechGlassRoughness;
extern float s_mechGlassLumaThresh;
extern float s_mechGlassMaxChanThresh;
extern bool s_mechSpecDebugMask;
extern float s_mechBackFillStrength;
extern int s_standardLitEnabled;
extern float s_pbrMetallicInfluence;
extern float s_pbrRoughnessMin;
extern float s_pbrRoughnessMax;
extern float s_pbrAmbientSpecularStrength;
extern uint32_t s_mechPaintSurfaceMaterialIdx;
extern float s_pbrWearStrength;
extern int s_pbrTriplanar;
extern float s_pbrTriplanarScale;
extern GLint s_loc_u_instanceBase;
extern GLint s_loc_u_materialFlags;
extern GLint s_loc_terrainMVP;
extern GLint s_loc_u_mvp;
extern GLint s_loc_u_tex;
extern GLint s_loc_u_fogValue;
extern GLint s_loc_u_debugMode;
extern GLint s_loc_u_mechAmbientV1Strength;
extern GLint s_loc_u_lightingMode;
extern GLint s_loc_u_skinningMode;
extern GLint s_loc_u_sunAxisMc2Fix;
extern GLint s_loc_u_mechSpecularV1Strength;
extern GLint s_loc_u_mechMetalRoughness;
extern GLint s_loc_u_mechGlassRoughness;
extern GLint s_loc_u_mechGlassLumaThresh;
extern GLint s_loc_u_mechGlassMaxChanThresh;
extern GLint s_loc_u_mechSpecDebugMask;
extern GLint s_loc_u_mechBackFillStrength;
extern float g_importedMechGamma;
extern float g_importedMechAlbedoScale;
extern float g_importedMechAoStrength;
extern float g_importedMechNormalStrength;
extern GLint s_loc_u_importedGamma;
extern GLint s_loc_u_importedAlbedoScale;
extern GLint s_loc_u_mechAoTex;
extern GLint s_loc_u_aoStrength;
extern GLint s_loc_u_mechNormalTex;
extern GLint s_loc_u_normalStrength;
extern GLint s_loc_u_standardLitEnabled;
extern GLint s_loc_u_pbrNormalTex;
extern GLint s_loc_u_pbrOrmTex;
extern GLint s_loc_u_pbrTileScale;
extern GLint s_loc_u_pbrMetallicInfluence;
extern GLint s_loc_u_pbrRoughnessMin;
extern GLint s_loc_u_pbrRoughnessMax;
extern GLint s_loc_u_pbrAmbientSpecularStrength;
extern GLint s_loc_u_pbrPaintNormalTex;
extern GLint s_loc_u_pbrPaintOrmTex;
extern GLint s_loc_u_pbrWearStrength;
extern GLint s_loc_u_pbrTriplanar;
extern GLint s_loc_u_pbrTriplanarScale;
extern GLint s_loc_u_iblSh;
extern GLint s_loc_u_iblShStrength;
extern GLuint s_sharedVao;
extern GLuint s_sharedVbo;
extern GLuint s_sharedIbo;
extern GLuint s_sampler;
extern bool s_geometryFinalized;
extern bool s_pendingLateTypes;
extern uint32_t s_lateStagedCount;
extern std::vector<uint8_t> s_stagingVbo;
extern std::vector<uint32_t> s_stagingIbo;
extern std::vector<GpuMechTypeLodRecord> s_typeLodRecords;
extern std::vector<GpuMechPacket> s_packets;
extern std::unordered_map<TypeLodKey, uint32_t, TypeLodKeyHash> s_typeLodIndex;
extern GLuint s_instanceSsbo;
extern GLuint s_boneSsbo;
extern void* s_instanceMap;
extern void* s_boneMap;
extern size_t s_instanceCapacity;
extern size_t s_boneCapacity;
extern uint32_t s_frameSlot;
extern GLsync s_fence[MECH_RING_FRAMES];
extern std::vector<PendingSubmit> s_pendingSubmits;
extern std::vector<ExtractedMechPacket> s_mechExtractPersist;
extern std::vector<ShadowDrawEntry> s_lastDrawCalls;
extern size_t s_lastTotalInstances;
extern size_t s_lastTotalBones;
extern uint64_t s_lastFlushSubmitCount;
extern int s_shadowTypesDrawn;
extern int s_shadowInstDrawn;
extern bool s_mechBatcherTrace;
extern bool s_mechBatcherTraceInit;
extern bool s_mechLightTrace;
extern bool s_mechLightTraceInit;
extern uint32_t s_lightCacheFullFrames;
extern uint32_t s_eligibleActorsThisFrame;
extern uint32_t s_fallbacksThisFrame[kGpuMechFallbackReasonCount];
extern uint64_t s_gpuMechIdWritesThisMission;
extern uint64_t s_allowedLateRegEvents;
extern uint64_t s_disallowedLateRegEvents;
extern bool s_lastFailWasLateReg;
extern std::unordered_map<const Mech3DAppearanceType*, uint32_t> s_spotlightRealTypeNodeCount;
extern uint64_t s_spotlightReal_typeNodeEvents;
extern uint64_t s_spotlightReal_drawWindow;
extern uint64_t s_spotlightReal_drawMono;
extern uint64_t s_spotlightReal_drawFrameCount;
extern bool s_spotlightReal_firstHitTypeNode;
extern bool s_spotlightReal_firstHitDraw;
extern std::vector<RenderCore::MaterialGpu> s_mechMaterialTable;
extern std::unordered_map<uint32_t, uint32_t> s_mechHandleToMaterialIdx;
extern std::vector<uint32_t> s_mechDrawMaterialIdx;
extern uint32_t s_mechMaterialGpuFrameCount;
extern uint32_t s_mechSurfaceMaterialIdx;
extern bool s_mechSurfaceMaterialIdxResolved;

// File-scope gate caches (defined in gos_mech_batcher.cpp; evaluated eagerly there).
extern const bool s_mechViewUniforms;
extern const bool s_mechViewUniformsDiag;
extern const bool s_mechIblShEnabled;
extern const bool s_mechSpotlightRealTrace;
extern const bool s_mechMaterialGpuEnabled;
extern RenderCore::GpuBufferOwner s_mechMaterialSsbo;

// Helpers defined in gos_mech_batcher.cpp that the other pieces call.
bool s_mechFixBMvpEnabled();
const float* gos_GetMechFixBMVPMat4();
void loadProgramsIfNeeded();
void ensureRingCapacity(size_t neededInstances, size_t neededBones);

#ifdef MC2_VULKAN_ISLAND
// VK-P14 mech island: lazy gate (function-local static) and the feed snapshots flush() fills.
bool vkMechGateOn();
extern std::vector<GpuMechInstance> s_vkMechInstSnapshot;
extern std::vector<GpuMechBone>     s_vkMechBoneSnapshot;
extern std::vector<Mc2MechDrawRun>  s_vkMechRunSnapshot;
extern float                        s_vkMechWorldToClipGL[16];
extern float                        s_vkMechObjectDepthBias;
extern bool                         s_vkMechFrameReady;
extern std::vector<uint8_t>         s_vkVboNativeCopy;
extern uint32_t                     s_vkMechGeomGen;
#endif

// Free functions defined in the split pieces.
void recomputeMechNodeNormals(std::vector<uint8_t>& vbo, size_t startByte, size_t endByte, int mode, float smoothDeg);
void uploadMechGeometryVbo();

#endif // GOS_MECH_BATCHER_INTERNAL_H
