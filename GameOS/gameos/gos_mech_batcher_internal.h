// Shared surface of the gos_mech_batcher.cpp translation units (gos_mech_batcher.cpp, gos_mech_batcher_{geometry,flush,api}.cpp).
//
// gos_mech_batcher.cpp keeps the file-scope state (GL names, program locations, rings, tuning values), program loading, ring
// capacity, map load / unload and flushShadow; the rest of GpuMechBatcher is split by responsibility (see DOC in each piece).
#ifndef GOS_MECH_BATCHER_INTERNAL_H
#define GOS_MECH_BATCHER_INTERNAL_H

#include "gos_mech_batcher.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gos_materials.h"       // gos_materials::init / shutdown / getProfileIndex (CPU profile table)
#include "vk_feed_dump.h"        // VK-OFFLINE-PROOFS-1: MC2_VK_FEED_DUMP golden dump
#include "render_snapshot.h"  // MECH-EXTRACTION-0: ExtractedMechPacket, RenderSnapshot
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
#include "../../mclib/render_submissions.h"       // mc2::textures::sourceId (live resolve)
#include "../../mclib/terrain_depth_bias.h"  // VK-P14 MECH: OBJECT_DEPTH_BIAS feed
#include "vulkan_mech_island.h"       // VK-P14 MECH: island feed contract (Vulkan-free POD)
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
#include "gos_object_draw_mvp.h"  // RENDER-VIEW-CURRENCY-1 currency-checked accessor
#include "Gates/gate.h"
#include "../../RenderCore/MaterialGpu.h"
#include "../../RenderCore/frame_executor.h"     // APPLY-STATE-MECHOPAQUE-1: ApplyPassId

extern "C" void mc2_framegraph_executor_bump_apply_state(unsigned id);

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

// MECH-LIGHTROW-FRESHNESS-1: bound is (int)GpuMechFallbackReason::StaleLightRow + 1.
// Sized from the enum rather than a bare literal so a future append cannot
// silently write past the end -- the previous bare 5 was exactly that hazard.
static constexpr int kGpuMechFallbackReasonCount =
    static_cast<int>(GpuMechFallbackReason::StaleLightRow) + 1;

// Globals defined in gos_mech_batcher.cpp (or elsewhere in the engine) that the split pieces share.
extern "C" uint64_t consumeAndResetMlrMechDraws();
extern "C" void consumeAndResetMechLightRowCounters(uint64_t* staleOut,
                                                    uint64_t* oobOut);
extern bool g_drawMechs;
extern bool g_useGpuMechs;
extern int g_mechPreviewRenderDepth;
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
extern float g_importedMechGamma;
extern float g_importedMechAlbedoScale;
extern float g_importedMechAoStrength;
extern float g_importedMechNormalStrength;
extern bool s_geometryFinalized;
extern bool s_pendingLateTypes;
extern uint32_t s_lateStagedCount;
extern std::vector<uint8_t> s_stagingVbo;
extern std::vector<uint32_t> s_stagingIbo;
extern std::vector<GpuMechTypeLodRecord> s_typeLodRecords;
extern std::vector<GpuMechPacket> s_packets;
extern std::unordered_map<TypeLodKey, uint32_t, TypeLodKeyHash> s_typeLodIndex;
extern std::vector<PendingSubmit> s_pendingSubmits;
extern std::vector<ExtractedMechPacket> s_mechExtractPersist;
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
extern const bool s_mechSpotlightRealTrace;
extern const bool s_mechMaterialGpuEnabled;

// Helpers defined in gos_mech_batcher.cpp that the other pieces call.
void noteDeprecatedMechFeedNative();
const float* gos_GetMechFixBMVPMat4();

// VK-P14 mech island: the feed snapshots flush() fills.
extern std::vector<GpuMechInstance> s_vkMechInstSnapshot;
extern std::vector<GpuMechBone>     s_vkMechBoneSnapshot;
extern std::vector<Mc2MechDrawRun>  s_vkMechRunSnapshot;
extern float                        s_vkMechWorldToClipGL[16];
extern float                        s_vkMechObjectDepthBias;
extern bool                         s_vkMechFrameReady;
extern std::vector<uint8_t>         s_vkVboNativeCopy;
extern uint32_t                     s_vkMechGeomGen;
extern uint32_t                     s_vkMechSnapshotGeomGen;

// Free functions defined in the split pieces.
void recomputeMechNodeNormals(std::vector<uint8_t>& vbo, size_t startByte, size_t endByte, int mode, float smoothDeg);
void uploadMechGeometryVbo();

#endif // GOS_MECH_BATCHER_INTERNAL_H
