// code/gameplay_pick.cpp
//
// M2-pre: shared gameplay-pick spine. See gameplay_pick.h for the
// full contract. Implementation steps:
//   T1: screenToFboPixel (this file's pure transform; the lowest-risk
//       extraction with a verbatim copy of the M1.6 inline coord block).
//   T2: tryGameplayPick (the spine; gate ladder + coord scaling +
//       lookupAtPixel).
//   T4: RunGameplayPickSelfTest (gated by MC2_GAMEPLAY_PICK_SELFTEST=1).

#include "gameplay_pick.h"

#include <cstdio>   // std::fprintf for self-test result lines
#include <cstdlib>  // std::getenv for the self-test env-flag gate
#include <cstring>  // std::strcmp for env-flag value parse

// GameOS header declares both gos_GetViewport (with __stdcall calling
// convention) and the Environment struct (drawableWidth/Height fields).
// The original M1.6 inline at code/missiongui.cpp:6218-6220 reads from
// these same symbols; same include avoids linkage mismatches.
#include "../GameOS/include/gameos.hpp"
#include "../RenderWorld/ScreenPick.h"  // ScreenPickContext, screenPickCompute
#include "Gates/gate.h"
#include "mc2fmt.h"

//----------------------------------------------------------------------
// screenToFboPixel -- pure coord transform.
//
// Derivation (verbatim from M1.6 commit 1853ad9 inline comment at
// code/missiongui.cpp:6197-6216):
//
//   missiongui mouseX/Y from userInput->getMouseX/Y() is
//   mouseXPosition * viewMulX, where mouseXPosition is normalized to
//   drawable (0.0-1.0) and viewMulX is the UI viewport width (800 in
//   the 800x600 logical canvas). So mouseX/Y is in viewport-relative
//   coords (0-vMulX, 0-vMulY), NOT FBO pixels. lookupAtPixel needs
//   FBO pixels.
//
//   Translation: mouseX/Y (viewport space) -> FBO pixel:
//     normalized = mouseX / vMulX (fraction across viewport)
//     fboPixel   = vAddX + normalized * vMulX_fbo
//   where vMulX/Y from gos_GetViewport are already in FBO-pixel units
//   (viewportRight - viewportLeft) * drawableWidth. So vMulX IS the
//   FBO-pixel width of the viewport region. But mouseXPosition is
//   normalized to drawable (not viewport), and getMouseX() multiplies
//   by vMulX -- yielding (mouseX_pixel_in_drawable / drawableWidth) *
//   viewport_pixel_width. For full-screen viewport (vMulX=drawableWidth),
//   this is identity. For sub-viewport, it's a scaled-down value.
//
//   To convert back: multiply by (drawableWidth / vMulX) to get raw
//   drawable-pixel-X. (Equivalent to "undo the viewport scaling".)
//   Then add vAddX for viewport-offset case. Y mirrors.
//
// Precondition: caller has off-screen-guarded (mouseX/Y in [0, vMulX/Y)
// AND vMulX/Y > 0). This helper does NOT re-check.
//----------------------------------------------------------------------
static void screenToFboPixel(int   mouseX,         int   mouseY,
                             float vMulX,          float vMulY,
                             float vAddX,          float vAddY,
                             int   drawableWidth,  int   drawableHeight,
                             int*  outFboX,        int*  outFboY,
                             int*  outGlX,         int*  outGlY)
{
    RenderWorld::ScreenPickContext ctx;
    ctx.mouseX = mouseX; ctx.mouseY = mouseY;
    ctx.vMulX  = vMulX;  ctx.vMulY  = vMulY;
    ctx.vAddX  = vAddX;  ctx.vAddY  = vAddY;
    ctx.drawableWidth  = drawableWidth;
    ctx.drawableHeight = drawableHeight;
    RenderWorld::screenPickCompute(&ctx);
    *outFboX = ctx.fboX; *outFboY = ctx.fboY;
    *outGlX  = ctx.glX;  *outGlY  = ctx.glY;
}

//----------------------------------------------------------------------
// tryGameplayPick -- the shared gameplay-pick spine.
// Implementation lands in T2; T1 ships a STUB so the header symbol
// resolves cleanly and the linker accepts the new TU.
//----------------------------------------------------------------------
GameplayPickResult tryGameplayPick(const GameplayPickRequest& req)
{
    GameplayPickResult r{};
    r.ctx.mouseX  = req.mouseX;
    r.ctx.mouseY  = req.mouseY;
    r.outcome     = GameplayPickResult::Outcome::skipped;

    // Engine substrate gate. M1.5 cached bool; cheap.
    if (!RenderWorld::IsObjectIdBufferEnabled())
        return r;

    // Gesture gates (M1.6 Section 3 detection condition).
    if (!req.shiftDn)       return r;
    if (!req.leftClicked)   return r;
    if (req.bGui)           return r;
    if (req.bLeftDouble)    return r;

    // Mover-first fallback gate (M1.6 Section 4 + Q6/Q8 invariant).
    if (req.moverSelectedThisFrame) {
        r.outcome = GameplayPickResult::Outcome::gated;
        return r;
    }

    // Viewport state for coord translation.
    gos_GetViewport(&r.ctx.vMulX, &r.ctx.vMulY,
                    &r.ctx.vAddX, &r.ctx.vAddY);
    r.ctx.drawableWidth  = Environment.drawableWidth;
    r.ctx.drawableHeight = Environment.drawableHeight;

    // Off-screen guard (viewport-space bounds).
    if (req.mouseX < 0 || req.mouseY < 0
        || req.mouseX >= static_cast<int>(r.ctx.vMulX)
        || req.mouseY >= static_cast<int>(r.ctx.vMulY))
        return r;

    // Coord translation: viewport-space -> FBO pixel -> GL pixel.
    screenToFboPixel(req.mouseX, req.mouseY,
                     r.ctx.vMulX, r.ctx.vMulY,
                     r.ctx.vAddX, r.ctx.vAddY,
                     r.ctx.drawableWidth, r.ctx.drawableHeight,
                     &r.ctx.fboX, &r.ctx.fboY,
                     &r.ctx.glX,  &r.ctx.glY);

    // Synchronous single-pixel readback (M1.5 substrate).
    r.lookup = RenderWorld::lookupAtPixel(r.ctx.glX, r.ctx.glY);

    r.outcome = r.lookup.isValid
        ? GameplayPickResult::Outcome::hit
        : GameplayPickResult::Outcome::miss;
    return r;
}

//----------------------------------------------------------------------
// RunGameplayPickSelfTest -- validation-gate self-test.
//
// Gated by MC2_GAMEPLAY_PICK_SELFTEST=1 AND MC2_OBJECT_ID_BUFFER=1.
// Substrate-off makes the test vacuous (step 8 cannot reach
// lookupAtPixel); we refuse to run rather than emit a misleading PASS.
//
// 8-step assertion sequence per spec Section 8 Gate 2:
//   step=1 shiftDn=false                                  -> skipped
//   step=2 leftClicked=false                              -> skipped
//   step=3 bGui=true                                      -> skipped
//   step=4 bLeftDouble=true                               -> skipped
//   step=5 moverSelectedThisFrame=true (else clean)       -> gated
//   step=6 all gates clean + mouseX=-1                    -> skipped (off-screen)
//   step=7 all gates clean + mouseY=INT_MAX               -> skipped (off-screen)
//   step=8 all gates clean + center pixel (640,360)       -> miss
//
// Signal-reality note: RunGameplayPickSelfTest() runs at the tail of
// RenderWorld::init(), which executes BEFORE any scene frame has been
// rendered into the FBO. Attachment-2 (the R32_UINT object-id buffer)
// is freshly cleared to 0 at init; lookupAtPixel() will read raw=0
// and return LookupResult{isValid=false}. Step 8 therefore expects
// outcome=miss DETERMINISTICALLY (NOT hit-or-miss). This reduces step
// 8 from a "lookup correctness check" to a SPINE REACHABILITY check:
// it proves the gate chain (env -> gesture -> mover-fallback ->
// viewport -> bounds -> coord-scale) reaches lookupAtPixel without
// short-circuiting. Whether lookupAtPixel returns hit on a live scene
// is a substrate-level concern verified by the M1.5 OBJECT_ID_SELFTEST
// passive canary, NOT by this gate.
//
// Result lines mirror [RENDER_WORLD_SELFTEST v1]:
//   [GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all
//   [GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=N expected=<...> actual=<...>
//
// FAIL is a STOP: indicates the extracted helper diverged from M1.6
// gate semantics.
//----------------------------------------------------------------------

// Local helper: build a "clean" request (gates pass; would reach
// lookupAtPixel) and let callers mutate one field per step.
static GameplayPickRequest selftestCleanRequest() {
    GameplayPickRequest req{};
    req.mouseX                  = 640;   // viewport-center on 1280x720 logical canvas
    req.mouseY                  = 360;
    req.shiftDn                 = true;
    req.leftClicked             = true;
    req.bGui                    = false;
    req.bLeftDouble             = false;
    req.moverSelectedThisFrame  = false;
    return req;
}

// Local helper: outcome enum -> stable string for FAIL line.
static const char* selftestOutcomeName(GameplayPickResult::Outcome o) {
    switch (o) {
    case GameplayPickResult::Outcome::skipped: return "skipped";
    case GameplayPickResult::Outcome::gated:   return "gated";
    case GameplayPickResult::Outcome::miss:    return "miss";
    case GameplayPickResult::Outcome::hit:     return "hit";
    }
    return "unknown";
}

void RunGameplayPickSelfTest()
{
    if (!mc2::gate::get<"MC2_GAMEPLAY_PICK_SELFTEST">()) return;

    // Defense: substrate must be on for step 8 to be meaningful. If
    // substrate is off, every step 1-7 still asserts correctly, BUT
    // step 8 would assert skipped (which IS the expected outcome under
    // substrate-off) instead of hit/miss. To avoid the confusion of
    // "PASS step=all" meaning two different things depending on
    // substrate state, refuse to run with substrate off.
    if (!RenderWorld::IsObjectIdBufferEnabled()) {
        mc2::fmt::print_err("[GAMEPLAY_PICK_SELFTEST v1] result=SKIP reason=substrate_off\n");
        return;
    }

    using Outcome = GameplayPickResult::Outcome;

    // Step 1: shiftDn=false -> skipped
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.shiftDn = false;
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::skipped) {
            mc2::fmt::print_err("[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=1 expected=skipped actual={}\n", mc2::fmt::safe(selftestOutcomeName(r.outcome)));
            return;
        }
    }

    // Step 2: leftClicked=false -> skipped
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.leftClicked = false;
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::skipped) {
            mc2::fmt::print_err("[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=2 expected=skipped actual={}\n", mc2::fmt::safe(selftestOutcomeName(r.outcome)));
            return;
        }
    }

    // Step 3: bGui=true -> skipped
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.bGui = true;
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::skipped) {
            mc2::fmt::print_err("[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=3 expected=skipped actual={}\n", mc2::fmt::safe(selftestOutcomeName(r.outcome)));
            return;
        }
    }

    // Step 4: bLeftDouble=true -> skipped
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.bLeftDouble = true;
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::skipped) {
            mc2::fmt::print_err("[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=4 expected=skipped actual={}\n", mc2::fmt::safe(selftestOutcomeName(r.outcome)));
            return;
        }
    }

    // Step 5: moverSelectedThisFrame=true (else clean) -> gated
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.moverSelectedThisFrame = true;
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::gated) {
            mc2::fmt::print_err("[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=5 expected=gated actual={}\n", mc2::fmt::safe(selftestOutcomeName(r.outcome)));
            return;
        }
    }

    // Step 6: clean gates + mouseX=-1 -> skipped (off-screen)
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.mouseX = -1;
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::skipped) {
            mc2::fmt::print_err("[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=6 expected=skipped actual={}\n", mc2::fmt::safe(selftestOutcomeName(r.outcome)));
            return;
        }
    }

    // Step 7: clean gates + mouseY huge -> skipped (off-screen)
    {
        GameplayPickRequest req = selftestCleanRequest();
        req.mouseY = 100000;  // larger than any plausible vMulY
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::skipped) {
            mc2::fmt::print_err("[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=7 expected=skipped actual={}\n", mc2::fmt::safe(selftestOutcomeName(r.outcome)));
            return;
        }
    }

    // Step 8: clean gates + center pixel -> miss (DETERMINISTIC at init time)
    //
    // init() runs pre-frame; attachment-2 is freshly cleared to 0, so
    // lookupAtPixel returns isValid=false and tryGameplayPick reports
    // outcome=miss. The assertion is spine-reachability: NOT skipped
    // AND NOT gated. We accept outcome=hit as a PASS too (defensive --
    // if a future init-order change renders something into the FBO
    // before this self-test runs, hit is the correct outcome and we do
    // not want to flake), but the EXPECTED outcome is miss.
    {
        GameplayPickRequest req = selftestCleanRequest();
        GameplayPickResult r = tryGameplayPick(req);
        if (r.outcome != Outcome::miss && r.outcome != Outcome::hit) {
            mc2::fmt::print_err("[GAMEPLAY_PICK_SELFTEST v1] result=FAIL step=8 expected=miss actual={}\n", mc2::fmt::safe(selftestOutcomeName(r.outcome)));
            return;
        }
    }

    mc2::fmt::print_err("[GAMEPLAY_PICK_SELFTEST v1] result=PASS step=all\n");
}
