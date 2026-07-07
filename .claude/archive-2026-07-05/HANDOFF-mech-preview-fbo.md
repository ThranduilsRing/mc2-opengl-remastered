# HANDOFF: mech-preview-panel FBO composite — mid-debug, root cause not fully found

Session: UI-phase1 integration (modder's ImGui UI merge) → cursor-z-order fix → mech-bay/mechlopedia
3D preview blank bug. Still blank as of this handoff. Use subagents heavy, harness first.

## Where things live
- Worktree: `A:/Games/mc2-ui-phase1` (branch `claude/ui-phase1-integration`, off nifty-mendeleev)
- Deploy: `A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0`
- Build: `cmake --build build64 --config RelWithDebInfo --target mc2` (VS cmake path, see CLAUDE.md)
- Deploy cmd: `py -3 scripts/deploy_payload.py "A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0" --source-root . --build-dir build64/RelWithDebInfo --exe-name mc2.exe --allow-new-target`
- **ALWAYS after deploy**: `cp /tmp/uimerge/run-with-log.bat.user-backup <deploy>/run-with-log.bat` — user customized this file, deploy_payload.py clobbers it with stock, user asked to stop that.

## Offline harness (built this session — USE IT, don't ask user to test manually)
`py -3 scripts/mechbay_preview_harness.py bay --frame 1200 --timeout 35 --win-after-sec 2`
- Stock campaign (name `"campaign"`, no MC2_ACTIVE_MOD), boots via MC2_BOOT_TO_BAY.
- Stock boot does NOT skip to logistics — it launches the real first mission. Needed
  MC2_SOAK_AUTOWIN=1 + MC2_SOAK_WIN_AFTER_SEC to actually finish it.
- New engine flag added: `MC2_SOAK_STOP_AT_BAY=1` (missionbegin.cpp) — parks at the bay
  screen instead of auto-advancing through purchase/loadout/launch. Without this the
  soak blows straight through the bay into mission 2.
- Screenshot: `MC2_SCREENSHOT_SOURCE=backbuffer` (NEW flag, gameosmain.cpp) — the
  existing `MC2_SCREENSHOT_AT_FRAME` hook only ever captured the in-mission scene FBO
  (useless for logistics/menu screens, which never touch it). `backbuffer` mode reads
  real FBO 0 instead — needs a non-minimized window, that's fine here.
- Read result: `py -3 -c "from PIL import Image; Image.open('...tga').save('...png')"`
  then view the PNG.
- `preview_debug.log` (deploy root) has extensive `[PREVIEW]` diagnostics gated by
  `MC2_LOG_PREVIEW=1` (harness sets this). `grep "post-draw"` etc.

## The bug
Mech Bay / Mechlopedia (Mech + Building screens) / Mech Purchasing / Pilot Ready /
Mech Lab / Mission Briefing / Options-Gameplay / MP Prefs / MP Setup / Salvage all show
an EMPTY 3D mech-preview panel. Confirmed via harness screenshot (Mech Bay: stats/text
populate correctly, the "BUSHWACKER" preview box is solid empty).

## Architecture (this part IS correct, user's own idea, keep it)
Legacy `SimpleCamera`/MLR mech-preview rendering was written assuming the real screen
is 800x600 (matches `Environment.screenWidth` staying 800x600 by design — do NOT try
to make it track real resolution, that breaks other legacy tuned-resolution code
elsewhere, already tried + reverted, see git history / `g_hudResClampEnabled` back to
default `true` in gameos_graphics.cpp). Correct fix: let the legacy math keep believing
800x600, render into a FIXED 800x600 offscreen FBO, then composite that texture
(UV-cropped to the camera's own `bounds[]` sub-rect) into the real ImGui panel via
`GuiRuntime::DrawUiImage`. This composite step is CONFIRMED WORKING (`DrawUiImage`
returns 1, texture handle valid, panel rect lands in the right screen location) — the
remaining bug is that the mesh renders **zero pixels** into the FBO in the first place.

### New API (gameos_graphics.cpp + gameos.hpp)
- `gos_BeginCameraPreviewRender()` / `gos_EndCameraPreviewRender()` — bind/unbind a
  fixed-800x600 FBO (`ensureCamPreviewFbo`), restore real viewport after.
- `gos_GetCameraPreviewTexture()` — GL texture handle of the color attachment.
- `SimpleCamera::setPreviewOffscreen(bool)` / `SimpleCamera::drawPreviewToPanel(x,y,w,h)`
  (simplecamera.h/cpp) — opt a camera into the offscreen path + composite call.
  Gated per-caller (NOT inferred from `mission==NULL`) because Mech Bay has no
  defs/ImGui panel for this at all (no `hasDefsUiPage()`, no `Gui3DView` in
  `mcl_mb_layout.fit`) — those callers pass real-pixel rect via `tbSx`/`tbSy` scale
  instead of a defs panel key.
- Wired into: `mechlopedia.cpp` (Mech+Building screens — real Gui3DView keys
  `game.mcl_en_mechs.3dview.walking_mech_should_be_centered_here` /
  `game.mcl_en_bldg.3dview.rotating_picture_of_building_goes_here_davion_version`),
  `mechbayscreen.cpp`, `mechlabscreen.cpp`, `missionbriefingscreen.cpp`,
  `optionsarea.cpp`, `mpprefs.cpp`, `mpsetuparea.cpp`, `salvagemecharea.cpp`,
  `logisticsmechdisplay.cpp` (shared by mechpurchasescreen.cpp + pilotreadyscreen.cpp).

## Two real bugs found + fixed this session (confirmed via harness, neither was THE fix)
1. **`SimpleCamera::render()` never called `TG_Shape::SetCameraMatrices()`** (the base
   `Camera::render()` does, camera.cpp:2313) — so the global static
   `TG_Shape::s_worldToClip` (consumed by `TG_MultiShape::Render`, msl.cpp:2068) was
   whatever the last WORLD camera left it at, not this preview camera's matrix. Added
   the missing call in simplecamera.cpp right before `theClipper->StartDraw(...)`.
   Confirmed matrix is now sane (logged, not degenerate). Made no visible difference
   alone — bug #2 was still masking it.
2. **`gos_State_IsHUD=1` defers triangles into a batch queue instead of drawing them.**
   `gosRenderer::drawTris` (gameos_graphics.cpp:6155) checks `renderStates_[gos_State_IsHUD]`
   — if true, it pushes into `hudBatch_` and RETURNS, never rasterizing immediately. The
   ENTIRE Mech Bay/logistics render runs with IsHUD=1 (mechcmd2.cpp brackets
   `logistics->render()` in `gos_SetRenderState(gos_State_IsHUD,1)`). Added
   `gos_SetRenderState(gos_State_IsHUD, 0)` in SimpleCamera::render() for the
   `isPanelPreview` case, right after `gos_PushRenderStates()` (auto-restored by the
   existing `gos_PopRenderStates()` at scope end). **Confirmed working** via a
   diagnostic added directly in `drawTris` (`isHUD=0` now logged correctly for the
   mech's triangle draws).

## Where it stands RIGHT NOW (unsolved)
Despite both fixes, the FBO is STILL empty (full-800x600-pixel-grid scan finds zero
non-background pixels — added this scan directly in simplecamera.cpp's post-draw
diagnostic, `nonBgCount=0` every time).

**New clue, not yet chased down**: added an FBO-binding check inside `drawTris` itself
(`curFbo` via `glGetIntegerv(GL_FRAMEBUFFER_BINDING,...)`, logged alongside `isHUD`).
Result: **`curFbo=0`** at the moment of the mech's triangle draw calls — but
`simplecamera.cpp`'s own "pre-draw" log (right before calling `pObject->render()`,
which is what eventually calls into `drawTris`) shows **`fbo=8`** (our correct preview
FBO) at that point, and `fboAfterBegin=8` right after `gos_BeginCameraPreviewRender()`
too. **Something in the call chain between simplecamera.cpp's `pObject->render()` call
and the actual `gos_DrawTriangles`/`drawTris` call rebinds FBO 0.**

Call chain to search (not yet done):
`SimpleCamera::render()` → `pObject->render()` (ObjectAppearance, appear.h) →
`Mech3DAppearance::render` (mech3d.cpp, has the `shouldRender=1` trace around line
2803-2808) → `mechShape->Render(true)` (mech3d.cpp:3001, `mechShape` is
`TG_MultiShapePtr` — NOT `TG_TypeMultiShape`, those are unrelated same-named-ish
classes, don't confuse them) → `TG_MultiShape::Render` (msl.cpp:2050) →
`listOfShapes[i].node->Render(...)` → `TG_Shape::Render` (tgl.cpp:3059) → eventually
`gos_DrawTriangles(gVertex, 3)` (tgl.cpp:3230, inside the `drawOldWay` branch) →
`gosRenderer::drawTris` (gameos_graphics.cpp:6149).

**Next step**: add the same `glGetIntegerv(GL_FRAMEBUFFER_BINDING,...)` diagnostic at
each hop in that chain (start with the top of `Mech3DAppearance::render` — note it
calls `mechShape->SetTextureHandle(0, localTextureHandle)` very early, BEFORE the
shouldRender check — texture handle/upload paths are a prime suspect for an FBO
rebind, e.g. mipmap generation via a scratch FBO that "restores" to 0 instead of the
previous binding). Also check `gos_SetRenderState(gos_State_Texture, ...)` at
tgl.cpp:3207 and whatever `selectBasicRenderMaterial`/`gosRenderMaterial::setTransform`
do inside `drawTris` itself (lines ~6176-6198) — didn't get to check those this
session. Bisect with the harness (fast loop — rebuild, deploy, rerun harness script,
grep `preview_debug.log`, no manual clicking needed) rather than guessing blind.

## Separate, untriaged bugs (user mentioned, not investigated at all)
- Mech icons and pilot icons (2D icon textures, not the 3D preview) not displaying
  correctly in the new UI. Different subsystem, zero investigation done.
- Encyclopedia (Mechlopedia) mech list not populating — can't even select a mech to
  preview there. Separate from Mech Bay (which DOES populate/select fine). Zero
  investigation done — was going to be next after Mech Bay, per user's priority order
  ("get mech bay working" first, ignore encyclopedia for now).

## Open merge-conflict markers from the original UI-phase1 merge (unrelated to this bug,
still worth resolving eventually)
`grep -rn "MERGE-CONFLICT-UI-PHASE1"` across the repo — last known open ones:
`GuiRuntime.cpp` (editor init entry point ambiguity — does EditRel.exe call
`InitEditorOpenGLOnly()` or `Init()`? not verified), `tgl.cpp` (2 skipped GPU-shape
emission branches, `bShadersDrawPathEnabled` default-flip skipped — deliberately not
applied, risk to PBR/GPU-offload work). Re-grep for current authoritative list, some
may have been resolved inline during this session's work.

## Discipline notes for next session
- User explicitly asked for subagent-heavy / harness-first work. The mechbay harness
  above is the payoff — use it before asking the user to test anything.
- User is caveman-mode; keep responses terse.
- Don't re-attempt the "make Environment.screenWidth real resolution" approach — it
  was reviewed adversarially by multiple agents and rejected (breaks mechicon.cpp /
  keyboardref.cpp / loadscreen.cpp tuned-resolution branches, gets re-clamped
  elsewhere). The fixed-800x600-FBO approach is the correct direction.
