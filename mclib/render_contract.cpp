// render_contract.cpp — Render Contract Registry table (phase 2).
//
// Phase 2 changes from phase 1:
//   - All TODO_RENDER_CONTRACT rows filled in with audited values.
//   - PassStateContract gains attachmentCount + RequiredAttachments.
//   - ShaderOutputContract table added (output location declarations).
//   - assertPassContract() + initRenderContractAssert() added; gated by
//     MC2_RENDER_CONTRACT_ASSERT env var. Calls glGetIntegerv/glGetBooleanv
//     to verify live GL state; safe only after glewInit().
//
// Authoritative Phase-1 entries (TerrainBase, TerrainOverlay, TerrainDecal,
// Grass, StaticProp, PostProcess) are unchanged in semantics. New Phase-2
// audit entries (Water, OpaqueObject, AlphaObject, ParticleEffect, UI,
// DebugOverlay, ShadowCaster) reflect best-audit at commit time; mark any
// further findings with TODO_RENDER_CONTRACT_P3 for the next pass.
//
// See: docs/superpowers/specs/2026-04-26-render-contract-registry-design.md

#include "render_contract.h"

// GL access for assertPassContract. Only called from render paths,
// well after glewInit(); glGetIntegerv/glGetBooleanv extension pointers
// are guaranteed valid at that point.
#include <GL/glew.h>

#include <cstdio>   // fprintf
#include <cstdlib>  // getenv

#include "RenderCore/RenderPassContract.h"
#include "RenderCore/RenderResourceRegistry.h"
#include "RenderCore/ambient_contract.h"   // FRAME-GRAPH-AMBIENT-RUNTIME-1 cross-check
#include "RenderCore/fbo_ledger.h"         // FRAME-GRAPH-FBO-LEDGER-1 cross-check
#include "RenderCore/frame_pass_trace.h"           // FRAME-GRAPH-EXECUTOR-DRYRUN-1 pure kernel
#include "RenderCore/terrain_subpass_contract.h"   // dominantTerrainPathLive / terrainPathsThatDrew
#include "RenderCore/top_level_pass_executor.h"    // SAME-ORDER-EXECUTOR-VALIDATE-1 descriptor table

namespace render_contract {

namespace {

using BM  = PassStateContract::BlendMode;
using RA  = RequiredAttachments;
using SOC = ShaderOutputContract;

// TODO_RENDER_CONTRACT sentinels — used for rows that remain unaudited.
constexpr ShadowContract kTodoShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,
};
constexpr PassStateContract kTodoState{
    /*requiresDepthTest*/    false,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::Opaque,
    /*requiresMRT*/          false,
    /*attachmentCount*/      1,
    /*attachments*/          RA{true, false, false},
    /*expectedFBO*/          "TODO_RENDER_CONTRACT",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kTodoShaderOutput{
    /*writesLocation0*/ false,
    /*writesLocation1*/ false,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// ---- Phase-1 authoritative entries (shadow + state contracts) ---------------

constexpr ShadowContract kTerrainBaseShadow{
    /*castsStaticShadow*/    true,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,   // gos_terrain.frag writes GBuffer1.a = 1.0
};
constexpr PassStateContract kTerrainBaseState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   true,
    /*blend*/                BM::Opaque,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,      // min 2; 3 when MC2_OBJECT_ID_BUFFER=1 (safe superset)
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kTerrainBaseOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,   // rc_gbuffer1_shadowHandled() macro
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

constexpr ShadowContract kTerrainOverlayShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,   // terrain_overlay.frag writes GBuffer1.a = 1.0
};
constexpr PassStateContract kTerrainOverlayState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::AlphaBlend,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kTerrainOverlayOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

constexpr ShadowContract kTerrainDecalShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,   // decal.frag writes GBuffer1.a = 1.0
};
constexpr PassStateContract kTerrainDecalState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::AlphaBlend,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kTerrainDecalOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

constexpr ShadowContract kGrassShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,   // the retired grass pass wrote GBuffer1.a = 1.0
};
constexpr PassStateContract kGrassState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   true,
    /*blend*/                BM::AlphaTest,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kGrassOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

constexpr ShadowContract kStaticPropShadow{
    /*castsStaticShadow*/    true,
    /*castsDynamicShadow*/   true,
    /*skipsPostScreenShadow*/false,  // static_prop.frag writes GBuffer1.a = 0.0
};
constexpr PassStateContract kStaticPropState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   true,
    /*blend*/                BM::Opaque,
    /*requiresMRT*/          true,
    /*attachmentCount*/      3,      // writes objectId at location 2 (M1.5)
    /*attachments*/          RA{true, true, true},
    /*expectedFBO*/          "scene HDR FBO (MRT + objectId)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kStaticPropOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,
    /*writesLocation2*/ true,   // layout(location=2) out uint v_objectId (M1.5)
    /*writesDepth*/     false,
};

// VegetationCards: instanced crossed-quad billboards, alpha-discard (v1).
// No shadows cast (v1 — no shadow atlas slot allocated).
// Depth test ON (must z-reject behind terrain/mechs); depth write OFF (alpha-discard
// geom without depth write avoids self-occlusion artefacts in crossed-quad pairs).
// AlphaTest blend = the frag shader discards fragments below threshold.
// skipsPostScreenShadow true: vegetation is already lit at draw time, same
// as grass; post-screen shadow darkening on top would double-shade.
constexpr ShadowContract kVegetationCardsShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/true,
};
constexpr PassStateContract kVegetationCardsState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::AlphaTest,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kVegetationCardsOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,   // writes GBuffer1.a = 1.0 to skip post-screen shadow
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

constexpr ShadowContract kPostProcessShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,  // operates on G-buffer; not applicable
};
constexpr PassStateContract kPostProcessState{
    /*requiresDepthTest*/    false,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::Opaque,
    /*requiresMRT*/          false,
    /*attachmentCount*/      1,
    /*attachments*/          RA{true, false, false},
    /*expectedFBO*/          "post FBO (single attachment)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kPostProcessOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ false,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// ---- Phase-2 audit entries --------------------------------------------------

// Water: renders into scene HDR FBO; uses MRT so shadow_screen.frag can
// sample GBuffer1. gos_terrain.frag water path writes GBuffer1.a =
// materialAlpha (documented §3.1 ambiguity — continuous, not boolean).
// Depth write ON: water surface claims depth to prevent terrain bleed-through.
constexpr ShadowContract kWaterShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,  // materialAlpha path; value depends on pixel
};
constexpr PassStateContract kWaterState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   true,
    /*blend*/                BM::AlphaBlend,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kWaterOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,   // materialAlpha — known §3.1 ambiguity
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// OpaqueObject (mechs): MRT scene FBO; writes objectId at location 2 (M2.5).
// No static shadow (MLR path; shadow atlas is terrain-only in current impl).
constexpr ShadowContract kOpaqueObjectShadow{
    /*castsStaticShadow*/    false,  // TODO_RENDER_CONTRACT_P3: confirm shadow atlas scope
    /*castsDynamicShadow*/   true,
    /*skipsPostScreenShadow*/false,  // mechs receive screen shadow
};
constexpr PassStateContract kOpaqueObjectState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   true,
    /*blend*/                BM::Opaque,
    /*requiresMRT*/          true,
    /*attachmentCount*/      3,      // writes objectId (M2.5)
    /*attachments*/          RA{true, true, true},
    /*expectedFBO*/          "scene HDR FBO (MRT + objectId)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kOpaqueObjectOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,
    /*writesLocation2*/ true,   // mech.frag layout(location=2) out uint v_objectId (M2.5)
    /*writesDepth*/     false,
};

// AlphaObject: same FBO as opaque, depth-tested but no depth-write (standard
// alpha ordering). Does not write objectId (alpha geometry not picked).
constexpr ShadowContract kAlphaObjectShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,
};
constexpr PassStateContract kAlphaObjectState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::AlphaBlend,
    /*requiresMRT*/          true,
    /*attachmentCount*/      2,
    /*attachments*/          RA{true, true, false},
    /*expectedFBO*/          "scene HDR FBO (MRT)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kAlphaObjectOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ true,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// ParticleEffect: alpha-blend forward pass INTO the scene FBO while
// MRT is still active. Only writes location 0; location 1 write would corrupt
// GBuffer1 shadow mask for transparent pixels.
constexpr ShadowContract kParticleEffectShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,
};
constexpr PassStateContract kParticleEffectState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::AlphaBlend,  // live: SRC_ALPHA/ONE_MINUS_SRC_ALPHA (gos_particle_bridge.cpp:1164)
    /*requiresMRT*/          false,  // doesn't rely on MRT; just needs color0
    /*attachmentCount*/      1,
    /*attachments*/          RA{true, false, false},
    /*expectedFBO*/          "scene HDR FBO (color0 only needed)",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kParticleEffectOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ false,  // must NOT write GBuffer1 (shadow mask corruption)
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// UI: screen-space, no depth, single attachment (backbuffer or blit target).
constexpr ShadowContract kUIShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,
};
constexpr PassStateContract kUIState{
    /*requiresDepthTest*/    false,
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::AlphaBlend,
    /*requiresMRT*/          false,
    /*attachmentCount*/      1,
    /*attachments*/          RA{true, false, false},
    /*expectedFBO*/          "backbuffer / blit target",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kUIOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ false,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// DebugOverlay: overlaid on screen, must restore blend+depth state on exit.
constexpr ShadowContract kDebugOverlayShadow{
    /*castsStaticShadow*/    false,
    /*castsDynamicShadow*/   false,
    /*skipsPostScreenShadow*/false,
};
constexpr PassStateContract kDebugOverlayState{
    /*requiresDepthTest*/    false,  // depth-aware debug vis enables this per-mode
    /*requiresDepthWrite*/   false,
    /*blend*/                BM::AlphaBlend,
    /*requiresMRT*/          false,
    /*attachmentCount*/      1,
    /*attachments*/          RA{true, false, false},
    /*expectedFBO*/          "any (overlay renders into current target)",
    /*restoresStateOnExit*/  true,
};
constexpr SOC kDebugOverlayOutput{
    /*writesLocation0*/ true,
    /*writesLocation1*/ false,
    /*writesLocation2*/ false,
    /*writesDepth*/     false,
};

// ShadowCaster: depth-only pass; no color outputs. Separate depth-only FBO
// for the shadow atlas / dynamic shadow map. Explicit depth write is the
// entire purpose of the pass.
constexpr ShadowContract kShadowCasterShadow{
    /*castsStaticShadow*/    true,
    /*castsDynamicShadow*/   true,
    /*skipsPostScreenShadow*/false,  // not applicable
};
constexpr PassStateContract kShadowCasterState{
    /*requiresDepthTest*/    true,
    /*requiresDepthWrite*/   true,
    /*blend*/                BM::Opaque,
    /*requiresMRT*/          false,
    /*attachmentCount*/      0,      // depth-only FBO; no color attachments
    /*attachments*/          RA{false, false, false},
    /*expectedFBO*/          "shadow depth-only FBO",
    /*restoresStateOnExit*/  false,
};
constexpr SOC kShadowCasterOutput{
    /*writesLocation0*/ false,
    /*writesLocation1*/ false,
    /*writesLocation2*/ false,
    /*writesDepth*/     true,   // depth-only; may use gl_FragDepth or implicit
};

// ---- Lookup helpers --------------------------------------------------------

const ShadowContract& lookupShadow(PassIdentity id) {
    switch (id) {
        case PassIdentity::TerrainBase:    return kTerrainBaseShadow;
        case PassIdentity::TerrainOverlay: return kTerrainOverlayShadow;
        case PassIdentity::TerrainDecal:   return kTerrainDecalShadow;
        case PassIdentity::Grass:          return kGrassShadow;
        case PassIdentity::Water:          return kWaterShadow;
        case PassIdentity::OpaqueObject:   return kOpaqueObjectShadow;
        case PassIdentity::AlphaObject:    return kAlphaObjectShadow;
        case PassIdentity::StaticProp:        return kStaticPropShadow;
        case PassIdentity::VegetationCards:   return kVegetationCardsShadow;
        case PassIdentity::ParticleEffect:    return kParticleEffectShadow;
        case PassIdentity::UI:             return kUIShadow;
        case PassIdentity::DebugOverlay:   return kDebugOverlayShadow;
        case PassIdentity::ShadowCaster:   return kShadowCasterShadow;
        case PassIdentity::PostProcess:    return kPostProcessShadow;
        case PassIdentity::Unknown:        return kTodoShadow;
    }
    return kTodoShadow;
}

const PassStateContract& lookupState(PassIdentity id) {
    switch (id) {
        case PassIdentity::TerrainBase:    return kTerrainBaseState;
        case PassIdentity::TerrainOverlay: return kTerrainOverlayState;
        case PassIdentity::TerrainDecal:   return kTerrainDecalState;
        case PassIdentity::Grass:          return kGrassState;
        case PassIdentity::Water:          return kWaterState;
        case PassIdentity::OpaqueObject:   return kOpaqueObjectState;
        case PassIdentity::AlphaObject:    return kAlphaObjectState;
        case PassIdentity::StaticProp:        return kStaticPropState;
        case PassIdentity::VegetationCards:   return kVegetationCardsState;
        case PassIdentity::ParticleEffect:    return kParticleEffectState;
        case PassIdentity::UI:             return kUIState;
        case PassIdentity::DebugOverlay:   return kDebugOverlayState;
        case PassIdentity::ShadowCaster:   return kShadowCasterState;
        case PassIdentity::PostProcess:    return kPostProcessState;
        case PassIdentity::Unknown:        return kTodoState;
    }
    return kTodoState;
}

const SOC& lookupShaderOutput(PassIdentity id) {
    switch (id) {
        case PassIdentity::TerrainBase:    return kTerrainBaseOutput;
        case PassIdentity::TerrainOverlay: return kTerrainOverlayOutput;
        case PassIdentity::TerrainDecal:   return kTerrainDecalOutput;
        case PassIdentity::Grass:          return kGrassOutput;
        case PassIdentity::Water:          return kWaterOutput;
        case PassIdentity::OpaqueObject:   return kOpaqueObjectOutput;
        case PassIdentity::AlphaObject:    return kAlphaObjectOutput;
        case PassIdentity::StaticProp:        return kStaticPropOutput;
        case PassIdentity::VegetationCards:   return kVegetationCardsOutput;
        case PassIdentity::ParticleEffect:    return kParticleEffectOutput;
        case PassIdentity::UI:             return kUIOutput;
        case PassIdentity::DebugOverlay:   return kDebugOverlayOutput;
        case PassIdentity::ShadowCaster:   return kShadowCasterOutput;
        case PassIdentity::PostProcess:    return kPostProcessOutput;
        case PassIdentity::Unknown:        return kTodoShaderOutput;
    }
    return kTodoShaderOutput;
}

// ---- Assert machinery -------------------------------------------------------

static bool s_assertEnabled = false;

static void fatalContractViolation(const char* passName, const char* hint,
                                   const char* msg)
{
    fprintf(stderr,
        "[RENDER_CONTRACT_ASSERT] FATAL pass=%s hint=%s: %s\n",
        passName, hint ? hint : "?", msg);
    fflush(stderr);
    std::abort();
}

static void checkDrawBuffer(GLenum query, bool required,
                            const char* slotName, const char* passName,
                            const char* hint)
{
    if (!required) return;
    GLint buf = 0;
    glGetIntegerv(query, &buf);
    if (buf == GL_NONE) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%s required but draw buffer is GL_NONE", slotName);
        fatalContractViolation(passName, hint, msg);
    }
}

// Returns the GL depth func expected for this pass, or 0 meaning "don't check."
// Opaque scene passes use reverse-Z (GL_GEQUAL); shadow caster uses forward-Z (GL_LESS).
static GLenum expectedDepthFuncFor(PassIdentity id)
{
    switch (id) {
        case PassIdentity::TerrainBase:
        case PassIdentity::Grass:
        case PassIdentity::StaticProp:
        case PassIdentity::OpaqueObject:
            return GL_GEQUAL;
        case PassIdentity::ShadowCaster:
            return GL_LESS;
        default:
            return 0; // not checked for this pass
    }
}

} // namespace

// ---- Public API ------------------------------------------------------------

const ShadowContract&       shadowContractFor(PassIdentity id)      { return lookupShadow(id); }
const PassStateContract&    stateContractFor(PassIdentity id)       { return lookupState(id); }
const ShaderOutputContract& shaderOutputContractFor(PassIdentity id){ return lookupShaderOutput(id); }

const char* passIdentityName(PassIdentity id) {
    switch (id) {
        case PassIdentity::Unknown:        return "Unknown";
        case PassIdentity::TerrainBase:    return "TerrainBase";
        case PassIdentity::TerrainOverlay: return "TerrainOverlay";
        case PassIdentity::TerrainDecal:   return "TerrainDecal";
        case PassIdentity::Grass:          return "Grass";
        case PassIdentity::Water:          return "Water";
        case PassIdentity::OpaqueObject:   return "OpaqueObject";
        case PassIdentity::AlphaObject:    return "AlphaObject";
        case PassIdentity::StaticProp:        return "StaticProp";
        case PassIdentity::VegetationCards:   return "VegetationCards";
        case PassIdentity::ParticleEffect:    return "ParticleEffect";
        case PassIdentity::UI:             return "UI";
        case PassIdentity::DebugOverlay:   return "DebugOverlay";
        case PassIdentity::ShadowCaster:   return "ShadowCaster";
        case PassIdentity::PostProcess:    return "PostProcess";
    }
    return "Unknown";
}

// Lossy collapse of PassIdentity (fine-grained callsite tags) onto the coarse
// owner-lane RenderCore::RenderPassId taxonomy. Several PassIdentity values map
// onto one RenderPassId lane (documented per-case below); tags with no owner
// lane collapse to RenderPassId::None.
RenderCore::RenderPassId toRenderPassId(PassIdentity id) {
    using R = RenderCore::RenderPassId;
    switch (id) {
        case PassIdentity::Unknown:         return R::None;
        case PassIdentity::TerrainBase:     return R::Terrain;
        case PassIdentity::TerrainOverlay:  return R::TerrainOverlay;
        case PassIdentity::TerrainDecal:    return R::TerrainDecal;
        case PassIdentity::Grass:           return R::Terrain;          // terrain-derived; the GPU grass pass is retired
        case PassIdentity::Water:           return R::Water;
        case PassIdentity::OpaqueObject:    return R::MechOpaque;       // lossy: also vehicles + legacy buildings
        case PassIdentity::AlphaObject:     return R::MechOpaque;       // lossy: alpha-tested/blended objects
        case PassIdentity::StaticProp:      return R::StaticPropOpaque;
        case PassIdentity::VegetationCards: return R::VegetationCards;
        case PassIdentity::ParticleEffect:  return R::VFX;
        case PassIdentity::UI:              return R::UI;
        case PassIdentity::DebugOverlay:    return R::None;             // diagnostic only, no owner lane
        case PassIdentity::ShadowCaster:    return R::Shadow;
        case PassIdentity::PostProcess:     return R::PostProcess;
    }
    return RenderCore::RenderPassId::None;
}

void initRenderContractAssert() {
    const char* v = ::getenv("MC2_RENDER_CONTRACT_ASSERT");
    s_assertEnabled = (v && v[0] != '\0' && v[0] != '0');
    if (s_assertEnabled)
        fprintf(stderr, "[RENDER_CONTRACT v2] assert mode ACTIVE\n");
}

void assertPassContract(PassIdentity id, const char* hint) {
    if (!s_assertEnabled) return;
    const PassStateContract& state = lookupState(id);
    const char* passName = passIdentityName(id);

    // --- draw-buffer attachment checks ---
    checkDrawBuffer(GL_DRAW_BUFFER0, state.attachments.color0,
                    "COLOR_ATTACHMENT0", passName, hint);
    checkDrawBuffer(GL_DRAW_BUFFER1, state.attachments.color1,
                    "COLOR_ATTACHMENT1", passName, hint);
    checkDrawBuffer(GL_DRAW_BUFFER2, state.attachments.color2,
                    "COLOR_ATTACHMENT2", passName, hint);

    // --- depth test ---
    if (state.requiresDepthTest) {
        GLboolean dt = GL_FALSE;
        glGetBooleanv(GL_DEPTH_TEST, &dt);
        if (!dt)
            fatalContractViolation(passName, hint,
                "depth test required but GL_DEPTH_TEST disabled");
    }

    // --- depth write ---
    if (state.requiresDepthWrite) {
        GLboolean dw = GL_FALSE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &dw);
        if (!dw)
            fatalContractViolation(passName, hint,
                "depth write required but GL_DEPTH_WRITEMASK=FALSE");
    }

    // --- depth func: GL_GEQUAL for opaque scene, GL_LESS for shadow ---
    const GLenum wantDF = expectedDepthFuncFor(id);
    if (wantDF != 0) {
        GLint df = 0;
        glGetIntegerv(GL_DEPTH_FUNC, &df);
        if ((GLenum)df != wantDF) {
            const char* msg =
                (wantDF == GL_GEQUAL) ?
                    "depth func must be GL_GEQUAL (reverse-Z scene pass)" :
                    "depth func must be GL_LESS (shadow forward-Z pass)";
            fatalContractViolation(passName, hint, msg);
        }
    }

    // --- blend disabled for opaque passes ---
    if (state.blend == BM::Opaque) {
        GLboolean blendOn = GL_FALSE;
        glGetBooleanv(GL_BLEND, &blendOn);
        if (blendOn)
            fatalContractViolation(passName, hint,
                "GL_BLEND enabled but pass blend contract is Opaque");
    }
}

// ---- [RENDER_PASS v1] telemetry (slice D1) ----------------------------------
//
// Advisory, log-only, env-gated (MC2_RENDER_PASS_TELEMETRY=1). When OFF the
// hot-path cost is a single cached-bool branch. When ON, emission is
// rate-limited to one line per pass per sampled frame; a frame is sampled
// every kTelemetryFrameInterval frames (300, matching debug_state_dump
// cadence). GL queries (FBO binding, viewport, draw-buffer count) run only
// on sampled frames at pass-begin points.
//
// D2 (deferred): per-pass draw-call/triangle/texture-bind counters + GPU
// timing — those need counter feeds at centralized draw-submission sites.

namespace {

constexpr std::uint64_t kTelemetryFrameInterval = 300;

bool          s_telemetryEnabled = false;
std::uint64_t s_telemetryFrame   = 0;
std::uint64_t s_telemetryLastEmitFrame[16] = {};  // index = (uint8)PassIdentity; 0 sentinel handled below
bool          s_telemetryEverEmitted[16]   = {};

} // namespace

void initRenderPassTelemetry() {
    const char* v = ::getenv("MC2_RENDER_PASS_TELEMETRY");
    s_telemetryEnabled = (v != nullptr && v[0] == '1');
    if (s_telemetryEnabled) {
        printf("[RENDER_PASS v1] telemetry ACTIVE (sample every %u frames)\n",
               (unsigned)kTelemetryFrameInterval);
        fflush(stdout);
    }
}

// FRAME-GRAPH-EXECUTOR-DRYRUN-1: per-frame boundary hook (defined in the anon namespace
// below). Forward-declared here so the once-per-presented-frame tick can drive it. Self-gated
// inside on MC2_FRAMEGRAPH_DRYRUN, so it is inert (and gate-independent of telemetry) by default.
namespace { void dryrunFrameBoundary(); }

void renderPassTelemetryFrameTick() {
    dryrunFrameBoundary();   // gate-independent caller; self-gated inside. Runs every frame.
    if (!s_telemetryEnabled) return;
    ++s_telemetryFrame;
}

// FRAME-GRAPH-AMBIENT-RUNTIME-1: defined below; called here at the most-covered pass
// seam (noteRenderPass fires for every pass owner every frame, independent of the
// telemetry gate below).
void ambientProbeAtPassBegin(RenderCore::RenderPassId id);

// ---- FRAME-GRAPH-EXECUTOR-DRYRUN-1: per-frame observe-and-diff recorder ------
//
// Self-gated on MC2_FRAMEGRAPH_DRYRUN (read once). DEFAULT-OFF, byte-identical when unset:
// the recorder does nothing, no new GL calls, no counter work. When enabled it FILLS a
// process-static current-frame FramePassTrace at each noteRenderPass callsite (id collapsed to
// the coarse RenderPassId, monotonic record order, bound draw-FBO resolved via the SAME ledger
// the FBO guard uses — no new sampling logic), then at the per-frame boundary snapshots the
// dominant terrain branch + drew-count, runs the pure dryRunCompare(), and ACCUMULATES the
// result into process-static counters. Pure comparison lives in RenderCore/frame_pass_trace.h.
//
// Unobserved != diverged: only 7 of 11 passes hit this seam today (recon). Passes that never
// record are classified UNOBSERVED by the kernel, never "missing fire" -> zero false alarms.
namespace {

using namespace RenderCore;
using namespace RenderCore::framegraph;

bool dryrunEnabled() {
    static const bool s = []{
        const char* v = ::getenv("MC2_FRAMEGRAPH_DRYRUN");
        return v && v[0] == '1';
    }();
    return s;
}

FramePassTrace g_dryrunTrace;
bool           g_dryrunTraceInit = false;

// Accumulated across all frames since process start (surfaced via extern "C" below).
unsigned long g_dryrunFrames              = 0;  // frames compared
unsigned long g_dryrunOutOfOrder          = 0;  // total out-of-order events
unsigned long g_dryrunUnobservedTotal     = 0;  // total unobserved declared-slot occurrences
unsigned long g_dryrunObservedTotal       = 0;  // DRYRUN-OBSERVE-COVERAGE-1: total fired (observed) declared-slot occurrences
unsigned long g_dryrunTerrainMutexViol    = 0;  // frames with >1 terrain branch drawing
unsigned long g_dryrunLatchMissFrames     = 0;  // frames whose dominant branch declares-but-misses latch
unsigned long g_dryrunKnownEarlySuppressed = 0; // DRYRUN-DRAWSITE-ORDER-1: suppressed early-draw events

void dryrunEnsureFrameInit() {
    if (!g_dryrunTraceInit) {
        resetTrace(g_dryrunTrace, RenderCore::kFramePassOrder, RenderCore::kFramePassOrderCount);
        g_dryrunTraceInit = true;
    }
}

// Called from noteRenderPass when the gate is on. Records the fired pass into the trace.
void dryrunRecordPass(RenderCore::RenderPassId id) {
    if (id == RenderCore::RenderPassId::None) return;   // tags with no owner lane
    dryrunEnsureFrameInit();
    // Resolve the bound draw FBO to a logical target via the existing ledger (no new sampling).
    GLint bound = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
    const RenderCore::RenderResourceId fbo =
        RenderCore::framegraph::fboLedger().resolve(static_cast<unsigned>(bound));
    recordPassFired(g_dryrunTrace, id, fbo,
                    RenderCore::kFramePassOrder, RenderCore::kFramePassOrderCount);
}

// Called once per presented frame (gate-independent caller; self-gated inside).
void dryrunFrameBoundary() {
    if (!dryrunEnabled()) return;
    dryrunEnsureFrameInit();
    // Snapshot terrain branch + how many branches drew this frame (no globals in the kernel).
    unsigned long tcounts[static_cast<int>(RenderCore::framegraph::TerrainPath::Count)];
    for (int i = 0; i < static_cast<int>(RenderCore::framegraph::TerrainPath::Count); ++i)
        tcounts[i] = RenderCore::framegraph::terrainPathCount(
                         static_cast<RenderCore::framegraph::TerrainPath>(i));
    g_dryrunTrace.terrainBranch    = RenderCore::framegraph::dominantTerrainPath(tcounts);
    g_dryrunTrace.terrainDrewCount = RenderCore::framegraph::terrainPathsThatDrew(tcounts);

    // DRYRUN-DRAWSITE-ORDER-1: if the dominant terrain branch draws pre-renderLists (Gamecam
    // draw-site, i.e. LOD-chunk), mark the Terrain entry as knownEarlyDrawSite so
    // dryRunCompare suppresses its out-of-order event into knownEarlySuppressed rather than
    // outOfOrderCount. The draw-site is data-driven from the contract table — no hardcode.
    {
        const RenderCore::framegraph::TerrainPath dom = g_dryrunTrace.terrainBranch;
        const RenderCore::framegraph::TerrainSubPass* sp =
            RenderCore::framegraph::findTerrainSubPass(dom);
        if (sp && sp->drawSite == RenderCore::framegraph::TerrainDrawSite::Gamecam) {
            RenderCore::framegraph::markEntryKnownEarly(
                g_dryrunTrace, RenderCore::RenderPassId::Terrain,
                RenderCore::kFramePassOrder, RenderCore::kFramePassOrderCount);
        }
    }

    const RenderCore::framegraph::DryRunReport rep =
        RenderCore::framegraph::dryRunCompare(g_dryrunTrace,
            RenderCore::kFramePassOrder, RenderCore::kFramePassOrderCount);

    ++g_dryrunFrames;
    g_dryrunOutOfOrder           += static_cast<unsigned long>(rep.outOfOrderCount);
    g_dryrunUnobservedTotal      += static_cast<unsigned long>(rep.unobservedCount);
    g_dryrunObservedTotal        += static_cast<unsigned long>(rep.firedCount);  // DRYRUN-OBSERVE-COVERAGE-1
    g_dryrunKnownEarlySuppressed += static_cast<unsigned long>(rep.knownEarlySuppressed);
    if (rep.terrainMutexViolation)  ++g_dryrunTerrainMutexViol;
    if (rep.terrainLatchMissActive) ++g_dryrunLatchMissFrames;

    // Log the first N divergences to stderr like the ambient guard does.
    if (rep.outOfOrderCount > 0 || rep.terrainMutexViolation) {
        static unsigned s_logged = 0;
        if (s_logged < 32u) {
            ++s_logged;
            fprintf(stderr,
                "[FRAMEGRAPH_DRYRUN] frame=%lu fired=%d unobserved=%d outOfOrder=%d "
                "firstOOO=%u terrainMutex=%d latchMiss=%d\n",
                g_dryrunFrames, rep.firedCount, rep.unobservedCount, rep.outOfOrderCount,
                (unsigned)rep.firstOutOfOrderPass,
                (int)rep.terrainMutexViolation, (int)rep.terrainLatchMissActive);
            fflush(stderr);
        }
    }

    // Reset for the next frame.
    resetTrace(g_dryrunTrace, RenderCore::kFramePassOrder, RenderCore::kFramePassOrderCount);
}

} // namespace (FRAME-GRAPH-EXECUTOR-DRYRUN-1)

// Read by the debug-state dump (GameOS, no GL include) via extern "C".
extern "C" unsigned long mc2_framegraph_dryrun_enabled()            { return dryrunEnabled() ? 1ul : 0ul; }
extern "C" unsigned long mc2_framegraph_dryrun_frames()             { return g_dryrunFrames; }
extern "C" unsigned long mc2_framegraph_dryrun_out_of_order()       { return g_dryrunOutOfOrder; }
extern "C" unsigned long mc2_framegraph_dryrun_unobserved()         { return g_dryrunUnobservedTotal; }
extern "C" unsigned long mc2_framegraph_dryrun_observed()           { return g_dryrunObservedTotal; }  // DRYRUN-OBSERVE-COVERAGE-1
extern "C" unsigned long mc2_framegraph_dryrun_terrain_mutex()      { return g_dryrunTerrainMutexViol; }
extern "C" unsigned long mc2_framegraph_dryrun_latch_miss()         { return g_dryrunLatchMissFrames; }
extern "C" unsigned long mc2_framegraph_dryrun_known_early_suppressed() { return g_dryrunKnownEarlySuppressed; }

void noteRenderPass(PassIdentity id, const char* callerHint) {
    ambientProbeAtPassBegin(toRenderPassId(id));   // self-gated; before the telemetry gate
    if (dryrunEnabled()) dryrunRecordPass(toRenderPassId(id));  // FRAME-GRAPH-EXECUTOR-DRYRUN-1
    if (!s_telemetryEnabled) return;
    // Sample frame 1 (first full frame) then every kTelemetryFrameInterval.
    if (s_telemetryFrame % kTelemetryFrameInterval != 1) return;

    const unsigned idx = static_cast<unsigned>(id) & 15u;
    if (s_telemetryEverEmitted[idx] && s_telemetryLastEmitFrame[idx] == s_telemetryFrame)
        return;  // already emitted for this pass this sampled frame
    s_telemetryEverEmitted[idx]   = true;
    s_telemetryLastEmitFrame[idx] = s_telemetryFrame;

    GLint fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    int drawBuffers = 0;
    for (int i = 0; i < 8; ++i) {
        GLint buf = 0;
        glGetIntegerv(GL_DRAW_BUFFER0 + i, &buf);
        if (buf != GL_NONE) ++drawBuffers;
    }

    printf("[RENDER_PASS v1] frame=%llu pass=%s fbo=%d viewport=%d,%d,%d,%d drawbuffers=%d phase=begin hint=%s\n",
           (unsigned long long)s_telemetryFrame, passIdentityName(id), (int)fbo,
           (int)vp[0], (int)vp[1], (int)vp[2], (int)vp[3], drawBuffers,
           callerHint ? callerHint : "?");
    fflush(stdout);
}

// DRYRUN-OBSERVE-COVERAGE-1: thin extern "C" shim so code/gamecam.cpp can fire the
// observe-only VFX/ParticleEffect note without including render_contract.h (its
// RenderCore/ include isn't on the mc2/code TU include path). Pure forward; no draw change.
extern "C" void mc2_note_particle_effect_pass() {
    noteRenderPass(PassIdentity::ParticleEffect, "gamecam::particlesFlush");
}

// VFX-FBO-ONLY-VALIDATE-1: thin extern "C" shims so code/gamecam.cpp can take top-level
// executor ownership of the VFX/ParticleEffect pass without including render_contract.h
// (same TU-include-path constraint as the note shim above). FBO-only validation: the
// AmbientContract has NO VFX row (the note seam fires pre-body in a different TU, so ambient
// axes are not honestly declarable), but the FBO ledger declares MainColor (scene HDR FBO
// bound across the whole flush window). executorOwn* are no-ops when MC2_FRAMEGRAPH_EXECUTOR
// is unset (byte-identical OFF). PassIdentity::ParticleEffect -> RenderPassId::VFX via
// toRenderPassId(), same mapping path Water uses. No draw/state change.
extern "C" void mc2_vfx_pass_begin() {
    executorOwnBeginTopLevel(PassIdentity::ParticleEffect, "gamecam::particlesFlush");
}
extern "C" void mc2_vfx_pass_end() {
    executorOwnEndTopLevel(PassIdentity::ParticleEffect, "gamecam::particlesFlush");
}

// ---- CONTRACT-3: per-frame resource-ordering audit ---------------------------
//
// MC2_RENDER_PASS_ORDER=1  -- check reads[] satisfied each beginPass
// MC2_RENDER_PASS_TELEMETRY (shared with [RENDER_PASS v1]) also drives endPass telemetry
//
// FrameResourceState is process-global static (no GL dependency; safe from
// any call site). forceEnabled is set by _test_forceOrderAudit() for CI use.

namespace {

struct FrameResourceState {
    uint32_t writtenBits    = 0;
    uint32_t violationCount = 0;
    int      telemetryTick  = 0;
    bool     orderEnabled   = false;
    bool     telEnabled     = false;
    bool     forceEnabled   = false; // test-only override
};
static FrameResourceState s_frame;

static constexpr int kPassOrderTelemetryInterval = 300;

bool isResourceWritten(RenderCore::RenderResourceId id) {
    if (id == RenderCore::RenderResourceId::Unknown) return true;
    return (s_frame.writtenBits & (1u << static_cast<uint32_t>(id))) != 0;
}

void markResourceWritten(RenderCore::RenderResourceId id) {
    if (id == RenderCore::RenderResourceId::Unknown) return;
    s_frame.writtenBits |= (1u << static_cast<uint32_t>(id));
}

const char* resourceName(RenderCore::RenderResourceId id) {
    switch (id) {
        case RenderCore::RenderResourceId::Unknown:              return "Unknown";
        case RenderCore::RenderResourceId::MainColor:            return "MainColor";
        case RenderCore::RenderResourceId::MainDepth:            return "MainDepth";
        case RenderCore::RenderResourceId::ShadowStaticMap:      return "ShadowStaticMap";
        case RenderCore::RenderResourceId::TerrainHeightTexture: return "TerrainHeightTexture";
        case RenderCore::RenderResourceId::MaterialGpuBuffer:    return "MaterialGpuBuffer";
        case RenderCore::RenderResourceId::ShadowDynamicMap:     return "ShadowDynamicMap";
        case RenderCore::RenderResourceId::WaterReflectionColor: return "WaterReflectionColor";
        case RenderCore::RenderResourceId::WaterReflectionDepth: return "WaterReflectionDepth";
        default:                                                  return "???";
    }
}

const RenderCore::RenderPassContract* findPassContract(RenderCore::RenderPassId id) {
    for (const auto& c : RenderCore::kRenderPassContracts) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

} // anonymous namespace (CONTRACT-3 helpers)

void initRenderPassOrder() {
    const char* v = getenv("MC2_RENDER_PASS_ORDER");
    s_frame.orderEnabled = s_frame.forceEnabled || (v && v[0] == '1');
    const char* t = getenv("MC2_RENDER_PASS_TELEMETRY");
    s_frame.telEnabled = s_frame.forceEnabled || (t && t[0] == '1');
}

void _test_forceOrderAudit(bool enabled) {
    s_frame.forceEnabled = enabled;
    if (enabled) {
        s_frame.orderEnabled = true;
        s_frame.telEnabled   = true;
    }
}

void frameBegin() {
    s_frame.writtenBits    = 0;
    s_frame.violationCount = 0;
    // Pre-seed resources that are always valid (not produced per-frame):
    //   ShadowStaticMap: built once per mission at renderLists() preamble
    //   TerrainHeightTexture: uploaded at mission load
    markResourceWritten(RenderCore::RenderResourceId::ShadowStaticMap);
    markResourceWritten(RenderCore::RenderResourceId::TerrainHeightTexture);
    ++s_frame.telemetryTick;
}

// FRAME-GRAPH-AMBIENT-RUNTIME-1: default-OFF diagnostic probe. Samples live GL ambient
// state at pass entry and compares against the declared ambient ledger
// (RenderCore/ambient_contract.h) via the pure compareAmbient(). Counts divergences;
// MC2_AMBIENT_ASSERT_FATAL aborts. Default OFF because it is a LEDGER-VS-REALITY
// validation tool, not yet a guard: colorMask is re-asserted mid-pass, so an entry-time
// mismatch is DATA about the entry/established boundary, not necessarily a bug. The
// pure comparison is offline-tested (tests/unit/test_frame_graph.cpp); only sampling is
// here. This is the bridge from "ambient state declared" to "ambient state verified".
namespace {
unsigned long g_ambientMismatchCount = 0;
unsigned long g_ambientProbeSamples  = 0;  // declared passes actually compared (proves it ran)
unsigned long g_fboMismatchCount     = 0;  // FRAME-GRAPH-FBO-LEDGER-1
unsigned long g_fboSamples           = 0;
// AMBIENT-VIEWPORT-PROBE-1: OBSERVE-ONLY viewport-axis mismatch counter. Kept SEPARATE
// from g_ambientMismatchCount so the new axis never trips the existing guard's fatal /
// CI path — this slice measures first. Non-fatal even under MC2_FRAMEGRAPH_AMBIENT_FATAL.
unsigned long g_viewportMismatchCount = 0;
unsigned long g_viewportProbeSamples  = 0;

RenderCore::framegraph::ColorMaskState sampleColorMask() {
    GLboolean m[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetBooleanv(GL_COLOR_WRITEMASK, m);
    using C = RenderCore::framegraph::ColorMaskState;
    if (m[0] && m[1] && m[2] && m[3]) return C::AllOn;
    if (!m[0] && !m[1] && !m[2] && !m[3]) return C::AllOff;
    return C::Inherit;   // mixed/partial -> unclassifiable, skipped by compareAmbient
}
RenderCore::framegraph::DepthFuncState sampleDepthFunc() {
    GLint df = 0;
    glGetIntegerv(GL_DEPTH_FUNC, &df);
    using D = RenderCore::framegraph::DepthFuncState;
    if (df == GL_GEQUAL) return D::SceneGEqual;
    if (df == GL_LESS)   return D::ShadowLess;
    return D::Inherit;
}
RenderCore::framegraph::BlendState sampleBlend() {
    using B = RenderCore::framegraph::BlendState;
    return glIsEnabled(GL_BLEND) ? B::On : B::Off;
}
RenderCore::framegraph::DepthWriteState sampleDepthWrite() {
    GLboolean w = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &w);
    using D = RenderCore::framegraph::DepthWriteState;
    return w ? D::On : D::Off;
}
// AMBIENT-VIEWPORT-PROBE-1: glGet-only sample of GL_VIEWPORT, classified by the pure
// classifyViewport(). No GL state change.
RenderCore::framegraph::ViewportKind sampleViewport() {
    GLint vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, vp);
    return RenderCore::framegraph::classifyViewport(vp[0], vp[1], vp[2], vp[3]);
}
} // namespace

void ambientProbeAtPassBegin(RenderCore::RenderPassId id) {
    // FRAME-GRAPH-AMBIENT-GUARD-1: promoted from default-OFF probe to DEFAULT-ON guard
    // for the verified axes (colorMask/depthFunc/depthWrite; blend stays Inherit ->
    // skipped, the probe disproved it as per-pass). Read-only (glGet + count); rendering
    // stays byte-identical. Disable for perf-sensitive runs via
    // MC2_FRAMEGRAPH_AMBIENT_GUARD=0. CI hard-fail via MC2_FRAMEGRAPH_AMBIENT_FATAL=1.
    // The "fail dev/smoke if mismatch>0" tier is scripts/check-ambient-guard.py (reads
    // the dump counter), so a bad ambient handoff trips immediately instead of surfacing
    // later as flicker / missing geometry / wrong depth.
    static const bool s_guardEnabled = []{
        const char* v = ::getenv("MC2_FRAMEGRAPH_AMBIENT_GUARD");
        return !(v && v[0] == '0');   // default ON; only =0 disables
    }();
    if (!s_guardEnabled) return;
    static const bool s_fatal = []{
        return ::getenv("MC2_FRAMEGRAPH_AMBIENT_FATAL") != nullptr
            || ::getenv("MC2_AMBIENT_ASSERT_FATAL") != nullptr;   // legacy alias
    }();

    // --- AMBIENT axes (colorMask / depthFunc / depthWrite) -----------------------
    if (const RenderCore::framegraph::AmbientContract* decl =
            RenderCore::framegraph::findAmbient(id)) {
        ++g_ambientProbeSamples;
        RenderCore::framegraph::AmbientSample live;
        live.colorMask  = sampleColorMask();
        live.depthFunc  = sampleDepthFunc();
        live.blend      = sampleBlend();
        live.depthWrite = sampleDepthWrite();
        // NOTE: viewport is deliberately NOT set on `live` here, so the shared mm/fatal
        // path below stays byte-identical to the pre-slice guard. The viewport axis is
        // measured OBSERVE-ONLY in a separate block (its own counter, never fatal).
        const RenderCore::framegraph::AmbientMismatch mm =
            RenderCore::framegraph::compareAmbient(*decl, live);

        // --- AMBIENT-VIEWPORT-PROBE-1 (OBSERVE-ONLY) -----------------------------
        // Sample GL_VIEWPORT, classify, compare only against a declared viewport kind.
        // Counts divergences (detects the suspected shadow-viewport save/restore leak);
        // NEVER aborts and NEVER changes control flow, so a pre-existing viewport leak
        // measures rather than breaks. glGet only — no GL state change.
        if (decl->viewport != RenderCore::framegraph::ViewportKind::Inherit) {
            ++g_viewportProbeSamples;
            RenderCore::framegraph::AmbientSample vpLive;
            vpLive.viewport = sampleViewport();
            RenderCore::framegraph::AmbientMismatch vpm;
            if (vpLive.viewport != RenderCore::framegraph::ViewportKind::Inherit)
                vpm.viewport = (decl->viewport != vpLive.viewport);
            if (vpm.viewport) {
                ++g_viewportMismatchCount;
                static unsigned s_vpLogged = 0;
                if (s_vpLogged < 32u) {
                    ++s_vpLogged;
                    fprintf(stderr,
                        "[VIEWPORT_PROBE] phase=begin pass=\"%s\" vpMiss=1 "
                        "(decl vp=%d | live vp=%d) [observe-only]\n",
                        decl->note ? decl->note : "?",
                        (int)decl->viewport, (int)vpLive.viewport);
                    fflush(stderr);
                }
                // Intentionally NO abort() — observe-only.
            }
        }

        if (mm.any()) {
            ++g_ambientMismatchCount;
            static unsigned s_logged = 0;
            if (s_logged < 32u) {
                ++s_logged;
                fprintf(stderr,
                    "[AMBIENT_GUARD] phase=begin pass=\"%s\" cmMiss=%d dfMiss=%d dwMiss=%d "
                    "(decl cm=%d df=%d dw=%d | live cm=%d df=%d dw=%d)\n",
                    decl->note ? decl->note : "?",
                    (int)mm.colorMask, (int)mm.depthFunc, (int)mm.depthWrite,
                    (int)decl->colorMaskOnEntry, (int)decl->depthFunc, (int)decl->depthWrite,
                    (int)live.colorMask, (int)live.depthFunc, (int)live.depthWrite);
                fflush(stderr);
            }
            if (s_fatal) abort();
        }
    }

    // --- FBO target (FRAME-GRAPH-FBO-LEDGER-1) -----------------------------------
    // Independent of the ambient contract: a pass may declare an FBO target but no
    // ambient row (e.g. TerrainOverlay/Decal). Sample the bound draw FBO, resolve it to
    // a logical target via the ledger, compare to the pass's declared target.
    const RenderCore::RenderResourceId fboDecl =
        RenderCore::framegraph::declaredFboTarget(id);
    if (fboDecl != RenderCore::RenderResourceId::Unknown) {
        ++g_fboSamples;
        GLint bound = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
        const RenderCore::RenderResourceId actual =
            RenderCore::framegraph::fboLedger().resolve(static_cast<unsigned>(bound));
        if (RenderCore::framegraph::fboMismatch(fboDecl, actual)) {
            ++g_fboMismatchCount;
            static unsigned s_fboLogged = 0;
            if (s_fboLogged < 32u) {
                ++s_fboLogged;
                fprintf(stderr,
                    "[FBO_GUARD] phase=begin passId=%u declTarget=%u actualTarget=%u boundFbo=%d\n",
                    (unsigned)id, (unsigned)fboDecl, (unsigned)actual, (int)bound);
                fflush(stderr);
            }
            if (s_fatal) abort();
        }
    }
}

// Read by the debug-state dump (GameOS, no GL include) via extern "C".
extern "C" unsigned long mc2_ambient_mismatch_count() { return g_ambientMismatchCount; }
extern "C" unsigned long mc2_ambient_probe_samples()  { return g_ambientProbeSamples;  }
extern "C" unsigned long mc2_fbo_mismatch_count()     { return g_fboMismatchCount;     }
extern "C" unsigned long mc2_fbo_samples()            { return g_fboSamples;           }
// AMBIENT-VIEWPORT-PROBE-1 (observe-only).
extern "C" unsigned long mc2_viewport_mismatch_count() { return g_viewportMismatchCount; }
extern "C" unsigned long mc2_viewport_probe_samples()  { return g_viewportProbeSamples;  }

// --- UI-PASS-DRAWCOUNT-AMBIENT-MEASURE-1 ------------------------------------------
// Observe-only telemetry for the EXISTING PassIdentity::UI RAII scope
// (gosRenderer::flushHUDBatch). Measures (a) per-frame UI draw count by kind and
// (b) a read-only sample of the UI pass's ambient ENTRY state, so a LATER slice can
// promote UI from DO_NOT_MODEL -> modeled. Gate: MC2_UI_PASS_MEASURE (default-OFF).
// The gate is enforced at the flushHUDBatch call sites (byte-identical when unset);
// these entry points reuse the anon-namespace sampleColorMask()/... helpers above so
// all glGet stays in this GL-aware TU. Read-only (glGet + count) — NO GL state change.
//
// UI entry ambient is DO_NOT_MODEL today; the "consistent" flag tests whether ONE
// honest UI ambient contract is DECLARABLE: we latch the first-frame sample and count
// any later frame whose entry sample differs (drift). driftCount==0 across a run =>
// entry state is stable frame-to-frame => declarable.
namespace {
// Per-frame draw counters (reset at each UI pass entry sample).
unsigned long g_uiDrawTotal = 0;   // last completed frame's total UI draws
unsigned long g_uiDrawQuad  = 0;
unsigned long g_uiDrawLine  = 0;
unsigned long g_uiDrawTri   = 0;
unsigned long g_uiDrawText  = 0;
// Live accumulators for the frame currently being replayed.
unsigned long g_uiDrawTotalCur = 0;
unsigned long g_uiDrawQuadCur  = 0;
unsigned long g_uiDrawLineCur  = 0;
unsigned long g_uiDrawTriCur   = 0;
unsigned long g_uiDrawTextCur  = 0;
// Entry ambient sample (numeric enum codes; see ambient_contract.h enums).
unsigned long g_uiEntrySamples   = 0;   // # of UI pass entries sampled (proves it ran)
unsigned long g_uiEntryColorMask = 0;   // ColorMaskState (0=Inherit/mixed,1=AllOn,2=AllOff)
unsigned long g_uiEntryDepthFunc = 0;   // DepthFuncState (0=Inherit,1=SceneGEqual,2=ShadowLess)
unsigned long g_uiEntryBlend     = 0;   // BlendState (1=On,2=Off)
unsigned long g_uiEntryDepthWrite= 0;   // DepthWriteState (1=On,2=Off)
bool          g_uiEntryLatched   = false;
unsigned long g_uiFirstColorMask = 0, g_uiFirstDepthFunc = 0,
              g_uiFirstBlend = 0, g_uiFirstDepthWrite = 0;
unsigned long g_uiEntryDriftCount = 0;  // # entries differing from the first latched sample
// UI-PASS-MODEL-1: OBSERVE-ONLY UI ambient guard counters. Kept SEPARATE from
// g_ambientMismatchCount so the newly-modeled UI row can never trip the existing
// default-ON ambient guard's fatal / CI path — UI is verified observe-first, exactly
// like AMBIENT-VIEWPORT-PROBE-1 measured before enforcing.
unsigned long g_uiAmbientSamples       = 0;  // UI ambient compares run (proves it fired)
unsigned long g_uiAmbientMismatchCount = 0;  // UI entry-vs-declared divergences (never fatal)
} // namespace

// Called ONCE at UI pass entry (flushHUDBatch, before the replay loop). Samples the
// ambient axes the ambient contract tracks + rolls the prior frame's draw tallies into
// the reported (last-completed) counters and resets the live accumulators.
extern "C" void mc2_ui_pass_sample_entry_ambient() {
    // Roll previous frame's live tallies into the reported snapshot.
    g_uiDrawTotal = g_uiDrawTotalCur; g_uiDrawTotalCur = 0;
    g_uiDrawQuad  = g_uiDrawQuadCur;  g_uiDrawQuadCur  = 0;
    g_uiDrawLine  = g_uiDrawLineCur;  g_uiDrawLineCur  = 0;
    g_uiDrawTri   = g_uiDrawTriCur;   g_uiDrawTriCur   = 0;
    g_uiDrawText  = g_uiDrawTextCur;  g_uiDrawTextCur  = 0;

    ++g_uiEntrySamples;
    const unsigned long cm = (unsigned long)sampleColorMask();
    const unsigned long df = (unsigned long)sampleDepthFunc();
    const unsigned long bl = (unsigned long)sampleBlend();
    const unsigned long dw = (unsigned long)sampleDepthWrite();
    g_uiEntryColorMask = cm; g_uiEntryDepthFunc = df;
    g_uiEntryBlend = bl;     g_uiEntryDepthWrite = dw;
    if (!g_uiEntryLatched) {
        g_uiFirstColorMask = cm; g_uiFirstDepthFunc = df;
        g_uiFirstBlend = bl;     g_uiFirstDepthWrite = dw;
        g_uiEntryLatched = true;
    } else if (cm != g_uiFirstColorMask || df != g_uiFirstDepthFunc ||
               bl != g_uiFirstBlend    || dw != g_uiFirstDepthWrite) {
        ++g_uiEntryDriftCount;
    }
}

// Called per replayed hudBatch_ draw (kind: 0=quad,1=line,2=tri,3=text).
extern "C" void mc2_ui_pass_note_draw(unsigned kind) {
    ++g_uiDrawTotalCur;
    switch (kind) {
        case 0: ++g_uiDrawQuadCur; break;
        case 1: ++g_uiDrawLineCur; break;
        case 2: ++g_uiDrawTriCur;  break;
        case 3: ++g_uiDrawTextCur; break;
        default: break;
    }
}

extern "C" unsigned long mc2_ui_pass_draw_count()        { return g_uiDrawTotal; }
extern "C" unsigned long mc2_ui_pass_draw_quads()        { return g_uiDrawQuad;  }
extern "C" unsigned long mc2_ui_pass_draw_lines()        { return g_uiDrawLine;  }
extern "C" unsigned long mc2_ui_pass_draw_tris()         { return g_uiDrawTri;   }
extern "C" unsigned long mc2_ui_pass_draw_text()         { return g_uiDrawText;  }
extern "C" unsigned long mc2_ui_pass_entry_samples()     { return g_uiEntrySamples;    }
extern "C" unsigned long mc2_ui_pass_entry_colormask()   { return g_uiEntryColorMask;  }
extern "C" unsigned long mc2_ui_pass_entry_depthfunc()   { return g_uiEntryDepthFunc;  }
extern "C" unsigned long mc2_ui_pass_entry_blend()       { return g_uiEntryBlend;      }
extern "C" unsigned long mc2_ui_pass_entry_depthwrite()  { return g_uiEntryDepthWrite; }
extern "C" unsigned long mc2_ui_pass_entry_drift()       { return g_uiEntryDriftCount; }

// UI-PASS-MODEL-1: OBSERVE-ONLY UI ambient guard. Samples the UI pass entry ambient
// (same read-only glGet helpers as the guard/measure) and compares against the now-declared
// UI row in kPassAmbient[] via the pure compareAmbient() + the viewport axis. Counts
// divergences into g_uiAmbientMismatchCount; NEVER aborts and NEVER changes control flow or
// GL state. This is the runtime cross-check that promotes UI DO_NOT_MODEL -> modeled: the
// ambient guard now VERIFIES UI entry state every frame instead of skipping it. Called at UI
// pass entry (flushHUDBatch, after pp->endScene binds FB0 + fullscreen viewport, after the
// VAO rebind) — the exact point UI-PASS-DRAWCOUNT-AMBIENT-MEASURE-1 sampled. Kept observe-only
// (its own counter, non-fatal) until verified 0 across all tier1, per MEASURE-THEN-ENFORCE.
extern "C" void mc2_ui_pass_check_ambient() {
    const RenderCore::framegraph::AmbientContract* decl =
        RenderCore::framegraph::findAmbient(RenderCore::RenderPassId::UI);
    if (!decl) return;
    ++g_uiAmbientSamples;
    RenderCore::framegraph::AmbientSample live;
    live.colorMask  = sampleColorMask();
    live.depthFunc  = sampleDepthFunc();
    live.blend      = sampleBlend();
    live.depthWrite = sampleDepthWrite();
    live.viewport   = sampleViewport();
    const RenderCore::framegraph::AmbientMismatch mm =
        RenderCore::framegraph::compareAmbient(*decl, live);
    if (mm.any()) {
        ++g_uiAmbientMismatchCount;
        static unsigned s_uiLogged = 0;
        if (s_uiLogged < 32u) {
            ++s_uiLogged;
            fprintf(stderr,
                "[UI_AMBIENT_GUARD] phase=begin pass=\"UI\" cmMiss=%d dfMiss=%d blMiss=%d "
                "dwMiss=%d vpMiss=%d (decl cm=%d df=%d bl=%d dw=%d vp=%d | live cm=%d df=%d "
                "bl=%d dw=%d vp=%d) [observe-only]\n",
                (int)mm.colorMask, (int)mm.depthFunc, (int)mm.blend,
                (int)mm.depthWrite, (int)mm.viewport,
                (int)decl->colorMaskOnEntry, (int)decl->depthFunc, (int)decl->blend,
                (int)decl->depthWrite, (int)decl->viewport,
                (int)live.colorMask, (int)live.depthFunc, (int)live.blend,
                (int)live.depthWrite, (int)live.viewport);
            fflush(stderr);
        }
        // Intentionally NO abort() — observe-only until verified 0 across all tier1.
    }
}
extern "C" unsigned long mc2_ui_pass_ambient_samples()   { return g_uiAmbientSamples;       }
extern "C" unsigned long mc2_ui_pass_ambient_mismatches() { return g_uiAmbientMismatchCount; }

void beginPass(RenderCore::RenderPassId id) {
    ambientProbeAtPassBegin(id);   // runs before order gate
    if (!s_frame.orderEnabled) return;
    const RenderCore::RenderPassContract* c = findPassContract(id);
    if (!c) return;
    for (int i = 0; i < 4; ++i) {
        RenderCore::RenderResourceId r = c->reads[i];
        if (r == RenderCore::RenderResourceId::Unknown) break;
        if (!isResourceWritten(r)) {
            ++s_frame.violationCount;
            fprintf(stderr,
                "[RENDER_PASS_ORDER v3] VIOLATION: pass %s reads %s but no writer seen this frame\n",
                c->name, resourceName(r));
        }
    }
}

void endPass(RenderCore::RenderPassId id) {
    // Early-out on default path (both gates off) -- skip table scan.
    if (!s_frame.orderEnabled && !s_frame.telEnabled) return;

    const RenderCore::RenderPassContract* c = findPassContract(id);
    if (!c) return;

    // ORDER MATTERS: inspect read status BEFORE marking writes complete.
    // 1. Capture read ok/missing status for telemetry
    bool readOk[4] = {};
    int  readCount = 0;
    for (int i = 0; i < 4; ++i) {
        if (c->reads[i] == RenderCore::RenderResourceId::Unknown) break;
        readOk[i] = isResourceWritten(c->reads[i]);
        ++readCount;
    }

    // 2. Mark this pass's writes complete
    for (int i = 0; i < 4; ++i) {
        RenderCore::RenderResourceId w = c->writes[i];
        if (w == RenderCore::RenderResourceId::Unknown) break;
        markResourceWritten(w);
    }

    // 3. Emit telemetry (rate-limited)
    if (!s_frame.telEnabled) return;
    if (s_frame.telemetryTick % kPassOrderTelemetryInterval != 0) return;

    fprintf(stderr, "[RENDER_PASS_TELEMETRY v3] pass=%s", c->name);
    for (int i = 0; i < readCount; ++i) {
        fprintf(stderr, " read=%s:%s",
            resourceName(c->reads[i]),
            readOk[i] ? "ok" : "MISSING");
    }
    for (int i = 0; i < 4; ++i) {
        if (c->writes[i] == RenderCore::RenderResourceId::Unknown) break;
        fprintf(stderr, " write=%s", resourceName(c->writes[i]));
    }
    fprintf(stderr, "\n");
}

uint32_t getFrameViolationCount() {
    return s_frame.violationCount;
}

// ---- RENDER-PASS-CONTRACT-ENFORCEMENT-1: pass-scope tracking -----------------
//
// A bounded scope stack. begin pushes, end pops-and-checks, the frame boundary
// flushes any still-open scopes. Diagnostics only: every anomaly is logged to
// stderr and counted; nothing aborts and no GL/render state is touched. The
// stack is process-global (no GL dependency) and only mutated from the render
// thread, matching the existing CONTRACT-3 / telemetry single-thread model.

namespace {

constexpr int kPassScopeMaxDepth = 8;   // slice 1 stays at depth 1; cap guards runaway begins

struct PassScopeEntry {
    PassIdentity id;
    const char*  hint;   // borrowed literal / __func__; never owned
};

struct PassScopeState {
    bool           traceEnabled    = false;
    bool           assertEnabled   = false;
    int            depth           = 0;
    bool           overflowed      = false;  // latched once depth cap hit, until frame boundary
    uint32_t       violationCount  = 0;       // cumulative since process start
    PassScopeEntry stack[kPassScopeMaxDepth] = {};
};
static PassScopeState s_scope;

static inline bool passScopeActive() {
    return s_scope.traceEnabled || s_scope.assertEnabled;
}

static void passScopeViolation(const char* what, PassIdentity id, const char* hint) {
    ++s_scope.violationCount;
    if (!s_scope.assertEnabled) return;   // TRACE-only run: track silently, no verdict noise
    fprintf(stderr,
        "[RENDER_PASS_CONTRACT v1] VIOLATION(%s) pass=%s hint=%s depth=%d\n",
        what, passIdentityName(id), hint ? hint : "?", s_scope.depth);
    fflush(stderr);
}

} // anonymous namespace (scope-tracking helpers)

void initRenderPassScope() {
    const char* t = ::getenv("MC2_RENDER_PASS_CONTRACT_TRACE");
    const char* a = ::getenv("MC2_RENDER_PASS_CONTRACT_ASSERT");
    s_scope.traceEnabled  = (t && t[0] == '1');
    s_scope.assertEnabled = (a && a[0] == '1');
    s_scope.depth         = 0;
    s_scope.overflowed    = false;
    if (passScopeActive()) {
        fprintf(stderr, "[RENDER_PASS_CONTRACT v1] scope tracking ACTIVE (trace=%d assert=%d)\n",
                s_scope.traceEnabled ? 1 : 0, s_scope.assertEnabled ? 1 : 0);
        fflush(stderr);
    }
}

void beginPassScope(PassIdentity id, const char* hint) {
    if (!passScopeActive()) return;
    if (s_scope.depth >= kPassScopeMaxDepth) {
        if (!s_scope.overflowed) {
            s_scope.overflowed = true;
            passScopeViolation("depth-cap", id, hint);   // counts once per overflow episode
        }
        return;   // refuse to push past the cap; do not corrupt the stack
    }
    if (s_scope.traceEnabled) {
        fprintf(stderr, "[RENDER_PASS_CONTRACT v1] BEGIN depth=%d pass=%s hint=%s\n",
                s_scope.depth, passIdentityName(id), hint ? hint : "?");
        fflush(stderr);
    }
    s_scope.stack[s_scope.depth].id   = id;
    s_scope.stack[s_scope.depth].hint = hint;
    ++s_scope.depth;
}

void endPassScope(PassIdentity id, const char* hint) {
    if (!passScopeActive()) return;
    if (s_scope.depth <= 0) {
        passScopeViolation("end-without-begin", id, hint);
        return;
    }
    const PassScopeEntry& top = s_scope.stack[s_scope.depth - 1];
    if (top.id != id) {
        // Owner mismatch: closing a pass that is not the innermost open scope.
        if (s_scope.assertEnabled) {
            fprintf(stderr,
                "[RENDER_PASS_CONTRACT v1] VIOLATION(owner-mismatch) end=%s hint=%s "
                "but innermost open=%s (openedBy=%s) depth=%d\n",
                passIdentityName(id), hint ? hint : "?",
                passIdentityName(top.id), top.hint ? top.hint : "?", s_scope.depth);
            fflush(stderr);
        }
        ++s_scope.violationCount;
        // Recovery: pop the innermost so the stack still drains by frame end.
    }
    --s_scope.depth;
    if (s_scope.traceEnabled) {
        fprintf(stderr, "[RENDER_PASS_CONTRACT v1] END   depth=%d pass=%s hint=%s\n",
                s_scope.depth, passIdentityName(id), hint ? hint : "?");
        fflush(stderr);
    }
}

void renderPassScopeFrameBoundary() {
    if (!passScopeActive()) return;
    if (s_scope.depth != 0) {
        for (int i = s_scope.depth - 1; i >= 0; --i) {
            passScopeViolation("missing-end", s_scope.stack[i].id, s_scope.stack[i].hint);
        }
    }
    s_scope.depth      = 0;
    s_scope.overflowed = false;
}

uint32_t getPassScopeViolationCount() {
    return s_scope.violationCount;
}

// ---- SAME-ORDER-EXECUTOR-VALIDATE-1: top-level frame-order pass wrapper ----------
//
// VALIDATE-ONLY. Wraps StaticPropOpaque / Terrain / TerrainOverlay / TerrainDecal /
// VegetationCards at their begin/end seams. The body between begin and end is
// UNCHANGED — executor is ADDITIVE (no GL state change, no reorder, no scheduling).
//
// Gate: MC2_FRAMEGRAPH_EXECUTOR (default-OFF). When unset both wrappers are no-ops.
// PIN INVARIANT: markTerrainDrawn latch timing, g_dispatchMvp16 snapshot timing,
// knownEarly terrain handling, body-owned state setup, and call order are undisturbed.

// Process-lifetime counters in the same anonymous namespace block so they share
// visibility with sampleColorMask/sampleDepthFunc/sampleBlend/sampleDepthWrite
// (all anonymous-namespace members of this TU are in the same unnamed namespace).
namespace {

bool topLevelExecutorEnabled() {
    // Shares the same gate as the sub-stage island executor in gos_postprocess.cpp.
    static const bool s_on = []() {
        const char* v = ::getenv("MC2_FRAMEGRAPH_EXECUTOR");
        return v && v[0] == '1';
    }();
    return s_on;
}

// g_validatedTopLevel: End reached (begin+end structurally complete) — optimistic count.
// g_topLevelFailures: ambient or FBO mismatch, or glGetError != NO_ERROR.
// g_skippedDeferred: deferred pass Begin calls seen (Shadow/Mech/Water/VFX/UI).
unsigned long g_validatedTopLevel  = 0;
unsigned long g_topLevelFailures   = 0;
unsigned long g_skippedDeferred    = 0;

static constexpr unsigned kMaxTopLevelFailureLog = 32u;

} // namespace

// APPLY-STATE-TERRAINDECAL-1: public forwarder to the anonymous-namespace gate so
// out-of-TU executor apply-state callers (gameos_graphics.cpp) gate identically to
// executorOwnBeginTopLevel. Same cached env read (MC2_FRAMEGRAPH_EXECUTOR).
bool isTopLevelExecutorEnabled() {
    return topLevelExecutorEnabled();
}

void executorOwnBeginTopLevel(PassIdentity passId, const char* callerHint) {
    if (!topLevelExecutorEnabled()) return;

    const RenderCore::RenderPassId id = toRenderPassId(passId);
    const RenderCore::framegraph::TopLevelPassContract* c =
        RenderCore::framegraph::findTopLevelExecutorPass(id);
    if (!c) {
        // Deferred pass (Shadow/Mech/Water/VFX/UI) — count but do nothing.
        ++g_skippedDeferred;
        return;
    }

    // --- Ambient precondition (ONLY where declared, reusing same sampler helpers) ---
    if (c->validateAmbient) {
        if (const RenderCore::framegraph::AmbientContract* decl =
                RenderCore::framegraph::findAmbient(id)) {
            RenderCore::framegraph::AmbientSample live;
            live.colorMask  = sampleColorMask();
            live.depthFunc  = sampleDepthFunc();
            live.blend      = sampleBlend();
            live.depthWrite = sampleDepthWrite();
            const RenderCore::framegraph::AmbientMismatch mm =
                RenderCore::framegraph::compareAmbient(*decl, live);
            if (mm.any()) {
                ++g_topLevelFailures;
                if (g_topLevelFailures <= kMaxTopLevelFailureLog)
                    fprintf(stderr,
                        "[EXECUTOR_TOPLEVEL v1] BEGIN FAIL ambient pass=%s hint=%s "
                        "cmMiss=%d dfMiss=%d dwMiss=%d failures=%lu\n",
                        c->note, callerHint ? callerHint : "?",
                        (int)mm.colorMask, (int)mm.depthFunc, (int)mm.depthWrite,
                        g_topLevelFailures);
            }
        }
    }

    // --- FBO precondition (ONLY where declared, reusing same FBO ledger helpers) ---
    if (c->validateFbo) {
        const RenderCore::RenderResourceId fboDecl =
            RenderCore::framegraph::declaredFboTarget(id);
        if (fboDecl != RenderCore::RenderResourceId::Unknown) {
            GLint bound = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
            const RenderCore::RenderResourceId actual =
                RenderCore::framegraph::fboLedger().resolve(static_cast<unsigned>(bound));
            if (RenderCore::framegraph::fboMismatch(fboDecl, actual)) {
                ++g_topLevelFailures;
                if (g_topLevelFailures <= kMaxTopLevelFailureLog)
                    fprintf(stderr,
                        "[EXECUTOR_TOPLEVEL v1] BEGIN FAIL fbo pass=%s hint=%s "
                        "declFbo=%u actualFbo=%u boundFbo=%d failures=%lu\n",
                        c->note, callerHint ? callerHint : "?",
                        (unsigned)fboDecl, (unsigned)actual, (int)bound,
                        g_topLevelFailures);
            }
        }
    }

    // PIN INVARIANT: no GL state change, no reorder, no scheduling.
    // The body runs UNCHANGED between this call and executorOwnEndTopLevel.
}

void executorOwnEndTopLevel(PassIdentity passId, const char* callerHint) {
    if (!topLevelExecutorEnabled()) return;

    const RenderCore::RenderPassId id = toRenderPassId(passId);
    const RenderCore::framegraph::TopLevelPassContract* c =
        RenderCore::framegraph::findTopLevelExecutorPass(id);
    if (!c) return;  // deferred pass; Begin already counted it

    // --- Postcondition: glGetError must be GL_NO_ERROR ---
    {
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            ++g_topLevelFailures;
            if (g_topLevelFailures <= kMaxTopLevelFailureLog)
                fprintf(stderr,
                    "[EXECUTOR_TOPLEVEL v1] END FAIL glGetError pass=%s hint=%s "
                    "err=0x%x failures=%lu\n",
                    c->note, callerHint ? callerHint : "?",
                    (unsigned)err, g_topLevelFailures);
            // Clear the error state so downstream code doesn't trip on our sample.
            while (glGetError() != GL_NO_ERROR) {}
        }
    }

    // --- FBO postcondition (ONLY where declared) ---
    if (c->validateFbo) {
        const RenderCore::RenderResourceId fboDecl =
            RenderCore::framegraph::declaredFboTarget(id);
        if (fboDecl != RenderCore::RenderResourceId::Unknown) {
            GLint bound = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
            const RenderCore::RenderResourceId actual =
                RenderCore::framegraph::fboLedger().resolve(static_cast<unsigned>(bound));
            if (RenderCore::framegraph::fboMismatch(fboDecl, actual)) {
                ++g_topLevelFailures;
                if (g_topLevelFailures <= kMaxTopLevelFailureLog)
                    fprintf(stderr,
                        "[EXECUTOR_TOPLEVEL v1] END FAIL fbo pass=%s hint=%s "
                        "declFbo=%u actualFbo=%u boundFbo=%d failures=%lu\n",
                        c->note, callerHint ? callerHint : "?",
                        (unsigned)fboDecl, (unsigned)actual, (int)bound,
                        g_topLevelFailures);
            }
        }
    }

    // Count every completed end (optimistic; failures separately surfaced in dump).
    // Matches the sub-stage island model in gos_postprocess.cpp.
    ++g_validatedTopLevel;
    // PIN INVARIANT: no GL state change, no reorder, no scheduling.
}

extern "C" unsigned long mc2_framegraph_executor_validated_top_level_passes() {
    return g_validatedTopLevel;
}
// FRAMEGRAPH-APPLY-STATE-ISLAND-1: forward to the live counter in gos_postprocess.cpp.
extern "C" unsigned long mc2_framegraph_executor_apply_state_passes_impl();
extern "C" unsigned long mc2_framegraph_executor_apply_state_passes() {
    return mc2_framegraph_executor_apply_state_passes_impl();
}
// PER-PASS-APPLY-COUNTERS-1: per-pass public forwarder (two-hop, mirrors above).
extern "C" unsigned long mc2_framegraph_executor_apply_state_by_pass_impl(unsigned id);
extern "C" unsigned long mc2_framegraph_executor_apply_state_by_pass(unsigned id) {
    return mc2_framegraph_executor_apply_state_by_pass_impl(id);
}
extern "C" unsigned long mc2_framegraph_executor_scheduled_passes() {
    return 0ul;  // always 0; VALIDATE-ONLY slice, no scheduling
}
extern "C" unsigned long mc2_framegraph_executor_skipped_deferred_passes() {
    return g_skippedDeferred;
}

} // namespace render_contract
