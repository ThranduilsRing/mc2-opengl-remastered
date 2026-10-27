// code/gameplay_pick.h
//
// M2-pre (RenderWorld Slice): shared gameplay-pick spine extracted from
// MissionInterfaceManager::tryStaticPropPick (code/missiongui.cpp).
// Pays the M1.6 greybeard debt: when M2.6 mech-pickup wires up, both
// callers (tryStaticPropPick + the future tryMechPick) call
// tryGameplayPick() instead of duplicating the inline machinery.
//
// Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md
// Plan: docs/superpowers/plans/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-plan.md
//
// Free-function utility header -- new convention in code/ (existing
// headers here are class-method declarations). Justified by the named
// multi-consumer trajectory in spec Section 7.

#ifndef GAMEPLAY_PICK_H
#define GAMEPLAY_PICK_H

// Gameplay selection shares grounded bounds with hover and order targets.
class GameObject;
GameObject* pickGameplaySelection(long x, long y);

#include "../RenderWorld/RenderWorld.h"  // RenderWorld::LookupResult

// Input gesture + missiongui state. Mirrors the 7 args
// tryStaticPropPick already takes, plus nothing.
struct GameplayPickRequest {
    // Viewport-space mouse coords (Win32 convention: origin top-left,
    // Y grows down). Match userInput->getMouseX/Y().
    int  mouseX;
    int  mouseY;

    // Edge-triggered gesture flags from missiongui input gather.
    bool shiftDn;            // userInput->shift() this frame
    bool leftClicked;        // (!isLeftDrag && !isRightDrag && isLeftClick && !lastUpdateDoubleClick)
    bool bGui;               // cursor in HUD/control region
    bool bLeftDouble;        // double-click in progress

    // Mover-first fallback observable. Caller sets to true when the
    // legacy click-driven selection path consumed THIS click (see M1.6
    // Q6 4-site instrumentation map at code/missiongui.cpp:1476/1511/
    // 1740/1763). Spec Q7 RESOLVED: keep the mover-only name.
    bool moverSelectedThisFrame;
};

// Diagnostic context propagated to caller for logging. All fields echo
// the inputs + the intermediate coord-translation results so caller
// logs can show the full transform on one line. Field set mirrors the
// M2.6 [GAMEPLAY_PICK v1] hit/miss log printf args exactly.
struct GameplayPickContext {
    int   mouseX, mouseY;        // echo of request (viewport-space, top-left origin)
    int   fboX,   fboY;          // post-scale FBO pixel (top-left origin)
    int   glX,    glY;           // post-y-flip GL pixel (bottom-left origin)
    float vMulX,  vMulY;         // gos_GetViewport scale (FBO-pixel units)
    float vAddX,  vAddY;         // gos_GetViewport offset
    int   drawableWidth;
    int   drawableHeight;
};

// Outcome enum + result. Caller switches on outcome.
struct GameplayPickResult {
    enum class Outcome {
        skipped, // env / gesture / off-screen gates failed; no lookup ran
        gated,   // moverSelectedThisFrame fired; legacy path won
        miss,    // lookupAtPixel ran; pixel was background (raw=0)
        hit,     // lookupAtPixel ran; valid handle returned
    };
    Outcome                    outcome;
    RenderWorld::LookupResult  lookup;  // valid iff outcome == hit
    GameplayPickContext        ctx;
};

// The shared gameplay-pick dispatcher. Single substrate-consumer spine.
// See spec Section 4 for full algorithm + ordering. M2.6 mech-pickup
// will be the second caller (spec Section 10 extension contract).
//
// Side effects: NONE on outcome in {skipped, gated}. ONE synchronous
// glReadPixels via RenderWorld::lookupAtPixel when all gates pass and
// moverSelectedThisFrame is false (i.e. outcome IN {miss, hit}).
// Logging: NONE. Debug-state mutation: NONE. Caller owns both.
// Thread-safety: main thread only (same as M1.6).
GameplayPickResult tryGameplayPick(const GameplayPickRequest& req);

// Validation-gate self-test. Gated by MC2_GAMEPLAY_PICK_SELFTEST=1 +
// MC2_OBJECT_ID_BUFFER=1. Exercises tryGameplayPick with 8 synthetic
// GameplayPickRequest inputs; asserts each result.outcome matches
// expected. Emits to stderr:
//   [GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all
//   [GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=N expected=<...> actual=<...>
// Mirrors M1.5 RenderWorld::runSubstrateSelfTest() shape. FAIL is a
// STOP indicating the extracted spine has diverged from M1.6 gate
// semantics. Spec Section 8 Gate 2.
void RunGameplayPickSelfTest();

#endif // GAMEPLAY_PICK_H
