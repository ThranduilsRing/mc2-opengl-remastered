// render_frame_driver.h — GAME-EDITOR-RENDER-FRAME-DRIVER-1 (Slice 6, capstone)
//
// Single NAMED seam for the per-frame world-render dispatch that the game
// (mc2.exe) and the mission editor (EditRel.exe) BOTH already run identically.
//
// Across Slices 0–5 we verified the two hosts drive world rendering through the
// SAME shared calls — the historical divergences were clip-control (S0/S1),
// pick (S3), GPU includes (S2), lifecycle (S4) and passes (S5), all resolved.
// The only thing left was the absence of a named driver: the shared sequence
// lived as a hand-copied bracket in two TUs, so a future render feature could
// silently fork between hosts again. This file formalizes that bracket so it
// CANNOT fork — both hosts call one function.
//
// SCOPE — what is INSIDE the seam (this driver owns it):
//     gos_RendererBeginFrame();
//     Environment.UpdateRenderers();   // => Camera::render world dispatch
//     gos_RendererEndFrame();
// That three-call dispatch is byte-identical in gameosmain.cpp (lines ~577-579)
// and EditorGameOS.cpp (lines ~655-672 once DoGameLogic is hoisted out). It is
// the fork-risk core.
//
// SCOPE — what stays OUTSIDE the seam (host-specific, left at each call site):
//   * gosPostProcess beginScene()/endScene(), resize(), clearGBuffer1()
//     (game has sky-color logic + stencil clear + render_contract scopes; the
//     editor has its own clears, depth-func parity setup, and RTT capture).
//   * The scene/depth clears and clear-color selection.
//   * gos_RendererHandleEvents() (event pump — placement differs per host).
//   * Environment.DoGameLogic() (the editor runs it between BeginFrame and
//     UpdateRenderers to drive EditorInterface; the game runs it elsewhere).
//   * All editor overlays / gizmos / ImGui / RTT present, and the game's HUD /
//     Tracy zones / RenderWorld::frameBannerTick().
// Per the parity TD rule: "the editor may add overlays but must not own a
// parallel render implementation." Overlays bracket the driver call; they do
// not enter it.

#ifndef GOS_RENDER_FRAME_DRIVER_H
#define GOS_RENDER_FRAME_DRIVER_H

#include "render_host_kind.h"   // reuse RenderHostKind { Game, Editor }

// Per-frame description of host-disambiguating render behavior. Intentionally
// minimal: world/camera state already flows through the existing globals
// (eye, land, Environment), so the driver reads those directly — this struct
// only carries the few flags that distinguish host behavior.
struct RenderFrameDesc {
    RenderHostKind host = RenderHostKind::Game;

    // Editor sets true so a future feature inside the seam could branch on
    // "this host paints editor overlays" without re-introducing a host fork at
    // the call site. The game leaves it false. (Overlays themselves stay
    // OUTSIDE the driver — this is purely a behavior tag.)
    bool drawEditorOverlays = false;

    // Editor sets true: this frame's scene FBO must populate the ObjectID
    // attachment for pick (S3). The game leaves it at its default. Reserved for
    // future intra-seam use; today both hosts already arrange selectability via
    // their own beginScene(), which stays outside the seam.
    bool editorSelectable = false;
};

// The shared per-frame world-render dispatch. Encapsulates EXACTLY the
// begin/UpdateRenderers/end bracket both hosts run today — nothing more.
// Callers keep their host-specific beginScene/clears/overlays before and after.
void RenderFrameDriver_RenderWorld(const RenderFrameDesc& desc);

#endif // GOS_RENDER_FRAME_DRIVER_H
