// RENDER-PASS-ATTACHMENT-SCAFFOLD-1 — pass-owned attachment I/O contract registry.
//
// OBSERVE, NOT STEER. Header-only, all-inline, zero runtime cost. Adds NO GL calls,
// NO FBO bind, NO draw reorder. It encodes the per-pass attachment contract recon
// (docs/render-backend-seams/render-pass-attachment-contract-recon-1.md) as DATA so:
//   (a) the [FRAME_PLAN] trace can self-report target / draw-buffer set / reads, and
//   (b) a checker (check-pass-attachment-contract.py) can verify the table stays
//       internally consistent and matches the live colorMask / draw-buffer code.
//
// This is the SCAFFOLD: named resources + named draw-buffer sets + a static contract
// table keyed by the render_frame_plan trace `pass` string. Routing/feedback-barrier
// enforcement are later slices (FRAME-RESOURCE-FEEDBACK-CHECKER-1, DRAWBUFFER-OWNERSHIP-1).
#pragma once

#include <cstddef>
#include <cstdio>

namespace render_pass_attachment {

// Frame-internal resources that matter for feedback / hand-off reasoning. External
// material textures (atlas, PBR maps) are intentionally OMITTED — they are never the
// active render target, so they cannot form a read-from-bound-attachment loop.
enum class FrameResourceId {
    None,
    SceneColor,      // sceneColorTex_ RGBA16F (COLOR0) — LIVE attachment
    SceneNormal,     // sceneNormalTex_ RGBA16F (COLOR1) — LIVE attachment
    SceneObjectId,   // sceneObjectIdTex_ R32UI (COLOR2) — LIVE attachment
    SceneDepth,      // sceneDepthTex_ DEPTH24_STENCIL8 — LIVE attachment
    SceneColorCopy,  // sceneColorCopyTex_ — feedback-safe COPY of SceneColor
    SceneDepthCopy,  // sceneDepthCopyTex_ — feedback-safe COPY of SceneDepth
    ShadowStatic,    // static shadow map (separate FBO)
    ShadowDynamic,   // dynamic/CSM shadow map (separate FBO)
    SsaoColor,       // ssaoColorTex_ (separate half-res FBO)
    WaterReflRT,     // waterReflColorTex_ (separate quarter-res FBO)
    Hzb,             // Hi-Z pyramid (separate FBO)
};

enum class DrawBufferSetId {
    None,
    MainSceneMRT,    // {COLOR0, COLOR1[, COLOR2]} — set by setSceneDrawBuffers(MainSceneMRT)
    SingleColor0,    // {COLOR0} — set by setSceneDrawBuffers(SingleColor)
    Backbuffer,      // default framebuffer single buffer (FBO 0)
};

enum class TargetFbo {
    None,
    SceneFBO,        // sceneFBO_ MRT
    Backbuffer,      // FBO 0
};

// True if this resource is a LIVE attachment (sampling it while it is the render
// target is a feedback loop). Copies / separate-FBO resources are not.
constexpr bool isLiveSceneAttachment(FrameResourceId r) {
    return r == FrameResourceId::SceneColor || r == FrameResourceId::SceneNormal ||
           r == FrameResourceId::SceneObjectId || r == FrameResourceId::SceneDepth;
}

struct Contract {
    const char*     pass;          // == render_frame_plan trace `pass` string (lookup key)
    TargetFbo       target;        // FBO this pass renders into
    DrawBufferSetId drawBuffers;   // active draw-buffer set during the pass
    bool            w0, w1, w2;    // color outputs written (COLOR0 / COLOR1 / COLOR2)
    FrameResourceId reads[4];      // frame resources sampled (None-padded)
    bool            ownsColorMask; // opts into pipeline_binder colorMask ownership
    bool            feedbackSafe;  // asserted: does not sample a LIVE attachment it writes
};

// The 14-pass contract registry. Keyed by the trace `pass` string. Passes that do not
// (yet) call render_frame_plan::trace are still recorded for documentation + checker
// coverage; the trace enrichment simply never fires for them until a trace site exists.
inline constexpr Contract kContracts[] = {
    // Scene geometry (inherit the MRT draw set bound by terrain).
    { "TerrainSolid",    TargetFbo::SceneFBO,    DrawBufferSetId::MainSceneMRT, true,  true,  false,
      { FrameResourceId::ShadowStatic, FrameResourceId::ShadowDynamic, FrameResourceId::None, FrameResourceId::None }, false, true },
    { "MechOpaque",      TargetFbo::SceneFBO,    DrawBufferSetId::MainSceneMRT, true,  true,  true,
      { FrameResourceId::ShadowDynamic, FrameResourceId::None, FrameResourceId::None, FrameResourceId::None }, false, true },
    { "StaticPropOpaque",TargetFbo::SceneFBO,    DrawBufferSetId::MainSceneMRT, true,  true,  true,
      { FrameResourceId::ShadowDynamic, FrameResourceId::None, FrameResourceId::None, FrameResourceId::None }, false, true },
    { "WaterFastPath",   TargetFbo::SceneFBO,    DrawBufferSetId::MainSceneMRT, true,  true,  false,
      { FrameResourceId::WaterReflRT, FrameResourceId::None, FrameResourceId::None, FrameResourceId::None }, false, true },

    // VFX (in-scene). Billboard/mesh sample feedback-safe copies; tube forces SingleColor.
    { "VfxBillboard",    TargetFbo::SceneFBO,    DrawBufferSetId::MainSceneMRT, true,  false, false,
      { FrameResourceId::SceneDepthCopy, FrameResourceId::SceneColorCopy, FrameResourceId::None, FrameResourceId::None }, false, true },
    { "VfxTube",         TargetFbo::SceneFBO,    DrawBufferSetId::SingleColor0, true,  false, false,
      { FrameResourceId::None, FrameResourceId::None, FrameResourceId::None, FrameResourceId::None }, false, true },
    { "VfxMesh",         TargetFbo::SceneFBO,    DrawBufferSetId::MainSceneMRT, true,  false, false,
      { FrameResourceId::SceneColorCopy, FrameResourceId::None, FrameResourceId::None, FrameResourceId::None }, false, true },

    // Projected decals (post-opaque). Samples depth COPY + live normal (never written here).
    { "ProjectedDecals", TargetFbo::SceneFBO,    DrawBufferSetId::SingleColor0, true,  false, false,
      { FrameResourceId::SceneDepthCopy, FrameResourceId::SceneNormal, FrameResourceId::None, FrameResourceId::None }, false, true },

    // Post-process family (SingleColor {0}, colorMask-owned per COLORMASK-ROLLOUT-POSTFX-1).
    { "SsaoApply",       TargetFbo::SceneFBO,    DrawBufferSetId::SingleColor0, true,  false, false,
      { FrameResourceId::SsaoColor, FrameResourceId::SceneDepth, FrameResourceId::None, FrameResourceId::None }, true, true },
    { "ScreenShadow",    TargetFbo::SceneFBO,    DrawBufferSetId::SingleColor0, true,  false, false,
      { FrameResourceId::ShadowDynamic, FrameResourceId::SceneDepth, FrameResourceId::None, FrameResourceId::None }, true, true },
    { "CloudShadow",     TargetFbo::SceneFBO,    DrawBufferSetId::SingleColor0, true,  false, false,
      { FrameResourceId::SceneDepth, FrameResourceId::None, FrameResourceId::None, FrameResourceId::None }, true, true },
    { "Shoreline",       TargetFbo::SceneFBO,    DrawBufferSetId::SingleColor0, true,  false, false,
      { FrameResourceId::SceneDepth, FrameResourceId::None, FrameResourceId::None, FrameResourceId::None }, true, true },
    { "EdgeFog",         TargetFbo::SceneFBO,    DrawBufferSetId::SingleColor0, true,  false, false,
      { FrameResourceId::SceneDepth, FrameResourceId::None, FrameResourceId::None, FrameResourceId::None }, true, true },
    { "FogOob",          TargetFbo::SceneFBO,    DrawBufferSetId::SingleColor0, true,  false, false,
      { FrameResourceId::SceneDepth, FrameResourceId::None, FrameResourceId::None, FrameResourceId::None }, true, true },

    // Composite: resolves SceneColor -> backbuffer (target switches to FBO 0 first).
    { "Composite",       TargetFbo::Backbuffer,  DrawBufferSetId::SingleColor0, true,  false, false,
      { FrameResourceId::SceneColor, FrameResourceId::None, FrameResourceId::None, FrameResourceId::None }, true, true },

    // HUD: 2D over the backbuffer; no scene-resource sampling.
    { "Hud",             TargetFbo::Backbuffer,  DrawBufferSetId::Backbuffer,   true,  false, false,
      { FrameResourceId::None, FrameResourceId::None, FrameResourceId::None, FrameResourceId::None }, false, true },
};

constexpr std::size_t kContractCount = sizeof(kContracts) / sizeof(kContracts[0]);

inline const char* resourceName(FrameResourceId r) {
    switch (r) {
        case FrameResourceId::None:          return "None";
        case FrameResourceId::SceneColor:    return "SceneColor";
        case FrameResourceId::SceneNormal:   return "SceneNormal";
        case FrameResourceId::SceneObjectId: return "SceneObjectId";
        case FrameResourceId::SceneDepth:    return "SceneDepth";
        case FrameResourceId::SceneColorCopy:return "SceneColorCopy";
        case FrameResourceId::SceneDepthCopy:return "SceneDepthCopy";
        case FrameResourceId::ShadowStatic:  return "ShadowStatic";
        case FrameResourceId::ShadowDynamic: return "ShadowDynamic";
        case FrameResourceId::SsaoColor:     return "SsaoColor";
        case FrameResourceId::WaterReflRT:   return "WaterReflRT";
        case FrameResourceId::Hzb:           return "Hzb";
    }
    return "?";
}

inline const char* drawBufName(DrawBufferSetId d) {
    switch (d) {
        case DrawBufferSetId::None:         return "None";
        case DrawBufferSetId::MainSceneMRT: return "MainSceneMRT";
        case DrawBufferSetId::SingleColor0: return "SingleColor0";
        case DrawBufferSetId::Backbuffer:   return "Backbuffer";
    }
    return "?";
}

inline const char* targetName(TargetFbo t) {
    switch (t) {
        case TargetFbo::None:       return "None";
        case TargetFbo::SceneFBO:   return "SceneFBO";
        case TargetFbo::Backbuffer: return "Backbuffer";
    }
    return "?";
}

inline const Contract* find(const char* pass) {
    if (!pass) return nullptr;
    for (std::size_t i = 0; i < kContractCount; ++i) {
        const char* a = kContracts[i].pass;
        const char* b = pass;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') return &kContracts[i];
    }
    return nullptr;
}

// Compose " target=.. drawBuffers=.. writes=NNN reads=A,B" into buf. Inline, no alloc.
inline void format(const Contract& c, char* buf, std::size_t n) {
    char reads[96];
    std::size_t ri = 0;
    bool any = false;
    for (const FrameResourceId r : c.reads) {
        if (r == FrameResourceId::None) continue;
        const char* rn = resourceName(r);
        if (any && ri + 1 < sizeof(reads)) reads[ri++] = ',';
        for (const char* p = rn; *p && ri + 1 < sizeof(reads); ++p) reads[ri++] = *p;
        any = true;
    }
    if (!any) { reads[0] = 'N'; reads[1] = 'o'; reads[2] = 'n'; reads[3] = 'e'; ri = 4; }
    reads[ri] = '\0';

    // writes mask as digits, e.g. "0", "01", "012", or "-" if nothing.
    char writes[4]; std::size_t wi = 0;
    if (c.w0) writes[wi++] = '0';
    if (c.w1) writes[wi++] = '1';
    if (c.w2) writes[wi++] = '2';
    if (wi == 0) writes[wi++] = '-';
    writes[wi] = '\0';

    std::snprintf(buf, n, " target=%s drawBuffers=%s writes=%s reads=%s",
                  targetName(c.target), drawBufName(c.drawBuffers), writes, reads);
}

}  // namespace render_pass_attachment
