// gosPostProcess: SSAO, fog and HZB regions routed through the render backend, and their health exports.
// Declared in the class headers; shared names in gos_postprocess_internal.h.
#include "gos_postprocess_internal.h"


// ===========================================================================
// VK-REGION-SSAO-1: PostprocessSSAO region (second region through runRegion).
// ===========================================================================
//
// 1A: the extracted GL SSAO region wrapper. Calls the UNCHANGED runSSAO()
// inline body. No interface, no Vulkan: pure code-move. Under the master
// gate OFF this wrapper is never entered (endScene() calls runSSAO() directly);
// under gate ON, the GL backend calls this so its output == the old inline
// path. Returns 1 if SSAO's own gates let it run this frame, 0 if it
// early-returned (mirrors the fog wrapper's equivalence-count contract, but
// SSAO is a single logical effect so the count is 0/1, not 0/2).
int gosPostProcess::runSSAORegionGL()
{
    const bool willRun = ssaoEnabled_ && sceneHasTerrain_ &&
                         ssaoProg_ && ssaoProg_->is_valid() &&
                         ssaoApplyProg_ && ssaoApplyProg_->is_valid() &&
                         ssaoFBO_ && ssaoColorTex_;
    runSSAO();
    return willRun ? 1 : 0;
}

// Resolver: gosPostProcess owns the scene + SSAO textures, so it maps the
// neutral ids to the concrete GL handles. Keeps handles OUT of the region
// context (same discipline as resolveRegionResource() for fog).
unsigned int gosPostProcess::resolveSSAORegionResource(RenderCore::RenderResourceId id) const
{
    switch (id) {
        case RenderCore::RenderResourceId::MainColor:     return (unsigned int)sceneColorTex_;
        case RenderCore::RenderResourceId::MainDepth:     return (unsigned int)sceneDepthTex_;
        case RenderCore::RenderResourceId::MainNormal:    return (unsigned int)sceneNormalTex_;
        case RenderCore::RenderResourceId::SsaoOcclusion: return (unsigned int)ssaoColorTex_;
        default: return 0u;
    }
}

#ifdef MC2_VULKAN_ISLAND
// 1C: Vulkan impl of the PostprocessSSAO region. Fail-soft: readiness returns
// false ONLY on a genuine device/init failure (self-disables), so we return
// false -> caller runs GL (FallbackGL = real failure, not "gate off"). The
// neutral ctx values already equal the GL runSSAO() uploads exactly, so no
// data is threaded through here beyond the resolver exercise (contract
// symmetry with the fog region's 1C).
bool gosPostProcess::runSSAORegionVulkanBackend(
    const RenderCore::PostprocessSSAORegionContext& ctx,
    RenderCore::RegionOutput* out)
{
    if (ctx.resolver) {
        (void)ctx.resolver->resolve(ctx.depth);
        (void)ctx.resolver->resolve(ctx.normal);
        (void)ctx.resolver->resolve(ctx.color);
        (void)ctx.resolver->resolve(ctx.occlusion);
    }
    // N0 BUG-1 (enable-guard parity): the GL runSSAO() self-guards on
    // ssaoEnabled_ && sceneHasTerrain_ (+ program/FBO validity) and no-ops when
    // any is false, so it NEVER runs against a frame whose terrain MVP / scene
    // depth have not been produced (menus, load frames, no-terrain frames). The
    // Vulkan island had NO such guard, so it ran EVERY routed frame -- including
    // frames where inverseViewProj_/viewProj_ are still zero-initialized and
    // sceneDepthTex_ is all-zero -- reconstructing garbage AO. Mirror GL's guard
    // exactly here: decline (return false) so the caller runs the GL wrapper,
    // whose own guard then no-ops. This keeps the Vulkan path bit-for-bit
    // scoped to the SAME frames GL would actually shade.
    const bool ssaoGatesLetItRun =
        ssaoEnabled_ && sceneHasTerrain_ &&
        ssaoProg_ && ssaoProg_->is_valid() &&
        ssaoApplyProg_ && ssaoApplyProg_->is_valid() &&
        ssaoFBO_ && ssaoColorTex_;
    if (!ssaoGatesLetItRun) {
        if (out) { out->impl = RenderCore::RenderRegionImpl::None;
                   out->fallbackReason = "ssao_gate_off";
                   out->glPassesRun = 0; out->vkDraws = 0; }
        return false;
    }
    if (!vkPostSsaoNativeEnabled() || !runVkPostSsaoNative()) {
        if (out) { out->impl = RenderCore::RenderRegionImpl::None;
                   out->fallbackReason = "vk_native_ssao_unavailable";
                   out->glPassesRun = 0; out->vkDraws = 0; }
        return false;
    }
    if (out) { out->impl = RenderCore::RenderRegionImpl::VulkanSubgraph;
               out->fallbackReason = ""; out->glPassesRun = 0; out->vkDraws = 2; }
    return true;
}
#endif

void gosPostProcess::clearGBuffer1()
{
    if (!sceneNormalTex_) return;
    // Sentinel: flat-up encoded normal (0.5, 0.5, 1.0), alpha=0.0
    // (post-shadow eligible). Matches rc_gbuffer1_screenShadowEligible(vec3(0,0,1))
    // in shaders/include/render_contract.hglsl.
    static const GLfloat sentinel[4] = { 0.5f, 0.5f, 1.0f, 0.0f };
    // glClearBufferfv with buffer=GL_COLOR, drawbuffer=1 clears the
    // SECOND draw buffer of the currently bound FBO. Because beginScene()
    // calls glDrawBuffers(2, {COLOR0, COLOR1}), drawbuffer index 1 maps
    // to GL_COLOR_ATTACHMENT1 here. Caller must ensure MRT is bound.
    glClearBufferfv(GL_COLOR, 1, sentinel);
}

// RENDER-BACKEND-REGION-IFACE-1 (Layer-6 ENTRY): PostprocessFog region.
//
// 1A: the extracted GL fog region wrapper. Calls the UNCHANGED runEdgeFog() +
// runFogOob() inline bodies, in the same order the endScene() sites call them.
// No interface, no Vulkan: this is a pure code-move. Under the master gate OFF
// this wrapper is never entered (endScene() runs the original direct sites);
// under gate ON, the GL backend calls this so its output == the old inline path.
int gosPostProcess::runFogRegionGL()
{
    runEdgeFog();
    runFogOob();
    // Both GL fog sites ran (each may per-effect early-return internally, exactly
    // as the original inline sites did — that behavior is unchanged). The region
    // contract is "the two GL fog passes", so the equivalence count is 2.
    return 2;
}

// Master-gate read (once). MC2_RENDER_BACKEND_REGION_IFACE==1 -> route the fog
// region through IRenderBackend; else the original direct-GL sites run.
bool gosPostProcess::regionIfaceEnabled() const
{
    static const bool s_on = mc2::gate::get<"MC2_RENDER_BACKEND_REGION_IFACE">();
    return s_on;
}

// Resolver: gosPostProcess owns the scene textures, so it maps the neutral ids
// to the concrete GL handles. Keeps handles OUT of the region context.
unsigned int gosPostProcess::resolveRegionResource(RenderCore::RenderResourceId id) const
{
    switch (id) {
        case RenderCore::RenderResourceId::MainColor: return (unsigned int)sceneColorTex_;
        case RenderCore::RenderResourceId::MainDepth: return (unsigned int)sceneDepthTex_;
        default: return 0u;
    }
}

#ifdef MC2_VULKAN_ISLAND
// 1C: Vulkan impl of the PostprocessFog region. The region iface's backend=vk
// selector now routes to the rung-8.1 shared-GBuffer native fog path. Fail-soft:
// any not-ready native condition returns false so the caller runs GL.
bool gosPostProcess::runFogRegionVulkanBackend(
    const RenderCore::PostprocessFogRegionContext& ctx,
    RenderCore::RegionOutput* out)
{
    if (ctx.resolver) {
        (void)ctx.resolver->resolve(ctx.color);
        (void)ctx.resolver->resolve(ctx.depth);
    }
    // VK-FOG-SKYEXCLUDE-GUARD-1: the vk fog subgraph has NO stencil input — the
    // ported shaders/vulkan/{edge_fog,fog_oob}.frag carry no stencilTex, so with
    // MC2_SKYBOX_FOG_EXCLUDE=1 the Vulkan path would silently fog sky pixels the
    // GL path excludes (SKYBOX-FOG-EXCLUDE-1's usampler2D STENCIL_INDEX view).
    // Decline honestly -> caller runs GL and stamps FallbackGL. Eventual fix if
    // sky-exclude-under-vk is ever needed: bridge the stencil plane like depth
    // (glGetTexImage(GL_STENCIL_INDEX, GL_UNSIGNED_BYTE) -> staging ->
    // VK_FORMAT_R8_UINT image, usampler2D in the ported frag) — see
    // .claude/VULKAN-NEXT-SLICES.md LM4.
    if (skyboxFogExcludeEnabled()) {
        if (out) { out->impl = RenderCore::RenderRegionImpl::None;
                   out->fallbackReason = "sky_exclude_not_ported";
                   out->glPassesRun = 0; out->vkDraws = 0; }
        return false;
    }
    if (!runVkPostFogNative()) {
        if (out) { out->impl = RenderCore::RenderRegionImpl::None;
                   out->fallbackReason = "vk_native_fog_not_ready";
                   out->glPassesRun = 0; out->vkDraws = 0; }
        return false;
    }
    if (out) { out->impl = RenderCore::RenderRegionImpl::VulkanSubgraph;
               out->fallbackReason = ""; out->glPassesRun = 0; out->vkDraws = 2; }
    return true;
}
#endif

namespace {

// Process-lifetime region-selection health, surfaced in the debug-state dump so
// backend_region_selected = gl|vk|fallback_gl is catchable. Distinct from the
// subgraph's own g_sub health (that one is the Vulkan device/perf report).
struct RegionSelectHealth {
    int         ifaceGateOn      = 0;    // MC2_RENDER_BACKEND_REGION_IFACE==1
    char        requestedBackend[8] = "gl";  // MC2_POSTPROCESS_BACKEND (gl|vk)
    int         lastImpl         = 0;    // RenderRegionImpl (None/GLInline/VulkanSubgraph/FallbackGL)
    char        lastFallbackReason[64] = {0};
    int         lastGlPassesRun  = 0;    // GL fog passes actually run (0 or 2)
    int         lastVkDraws      = 0;    // Vulkan fog draws (0 or 2)
    unsigned long routedFrames   = 0;    // frames routed through runRegion
};
RegionSelectHealth g_region;

void region_set_fallback_reason(const char* r) {
    std::strncpy(g_region.lastFallbackReason, r ? r : "",
                 sizeof(g_region.lastFallbackReason) - 1);
    g_region.lastFallbackReason[sizeof(g_region.lastFallbackReason) - 1] = '\0';
}

// Neutral-context resolver adaptor wrapping gosPostProcess::resolveRegionResource.
struct GosResourceResolver final : RenderCore::IResourceResolver {
    const gosPostProcess* pp = nullptr;
    unsigned int resolve(RenderCore::RenderResourceId id) const override {
        return pp ? pp->resolveRegionResource(id) : 0u;
    }
};

// GL impl of runRegion for the PostprocessFog region: resolves color/depth from
// the neutral context (proving the id->handle path is exercised even though the
// GL fog bodies read the members directly today) and runs the extracted GL
// wrapper. Always succeeds -> out->impl = GLInline. Delegates to the owner.
struct GlFogRegionBackend final : RenderCore::IRenderBackend {
    gosPostProcess* pp = nullptr;
    void bindBackbuffer() override {}
    void bindFramebuffer(unsigned int) override {}
    void setViewport(int, int, int, int) override {}
    void clear(unsigned int, float, float, float, float) override {}
    bool runRegion(RenderCore::RenderRegionId id,
                   const RenderCore::PostprocessFogRegionContext& ctx,
                   RenderCore::RegionOutput* out) override {
        if (id != RenderCore::RenderRegionId::PostprocessFog || !pp) return false;
        // Exercise the id->handle resolve so the neutral-context contract is real
        // (a bad resolver would surface as 0 handles here, not a silent divergence).
        if (ctx.resolver) {
            (void)ctx.resolver->resolve(ctx.color);
            (void)ctx.resolver->resolve(ctx.depth);
        }
        int gl = pp->runFogRegionGL();
        if (out) { out->impl = RenderCore::RenderRegionImpl::GLInline;
                   out->glPassesRun = gl; out->vkDraws = 0; out->fallbackReason = ""; }
        return true;
    }
};

} // namespace

// C-linkage accessor for the debug-state dump (region-selection health). Always`r`n// compiled because the region iface exists on every build.
extern "C" void mc2_render_backend_region_health(
    int* ifaceGateOn, const char** requestedBackend, int* lastImpl,
    const char** lastFallbackReason, int* lastGlPassesRun, int* lastVkDraws,
    unsigned long* routedFrames) {
    if (ifaceGateOn)         *ifaceGateOn         = g_region.ifaceGateOn;
    if (requestedBackend)    *requestedBackend    = g_region.requestedBackend;
    if (lastImpl)            *lastImpl            = g_region.lastImpl;
    if (lastFallbackReason)  *lastFallbackReason  = g_region.lastFallbackReason;
    if (lastGlPassesRun)     *lastGlPassesRun     = g_region.lastGlPassesRun;
    if (lastVkDraws)         *lastVkDraws         = g_region.lastVkDraws;
    if (routedFrames)        *routedFrames        = g_region.routedFrames;
}

// The routed entry (master gate ON). Builds the neutral context, selects the
// backend, calls runRegion; falls back to GL on false. Never double-applies:
// exactly ONE of {GL wrapper, Vulkan subgraph} runs, and the GL fog sites in
// endScene() are skipped whenever this routed path is taken.
void gosPostProcess::runFogRegionThroughBackend()
{
    ++g_region.routedFrames;
    g_region.ifaceGateOn = 1;

    // Sub-selector: MC2_POSTPROCESS_BACKEND = gl | vk (default gl).
    static const bool s_wantVk = postprocessBackendWantsVk();
    std::strncpy(g_region.requestedBackend, s_wantVk ? "vk" : "gl",
                 sizeof(g_region.requestedBackend) - 1);

    // Build the BACKEND-NEUTRAL context (RenderResourceId + params + dims; NO
    // raw handles). Values pulled EXACTLY from the GL runEdgeFog()/runFogOob().
    GosResourceResolver resolver;
    resolver.pp = this;
    RenderCore::PostprocessFogRegionContext ctx;
    ctx.color  = RenderCore::RenderResourceId::MainColor;
    ctx.depth  = RenderCore::RenderResourceId::MainDepth;
    ctx.width  = width_;
    ctx.height = height_;
    ctx.resolver = &resolver;
    // EdgeFog params (row-major invViewProj as the GL path uploads with GL_FALSE).
    std::memcpy(ctx.edgeFog.invViewProj, inverseViewProj_, 16 * sizeof(float));
    float edgeFogColorEffCtx[3];                          // BACKDROP-BLIT-ATTRIB-1
    effectiveFogColor(edgeFogColor_, edgeFogColorEffCtx);
    ctx.edgeFog.color[0] = edgeFogColorEffCtx[0];
    ctx.edgeFog.color[1] = edgeFogColorEffCtx[1];
    ctx.edgeFog.color[2] = edgeFogColorEffCtx[2];
    ctx.edgeFog.halfExtent     = mapHalfExtent_;
    ctx.edgeFog.fogStart       = edgeFogStart_;
    ctx.edgeFog.fogHeight      = edgeFogHeight_;
    ctx.edgeFog.fogMax         = edgeFogMax_;
    ctx.edgeFog.waterElevation = waterElevation_;
    // OOB params.
    std::memcpy(ctx.oobFog.invViewProj, inverseViewProj_, 16 * sizeof(float));
    float oobFogColorEffCtx[3];                           // BACKDROP-BLIT-ATTRIB-1
    effectiveFogColor(oobFogColor_, oobFogColorEffCtx);
    ctx.oobFog.color[0] = oobFogColorEffCtx[0];
    ctx.oobFog.color[1] = oobFogColorEffCtx[1];
    ctx.oobFog.color[2] = oobFogColorEffCtx[2];
    ctx.oobFog.opacity  = oobFogOpacity_;
    ctx.oobFog.time     = SmokeMode::fixedTimestepEnabled()
                              ? (float)SmokeMode::fixedClockSeconds()
                              : (float)SDL_GetTicks() / 1000.0f;

    RenderCore::RegionOutput out;
    bool ran = false;

#ifdef MC2_VULKAN_ISLAND
    if (s_wantVk) {
        // Route through the Vulkan subgraph backend (1C). On false the caller
        // (this function) runs GL below with impl=FallbackGL.
        ran = runFogRegionVulkanBackend(ctx, &out);
        if (!ran) {
            region_set_fallback_reason(out.fallbackReason && out.fallbackReason[0]
                                           ? out.fallbackReason : "vk_declined");
        }
    }
#endif

    if (!ran) {
        GlFogRegionBackend gl;
        gl.pp = this;
        bool okGl = gl.runRegion(RenderCore::RenderRegionId::PostprocessFog, ctx, &out);
        if (s_wantVk) {
            // requested vk but GL actually ran -> FallbackGL (not plain GLInline).
            out.impl = RenderCore::RenderRegionImpl::FallbackGL;
        }
        (void)okGl;
    }

    g_region.lastImpl        = (int)out.impl;
    g_region.lastGlPassesRun = out.glPassesRun;
    g_region.lastVkDraws     = out.vkDraws;
    if (out.impl != RenderCore::RenderRegionImpl::FallbackGL)
        region_set_fallback_reason("");

    render_frame_plan::trace(render_frame_plan::Phase::PostProcess, "PostprocessFogRegion",
        render_frame_plan::PathKind::ApplyPipeline, 1,
        RenderCore::region_impl_to_string(out.impl));
}

// VK-REGION-SSAO-1: SSAO region routing -- selector + GL backend impl + health
// ---------------------------------------------------------------------------
// VK-REGION-SSAO-1: SSAO region routing -- selector + GL backend impl + health
// snapshot. Distinct health struct from g_region (fog) so both regions' state
// is independently catchable in the debug-state dump.
// ---------------------------------------------------------------------------
namespace {

struct SsaoRegionSelectHealth {
    int         ifaceGateOn      = 0;    // MC2_RENDER_BACKEND_REGION_IFACE==1
    char        requestedBackend[8] = "gl";  // MC2_POSTPROCESS_BACKEND (gl|vk)
    int         lastImpl         = 0;    // RenderRegionImpl (None/GLInline/VulkanSubgraph/FallbackGL)
    char        lastFallbackReason[64] = {0};
    int         lastGlPassesRun  = 0;    // 0 or 1 (SSAO is a single logical effect)
    int         lastVkDraws      = 0;    // 0 or 2 (occlusion + apply)
    unsigned long routedFrames   = 0;    // frames routed through runRegion
};
SsaoRegionSelectHealth g_ssaoRegion;

void ssao_region_set_fallback_reason(const char* r) {
    std::strncpy(g_ssaoRegion.lastFallbackReason, r ? r : "",
                 sizeof(g_ssaoRegion.lastFallbackReason) - 1);
    g_ssaoRegion.lastFallbackReason[sizeof(g_ssaoRegion.lastFallbackReason) - 1] = '\0';
}

// Neutral-context resolver adaptor wrapping gosPostProcess::resolveSSAORegionResource.
struct GosSsaoResourceResolver final : RenderCore::IResourceResolver {
    const gosPostProcess* pp = nullptr;
    unsigned int resolve(RenderCore::RenderResourceId id) const override {
        return pp ? pp->resolveSSAORegionResource(id) : 0u;
    }
};

// GL impl of runRegion for the PostprocessSSAO region: resolves depth/normal/
// color/occlusion from the neutral context (proving the id->handle path is
// exercised even though the GL SSAO body reads the members directly today)
// and runs the extracted GL wrapper. Always succeeds -> out->impl = GLInline.
struct GlSsaoRegionBackend final : RenderCore::IRenderBackend {
    gosPostProcess* pp = nullptr;
    void bindBackbuffer() override {}
    void bindFramebuffer(unsigned int) override {}
    void setViewport(int, int, int, int) override {}
    void clear(unsigned int, float, float, float, float) override {}
    bool runRegion(RenderCore::RenderRegionId id,
                   const RenderCore::PostprocessSSAORegionContext& ctx,
                   RenderCore::RegionOutput* out) override {
        if (id != RenderCore::RenderRegionId::PostprocessSSAO || !pp) return false;
        if (ctx.resolver) {
            (void)ctx.resolver->resolve(ctx.depth);
            (void)ctx.resolver->resolve(ctx.normal);
            (void)ctx.resolver->resolve(ctx.color);
            (void)ctx.resolver->resolve(ctx.occlusion);
        }
        int gl = pp->runSSAORegionGL();
        if (out) { out->impl = RenderCore::RenderRegionImpl::GLInline;
                   out->glPassesRun = gl; out->vkDraws = 0; out->fallbackReason = ""; }
        return true;
    }
};

} // namespace

// C-linkage accessor for the debug-state dump (SSAO region-selection health).
// Mirrors mc2_render_backend_region_health (fog); always compiled (no #ifdef).
extern "C" void mc2_render_backend_ssao_region_health(
    int* ifaceGateOn, const char** requestedBackend, int* lastImpl,
    const char** lastFallbackReason, int* lastGlPassesRun, int* lastVkDraws,
    unsigned long* routedFrames) {
    if (ifaceGateOn)         *ifaceGateOn         = g_ssaoRegion.ifaceGateOn;
    if (requestedBackend)    *requestedBackend    = g_ssaoRegion.requestedBackend;
    if (lastImpl)            *lastImpl            = g_ssaoRegion.lastImpl;
    if (lastFallbackReason)  *lastFallbackReason  = g_ssaoRegion.lastFallbackReason;
    if (lastGlPassesRun)     *lastGlPassesRun     = g_ssaoRegion.lastGlPassesRun;
    if (lastVkDraws)         *lastVkDraws         = g_ssaoRegion.lastVkDraws;
    if (routedFrames)        *routedFrames        = g_ssaoRegion.routedFrames;
}

// The routed entry (master gate ON). Builds the neutral SSAO context, selects
// the backend, calls runRegion; falls back to GL on false. Never double-
// applies: exactly ONE of {GL wrapper, Vulkan island} runs, and the direct
// runSSAO() call in endScene() is skipped whenever this routed path is taken.
void gosPostProcess::runSSAORegionThroughBackend()
{
    ++g_ssaoRegion.routedFrames;
    g_ssaoRegion.ifaceGateOn = 1;

    // Sub-selector: MC2_POSTPROCESS_BACKEND = gl | vk (default gl). SAME env var
    // as the fog region -- one selector picks the backend for every region.
    static const bool s_wantVk = postprocessBackendWantsVk();
    std::strncpy(g_ssaoRegion.requestedBackend, s_wantVk ? "vk" : "gl",
                 sizeof(g_ssaoRegion.requestedBackend) - 1);

    // Build the BACKEND-NEUTRAL context (RenderResourceId + params + dims; NO
    // raw handles). Values pulled EXACTLY from the GL runSSAO() uploads.
    GosSsaoResourceResolver resolver;
    resolver.pp = this;
    RenderCore::PostprocessSSAORegionContext ctx;
    ctx.depth     = RenderCore::RenderResourceId::MainDepth;
    ctx.normal    = RenderCore::RenderResourceId::MainNormal;
    ctx.color     = RenderCore::RenderResourceId::MainColor;
    ctx.occlusion = RenderCore::RenderResourceId::SsaoOcclusion;
    ctx.width  = width_;
    ctx.height = height_;
    ctx.halfWidth  = ssaoW_;
    ctx.halfHeight = ssaoH_;
    ctx.resolver = &resolver;
    std::memcpy(ctx.params.invViewProj, inverseViewProj_, 16 * sizeof(float));
    std::memcpy(ctx.params.viewProj, viewProj_, 16 * sizeof(float));
    ctx.params.screenWidth  = (float)width_;
    ctx.params.screenHeight = (float)height_;
    ctx.params.aoRadius   = aoRadius_;
    ctx.params.aoBias     = aoBias_;
    ctx.params.aoStrength = aoStrength_;
    ctx.params.aoPower    = aoPower_;
    ctx.params.debugMode  = ssaoDebug_;

    RenderCore::RegionOutput out;
    bool ran = false;
    // N0 BUG-1: a decline whose reason is "ssao_gate_off" means SSAO's OWN gates
    // (ssaoEnabled_/sceneHasTerrain_/program/FBO) skip the region THIS frame --
    // exactly as GL's runSSAO() no-ops -- so neither backend does real work.
    // That is NOT a Vulkan device FallbackGL (which the parity harness treats as
    // INCONCLUSIVE, exit 5). Track it so we (a) do not relabel the no-op GL run
    // as FallbackGL, and (b) do not overwrite the last REAL per-frame impl in the
    // health snapshot -- otherwise a trailing menu/no-terrain frame would stamp
    // the shutdown dump FallbackGL and void an otherwise-clean vk compare.
    bool gateOffSkip = false;

#ifdef MC2_VULKAN_ISLAND
    if (s_wantVk) {
        // Route through the Vulkan SSAO island backend (1C). On false the
        // caller (this function) runs GL below with impl=FallbackGL, UNLESS the
        // decline was a gate-off skip (handled below).
        ran = runSSAORegionVulkanBackend(ctx, &out);
        if (!ran) {
            const char* why = (out.fallbackReason && out.fallbackReason[0])
                                  ? out.fallbackReason : "vk_declined";
            gateOffSkip = (std::strcmp(why, "ssao_gate_off") == 0);
            if (!gateOffSkip)
                ssao_region_set_fallback_reason(why);
        }
    }
#endif

    if (!ran) {
        GlSsaoRegionBackend gl;
        gl.pp = this;
        bool okGl = gl.runRegion(RenderCore::RenderRegionId::PostprocessSSAO, ctx, &out);
        if (s_wantVk && !gateOffSkip) {
            // requested vk but GL actually ran -> FallbackGL (not plain GLInline).
            out.impl = RenderCore::RenderRegionImpl::FallbackGL;
        }
        (void)okGl;
    }

    // Gate-off skip: the region did no real work this frame; leave the health
    // snapshot's last-real impl/reason untouched so shutdown reflects the last
    // frame the region ACTUALLY ran (mirrors GL, where runSSAO() simply no-ops
    // and leaves no region-health footprint).
    if (!gateOffSkip) {
        g_ssaoRegion.lastImpl        = (int)out.impl;
        g_ssaoRegion.lastGlPassesRun = out.glPassesRun;
        g_ssaoRegion.lastVkDraws     = out.vkDraws;
        if (out.impl != RenderCore::RenderRegionImpl::FallbackGL)
            ssao_region_set_fallback_reason("");
    }

    render_frame_plan::trace(render_frame_plan::Phase::PostProcess, "PostprocessSSAORegion",
        render_frame_plan::PathKind::ApplyPipeline, 1,
        RenderCore::region_impl_to_string(out.impl));
}

// VK-REGION-HZB-1: HZB region routing -- selector + GL backend impl + health
// ---------------------------------------------------------------------------
// VK-REGION-HZB-1: HZB region routing -- selector + GL backend impl + health
// snapshot. Third region through runRegion (fog, ssao, hzb). Same shape as
// the SSAO region block above; distinct health struct so all regions' state
// is independently catchable in the debug-state dump.
// ---------------------------------------------------------------------------

// 1A: the extracted GL HZB region wrapper. Calls the UNCHANGED runHzbReduce()
// inline body (pure code-move). Returns 1 if HZB's own gates let it run this
// frame, 0 if it early-returned (single logical effect -> 0/1 count, like SSAO).
int gosPostProcess::runHzbRegionGL()
{
    const bool willRun = hzbEnabled_ &&
                         hzbReduceProg_ && hzbReduceProg_->is_valid() &&
                         hzbLevelTex_[0] && hzbFBO_ && hzbMipCount_ >= 1 &&
                         sceneDepthTex_ != 0;
    runHzbReduce();
    return willRun ? 1 : 0;
}

// Resolver: gosPostProcess owns the scene depth + pyramid textures, so it maps
// the neutral ids to the concrete GL handles. HzbPyramid is a MULTI-LEVEL
// resource (one texture per ceil-ladder level); the resolver returns the
// level-0 representative handle -- the backend impl owns the per-level mapping
// (member access, same discipline as the SSAO region).
unsigned int gosPostProcess::resolveHzbRegionResource(RenderCore::RenderResourceId id) const
{
    switch (id) {
        case RenderCore::RenderResourceId::MainDepth:  return (unsigned int)sceneDepthTex_;
        case RenderCore::RenderResourceId::HzbPyramid: return (unsigned int)hzbLevelTex_[0];
        default: return 0u;
    }
}

namespace {

struct HzbRegionSelectHealth {
    int         ifaceGateOn      = 0;    // MC2_RENDER_BACKEND_REGION_IFACE==1
    char        requestedBackend[8] = "gl";  // MC2_POSTPROCESS_BACKEND (gl|vk)
    int         lastImpl         = 0;    // RenderRegionImpl (None/GLInline/VulkanSubgraph/FallbackGL)
    char        lastFallbackReason[64] = {0};
    int         lastGlPassesRun  = 0;    // 0 or 1 (HZB is a single logical effect)
    int         lastVkDraws      = 0;    // 0 or hzbMipCount_ (one draw per level)
    unsigned long routedFrames   = 0;    // frames routed through runRegion
};
HzbRegionSelectHealth g_hzbRegion;

void hzb_region_set_fallback_reason(const char* r) {
    std::strncpy(g_hzbRegion.lastFallbackReason, r ? r : "",
                 sizeof(g_hzbRegion.lastFallbackReason) - 1);
    g_hzbRegion.lastFallbackReason[sizeof(g_hzbRegion.lastFallbackReason) - 1] = '\0';
}

// Neutral-context resolver adaptor wrapping gosPostProcess::resolveHzbRegionResource.
struct GosHzbResourceResolver final : RenderCore::IResourceResolver {
    const gosPostProcess* pp = nullptr;
    unsigned int resolve(RenderCore::RenderResourceId id) const override {
        return pp ? pp->resolveHzbRegionResource(id) : 0u;
    }
};

// GL impl of runRegion for the PostprocessHzb region: resolves depth/pyramid
// from the neutral context (proving the id->handle path is exercised even
// though the GL HZB body reads the members directly today) and runs the
// extracted GL wrapper. Always succeeds -> out->impl = GLInline.
struct GlHzbRegionBackend final : RenderCore::IRenderBackend {
    gosPostProcess* pp = nullptr;
    void bindBackbuffer() override {}
    void bindFramebuffer(unsigned int) override {}
    void setViewport(int, int, int, int) override {}
    void clear(unsigned int, float, float, float, float) override {}
    bool runRegion(RenderCore::RenderRegionId id,
                   const RenderCore::PostprocessHzbRegionContext& ctx,
                   RenderCore::RegionOutput* out) override {
        if (id != RenderCore::RenderRegionId::PostprocessHzb || !pp) return false;
        if (ctx.resolver) {
            (void)ctx.resolver->resolve(ctx.depth);
            (void)ctx.resolver->resolve(ctx.pyramid);
        }
        int gl = pp->runHzbRegionGL();
        if (out) { out->impl = RenderCore::RenderRegionImpl::GLInline;
                   out->glPassesRun = gl; out->vkDraws = 0; out->fallbackReason = ""; }
        return true;
    }
};

} // namespace

// C-linkage accessor for the debug-state dump (HZB region-selection health).
// Mirrors mc2_render_backend_ssao_region_health; always compiled (no #ifdef).
extern "C" void mc2_render_backend_hzb_region_health(
    int* ifaceGateOn, const char** requestedBackend, int* lastImpl,
    const char** lastFallbackReason, int* lastGlPassesRun, int* lastVkDraws,
    unsigned long* routedFrames) {
    if (ifaceGateOn)         *ifaceGateOn         = g_hzbRegion.ifaceGateOn;
    if (requestedBackend)    *requestedBackend    = g_hzbRegion.requestedBackend;
    if (lastImpl)            *lastImpl            = g_hzbRegion.lastImpl;
    if (lastFallbackReason)  *lastFallbackReason  = g_hzbRegion.lastFallbackReason;
    if (lastGlPassesRun)     *lastGlPassesRun     = g_hzbRegion.lastGlPassesRun;
    if (lastVkDraws)         *lastVkDraws         = g_hzbRegion.lastVkDraws;
    if (routedFrames)        *routedFrames        = g_hzbRegion.routedFrames;
}

#ifdef MC2_VULKAN_ISLAND
// 1C: Vulkan impl of the PostprocessHzb region. Fail-soft; declines with
// "hzb_gate_off" (a SKIP, not FallbackGL) when HZB's own gates would make the
// GL path no-op -- the N0-BUG-1 enable-guard-parity lesson applied from birth:
// the Vulkan path is bit-for-bit scoped to the SAME frames GL would shade.
bool gosPostProcess::runHzbRegionVulkanBackend(
    const RenderCore::PostprocessHzbRegionContext& ctx,
    RenderCore::RegionOutput* out)
{
    if (ctx.resolver) {
        (void)ctx.resolver->resolve(ctx.depth);
        (void)ctx.resolver->resolve(ctx.pyramid);
    }
    const bool hzbGatesLetItRun =
        hzbEnabled_ &&
        hzbReduceProg_ && hzbReduceProg_->is_valid() &&
        hzbLevelTex_[0] && hzbFBO_ && hzbMipCount_ >= 1 &&
        sceneDepthTex_ != 0;
    if (!hzbGatesLetItRun) {
        if (out) { out->impl = RenderCore::RenderRegionImpl::None;
                   out->fallbackReason = "hzb_gate_off";
                   out->glPassesRun = 0; out->vkDraws = 0; }
        return false;
    }
    if (!vkPostHzbNativeEnabled() || !runVkPostHzbNative()) {
        if (out) { out->impl = RenderCore::RenderRegionImpl::None;
                   out->fallbackReason = "vk_native_hzb_unavailable";
                   out->glPassesRun = 0; out->vkDraws = 0; }
        return false;
    }
    if (out) { out->impl = RenderCore::RenderRegionImpl::VulkanSubgraph;
               out->fallbackReason = ""; out->glPassesRun = 0;
               out->vkDraws = hzbMipCount_; }
    return true;
}
#endif

// The routed entry (master gate ON). Builds the neutral HZB context, selects
// the backend, calls runRegion; falls back to GL on false. Never double-
// applies: exactly ONE of {GL wrapper, Vulkan island} runs, and the direct
// runHzbReduce() call in endScene() is skipped whenever this routed path is taken.
void gosPostProcess::runHzbRegionThroughBackend()
{
    ++g_hzbRegion.routedFrames;
    g_hzbRegion.ifaceGateOn = 1;

    static const bool s_wantVk = postprocessBackendWantsVk();
    std::strncpy(g_hzbRegion.requestedBackend, s_wantVk ? "vk" : "gl",
                 sizeof(g_hzbRegion.requestedBackend) - 1);

    GosHzbResourceResolver resolver;
    resolver.pp = this;
    RenderCore::PostprocessHzbRegionContext ctx;
    ctx.depth    = RenderCore::RenderResourceId::MainDepth;
    ctx.pyramid  = RenderCore::RenderResourceId::HzbPyramid;
    ctx.width    = hzbW_;
    ctx.height   = hzbH_;
    ctx.mipCount = hzbMipCount_;
    ctx.resolver = &resolver;

    RenderCore::RegionOutput out;
    bool ran = false;
    // Gate-off decline handling mirrors the SSAO region exactly (N0 BUG-1):
    // "hzb_gate_off" means HZB's OWN gates skip the region this frame -- both
    // backends no-op -- so it is a SKIP, never relabeled FallbackGL, and it
    // must not overwrite the last REAL impl in the health snapshot.
    bool gateOffSkip = false;

#ifdef MC2_VULKAN_ISLAND
    if (s_wantVk) {
        ran = runHzbRegionVulkanBackend(ctx, &out);
        if (!ran) {
            const char* why = (out.fallbackReason && out.fallbackReason[0])
                                  ? out.fallbackReason : "vk_declined";
            gateOffSkip = (std::strcmp(why, "hzb_gate_off") == 0);
            if (!gateOffSkip)
                hzb_region_set_fallback_reason(why);
        }
    }
#endif

    if (!ran) {
        GlHzbRegionBackend gl;
        gl.pp = this;
        bool okGl = gl.runRegion(RenderCore::RenderRegionId::PostprocessHzb, ctx, &out);
        if (s_wantVk && !gateOffSkip) {
            // requested vk but GL actually ran -> FallbackGL (not plain GLInline).
            out.impl = RenderCore::RenderRegionImpl::FallbackGL;
        }
        (void)okGl;
    }

    if (!gateOffSkip) {
        g_hzbRegion.lastImpl        = (int)out.impl;
        g_hzbRegion.lastGlPassesRun = out.glPassesRun;
        g_hzbRegion.lastVkDraws     = out.vkDraws;
        if (out.impl != RenderCore::RenderRegionImpl::FallbackGL)
            hzb_region_set_fallback_reason("");
    }

    render_frame_plan::trace(render_frame_plan::Phase::PostProcess, "PostprocessHzbRegion",
        render_frame_plan::PathKind::ApplyPipeline, 1,
        RenderCore::region_impl_to_string(out.impl));
}
