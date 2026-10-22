// GpuStaticPropBatcher: StaticPropViewToShaderMode, onMapLoad, setMissionForIbl, onMapUnload, registerType and registerMultiShape.
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include <algorithm>
#include "gos_static_prop_batcher_internal.h"
#include "static_caster_set.h"
#include "../../mclib/texture_assets.h"
#include "mc2fmt.h"

// ---------------------------------------------------------------------------
// DEBUG-VIEW-REGISTRY-1: static-prop shader mode <-> RenderDebugView mapping.
// Defined outside the anonymous namespace so they have external linkage and
// can be called from gui_runtime (EditorInspector) via the header declarations.
// Shader branch numbers MUST NOT change (would cause shader_reflect drift).
// 0=Final, 1=Albedo, 2=MaterialIdx, 3=Normal, 4=TexArrayLayer, 5=Roughness, 6=Metallic
// ---------------------------------------------------------------------------
int StaticPropViewToShaderMode(RenderDebugView v) {
    switch (v) {
        case RenderDebugView::Final:          return 0;
        case RenderDebugView::Albedo:         return 1;
        case RenderDebugView::MaterialIdx:    return 2;
        case RenderDebugView::Normal:         return 3;
        case RenderDebugView::TexArrayLayer:  return 4;
        case RenderDebugView::Roughness:      return 5;
        case RenderDebugView::Metallic:       return 6;
        default:                              return 0;  // unsupported -> Final
    }
}

RenderDebugView StaticPropShaderModeToView(int m) {
    switch (m) {
        case 0: return RenderDebugView::Final;
        case 1: return RenderDebugView::Albedo;
        case 2: return RenderDebugView::MaterialIdx;
        case 3: return RenderDebugView::Normal;
        case 4: return RenderDebugView::TexArrayLayer;
        case 5: return RenderDebugView::Roughness;
        case 6: return RenderDebugView::Metallic;
        default: return RenderDebugView::Final;
    }
}

void batcher_setDebugMaterialMode(int shaderMode) {
    shaderMode = std::clamp(shaderMode, 0, 6);
    s_staticPropDebugMaterialMode = shaderMode;
}

int batcher_getDebugMaterialMode() {
    return s_staticPropDebugMaterialMode;
}

GpuStaticPropBatcher& GpuStaticPropBatcher::instance() {
    static GpuStaticPropBatcher s;
    return s;
}

void GpuStaticPropBatcher::onMapLoad() {
    s_packets.clear();
    s_packetAlphaBaked.clear();
    s_types.clear();
    s_perTypePeak.clear();
    s_coalesceFrameSlot = 0;  // reset coalesce ring slot per-mission for hygiene
    s_typeIndex.clear();
    s_typeDescTable.clear();
    // Release (not just clear) the previous map's CPU geometry copy so it is freed at map change, not kept at the largest
    // map's capacity.
    std::vector<uint8_t>().swap(s_stagingVbo);
    std::vector<uint32_t>().swap(s_stagingIbo);
    s_sharedVboBytes = 0;   // VK-P8a SHADOW-INTEROP: geometry feed invalid after unmap
    s_sharedIboBytes = 0;
#ifdef MC2_VULKAN_ISLAND
    mc2_vk_shadow_island_invalidate_geometry();
    mc2shadow::staticCasterSet().clear();   // the next mission rebuilds it once its geometry is final
    mc2_vk_color_prop_island_invalidate_geometry();   // map change: drop map-1 VBO/IBO
#endif
    s_typeAlbedoLayer.clear();  // VK-P11 STATIC-PROP MATERIAL: feed invalid after unmap
    s_typeAlphaClass.clear();
    s_typeHotColors.clear();
    s_failedTypes.clear();
    s_geometryFinalized = false;
    s_fatalRegistrationFailure = false;

    s_sortedTypeOrder.clear();
    s_alphaOffCount = 0;
    s_alphaOnCount  = 0;
    s_offGroupTotalBytes = 0;
    s_onGroupTotalBytes  = 0;
    s_coalescePerFrameInstanceBytes = 0;
    s_coalesceLayoutReady    = false;
    s_coalesceEnabled        = false;
    s_coalesceArmed          = false;
    s_coalesceFirstFlushDone = false;

    // V-IBL-STATIC-2: re-default to baseline set at every map boundary so a
    // mission that calls setMissionForIbl(nullptr) (or never calls it -- e.g.
    // editor / savegame paths) lands on "default" regardless of what the
    // previous mission resolved to.
    s_currentShSet = &RenderCore::kIblShSets[0];
}

// V-IBL-STATIC-2: select per-mission SH coefficient set. Called from
// Mission::init right after onMapLoad(); safe to call with nullptr/empty
// (resolves to "default"); the registry names the set, else default.
void GpuStaticPropBatcher::setMissionForIbl(const char* missionName) {
    s_currentShSet = &RenderCore::lookupShSet(missionName);
}

// V-IBL-STATIC-2: inspector accessor. Never returns nullptr.
const char* GpuStaticPropBatcher::getCurrentShSetName() {
    return s_currentShSet ? s_currentShSet->name : "default";
}

// V-IBL-STATIC-2: ibl_sh_runtime.h bridge. GuiRuntime/EditorInspector.cpp
// includes ibl_sh_runtime.h (no Stuff/ dependency) and calls this to
// surface the active set name without pulling the batcher header.
const char* ibl_sh_runtime_currentSetName() {
    return GpuStaticPropBatcher::getCurrentShSetName();
}

void GpuStaticPropBatcher::onMapUnload() {
    GpuStaticPropRegistry::staticPropRegistryClearMaterialCache();
    for (DWORD nodeIdx : s_coalescePinnedNodes) mc2::textures::unpin(nodeIdx);
    s_coalescePinnedNodes.clear();
    for (auto& array : albedoArrays_) array = {};
    albedoKey_ = {};
    s_packetTexArrayLayer.clear(); s_packetUvScaleX.clear(); s_packetUvScaleY.clear();
    s_typeInstanceCountPrevFrame.clear();
    s_packetMaterialIdx.clear(); s_materialGpuTable.clear(); s_materialInventory.clear();
    s_materialGpuSidecarValid = false;
    s_sortedPacketOrder.clear(); s_alphaOffCmdCount = s_alphaOnCmdCount = 0;
    s_bucketsByType.clear(); s_persistentStaticStore.clear();
    s_persistentStaticGen = s_staticBaseBuiltGen = 0xFFFFFFFFFFFFFFFFull;
    s_staticBaseInstanceForType.clear();
    onMapLoad();
}

//
// Packet enumeration: TG_TypeMultiShape::listOfTypeShapes[] nodes are leaves
// (each is either a TG_TypeShape with geometry or a SHAPE_NODE-less bone).
// Callers iterate listOfTypeShapes in author order and call registerType() on
// each SHAPE_NODE leaf, so per-type packet order within this function only
// needs to preserve the flat listOfTypeTriangles author order.
//
// Vertex layout note: in this fork TG_TypeVertex has no UVs; UVs live on
// TG_TypeTriangle::uvdata as per-corner u0/v0/u1/v1/u2/v2. The same vertex can
// carry different UVs on different triangles, so we cannot emit one shared
// vertex per TG_TypeVertex and share it across triangles with an index buffer.
// We expand each triangle to 3 fresh vertices (triangle-soup) and emit a
// trivial 0..N*3-1 index buffer. baseVertex points at the start of this
// type's vertex run in the shared VBO. Packet indexCount = runTris * 3.
// ---------------------------------------------------------------------------
void GpuStaticPropBatcher::registerType(TG_TypeShape* typeShape, TG_TypeMultiShape* multiShape) {
    // multiShape parameter introduced by 5327c4b API cleanup for upcoming
    // alpha-test self-awareness work (Path 4 — texture-name-based material
    // classification at register time). Spec:
    // docs/superpowers/specs/2026-05-06-static-prop-alpha-test-self-awareness.md
    // The header signature was committed in 5327c4b without the matching .cpp
    // signature update, leaving the build broken. This commit closes the gap
    // — multiShape is captured here but not yet consumed; the consumption
    // logic lands in the follow-up alpha-test self-awareness slice.
    (void)multiShape;
    if (!typeShape) return;
    if (s_gpuPropsTrace) ++g_regCall[g_lightProbeSetupPath & 3];   // [GPUPROPS v1]
    if (s_typeIndex.count(typeShape)) return;  // idempotent
    if (s_geometryFinalized) {
        // Layer B: register-after-finalize is a bug in the map-load walk.
        if (!s_failedTypes[typeShape]) {
            mc2::fmt::print_err("[GPUPROPS] late registerType for {:016X} -- CPU-fallback for this type\n", mc2::fmt::as_ptr(static_cast<void*>(typeShape)));
            s_failedTypes[typeShape] = true;
        }
        if (s_gpuPropsTrace) ++g_regLateDrop[g_lightProbeSetupPath & 3];
        return;
    }

    // GHOST-CONE-RECIPE-SKIP-1 (batch-7 items 4+5, ROOT fix): SpotLight_ cone
    // TYPE geometry must never enter the immutable VBO / packet stream. The
    // per-instance skips (submitMultiShape ~:5006 + the bdactor recipe loops)
    // keep cone INSTANCES out, but the substrate-coalesce multidraw proved able
    // to draw cone PACKETS anyway (mc2_10 saved_212019: opaque tan wedge gone
    // with MC2_SUBSTRATE_COALESCE_LEGACY=1, present with coalesce armed, on an
    // exe whose instance paths all skip cones). Registering the cone type as an
    // EMPTY type (zero packets — the exact shape the helper branch produces)
    // removes the geometry at the source; every consumer already treats
    // zero-packet types as no-draw. Cones are represented by real TG_Lights +
    // the additive cone glow (LIGHT-CONE-MODELS-1) instead.
    // Killswitch MC2_STATICPROP_RECIPE_SPOTLIGHT_SKIP=0 restores the bake.
    {
        static const bool s_typeSpotSkip = mc2::gate::get<"MC2_STATICPROP_RECIPE_SPOTLIGHT_SKIP">();
        const char* nid = typeShape->getNodeId();
        if (s_typeSpotSkip && nid && _strnicmp(nid, "SpotLight_", 10) == 0) {
            GpuStaticPropType emptyType{};
            emptyType.firstPacket = static_cast<uint32_t>(s_packets.size());
            emptyType.packetCount = 0;
            emptyType.vertexCount = 0;
            emptyType.source      = typeShape;
            s_typeIndex[typeShape] = static_cast<uint32_t>(s_types.size());
            s_types.push_back(emptyType);
            return;
        }
    }

    const uint32_t numTris = typeShape->numTypeTriangles;
    if (numTris == 0 || !typeShape->listOfTypeTriangles ||
        !typeShape->listOfTypeVertices) {
        // Empty / helper node -- register with zero packets so duplicate
        // calls remain idempotent.
        GpuStaticPropType emptyType{};
        emptyType.firstPacket = static_cast<uint32_t>(s_packets.size());
        emptyType.packetCount = 0;
        emptyType.vertexCount = 0;
        emptyType.source      = typeShape;
        s_typeIndex[typeShape] = static_cast<uint32_t>(s_types.size());
        s_types.push_back(emptyType);
        return;
    }

    const uint32_t baseVertex = static_cast<uint32_t>(s_stagingVbo.size() / kVertexStride);
    const uint32_t newTypeID  = static_cast<uint32_t>(s_types.size());

    // Group triangles with the same localTextureHandle into contiguous packets,
    // preserving authored listOfTypeTriangles order. Each packet emits 3
    // vertices per triangle into s_stagingVbo and 3 consecutive indices into
    // s_stagingIbo (triangle-soup -- see vertex layout note above).
    uint32_t runStart = 0;
    uint32_t packetCountForThisType = 0;
    while (runStart < numTris) {
        const DWORD runTextureIdx =
            typeShape->listOfTypeTriangles[runStart].localTextureHandle;
        uint32_t runEnd = runStart;
        while (runEnd < numTris &&
               typeShape->listOfTypeTriangles[runEnd].localTextureHandle == runTextureIdx) {
            ++runEnd;
        }

        const uint32_t packetFirstIndex = static_cast<uint32_t>(s_stagingIbo.size());

        for (uint32_t t = runStart; t < runEnd; ++t) {
            const TG_TypeTriangle& tri = typeShape->listOfTypeTriangles[t];

            const float cornerU[3] = { tri.uvdata.u0, tri.uvdata.u1, tri.uvdata.u2 };
            const float cornerV[3] = { tri.uvdata.v0, tri.uvdata.v1, tri.uvdata.v2 };

            for (int c = 0; c < 3; ++c) {
                const uint32_t localVertIdx = tri.Vertices[c];
                // localVertIdx is an index into listOfTypeVertices for the
                // source TG_TypeVertex; we still pass it through to the shader
                // as a_localVertexID for per-instance color indexing.
                const TG_TypeVertex& src = typeShape->listOfTypeVertices[localVertIdx];

                uint8_t vert[kVertexStride] = {};
                std::memcpy(vert +  0, &src.position.x, 4);
                std::memcpy(vert +  4, &src.position.y, 4);
                std::memcpy(vert +  8, &src.position.z, 4);
                std::memcpy(vert + 12, &src.normal.x,   4);
                std::memcpy(vert + 16, &src.normal.y,   4);
                std::memcpy(vert + 20, &src.normal.z,   4);
                std::memcpy(vert + 24, &cornerU[c],     4);
                std::memcpy(vert + 28, &cornerV[c],     4);
                std::memcpy(vert + 32, &localVertIdx,   4);
                // Slice 2 (object-offload) — Stage 2.C.2: per-vertex aRGBLight
                // tag at VBO offset 36 (was zero-filled in Stage 2.A's substrate).
                // Source is TG_TypeVertex::aRGBLight at mclib/tgl.h:54 — the per-
                // type vertex hot-color tag (e.g. 0xffff00ff = "lit window at
                // night", 0xffffff00 = "outside building light"). lighting.hglsl
                // get_base_light() decodes the magic via its expected B,G,R,A
                // byte order on little-endian x86, matching memory/mc2_argb_packing.md.
                // Raw DWORD memcpy preserves the exact bit pattern.
                std::memcpy(vert + 36, &src.aRGBLight,  4);
                s_stagingVbo.insert(s_stagingVbo.end(), vert, vert + kVertexStride);

                const uint32_t expandedIdx =
                    static_cast<uint32_t>((s_stagingVbo.size() / kVertexStride) -
                                          1 - baseVertex);
                s_stagingIbo.push_back(expandedIdx);
            }
        }

        GpuStaticPropPacket pkt{};
        pkt.firstIndex    = packetFirstIndex;
        pkt.indexCount    = (runEnd - runStart) * 3;
        pkt.baseVertex    = static_cast<int32_t>(baseVertex);
        // Store the texture slot index, not the handle. MC2 updates the
        // handle each frame via SetTextureHandle; resolving it at draw
        // time picks up the current value instead of a stale snapshot.
        pkt.textureSlot   = runTextureIdx;
        // alphaTestOn captures shape-level alpha test (trees, via SetAlphaTest).
        // textureAlpha per-slot is resolved at draw time (after bdactor.cpp init completes).
        pkt.materialFlags = typeShape->alphaTestOn ? STATIC_PROP_FLAG_ALPHA_TEST : 0;
        pkt.owningTypeID  = newTypeID;
        s_packets.push_back(pkt);
        ++packetCountForThisType;

        runStart = runEnd;
    }

    const uint32_t numVerts = typeShape->numTypeVertices;

    GpuStaticPropType type{};
    type.firstPacket = static_cast<uint32_t>(s_packets.size()) - packetCountForThisType;
    type.packetCount = packetCountForThisType;
    type.vertexCount = numVerts;
    type.source      = typeShape;

    s_typeIndex[typeShape] = newTypeID;
    s_types.push_back(type);

    // [SEAMPROBE] stage 4: record geometry of each freshly-registered type
    // (capped) so override render-shape leaves can be matched by ptr in the log.
    {
        static int s_seamRegLogged = 0;
        if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">() && s_seamRegLogged < 400) {
            ++s_seamRegLogged;
            mc2::fmt::print_err("[SEAMPROBE] sample=first400 registerType src={:016X} typeID={:d} numTris={:d} numVerts={:d} finalized={:d}\n", mc2::fmt::as_ptr(static_cast<void*>(typeShape)), mc2::fmt::as_u32(newTypeID), mc2::fmt::as_u32(numTris), mc2::fmt::as_u32(numVerts), mc2::fmt::as_i32(static_cast<int>(s_geometryFinalized)));
            fflush(stderr);
        }
    }
}

void GpuStaticPropBatcher::registerMultiShape(TG_TypeMultiShape* multiShape, bool isOverride) {
    if (!multiShape) return;
    const long n       = multiShape->GetNumShapes();
    const long numTxms = multiShape->GetNumTextures();
    for (long i = 0; i < n; ++i) {
        TG_TypeNodePtr node = multiShape->GetTypeNode(i);
        if (node && node->GetNodeType() == SHAPE_NODE) {
            TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(node);
            registerType(typeShape, multiShape);
            // MODEL-OVERRIDE-GPU-BATCHER-SEAM: tag the just-registered (or
            // pre-existing idempotent) type as override-backed so the coalesce
            // layer-build routes its untextured packets to a valid default
            // layer instead of culling them. Look up by source ptr.
            if (isOverride) {
                auto tit = s_typeIndex.find(typeShape);
                if (tit != s_typeIndex.end() && tit->second < s_types.size()) {
                    s_types[tit->second].isOverride = true;
                }
            }
            // GPU-offloaded actors bypass TransformMultiShape, so the leaf
            // TG_TypeShape::listOfTextures[j].gosTextureHandle is never set by
            // TMS. Prime it now from the multi-type's
            // mcTextureNodeIndex — the same value TMS would use. This ensures
            // flush()'s draw-time resolve picks up the correct (upscaled) handle
            // instead of the 0xffffffff left by TG_TypeShape::init().
            for (long j = 0; j < numTxms; ++j) {
                const DWORD nodeIdx = multiShape->GetTextureHandle(j);
                typeShape->SetTextureHandle(j, nodeIdx);
            }
        }
    }
}
