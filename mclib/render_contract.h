// render_contract.h — Render Contract Registry (phase 2).
//
// Names the implicit contracts that govern how each render path interacts
// with the G-buffer, shadow pipeline, depth/blend state, and post-process
// passes. Phase 2 extends phase 1 with:
//
//   - RequiredAttachments: which COLOR_ATTACHMENTx a pass needs in the
//     active draw-buffer list (critical after M1.5 added attachment-2).
//   - ShaderOutputContract: which layout(location=N) outputs a pass's
//     fragment shader declares (uniqueness check; must not collide).
//   - attachmentCount in PassStateContract: expected glDrawBuffers arg.
//   - Debug assertions gated by MC2_RENDER_CONTRACT_ASSERT env var:
//     assertPassContract() queries live GL state and compares to contract.
//
// Phase 2 also fills in the remaining TODO_RENDER_CONTRACT rows.
//
// Two load-bearing audit findings drove this header (from phase 1):
//
//   1. GBuffer1.alpha is a POST-PROCESS SHADOW MASK, not a "terrain flag."
//      Terrain, grass, terrain decals, and terrain overlays all opt in by
//      writing 1.0; shadow_screen.frag's "isTerrain" local is misnamed.
//
//   2. The contract is already silently ambiguous: gos_terrain.frag writes
//      a continuous materialAlpha into the same channel for water/shoreline
//      pixels, and several MRT-bound shaders do not declare GBuffer1 at all.
//
// See: docs/superpowers/specs/2026-04-26-render-contract-registry-design.md

#ifndef MC2_RENDER_CONTRACT_H
#define MC2_RENDER_CONTRACT_H

#include "mc2_czstring.h"
#include <cstdint>
#include "RenderCore/RenderPassContract.h"

namespace render_contract {

// The kind of draw call. Queryable from material setup, shadow paths, FBO
// state machine. Phase 1 default for an unclassified callsite is Unknown;
// callsites are tagged via the // [RENDER_CONTRACT:Pass=...] marker.
enum class PassIdentity : std::uint8_t {
    Unknown = 0,
    TerrainBase,        // tessellated heightfield (gos_terrain.frag)
    TerrainOverlay,     // perimeter cement / transitions (terrain_overlay.frag)
    TerrainDecal,       // craters, footprints, scorch marks (decal.frag)
    Grass,              // GPU grass (shader retired) — terrain-derived
    Water,              // water surface + detail (intentional projected path)
    OpaqueObject,       // mechs, vehicles, buildings — opaque pass
    AlphaObject,        // same shapes when alpha-tested or alpha-blended
    StaticProp,         // GPU static-prop renderer (static_prop.frag)
    VegetationCards,    // instanced crossed-quad vegetation billboards (v1 alpha-discard)
    ParticleEffect,     // weapon bolts, weather, clouds, explosions
    UI,                 // HUD, text, menu (screen-space)
    DebugOverlay,       // F1 / F2 / RAlt+P / etc. — diagnostic only
    ShadowCaster,       // depth-only pass for static or dynamic shadow map
    PostProcess,        // shadow_screen, ssao, bloom, godray, shoreline
};

// What each G-buffer attachment slot carries. The alpha of slot 1 is
// renamed honestly: it is a post-process shadow mask, not a terrain flag.
enum class GBufferSlot : std::uint8_t {
    Color0_Albedo = 0,
    Normal1_PostShadowMask = 1,   // RGB = normal*0.5+0.5; a > 0.5 → skip post-shadow
    ObjectId2_R32UI = 2,          // R32_UINT object handle (M1.5+; MC2_OBJECT_ID_BUFFER)
};

// Which COLOR_ATTACHMENTx a pass needs present in the active glDrawBuffers
// list. A slot may be bound without being written (e.g. terrain runs while
// attachment-2 is bound but doesn't declare layout(location=2)) — that is
// legal in GL. assertPassContract() verifies the REQUIRED slots are non-NONE.
struct RequiredAttachments {
    bool color0;    // GL_COLOR_ATTACHMENT0 — albedo / HDR scene color
    bool color1;    // GL_COLOR_ATTACHMENT1 — GBuffer normal + post-shadow mask
    bool color2;    // GL_COLOR_ATTACHMENT2 — R32_UINT object ID (M1.5+)
};

// A pass's shadow-pipeline relationship.
struct ShadowContract {
    bool castsStaticShadow;       // included in static terrain shadow atlas
    bool castsDynamicShadow;      // included in dynamic local shadow map
    bool skipsPostScreenShadow;   // shadow_screen.frag will not darken this pixel
};

// What layout(location=N) outputs the fragment shader declares. Must be
// unique within a pass (duplicate output locations are a GLSL compile error,
// but the contract documents what's expected for auditing). assertPassContract
// does NOT validate shader outputs — these are compile-time GLSL constraints.
// The struct is informational; use it for cross-reference audits.
struct ShaderOutputContract {
    bool writesLocation0;   // out vec4 at location 0 — albedo / HDR color
    bool writesLocation1;   // out vec4 at location 1 — normal + shadow mask
    bool writesLocation2;   // out uint at location 2 — object ID (M1.5+)
    bool writesDepth;       // explicit gl_FragDepth write (depth passes only)
};

// A pass's GL-state contract. Documents what the pass requires on entry
// and guarantees on exit. assertPassContract() validates the GL-side fields
// (attachments, depthTest, depthWrite) at runtime under MC2_RENDER_CONTRACT_ASSERT.
struct PassStateContract {
    enum class BlendMode : std::uint8_t { Opaque, AlphaBlend, AlphaTest, Additive };

    bool               requiresDepthTest;
    bool               requiresDepthWrite;
    BlendMode          blend;
    bool               requiresMRT;       // expects 2+ attachments via glDrawBuffers
    std::uint8_t       attachmentCount;   // expected glDrawBuffers count (0=depth-only)
    RequiredAttachments attachments;      // which slots must be non-NONE
    const char*        expectedFBO;       // documentary string
    bool               restoresStateOnExit; // overlay passes must restore state
};

// Registry accessors. Defined in render_contract.cpp.
const ShadowContract&       shadowContractFor(PassIdentity);
const PassStateContract&    stateContractFor(PassIdentity);
const ShaderOutputContract& shaderOutputContractFor(PassIdentity);
const char*                 passIdentityName(PassIdentity);

// Lossy collapse of the fine-grained callsite-tag taxonomy (PassIdentity, 15)
// onto the coarse owner-lane taxonomy (RenderCore::RenderPassId). Returns
// RenderPassId::None for tags with no owner lane (Unknown, DebugOverlay).
RenderCore::RenderPassId toRenderPassId(PassIdentity);

// CONTRACT-3: per-frame resource-ordering audit.
// Env gates:
//   MC2_RENDER_PASS_ORDER=1   -- check reads[] satisfied; warn + count violations
//   MC2_RENDER_PASS_TELEMETRY=1 -- emit rate-limited pass manifest (every 300 frames)
//
// Call frameBegin() once per frame before any pass.
// Call beginPass(id) before each tracked pass; endPass(id) after.
// Returns count of ordering violations since frameBegin(); 0 under normal operation.

void initRenderPassOrder();
void frameBegin();
void beginPass(RenderCore::RenderPassId id);
void endPass(RenderCore::RenderPassId id);
uint32_t getFrameViolationCount();
void _test_forceOrderAudit(bool enabled); // test-only override

// [RENDER_PASS v1] advisory telemetry (slice D1). Gated by
// MC2_RENDER_PASS_TELEMETRY=1 (cached bool; zero log lines and near-zero
// cost when unset). noteRenderPass() is called at a pass's begin point and
// emits one line per pass per sampled frame (every 300 frames, matching
// debug_state_dump cadence):
//   [RENDER_PASS v1] frame=N pass=TerrainBase fbo=3 viewport=0,0,1600,900 drawbuffers=2 phase=begin
// Advisory/log-only: no verdict, no behavior change. Timing + draw-call
// counters are deferred to D2 (would require scattering counters across
// draw-submission sites).
void initRenderPassTelemetry();              // reads env; call once after GL init
void renderPassTelemetryFrameTick();         // call once per presented frame
void noteRenderPass(PassIdentity id, mc2::czstring callerHint = nullptr);

// ---- RENDER-PASS-CONTRACT-ENFORCEMENT-1: pass-scope tracking ----------------
//
// Turns RenderPassContract from documentation into runtime-observable truth by
// tracking pass begin/end as a SCOPE STACK. Detects three discipline errors:
//   - nesting        : a begin whose parent stack reaches the fixed depth cap
//   - owner mismatch  : endPassScope(X) while the innermost open scope is Y
//   - missing end     : scopes still open at the frame boundary
//
// DIAGNOSTICS ONLY. Never aborts, never mutates GL or render state. When both
// gates are unset the hot-path cost is a single cached-bool branch. This is a
// SEPARATE system from assertPassContract() (which aborts under
// MC2_RENDER_CONTRACT_ASSERT) and from the CONTRACT-3 ordering audit
// (MC2_RENDER_PASS_ORDER). It owns two distinct env gates:
//   MC2_RENDER_PASS_CONTRACT_TRACE=1  -- log every begin/end with stack depth
//   MC2_RENDER_PASS_CONTRACT_ASSERT=1 -- count + warn on discipline violations
// TRACE implies tracking; ASSERT implies tracking + violation reporting. Either
// gate alone enables the stack; neither => fully inert.
//
// Slice 1 wraps four unambiguous once-per-frame, self-contained scopes
// (TerrainOverlay, TerrainDecal, PostProcess, UI/HUD). They do not mutually
// nest, so the live stack stays flat; deeper scopes (scene/renderLists/shadow)
// are deferred to a later slice once depth>1 behaviour is validated.
//
// Call initRenderPassScope() once after GL init (reads env). Call
// renderPassScopeFrameBoundary() exactly once per presented frame, AFTER all
// passes for the frame have ended, to flush missing-end violations and reset
// the stack. Pass __func__ (or a static literal) as hint.
void initRenderPassScope();
void beginPassScope(PassIdentity id, mc2::czstring hint = nullptr);
void endPassScope(PassIdentity id, mc2::czstring hint = nullptr);
void renderPassScopeFrameBoundary();
uint32_t getPassScopeViolationCount();   // cumulative since process start (test/inspect)

// ---- SAME-ORDER-EXECUTOR-VALIDATE-1: top-level frame-order pass wrapper --------
//
// VALIDATE-ONLY. The body still sets its own state; the executor wraps + validates +
// counts. NO glApplyState, NO reorder, NO scheduling.
//
// Gate: MC2_FRAMEGRAPH_EXECUTOR (default-OFF). When unset both wrappers are no-ops
// (byte-identical). On mismatch: NON-FATAL (increments failure counter + capped log).
//
// PIN INVARIANT: the wrapper must NOT disturb markTerrainDrawn latch timing,
// g_dispatchMvp16 snapshot timing, knownEarly terrain handling, body-owned state
// setup, or existing pass call order. Wrapper calls are ADDITIVE around the
// existing draw — no GL state change, no reorder.
void executorOwnBeginTopLevel(PassIdentity id, mc2::czstring callerHint = nullptr);
void executorOwnEndTopLevel(PassIdentity id, mc2::czstring callerHint = nullptr);

// APPLY-STATE-TERRAINDECAL-1: public accessor for the MC2_FRAMEGRAPH_EXECUTOR gate
// used by executorOwnBeginTopLevel. Callers in other TUs (gameos_graphics.cpp) gate
// their executor apply-state calls on the SAME predicate so the apply only runs when
// the executor is ON and the pass body runs — identical gating to the validate path.
bool isTopLevelExecutorEnabled();

} // namespace render_contract

#endif // MC2_RENDER_CONTRACT_H
