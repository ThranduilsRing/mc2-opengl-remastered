// GpuMechBatcher: the VK island feed accessors, the render-snapshot compare and the extern C tuning / debug setters and getters.
// Declared in the class headers; shared names in gos_mech_batcher_internal.h.
#include <algorithm>
#include "gos_mech_batcher_internal.h"
#include "../../mclib/texture_assets.h"
#include "mc2fmt.h"
#include "../../RenderCore/FramePackets.h"
#include "gos_terrain_lighting.h"      // GetObjectLightPoolParams


// ---------------------------------------------------------------------------
// VK-P14 MECH TIER-1: island feed accessors (Vulkan-free; the island never
// includes this batcher). Geometry is copy-once; the per-frame feed points at
// the persistent snapshots built at flush() end. Return 0 if not ready.
// ---------------------------------------------------------------------------
extern "C" void mc2GetSceneFogColorRGB(float out[3]);

extern "C" uint32_t mc2_mech_vertex_stride(void) { return static_cast<uint32_t>(sizeof(GpuMechVertex)); }

extern "C" int mc2_mech_island_get_geometry_feed(Mc2MechGeomFeed* out) {
    if (!out) return 0;
    if (!s_geometryFinalized) return 0;
    // The CPU bytes the GL buffers are uploaded from: the normals-recomputed vertex blob and the staged indices.
    const uint64_t vboBytes = static_cast<uint64_t>(s_vkVboNativeCopy.size());
    const uint64_t iboBytes = static_cast<uint64_t>(s_vkIboNativeCopy.size()) * sizeof(uint32_t);
    if (vboBytes == 0 || iboBytes == 0) return 0;
    out->vboBytes     = vboBytes;
    out->iboBytes     = iboBytes;
    out->vertexStride = static_cast<uint32_t>(sizeof(GpuMechVertex));   // 48
    out->vboCpu = s_vkVboNativeCopy.data();
    out->iboCpu = s_vkIboNativeCopy.data();
    out->geomGen = s_vkMechGeomGen;
    return 1;
}

extern "C" int mc2_mech_island_get_shadow_feed(Mc2MechShadowFeed* out) {
    if (!s_vkMechShadowReady) return 0;
    if (!out) return 0;
    if (!mc2::gate::get<"MC2_SHADOW_ENABLE">()) return 0;
    if (!s_geometryFinalized) return 0;
    if (s_vkMechInstSnapshot.empty() || s_vkMechRunSnapshot.empty() || s_vkMechBoneSnapshot.empty()) return 0;
    // The snapshot's runs index the geometry of the generation it was built against; after a rebuild they name ranges of a different index set.
    if (s_vkMechSnapshotGeomGen != s_vkMechGeomGen) return 0;
    if (s_vkVboNativeCopy.empty() || s_vkIboNativeCopy.empty()) return 0;
    out->instances64 = s_vkMechInstSnapshot.data(); out->instanceCount = static_cast<uint32_t>(s_vkMechInstSnapshot.size());
    out->bones64     = s_vkMechBoneSnapshot.data(); out->boneCount     = static_cast<uint32_t>(s_vkMechBoneSnapshot.size());
    out->runs        = s_vkMechRunSnapshot.data();  out->runCount      = static_cast<uint32_t>(s_vkMechRunSnapshot.size());
    out->vboCpu = s_vkVboNativeCopy.data(); out->vboBytes = static_cast<uint64_t>(s_vkVboNativeCopy.size());
    out->iboCpu = s_vkIboNativeCopy.data(); out->iboBytes = static_cast<uint64_t>(s_vkIboNativeCopy.size()) * sizeof(uint32_t);
    out->vertexStride = static_cast<uint32_t>(sizeof(GpuMechVertex));
    out->geomGen = s_vkMechGeomGen;
    return 1;
}

extern "C" int mc2_mech_island_get_frame_feed(Mc2MechFrameFeed* out) {
    if (!out) return 0;
    if (!s_vkMechFrameReady) return 0;
    if (s_vkMechInstSnapshot.empty() || s_vkMechRunSnapshot.empty()) return 0;
    // Stale snapshot from before a geometry rebuild (new map, late types): its firstIndex values index the OLD index set (the draw would
    // read past the new buffer). The next flush() rebuilds it against the new set.
    if (s_vkMechSnapshotGeomGen != s_vkMechGeomGen) return 0;
    out->instances64   = s_vkMechInstSnapshot.data();
    out->instanceCount = static_cast<uint32_t>(s_vkMechInstSnapshot.size());
    out->bones64       = s_vkMechBoneSnapshot.data();
    out->boneCount     = static_cast<uint32_t>(s_vkMechBoneSnapshot.size());
    out->motionKeys    = s_vkMechMotionKeys.size() == s_vkMechInstSnapshot.size() ? s_vkMechMotionKeys.data() : nullptr;
    out->runs          = s_vkMechRunSnapshot.data();
    out->runCount      = static_cast<uint32_t>(s_vkMechRunSnapshot.size());
    out->leafCount     = s_vkMechLeafCount;
    // VK-FLIP-FIELD-BUG4-1: use the LIVE camera MVP, not the flush-time snapshot.
    // s_vkMechWorldToClipGL is captured only inside GpuMechBatcher::flush() (batcher
    // submit path). On frames where the batcher skips its flush (mech instance data
    // unchanged — units idle), that snapshot is STALE, but a camera PAN still changes
    // the view/proj. The island then draws mechs with last frame's MVP -> mechs FREEZE
    // in screen space while the terrain (which re-projects live) pans beneath them.
    // This accessor runs on the render thread after the camera update, so
    // gos_GetMechFixBMVPMat4() returns the current-frame MVP here. Instances may be one
    // frame stale (imperceptible when idle); the PROJECTION is always current -> no
    // screen-space stick. Falls back to the snapshot if the live accessor returns null.
    if (const float* liveMvp = gos_GetMechFixBMVPMat4())
        std::memcpy(out->worldToClipGL, liveMvp, sizeof(out->worldToClipGL));
    else
        std::memcpy(out->worldToClipGL, s_vkMechWorldToClipGL, sizeof(out->worldToClipGL));
    out->objectDepthBias = s_vkMechObjectDepthBias;
    // Fog colour and the game's CPU LightsData array (the bytes the GL SSBO is uploaded from) for calc_light + fog.
    out->fogColorRGB[0] = out->fogColorRGB[1] = out->fogColorRGB[2] = 0.0f;
    mc2GetSceneFogColorRGB(out->fogColorRGB);
    out->lights = mc2::vkLights::captureFeed();
    // VK-PARITY-MECH-1: default mech FS shading inputs from the GL producers.
    {
        const RenderCore::ViewUniforms vu = RenderCore::framePackets().copyMainView().uniforms;   // by value: readers copy synchronously (FramePackets.h)
        for (int k = 0; k < 4; ++k) out->cameraWorldPos[k] = vu.cameraWorldPos[k];
        GpuMechShadingParams shp{};
        gpuMechShadingParams(&shp);
        out->ambientStrength     = shp.ambientStrength;
        out->specularStrength    = shp.specularStrength;
        out->metalRoughness      = shp.metalRoughness;
        out->glassRoughness      = shp.glassRoughness;
        out->glassLumaThresh     = shp.glassLumaThresh;
        out->glassMaxChanThresh  = shp.glassMaxChanThresh;
        out->specDebugMask       = shp.specDebugMask;
        out->importedGamma       = shp.importedGamma;
        out->importedAlbedoScale = shp.importedAlbedoScale;
        out->aoStrength          = shp.aoStrength;
        out->normalStrength      = shp.normalStrength;
        gos_terrain_lighting::ObjectLightPoolParams pp{};
        gos_terrain_lighting::GetObjectLightPoolParams(&pp);
        out->numPoolLights  = pp.pool ? pp.count : 0;
        out->poolLights48   = pp.pool;
        out->poolSoft       = pp.soft;
        out->poolMoverDim   = pp.moverDim;
        out->poolMoverScale = pp.moverScale;
        out->poolMoverMax   = pp.moverMax;
        out->poolCompress   = pp.compress;
        out->poolObjScale   = pp.objScale;
    }

    // VK-OFFLINE-PROOFS-1: golden feed dump. Fires once per process (see
    // vk_feed_dump.h). No-op unless MC2_VK_FEED_DUMP is set.
    if (mc2vkfeeddump::enabled() && mc2vkfeeddump::readyToDump() && !mc2vkfeeddump::latch("mech")) {
        if (FILE* f = mc2vkfeeddump::openAppend(/*truncateFirst=*/false)) {
            mc2::fmt::print(f, "{{\"island\":\"mech\",\"instanceCount\":{:d},\"boneCount\":{:d},\"runCount\":{:d},\"objectDepthBias\":{:.6f},\"instances\":[", mc2::fmt::as_u32(out->instanceCount), mc2::fmt::as_u32(out->boneCount), mc2::fmt::as_u32(out->runCount), out->objectDepthBias);
            const GpuMechInstance* insts =
                reinterpret_cast<const GpuMechInstance*>(out->instances64);
            const GpuMechBone* bones =
                reinterpret_cast<const GpuMechBone*>(out->bones64);
            for (uint32_t i = 0; i < out->instanceCount; ++i) {
                const GpuMechInstance& mi = insts[i];
                float rootX = 0.f, rootY = 0.f, rootZ = 0.f;
                if (bones && mi.baseBoneOffset < out->boneCount) {
                    const GpuMechBone& b0 = bones[mi.baseBoneOffset];
                    rootX = b0.row0[3]; rootY = b0.row1[3]; rootZ = b0.row2[3];
                }
                mc2::fmt::print(f, "{}{{\"typeLodRecordIndex\":{:d},\"baseBoneOffset\":{:d},\"objectIdRaw\":{:d},\"materialIdx\":{:d},\"rootBoneX\":{:.4f},\"rootBoneY\":{:.4f},\"rootBoneZ\":{:.4f}}}", mc2::fmt::safe(i ? "," : ""), mc2::fmt::as_u32(mi.typeLodRecordIndex), mc2::fmt::as_u32(mi.baseBoneOffset), mc2::fmt::as_u32(mi.objectIdRaw), mc2::fmt::as_u32(mi.materialIdx), rootX, rootY, rootZ);
            }
            mc2::fmt::print(f, "]}}\n");
            std::fclose(f);
        }
        mc2vkfeeddump::latch("mech") = true;
    }
    return 1;
}

void batcher_compareMechSnapshot(RenderSnapshot* snap) {
    if (!snap) return;
    const uint32_t liveCount  = static_cast<uint32_t>(s_mechExtractPersist.size());
    const uint32_t snapCount  = static_cast<uint32_t>(snap->mechPackets.size());
    if (snapCount != liveCount)
        snap->mechCountMismatch = 1u;
    const uint32_t compareCount = snapCount < liveCount ? snapCount : liveCount;
    for (uint32_t i = 0u; i < compareCount; ++i) {
        const ExtractedMechPacket& row = snap->mechPackets[i];
        const ExtractedMechPacket& ref = s_mechExtractPersist[i];
        if (row.typeLodIdx  != ref.typeLodIdx)  ++snap->mechHandleMismatch;
        if (row.objectIdRaw != ref.objectIdRaw) ++snap->mechObjectIdMismatch;
        if (row.texHandle   != ref.texHandle)   ++snap->mechTexHandleMismatch;
        // MECH-EXTRACTION-1: independent live materialIdx from s_mechHandleToMaterialIdx
        // (still valid post-flush; cleared at START of next flush). Stronger than
        // snapshot-vs-persist self-consistency — map is the same authority the SSBO
        // upload used, so a divergence here means the persist capture was wrong.
        {
            auto it = s_mechHandleToMaterialIdx.find(row.texHandle);
            uint32_t liveMat = 0xFFFFFFFFu;
            if (s_mechMaterialGpuEnabled && it != s_mechHandleToMaterialIdx.end()) {
                const auto& [handle, index] = *it;
                liveMat = index;
            }
            if (row.materialIdx != liveMat)
                ++snap->mechMaterialIdxMismatch;
        }
    }
}

// MECH-SPINE-1: read-only free-function accessors for the Object Inspector's
// Mech Snapshot panel. Mirrors the TERRAIN-SPINE-0 pattern (program-id
// accessors in gameos_graphics.cpp). Each returns 0 / nullptr when the
// underlying state is not yet initialized. Read-only — no GL state touched,
// no draw-path mutation.
// SHADOW-SPINE-0: read-only accessors for mech shadow caster counts written
// at the end of flushShadow(). Returns last-frame values; not reset.
extern "C" uint32_t gos_getMechShadowTypesDrawn() {
    return static_cast<uint32_t>(s_shadowTypesDrawn);
}
extern "C" uint32_t gos_getMechShadowInstDrawn() {
    return static_cast<uint32_t>(s_shadowInstDrawn);
}
extern "C" const char* gos_getMechTextureNameByNodeIdx(uint32_t nodeIdx) {
    if (nodeIdx == 0xFFFFFFFFu || !mc2::textures::ready()) return nullptr;
    return mc2::textures::name(static_cast<DWORD>(nodeIdx));
}

// MECH-NORMALS-FIX-1: runtime getter/setter API for the normals mode and
// smoothing angle. These allow ImGui controls to dial settings without a restart.
// batcher_rebuildMechNormals() applies the current settings by re-uploading the
// VBO from the pristine s_stagingVbo (mode 0 = byte-identical to cooked).

extern "C" void batcher_setMechNormalsMode(int mode) {
    mode = std::clamp(mode, 0, 2);
    s_mechNormalsMode = mode;
}

extern "C" int batcher_getMechNormalsMode() {
    return s_mechNormalsMode;
}

extern "C" void batcher_setMechNormalsSmoothDeg(float deg) {
    deg = std::clamp(deg, 1.0f, 179.0f);
    s_mechNormalsSmoothDeg = deg;
}

extern "C" float batcher_getMechNormalsSmoothDeg() {
    return s_mechNormalsSmoothDeg;
}

// MECH-SPECULAR-V1: runtime API for the gated Blinn specular sheen. All per-flush
// uniforms — no VBO rebuild. Changes take effect the next frame. When the
// viewuniforms variant is not active (s_mechViewUniforms==false) the C++ flush
// uploads strength 0 regardless of the gate, so the setter is safe to call but
// the visual has no effect until MC2_MECH_VIEWUNIFORMS=1 is also set.
extern "C" void  batcher_setMechSpecularEnabled(int on)      { s_mechSpecularV1 = (on != 0); }
extern "C" int   batcher_getMechSpecularEnabled()            { return s_mechSpecularV1 ? 1 : 0; }
extern "C" void  batcher_setMechSpecularStrength(float s) {
    s = std::clamp(s, 0.0f, 4.0f);
    s_mechSpecularStrength = s;
}
extern "C" float batcher_getMechSpecularStrength()           { return s_mechSpecularStrength; }
extern "C" void  batcher_setMechMetalRoughness(float r) {
    r = std::clamp(r, 0.04f, 1.0f);
    s_mechMetalRoughness = r;
}
extern "C" float batcher_getMechMetalRoughness()             { return s_mechMetalRoughness; }
// BT2018-MECH-MATERIAL-GAMMA-1/TUNING-1: imported-mech albedo knobs (imported only).
extern "C" void  batcher_setImportedMechGamma(float g) {
    g_importedMechGamma = std::clamp(g, 1.0f, 2.4f);
}
extern "C" float batcher_getImportedMechGamma()              { return g_importedMechGamma; }
extern "C" void  batcher_setImportedMechAlbedoScale(float s) {
    g_importedMechAlbedoScale = std::clamp(s, 0.25f, 2.5f);
}
extern "C" float batcher_getImportedMechAlbedoScale()        { return g_importedMechAlbedoScale; }
extern "C" void  batcher_setImportedMechAoStrength(float s) {
    g_importedMechAoStrength = std::clamp(s, 0.0f, 1.0f);
}
extern "C" float batcher_getImportedMechAoStrength()         { return g_importedMechAoStrength; }
extern "C" void  batcher_setImportedMechNormalStrength(float s) {
    g_importedMechNormalStrength = std::clamp(s, 0.0f, 1.0f);
}
extern "C" float batcher_getImportedMechNormalStrength()     { return g_importedMechNormalStrength; }
extern "C" void  batcher_setMechGlassRoughness(float r) {
    r = std::clamp(r, 0.04f, 1.0f);
    s_mechGlassRoughness = r;
}
extern "C" float batcher_getMechGlassRoughness()             { return s_mechGlassRoughness; }
extern "C" void  batcher_setMechGlassLumaThresh(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    s_mechGlassLumaThresh = t;
}
extern "C" float batcher_getMechGlassLumaThresh()            { return s_mechGlassLumaThresh; }
extern "C" void  batcher_setMechGlassMaxChanThresh(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    s_mechGlassMaxChanThresh = t;
}
extern "C" float batcher_getMechGlassMaxChanThresh()         { return s_mechGlassMaxChanThresh; }
extern "C" void  batcher_setMechSpecDebugMask(int on)        { s_mechSpecDebugMask = (on != 0); }
extern "C" int   batcher_getMechSpecDebugMask()              { return s_mechSpecDebugMask ? 1 : 0; }

// batcher_rebuildMechNormals: rebuild the geometry bytes the Vulkan islands upload with the current
// s_mechNormalsMode and s_mechNormalsSmoothDeg. No-op if geometry is not yet finalized. The new
// geometry generation makes every consumer re-upload exactly once.
extern "C" void batcher_rebuildMechNormals() {
    if (!s_geometryFinalized) return;

    uploadMechGeometryVbo();

    MC2_DIAGFE(stderr,
        "[MECHBATCHER v1] event=normals_rebuild mode=%d smoothDeg=%.1f nodes=%zu vbo_bytes=%zu\n",
        s_mechNormalsMode, s_mechNormalsSmoothDeg, s_nodeVboRanges.size(), s_stagingVbo.size());
}
