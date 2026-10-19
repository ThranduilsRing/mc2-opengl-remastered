// Native overlay/decal batch queues and frame inputs; shared top-level GL axes adapter.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"
#include "mc2fmt.h"


// OVERLAY-BIAS-LINEARIZE-1: bias-mode selector for the overlay/decal VS
// (terrain_overlay.vert, shared by TerrainOverlayBatch + DecalBatch +
// decal-static). Default 0 = distance-proportional bias (world error ~
// linear -> no far-zoom overlay float). MC2_OVERLAY_BIAS_LEGACY=1 -> legacy
// constant NDC offset. Single consumer is the VS (no compute admission gate
// mirrors the overlay bias). See mclib/terrain_depth_bias.h.
static int mc2OverlayBiasLegacy() {
    static const int mode = mc2::gate::get<"MC2_OVERLAY_BIAS_LEGACY">();
    return mode;
}

// Private helper: push one triangle into a batch, grouping consecutive same-texture
// calls into a single draw entry.  Batches are cleared at the END of the draw
// functions (called exactly once per frame from renderLists), not on push.
void gosRenderer::pushToOverlayBatch_(OverlayBatch_& b,
                                      const WorldOverlayVert* v3,
                                      unsigned int texHandle)
{
    if (!b.draws.empty() && b.draws.back().texHandle == texHandle) {
        b.draws.back().vertCount += 3;
    } else {
        OverlayBatchEntry_ entry;
        entry.texHandle = texHandle;
        entry.firstVert = (unsigned int)b.verts.size();
        entry.vertCount = 3;
        b.draws.push_back(entry);
    }
    b.verts.push_back(v3[0]);
    b.verts.push_back(v3[1]);
    b.verts.push_back(v3[2]);
}

void gosRenderer::pushTerrainOverlayTri(const WorldOverlayVert* verts3, unsigned int texHandle)
{
    pushToOverlayBatch_(terrainOverlayBatch_, verts3, texHandle);
}

void gosRenderer::pushDecalTri(const WorldOverlayVert* verts3, unsigned int texHandle)
{
    pushToOverlayBatch_(decalBatch_, verts3, texHandle);
}

#ifdef MC2_VULKAN_ISLAND
// VK-PARITY-OVERLAY-1: GL-side per-frame inputs of the overlay/decal shaders that the VK
// owner (vulkan_terrain_island.cpp) cannot read itself. Same values uploadOverlayUniforms_ gives GL.
extern "C" void mc2_vk_overlay_fill_frame(Mc2VkOverlayFrame* out)
{
    if (!out) return;
    *out = Mc2VkOverlayFrame{};
    if (g_gos_renderer) {
        const vec4& fc = g_gos_renderer->getFogColor();
        out->fogColor[0] = fc.x; out->fogColor[1] = fc.y; out->fogColor[2] = fc.z; out->fogColor[3] = fc.w;
    }
    out->artFlatExempt = 0.0f;   // stock flat-pad multiply (the RUNWAY-CLOSE-1 exemption arm is gone; the push-constant slot stays)
    gosPostProcess* pp = getGosPostProcess();
    out->mapHalfExtent = pp ? pp->getMapHalfExtent() : 0.0f;
    out->biasLegacy = mc2OverlayBiasLegacy();
    static const int s_cementAdopt = mc2::gate::get<"MC2_OVERLAY_CEMENT_ADOPT">();
    static const int s_seamFix = mc2::gate::get<"MC2_CEMENT_ATLAS_SEAM_FIX">();
    out->cementAdopt = s_cementAdopt;
    out->seamFix = s_seamFix;
}
#endif

// Draw the terrain overlay batch (alpha cement perimeter tiles).
// Render state: opaque, depth-write ON, depth-test LEQUAL (same as solid terrain).
// MRT: terrain_overlay.frag writes GBuffer1.alpha=1 → shadow_screen skips these pixels.
// Batch cleared after draw.
// [RENDER_CONTRACT:Pass=TerrainOverlay id=gosRenderer_drawTerrainOverlays]
void gosRenderer::drawTerrainOverlays()
{
#ifdef MC2_VULKAN_ISLAND
    if (!terrainOverlayBatch_.draws.empty() && mc2_vk_overlay_enabled()) {
        static_assert(sizeof(OverlayBatchEntry_) == sizeof(Mc2VkOverlayDraw));
        mc2_vk_overlay_submit(0u, terrainOverlayBatch_.verts.data(), static_cast<uint32_t>(terrainOverlayBatch_.verts.size()),
            reinterpret_cast<const Mc2VkOverlayDraw*>(terrainOverlayBatch_.draws.data()),
            static_cast<uint32_t>(terrainOverlayBatch_.draws.size()), 0);
    }
#endif
    terrainOverlayBatch_.verts.clear();
    terrainOverlayBatch_.draws.clear();
}

// Static overlay batches submit their retained CPU bake directly to Vulkan.
bool gosRenderer::drawDecalStaticBatch(unsigned int vboGL,
                                       const struct GosDecalStaticDraw* draws,
                                       int drawCount)
{
    // Static overlays submit their retained CPU bake directly to Vulkan.
    return false;
}

// Draw the decal batch (bomb craters + mech footprints).
// Render state: alpha blend, depth-write OFF, depth-test LEQUAL.
// Batch cleared after draw.
// [RENDER_CONTRACT:Pass=TerrainDecal id=gosRenderer_drawDecals]
void gosRenderer::drawDecals()
{
#ifdef MC2_VULKAN_ISLAND
    if (!decalBatch_.draws.empty() && mc2_vk_overlay_enabled()) {
        static_assert(sizeof(OverlayBatchEntry_) == sizeof(Mc2VkOverlayDraw));
        mc2_vk_overlay_submit(1u, decalBatch_.verts.data(), static_cast<uint32_t>(decalBatch_.verts.size()),
            reinterpret_cast<const Mc2VkOverlayDraw*>(decalBatch_.draws.data()),
            static_cast<uint32_t>(decalBatch_.draws.size()), 0);
    }
#endif
    decalBatch_.verts.clear();
    decalBatch_.draws.clear();
}

void gosRenderer::executorApplyTerrainDecalState()
{
    // Native overlay pipelines own their render state.
}

void gosRenderer::executorApplyTerrainOverlayState()
{
    // Native overlay pipelines own their render state.
}

// FRAMEGRAPH-APPLY-STATE-EXTEND-1: shared GENERIC-AXES apply helper (declared in
// RenderCore/top_level_apply_axes.h). Applies the OPTIONAL non-pipeline axes a
// TopLevelStateDesc declares, each SKIPPED on its sentinel so the 4 unchanged consumers
// (fboTarget=Unknown / viewport=Inherit / clear=None) are byte-identical no-ops. The
// pipeline is applied by the per-pass fn AFTER this returns. Cross-TU shared so the
// upcoming Shadow slice reuses it (FBO + ShadowMap viewport + DepthForwardZ clear).
namespace RenderCore { namespace framegraph {
void applyTopLevelGenericAxes(const TopLevelStateDesc& d, unsigned fbo, int vpW, int vpH) {
    // 1. FBO bind — skip on Unknown (body inherits the bound FBO).
    if (d.fboTarget != RenderResourceId::Unknown) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    }
    // 2. Viewport — skip on Inherit (body inherits the active viewport).
    if (d.viewport != ViewportKind::Inherit) {
        glViewport(0, 0, vpW, vpH);
    }
    // 3. Depth clear — skip on None. DepthForwardZ = forward-Z clear; the scene runs reverse-Z
    //    (clear-to-0) by default, so we clear to 1 then RESTORE clear-depth to 0 to leave global
    //    GL clear-depth state unchanged. NOT used this slice (StaticProp.clear == None); present
    //    for the Shadow slice (forward-Z shadow map).
    if (d.clear == ClearSpec::DepthForwardZ) {
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
        glClearDepth(0.0);
    }
}
}} // namespace RenderCore::framegraph


void __stdcall gos_PushTerrainOverlay(const WorldOverlayVert* verts3, unsigned int texHandle) {
    // PR2b Stage 0b probe — counts every entry into the producer regardless
    // of which call site invoked. Diagnoses whether the overlay-via-quad-walk
    // pipeline is live anywhere in tier1.
    gos_terrain_indirect::Counters_AddGosPushOverlayCall();
    if (g_gos_renderer && verts3) g_gos_renderer->pushTerrainOverlayTri(verts3, texHandle);
}

void __stdcall gos_PushDecal(const WorldOverlayVert* verts3, unsigned int texHandle) {
    if (g_gos_renderer && verts3) g_gos_renderer->pushDecalTri(verts3, texHandle);
}

void __stdcall gos_DrawTerrainOverlays() {
    if (g_gos_renderer) g_gos_renderer->drawTerrainOverlays();
}

void __stdcall gos_DrawDecals() {
    if (g_gos_renderer) g_gos_renderer->drawDecals();
}
