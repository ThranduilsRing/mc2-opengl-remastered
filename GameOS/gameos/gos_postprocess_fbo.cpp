// gosPostProcess: gosPostProcess teardown, resize, scene/post FBO creation and destruction, and the particle/VFX scene copies.
// Declared in the class headers; shared names in gos_postprocess_internal.h.
#include "gos_postprocess_internal.h"
#include "mc2fmt.h"


static bool mc2VkSharedSceneStrictEnabled()
{
    static const bool s_on = mc2::gate::get<"MC2_VK_SHARED_SCENE_STRICT">();
    return s_on;
}

// VK-RUNG8-ITEM5 boot-decline canary. With the GL-import half deleted, the shared
// scene is VK-only and arms only under the full native-present stack. If create()
// still fails (VK context can't be born / resource alloc fails), the ONLY safe
// outcome is to decline the ENTIRE VK shared-GBuffer session to pure GL: GL renders
// its own targets, the VK islands fail their resource-readiness and fall to GL, and
// present composites the GL scene. That decline is normally silent (a valid pure-GL
// session); MC2_VK_SHARED_SCENE_STRICT=1 makes it a hard abort so deletion-readiness
// / Metal-gate proofs cannot pass through a hidden shared-scene decline.
void mc2VkSharedSceneStrictFallback(const char* slot, const char* reason)
{
    if (!mc2VkSharedSceneStrictEnabled())
        return;
    mc2::fmt::print_err("[VK_SHARED_SCENE_STRICT] abort slot={} reason={} (VK shared-GBuffer declined to pure GL)\n", mc2::fmt::safe(slot ? slot : "unknown"), mc2::fmt::safe(reason ? reason : "unknown"));
    std::fflush(stderr);
    std::abort();
}

void gosPostProcess::destroy()
{
    if (!initialized_)
        return;

    destroyFBOs();
    destroyFullscreenQuad();

#ifdef MC2_VULKAN_ISLAND
    // VK-POST-FOG-NATIVE-1 (rung 8.1): same teardown for the native fog pass.
    // (destroyFBOs also tears it down on resize; this covers final shutdown —
    // destroy() runs after destroyFBOs, so this is usually a no-op that also
    // prints the health line.)
    destroyVkPostFogNative();
    // VK-POST-SCREENFX-NATIVE-1 (rung 8.3): same teardown for the native
    destroyVkPostScreenFxNative();
    destroyVkPostHzbNative();
#endif

    hdriCpu_.reset();
    hdriReady_ = false;
    hdriEnabled_ = false;

    destroyShadows();
    destroyDynamicShadows();

    s_postProcess = nullptr;
    initialized_ = false;
}


void gosPostProcess::resize(int w, int h)
{
    if (w == width_ && h == height_)
        return;

    width_ = w;
    height_ = h;

    destroyFBOs();
    createFBOs(w, h);
}

void gosPostProcess::createFBOs(int w, int h)
{
    glGenFramebuffers(1, &sceneFBO_);
    RenderCore::framegraph::fboLedger().registerFbo(sceneFBO_, RenderCore::RenderResourceId::MainColor);  // FBO-LEDGER-1
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);

    // Color attachment: RGBA16F
    // TEX-CLASS: render-target -- scene HDR color (MRT0, FBO attach)
    //
    // VK-P11 TIER-1 SHARED SCENE COLOR (gated): when MC2_VK_PROP_COLOR_TIER1 is ON
    // (requires shared depth), back sceneColorTex_ with a VK-built exportable RGBA16F
    // image imported into GL as GL_RGBA16F (byte-compatible with the plain GL scene
    // color format), so the VK prop island can co-write props into this SAME
    // attachment in-place. SINGLE color alloc+attach site. Fail-soft: any interop
    // failure falls through to the plain GL alloc below. Rebuilt on resize
    // (destroyFBOs tears down sharedSceneColor_ too).
    bool sceneColorShared = false;
#ifdef MC2_VULKAN_ISLAND
    // VK-RUNG8-ITEM5: the shared scene is VK-only (no GL import). It arms ONLY under
    // the full native-present stack; otherwise the VK islands would co-write into a
    // VK image that GL present can no longer read, so we decline the whole shared
    // scene to pure GL here (islands then fail their resource-readiness => GL).
    {
        const char* reason = "unset";
        sharedSceneColor_ = mc2SharedSceneColorCreate(MC2_SHARED_SCENE_COLOR0, w, h,
                                                      &reason);
        if (sharedSceneColor_) {
            // The VK islands co-write into the VK image; the GL FBO attachment is a
            // plain-GL placeholder (never GL-rendered, only a valid attach target).
            sceneColorTex_ = createSceneColorPlaceholderTex(w, h, GL_RGBA16F,
                                                            GL_RGBA, GL_FLOAT,
                                                            GL_LINEAR);
            sceneColorShared = true;
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, sceneColorTex_, 0);
            mc2::fmt::print_err("[VK_SHARED_SCENE_COLOR] color active placeholder_tex={:d} {:d}x{:d} (VK-only MRT0)\n", mc2::fmt::as_u32((unsigned)sceneColorTex_), mc2::fmt::as_i32(w), mc2::fmt::as_i32(h));
        } else {
            sharedSceneColor_ = nullptr;
            mc2::fmt::print_err("[VK_SHARED_SCENE_COLOR] color declined reason={} (pure GL RGBA16F)\n", mc2::fmt::safe(reason ? reason : "unknown"));
            mc2VkSharedSceneStrictFallback("color", reason);
        }
    }
#endif
    if (!sceneColorShared) {
        glGenTextures(1, &sceneColorTex_);
        glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneColorTex_, 0);
    }

    // LINEAR-COLOR-AUDIT-1: one-shot runtime color-pipeline probe. Logging only,
    // no GL state change. Gate MC2_LIGHTING_LINEAR_AUDIT=1. Confirms at RUNTIME
    // (not just from source) the facts linear-correctness/PBR work depends on:
    // whether GL_FRAMEBUFFER_SRGB auto-encode is enabled, the scene render-target
    // internal format, and whether the HDR/tonemap post path is engaged.
    if (mc2::gate::get<"MC2_LIGHTING_LINEAR_AUDIT">()) {
        static bool s_linearAudited = false;
        if (!s_linearAudited) {
            s_linearAudited = true;
            GLint sceneFmt = 0;  // sceneColorTex_ is still bound to GL_TEXTURE_2D here
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &sceneFmt);
            GLboolean fbSrgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
            mc2::fmt::print_err("[LINEAR_AUDIT] event=color_pipeline_probe framebuffer_srgb={:d} scene_color_internalformat=0x{:04X} (0x881A=GL_RGBA16F) verdict={}\n", mc2::fmt::as_i32((int)fbSrgb), mc2::fmt::as_u32((unsigned)sceneFmt), mc2::fmt::safe((fbSrgb == GL_FALSE)
                  ? "GAMMA_SPACE_MATH:no_FRAMEBUFFER_SRGB->shader_lighting_in_sRGB_space->linear_correctness_required_before_PBR"
                  : "FRAMEBUFFER_SRGB_ON:GL_auto_encodes->verify_shader_inputs_decoded_to_linear"));
            fflush(stderr);
        }
    }

    // Depth/stencil texture (sampleable for post-process depth reconstruction)
    // TEX-CLASS: render-target -- scene depth (sampleable, FBO attach)
    //
    // VK-TIER1 SHARED SCENE DEPTH (gated, GL-only slice): when the gate is ON and
    // the island is compiled, back sceneDepthTex_ with a VK-built exportable
    // D32S8 image imported into GL as GL_DEPTH24_STENCIL8 (the P5-T0 + depth-test
    // spikes proved this is transparent to the 42 depth-sample consumers, the
    // glCopyImageSubData snapshot, and the stencil glTextureView). This is the
    // SINGLE scene-depth alloc+attach site. Fail-soft: if the shared VK context /
    // interop is unavailable, we fall through to the plain GL alloc below so the
    // engine never crashes (gate-ON with no interop == the GL path). Rebuilt on
    // resize because destroyFBOs tears down sharedSceneDepth_ too.
    bool sceneDepthShared = false;
#ifdef MC2_VULKAN_ISLAND
    // VK-RUNG8-ITEM5: VK-only shared depth, armed only under the full native stack.
    {
        const char* reason = "unset";
        sharedSceneDepth_ = mc2SharedSceneDepthCreate(w, h, &reason);
        if (sharedSceneDepth_) {
            sceneDepthTex_ = createSceneDepthPlaceholderTex(w, h);
            sceneDepthShared = true;
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                   GL_TEXTURE_2D, sceneDepthTex_, 0);
            mc2::fmt::print_err("[VK_SHARED_SCENE_DEPTH] active placeholder_tex={:d} {:d}x{:d} (VK-only depth)\n", mc2::fmt::as_u32((unsigned)sceneDepthTex_), mc2::fmt::as_i32(w), mc2::fmt::as_i32(h));
        } else {
            sharedSceneDepth_ = nullptr;
            mc2::fmt::print_err("[VK_SHARED_SCENE_DEPTH] declined reason={} (pure GL GL_DEPTH24_STENCIL8)\n", mc2::fmt::safe(reason ? reason : "unknown"));
            mc2VkSharedSceneStrictFallback("depth", reason);
        }
    }
#endif
    if (!sceneDepthShared) {
        glGenTextures(1, &sceneDepthTex_);
        glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, sceneDepthTex_, 0);
    }

    // SKYBOX-FOG-EXCLUDE-1: stencil-only view onto sceneDepthTex_'s storage.
    // glTextureView requires immutable storage (glTexStorage2D above), which
    // sceneDepthTex_ already uses. GL_DEPTH_STENCIL_TEXTURE_MODE=GL_STENCIL_INDEX
    // is baked into the view at creation, so runEdgeFog/runFogOob can sample
    // depth (via sceneDepthTex_) and stencil (via this view) in the SAME draw
    // without per-bind mode toggling on one shared object.
    {
        // TEX-CLASS: render-target -- stencil texture-view over sceneDepthTex_
        glGenTextures(1, &sceneStencilViewTex_);
        glTextureView(sceneStencilViewTex_, GL_TEXTURE_2D, sceneDepthTex_,
                      GL_DEPTH24_STENCIL8, 0, 1, 0, 1);
        glBindTexture(GL_TEXTURE_2D, sceneStencilViewTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Normal buffer: MRT attachment 1 (rgb=world normal encoded, a=shadow skip flag)
    // TEX-CLASS: render-target -- GBuffer normal (MRT1, FBO attach)
    //
    // VK-P11 TIER-1 SHARED SCENE NORMAL (gated): mirror of the color slot above.
    // Backed by a VK-built exportable RGBA16F imported as GL_RGBA16F so the VK prop
    // island co-writes the GBuffer1 normal (N*0.5+0.5, a=0.25) into this SAME
    // attachment in-place. SINGLE normal alloc+attach site. Fail-soft to plain GL.
    bool sceneNormalShared = false;
#ifdef MC2_VULKAN_ISLAND
    // VK-RUNG8-ITEM5: VK-only shared normal (MRT1), armed only under the full stack.
    {
        const char* reason = "unset";
        sharedSceneNormal_ = mc2SharedSceneColorCreate(MC2_SHARED_SCENE_NORMAL, w, h,
                                                       &reason);
        if (sharedSceneNormal_) {
            sceneNormalTex_ = createSceneColorPlaceholderTex(w, h, GL_RGBA16F,
                                                             GL_RGBA, GL_FLOAT,
                                                             GL_NEAREST);
            sceneNormalShared = true;
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                                   GL_TEXTURE_2D, sceneNormalTex_, 0);
            mc2::fmt::print_err("[VK_SHARED_SCENE_COLOR] normal active placeholder_tex={:d} {:d}x{:d} (VK-only MRT1)\n", mc2::fmt::as_u32((unsigned)sceneNormalTex_), mc2::fmt::as_i32(w), mc2::fmt::as_i32(h));
        } else {
            sharedSceneNormal_ = nullptr;
            mc2::fmt::print_err("[VK_SHARED_SCENE_COLOR] normal declined reason={} (pure GL RGBA16F)\n", mc2::fmt::safe(reason ? reason : "unknown"));
            mc2VkSharedSceneStrictFallback("normal", reason);
        }
    }
#endif
    if (!sceneNormalShared) {
        glGenTextures(1, &sceneNormalTex_);
        glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, sceneNormalTex_, 0);
    }

    // M1.5: object-ID attachment-2 (GL_R32UI). Gated on
    // MC2_OBJECT_ID_BUFFER; when env-OFF we skip the texture
    // creation entirely so env-OFF runtime cost is exactly zero on
    // the FBO side. glTexImage2D matches the sceneNormalTex_ pattern
    // above (decision m4); glTexStorage2D migration deferred.
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        bool sceneObjectIdShared = false;
#ifdef MC2_VULKAN_ISLAND
        // VK-RUNG8-ITEM5: VK-only shared object-id (MRT2), armed only under the full stack.
        {
            const char* reason = "unset";
            sharedSceneObjectId_ = mc2SharedSceneColorCreate(MC2_SHARED_SCENE_OBJECT_ID, w, h,
                                                             &reason);
            if (sharedSceneObjectId_) {
                sceneObjectIdTex_ = createSceneColorPlaceholderTex(w, h, GL_R32UI,
                                                                   GL_RED_INTEGER,
                                                                   GL_UNSIGNED_INT,
                                                                   GL_NEAREST);
                sceneObjectIdShared = true;
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                                       GL_TEXTURE_2D, sceneObjectIdTex_, 0);
                mc2::fmt::print_err("[VK_SHARED_SCENE_COLOR] objectId active placeholder_tex={:d} {:d}x{:d} (VK-only MRT2)\n", mc2::fmt::as_u32((unsigned)sceneObjectIdTex_), mc2::fmt::as_i32(w), mc2::fmt::as_i32(h));
            } else {
                sharedSceneObjectId_ = nullptr;
                mc2::fmt::print_err("[VK_SHARED_SCENE_COLOR] objectId declined reason={} (pure GL_R32UI)\n", mc2::fmt::safe(reason ? reason : "unknown"));
                mc2VkSharedSceneStrictFallback("objectId", reason);
            }
        }
#endif
        if (!sceneObjectIdShared) {
            // TEX-CLASS: render-target -- objectID GBuffer (MRT2, FBO attach)
            glGenTextures(1, &sceneObjectIdTex_);
            glBindTexture(GL_TEXTURE_2D, sceneObjectIdTex_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, w, h, 0,
                         GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                                   GL_TEXTURE_2D, sceneObjectIdTex_, 0);
        }
    }

    // MRT: draw to color attachments via centralized policy. Helper
    // adds GL_COLOR_ATTACHMENT2 when env-ON AND sceneObjectIdTex_
    // exists (M1.5 C1 + M3 fix).
    setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT,
                        sceneObjectIdTex_ != 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        mc2::fmt::print_err("gosPostProcess: scene FBO incomplete (0x{:x})\n", mc2::fmt::as_u32(status));
    }

    // REGISTER-MAIN-DEPTH-RESOURCE: publish the full-res scene depth as the
    // MainDepth slot (descriptive only; this owner keeps the GL lifetime).
    // sceneDepthTex_ is a sampleable GL_TEXTURE_2D (GL_DEPTH24_STENCIL8); the
    // Depth24 enum is the closest descriptive label (no combined D/S enum).
    // A future HZB build pass reads desc->glName to source the pyramid.
    {
        RenderCore::RenderResourceDesc dd;
        dd.id        = RenderCore::RenderResourceId::MainDepth;
        dd.kind      = RenderCore::RenderResourceKind::Texture2D;
        dd.lifetime  = RenderCore::RenderResourceLifetime::Persistent;  // screen-sized, recreated only on resize
        dd.format    = RenderCore::RenderResourceFormat::Depth24;
        dd.debugName = "MainDepth";
        dd.width     = (uint32_t)w;
        dd.height    = (uint32_t)h;
        dd.glName    = sceneDepthTex_;
        dd.valid     = (status == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(dd);
    }

    // REGISTRY-POSTPROCESS-FBO-1: publish the scene FBO color targets (observe-only
    // metadata; this owner keeps the GL lifetimes — registry is never read by the
    // draw path). MainColor closes the asymmetry where the FBO-LEDGER tracked
    // sceneFBO_ but the resource registry had no glName for sceneColorTex_.
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::MainColor;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;  // screen-sized, recreated only on resize
        d.format    = RenderCore::RenderResourceFormat::RGBA16F;
        d.debugName = "MainColor";
        d.width     = (uint32_t)w;
        d.height    = (uint32_t)h;
        d.glName    = sceneColorTex_;
        d.valid     = (status == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(d);
    }
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::MainNormal;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;  // screen-sized GBuffer1, recreated only on resize
        d.format    = RenderCore::RenderResourceFormat::RGBA16F;
        d.debugName = "MainNormal";
        d.width     = (uint32_t)w;
        d.height    = (uint32_t)h;
        d.glName    = sceneNormalTex_;
        d.valid     = (status == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(d);
    }
    // SceneObjectId is gated on RenderWorld::IsObjectIdBufferEnabled(); the GBuffer2
    // texture only exists when the gate is live, so register valid ONLY then. When
    // the gate is OFF the slot stays at its default (unregistered / invalid).
    if (sceneObjectIdTex_ != 0) {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::SceneObjectId;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;  // screen-sized GBuffer2, recreated only on resize
        d.format    = RenderCore::RenderResourceFormat::Unknown;  // GL_R32UI; no enum slot
        d.debugName = "SceneObjectId";
        d.width     = (uint32_t)w;
        d.height    = (uint32_t)h;
        d.glName    = sceneObjectIdTex_;
        d.valid     = (status == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(d);
    }

    // --- WATER-REFLECTION-RESOURCE-1: quarter-res reflection target ---
    // Substrate only: allocated + registered, but NO producer renders into it
    // until Phase C, so the texture reads black. Color (RGBA16F) + depth (D24).
    {
        waterReflW_ = w / 4; if (waterReflW_ < 1) waterReflW_ = 1;
        waterReflH_ = h / 4; if (waterReflH_ < 1) waterReflH_ = 1;

        // TEX-CLASS: render-target -- water reflection color (FBO attach)
        glGenTextures(1, &waterReflColorTex_);
        glBindTexture(GL_TEXTURE_2D, waterReflColorTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, waterReflW_, waterReflH_, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // TEX-CLASS: render-target -- water reflection depth (FBO attach)
        glGenTextures(1, &waterReflDepthTex_);
        glBindTexture(GL_TEXTURE_2D, waterReflDepthTex_);
        // GL_FLOAT type + CLAMP_TO_EDGE wrap: match the project's other depth
        // textures (shadow maps) to avoid AMD quirks + OOB-sample wrapping traps.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, waterReflW_, waterReflH_, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &waterReflFBO_);
        RenderCore::framegraph::fboLedger().registerFbo(waterReflFBO_, RenderCore::RenderResourceId::WaterReflectionColor);  // FBO-LEDGER-1
        glBindFramebuffer(GL_FRAMEBUFFER, waterReflFBO_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, waterReflColorTex_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_TEXTURE_2D, waterReflDepthTex_, 0);

        GLenum wrStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (wrStatus != GL_FRAMEBUFFER_COMPLETE)
            mc2::fmt::print_err("gosPostProcess: water reflection FBO incomplete (0x{:x})\n", mc2::fmt::as_u32(wrStatus));

        // Register descriptors (descriptive; this owner keeps the GL lifetime).
        RenderCore::RenderResourceDesc cdesc;
        cdesc.id        = RenderCore::RenderResourceId::WaterReflectionColor;
        cdesc.kind      = RenderCore::RenderResourceKind::Texture2D;
        cdesc.lifetime  = RenderCore::RenderResourceLifetime::External;  // temporal N-1 reflection target
        cdesc.format    = RenderCore::RenderResourceFormat::RGBA16F;
        cdesc.debugName = "WaterReflectionColor";
        cdesc.width     = (uint32_t)waterReflW_;
        cdesc.height    = (uint32_t)waterReflH_;
        cdesc.glName    = waterReflColorTex_;
        cdesc.valid     = (wrStatus == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(cdesc);

        RenderCore::RenderResourceDesc ddesc;
        ddesc.id        = RenderCore::RenderResourceId::WaterReflectionDepth;
        ddesc.kind      = RenderCore::RenderResourceKind::Texture2D;
        ddesc.lifetime  = RenderCore::RenderResourceLifetime::External;  // temporal N-1 reflection target
        ddesc.format    = RenderCore::RenderResourceFormat::Depth24;
        ddesc.debugName = "WaterReflectionDepth";
        ddesc.width     = (uint32_t)waterReflW_;
        ddesc.height    = (uint32_t)waterReflH_;
        ddesc.glName    = waterReflDepthTex_;
        ddesc.valid     = (wrStatus == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(ddesc);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gosPostProcess::copySceneDepthForParticles()
{
    // VFX-SOFT-PARTICLES-MVP-1: snapshot the current scene depth into a
    // dedicated texture so the in-scene particle flush can sample it without a
    // GL feedback loop (sceneDepthTex_ is the bound FBO's depth attachment).
    // Same internal format as sceneDepthTex_ (DEPTH24_STENCIL8) -> straight
    // glCopyImageSubData (no blit/format-match constraints). Lazily allocated;
    // full-res; freed + re-created on resize (destroyFBOs zeroes it).
    if (width_ <= 0 || height_ <= 0 || sceneDepthTex_ == 0) return;

    if (sceneDepthCopyTex_ == 0) {
        // TEX-CLASS: render-target -- depth snapshot for soft particles (copy dst)
        glGenTextures(1, &sceneDepthCopyTex_);
        glBindTexture(GL_TEXTURE_2D, sceneDepthCopyTex_);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, width_, height_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        // REGISTRY-POSTPROCESS-FBO-1: publish the lazily-allocated depth-copy RT
        // (observe-only). Registered at the lazy-alloc site so glName is the live
        // handle; invalidated in destroyFBOs (resize re-allocates lazily).
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::SceneDepthCopy;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::FrameLocal;  // per-frame depth snapshot for soft-depth sampling
        d.format    = RenderCore::RenderResourceFormat::Depth24;  // DEPTH24_STENCIL8; closest enum
        d.debugName = "SceneDepthCopy";
        d.width     = (uint32_t)width_;
        d.height    = (uint32_t)height_;
        d.glName    = sceneDepthCopyTex_;
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    // Texture-to-texture copy of the whole depth image. Ordered after the
    // opaque scene's depth writes; particles never write depth so the source
    // is stable at flush time.
    glCopyImageSubData(sceneDepthTex_,     GL_TEXTURE_2D, 0, 0, 0, 0,
                       sceneDepthCopyTex_, GL_TEXTURE_2D, 0, 0, 0, 0,
                       width_, height_, 1);
}

void gosPostProcess::copySceneColorForVfx()
{
    // VFX-SCENECOLOR-GRAB-1 (FRAME_RESOURCE_SUBSTRATE): snapshot the resolved
    // scene COLOR into a dedicated texture so a future in-scene VFX/transparent
    // pass can sample it WITHOUT a GL feedback loop (sceneColorTex_ is the bound
    // FBO's COLOR_ATTACHMENT0 during the flush). Mirrors copySceneDepthForParticles
    // exactly: same internal format as sceneColorTex_ (RGBA16F) -> a straight
    // glCopyImageSubData (no blit/format-match constraint). Lazily allocated;
    // full-res; freed + re-created on resize (destroyFBOs zeroes it). There is
    // NO consumer yet — this slice only produces the resource (gate default OFF).
    if (width_ <= 0 || height_ <= 0 || sceneColorTex_ == 0) return;

    if (sceneColorCopyTex_ == 0) {
        // TEX-CLASS: render-target -- scene-color snapshot for VFX feedback (copy dst)
        glGenTextures(1, &sceneColorCopyTex_);
        glBindTexture(GL_TEXTURE_2D, sceneColorCopyTex_);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, width_, height_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        // REGISTRY-SCENECOLORCOPY-PRODUCER-1: publish the lazily-allocated scene
        // color-copy RT (observe-only). Registered at the lazy-alloc site so glName
        // is the live handle; invalidated in destroyFBOs (resize re-allocates
        // lazily). Mirrors SceneDepthCopy (REGISTRY-POSTPROCESS-FBO-1). Closes the
        // "id-without-producer" gap for SceneColorCopy (enum id 20).
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::SceneColorCopy;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::FrameLocal;  // per-frame scene-color snapshot for VFX feedback safety
        d.format    = RenderCore::RenderResourceFormat::RGBA16F;
        d.debugName = "SceneColorCopy";
        d.width     = (uint32_t)width_;
        d.height    = (uint32_t)height_;
        d.glName    = sceneColorCopyTex_;
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    // Texture-to-texture copy of the whole color image. Ordered after the
    // opaque scene color is resolved but before the VFX/transparent flush that
    // would sample it, so the snapshot is the scene as it stands pre-VFX.
    // RGBA16F -> RGBA16F: same internalformat, GL-clean under glCopyImageSubData.
    glCopyImageSubData(sceneColorTex_,     GL_TEXTURE_2D, 0, 0, 0, 0,
                       sceneColorCopyTex_, GL_TEXTURE_2D, 0, 0, 0, 0,
                       width_, height_, 1);
}

void gosPostProcess::destroyFBOs()
{
#ifdef MC2_VULKAN_ISLAND
    // Retire depth sampling views before the shared depth image.
    destroyVkPostHzbNative();
    // VK-POST-FOG-NATIVE-1: torn down FIRST — it borrows the shared scene
    // color0/depth resources destroyed below (framebuffer/view over their
    // VkImages must not outlive them). Rebuilt lazily at the next fog site.
    destroyVkPostFogNative();
    // VK-POST-SCREENFX-NATIVE-1: same discipline — borrows shared color0/
    // normal/depth (framebuffer/views over their VkImages must not outlive
    // them). Rebuilt lazily at the next cloud/shoreline site.
    destroyVkPostScreenFxNative();
#endif
    if (sceneFBO_) {
        glDeleteFramebuffers(1, &sceneFBO_);
        sceneFBO_ = 0;
    }
    if (sceneColorTex_) {
#ifdef MC2_VULKAN_ISLAND
        if (sharedSceneColor_) {
            // VK-RUNG8-ITEM5: sceneColorTex_ is a plain-GL placeholder owned here (no
            // interop view); delete it, then free the VK-only shared image.
            glDeleteTextures(1, &sceneColorTex_);
            mc2SharedSceneColorDestroy(sharedSceneColor_);
            sharedSceneColor_ = nullptr;
            sceneColorTex_ = 0;
        } else
#endif
        {
            glDeleteTextures(1, &sceneColorTex_);
            sceneColorTex_ = 0;
        }
        // REGISTRY-POSTPROCESS-FBO-1: mark MainColor unavailable on teardown
        // (resize destroys+recreates; createFBOs re-validates).
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::MainColor; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    if (sceneDepthTex_) {
#ifdef MC2_VULKAN_ISLAND
        if (sharedSceneDepth_) {
            // VK-RUNG8-ITEM5: sceneDepthTex_ is a plain-GL placeholder owned here (no
            // interop view); delete it, then free the VK-only shared depth image.
            glDeleteTextures(1, &sceneDepthTex_);
            mc2SharedSceneDepthDestroy(sharedSceneDepth_);
            sharedSceneDepth_ = nullptr;
            sceneDepthTex_ = 0;
        } else
#endif
        {
            glDeleteTextures(1, &sceneDepthTex_);
            sceneDepthTex_ = 0;
        }
        // REGISTER-MAIN-DEPTH-RESOURCE: mark the slot unavailable on teardown
        // (resize destroys+recreates, so this re-validates in createFBOs).
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::MainDepth; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    // SKYBOX-FOG-EXCLUDE-1: the stencil view is a texture-view onto
    // sceneDepthTex_'s storage, not a separate allocation -- it must be
    // deleted before/independently of the parent (view deletion never frees
    // the parent's storage either way, but drop our handle so resize()
    // re-creates a fresh view against the new sceneDepthTex_).
    if (sceneStencilViewTex_) {
        glDeleteTextures(1, &sceneStencilViewTex_);
        sceneStencilViewTex_ = 0;
    }
    if (sceneNormalTex_) {
#ifdef MC2_VULKAN_ISLAND
        if (sharedSceneNormal_) {
            // VK-RUNG8-ITEM5: plain-GL placeholder owned here; delete it, free the VK image.
            glDeleteTextures(1, &sceneNormalTex_);
            mc2SharedSceneColorDestroy(sharedSceneNormal_);
            sharedSceneNormal_ = nullptr;
            sceneNormalTex_ = 0;
        } else
#endif
        {
            glDeleteTextures(1, &sceneNormalTex_);
            sceneNormalTex_ = 0;
        }
        // REGISTRY-POSTPROCESS-FBO-1: mark MainNormal unavailable on teardown.
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::MainNormal; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    if (sceneObjectIdTex_) {
#ifdef MC2_VULKAN_ISLAND
        if (sharedSceneObjectId_) {
            // VK-RUNG8-ITEM5: plain-GL placeholder owned here; delete it, free the VK image.
            glDeleteTextures(1, &sceneObjectIdTex_);
            mc2SharedSceneColorDestroy(sharedSceneObjectId_);
            sharedSceneObjectId_ = nullptr;
            sceneObjectIdTex_ = 0;
        } else
#endif
        {
            glDeleteTextures(1, &sceneObjectIdTex_);
            sceneObjectIdTex_ = 0;
        }
        // REGISTRY-POSTPROCESS-FBO-1: mark SceneObjectId unavailable on teardown.
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::SceneObjectId; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    if (sceneDepthCopyTex_) {  // VFX-SOFT-PARTICLES-MVP-1
        glDeleteTextures(1, &sceneDepthCopyTex_);
        sceneDepthCopyTex_ = 0;
        // REGISTRY-POSTPROCESS-FBO-1: mark SceneDepthCopy unavailable on teardown
        // (lazily re-allocated by copySceneDepthForParticles after resize).
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::SceneDepthCopy; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    if (sceneColorCopyTex_) {  // VFX-SCENECOLOR-GRAB-1
        glDeleteTextures(1, &sceneColorCopyTex_);
        sceneColorCopyTex_ = 0;
        // REGISTRY-SCENECOLORCOPY-PRODUCER-1: mark SceneColorCopy unavailable on
        // teardown (lazily re-allocated by copySceneColorForVfx after resize).
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::SceneColorCopy; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
    // WATER-REFLECTION-RESOURCE-1: free reflection target + mark slots invalid.
    if (waterReflColorTex_) { glDeleteTextures(1, &waterReflColorTex_); waterReflColorTex_ = 0; }
    if (waterReflDepthTex_) { glDeleteTextures(1, &waterReflDepthTex_); waterReflDepthTex_ = 0; }
    if (waterReflFBO_)      { glDeleteFramebuffers(1, &waterReflFBO_);   waterReflFBO_ = 0; }
    waterReflW_ = waterReflH_ = 0;
    {
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::WaterReflectionColor; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
        inv.id = RenderCore::RenderResourceId::WaterReflectionDepth;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
}

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
