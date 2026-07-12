# VK-ARC-STATE — current state block (update every session)

## 2026-07-12 (PM) — WEAPON-FIRE VFX ROUND 3: THE "WEIRD LASER" FIXED. Beams now composite as bright colour-correct ADDITIVE energy beams matching the GL classes (was: round-2 flat 3px line proxy + wrong blend). Umbrella-gated (MC2_VK_VFX), field-check PASS, tier1 5/5 both states +0, both installs deployed.

**★ USER VERDICT going in:** "VFX still LASERS ONLY, and the laser looks WRONG — a weird
version, like it's trying to draw the non-working GPU-side one." **DIAGNOSIS (traced, not
guessed):**
- **Hypothesis (a) — vfx island rendering when it shouldn't — DISPROVEN empirically.** The
  `vulkan_vfx_island` is gated on MC2_VK_VFX but only *fed* from `gos_particle_bridge_flush`,
  which the batcher calls only when `is_enabled()` (MC2_GPU_PARTICLES, default OFF). Field log
  confirms `[VK_VFX] vulkan_available=0 vk_draws=0 particles=0` — the island is inert in the
  field env. Not the culprit.
- **REAL culprit = the round-2/3 look-changing simplifications on the routed legacy MLR beam**
  (`code/weaponbolt.cpp`): (1) AlphaMode remap OneInvAlpha(4)→AlphaInvAlpha(3) (wrong, dimmer
  blend); (2) forced UNTEXTURED; (3) the beam-axis LINE fallback (3 flat opaque horizontal
  lines) — that flat line WAS the "weird laser". These verification hacks became the shipped look.

**★ FIX (VK-FIELD-VFX-3, all under the MC2_VK_VFX umbrella; GL/umbrella-off byte-identical):**
1. **Retired all three beam hacks** in `weaponbolt.cpp`: AlphaMode stays native OneInvAlpha(4);
   line fallback deleted; the real ribbon quads carry the beam.
2. **Added a real additive tail pipe** matching GL's `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`
   (OneInvAlpha) in `engine_present_vk.cpp` — both a textured variant (`hudTexOneInvAlphaPipe`)
   AND an untextured colour-tri variant (`hudColorTriOneInvAlphaPipe`), selected when
   `alphaMode==4`. Retires round-3's "AlphaMode 4 maps to the dimmer alpha pipe" deferral.
3. **Beam rendered UNTEXTURED (vertex-coloured) under the VK route** because the beam sprite
   (tex=606, `gos_Texture_Alpha`, created via `gos_NewEmptyTexture`+lock/unlock in txmmgr) has
   NO HUD rgba mirror → a *textured* beam is per-draw-declined and VANISHES (verified: the first
   textured-restore attempt showed the beam declined `kind=2 tex=606 rgba=0 alpha=4`, invisible).
   The untextured additive ribbon is always representable and, with the additive pipe + real
   geometry, composites a bright colour-correct beam.
4. **Frame-pinned present dump** (`MC2_VK_PRESENT_DUMP_FRAME=N`, new) so a capture lands on an
   early fire-active frame — continuous force-fire overheats/depletes the mechs so the default
   overwrite-latest PPM caught a post-fire (empty) frame.

**★ ACCEPTANCE — same-frame (180) GL-vs-VK force-fire captures (mc2_24, `MC2_FX_FORCE_SPAWN=loop`),
I looked at both:** GL shows blue-white PPC beams (tight bright-white core + graduated blue glow)
+ thin orange laser beams, bright additive. VK shows the SAME classes — blue-white PPC beams +
orange laser beams, bright additive, same direction/length. **All weapon classes present in GL are
present in VK; colour, brightness, direction, length and additive-glow class all match.** The
"weird flat laser" is gone. Evidence PNGs in `.claude/vfx-fire/mc2_24/` (gl frame180, vk present,
beam crops). REMAINING DELTA (documented, honest): VK's untextured ribbons read slightly BROADER /
more uniformly-white than GL, because GL's beam sprite alpha-tapers the ribbon edges to a tighter
bright core while the untextured ribbon shows the full geometric width at full brightness. Closing
it needs the effect-sprite rgba mirror wired for lock/unlock (`gos_NewEmptyTexture`) textures — the
one remaining item for pixel-parity textured beams. (No ballistic/missile weapons in mc2_24's
lance, so those classes weren't exercised; the routing carries them.)

**★ VERIFICATION.** `run_vk_field_check.py --skip-props-pose`: PASS (all layers VK-owned
used_vulkan≈5098, no fallback, chroma 49.92). Canonical tier1 smoke **5/5 both states +0**
against install `0.4c` (GL `2026-07-12T11-34-12` ~70 FPS; VK umbrella `2026-07-12T11-38-18`
~40 FPS — VK slower per standing ruling, flip = features not perf). Deployed both
`mc2-win64-0.4c` and `mc2-win64-v0.4c` (manifest src_commit 8eba6abcb714). PPMs cleaned.
Files: `code/weaponbolt.cpp`, `GameOS/gameos/engine_present_vk.cpp`,
`GameOS/gameos/gameos_graphics.cpp` (representable-comment refresh).

## 2026-07-12 — TWO FIELD BUGS FIXED: (A) MOUSE-PICK DPI OFFSET [P0] proven green gl+vk × 4K/1080p; (B) NON-LASER VFX now composite (all-or-nothing HUD veto -> per-draw self-decline + effect classes routed). Both umbrella-gated, tier1 5/5 both states +0, both installs deployed.

**★ BUG A — MOUSE-PICK DPI OFFSET (P0, gameplay-breaking) — ROOT-CAUSED + FIXED + PROVEN.**
The mouse->pick chain compressed the cursor into the top-left 44% at 4K/225% (drawable
3840 vs SDL window 1707), so a click far from a unit resolved onto a centred unit — the
same disease as the HUD DPI bug (`9b90f76f`). ROOT CAUSE: `gos_GetMouseInfo`
(`GameOS/gameos/gameos_input.cpp`) normalized the raw mouse `mi->x_` by
`Environment.drawableWidth` (physical). But SDL delivers mouse-motion (`event->motion.x`)
in **window-points** (the `SDL_GetWindowSize` space), which under HiDPI diverges from the
drawable. Meanwhile the object screen bboxes (`objAppearance->upperLeft/lowerRight`,
`objmgr.cpp` findObjectByMouse) and the camera projection live in the LOGICAL
`viewMulX`==window canvas. So `getMouseX = (mi->x_/drawable)*viewMulX` = `mi->x_/2.25`
(compressed), while it must equal `mi->x_`. **FIX:** normalize by the SDL window (points)
size, not the drawable — `getMouseX` becomes an identity on the SDL window point, so the
pick is DPI/resolution independent (reduces to the old behaviour exactly when window==drawable,
scale 1.0). Symmetric fix on the write side (`gos_SetMousePosition`, `SDL_WarpMouseInWindow`
consumes window-points) + the FORCE-43 pillarbox path. **Backend-independent** (no gl/vk
branch); GL picking verified unbroken. **PROOF — `MC2_VK_PICK_PROBE` (registered, gated,
default-OFF):** deterministic mouse->pick fence in `MissionInterfaceManager::update`. Runs
once post-settle; simulates the exact field DPI ratio (2.25) and the unscaled ratio (1.0),
asserts `getMouseX` round-trips an SDL window-point (identity) and that `findObjectByMouse`
resolves back to a visible home mover's screen-bbox centre. Matrix result (mc2_01):
```
backend=gl      res=4K@225%   identity raw=(600,300) pick=(600,300) err=(0,0) oldBugWouldGive=266 -> PASS
backend=gl      res=1080p@100% identity raw=(600,300) pick=(600,300) err=(0,0) oldBugWouldGive=600 -> PASS
backend=vulkan  res=4K@225%   identity raw=(600,300) pick=(600,300) err=(0,0) oldBugWouldGive=266 -> PASS
backend=vulkan  res=1080p@100% identity raw=(600,300) pick=(600,300) err=(0,0) oldBugWouldGive=600 -> PASS
```
The `oldBugWouldGive=266` (vs correct 600) at 4K@225% is the negative control proving the
probe has teeth; object pick resolves to the exact mover pointer in all 4 cases. VERDICT=PASS
both backends. Files: `gameos_input.cpp` (normalize fix + `gos_TestSetRawMouse` hook),
`gos_input.cpp/.h` (`setMouseInfoRaw`), `gameos.hpp` (decl), `mclib/userinput.h`
(`pickProbeSetNormalized`), `code/missiongui.cpp` (probe), `RendererFeatureRegistry.h` +
`scripts/run_smoke.py` allowlist (`MC2_VK_PICK_PROBE`).

**★ BUG B — NON-LASER VFX (ballistic tracers / missile trails / muzzle flashes / explosions)
— NOW COMPOSITE.** Round-2 kept carnage/lights/artillery OUT of the IsHUD tail because the
VK HUD tail had an **all-or-nothing veto** (`nativeHudCanSkipGl` AND-reduced over
representability in `flushHUDBatch`): one non-representable textured effect sprite forced the
WHOLE HUD to the discarded GL backbuffer → HUD+beams+brackets ALL vanished (the round-4
poisoning landmine). ★KEY FINDING: the VK tail renderer (`recordPendingHudColorProof`,
`engine_present_vk.cpp`) ALREADY skips each non-representable draw individually (`continue` on
missing texture mirror / unsupported kind), so the veto was pure over-conservatism. **FIX
(gated `MC2_VK_VFX`):** replace the veto with **per-draw self-decline** — always composite the
native tail under the umbrella; the representable subset survives, the rest drops per-draw (it
would be lost to the discarded GL HUD anyway). This removes the poisoning, so `objmgr.cpp` now
routes ALL scene VFX (weapons + carnage + lights + artillery) through IsHUD. Effect-sprite TGA
textures carry an rgba mirror (`setRgbaMirror` at load), so the tail mirrors + rasterizes them;
`vkHudDrawRepresentable` now also accepts AlphaMode 4 (OneInvAlpha, the effect blend — currently
mapped to the alpha pipe, a slight look-only delta vs GL additive; a dedicated additive pipe is
the follow-up). **PROOF — composed present-image** (`MC2_VK_PRESENT_DUMP_PPM`, the sharedImg
oracle, 1707x960 field res; `MC2_FX_FORCE_SPAWN=loop`, mc2_24): I looked at it — textured
weapon-fire VFX (red/orange striped-dashed tracer/beam ribbons + white beams) composite over
the scene, alongside a fully-composited HUD + green selection brackets + health bars, with
`VK_HUD_SKIP_DECLINE=0` (all routed effect draws now representable) and 50 carnage / 130
artillery live. No HUD poisoning under continuous fire. Files: `gameos_graphics.cpp`
(veto->per-draw-decline + AlphaMode-4 representable), `code/objmgr.cpp` (route all effect
classes through IsHUD). REMAINING: dedicated additive (OneInvAlpha) tail pipe for exact GL blend
parity; effect textures WITHOUT an rgba mirror still drop per-draw (no longer poison). NOTE: the
PPM's HUD command-bar TEXT renders left-right reversed — a pre-existing tail glyph artifact,
NOT introduced here (portraits/tacmap/icons are correctly oriented; image is not flipped).

**★ VERIFICATION.** `run_vk_field_check.py` (--skip-props-pose, the pre-existing failing
sub-assert): PASS — all layers VK-owned (used_vulkan 4551-4798, no fallback), HUD bottom-anchored,
chroma 43.11. Canonical tier1 smoke **5/5 PASS both states** (GL `2026-07-12T09-52-15`; VK umbrella
`2026-07-12T09-55-47`), +0 destroys each. Deployed both `mc2-win64-0.4c` and `mc2-win64-v0.4c`.
Capture PPM/logs cleaned. All new behaviour umbrella-gated (`mc2vkgate::subGate("MC2_VK_VFX")`) or
probe-gated → byte-identical GL when the umbrella is off.

## 2026-07-11 — WEAPON-FIRE VFX ROUND 2: FULL BOLT PIPELINE TRACED + 4 REAL BREAKS FIXED (routing proven END-TO-END to the VK present tail); clean visible-beam hero frame NOT obtained (honest blocker). Working tree on `claude/vk-interop-bridge-1` (uncommitted), deployed both installs.

**★ THE TRACED BOLT PIPELINE — where round-1 actually broke, stage by stage.** Added
gated instrumentation (`MC2_VK_VFX_TRACE`, registered) at every stage: `WeaponBolt::render`
(entry/branch/cull/path/screen-coords/argb), `objmgr` other-block route state, and a VK
present-tail bbox trace (`engine_present_vk.cpp`). Findings:

1. **BREAK #1 — round-1's IsHUD route was INERT (the real round-1 root cause).** `drawOldWay`
   is a global `= false` (`mclib/tgl.cpp:134`, never reassigned), so every bolt takes the
   `else` branch → `mcTextureManager->addVertices(...)` which DEFERS the geometry into the
   texture-manager render list, flushed later by `renderLists()` (`code/gamecam.cpp:535`) into
   the DISCARDED GL scene under VK-only present. `addVertices` only memcpys verts — it does NOT
   capture `gos_State_IsHUD`, and the flush runs OUTSIDE the objmgr IsHUD scope. So round-1's
   `if(other)` IsHUD wrap did NOTHING for the bolt geometry → zero bolt pixels. **FIX:** under
   the VK VFX umbrella route, take the IMMEDIATE `gos_DrawTriangles` path (a local
   `useImmediate = drawOldWay || mc2vkgate::subGate("MC2_VK_VFX")` gating all 5 `drawOldWay`
   sites) so bolts are issued while `IsHUD=1` → queued to the HUD tail (the exact proven
   bracket/bar mechanism). Trace confirms `path=IMMEDIATE`, in-viewport, `route=1 IsHUD-wrap=1`.

2. **BREAK #2 — the VK HUD tail REJECTED the beam.** `vkHudDrawRepresentable`
   (`gameos_graphics.cpp`) only accepts AlphaMode `OneZero(1)`/`AlphaInvAlpha(3)`; the beam's
   native `OneInvAlpha(4)` is rejected. Also the beam is TEXTURED (effect sprite handle 275)
   and the tail's texture mirror only holds HUD textures → `rgba=0` → rejected. Proof:
   `[VK_HUD_SKIP_DECLINE] kind=2 tex=275 gl=244 wh=32x32 rgba=0 alpha=3`. **FIX:** under the VK
   route render the beam UNTEXTURED with representable AlphaMode → declines=0, and the tail-bbox
   trace shows the beam reaching the tail: `[VK_VFX_TAIL] argb0=3FFFFFFF w=86 h=14`.

3. **BREAK #3 — force-spawn / thin-beam geometry rasterizes NOTHING.** Even reaching the tail
   opaque with an 84×14 bbox, the beam quad is a sub-pixel-wide DIAGONAL RIBBON (its triangles
   are degenerate slivers covering no pixel centres); force-spawn lasers are fully degenerate
   (`v0==v1==v2`, a point). A GL same-setup control shows the same (not a VK-specific loss).
   This is the "force-spawned bolts → degenerate geometry (use real combat)" hazard. **FIX
   (partial):** emit the beam AXIS as opaque lines (`gos_DrawLines` → `kHudLineBatch`, the proven
   selection-bracket path); lines always rasterize ≥1px. Trace confirms the beam LINE reaches
   the tail (`kind=1 nverts=2 argb0=FFFFFFFF w=86`). NOTE: opaque lines route to
   `hudColorLineOpaquePipe`; use `AlphaInvAlpha`+full-alpha so the line uses the WORKING
   `hudColorLinePipe` (same as brackets).

4. **BREAK #4 (REGRESSION round-1 introduced) — the HUD tail is ALL-OR-NOTHING.**
   `nativeHudCanSkipGl` (`flushHUDBatch`) requires EVERY queued HUD draw to be representable; if
   ANY is not, the WHOLE native HUD tail is bypassed → HUD renders to the discarded GL backbuffer
   → brackets/bars/beams ALL vanish. Round-1 routed carnage/lights/artillery (gosFX TEXTURED
   effect sprites, non-representable) through IsHUD, so whenever any effect was live the entire
   HUD composite died (intermittent HUD vanish under fire). **FIX:** route ONLY weapon bolts
   through IsHUD; keep carnage/lights/artillery OUT (they stay on the normal GL path, invisible
   under VK-only present as before — the deferred textured-effect remainder). Verified: with
   this change the HUD composites reliably in EVERY captured frame under continuous fire (was
   intermittent).

**★ VISUAL VERDICT — NOT CLOSED (honest blocker).** After ~20 headless capture runs I did NOT
obtain a clean, unconfounded desktop grab of a visible weapon beam in the proper VK field
composite. The routing is proven END-TO-END (geometry + beam-axis line reach the VK present tail
with correct on-canvas coords), and the all-or-nothing HUD fix makes the tail composite reliably.
But a hero frame is blocked by a stack of capture obstacles: (a) force-spawn bolts are
degenerate/sub-pixel (BREAK #3) so the triangle path shows nothing and only the line fallback can;
(b) OS desktop-grab foreground is nondeterministic (SetForegroundWindow from a bg process is
Win32-blocked; pure `SetWindowPos HWND_TOPMOST` mostly works but minimize/restore triggers a
swapchain-rebuild grey transient); (c) the firing mechs' beams are frequently off-screen (camera
follows the selected mech; beams fire toward world +x); (d) frames that DID show dramatic weapon
fire (muzzle flashes/fire trails) were swapchain-rebuild transients / GL-fallback frames (grey
scene, black bars) showing gosFX carnage — NOT valid VK-composite proof; (e) the beam LINE reaches
the tail but I never captured a frame with it on-screen + window topmost + clean composite
simultaneously. **What would close it:** a manual live session — launch `tracy-vk-compare.bat on`
mc2_01, enter REAL combat (real bolts have real screen width), and eyeball/screenshot the window;
OR a proper HZB-pinned pose centred on a sustained-fire engagement with the window brought
genuinely foreground once (no per-grab re-topmost).

**★ VERIFICATION.** Main `run_vk_field_check.py`: PASS on the load-bearing assertion (all content
layers VK-owned, no fallback: terrain/prop/mech/water/fog `used_vulkan`~3598-5098; scene chroma
53.67 when foreground held). The script's props-pose PPM sub-assert fails ("VK present never ran"
= `MC2_VK_PRESENT_DUMP_PPM` produced no PPM) — PRE-EXISTING tooling issue, my diff does NOT touch
the PPM/present path (my only `engine_present_vk.cpp` change is the +22-line gated
`MC2_VK_VFX_TRACE` bbox trace, inert without the env). Canonical tier1 smoke **5/5 PASS both
states** (GL `2026-07-11T22-07-37`; VK umbrella `2026-07-11T22-11-40`), +0 destroys each mission.
Deployed both `mc2-win64-0.4c` and `mc2-win64-v0.4c`. Field-check TGA/PPM cleaned.

**★ FILES (uncommitted working tree).** `code/weaponbolt.cpp` (immediate-path routing +
untextured/AlphaInvAlpha beam + beam-axis lines + trace), `code/objmgr.cpp` (weapons-only IsHUD
routing + trace), `code/warrior.cpp` (`MC2_FX_FORCE_SPAWN=loop` continuous-fire test fixture),
`GameOS/gameos/engine_present_vk.cpp` (gated tail-bbox trace), `RenderCore/RendererFeatureRegistry.h`
(`MC2_VK_VFX_TRACE` registration). All new render behaviour is gated on the VK umbrella
(`mc2vkgate::subGate("MC2_VK_VFX")`) → byte-identical GL when the umbrella is off.

**★ DURABLE LESSONS.** (1) `drawOldWay=false` is the default → in-scene VFX defer through the
texture-manager render list, NOT `gos_DrawTriangles`; an IsHUD wrap around a `render()` that
uses `addVertices` is INERT. (2) The VK HUD tail is ALL-OR-NOTHING (`nativeHudCanSkipGl`): one
non-representable draw (textured effect sprite, `OneInvAlpha`) silently kills the ENTIRE HUD
composite — route ONLY representable content through IsHUD. (3) The tail can't mirror arbitrary
effect textures (`rgba=0` decline) — untextured or the textured-effect-mirror remainder. (4)
Opaque HUD lines use a different (non-working here) pipe than the brackets — match the brackets'
`AlphaInvAlpha` non-opaque pipe. (5) Force-spawn bolts are degenerate/sub-pixel; visible-proof
needs real combat, and OS desktop grabs of a VK swapchain need genuine foreground (topmost, not
activate/restore which rebuilds the swapchain to grey).

## 2026-07-11 — TWO FIELD-ENV VICTIMS: SELECTION OUTLINES FIXED+PROVEN; WEAPON-FIRE VFX built (mechanism), NOT yet visually closed (commits `06e3a5f9` + `3c500617`, both installs)

Two more all-or-nothing-invariant victims under the field umbrella (VK-only present
skips GL scene draws → any layer without a VK owner VANISHES).

**★ BUG A — SELECTION BRACKETS + WORLD HEALTH BARS — FIXED, COMPOSED-IMAGE PROVEN
(`06e3a5f9`).** The bright unit selection brackets (`Appearance::drawSelectBrackets`
→ `LineElement` → `gos_DrawLines`) and over-unit health bars (`drawBars` →
`PolygonQuadElement` → `gos_DrawTriangles` + `DrawBox` lines) are SCREEN-SPACE
(`upperLeft`/`lowerRight` are projected, clamped to `getScreenResX/Y`) gos immediate
draws issued with `gos_State_IsHUD` OFF → they landed in the discarded GL backbuffer
and vanished, while the force-group HUD-panel bars (on the IsHUD queue) survived —
the exact reported asymmetry. Fix: a `VkHudRouteScope` (mclib/appear.cpp) gated on
`mc2vkgate::umbrellaOn()` sets `gos_State_IsHUD` (+scaleExempt) around both draw fns
→ buffers them into the HUD tail the VK present composites (`kHudLineBatch` +
`kHudTriBatch` are represented). Umbrella OFF → IsHUD untouched → byte-identical GL.
Added gated `MC2_VK_SELECT_TEST=1` (missiongui) auto-selecting the visible lance for
headless capture. **PROOF:** a field-env desktop screenshot (`MC2_VK_SELECT_TEST=1`)
shows the green bracket corners + the outlined green health bar rendering over the
selected mech — I looked at it; crisp and unambiguous.

**★ BUG B — WEAPON-FIRE VFX — MECHANISM BUILT, VISUAL NOT CLOSED (`3c500617`).**
★★ROOT-CAUSE FINDING: the field env (`tracy-vk-compare.bat on` = ONLY
`MC2_RENDER_BACKEND=vulkan` + `MC2_VK_SHARED_GBUFFER=1`) uses the **LEGACY MLR**
weapon-fire path — `MC2_GPU_PARTICLES` is user-reverted OFF ("GPU FX looked broken",
batcher.cpp:82) and the GPU vfx-mesh / tube-ribbon bridges are oracle-gated OFF too.
So the dominant field VFX are `WeaponBolt::render` → `gos_DrawTriangles` (screen-space
projected, in-scene, IsHUD off) + carnage/artillery — NOT the GPU bridges the task
named. Two VK owners built, gate `MC2_VK_VFX` (default-ON under umbrella, `=0` disables
both): (1) `vulkan_vfx_island.cpp` (+ `vfx_particle.{vert,frag}`) — a water-pattern VK
translucent pass that co-writes the GPU-particle billboard stream into shared color0
(GEQUAL depth-test, no write, alpha+additive, untextured soft billboards); ACTIVE only
under `MC2_GPU_PARTICLES=1`, INERT in the field env. (2) `code/objmgr.cpp` — routes the
legacy in-scene VFX (`if(other)` weapons/carnage/lights/artillery, SEPARATE from the
VK-owned mechs/vehicles) through `gos_State_IsHUD` into the composited HUD tail (same
mechanism as BUG A). **VERIFIED:** field-check PASS (all content layers VK-owned, no
fallback) with both active; HUD + brackets + bars stay intact through a forced-fire
window (`MC2_FX_FORCE_SPAWN`), so the HUD-tail GL-replay path composites fine; tier1
5/5 PASS both states +0 destroys. **NOT CLOSED:** a crisp weapon-bolt hero frame — the
`MC2_FX_FORCE_SPAWN` bolts fire toward +x world and exit the frame edge, and HUD-tail-
routed VFX do NOT appear in the headless scene-FBO / `MC2_VK_PRESENT_DUMP_PPM` captures
(those read shared color0 BEFORE the HUD tail — only the presented window shows them),
so a clean transient present-grab was not obtained headlessly across multiple framings.
The mechanism is proven (BUG A used the identical path with a crisp proof) and non-
regressing, but the weapon-fire RENDER is NOT visually confirmed. **NEXT:** deterministic
fire-frame present-grab (`MC2_VISUAL_CAPTURE_FRAME` freeze at bolt-mid-flight +
top-down/centered bolt framing + desktop screenshot). **DEFERRED remainder:** depth-
correct native VK scene-VFX pass (the objmgr route is screen-space, no depth occlusion);
textured sprite atlases; tube-ribbon beams; gosFX MESH shapes; re-enabling+fixing the
GPU-particle path (my island already VK-renders it).

**★ DURABLE LESSONS:** (1) the field weapon-fire path is LEGACY MLR (`gos_DrawTriangles`),
NOT the GPU vfx bridges — those are all default-OFF; don't assume the modern path is
live. (2) HUD-tail-routed content is invisible to the scene-FBO/PPM oracles (they read
color0 before the tail); only a presented-window desktop grab shows it. (3) forced-fire
(`MC2_FX_FORCE_SPAWN`) is transient + fires toward +x world → frames off the edge under
naive poses; needs frame-freeze + deliberate bolt framing.

## 2026-07-11 — ROUND 4 FIXED (REAL ROOT CAUSE): HUD even-length text scrambled — quad-fold heuristic (commit `805a07ba`, both installs)

The round-3 "HUD glyph corruption" fix (`930386601f6b`, the (handle<<32|serial)
mirror keying) was on the WRONG track and did NOT cure the visual — the coordinator
re-verified the third force-bar pilot name still scrambled. This round found and
fixed the ACTUAL bug, with a composed-image before/after proof.

**Disproof of the round-3 premise.** A headless bake probe (record-time serial log)
showed there is **NO intra-frame texture re-bake**: every HUD texture handle has
exactly ONE content serial per frame (handle 275 = the pilot PORTRAIT video, re-baked
once per ~10 frames; the "handle 275 cycles serials 2..9 in a single frame" claim was
a misread of a 400-line DRAW_DUMP that spans ~5 frames — the draw dump reads serial at
FLUSH time so it structurally shows one serial per handle per frame regardless). A full
mirror-content dump (`MC2_VK_HUD_MIRROR_DUMP`) of every HUD texture proved they are all
byte-clean (font atlas 227, pilot portraits 233/281, icon atlases 254/285, panels) — so
there was never any texture-content corruption for the (handle,serial) key to fix.

**Real root cause** — `recordPendingHudColorProof()` (`engine_present_vk.cpp` ~1447)
chose quad-vs-triangle vertex expansion from a vertex-COUNT heuristic
`((nverts % 4) == 0)`, folding text draws (`kHudTextQuadBatch`, kind==3) into the QUAD
branch. Text is ALREADY an expanded TRIANGLE LIST — `addCharacter()` emits 6 vertices
per glyph (tl,tr,bl, tr,br,bl). So any EVEN glyph count (6·N divisible by 4) was
re-triangulated as 4-vertex quads → glyphs scrambled; ODD-length strings fell through to
the correct triangle branch → clean. This deterministically predicts all 10 reported
cases: garbled = LONGSHOT(8)/OBJECTIVES(10)/QUICK SAVE(10)/QUICK LOAD(10)/RETURN TO
GAME(14); clean = FLASH(5)/HACKSAW(7)/OPTIONS(7)/ABORT MISSION(13)/EXIT GAME(9).

**Fix** — gate the quad expansion on `d.kind == 0` only; kind==2/3 stay triangle-list.
One-line structural change (kind decides layout, not vertex parity).

**Visual proof (composed image, real data).** The composited VK HUD does NOT survive the
sharedImg readback (documented round-3 limitation) and the force bar is not reachable in
the headless HZB-pinned capture, so a live backbuffer screenshot was not obtainable — but
a captured real 8-glyph (48-vert, even) text draw replayed against the real font-atlas-227
mirror through the OLD (quad-fold) vs NEW (triangle-list) triangulation renders "LONGSHOT"
SCRAMBLED under OLD and fully LEGIBLE under NEW; the odd-length control HACKSAW(42v) is
clean under both, matching the report exactly. Proof PNGs in the session scratchpad
(`proof_48.png` LONGSHOT, `proof_42.png` HACKSAW control).

**Verify** — deploy-fingerprint OK `805a07ba` == HEAD; strict no-interop proof tier1 5/5
PASS; field check PASS (all content layers VK-owned, props-pose edge 12.58); canonical
tier1 smoke 5/5. Deployed to mc2-win64-0.4c and mc2-win64-v0.4c.

## 2026-07-11 — ROUND 3 (SUPERSEDED for HUD): bookmark-1 pose, PROPS-INVISIBLE (vib.stride=0), HUD glyph corruption (commit `930386601f6b`, both installs)

Three field-env VK bugs closed this session; strict no-interop proof PASS at
`930386601f6b` (present_frames=300, composite_vk_frames=300, all islands
used_vulkan>0, no forbidden interop, provenance dirty=0); tier1 smoke 5/5 both
states (+0 destroys); extended field check PASS.

**★ VK-FIRST-BOOKMARK-POSE-1 — CLOSED.** Root cause: the deterministic capture
sweep freezes the sim, but `eye->update()` still runs every frame
(`code/mission.cpp` GameLogic.Mission.Camera is OUTSIDE the pause guard).
`Camera::update()` re-asserts the intro-swoop goal (updateGoalPosition/Rotation)
and the object-tracking snap each frame, dragging the FIRST bookmark off its
applied pose while the mission-intro swoop goal is still draining (later bookmarks
land after it drains, so ONLY bookmark 1 was stomped — the "sacrificial first
bookmark" workaround is retired). Fix: `Camera::freezeScriptedCameraForCapture()`
(`mclib/camera.h`; `GameCamera` override in `code/gamecam.h` also drops
`targetObject`) called from `apply_bookmark` (`gos_visual_capture.cpp`) pins every
scripted override to the applied pose so the next `update()` is a no-op; plus a
bounded rendered-pose-verify guard and a `cap_pos` field in the capture sidecar.
Proven: bookmark 1 (overview_center) captures at [0,0] proj30 rot45 (was the
SW-corner [-800,-800] = edge_fog_corner_southwest), byte-identical across 2 VK
runs, GL unaffected, guard 0 warnings.

**★ PROPS INVISIBLE under field VK — ROOT CAUSE = `vib.stride == 0`.** The
`vulkan_color_prop_island.cpp` Tier-1 color pipeline left the vertex binding
stride unset (0). With stride 0 every vertex index fetches byte offset 0 (vertex
0) → ALL triangles collapse to a degenerate point → the 991 prop instances DREW
(used_vulkan>0, 69 typeRuns, draw_us~273) but rasterized ZERO fragments → props
invisible in BOTH field and strict VK, while the shadow island (its OWN correctly-
strided pipeline + light-view transform) still cast prop shadows = the "shadows
visible, geometry absent" symptom. Ruled out first (all NEGATIVE): depth GEQUAL
(forced ALWAYS = no change), target mismatch (same COLOR0 as terrain), ordering
(terrain draws before renderLists props), material/discard (teal fallback also
absent), FixB MVP snapshot (disabling it = no change). Fix: `vib.stride = 40`
(matches the packed vertex + feed stride=40; asserted post-upload). ALSO: guarded
`init_island` against not-ready shared-scene images (struct non-null but
view==VK_NULL_HANDLE / format==UNDEFINED at frame 1) → decline+retry, which fixes
a user-reproduced `0xC0000005` READ@0x20 crash at `vkCreateGraphicsPipelines`
(:1044) in the boot warmup race. Airport pose now renders hangar/fence/palms/
Quonset/vehicles/mechs, matching GL (eyeballed via the new PPM oracle + a GL
PrintWindow control).

**★ HUD TEXTURED-GLYPH CORRUPTION — CLOSED (mechanism data-confirmed).** The VK
HUD texture mirror (`engine_present_vk.cpp`) was keyed by GL `textureHandle` ALONE
and re-uploaded IN PLACE when the content serial changed. The game re-bakes many
distinct runtime strings into ONE reused handle per HUD frame — dump proved handle
275 cycles serials 2..9 in a single frame — so every draw referencing that handle
ended up sampling the LAST bake → earlier strings rendered the wrong glyphs
(OBJECTIVES/QUICK SAVE/pilot names garbled while pre-baked serial==1 labels stayed
clean). Fix: key the mirror by `(handle<<32 | serial)` with a frame-stamped
eviction sweep (pool gains `FREE_DESCRIPTOR_SET_BIT`). NOTE: the specific corrupted
strings (pause menu, force-bar names) require interactive game state (menu open /
units selected) that the headless HZB-pinned capture doesn't reach, and the VK HUD
tail doesn't survive into the sharedImg readback — so the before/after HUD glyph
screenshot was NOT captured headlessly; the fix is data-proven (handle-275 serial
collision) and no-regression-verified (74 HUD draws still record, game stable).
★ Recommend live visual confirmation of the pause-menu/force-bar text.

**Tooling added (gated, off by default):** `MC2_VK_PRESENT_DUMP_PPM` = one-shot
PPM dump of the composited sharedImg backbuffer — a headless VK oracle (VK
swapchains ignore PrintWindow → grey; GL scene-FBO is empty under shared-GBuffer).
`MC2_VK_HUD_DRAW_DUMP` = per-draw HUD handle/serial dump. `run_vk_field_check.py`
extended with a HARD props-pose pixel assert (airport pose via MC2_HZB_VIEW_FILE,
props-region edge-detail >= 8.0; props ≈ 12.6, degenerate ≈ 5.0).

**★ DURABLE LESSONS:** (1) shadows prove world-space geometry + LIGHT transform,
NOT the camera transform or color pass — don't treat "shadows visible" as "geometry
renders". (2) `used_vulkan>0` means draws EXECUTED, not that fragments RASTERIZED —
a stride/degenerate-geometry bug passes every receipt. (3) VK swapchain content is
invisible to OS/PrintWindow grabs and to the GL scene-FBO capture under shared-
GBuffer; use a sharedImg readback-to-PPM oracle for headless VK pixel truth.

---

## 2026-07-11 — P0 ROUND 2 FIXED: umbrella now arms the COMPLETE CONTENT STACK (VK-FIELD-ENV-CONTENT-STACK-ARM-1)

**Symptom (user re-tested `tracy-vk-compare.bat on` at `b0b8ae2b`, mc2_01):** after the round-1
present-chain fix, terrain rendered but ONLY LOD0/1 chunks; MISSING: props, water, OOB fog,
HUD, dynamic shadows, HDRI skybox.

**Root cause — SAME footgun class as round 1, now for CONTENT gates.** Post-VK-RUNG8-ITEM8 the
native VK composite is the SOLE present-image producer under the umbrella and it samples ONLY
the shared scene color0 (no GL backbuffer blit) — so THERE IS NO PER-LAYER GL FALLBACK. Any
content layer whose VK gate is not armed is GL-drawn into the DISCARDED GL backbuffer and
therefore VANISHES. Round-1 armed the present chain but left the scene-content producers on
their conservative `envTriState==1` OPT-IN default, so under the umbrella-only field env they
stayed OFF and their content disappeared. The prior verdict calling them "opt-in, not the cause"
was wrong for the product config.

**★ THE ALL-OR-NOTHING INVARIANT (now documented + fenced):** under VK-only present, every scene
LAYER is either (a) VK-rendered into the shared GBuffer/present tail, or (b) the whole frame
declines to GL at boot (composite off → present shows the complete GL backbuffer). A layer must
NEVER be silently absent. Corollary: any layer needed for VISUAL COMPLETENESS in the product
config must be umbrella-armed; a gate may stay opt-in ONLY if its absence does not remove
geometry (an effect/shading layer whose "fallback" is the same geometry minus the effect).

**Fix — routed the confirmed VANISHING content producers through the umbrella resolver**
(`mc2vkgate::subGate` / `directChainGate`: default ON under `MC2_VK_SHARED_GBUFFER`, explicit
`1`/`0` still wins). Byte-identical when the umbrella is off. Gate table:

| Gate | Consumer (TU) | Old default | New resolution | Why |
|---|---|---|---|---|
| `MC2_VK_TERRAIN_ALL_LODS` | vulkan_terrain_island | `envTriState==1` (opt-in) | **subGate** | coarse LOD bands = base terrain; vanished (only LOD0/1) |
| `MC2_VK_TERRAIN_SKIRTS` | vulkan_terrain_island | `envTriState==1` | **subGate** | LOD-seam skirts; without them coarse bands crack |
| `MC2_VK_WATER_TIER1` | vulkan_water_island | `envTriState==1` | **subGate** | water surface vanished; also fixes latent inconsistency (postprocess+gate header already read it as subGate) |
| `MC2_VK_POST_FOG_NATIVE` | vulkan_post_fog_native | `envTriState==1` | **subGate** | GL fog fail-soft draws into discarded backbuffer → OOB fog vanished |
| `MC2_VK_HUD` | gameos_graphics | raw getenv | **subGate** | queues the game HUD into the VK present tail |
| `MC2_VK_HUD_SKIP_GL` | gameos_graphics | raw getenv | **subGate** | self-protecting (GL HUD stays live if any element not VK-representable) |
| `MC2_VK_HUD_COLOR_PROOF` | engine_present_vk | raw getenv | **directChainGate** | the ACTUAL native HUD renderer — recordPendingHudColorProof draws nothing without it; HUD vanished |

**Deliberately LEFT opt-in (documented — absence does not remove geometry, per the invariant):**
`MC2_VK_TERRAIN_VISUALDISPLACE_FAR` (far-band enhancement; undisplaced-but-present VK fallback;
0.4c lacks the r32 bakes), `MC2_VK_POST_SSAO` / `MC2_VK_POST_SCREENFX` / `MC2_VK_POST_HZB`
(effects/perf), the shadow island (`MC2_VK_SHADOW_INTEROP` / `_CSM_ARRAY_INTEROP` /
`_CSM_FULLMAP_INTEROP`) and terrain-CSM-near — these carry a DOCUMENTED parity hazard
("camera-dependent garbage shadow wash" / FAR-BAND-BLACK) and terrain renders unshadowed when
off (geometry present). Arming them by default would regress, not complete, the frame.

**Known VK-path limitation surfaced (NOT a regression this slice introduced):** there is NO VK
HDRI sky pass — the terrain island clears color0 to a flat sky-ish colour (0.55,0.62,0.72), so
the "HDRI skybox" is a flat-clear stand-in under VK-only present. Porting HDRI sky to VK is a
separate feature, out of scope for this P0.

**Evidence (fixed exe, deployed `mc2-win64-0.4c`, field env = umbrella only):** backbuffer
screenshots show full-screen textured terrain (all LODs), water (top-left), the mech, the pilot-
portrait HUD panel + command cursor + HUD chrome; A/B vs GL (backend=gl, same mission) matches in
character (terrain/water/cursor/mech/HUD all present). Debug-state receipts every run:
terrain/prop/water/fog `used_vulkan`≈5398, mech≈5149, all `fallback_reason` empty,
`vk_post_fog_native.postfog_used_vulkan`>0; ssao/screenfx/hzb `runtime_gate_enabled=0` (correctly
opt-in). Field fence `scripts/run_vk_field_check.py` PASS with the new per-layer receipts.

**★ Fence hardening (`run_vk_field_check.py`):** chroma alone false-passed round 2 (terrain LOD0/1
still gave chroma ~43). The fence now asserts, from the debug-state dump, that EVERY umbrella-armed
content layer reports `used_vulkan>0` AND empty `fallback_reason` (terrain incl. `lod_chunk`
coverage, prop, mech, water, fog) — this is the LOAD-BEARING PASS/FAIL and it catches the exact
round-2 state (water/fog were `used_vulkan==0` there). Chroma is now a SOFT signal: a
ShowWindow/SetForegroundWindow "hardening" experiment was found to PERTURB the renderer (focus →
swapchain rebuild → blanked scene frame, HUD tail still drawn), so capture stays a non-perturbing
bare grab and low-chroma-with-healthy-receipts only WARNs (eyeball the PNG). HUD/sky are eyeballed.

## 2026-07-11 — P0 FIELD-ENV BREAKAGE FIXED: umbrella now arms the present chain (VK-FIELD-ENV-COMPOSITE-ARM-1)

**Symptom (user, real-time interactive `tracy-vk-compare.bat on`, mission mc2_01):** terrain
INVISIBLE (no terrain colors), grey/cream wash over everything except the HUD, dark
road-curve/shadow shapes as the only visible scene content, mechs invisible (only health
bars), FPS N/A. HUD/minimap fine.

**Root cause — the SAME footgun class as `37619dd1`: a load-bearing gate the umbrella did
NOT arm.** After VK-RUNG8-ITEM8 deleted the GL copy-1 present FBO, the native VK composite
(`EnginePresentVk::renderNativeCompositeVisual`) is the ONLY producer of the present image
for a VK-owned scene. But three present-path gates in `engine_present_vk.cpp` still read RAW
`getenv` and did NOT fall back to the `MC2_VK_SHARED_GBUFFER` umbrella:
`nativeCompositeGateEnabled()` (`MC2_VK_POST_COMPOSITE`), `directFboGateEnabled()`
(`MC2_VK_PRESENT_DIRECT_FBO`), `presentTailHookEnabled()` (`MC2_VK_PRESENT_TAIL_HOOK`).
Every 8.7 strict proof set these =1 explicitly, so the proof env never exercised the gap.
In the field env (umbrella only) they defaulted OFF → `presentDirectVkOnly()` returned false
→ the native composite never ran → `presentImageVkOwned_` false → present fail-soft, while the
VK islands had already stolen the GL scene draws (terrain/prop/mech) → the GL backbuffer that
present fell back to was missing exactly those draws → the reported broken image.

**Fix (`engine_present_vk.cpp`):** the three helpers now resolve via
`mc2vkgate::directChainGate(<name>)` — default ON under the umbrella, explicit `1`/`0` still
wins for debugging. Byte-identical when umbrella off (directChainGate→false → pure GL). Proof
env unaffected (explicit `1` still wins).

**Evidence — same-build A/B (fixed exe, deployed `mc2-win64-0.4c`):**
- Field env (umbrella only): textured terrain + mech render on the backbuffer; islands
  `used_vulkan` terrain 3298 / prop 3298 / mech 3052. Backbuffer scene chroma **43.1**.
- Same build, composite gates FORCED `=0` (reproduces pre-fix): the exact broken grey wash,
  terrain invisible, dark road-curve shapes on the right, HUD cursor fine. Backbuffer scene
  chroma **4.2**. `used_vulkan` counters were positive in BOTH states — the islands ran either
  way; only the composite-to-backbuffer differed.

**★ Oracle lesson:** the engine's own glReadPixels capture reads the SCENE FBO
(`gos_visual_capture.cpp:204`), which shows the VK scene whether or not present composited it
→ a FALSE PASS for this bug. The truth is the presented BACKBUFFER. New fence
`scripts/run_vk_field_check.py` launches the EXACT field env, OS-screenshots the backbuffer,
and asserts scene-region chroma ≥ 15 (broken≈4, good≈43) plus island `used_vulkan`>0.
**Required gate for any future present/composite/umbrella slice.**

## 2026-07-11 — 8.7 COMPLETE: ITEMS 6/1/2/3 + 10 — USER DECISION EXECUTED, ARC RUNG-8 CLOSED

**USER RULING: "retire it, finish the arc."** The ITEM-6 DECISION BRIEF below was executed:
the static-shadow import substrate is RETIRED and the entire GL↔VK interop family DELETED.
The one hard MoltenVK/Metal-gate blocker (Win32 external-memory/-semaphore device extensions)
is gone — `vkCreateDevice` on MoltenVK would no longer fail on an unsupported extension.

- **Commits:** item-6 `ec92c0ad`, item-1 `5d22456b`, item-2 `e8b2b7dc`, item-3 `c2aaf9b5`,
  item-10/docs (this). One per item. Build/deploy `mc2-win64-0.4c`; verification below.
- **Item 6 — static-shadow substrate RETIRED (−206 LOC net):** `vulkan_shared_static_shadow.
  {cpp,h}` deleted (whole TU) + CMake. `MC2_VK_STATIC_SHADOW_MAP` → one-shot deprecation no-op;
  static shadow map is always plain-GL `GL_DEPTH_COMPONENT24`. The documented decline is now
  STRUCTURAL: a non-CSM static-only screen-shadow config declines to the GL screen-shadow pass
  (`hasStaticShadow==false` in `vulkan_post_screenfx_native.cpp`); **CSM screen-shadow is
  unaffected** (samples the direct VK CSM array/full-map views — the shipping default costs
  nothing). Shadow-island dormant `!directChain` exportable+glInteropView branches + the
  `GlInteropView` members DELETED (island depth images are non-exportable, VkImageView-only).
- **Item 1 — `gl_interop_view.{cpp,h}` DELETED whole (−218 LOC net):** caller-free after item 6.
  + CMake entry + 7 stale island `#include`s. Checker FORBIDDEN trip-wires (glInteropViewCreate/
  Signal/Wait) KEPT — now structurally unreachable, the strongest FORBIDDEN.
- **Item 2 — exportable IMAGE half of `vk_image_resource.{cpp,h}` DELETED (−94 LOC net):**
  `VkExternalMemoryImageCreateInfo` / `VkExportMemoryAllocateInfo` / `vkGetMemoryWin32HandleKHR`
  / `memWin32` / export-sanity queries / the strict-abort canary / `<windows.h>` /
  `VK_USE_PLATFORM_WIN32_KHR`. The `exportable` param dropped from all 5 callers (present,
  shadow×2, shared scene color/depth). Non-exportable dedicated core (VkMemoryDedicatedAllocateInfo
  = VK 1.1 core) + binary sems + `VkFrameSync` SURVIVE (all-island ABI).
- **Item 3 — `wantInterop` plumbing REMOVED (−20 LOC net) — THE METAL-GATE BLOCKER:**
  deleted the `VK_KHR_external_memory[_win32]` / `external_semaphore[_win32]` device exts + the
  external-mem/sem-capabilities + get_physical_device_properties_2 instance caps from
  `vulkan_context.cpp` (dedicated_allocation + get_memory_requirements_2 are VK 1.1 core, so the
  surviving dedicated allocs need no extension). Dropped the `wantInterop` init() param; renamed
  `ensureEngineVulkanContextForInterop`→`ensureEngineVulkanContext` (~13 callers) + gameosmain's
  `init(/*wantPresent=*/true)`. **VK_USE_PLATFORM_WIN32_KHR in vulkan_context.cpp KEPT** — it now
  serves only the Win32 SURFACE extension-name macro (comment updated; macOS = platform branch).
  **Device extensions now enabled at creation = surface + swapchain only** (`VK_KHR_surface` +
  `VK_KHR_win32_surface` + `VK_KHR_swapchain`).
- **Item 10 — gate model current:** `MC2_VK_STATIC_SHADOW_MAP` → RETIRED (registry + doc).
  `MC2_VK_NO_GL_INTEROP_STRICT` survives as the live guard on the terrain per-frame GL
  depth-readback fallback (the remaining GL-touch class). `MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT`
  is now **permanently satisfied BY CONSTRUCTION** — no exportable resource can be created, no
  code reader remains; kept as a checker provenance receipt. RETIRED-GATES table +
  `docs/tier1_env_vars.md` synced. Strict-runner/run_smoke allowlists keep the retired names
  for recipe stability.
- **This-session 8.7 tally: +161 / −699 (net −538 LOC) across 26 file-changes.**
- **RUNG 8 CLOSED.** VK owns the post-scene tail; the interop scaffolding is deleted; the Metal
  gate is OPEN. **What macOS still needs** (NOT in 8.7 — cross-ref MACOS-PORT-ROADMAP-1.md +
  METAL-GATE-AUDIT-2 rows): (1) **surface platform branch** — `VK_KHR_win32_surface` +
  raw `vkCreateWin32SurfaceKHR` → SDL_Vulkan_GetInstanceExtensions/CreateSurface
  (`VK_EXT_metal_surface`); (2) **GL-free asset-feed rewrite** — the copy-once
  `glGetTexImage`/`glGetCompressedTexImage`/`glGetBufferSubData` readbacks that source every VK
  texture/geometry feed → load KTX2/BC direct from disk into VK (the long pole, larger than 8.7,
  on no ladder yet); (3) **LP64 ABI-struct audit** (`pch.hpp` bare `#pragma pack(4)` boot
  blocker, long-field audit). Fail-soft semantics on macOS become product behavior (no GL rescue).

## 2026-07-11 — ITEMS 1+2 PARTIAL + ITEM 10 SWEEP — pre-item-6 autonomous close

Ladder rungs 8.7au (item-1), 8.7av (item-2), 8.7aw (item-10). The final autonomous
8.7 slice before the **item-6 USER DECISION**. With item 8 done, `glInteropSignalToVk/
WaitFromVk` had **zero live callers** and item 6 (static shadow) + the shadow-island
dormant `!directChain` branch were the only remaining exportable/GL-view callers.

- **Commits:** item-1 `326644f1` (delete signal/wait + dead self-test + strip dead
  semaphore imports from glInteropViewCreate), item-2 `88a15fae` (delete exportable
  semaphore Win32 export from vk_image_resource), item-10 `37619dd1` (remove dead
  present-gate resolver + doc sync), docs (this + ladder).
- **Item 1 (partial):** `gl_interop_view.{cpp,h}` — deleted `glInteropSignalToVk`,
  `glInteropWaitFromVk`, the entire `vkInteropSelfTest*` block (no callers), the
  `noGlInteropStrictEnabled` helper (only signal/wait used it), and the now-dead
  semaphore IMPORTS (`glDoneSem`/`glVkToGlSem` + the `GL_EXT_semaphore[_win32]` gate +
  the struct members). The adapter is now an IMAGE-ONLY zero-copy importer — all item 6
  needs. `glInteropViewCreate`/`Destroy` KEPT. ~250 LOC deleted.
- **Item 2 (partial):** `vk_image_resource.{cpp,h}` — deleted the exportable **semaphore
  Win32 export** (`VkExportSemaphoreCreateInfo` + `vkGetSemaphoreWin32HandleKHR` +
  `glToVkSemWin32[2]`/`vkToGlSemWin32[2]` + their `CloseHandle`). The binary semaphores +
  `VkFrameSync` + `beginFrame` STAY (shared all-island ABI). The exportable **IMAGE**
  half (memWin32 + `VkExportMemoryAllocateInfo` + format export-sanity) SURVIVES — item 6
  is a cohesive block that needs it (it is glInteropViewCreate's sole input). This is the
  **maximal** caller-free deletion; nothing else in the exportable path could go without
  breaking item 6. ~35 LOC deleted.
- **Shadow-island dormant `!directChain` branch:** LEFT untouched (compiled-not-taken;
  item-6-pending). It still compiles — it references `glInteropViewCreate` (kept) and
  no longer touches the deleted semaphore-Win32 members (it never did directly).
- **Item 10 (sweep):** found + removed the one piece of **dead resolver code beyond the
  no-op**: `mc2VkSharedSceneDirectResourceEnabled()` in `gos_postprocess.cpp` still
  required `MC2_VK_PRESENT_NO_GL_IMPORT==1 && MC2_VK_PRESENT_SKIP_GL_HANDOFF==1` (both
  retired to no-ops by item 8) — a latent footgun that would decline the whole shared
  scene to pure GL under the umbrella unless a user set two retired gates. Removed; the
  shared scene now arms on the post/composite direct-chain stack alone. All other retired
  gates already carry exactly the item-9 pattern (parseable no-op + one-shot note). The
  prop/water/mech `*_DIRECT_CHAIN` reads in `postDirectChainUpstreamOn()` are intentional
  umbrella debug-overrides (default-ON), NOT dead — kept. Docs (`tier1_env_vars.md`
  gate-model) synced; retired gates stay allowlisted in `run_smoke` (recipe stability).

### RETIRED-GATES table (gate → retired-by → replacement semantics)

| Gate | Retired by | Replacement semantics |
|---|---|---|
| `MC2_VK_MECH_FEED_NATIVE` | 8.7am `66f5c3e9` | Native CPU mech feed is unconditional; GL `glGetBufferSubData` readback deleted. No-op + one-shot note. |
| `MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR` | 8.7aq/item-4 `741b9e4f` | Structural predicate `mc2vkgate::terrainNativeFirstOwnerOn()` (terrain Tier-1 armed = native first owner, CLEAR loadOp, no handoff). No-op + note. |
| `MC2_VK_PROP_DIRECT_CHAIN` | item-5 `11f9f7fc` (hybrid-arm delete) | Island unconditionally native. Name survives ONLY as an umbrella debug-override in `postDirectChainUpstreamOn()` (default-ON under umbrella; explicit `=0` forces the post-upstream term OFF). No-op-for-island + one-shot note. |
| `MC2_VK_WATER_DIRECT_CHAIN` | item-5 `11f9f7fc` | Same as prop. |
| `MC2_VK_CSM_COPYMODE` | 8.7as/item-7 `9e80f139` | CSM copy-mode deleted; direct VK shadow views only. No-op + note. |
| `MC2_VK_CSM_BISECT` | 8.7as/item-7 `9e80f139` | CSM copy-mode/bisect recon probes deleted. No-op + note. |
| `MC2_VK_PRESENT_SKIP_GL_HANDOFF` | 8.7at/item-8 `60b1ac8a` | GL→VK present handoff deleted; skip is unconditional. No-op + note. Item-10 `37619dd1` removed its last (dead) resolver read. |
| `MC2_VK_PRESENT_NO_GL_IMPORT` | 8.7at/item-8 `354f9b6f` | Present image unconditionally VK-only. No-op + note. Item-10 removed its last (dead) resolver read. |
| `MC2_VK_*_DIRECT_CHAIN` (mech/shadow/post-fog/ssao/screenfx/hzb) | item-4 `a...`/`741b9e4f` | Umbrella-default-ON members; retired to no-ops when their island hybrid arms were deleted. See `tier1_env_vars.md` for the full family. |

**NOT retired (load-bearing — do NOT retire):** `MC2_VK_PRESENT_TAIL_HOOK` (item-8
verified: gates native HUD/ImGui tail), `MC2_VK_SHADOW_INTEROP` /
`MC2_VK_CSM_ARRAY_INTEROP` / `MC2_VK_CSM_FULLMAP_INTEROP` (feed the surviving direct CSM
producers), `MC2_VK_PRESENT_DIRECT_FBO` / `MC2_VK_POST_COMPOSITE` (live arming),
`MC2_VK_PRESENT_NATIVE_STRICT` / `MC2_VK_SHARED_SCENE_STRICT` /
`MC2_VK_NO_GL_INTEROP[_RESOURCE]_STRICT` (Metal-gate canaries).

### ★ ITEM-6 DECISION BRIEF (for the USER)

**What the static-shadow substrate is.** `vulkan_shared_static_shadow.cpp` (gate
`MC2_VK_STATIC_SHADOW_MAP`, default-OFF): builds an **exportable** VK `D32_SFLOAT` image
and imports it into GL as `GL_DEPTH_COMPONENT32F`. GL renders the static shadow pass into
it and GL screen-shadow samples it; rung 8.7n's native **static-only** screen-shadow pass
(`MC2_VK_POST_SCREENFX_SHADOW`, active only when `MC2_SHADOW_ENABLE` is OFF) samples this
same image as a VkImageResource. It is the **sole remaining live caller** of
`glInteropViewCreate` and the **sole remaining live exportable** `vkImageResourceCreate`.

**What DELETING it costs.** The native static-only screen-shadow slice loses its VK
static-shadow view, so a **non-CSM, static-only-shadow** config would fall back to the GL
screen-shadow body (Windows) / have no VK-native static screen-shadow (macOS — acceptable,
since CSM is the path). **The shipping default (`MC2_SHADOW_CSM`, launcher default) costs
NOTHING** — CSM screen-shadow (8.7o) samples the direct VK CSM array/full-map views, not
this substrate. The strict/Metal-gate stack already forces `MC2_VK_STATIC_SHADOW_MAP=0`
(8.7ad) and proves the full native tail works without it.

**What KEEPING it costs.** It holds hostage: the entire `gl_interop_view.{cpp,h}` TU
(image-import half + LUID match), the exportable-IMAGE half of `vk_image_resource.{cpp,h}`
(`VkExternalMemoryImageCreateInfo` + `VkExportMemoryAllocateInfo` + `vkGetMemoryWin32HandleKHR`
+ `memWin32`), and the **`VK_KHR_external_memory_win32` extension enablement** in
`vulkan_context.cpp`. That Win32 external-memory family is the one hard **MoltenVK/Metal-gate
blocker** left in the geometry/post/present stack (METAL-GATE-AUDIT-2 BLOCKER #1). While it
lives, items 1 (whole `gl_interop_view.cpp` TU) and 2 (exportable-image half) cannot go to
zero and the Metal gate stays closed.

**Recommendation: RETIRE it.** Delete `MC2_VK_STATIC_SHADOW_MAP` + the substrate, and
redefine the non-CSM static-only screen-shadow to run the GL screen-shadow body (fail-soft
on Windows; on macOS that config simply has no VK-native static screen-shadow, acceptable
because CSM — which does NOT use this substrate — is the shipping path). This is consistent
with METAL-GATE-AUDIT-2 audit-risk-#1 ("no GL rescue on macOS → strict canaries become
product behavior"). Retiring it clears items 2 + 1 to FULL deletion (whole
`gl_interop_view.cpp` + the exportable-image half of `vk_image_resource`) and closes the
last Win32 external-memory Metal-gate blocker. **NOT executed — USER DECISION.**

## 2026-07-11 — VK-RUNG8-ITEM8-PRESENT-TAIL-DELETE-1: ITEM 8 COMPLETE

Ladder rung 8.7at. HEAD `e09eee3e`, deployed `mc2-win64-0.4c` @ `e09eee3e320b dirty=0`.
**METAL-GATE-AUDIT-2 8.7 inventory row 8 (item 8) is DONE.** The present interop tail
(copy-1 GL blit + GL->VK semaphore handoff + the exportable present image + its GL view)
is deleted; the present image is now a VK-ONLY (non-exportable) VkImageResource written
solely by the native VK composite (+ native HUD/ImGui in the tail hook).

- **Commits:** stage 1 `60b1ac8a` (delete copy-1 branch + GL->VK handoff + fs), stage 2
  `354f9b6f` (delete exportable image + glView + copyFbo, VK-only alloc), checker
  `282ea3b6` (VK-only receipt replaces NO_GL_IMPORT env), fail-soft hardening `e09eee3e`
  (bound not-owned skip then drop to GL), docs (this).
- **Gates:** `MC2_VK_PRESENT_SKIP_GL_HANDOFF` + `MC2_VK_PRESENT_NO_GL_IMPORT` RETIRED to
  parseable no-ops with one-shot deprecation notes (item-9). `MC2_VK_PRESENT_TAIL_HOOK`
  KEPT — verified load-bearing for the native HUD/ImGui tail (NOT vestigial; fails the
  "only selected a deleted branch" test). `MC2_VK_PRESENT_DIRECT_FBO` / `MC2_VK_POST_COMPOSITE`
  KEPT (live arming). `MC2_VK_PRESENT_NATIVE_STRICT` KEPT — its copy1_would_blit /
  gl_handoff_would_signal triggers are now structural impossibilities; `present_image_not_vk_owned`
  is the sole surviving trigger.
- **Fail-soft (audit risk #1) redefined:** no GL-scene / VK-present hybrid can exist. Not
  VK-owned -> bounded warmup skip (240 frames), then `forceGLFallback` to GL. Surfaced +
  fixed a permanent-skip black-screen for an armed-but-never-composites config during the
  resize exercise.
- **MUST-SURVIVE verified untouched:** copy-2 `vkCmdBlitImage`, Win32 surface, the 8.7q
  resize/direct-FBO interlock, the `5207a070` swapchain-retry hardening.
- **Checker:** REQUIRE receipt `present image is a VK-only allocation (no GL interop view`;
  drop deprecated `MC2_VK_PRESENT_NO_GL_IMPORT=1` env requirement. Invariant present==copy1_skip
  ==gl_handoff_skip && copy1_blit==0 preserved (counters increment structurally).
- **Verification** (clean deployed `e09eee3e320b`): resource-strict visible proof **VERDICT
  PASS** (`.claude/vk-item8-proof-final/`): 11/11 deterministic bookmarks, all H1 forbidden 0
  incl. glInteropSignal/Wait/glInteropViewCreate/vkImageResourceCreate(exportable), VK-only
  receipt present, islands terrain/prop 1022 / mech 957 / water 1022 forbidden_*=0,
  present/copy1_skip/gl_handoff_skip/composite/imgui=900, copy1_blit=0, provenance sha match.
  Resize exercise (`.claude/vk-item8-resize2/`): `resize_count=3 resize_recreate_ok=3
  retries=0 failsoft=0`, VK present active, 0 NATIVE_WINDOW_IN_USE/DEVICE_LOST/crash,
  validation_errors=0. GL control gates-off `--verify`: 10/11 STABLE, lone `cross_map_view`
  DRIFT = documented GL pose nondeterminism. Canonical tier1 5/5 PASS both states +0 destroys
  (GL `2026-07-11T01-37-57`; VK umbrella `2026-07-11T01-41-26`).
- **Zero-caller status for items 2/1:** `glInteropSignalToVk/WaitFromVk` live callers now =
  **NONE** (present tail was the last). `glInteropViewCreate` / exportable `vkImageResourceCreate`
  callers = static shadow (item 6, STAYS) + shadow-island dormant `!directChain` branch
  (compiled-not-taken). Items 2 + 1 clear when item-6 retires the static-shadow substrate.
- **Remaining 8.7 order:** item 6-decision, then 2, 1, 3, 10.

## 2026-07-11 — VK-RUNG8-ITEM7-CSM-COPYMODE-DELETE-1: ITEM 7 COMPLETE

Ladder rung 8.7as, commit `9e80f139`, deployed `mc2-win64-0.4c` @ `9e80f139cc27 dirty=0`.
**METAL-GATE-AUDIT-2 8.7 inventory row 7 (item 7) is DONE.** The terrain CSM copy-mode
(per-frame `glGetTexImage` depth pull + `glCopyImageSubData` GL scratch depth array +
staging + `vkCmdCopyBufferToImage`) is deleted; the VK shadow island's direct D32
near-array (binding 8) + full-map (binding 9) `VkImageView`s are the only CSM producer.

- **Deleted** from `vulkan_terrain_island.cpp`: `copy_csm_array()`, the CSM full-map
  `copy_shadow_slot(perFrame)` fallback + FBO save/restore, `csmBisectMode()` + all
  `MC2_VK_CSM_BISECT` branches, the `MC2_VK_CSM_COPYMODE=noread` branch, and copy-only
  state (`csmArrayStage*`, `csmScratchTex/Dim/Layers`, `csmFullMapStage*` + destroys).
  `copy_csm_shadows()` is now direct-view-only (~150 LOC net deleted).
- **WARMUP DECISION (recorded):** shadow island does NOT produce from frame 1, so
  direct-only takes an **explicit warmup decline** — csm-not-built + the ~1-2 warmup
  frames return false -> `csmEnabled=0` -> legacy single-map dynshadow path. NOT stale:
  all ready/external/direct flags reset every frame -> declined frames bind the 1x1
  dummies, next ready frame re-points bindings 8/9. Removes the copy fallback WITHOUT
  introducing a cross-frame latch (matches PART A's stale-shadow non-repro finding; the
  user-sighted stale shadow was a per-launch warmup/ordering transient, not copy-mode-fed).
- **Gates:** `MC2_VK_CSM_COPYMODE` + `MC2_VK_CSM_BISECT` RETIRED to parseable no-ops +
  one-shot deprecation notes (item-9). `MC2_VK_CSM_ARRAY_INTEROP` /
  `MC2_VK_CSM_FULLMAP_INTEROP` KEPT (gate the surviving direct producers; checker H5
  unchanged). Parked `MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL` **UNTOUCHED** — it feeds
  binding 7 (dynamic single-map) via the surviving `copy_shadow_slot`, NOT the CSM
  copy-mode, so it does not ride copy-mode and does not die with it.
- **MUST-SURVIVE kept:** `copy_shadow_slot` (static item-6 + dynamic), 1x1 CSM dummies,
  `terrainPerFrameReadback` + `forbidden_getTexImage` counter (CSM call sites gone; still
  serves the dynamic single-map fallback + checker trip-wire).
- **Verification** (clean deployed `9e80f139cc27`): resource-strict visible proof
  `check_vk_nointerop_log.py --expect-sha 9e80f139cc27` **VERDICT PASS**
  (`.claude/vk-item7-csm-proof/`): 11/11 deterministic bookmarks incl. `ridge_lowangle`
  (shadow_cascade), all H1 forbidden 0 incl. `PERFRAME_READBACK`, all islands
  `forbidden_getTexImage=0`, ownership terrain 1022 / prop 1022 / mech 957 / water 1022,
  VK-only substrate, terrain `stage_us=2.3`, present counters 900/900 copy1_blit=0,
  provenance dirty=False sha match. CSM direct receipts (`.claude/vk-item7-copystats/`,
  `MC2_VK_TERRAIN_COPY_STATS=1`): `copies_ms=0.0004 csmArrayReady=1 csmArrayDirect=1
  csmFullMapReady=1 csmFullMapDirect=1`; shadow island `CSM array target 8192x8192
  layers=2 (direct binding 8)` + `CSM full-map 4096x4096 (direct binding 9)`. Canonical
  tier1 **5/5 PASS both states +0 destroys** (GL `2026-07-11T00-51-05`; VK umbrella
  `2026-07-11T00-54-33`). Shadow visual spot-check: `ridge_lowangle` rendered
  deterministically with CSM direct-produced (full VK-vs-GL A/B is confounded under the
  strict stack per the arc's standing finding).
- **Remaining 8.7 order:** items 8, then 6-decision, then 2, 1, 3, 10.

## 2026-07-11 — VK-RUNG8-ITEM5-SHARED-SCENE-DELETE-1: ITEM 5 COMPLETE

Ladder rung 8.7ar, commits `7ee0494f` (core) + `11f9f7fc` (prop/water hybrid arms) +
`12fe7c4b` (deprecation-note fixup), deployed `mc2-win64-0.4c` @ `12fe7c4b85b3 dirty=0`.
**METAL-GATE-AUDIT-2 8.7 inventory row 5 (item 5) is DONE.** The shared scene
color/normal/objectId + depth resources are now VK-ONLY (non-exportable); the GL-import
half is deleted and the former `vkOnlyNoGlView` path is the ONLY path.

- **Deleted**: exportable/`glInteropViewCreate` allocation branch + D24S8-preference
  block + `vkOnlyNoGlView`/`GLuint out`/`HasGlView`/`GetGlView`/`view` members from
  `vulkan_shared_scene_color/depth.{cpp,h}`; the GL-import FBO substitution machinery
  from `gos_postprocess.cpp`; the present native-composite `!sceneDirectChain` GL-view
  hybrid arm from `engine_present_vk.cpp`; the prop+water `!directChain` hybrid
  semaphore arms (item-3 trigger — `GetGlView` removal took their last input) mirroring
  mech `a0cba0c4`; the dead assign-only shared-scene GL-view fetches in
  terrain/mech/fog/hzb/screenfx/ssao.
- **Decline semantics (audit risk #1 resolved)**: the shared scene arms ONLY under the
  full native-present stack (`mc2VkSharedSceneDirectResourceEnabled()`); otherwise the
  WHOLE VK shared-GBuffer declines to pure GL (there is no more VK-scene/GL-present
  hybrid — a VK-only scene + GL present would be a disconnected placeholder). The GL FBO
  attachments are plain-GL placeholders. `mc2VkSharedSceneStrictFallback` redefined =
  the boot-decline canary (`MC2_VK_SHARED_SCENE_STRICT=1` aborts on the decline).
  **Behavior change**: `MC2_VK_SHARED_GBUFFER=1` WITHOUT the present-native gates now
  renders pure GL (was VK-scene-hybrid); WITH the full native stack it is VK-only. Inert
  under the strict stack (VK-only was already the path there).
- **Gates**: `MC2_VK_PROP_DIRECT_CHAIN` / `MC2_VK_WATER_DIRECT_CHAIN` RETIRED to
  parseable no-ops + one-shot deprecation notes fired at island init (item-9 pattern).
  MUST-SURVIVE kept: pure-GL alloc paths + the `glCopyImageSubData` scene snapshots +
  screenfx's item-6 static-shadow GL view.
- **Checker**: FORBIDDEN gains the deleted GL-import receipts (`VK-backed GL_*`,
  `imported to GL_`); new `check_shared_scene_vk_only` asserts `(VK-only MRT0)` /
  `(VK-only depth)` when the shared scene is armed.
- **Verification** (all at clean deployed `12fe7c4b85b3`): resource-strict visible proof
  `check_vk_nointerop_log.py --resource-strict --expect-sha 12fe7c4b85b3` **VERDICT
  PASS** (`.claude/vk-item5-proof-final2/`): 11/11 deterministic bookmarks, all H1
  forbidden 0 (incl. deleted GL-import receipts), all islands `forbidden_*=0`, ownership
  terrain 1022 / prop 1022 / mech 957 / water 1022, VK-only substrate receipts present,
  present/copy1_skip/gl_handoff_skip/composite/imgui = 900, copy1_blit=0. Override check
  (`.claude/vk-item5-override2/`): both deprecation notes printed, **zero** strict aborts
  (`=0` is inert — hybrid arm gone). GL control gates-off `--verify` byte pair: 10/11
  STABLE, lone `cross_map_view` DRIFT = documented pre-existing GL pose nondeterminism.
  Canonical tier1 5/5 PASS both states +0 destroys (GL, and
  `MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1`). Intermittent swapchain
  `NATIVE_WINDOW_IN_USE` flake hit the first strict attempts (present_frames=0, islands
  still native); cleared on retry.
- **Zero-caller status for items 2/1**: the shared-scene GL-view family is GONE.
  `glInteropSignalToVk/WaitFromVk` live callers now = **present tail ONLY** (item 8;
  prop/water were the last others). `glInteropViewCreate` / exportable
  `vkImageResourceCreate` callers = **static shadow** (item 6, STAYS) + shadow-island
  dormant `!directChain` branch + present tail (item 8). So items 2
  (`vk_image_resource` exportable half) + 1 (`gl_interop_view.cpp` whole TU) are NOT yet
  at zero callers — they clear only after items 6 + 8. **Remaining 8.7 order: 7, 8, 6,
  then 2, 1, 3, 10.**

## 2026-07-11 — VK-RUNG8-ITEM4-TERRAIN-DELETE-1: ITEM 4 COMPLETE — ALL ISLANDS

Ladder rung 8.7aq, commit `741b9e4f`, deployed `mc2-win64-0.4c` @ `741b9e4faffb dirty=0`.
The terrain island was the LAST island with a hybrid GL<->VK semaphore-handoff arm; it is
now native-first-owner-only. **METAL-GATE-AUDIT-2 8.7 inventory row 4 (item 4) is DONE
across all islands** (mech `a0cba0c4` + posts `bc923366` + shadow `2b4160ae` +
prop/water guard restore `c2c21682` + terrain `741b9e4f`).

- **Deleted** from `vulkan_terrain_island.cpp` run path: the `!nativePrefixClear` hybrid
  arm — normal/depth `vkImageResourceBeginFrame`, 3x `glInteropSignalToVk`, the
  waitSems/signalSems machinery (gpuC/N/D vk2gl signal plumbing), 3x
  `glInteropWaitFromVk`. Submit waits/signals NO GL<->VK semaphores (fsC fence orders
  CPU; color0 BeginFrame kept for cb parity + fence only). init: renderpass
  unconditionally CLEAR loadOp on color0/normal/depth; GL-view readiness requirement
  dropped (views still fetched = item-5 substrate, no longer consumed by this island).
  Health: `interop_active=0 vk2gl_sem=0` always.
- **PREFIX_CLEAR disposition:** `MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR` RETIRED → parseable
  no-op + one-line deprecation note when explicitly set (item-9 pattern). It was NOT a
  pure handoff gate — it also owned "skip the GL frame-entry clears + clearGBuffer1"
  (gameosmain) and fed prop/composite/post direct-chain predicates. That meaning is now
  the STRUCTURAL predicate `mc2vkgate::terrainNativeFirstOwnerOn()` (= `MC2_VK_TERRAIN_
  TIER1 && MC2_VK_SHARED_SCENE_DEPTH`, umbrella-resolved), rebound at all 4 consumer
  sites (gameosmain, `propDirectChainRequested`, `compositeDirectChainGateEnabled`,
  `postDirectChainUpstreamOn`). Under the umbrella this resolves IDENTICALLY to the old
  gate (clear colors/sentinel/reverse-Z far unchanged); explicit `MC2_VK_TERRAIN_TIER1=0`
  now correctly restores the GL clears too (island off => GL owns terrain AND its clears).
- **Gates kept:** all `MC2_VK_*_INTEROP` producer names (CSM array/fullmap/shadow) — they
  enable surviving native production. Checker H5 unchanged. Boot-decline: the existing
  terrain session-latch (`1f4a1a5a`) covers everything; no deleted branch was a decline
  target (the dropped GL-view requirement only ever declined the deleted arm).
- **Verification** (all at clean deployed `741b9e4faffb`): strict proof
  `check_vk_nointerop_log.py --expect-sha 741b9e4faffb` **VERDICT PASS**
  (`.claude/vk-item4-terrain-proof/`): 11/11 deterministic bookmarks, all H1 forbidden
  strings 0, all islands `forbidden_*=0` AND ownership `used_vulkan>0` (terrain 1022 /
  prop 1022 / mech 957 / water 1022), terrain `interop_active=0 vk2gl_sem=0 stage_us=3.3
  fallback_reason=""`, present counters 900/900 with `copy1_blit=0`. Override check
  (`.claude/vk-item4-terrain-override/`, explicit `=0` + `MC2_VK_TERRAIN_GL_FALLBACK_
  AUDIT=1`): deprecation note printed, island STILL native (`used_vulkan=1022`, zero
  strict aborts), audit steady `gl_fallback=0 edge_stitch=0 captured_stitch=3` —
  captured_stitch UNCHANGED vs the pre-deletion `.claude/edgestitch-audit/` receipt.
  GL control gates-off `--verify` byte pair: 10/11 STABLE; lone `cross_map_view` DRIFT =
  documented pre-existing GL pose nondeterminism (identical to the a0cba0c4 control).
  Canonical tier1 5/5 PASS both states +0 destroys (GL, and
  `MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1`).
- **Zero-caller status for the next items:** `glInteropSignalToVk/WaitFromVk` live
  callers remaining = prop + water hybrid arms ONLY (explicit `*_DIRECT_CHAIN=0` debug
  overrides; deferred pure-dead-code cleanup) + `engine_present_vk.cpp` (item-8 present
  tail, SURVIVES) + `gl_interop_view.*` itself (item-1). `glInteropViewCreate` /
  exportable `vkImageResourceCreate` callers = shared scene color/depth (item-5),
  static shadow (item-6, stays), shadow island dormant `!directChain` branch, present.
  Terrain + shadow + mech + posts: comments only.
- **Remaining 8.7 order:** items 5, 2, 1, 7, 8, 3, then item-10 vestigial gates.


## 2026-07-11 — VK-TERRAIN-EDGESTITCH-1: OPEN ITEM WAS ALREADY DONE (finding + de-stale) (LATEST)

The "terrain far-band edge-stitch is GL-owned, port it to VK" open item logged in
the 2026-07-10 blocks (and repeated in the 2026-07-11 shadow block's "Also OPEN"
line) was **already closed by commit `0aae0337` ("own terrain edge stitch"), an
ANCESTOR of the deployed HEAD `43d968c0`.** The open item was a documentation/
attribution error driven by stale in-code comments; NO re-port was needed.

**Ground-truth code state at HEAD (verified by git diff + read):**
- `0aae0337` removed the `edgeStitch[i]==0` term from the terrain capture predicate
  (`gos_terrain_lod_chunk.cpp` ~:1830): the `if (s_vkTerrainActive &&
  s_vkCaptureThisBand && s_vkOwnsDisplace)` gate no longer excludes stitched
  patches. `p.edgeStitch` is populated and pushed into the feed.
- `edgeStitch`/`quadCountX`/`quadCountY` ride the terrain patch UBO tail
  (`vulkan_terrain_island.cpp` struct off 592/596/600, populated :2666-2668).
- The coarse corner-pin math + the mode-2 far-band re-stitch math are in
  `shaders/vulkan/terrain_tier1.vert` (:120-126, :153-162) and `_skirt.vert`.
- The strict-visible runner already arms `MC2_VK_TERRAIN_ALL_LODS/SKIRTS/
  VISUALDISPLACE_FAR=1` (run_vk_nointerop_strict_visible.py:42-44), so under the
  strict stack coarse bands + skirts + mode-2 far displace + edge-stitch are ALL
  VK-owned.

**Why the open item looked real (the trap, now fixed):** the "edge-stitch remain
GL-owned this rung" comments at `vulkan_terrain_island.cpp:183` and
`gos_terrain_lod_chunk.cpp:~1812/1823/1831` PREDATE `0aae0337` and were never
updated; the 2026-07-10 analysis cited them as evidence. The elevated oblique_fog
91.4/ch + cliff 55.6/ch MADs cited as corroboration were **explicitly CONFOUNDED**
(GL-default-stack vs VK-full-strict-stack, per that same 2026-07-10 block) and were
captured WITH edge-stitch already VK-owned — they are NOT an edge-stitch-ownership
signal. This session de-staled all three comments so the phantom can't recur.

**Live proof (`64affd28`, deployed 0.4c, strict-visible sweep +
`MC2_VK_TERRAIN_GL_FALLBACK_AUDIT=1`, `.claude/edgestitch-audit/strict/`):**
`[VK_TERRAIN_GL_FALLBACK_AUDIT v1] total=15 captured=15 gl_fallback=0 edge_stitch=0
captured_stitch=3` — zero patches fall to GL for edge-stitch, and 3 CAPTURED
patches carry a nonzero stitch mask (direct proof stitched geometry is VK-owned).
Terrain island `used_vulkan=1022 fallback_reason="" forbidden_getTexImage=0
vk_draws=19`; `[VK_TERRAIN_ALL_LODS] feedValid=1 vkPatches=15 (6 LOD0 + 9 coarse)`;
11/11 deterministic bookmarks. New `captured_stitch=` counter added this session
(`gos_terrain_lod_chunk.cpp`, behind the existing default-OFF audit gate) is the
captured twin of the excluded `edge_stitch=` counter.

**REMAINDER (the genuinely-unported class, NOT the far horizon):** mode-1 LOD0
4x-finer displaced topology (`lodStep==1` + `visualDisplaceActive`) stays GL-owned
— `s_vkOwnsDisplace` excludes it. This is NEAR detail, not the far-band horizon,
and is the "separate L slice later" noted at gos_terrain_lod_chunk.cpp:1830. The
user-sighted intermittent stepped-horizon was self-labeled "run 1 bad, run 2 fine
→ warmup/ordering" and settled deterministic captures are clean (see 2026-07-11
PART A stale-shadow non-repro); no persistent edge-stitch ownership bug exists at
HEAD. If mode-1 4x is ever wanted, it is a fresh slice on the (already-shipped)
coarse-stitch foundation.

## 2026-07-11 — VK-RUNG8-ITEM4-SHADOW-DELETE-1 + STALE-SHADOW item non-repro

Two coupled shadow items closed. Ladder rung 8.7ap.

**PART A — user-sighted intermittent stale/static shadow: NOT REPRODUCED (item CLOSED as
non-repro, harness left in place).** Built the discriminator harness
`scripts/vk_shadow_stale_discriminator.py` (gated/opt-in): umbrella early-vs-settled +
camera-motion bookmark captures. Findings: (1) fixed-timestep captures are internally
byte-stable + `nondeterministic=False` at the shadow-bearing pose (`ridge_lowangle`,
covers `shadow_cascade`) across triggers; (2) full-frame early-vs-settled diffs are
CONFOUNDED by animation phase (water/fog/units advance between trigger frames → uniform
~30% change on ALL poses incl. no-shadow sky poses), so they are NOT a shadow signal; (3)
CODE AUDIT rules out a cross-frame stale-view latch: the terrain island resets
`csmArrayExternalView/csmArrayDirect/csmArrayReady` (`vulkan_terrain_island.cpp:2122-2127`)
and `dynShadowExternalView/dynShadowDirect/dynShadowReady` (:2312-2314) EVERY frame, and
the shadow island resets all three ready flags at the top of every
`mc2_vk_shadow_island_run()` (`vulkan_shadow_island_live.cpp:897-899`). The intermittent
symptom is therefore a per-launch warmup/ordering transient that the fixed-timestep +
fixed-seed deterministic capture harness cannot surface BY CONSTRUCTION (it removes the
timing variance a race needs). Consistent with the prior session's clean settled captures.
No fix landed (nothing to fix without a repro); harness committed for a future non-
deterministic repro attempt.

**PART B — item-4 shadow island conversion: DONE.** `vulkan_shadow_island_live.cpp` was the
LAST dual `useInterop`/`directChain` island. Converted to direct-chain-native-only like
`a0cba0c4` (mech): deleted the GL<->VK semaphore-handoff arm from the dyn + csm-fullmap run
functions (the `useGpuVk2Gl` branch = vk2gl signal in submit + `glInteropWaitFromVk`); the
submits now signal/wait no GL<->VK semaphores (fence orders CPU). The csm-array-layer run
was already handoff-free. Both run functions return 0 (native; consumers use the
`_get_*_view()` VkImageView accessors). `MC2_VK_SHADOW_DIRECT_CHAIN` RETIRED → parseable
no-op with a one-line deprecation note when explicitly set (`shadowDirectChainRequested()`/
`noteShadowDirectChain()` removed; `constexpr kShadowDirectChain=true`). **Item-5 substrate
KEPT per audit must-survive:** the exportable `vkImageResourceCreate(exportable=!directChain)`
+ `glInteropViewCreate` branches in `init_island`/`ensure_csm_fullmap_target` are RETAINED
but DORMANT (guarded on `!directChain`, always false now → compiled-but-not-taken, NOT
deleted) so the non-CSM screen-shadow config (item-6) still has them. Item-2 boot-decline
session-latch added (`g_shadowSessionDeclined`/`g_shadowInitReachedBuild`): permanent init
failure past the context+gate readiness gates latches a session decline with ONE receipt;
transient keeps fail-soft-retry. Checker H5 note updated (`check_vk_nointerop_log.py`):
`MC2_VK_SHADOW_DIRECT_CHAIN` is deprecated, reconciliation carried by the umbrella (still
back-compat accepts the explicit gate). The `MC2_VK_*_INTEROP` ENABLE gates
(`MC2_VK_SHADOW_INTEROP`/`MC2_VK_CSM_ARRAY_INTEROP`/`MC2_VK_CSM_FULLMAP_INTEROP`) are KEPT
(they enable the surviving native production, they did NOT only feed the deleted arm).

Under the umbrella the delete is behaviorally INERT (directChain already defaulted true →
`useGpuVk2Gl` was already false → the deleted branches never executed under strict).
Functional strict-visible proof (dirty `c2c21682+`, `--minimal-promotion`,
`.claude/vk-item4-shadow-func-check/`): 11/11 deterministic bookmarks, shadow health
`used_vulkan=4088 used_interop=0 interop_active=0 vk2gl_sem=0 direct_chain=1
fallback_reason="" forbidden_*=0`; `check_vk_nointerop_log.py --no-resource-strict`
**VERDICT PASS** (all islands ok, ownership ok, H3/H4/H5/H6 ok). Clean `--expect-sha` proof
+ tier1 both-state smoke = post-commit rebuild (see commit trailer).

Remaining item-4: the terrain hybrid submit arm (renders natively but the prefix-clear
gate + prop/water GL half-state interaction is delicate). Then items 5/2/1
(`gl_interop_view.cpp` family) — item-5's shadow-island exportable substrate now has ONE
fewer consumer (the deleted shadow handoff) but survives for item-6. Also OPEN from prior
session: terrain far-band edge-stitch (GL-owned, rung-5 mode-1) — separate class.

## 2026-07-10 — VK-OWNERSHIP-GAP-FIX: prop+water native co-write RESTORED under strict

Closes the ★★ BLOCKER logged below (the `04a15aec` prop/water run-entry guard bug).

**Root fix (Step 1).** `vulkan_color_prop_island.cpp` and `vulkan_water_island.cpp` had
`if (s->disabled || !s->inited || !s->useInterop) return 0;` at run entry. Direct-chain
sets `useInterop=false` (init), so under the umbrella the islands returned 0 and GL silently
drew props/water into the VK-owned GBuffer (`used_vulkan=0`, no forbidden_* — a GL base-
geometry flush is not an interop call, so the strict proof passed with a hole). Guard now
enters when direct-chain OR the hybrid interop arm is armed — mirrors mech/terrain
(`a0cba0c4`/`1f4a1a5a`), whose guards are `disabled || !inited` with directChain
unconditional-native:
`if (s->disabled || !s->inited || (!propDirectChainRequested() && !s->useInterop)) return 0;`
(and the `waterDirectChainRequested()` twin for water). init already supported direct-chain
(readiness gate only needs GL views when `!directChain`), so no init change was needed.

**Coupling fix.** `waterDirectChainRequested()` ANDed in `directChainGate("MC2_VK_MECH_DIRECT_CHAIN")`
— the DEPRECATED mech gate (mech is unconditionally native post-`a0cba0c4`). An explicit
`MC2_VK_MECH_DIRECT_CHAIN=0` (mech-hybrid debug override) wrongly forced water back onto the
hybrid arm. Water is now bound to its OWN gate only (`MC2_VK_WATER_DIRECT_CHAIN`).
(`postDirectChainUpstreamOn()` in `vk_shared_gbuffer_gate.h:117` also references the deprecated
mech gate but resolves ON under the umbrella — left as-is, mirroring the `a0cba0c4` precedent
that did not touch it; only an explicit =0 debug override would gate posts.)

**PARITY / OWNERSHIP PROOF (Step 2), `guardfix` @ `1ea7bc8a dirty=1`, mc2_01 strict-visible
`--resource-strict --minimal-promotion` (`.claude/ownership-fix/guardfix-mc2_01/`).** Island
receipts — the ownership gap is CLOSED:
- **prop `used_vulkan` 0 → 1022** `used_interop=0 interop_active=0 forbidden_*=0 fallback_reason=""`
  inst_records=1008 type_runs=80 geom_verts=23628 vk_draws=1.
- **water `used_vulkan=1022`** `used_interop=0 forbidden_*=0 depthin_*=0 fallback_reason=""`
  thin_records=3756 recipe_count=9801 vk_draws=1.
- terrain 1022 / mech 957 unchanged. All 11 bookmarks present + deterministic (`det=True`,
  `nondeterministic=False`), prop-heavy (highangle_wide=static_prop_pbr) + water (oblique_fog,
  pyramid_islands_topdown) among them → native path renders clean, no fallback/crash = no rot.
- Hardened checker (below) **VERDICT PASS** incl. new per-island ownership assertion.

Full-frame VK-vs-GL MADs were captured but are CONFOUNDED (the GL reference ran the deployed
default gate stack, the VK run ran the full strict post-native stack — apples-to-oranges;
historical parity work was single-variable within-backend). The single-variable control
(prop/water gate OFF under the same strict env) is NOT reachable — it aborts on
`vkImageResourceCreate(exportable)` (turning the tier gate off re-arms the interop path the
resource-strict mode forbids), which itself re-confirms the native path is the only valid
strict config. Recorded MADs for the record (NOT a clean parity signal):
overview_center 8.7/ch, highangle_wide 11.7/ch (both under the ~14-15/ch far-pose family
baseline), pyramid_islands_topdown 20.2/ch, runway_overview 32.7/ch, cliff_side_on 55.6/ch,
oblique_fog 91.4/ch (cov 98.6%). The elevated fog/horizon/cliff poses corroborate a SEPARATE
pre-existing terrain issue (see OPEN ITEMS) — they are NOT prop/water and NOT caused by this fix
(the edit is strictly inside the VK-armed `*_island_run()`; the pure-GL path is untouched by
construction, so gates-off byte-stability holds without a re-capture).

**Checker hardening (Step 4).** `check_vk_nointerop_log.py` gained `check_ownership()`: every
geometry island (`[VK_TERRAIN_TIER1]`/`[VK_PROP_COLOR_TIER1]`/`[VK_MECH_TIER1]`/`[VK_WATER_TIER1]`)
must report `used_vulkan > 0` — missing/zero = FAIL. The proof now asserts VK OWNERSHIP, not
just no-interop, closing the proof-hole CLASS that let the prop/water gap pass green. Dry-run
on the guardfix log: all four islands `used_vulkan>0 ok`, VERDICT PASS.

**Step 3 (hybrid-arm deletion for prop+water) — DEFERRED, documented.** The prop/water hybrid
(`!directChain`) arms are now DEAD under the umbrella (proven native above) and safe to delete
mirroring `a0cba0c4` (glInteropSignal/Wait plumbing + fsN/fsD BeginFrame + gpuC/N/D signal
plumbing out; `MC2_VK_PROP_DIRECT_CHAIN`/`MC2_VK_WATER_DIRECT_CHAIN` → no-ops; item-2
boot-decline session-latch wired). Deferred as a separate low-risk cleanup slice — pure dead-code
removal, no correctness bearing (the arm is unreachable under the umbrella; explicit =0 still
selects it until deleted).

### ★ OPEN ITEMS surfaced this session (terrain, NOT prop/water — pre-existing, separate class)

User/coordinator observed under the umbrella smoke (screenshot evidence): (1) a large terrain
face near-black with a **shadow that does not move with the camera** (baked/stale look),
(2) **stepped/blocky far-band horizon geometry**, (3) overall "way off" from GL. Intermittent
(run 1 bad, run 2 fine → warmup/ordering, not deterministic breakage). Findings:
- **Far-band / stepped horizon = terrain edge-stitch ownership, NOT the prop/water guard class.**
  Edge-stitch geometry is deliberately **GL-owned** this rung (rung-5 mode-1 edge-stitch deferred;
  `vulkan_terrain_island.cpp:183` "edge-stitch remain GL-owned"). The full strict stack drives
  `gl_handoff_skip_frames == present_frames` (canary invariant, verified 900==900), i.e. the GL
  scene path is fully skipped — so GL-owned stitch at LOD-band boundaries is dropped/mismatched
  under strict, producing far-band cracks. This is a geometry-ownership/target class, NOT a
  `useInterop` run-entry guard, so it is NOT fixed by this slice. Corroborated numerically by the
  elevated oblique_fog (91/ch cov 98.6%) + cliff_side_on (55/ch) MADs above. **Fix = port
  edge-stitch to VK ownership (rung-5 mode-1, explicitly deferred, its own slice).**
- **Stale/static shadow = suspected CSM warmup/ordering** (the `7a5bf1fa` strict fail-soft on the
  2-frame warmup CSM readback may leave shadow data stale until the VK shadow producer catches up;
  shadow island products may not re-produce on camera motion under direct-chain). NOT reproduced
  with a controlled early-vs-settled capture this session (settled bookmark captures are clean +
  deterministic); needs an early-frame (first 2-5s) vs settled (20s+) same-pose capture harness to
  isolate. Logged as an open shadow item with the repro recipe; out of scope for the prop/water fix.

---

## 2026-07-10 — VK-RUNG8-ITEM4-INTEROP-DELETE-1: item-4 SPLIT (mech + 4 post passes done)

Ladder rung 8.7ao. First interop-handoff BRANCH deletion of item-4 (METAL-GATE-AUDIT-2
8.7 inventory **row 4** — the per-island GL↔VK semaphore-handoff branches). Landed the
FULLY-VERIFIED chunk: the **mech geometry island** + the **4 native post passes**
(fog / SSAO / screenfx / HZB). Commits `a0cba0c4` (mech) + `bc923366` (post). ~161 LOC
net removed (351 del / 190 ins incl. comments), deployed `bc923366ef61` lane
`mc2-win64-0.4c`.

Per TU the hybrid (`!directChain`) arm was deleted: `glInteropSignalToVk`/
`glInteropWaitFromVk` paths, the `fsN`/`fsD`(/`fsS`) `vkImageResourceBeginFrame`, the
`gpu*` vk2gl signal plumbing, the `waitSems`/`signalSems` machinery, and the handoff-mode
fence. Mech submit now waits/signals nothing (fence orders CPU); each post pass submits on
`cbuf[0]`, waits/signals nothing, CPU-drains via `vkQueueWaitIdle`. init dropped the
`!directChain` GL-view readiness requirement (shared `VkImageResource`s still required; the
GL interop views = item-5 substrate still fetched, no longer consumed). **5 gates retired**
to parseable no-ops with one-line deprecation notes (item-9 pattern):
`MC2_VK_MECH_DIRECT_CHAIN`, `MC2_VK_POST_{FOG,SSAO,SCREENFX,HZB}_DIRECT_CHAIN`. The 4 post
resolvers now return `postDirectChainUpstreamOn()` only, and each post pass **declines**
(→ GL pass) when upstream is not direct-chain (item-2 native-or-decline). Mech got the
terrain-style **item-2 boot-decline** (session-latch on PERMANENT init failure past
readiness gates, ONE receipt; transient keeps fail-hard-retry). Pure-GL path untouched.

Verification at `bc923366 dirty=0`: strict no-interop proof **VERDICT PASS**
(`check_vk_nointerop_log.py --expect-sha bc923366`, minimal umbrella env via
`--minimal-promotion`, `.claude/vk-item4-proof-minimal-r2/`): all H1 forbidden interop
strings 0, all island `forbidden_*`/`depthin_*` 0, present_frames=900, terrain stage_us=9.7,
provenance dirty:false + sha match. Umbrella receipts: mech `used_vulkan=957 fallback=""`,
fog/ssao/screenfx/hzb `attempted==used_vulkan`, `direct_chain=1 used_interop=0 fallback=""`.
Override: `MC2_VK_POST_HZB_DIRECT_CHAIN=0` → deprecation note + HZB stays
`direct_chain=1 used_interop=0`, **zero** strict aborts (`.claude/vk-item4-override-hzb/`).
GL control (`--verify`, gates-off): 10/11 bookmarks byte-STABLE; the lone `cross_map_view`
DRIFT is a pre-existing GL pose nondeterminism unreachable by the VK-island edits. Canonical
tier1 **5/5 PASS both states** (GL `2026-07-10T20-37-43`, VK umbrella `2026-07-10T20-41-27`,
+0 destroys). First strict-proof attempt hit the known intermittent swapchain
`NATIVE_WINDOW_IN_USE` flake (present_frames=0, islands still native); cleared on retry.

★★ BLOCKER FOUND (report to arc owner) — **prop + water native co-write is DISABLED under
the umbrella/direct-chain.** The resource-strict commit `04a15aec` changed prop/water
`useInterop = !directChain` (false in direct-chain) but did NOT update their run-entry
guards (`vulkan_color_prop_island.cpp:1159` / `vulkan_water_island.cpp:982`
`if (... || !s->useInterop) return 0;`). So in the strict stack **prop shows
`used_vulkan=0`** and GL silently renders props into the VK-owned GBuffer (mc2_01 has
buildings; water=0 may also be no-water-in-view). Mech renders (`used_vulkan=957`) because
its guard has no `useInterop` check; terrain renders via the prefix-clear flow. The strict
no-interop proof still passes because GL prop/water draws aren't interop calls. **Item-4
for prop + water is BLOCKED**: deleting their hybrid arm removes the ONLY path that renders
them natively; enabling direct-chain there = a guard fix + a VK-vs-GL prop/water pixel
parity proof (its own slice). Also found: `waterDirectChainRequested()` consumes
`MC2_VK_MECH_DIRECT_CHAIN`, so an explicit `MC2_VK_MECH_DIRECT_CHAIN=0` cascades the
still-hybrid water island into its handoff path and trips the strict canary (proof of the
coupling; disappears once water's hybrid is deleted).

Remaining item-4 order (next sessions): (1) **prop+water guard fix + parity proof** →
delete their hybrid (unblocks their gates + reconciles `waterDirectChainRequested`);
(2) **terrain** hybrid submit arm (renders natively but the prefix-clear gate is
load-bearing + interacts with the prop/water GL half-state — delicate); (3) **shadow**
(`vulkan_shadow_island_live.cpp`, structurally different — exportable-resource creation
`!directChain` branch touches item-5, dual `useInterop`/`directChain`). Then items 5/2/1
(`gl_interop_view.cpp` family). No `MC2_VK_*_INTEROP` producer became a no-op in this chunk
(they feed the deferred shadow/terrain-CSM path), so the checker H5 rule was left unchanged.

## 2026-07-10 — VK-UMBRELLA-DIRECT-CHAIN-DEFAULT-1: item-4 prerequisite DONE

Ladder rung 8.7an. The `MC2_VK_SHARED_GBUFFER=1` umbrella now arms the 10
`*_DIRECT_CHAIN` gates BY DEFAULT — umbrella-ON MEANS the strict/direct-chain
no-GL-interop path. The old hybrid interop-semaphore path is reachable ONLY by an
explicit per-gate `=0` override (METAL-GATE-AUDIT-2 8.7 item-4's deletion target).
New resolver `mc2vkgate::directChainGate()` (`vk_shared_gbuffer_gate.h`); all 10
read sites flipped from `envTriState==1`/`getenv=='1'` to it (terrain-prefix-clear,
prop, mech, water, 5 post passes, composite, shadow) + `postDirectChainUpstreamOn()`.
`MC2_VK_OBJECTID_DIRECT_CHAIN` stays explicit (view-mode canary). Umbrella itself
stays DEFAULT-OFF (flip = user decision).

Item-2 boot-level native-or-decline: terrain first-owner latches a SESSION decline
on a PERMANENT init failure (past readiness gates) with ONE receipt line instead of
per-frame re-init churn; TRANSIENT readiness gates keep item-9 fail-hard-with-retry.
Per-island rollout to prop/mech/water/post/shadow is follow-up (item-4's stated
prerequisite — direct-chain promoted to default — is already satisfied).

Tooling: `run_vk_nointerop_strict_visible.py --minimal-promotion` drops the 10
direct-chain gates so the run proves the umbrella arms them; `check_vk_nointerop_log.py`
H5 relaxed to reconcile `MC2_VK_*_INTEROP=1` producer names via the umbrella as well
as via explicit `MC2_VK_SHADOW_DIRECT_CHAIN=1`. **Item 4 is now UNBLOCKED.**

## 2026-07-10 — VK-RUNG8-MECH-FEED-READBACK-DELETE-1: first interop deletion since 8.7h (LATEST)

Deleted the rung-7 GL-readback mech feed fallback (METAL-GATE-AUDIT-2 8.7 inventory
**row 9**), commit `66f5c3e9`, ladder rung 8.7am. The native CPU mech feed
(geometry from `s_vkVboNativeCopy`+`s_stagingIbo`, per-frame LightsData from
`mcTextureManager->lightData_`) is now the ONLY mech feed path: it is populated
whenever the VK mech island is active (`s_vkMechGateOn`), not just under the retired
`MC2_VK_MECH_FEED_NATIVE` opt-in, and the island's `glGetBufferSubData` geometry +
per-frame LightsData SSBO readbacks are gone. Fail-soft replaced by explicit
fail-hard-with-retry (geometry) / flat-lit-this-frame (lighting) -- no silent GL
rescue. `MC2_VK_MECH_FEED_NATIVE` is now a parseable no-op that prints a deprecation
note. Pure-GL product path untouched (all edits `#ifdef MC2_VULKAN_ISLAND` /
`s_vkMechGateOn`-gated / in VK-island-only accessors).

Verification at clean deployed `66f5c3e92076` (dirty=0, lane `mc2-win64-0.4c`):
strict no-interop proof re-run PASS both modes (handoff-strict
`.claude/vk-nointerop-item9-proof/` and resource-strict
`.claude/vk-nointerop-item9-proof-rstrict2/`, hardened checker `--expect-sha`,
`[VK_MECH_TIER1] used_vulkan>0 forbidden_*=0 fallback_reason=""`); default-umbrella
FEED-AUDIT `.claude/vk-item9-umbrella-audit/` shows `source=cpu-native` (was
gl-readback) with geometry + lights `mismatch=0` byte-identical, 122 mechs native;
canonical tier1 5/5 PASS both states (+0 destroys). First resource-strict attempt
hit the known intermittent swapchain `NATIVE_WINDOW_IN_USE` flake (present_frames=0
fail-soft, islands still native), cleared on retry. Next 8.7 deletion candidates:
item 4 semaphore-handoff branches (blocked on direct-chain-default / boot native-or-
decline), then items 5/2/1 (`gl_interop_view.cpp` family).

## 2026-07-10 — VK-NOINTEROP-STRICT-REVIEW: proof regenerated, prior proof invalidated

The checked-in no-interop proof (`.claude/vk-nointerop-resource-strict-review-current/`,
untracked, built dirty at `a7e3e5af`, 7 commits behind) was a **realized false-pass**:
`check_vk_nointerop_log.py` exited 0 while the 126 ms/frame 8192² glGetTexImage CSM
readback was active (later killed by `62707331`). Adversarial review found the checker
FORBIDDEN[] greped only trace/present strings (never GL-readback/interop entry points),
the islands' `forbidden_*` counters were **declared and printed but never incremented**
(terrain fallback readback bumped nothing — H1/H2), `max()`-over-frames let one good
frame pass (H3), the GL canaries were emitted-but-never-asserted and their gates weren't
even set (H4), `MC2_VK_*_INTEROP=1` sat unreconciled in a "no-interop" proof (H5), and
there was no dirty/sha provenance gate (H6).

Hardened this session (checker + runner + engine):
- Checker rewrite: per-frame `[VK_*_PERFRAME_READBACK]` marker in FORBIDDEN; every island
  health line REQUIRED present with all `forbidden_*`/`depthin_*` == 0 (missing = FAIL);
  frame-consistency from the single `[VK_PRESENT]` line (≥ min-frames AND ≥ 0.5·present);
  canary lines REQUIRED with draws=0/any_samples=0; `MC2_VK_SHADOW_DIRECT_CHAIN=1` required
  when any interop name set; `--expect-sha` provenance (manifest git + per-bookmark build
  stamps, dirty:false + sha match) + terrain `stage_us` ceiling.
- Engine: terrain island now increments `g_forbidden.glGetTexImage` + emits a one-time
  `[VK_TERRAIN_PERFRAME_READBACK v1]` marker at the per-frame shadow-readback fallback
  sites (`copy_shadow_slot` perFrame callers, `copy_csm_array`, bisect-2); copy-once
  asset-feed imports stay unwired (allowed, per METAL-GATE-AUDIT-2). `MC2_VISUAL_CAPTURE_EXIT_AFTER`
  requests a clean shutdown after the sweep so island destroy + final logHealth reach the log.
- Fingerprint contradiction fixed: `manifest_schema.git_block` now uses `git status
  --porcelain -uno` (tracked-only), matching the engine build stamp; the manifest writer
  had been overcounting untracked scratch as dirty.

Proof regenerated at `7a5bf1fa` with the hardened checker + canary gates enabled (visible
window, clean-exit after sweep). Two independent runs both **VERDICT PASS** (exit 0):
all 5 island health lines present with `forbidden_*`/`depthin_*` == 0, zero PERFRAME_READBACK
markers, terrain `stage_us`=7.4µs, canary GL-skip invariant (`gl_handoff_skip_frames`==
`present_frames`==900, `copy1_blit_frames`==0), provenance dirty:false + sha match. Artifacts
(untracked, matching the prior dir): `.claude/vk-nointerop-proof-7a5bf1fa/{run1,run2}/`.
Prior proof archived to `.claude/vk-nointerop-proof-archive-a7e3e5af/` (invalidated, not deleted).

Regen surfaced (and fixed) two real residuals the OLD dead-counter checker masked: a 2-frame
terrain warmup CSM GL readback (`forbidden_getTexImage`=2, now fail-soft under strict) and a
never-called `mc2_vk_shadow_island_destroy()` (leaked island + missing `[VK_SHADOW_INTEROP]`
proof line, now torn down). Determinism receipt (`run1/determinism_hashes.json`): only 2/11
bookmark PNGs are byte-identical across runs — the engine self-labels `deterministic=True`
(fixed sim clock) but cross-run PIXELS are not byte-stable in the VK strict stack (warmup
timing). The no-interop invariant itself IS deterministic (forbidden_*=0 on both runs); the
pixel-determinism caveat the review flagged is now measured, not assumed.

## 2026-07-10 — VK-DRAW-PERF-1 phase 1: GPU-timestamp breakdown

`721088fa` perf(vk): `MC2_VK_GPU_TIMESTAMPS` island GPU segment timers
(header-only `GameOS/gameos/vk_gpu_timestamps.h`, default-OFF, verification-only
class, smoke-allowlisted + tier1_env_vars documented + flags.auto.yaml regenerated).
`vkCmdWriteTimestamp` (VK 1.1 core, no device feature — queue timestampValidBits +
limits.timestampPeriod only, MoltenVK-clean) brackets the terrain + mech island
render passes with 3 stamps (pass-start / draws-start / pass-end). Emits a
`[VK_TERRAIN_ISLAND]`/`[VK_MECH_TIER1] GPU_TIMESTAMPS ...` line at island teardown.
Gate OFF = zero query commands, byte-identical.

Strict-native measured breakdown (800x600, EMA; full table in `.claude/VK-OPT-BACKLOG.md`):
- **terrain = GPU FRAGMENT-bound** — gpu_draw 1026µs (mc2_01) / 817µs (mc2_24) =
  67-73% of cpu_draw_us; setup 5µs negligible; ~2ns/px, scales to ≈4400µs at 1080p
  (matches the reported baseline). Vertex/bind cost is trivial (20-32 patches).
- **mech is NOT GPU-draw-bound** — gpu_draw only 17-25µs even at 122 inst/14814 verts.
  The "mech ~5000µs draw" premise is a misdiagnosis; mech cost is `stage_us`
  (SSBO build/upload 150-522µs) + submit/fence-wait, not the GPU pass.

Phase 2 ships NO fix: the VK terrain frag is byte-matched to GL (dyn CSM 8 taps,
static 16 taps, POM early-exit break all = GL) so there is no parity-safe fragment
reduction — a cut would violate the parity mandate; GL pays the same cost
(features-not-perf, not a regression). Verification: two strict-native visible
captures gate-ON, 11/11 deterministic bookmarks each, no validation errors / strict
abort / crash; `.claude/vk-drawperf/mc2_01/` + `.claude/vk-drawperf/mc2_24/`.
Honest next lever = per-island submit/fence-wait serialization (structural, deferred).

## 2026-07-10 — VK-SWAPCHAIN-RESIZE-HARDEN-1 (LATEST — closes the NATIVE_WINDOW_IN_USE follow-up)

`5207a070` fix(vk): swapchain resize recreate hardening. **Root-caused** the
intermittent `vkCreateSwapchainKHR → VK_ERROR_NATIVE_WINDOW_IN_USE`
(VkResult=-1000000001): `rebuildSwapchain`'s fallback retry created a fresh
swapchain with `oldSwapchain=VK_NULL_HANDLE` **while the old swapchain was still
alive on the same surface** (it was destroyed only *after* the retry). A surface
that still owns a live swapchain rejects a second create with
NATIVE_WINDOW_IN_USE. Fix: attempt 0 keeps proper `oldSwapchain` chaining; on
failure the old chain is **retired before** any oldSwapchain-free retry. Added
bounded retry (3 attempts) + `SDL_Delay(4)` backoff + re-quiesce for genuinely-
transient window states, and `resize_recreate_ok / _retries / _failsoft` receipts
in the `[VK_PRESENT]` health line. Fail-soft to GL stays the final fallback.
Only the error/retry path changed → a first-try success is byte-for-byte the old
happy path → **no gate**.

Verification (deploy `A:/Games/mc2-opengl/mc2-win64-0.4c`):
- **Resize-driver proof** (7 real OS `SetWindowPos` resizes on our exact PID,
  non-strict VK present): `resize_count=8 resize_recreate_ok=8
  resize_recreate_retries=0 resize_recreate_failsoft=0`, **0 NATIVE_WINDOW_IN_USE**,
  `present_frames=4501`, `fallback_reason=""`. Scene rendered across every resize.
- **Strict no-interop visible** (`.claude/vk-resize-harden-strict-proof/`): 11/11
  bookmarks present + deterministic; `check_vk_nointerop_log.py` **PASS** (all
  forbidden strings 0, `present_frames/copy1_skip/gl_handoff_skip/composite_vk/
  imgui_vk = 4200`, `copy1_blit=0`, env receipts present); new receipts emit in
  strict too (`resize_recreate_ok=1 retries=0 failsoft=0`).
- **Canonical GL tier1 5/5 PASS.**
- **Not reproduced**: the pathological NATIVE_WINDOW_IN_USE itself (desktop-state-
  dependent, intermittent; no runtime-resizable gate exists to force it). Fix is
  verified by root-cause code inspection + heavy exercise of the recreate path
  (8+ clean recreates) with the new counters showing 0 retries / 0 failsoft.

## 2026-07-10 — VK-TERRAIN-STAGING-PERF-1 + VK-IMGUI-SHUTDOWN-GUARD-1

Two P0 fixes committed on `claude/vk-interop-bridge-1` (build dir `build64_island`,
deploy `A:/Games/mc2-opengl/mc2-win64-0.4c`, exe sha `1715683481da8911`):

1. **`62707331` perf(vk): VK shadow producer in direct-chain mode.** Under
   `MC2_VK_SHADOW_DIRECT_CHAIN` the shadow island kept `useInterop=false`, so all
   `mc2_vk_shadow_island_run*` entry points declined and the strict native stack
   fell back to the terrain island's per-frame GL CSM `glGetTexImage` readback
   (8192^2 x layers). Added a `directChain` flag; producers now gate on
   `(useInterop || directChain)`. **mc2_01 strict native, before→after:
   stage_us 126815 → 3.0, avg_fps 7.4 → 79.0** (copy stats static/dyn/csm all
   <1us; csmArrayDirect/csmFullMapDirect=1). forbidden_getTexImage=0. 11/11
   bookmarks deterministic (proof-C hashes byte-identical to staging-A →
   VK self-diff 0.000). Hardened `check_vk_nointerop_log.py` PASS on the clean
   present log (present_frames=15600, composite/imgui_vk=15600, copy1_blit=0, no
   forbidden strings). Instrumentation: per-step sub-timers behind
   `MC2_VK_TERRAIN_COPY_STATS`.

2. **`9f43b092` fix(vk): guard ImGui Vulkan shutdown.** Exit-time AV (READ at
   0x88 in `ImGui_ImplVulkan_DestroyDeviceObjects`) when
   `shutdownImguiVulkanRenderer()` ran with `imguiVkInited` true but the ImGui
   context/BackendRendererUserData already gone (double-call from
   GuiRuntime::Shutdown + EnginePresentVk::destroy; also reachable after a
   present-init/swapchain-resize failure). Guarded the shutdown call on
   `GetCurrentContext()` + `BackendRendererUserData` non-null.

Both states green: **canonical GL tier1 5/5, VK strict tier1 5/5**, all clean
exits (dump_kind=shutdown, no crash report). VK still slower than GL per standing
"features not perf" ruling — but the 7.4→79 FPS fix removes the staging-path
pathology, not just a slowdown.

Remaining next dominator (mc2_01 strict): `draw_us` (terrain ~4400us, mech
~5000us) now dominates the island timers — GPU draw/submit cost, not staging.
Also seen this session: intermittent swapchain-resize `NATIVE_WINDOW_IN_USE`
(VkResult=-1000000001) when the window can't resize past 800x600 in some desktop
states — fail-soft (scene still renders+captures), now exits clean thanks to
fix #2; worth a follow-up harden of the resize retry path. **[CLOSED 2026-07-10 by
VK-SWAPCHAIN-RESIZE-HARDEN-1 `5207a070` — see top block.]**

---

HEAD routing note (2026-07-08): current shipped tip is rung 8.7ab
`VK-SHADOW-DIRECT-CHAIN-1` after the shadow-enabled visible no-interop strict proof
on branch `claude/vk-interop-bridge-1`, worktree
`A:/Games/mc2-vk-interop-bridge`, build dir `build64_island`.
Always confirm the exact commit with `git log -1 --oneline` before launching
agents; this file is append-heavy and older rung sections below are historical.
Deploy lane for Vulkan ownership work is the literal
`A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe`; pass `--exe` explicitly for
smoke, visual, and RenderDoc harnesses. Do not rely on default deploy/visual
lanes (`v0.4`, `v0.4d-rc1`, or non-v-prefixed `0.4c` paths).
Rung-8 sub-ladder: `.claude/VK-RUNG8-LADDER-1.md` (8.1-8.7w shipped). Visible
strict proof for the full native final tail lives at
`.claude/vk-present-native-strict-visible-clean/`: 3/3 deterministic
`mc2_01_buildings` bookmarks, `present_frames=2700`, `copy1_blit_frames=0`,
`gl_handoff_skip_frames=2700`, `composite_vk_frames=2700`,
`imgui_vk_frames=2700`, `fallback_reason=""`. Next work is not 8.4/8.5 replay;
it is remaining deletion readiness around `gl_interop_view.*`, shared scene
color/depth/present substrates, and any diagnostic/non-CSM fallback paths still
needed for the final Metal gate.

Current final-gate canaries: `MC2_VK_NO_GL_INTEROP_STRICT=1` aborts on any
production `glInteropSignalToVk`/`glInteropWaitFromVk` call, while
`MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT=1` aborts on exportable Win32
`VkImageResource` creation or `glInteropViewCreate`. Proof gates
`MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR=1`, `MC2_VK_PROP_DIRECT_CHAIN=1`,
`MC2_VK_MECH_DIRECT_CHAIN=1`, `MC2_VK_WATER_TIER1=1`,
`MC2_VK_WATER_DIRECT_CHAIN=1`, `MC2_VK_POST_FOG_NATIVE=1`,
`MC2_VK_POST_FOG_DIRECT_CHAIN=1`, `MC2_SSAO=1`,
`MC2_VK_POST_SSAO=1`, `MC2_VK_POST_SSAO_DIRECT_CHAIN=1`,
`MC2_SHADOW_ENABLE=1`, `MC2_SHADOW_CSM=1`,
`MC2_VK_SHADOW_INTEROP=1`, `MC2_VK_SHADOW_DIRECT_CHAIN=1`,
`MC2_VK_CSM_ARRAY_INTEROP=1`, `MC2_VK_CSM_FULLMAP_INTEROP=1`,
`MC2_VK_STATIC_SHADOW_MAP=0`, and `MC2_VK_POST_SCREENFX=1`,
`MC2_VK_POST_SCREENFX_SHADOW=1`, `MC2_VK_POST_SCREENFX_DIRECT_CHAIN=1`,
`MC2_HZB_BUILD=1`, `MC2_VK_POST_HZB=1`, `MC2_VK_POST_HZB_DIRECT_CHAIN=1`,
`MC2_VK_POST_COMPOSITE_DIRECT_CHAIN=1` now make a visible `mc2_01` full
bookmark sweep pass with the handoff strict canary armed:
`.claude/vk-nointerop-strict-visible-shadow-direct-clean-final/`, 11/11
deterministic. Log shows `sha=53311e5aced3 dirty=0`,
`MC2_VK_SHADOW_DIRECT_CHAIN=1 -- VK shadow products stay VK-owned; GL
semaphore wait is skipped`, first VK shadow/CSM products, present
`composite_vk_frames=6600 gl_handoff_skip_frames=6600`, and zero occurrences
of `[VK_NO_GL_INTEROP_STRICT]`, `glInteropSignalToVk`, or
`glInteropWaitFromVk`. Detailed post-health receipt is in the immediately prior
dirty proof `.claude/vk-nointerop-strict-visible-shadow-direct-dirty-receipts/`
with screenfx `shadow_frames=6629 direct_chain=1 used_interop=0` and
HZB/fog/SSAO/water/prop all `used_interop=0`.
Follow-up 8.7ad dirty proof
`.claude/vk-nointerop-strict-visible-static-off-dirty/` forced
`MC2_VK_STATIC_SHADOW_MAP=0`; it still passed 11/11 deterministic bookmarks
with no `VK_SHARED_STATIC_SHADOW` log lines and screenfx
`shadow_frames=4687 direct_chain=1 used_interop=0 fallback_reason=""`. This
removes the static-shadow export/import from the strict stack; resource-strict
still fails earlier at shared scene color/depth/present.
Follow-up 8.7ae harness proof
`.claude/vk-nointerop-strict-visible-ui-tail-long-dirty/` added native UI tail
gates to the strict stack: `MC2_VK_HUD=1`, `MC2_VK_HUD_COLOR_PROOF=1`,
`MC2_VK_HUD_SKIP_GL=1`, and `MC2_VK_IMGUI=1`. It passed 11/11 deterministic
bookmarks on clean deployed binary `sha=d5faad0feb69 dirty=0`; final present
receipt reached `present_frames=8100`, `copy1_skip_frames=8100`,
`gl_handoff_skip_frames=8100`, `tail_hook_frames=8100`,
`composite_vk_frames=8100`, `hud_vk_consumed_frames=7258`,
`hud_vk_textured_draws_last=69`, `imgui_vk_frames=8100`,
`fallback_reason=""`, with `[VK_HUD_SKIP v1] ... reason="native_full_coverage"`
and no `[VK_NO_GL_INTEROP_STRICT]` abort.
Follow-up 8.7af present no-import proof (commit `3339ca20`) added `MC2_VK_PRESENT_NO_GL_IMPORT=1`: EnginePresentVk allocates the present target as VK-only/non-exportable when the strict native final-tail stack is armed, and `gosPostProcess::endScene()` uses a VK-only direct-present sentinel instead of a GL FBO. Clean proof `.claude/vk-nointerop-strict-visible-present-noimport-clean-final/` passed 11/11 deterministic `mc2_01` bookmarks on deployed binary `sha=3339ca203cb6 dirty=0`; final present receipt reached `present_frames=6300`, `copy1_blit_frames=0`, `copy1_skip_frames=6300`, `gl_handoff_skip_frames=6300`, `tail_hook_frames=6300`, `composite_vk_frames=6300`, `hud_vk_consumed_frames=5523`, `hud_vk_textured_draws_last=69`, `imgui_vk_frames=6300`, `fallback_reason=""`. Absence check: zero `[VK_NO_GL_INTEROP_STRICT]`, zero `glInteropSignalToVk`, zero `glInteropWaitFromVk`, zero `VK_SHARED_STATIC_SHADOW`, zero present resize/copy fallback strings. This removes the present-specific GL memory import; remaining resource-strict blockers are shared scene color/depth/object-id and other earlier-scene export/import substrates.
Follow-up 8.7ag strict harness hardening (commit `53a38a95`) added the shipped
max-native geometry/feed gates to `run_vk_nointerop_strict_visible.py`:
`MC2_VK_TERRAIN_ALL_LODS=1`, `MC2_VK_TERRAIN_SKIRTS=1`,
`MC2_VK_TERRAIN_VISUALDISPLACE_FAR=1`, and `MC2_VK_MECH_FEED_NATIVE=1`.
Clean proof `.claude/vk-nointerop-strict-visible-maxnative-harness-dirty/`
used deployed binary `sha=3339ca203cb6 dirty=0`, passed 11/11 deterministic
`mc2_01` bookmarks, and kept final present at `present_frames=6300`,
`copy1_blit_frames=0`, `gl_handoff_skip_frames=6300`,
`composite_vk_frames=6300`, `hud_vk_consumed_frames=5518`,
`imgui_vk_frames=6300`, `fallback_reason=""`. Absence check stayed zero for
`[VK_NO_GL_INTEROP_STRICT]`, `glInteropSignalToVk`, `glInteropWaitFromVk`,
`VK_SHARED_STATIC_SHADOW`, and present resize/copy fallback strings.
Follow-up 8.7 scene draw canaries added scoped GL `GL_ANY_SAMPLES_PASSED`
queries around `gosRenderer::drawTerrainOverlays()`,
`drawDecalStaticBatch()`, and `drawDecals()` with
`MC2_VK_TERRAIN_OVERLAY_GL_CANARY=1`, `MC2_VK_DECAL_STATIC_GL_CANARY=1`, and
`MC2_VK_TERRAIN_DECAL_GL_CANARY=1`. Dirty proof
`.claude/vk-scene-draw-canaries-strict-dirty-rerun/` passed 11/11
deterministic `mc2_01` bookmarks on deployed binary `sha=7a02638532cc
dirty=1`; final present receipt reached `present_frames=6300`,
`copy1_blit_frames=0`, `copy1_skip_frames=6300`,
`gl_handoff_skip_frames=6300`, `tail_hook_frames=6300`,
`composite_vk_frames=6300`, `hud_vk_consumed_frames=5522`,
`imgui_vk_frames=6300`, `fallback_reason=""`. Absence check was zero for
`[VK_NO_GL_INTEROP_STRICT]`, `glInteropSignalToVk`, `glInteropWaitFromVk`,
`VK_SHARED_STATIC_SHADOW`, present resize/copy fallback strings, and
`[VK_SCENE_GL_DRAW_CANARY v1]`. Interpretation: in the max-native strict stack,
the overlay/static-decal/terrain-decal GL draw loops did not execute for the
`mc2_01` sweep; the broader `MC2_VK_SCENE_GL_PRODUCER_QUERY` remains polluted by
GL shadow-map work and should not be used as a sceneFBO producer proof by
itself.
Follow-up 8.7ah fog direct-resource init (commit `42cedc66`) made
`vulkan_post_fog_native.cpp` require only shared `VkImageResource` handles when
`MC2_VK_POST_FOG_DIRECT_CHAIN=1`; GL views remain mandatory only for handoff
mode. This does not remove the shared depth/color export/import substrate yet,
but it removes a false GL-wrapper dependency from the first honest direct
consumer. Clean proof
`.claude/vk-nointerop-strict-visible-fog-direct-resource-clean-rerun/` passed
11/11 deterministic `mc2_01` bookmarks on deployed binary `sha=42cedc661baf
dirty=0`; fog health reached `attempted=6503`, `used_vulkan=6503`,
`direct_chain=1`, `used_interop=0`, `gl_skipped=13006`,
`fallback_reason=""`; final present reached `present_frames=6300`,
`copy1_blit_frames=0`, `copy1_skip_frames=6300`,
`gl_handoff_skip_frames=6300`, `tail_hook_frames=6300`,
`composite_vk_frames=6300`, `hud_vk_consumed_frames=5519`,
`imgui_vk_frames=6300`, `fallback_reason=""`. Absence check: zero
`[VK_NO_GL_INTEROP_STRICT]`, zero `glInteropSignalToVk`, zero
`glInteropWaitFromVk`, zero `VK_SHARED_STATIC_SHADOW`, zero present
resize/copy fallback strings.
Follow-up 8.7ai post direct-resource init (commit `8d721188`) applied the same
direct-chain readiness rule to SSAO and screenfx: `vulkan_post_ssao_native.cpp`
and `vulkan_post_screenfx_native.cpp` require shared `VkImageResource` handles
under their direct-chain gates, while GL views remain mandatory only for handoff
mode. Clean proof `.claude/vk-nointerop-strict-visible-post-direct-resource-clean/`
passed 11/11 deterministic `mc2_01` bookmarks on deployed binary
`sha=8d7211888a2c dirty=0`; SSAO health reached `attempted=6493`,
`used_vulkan=6493`, `direct_chain=1`, `used_interop=0`,
`fallback_reason=""`; screenfx reached `attempted=12986`,
`used_vulkan=12986`, `shadow_frames=6493`, `shore_frames=6493`,
`direct_chain=1`, `used_interop=0`, `fallback_reason=""`. Final present
reached `present_frames=6300`, `copy1_blit_frames=0`,
`copy1_skip_frames=6300`, `gl_handoff_skip_frames=6300`,
`tail_hook_frames=6300`, `composite_vk_frames=6300`,
`hud_vk_consumed_frames=5519`, `imgui_vk_frames=6300`,
`fallback_reason=""`. Absence check: zero `[VK_NO_GL_INTEROP_STRICT]`, zero
`glInteropSignalToVk`, zero `glInteropWaitFromVk`, zero
`VK_SHARED_STATIC_SHADOW`, zero present resize/copy fallback strings.
Follow-up 8.7aj scene-producer audit added `MC2_VK_SCENE_GL_PRODUCER_QUERY`
and `MC2_VK_TERRAIN_GL_FALLBACK_AUDIT`. Dirty proof
`.claude/vk-terrain-gl-fallback-audit-strict-dirty/` passed 11/11 deterministic
`mc2_01` bookmarks, kept no-interop strict absence at zero, and proved the live
terrain GL residue was entirely edge-stitch fallback: steady audit lines had
`inactive=0 band=0 displace=0 other=0` with `edge_stitch=3..4`; the scene query
flipped to `any_samples=1` in the same phase.
Follow-up 8.7ak terrain edge-stitch ownership ports the GL coarse stitch and
mode-2 stitched-bake vertex math into `terrain_tier1.vert` and
`terrain_tier1_skirt.vert`, adds packed `edgeStitch` plus `quadCountX/Y` to the
terrain VK patch UBO tail, and removes edge-stitch from the GL fallback
predicate. Dirty proof `.claude/vk-terrain-edge-stitch-strict-dirty/` passed
11/11 deterministic bookmarks with final present `present_frames=6300`,
`copy1_blit_frames=0`, `gl_handoff_skip_frames=6300`,
`composite_vk_frames=6300`, `hud_vk_consumed_frames=5525`,
`imgui_vk_frames=6300`, `fallback_reason=""`. Terrain audit stayed at
`gl_fallback=0 edge_stitch=0` through frame 6300, proving terrain no longer
rasterizes through GL under the max-native strict stack. The scene producer query
still reports `any_samples=1`, so the next ownership slice is a non-terrain
sceneFBO writer canary/port (likely overlays, decals, or another direct world
draw), not resource-strict deletion yet.
Clean proof `.claude/vk-terrain-edge-stitch-strict-clean/` reran the same gate
on `sha=0aae033779b9 dirty=0` and preserved the same result: 11/11
deterministic, terrain `gl_fallback=0` through frame 6300, no strict interop, and
final present `present_frames=6300`, `copy1_blit_frames=0`,
`gl_handoff_skip_frames=6300`, `composite_vk_frames=6300`,
`hud_vk_consumed_frames=5600`, `imgui_vk_frames=6300`, `fallback_reason=""`.
Follow-up scoped canary `.claude/vk-screenshadow-gl-canary-strict-dirty/`
proved the broad `MC2_VK_SCENE_GL_PRODUCER_QUERY` is over-broad: it spans GL
shadow-map rendering, so `any_samples=1` is not by itself evidence of a
sceneFBO producer. The scoped `MC2_VK_SCREENSHADOW_GL_CANARY=1` wrapper around
the legacy `runScreenShadow()` GL draw emitted no draw lines in the strict stack,
while native screenfx health reported `shadow_frames=3245`, `gl_skipped=6490`,
`direct_chain=1`, `used_interop=0`, `fallback_reason=""`. Next producer search
must use scoped per-pass canaries, not the broad beginScene/endScene query.
Final exact-binary regression after reviewer fixes:
`.claude/vk-nointerop-strict-visible-final-smoke/`, engine smoke PASS,
7 deterministic bookmarks before duration, `composite_vk_frames=300`,
`gl_handoff_skip_frames=300`, `VK_WATER_TIER1 used_interop=0`, no strict abort.
LowLight alternate composite mode is also strict-clean at
`.claude/vk-nointerop-strict-visible-lowlight/` (`MC2_VIEW_MODE=lowlight`,
engine smoke PASS, `composite_vk_frames=300`, no strict abort). ObjectIdDebug
strict is green under `MC2_VK_OBJECTID_DIRECT_CHAIN=1`; it is a deletion canary,
not a pixel-parity port for native ObjectId MRT2 writes. Next direct-chain work
is shadow-specific VK->GL sampling paths and any diagnostic fallback paths still
needed before deleting `gl_interop_view.*` and Win32 external-memory plumbing.

Verification guardrails:
- Bracket every proof with lane hygiene: deployed exe fingerprint/manifest plus
  `[VK_*]` health lines showing `fallback_reason=""` for the feature under test.
- Minimized smoke is valid for crash/telemetry but cannot prove direct present,
  HUD/ImGui, or nonzero-surface final-tail behavior. Use visible bookmark
  captures for those.
- Bookmark sweep files must contain at least two entries; single-pose bookmark
  files are parser canaries only.
- RenderDoc capture is a separate lane; use `scripts/renderdoc_capture.py --exe`
  against the same `mc2-win64-v0.4c/mc2.exe`.
- Before deleting interop, add or use a canary mode that hard-fails on any
  shared-scene GL fallback/import path, so a passing smoke cannot hide a GL
  substrate rescue.

## RUNG 8.3 -- VK-POST-SCREENFX-NATIVE-1 (2026-07-07) -- SHIPPED

Dirty implementation recovered from the interrupted 8.3 agent and verified in-place.
New TU `GameOS/gameos/vulkan_post_screenfx_native.cpp`: cloud-shadow +
shoreline-foam render natively on the SHARED ENGINE VulkanContext, in one
render pass / one submit, multiplying into shared scene color0 in place. Inputs:
shared scene depth for cloud, shared scene normal/GBuffer1 for shoreline. Gate
`MC2_VK_POST_SCREENFX` (envTriState opt-in, default-OFF) with per-pass bisect
subgates `MC2_VK_POST_SCREENFX_SHADOW`, `MC2_VK_POST_SCREENFX_CLOUD`, and
`MC2_VK_POST_SCREENFX_SHORELINE`.
Return bitmask skips exactly the GL pass bodies that VK rendered. Screen-shadow
is intentionally NOT ported in this rung because the CSM cascade array has no
zero-copy VK producer yet; porting it now would add readbacks and violate the
rung goal.

Harness/code fixes during recovery:
- Fixed malformed `debug_state_dump.cpp` raw JSON insertion for
  `vk_post_screenfx_native`.
- Added missing `vk_shared_gbuffer_gate.h` include in `gos_postprocess.cpp`.
- Added `MC2_CLOUD_SHADOW` to `scripts/run_smoke.py` allowlist; without it,
  cloud proof was impossible because Popen dropped the gate. Corrected docs to
  match code: `MC2_CLOUD_SHADOW=1` is default-OFF.

Evidence (`.claude/vk-post-screenfx/`):
- Build: `build64_island` Vulkan target PASS; shader target PASS; deployed to
  `A:/Games/mc2-opengl/mc2-win64-0.4c` with explicit `--exe` lane.
- Offline: `check_ubo_layout.py --pair screenfx_cloud` PASS; `--pair
  screenfx_shore` PASS. `glsl_port_diff.py --all` shows the 4 pre-existing
  older divergences unchanged and the new pairs VERBATIM: `cloudShadowNoise`,
  `cloudShadowMain`, `shorelineNoise`, `shorelineMain`.
- Shoreline proof (`mc2_01`, water bookmark, gate ON both passes): capture
  receipt `attempted=280 used_vulkan=280 cloud_frames=0 shore_frames=280
  gl_skipped=280 fallback_reason=""`; base run had no native receipt. Visual
  A/B vs gate-off showed expected shoreline effect concentrated on water poses
  (oblique mean MAD 4.602/ch, overview 0.291/ch).
- Cloud proof (`mc2_24`, `MC2_CLOUD_SHADOW=1`,
  `MC2_VK_POST_SCREENFX_SHORELINE=0`): smoke receipt `attempted=113
  used_vulkan=113 cloud_frames=113 shore_frames=0 gl_skipped=113
  fallback_reason=""`. Visual A/B (`vk_cloud_base` vs `vk_cloud_native`) all
  three bookmarks deterministic; `visual_diff.py` rollup PASS/WARN only
  (2 pass, 1 warn, 0 fail). MAD: highangle 0.013/ch, overview 0.039/ch,
  ridge 0.006/ch, changed <=0.090%. Full capture receipt:
  `attempted=1044 used_vulkan=1044 cloud_frames=1044 shore_frames=0
  fallback_reason=""`.
- Smoke: `mc2_01` shoreline gate PASS; `mc2_24` cloud-only PASS. Canonical
  tier1 both-state PASS on explicit `--exe A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe`:
  gate-off `2026-07-07T19-27-55` 5/5 PASS +0 destroys; gate-on
  `2026-07-07T19-31-16` 5/5 PASS +0 destroys with `MC2_VK_POST_SCREENFX=1`
  and `MC2_CLOUD_SHADOW=1`. Gate-on receipts across all tier1 missions:
  attempted==used_vulkan, fallback_reason="", and both `cloud_frames` +
  `shore_frames` incremented (`mc2_01` 112/112, `mc2_03` 114/114,
  `mc2_10` 114/114, `mc2_17` 112/112, `mc2_24` 104/104).

Historical residuals at time of 8.3: this previously said to advance to 8.4
HZB compute. That work is now shipped; use the HEAD routing note above and
`.claude/VK-RUNG8-LADDER-1.md` for current next work. Keep historical receipts
below for provenance only.

## RUNG 8.2 -- VK-POST-SSAO-NATIVE-1 (2026-07-07) -- SHIPPED

Commits `658107ae` (feature) + `786f6999` (gate registration). New TU
`GameOS/gameos/vulkan_post_ssao_native.cpp`: SSAO gen+apply rendered natively on
the SHARED ENGINE VulkanContext -- gen samples the shared scene DEPTH + NORMAL
directly into a VK-OWNED half-res R16_SFLOAT occlusion image (the ladder's one
intermediate; device-local, non-shared), apply 4-tap-box-upsamples it (LINEAR)
and MULTIPLIES (DST_COLOR/ZERO) into the shared scene color0 IN PLACE. Sync =
8.1 fog pattern extended to THREE shared resources (color0 + normal + depth
BeginFrame/sems both directions; in-cb barriers depth DS<->SHADER_READ_ONLY and
normal COLOR_ATTACHMENT<->SHADER_READ_ONLY around the gen pass). Gate
`MC2_VK_POST_SSAO` (ladder's name; envTriState opt-in, default-OFF, new-rung
pattern). Highest precedence at the SSAO site (native > region-iface > GL);
parity guards mirror runSSAO()'s exact gate set (MC2_SSAO master gate,
sceneHasTerrain, programs/FBO valid) + falls through to GL when
`MC2_SSAO_DEBUG!=0` (grayscale-overwrite diagnostic NOT ported -- explicit
limitation). SHADERS REUSED VERBATIM: the island's parity-proven
ssao/ssao_apply SPIR-V (R16F format parity, LINEAR apply-sampler parity,
column-major matrix parity all inherited).

KILLS (vs the old vulkan_ssao_island readback pattern, measured live in the
attribution run): per-frame glGetTexImage DEPTH + NORMAL readbacks + staging
uploads, scene-color pull/preload + blended-color readback/writeback
(island forbidden_* counters showed 2497 each per 2497-frame run,
total_us ~15281/frame vs native render_us 300-680).

Evidence (`.claude/vk-post-ssao/` -- ssao_oracle.py + cap/*; MC2_SSAO=1 in ALL
configs; stock AO tunables are near-invisible at RTS poses, so parity was also
proven under a boosted tuning profile `visual_tuning_aoboost.json`
(aoRadius 24 / aoStrength 2.0 via MC2_VISUAL_TUNING_FILE) where the GL AO
effect is measurable (0.03-0.48/ch cov 1-5%)):
- Offline trio: check_vk_shaders 34/34, check_ubo_layout 7/7 pairs OK (new
  ssaonat_occ 7 fields / ssaonat_apply 1 field), glsl_port_diff --all baseline
  unchanged (no new ports; SPIR-V reuse).
- Byte-off control (gl vs gl2, MC2_SSAO=1, stock AND boosted tunings, 5 AO
  poses mc2_01): 0.000 all non-sacrificial poses.
- Receipts: gate-ON sweeps attempted==used_vulkan (479-495), fallback_reason="";
  gate-ON tier1 smoke used_vulkan=193-198/mission; debug-state JSON section
  `vk_post_ssao_native` (postssao_used_vulkan) verified in
  latest_render_state.json.
- Single-variable MAD (vk_base vs vk_ssao, SAME VK scene, boosted): at the
  deterministic poses 0.047-0.090/ch cov 2-5% -- SMALLER than the GL AO effect
  itself (0.19-0.48/ch) and well INSIDE the impl-swap family residual measured
  same session on the parity-proven OLD island (gl vs gl_island: up to
  0.92/ch cov 11.8% at cliff_side_on). overview_center/ridge_lowangle diffs
  (0.28/1.71) == the vk scene run-to-run SELF-noise at those poses (base
  self-diff 0.27/1.64) -- scene noise, not SSAO.
- vk_ssao self-diff determinism: 0.000 at all scene-deterministic poses.
- Sacrificial first bookmark used every sweep (VK-FIRST-BOOKMARK-POSE-1).
- Smoke: canonical tier1 5/5 PASS BOTH states on the fingerprint-verified exe
  (sha 786f69990c17 == HEAD), +0 destroy-delta all missions. Gate-ON ~6 FPS =
  the pre-existing umbrella-under-smoke cost (rung-6/7/8.1 note), not SSAO
  (native render_us ~300).

Residuals / next: (1) tiny impl-swap residual (<=0.09/ch boosted) inherited
from the island port family (hash12 sin/fract precision class) -- far below
the island's own accepted residual, not itemized; (2) MC2_SSAO_DEBUG
grayscale mode unported (falls through to GL, documented); (3) old SSAO
island + its readback taxes become DELETE candidates at rung 8.7. NEXT
SUB-RUNG: 8.3 screen-shadow/cloud-shadow/shoreline (MC2_VK_POST_SCREENFX,
needs verbatim shader ports + glsl_port_diff registration) or 8.4 HZB compute.

## RUNG 8.1 — VK-POST-FOG-NATIVE-1 (2026-07-07) — SHIPPED

First rung-8 (postprocess-tail ownership) sub-rung. New TU
`GameOS/gameos/vulkan_post_fog_native.cpp`: BOTH postprocess fog passes
(edge + oob) rendered natively on the SHARED ENGINE VulkanContext — samples the
shared scene depth directly, alpha-blends into the shared scene color0 IN PLACE
(water-tier1 sync protocol: dual BeginFrame + GL sems both directions, in-cb
depth DS_ATTACHMENT<->SHADER_READ_ONLY barriers, layout-stable LOAD/STORE pass).
Gate `MC2_VK_POST_FOG_NATIVE` (envTriState opt-in, default-OFF, new-rung
pattern). Highest precedence at the fog site (native > subgraph > island > GL);
parity guards fall through to GL when either GL fog pass would self-skip or
`MC2_SKYBOX_FOG_EXCLUDE=1` (VK fog shaders have no stencil sky-exclude path —
pre-existing subgraph limitation, now explicit). Shaders reused VERBATIM
(edge_fog/fog_oob SPIR-V); UBO PODs offline-checked (check_ubo_layout labels
postfog_edge 8 fields / postfog_oob 4 fields OK).

KILLS (vs the old fog subgraph path): per-frame `glGetTexImage(DEPTH)` CPU
readback + staging upload ("P5" finally closed), 2x per-frame
`glCopyImageSubData` color round-trips, and the subgraph's PRIVATE
VkInstance/VkDevice (which is provably UNUSABLE under `MC2_RENDER_BACKEND=vulkan`:
R2 interlock coerces its interop OFF and the readback-mode run CRASHED in
present swapchain rebuild — captured in `.claude/vk-post-fog/cap/vk_subgraph/`).

Evidence (`.claude/vk-post-fog/` — fog_oracle.py + cap/*):
- Offline trio: check_vk_shaders 34/34, check_ubo_layout 5/5 OK, glsl_port_diff
  --all green (no new shader ports; fog SPIR-V pre-dates the tool, reused as-is).
- Byte-off control (gl vs gl2, 5 fog poses mc2_01): 0.000 — clean.
- Receipts: sweep used_vulkan=495/495 attempted, fallback_reason="";
  gate-ON tier1 smoke mc2_01 used_vulkan=193/193; debug-state dump section
  `vk_post_fog_native` (postfog_used_vulkan) added for future captures.
- Single-variable MAD (vk_base vs vk_fog, SAME VK scene): oblique_fog
  10.13/ch cov 19.0%, oblique_horizon 8.51/ch cov 40.6% — ATTRIBUTED: the GL
  fog-impl-swap control (gl vs gl+subgraph, the parity-proven shipped VK fog)
  shows the SAME signature (10.89/19.3%, 8.62/40.7%), i.e. the delta is the
  KNOWN family fog-port residual, not a new defect. Remaining pose diffs
  (boundary_south 3.66@16%, overview 2.37@0.8%) == vk_base SELF-noise
  (base-vs-base2 3.69@15.2%, 2.34@0.7%); vk_fog itself is run-to-run
  DETERMINISTIC (self-diff 0.000 all poses).
- Smoke: canonical tier1 5/5 PASS both states, Δ destroys +0, manifest exe
  sha `46e81cb1d9f2f7c1` == deployed build. Gate-ON ~6 FPS = the pre-existing
  umbrella-under-smoke cost (identical without the fog gate; rung-6 note).

Residuals / next: (1) known fog-port residual at oblique poses (~8-11/ch in
the fog band) is inherited from the original island/subgraph port — if ever
worth closing, diff edge_fog.frag GL-vs-VK math paths and register
glsl_port_diff pairs; (2) sky-exclude (MC2_SKYBOX_FOG_EXCLUDE) unsupported
natively — port the stencil sample if that gate ever defaults ON; (3) the
subgraph + old fog islands become DELETE candidates at rung 8.7 (ladder), not
before. NEXT SUB-RUNG: 8.2 SSAO native (same shape: shared depth in, shared
color0 multiply out, engine context).

## RUNG 7 -- VK-MECH-FEED-NATIVE-1 (2026-07-07) -- SHIPPED

Commits `28fc0c9f` (code) + `f1f49df4` (gate registration). Gate
`MC2_VK_MECH_FEED_NATIVE` (envTriState opt-in, default-OFF, new-rung pattern);
safety gate `MC2_VK_MECH_FEED_AUDIT` (verification-only, HEIGHT_AUDIT class).
Scope note vs recon doc: the recon ladder's rung 7 headline is "mech material/
lighting full PBR" but material+lighting parity ALREADY landed pre-flip
(VK-FLIP-EVIDENCE-1 Lambert+fog+paint+scheme-cache); the remaining substance was
"kill the mech path's GL-object reads" (recon section A "Remaining interop
copies" row: these die at the next mech ownership rung). This rung delivers
exactly that subset:
- GEOMETRY (copy-once): island memcpy's the batcher's CPU sources -- new
  `s_vkVboNativeCopy` (the EXACT normals-recomputed `work` blob uploaded to
  s_sharedVbo; s_stagingVbo alone is NOT the uploaded bytes when
  s_mechNormalsMode!=0) + `s_stagingIbo` (uploaded verbatim). Kills both
  copy-once glGetBufferSubData readbacks.
- LIGHTSDATA (per-frame): island memcpy's `mcTextureManager->lightData_`
  (new public accessors getLightDataPtr/getLightDataBytes; the CPU array IS the
  byte source of the GL SSBO upload, txmmgr.cpp:2451-2459). Kills the ONLY
  per-frame GPU readback (glGetBufferSubData) on the mech path.
- Fail-soft: any missing CPU pointer (size-mismatch race, no texture manager)
  -> GL-readback path unchanged. Gate OFF = byte-identical old feed path.
- Feed struct additions: Mc2MechGeomFeed.vboCpu/iboCpu,
  Mc2MechFrameFeed.lightDataCpu/lightDataCpuBytes (null unless armed).
Evidence (oracle `.claude/vk-mech-feed-native/mech_feed_oracle.py`, caps under
`cap/mc2_24/` + `cap/mc2_01/`):
- AUDIT (byte-level golden-vs-native): FEED-AUDIT geometry mismatch_buffers=0 +
  lights byte-identical EVERY frame, mc2_24+mc2_01 sweeps AND all 5 tier1
  missions under gate-ON smoke (0 DIVERGES lines total). GL SSBO is
  capacity-sized so cpu<=gl bytes; min-prefix compare.
- Instance/cull parity: inst=122 bones=326 runs=60 geom_verts=14814 (mc2_24)
  and 38/140/25/5220 (mc2_01) IDENTICAL old-vs-native; used_vulkan receipts
  474/478/465 (VK runs), GL control used_vulkan=0.
- vk_feed_diff.py old-feed vs native-feed dumps: exit 0 (122 instances both).
- Single-variable MAD vk_old vs vk_native (mc2_24, sacrificial-first-bookmark):
  0.027-0.120/ch cov<=0.44% max~235 == the GLxGL byte-off floor measured same
  session (0.030-0.131/ch cov<=0.45%) -- transport change is pixel-neutral.
- Smoke: tier1 5/5 PASS both states, +0 destroy-delta (gates-OFF and full
  VK umbrella + FEED_NATIVE + FEED_AUDIT). Gate-ON FPS ~6 = the PRE-EXISTING
  umbrella-under-smoke cost, not a feed regression.
- TRAP (relearned): `run_smoke.py --deploy 0.4c` resolves to the STALE
  `mc2-win64-v0.4c` (v-prefixed) lane, NOT `mc2-win64-0.4c`. First smoke pair
  silently ran the old exe (log format betrayed it: no `source=` in the
  geometry line). Use `--exe A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe
  --no-lease` for this lane.
Residuals (honest, for rung 8 planning):
- Paint-scheme textures (run.slot0GlTex) still GL-sourced: copy-once
  glGetTexImage import keyed by live GL texture id (scheme cache). Making
  these native = mcTextureManager CPU-texture feed slice.
- Instance/bone/run snapshots are still BUILT by the GL batcher's flush()
  (CPU code, no GL reads, but flush-time production inside the GL frame) --
  full batcher-independence would mean reimplementing the skinning-palette/
  instance build outside flush; not needed to kill interop copies.
- GL-readback feed path retained as fail-soft (deletable at rung 8 when the
  GL side disappears).

## RUNG 6.5 — WATER REFLECTION PORT (2026-07-07) — SHIPPED (closes the rung-6 grazing gap)

Commits `f0de79c7` (port) + `006e2115` (glsl_port_diff + check_ubo_layout
registration). Ports GL's default-ON water reflection block
(gameos_graphics.cpp:3100 area; strength 1.5, `MC2_WATER_REFLECTION=0` kills)
VERBATIM into `shaders/vulkan/water_geom.frag`: SH-L2 sky coeffs + waterEvalHdri
(equirect, skyYaw-rotated, Reinhard, LOD from mc2WaterHdriLodDefault(), default
4.0) + waterEvalSkySh fallback + wave-normal/reflect setup + Schlick Fresnel mix
(REFL_F0 0.02 / REFL_MAX 0.55). 5 PORT pairs, ALL VERBATIM per glsl_port_diff.
- SKIPPED with evidence: (a) terrain-RT contact term — `MC2_WATER_REFLECTION_RT`
  default-OFF (`g_waterRtStrength=0.0` unless env set) -> the GL sub-block is a
  no-op in the field config; (b) sun-spec — `u_waterSpecIntensity` is uploaded
  but consumed by NO shader (grep of shaders/ = zero hits; dead upload), and
  defaults 0 anyway. Not RT ray-query — nothing MoltenVK-hostile added
  (VK 1.1 core; BC feature queried, fail-soft to SH if absent).
- Plumbing: WaterFsParams UBO +16B (f2.yzw = reflStrength/skyYaw/hdriLod, new
  f3 = screen size; `check_ubo_layout.py` waterfs pair registered, 7 fields OK),
  binding-2 combined image sampler, COPY-ONCE HDRI import from the GL texture
  (BC6H .ktx2 sidecar mip0 in the field config — GL's own textureLod clamps to
  mip 0 there, so 1-mip VK image is exact parity; RGBA16F+full-chain path for
  the tinyexr fallback). Honest cost: one-time staging copy at first armed-water
  frame (mc2_01 uses mud_road_puresky 16K BC6H = 128MB one-shot), ZERO per-frame
  cost; receipt `[VK_WATER_ISLAND] hdri REAL import: 16384x8192 fmt=143 mips=1`.
  Fail-soft = 1x1 dummy bound + fs UBO hdriLod patched to -1 -> SH-L2 fallback
  (same semantics as GL `!isHdriReady()`). Gate parity: fs UBO feeds
  `gos_GetWaterReflStrength()` -> `MC2_WATER_REFLECTION=0` skips the block on
  BOTH sides (proved live below).
- Offline: check_vk_shaders 34/34, check_ubo_layout 3/3 pairs OK,
  glsl_port_diff --all: 8 VERBATIM (5 new water pairs) + the 4 pre-existing
  recorded rename findings unchanged.
- Live (mc2_01 water sweep, sacrificial-first-bookmark, receipts every run:
  water island used_vulkan=488/472, thin=6167, recipes=9801; GL control
  used_vulkan=0): water-only delta (vk_base vs vk_water, SAME VK scene,
  single variable) at grazing oblique_low: **8.29/ch -> 3.64/ch full-image
  (cov 98%->38%)** == the rung-6 refl-off floor (3.56/ch cov 39%) — the
  reflection gap is CLOSED to the base-layer residual. overview_high 0.31/ch
  (bar <=0.6 met). Single-variable execution proof: vk_water vs
  vk_water_refloff (MC2_WATER_REFLECTION=0) = 9.35/ch cov 99% — the ported
  block demonstrably executes and honors the GL gate. Artifacts:
  `.claude/vk-water-tier1/cap/` (rung-6 baselines preserved in `cap_rung6/`),
  driver `run_65.py`, analyzer `mad_65.py`.
- Smoke: tier1 5/5 PASS both states (gates-OFF and full VK_BASE+WATER_TIER1),
  +0 destroy-delta all missions, worktree run_smoke vs 0.4c exe.
- Remaining rung-6 residual (unchanged, now the whole gap): base-layer 3.6/ch
  cov ~38% at grazing not yet itemized (suspects: fog band, alpha/blend LSB,
  detail layer); detail/spray layer (isWater==2) still GL-discarded both sides.

## RUNG 6 — VK-WATER-TIER1 (2026-07-07) — SHIPPED (base-layer, reflection-off subset)

Commits `06c4fe03` (island tier1 mode + gate registration) + `790388f8`
(waterBaseColor port markers). Gate `MC2_VK_WATER_TIER1` (default-OFF,
envTriState opt-in like SKIRTS/VISUALDISPLACE_FAR; precedence over the P9a
`MC2_VK_WATER_INTEROP`). `vulkan_water_island.cpp` tier1 mode renders the armed
water base layer IN-PLACE into the shared scene GBuffer (color0+normal+depth
LOAD-preserve, terrain-island sync protocol) — kills the P9a offscreen target,
the GL composite pass, AND the per-frame glGetTexImage depth readback
(receipts: depthin_getTexImage=0, depthin_copyBufToImg=0, forbidden_*=0 on the
live gate-ON run). Blend: FS stays premultiplied; pipeline color ONE/OMSA +
alpha SRC_ALPHA/OMSA == GL straight-alpha one-stage arithmetic; shared normal
attachment writeMask=0 (GL water GBuffer1 a=0 no-op parity). Depth: shared
depth LOAD (water runs after terrain), reverse-Z GEQUAL test+WRITE,
WATER_DEPTH_FUDGE_FAST 0.00025 lockstep unchanged (same VS).
- Offline: check_vk_shaders 34/34, check_ubo_layout OK; NEW port pair
  `waterBaseColor` (GL MDI FS vs water_geom.frag) = **VERBATIM** (17 lines).
- Byte-off control (gates unset, GL x2, mc2_01 water bookmarks): 0.000 /
  0.003 MAD — clean.
- Live gate-ON receipts (mc2_01, water bookmark sweep + sacrificial first
  bookmark for VK-FIRST-BOOKMARK-POSE-1): water island used_vulkan=492,
  vk_draws=1, thin=6167, recipes=9801; gate-OFF VK run: water used_vulkan=0.
- Water-only delta (vk_base vs vk_water, SAME VK scene, single variable):
  overview_high 0.54/ch cov 0.04%; oblique_low 8.29/ch cov 62%. ATTRIBUTED by
  experiment: GL water reflection defaults ON (strength 1.5 + HDRI LOD4,
  gameos_graphics.cpp:3100); with MC2_WATER_REFLECTION=0 the delta collapses
  to 3.56/ch cov 10.5% (oblique) / 0.32 (high). The VK FS is the documented
  reflection-OFF base-layer subset (P9a parity contract) — the dominant
  divergence is the MISSING reflection/HDRI feature, not a port defect.
- Depth discipline: shorelines correct, islands occlude water, no z-fight, no
  water-vanish (visual check on gate-ON captures). GL-vs-VK whole-frame pose/
  pillarbox framing difference at these bookmarks is the PRE-EXISTING VK-sweep
  camera artifact (present identically in vk_base, independent of this rung).
- Smoke: tier1 5/5 PASS both states (+0 destroys). Gate-ON smoke FPS ~7 is the
  PRE-EXISTING umbrella-under-smoke cost (measured identical without the water
  gate), not a water regression.
- Residuals / next sub-rung: (1) port the reflection block (SH-L2 sky + HDRI +
  RT sample + sun spec) into water_geom.frag + UBO to close the 3.6-8/ch
  grazing-angle gap — this is the remaining rung-6 feature work; (2) detail/
  spray layer (isWater==2 second MDI draw) still GL-discarded both sides, no
  port needed today; (3) refl-off residual 3.56/ch cov 10.5% at grazing not yet
  itemized (suspects: sun-spec default, fog band). Artifacts + oracle:
  `.claude/vk-water-tier1/` (water_oracle.py, cap/*).

## RUNG 5 — VK-TERRAIN-VISUALDISPLACE-FAR-1 (2026-07-07) — MODE-2 DONE

Commits `4c427cf9` (shaders) + `49ba7f25` (wiring). Gate
`MC2_VK_TERRAIN_VISUALDISPLACE_FAR` (default-OFF, envTriState opt-in, requires
ALL_LODS, smoke-allowlisted incl. GL `MC2_TERRAIN_VISUAL_DISPLACE_FAR` knob).
Scope: mode-2 far-band (lodStep>1, non-stitched) pure Z-swap ONLY; mode-1 (LOD0
4x topology) + stitched patches stay GL-owned (separate L slice later). VK skirt
TOP verts displace like GL (GL's mode-2 branch runs for skirt tops); bottoms stay
coarse-h - skirtDepth. New: binding-10 heightsFine SSBO (dummy 16B from frame 1),
snapshot_visual_height version-gated mission-load copy, UBO visualSide/
visualDisplaceFar/visualMode @ off 580/584/588 (1024B slot unchanged), per-patch
visualMode in Mc2TerrainTier1Patch, feed-dump displacedPatches field.
- Offline: check_vk_shaders 34/34, check_ubo_layout OK (47 fields), port pairs
  `visualDisplaceFar` x2 (main + skirt VS) = **VERBATIM** (local u_* aliases +
  anonymous SSBO instance keep the block textually identical to GL).
- Byte-off: control GL x2 gates-unset = 0.000 on 10/11 mc2_01 bookmarks
  (cross_map_view has PRE-EXISTING run-to-run drift, gates unset — excluded);
  displace-ON + umbrella+ALL_LODS but new gate OFF vs GL = 0.000-class (max MAD
  0.35/ch, cov <=1.4%) — predicate excludes everything, GL owns all pixels.
- Displaced-capture evidence: GL band-count telemetry LOD1=5 (GL ref run) ->
  LOD1=0 (VK gate-ON run) = VK captured exactly the 5 mode-2 patches; snapshot
  receipt `V=397 bytes=630436 ver=1`; used_vulkan=739 (VK run), 0 (GL run).
  NOTE: MC2_VK_FEED_DUMP produced no file this sweep (readyToDump gating) —
  evidence via band counters instead.
- Z-swap mechanism proof (single-variable): fade knob 1.0 vs 0.0, all else
  identical -> MAD 0.00-0.78/ch, cov <=0.81%, localized far-band silhouettes.
  The displacement math executes in VK and is small/localized as expected.
- GL-vs-VK absolute MADs gate-ON (mc2_01, 11 bookmarks): 13.6-44.3/ch, cov
  30-62%. ATTRIBUTION: dominated by the PRE-EXISTING bare-umbrella family
  residual (measured baseline at same poses, NO displacement anywhere:
  13.9-15.0/ch cov ~30%) — arming the gate flips far-band ownership to VK,
  exposing the known VK terrain look-class divergence (sky-band fill + lighting)
  on far surface. NOT a Z-swap defect (see fade-knob proof). Poses verified
  self-matching (no VK-FIRST-BOOKMARK-POSE-1 hit this sweep).
- Smoke: tier1 5/5 PASS both states (gates-OFF and displace+umbrella+ALL_LODS+
  gate-ON), +0 destroy-delta all missions, worktree run_smoke vs 0.4c exe.
- Residuals/deferred: mode-1 LOD0 port (L slice); stitched-patch ownership;
  0.4c lane ships NO visual_height_4x.r32 bakes (only v0.4 does) — live tests
  used MC2_TERRAIN_VISUAL_HEIGHT_FILE override; feed-dump readyToDump gating
  under sweeps; artifacts `.claude/vk-visdisp-far/`.

## Gate inventory (umbrella + sub-gates)

- Umbrella: `MC2_VK_SHARED_GBUFFER=1` arms the family; explicit per-gate env
  overrides (`MC2_VK_TERRAIN_TIER1`, `MC2_VK_TERRAIN_ALL_LODS`,
  `MC2_VK_TERRAIN_SKIRTS`, `MC2_VK_TERRAIN_LIGHTING`, `MC2_VK_TERRAIN_DYNSHADOW`,
  `MC2_VK_TERRAIN_DETAIL`, `MC2_VK_TERRAIN_LODFADE`, `MC2_VK_PROP_TIER1`,
  `MC2_VK_MECH_TIER1`, ...) win when explicitly set 0/1.
- All default-OFF; umbrella-ON verified live this session (see FPS table below).

## Current oracle numbers (this session, mc2_01, ~20s live run, umbrella ON)

Two runs sampled via `tools/perf_snapshot.py`:

| run | FPS(proxy) | terrain total_us | prop total_us | mech total_us |
|---|---|---|---|---|
| 1 | 211.2 | 4734.0 | 1585.1 | 810.5 |
| 2 | 217.8 | 4592.1 | 1596.4 | 806.0 |
| 3 (12s) | 385.1 | 2596.9 | 1658.6 | 863.4 |

FPS is a coarse proxy (`1e6/max(total_us across islands)`), NOT a real frame
counter — the debug-state dump has no frame-count field today. If one gets
added, update `tools/perf_snapshot.py` first.

## Ladder position / next rung

VK-OFFLINE-PROOFS-1 (this session): 5 offline verification tools built +
validated live against the real deployed exe. See `tools/` section below.
Next rung: build a real `MC2_VISUAL_BOOKMARK_CAPTURE` bookmark file so
`tools/vk_feed_diff.py` runs can be TRUE golden-vs-current (today's live
validation showed non-determinism run-to-run without a bookmark: mech gait /
prop streaming / camera drift all still moving — see readyToDump() gate in
`GameOS/gameos/vk_feed_dump.h`).

## VK-lane tool section (agent-efficiency — cross-worktree doc awkward, kept here)

- **`tools/vk_feed_diff.py`** — golden-vs-current diff of `MC2_VK_FEED_DUMP`
  dumps (terrain/prop/mech feeds). `py -3 tools/vk_feed_diff.py golden.jsonl current.jsonl [--island NAME] [--allow-missing]`.
  Validated: identical files → exit 0; synthetic ALL_LODS-on-vs-off diff →
  reports exactly the added coarse patch, exit 1.
- **`GameOS/gameos/vk_feed_dump.h`** — the dump-side harness the 3 feed
  producers (`gos_terrain_lod_chunk.cpp`, `gos_static_prop_batcher.cpp`,
  `gos_mech_batcher.cpp`) call into. Env `MC2_VK_FEED_DUMP=<path>`. Gated by
  `readyToDump()`: fires only when the sim is frozen for a bookmark sweep
  (`mc2CaptureGaitFreezeActive()`) if a bookmark is configured; else fires on
  first successful feed with a one-time stderr determinism warning.
- **`tools/glsl_port_diff.py`** — verbatim-port prover for `//PORT-BEGIN:name`/
  `//PORT-END:name` marker pairs. Registry (`--all`) covers `calc_light`
  (`shaders/include/lighting.hglsl` vs `shaders/vulkan/mech_tier1.vert`),
  `lodNear` (`shaders/gos_terrain.frag` vs `shaders/vulkan/terrain_tier1.frag`),
  `skirtVertMath` (`shaders/terrain_lod_chunk.vert` vs
  `shaders/vulkan/terrain_tier1_skirt.vert`). All 3 REPORT AS DIVERGING after
  whitespace/comment normalization — real renames (identifier/variable-name
  changes only, semantics preserved), not silently fixed. See findings below.
- **`tools/check_vk_shaders.py`** — wraps `glslangValidator -V` (same tool +
  flags CMake's VULKAN-SHADER-TOOLCHAIN-1 target uses) over `shaders/vulkan/*.{vert,frag}`,
  errors-only. `py -3 tools/check_vk_shaders.py`. Validated: 34/34 clean;
  deliberate syntax error in `fullscreen.frag` detected then reverted.
- **`tools/check_ubo_layout.py`** — cross-checks C++ island UBO mirror structs'
  `// off N` comments against independently-recomputed std140 offsets parsed
  from the GLSL uniform block. Registry: terrain (`TerrainFrameUBO`), prop
  (`ColorFrameUBO`). Validated: both OK (36 + 10 cross-checked fields);
  deliberate off-by-931 error in `vulkan_terrain_island.cpp` caught then reverted.
- **`tools/perf_snapshot.py`** — PID-tracked launch + N-second wait +
  debug-state-dump island-timing read + exact-PID kill, one-line FPS+per-island
  report. `py -3 tools/perf_snapshot.py --exe <path> --mission mc2_01 --duration 20 --env MC2_VK_SHARED_GBUFFER=1`.
  Validated live (see FPS table above); never leaves an orphaned process
  (finally-block exact-PID taskkill).

## Verbatim-port findings (tools/glsl_port_diff.py --all)

All three registered ports (`calc_light`, `lodNear`, `skirtVertMath`) are
**functionally-equivalent but NOT textually verbatim** — cosmetic identifier
renames only (e.g. `lights_index`→`idx`, `n_dot_l`→`ndl`, `u_skirtDepth`→
`uT.skirtDepth`). No logic divergence found. Not "fixed" — recorded as findings
per the tool's own contract (see tool docstring).

## Session update convention

Whoever picks up VK work next: update the FPS table + ladder position above
before ending the session. Keep this file under ~60 lines.

## FIELD-CONFIG SESSION 2026-07-06 (commit a50c6033) — full-stack verification

Standing rule: every fix verifies under the FULL launcher field env
(`tracy-vk-compare.bat` + `MC2_STATICPROP_TEX_TIER=512`), not gate-isolated.
Oracle: `.claude/mech-parity/field_oracle.py` (field GL/VK bookmark sweep). NOTE:
capture window MUST be visible (`wShowWindow=4`), NOT minimized(7) — minimized =
zero surface extent = VK present fail-softs to GL and silently invalidates the
run. Islands (terrain/shadow/prop/mech) still render under present-fail-soft, so
composited pixels remain a valid VK oracle.

- **BUG 1 (terrain shadow wash) FIXED (interim)** — MC2_VK_TERRAIN_CSM full-map
  subset is net-negative at play distance (coarse full-map cascade vs GL's near
  cascade = camera-swimming blobs). Field GL-vs-VK MAD 28-31 (wash) -> ~5
  (clean) on airport bookmarks. Double-gated behind
  `MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL=1` (parked escape hatch).
- **VK-TERRAIN-CSM-NEAR-1 (real cure, LANDED, FIELD-VERIFIED 2026-07-06 this
  session, commit `1a4969c8`)** — ports GL's `calcDynamicShadow` near-cascade
  cascade-select VERBATIM into `terrain_tier1.frag`
  (`//PORT-BEGIN/END:calcDynamicShadowCsm`, registered in
  `tools/glsl_port_diff.py`). Imports GL's near-cascade array
  (`dynShadowArrayTex_`, GL_TEXTURE_2D_ARRAY depth) as a VK D32 depth-COMPARE
  `VK_IMAGE_VIEW_TYPE_2D_ARRAY` (`sampler2DArrayShadow`, binding 8) + the
  separate full-map catch-all cascade (binding 9) + all cascade
  matrices/texel-world/csmDepthSpan into `TerrainFrameUBO` (grew 512B->1024B,
  `check_ubo_layout.py` passes, 44 fields cross-checked). Copy mechanism:
  glGetTexImage per frame (no zero-copy producer for the array exists yet —
  the VK shadow island only produces the single legacy map) — documented as a
  known cost, not hidden. Gate `MC2_VK_TERRAIN_CSM` now means THIS
  implementation (requires MC2_VK_TERRAIN_DYNSHADOW chain + GL
  MC2_SHADOW_CSM=1); FULLMAP_EXPERIMENTAL untouched as a fallback escape
  hatch, mutually exclusive C++ population (no collision).
  **FIELD RESULTS** (rebuilt+redeployed 0.4c @ `1a4969c8`, fingerprint
  confirmed): terrain island co-writes confirmed active (LOD0 TIER-1 patches
  drawn, `used_vulkan>0` both near+airport runs). Byte-off control (gates
  off, GL x2) = 0.000 on 10/11 mc2_01 bookmarks (1 negligible max=1px, cov
  0%). VK self-diff determinism at the near building-shadow pose: mean
  0.27–1.76/ch, cov ≤0.79% (clean; one lodfade-pose max=255 spike is the
  known mech-gait jitter, not terrain). Airport bookmarks (`overview_center`,
  `runway_overview`) did NOT reproduce the a50c6033 wash pattern specifically
  — but DID show a severe GL-vs-VK divergence (mean 32–34/ch, cov ~97%) that
  **bisects to a PRE-EXISTING, CSM-independent bug**: reproduced identically
  with `MC2_VK_TERRAIN_CSM` unset and even with only the bare
  `MC2_VK_SHARED_GBUFFER=1` umbrella set (no other sub-gates). This matches
  the already-open **FAR-BAND BLACK** entry below (`oblique_horizon`,
  `overview_center` — both wide/far bookmark poses). VK-TERRAIN-CSM-NEAR-1
  itself is NOT implicated (identical wash with/without the CSM gate). The
  building-shadow near-pose oracle (`near_land_low`) was separately blocked
  by an UNRELATED, reproducible GL-side bug this session:
  `[MECH_LOAD] WARN: mech appearance has no base shape ... mechType=0` fires
  deterministically on GL (both `gl`/`gl2` runs, byte-identical 0.000
  self-diff) at this pose+FORCE_LOD=0 combo — 6 mechs registered
  (`RENDER_WORLD registered=6 alive=6`) but none get a base shape, so GL
  renders NO mechs/shadows at this pose while VK renders all 3 correctly.
  Root cause not investigated further (out of CSM-near scope); flagging for
  a follow-up. The mech-free `near_land_off` control pose (same location,
  no units) gave GL-vs-VK mean_all 9.8/ch cov 20% (masking known sky/EXR-
  missing black band at rows 0-220, a pre-existing artifact present in prior
  sessions' `ridge_lowangle` captures too) — consistent with the arc's
  documented ~5-10/ch near-detail residual band, no new regression signal.
  Smoke: tier1 5/5 PASS both umbrella-OFF and full-field+CSM-ON states, 0
  crash, deploy fingerprint 1a4969c8 confirmed both runs.
  **VERDICT: CSM-near mechanism itself verified clean (byte-off clean, VK
  self-diff clean, terrain island actively co-writing, no CSM-attributable
  wash); the loud field numbers trace to two PRE-EXISTING unrelated bugs
  (FAR-BAND BLACK, GL mech-load-at-this-pose) that predate/are independent of
  this commit. Full artifacts: `.claude/mech-parity/csm_near_field/`.**
- **BUG 4 (mech screen-space freeze on pan) FIXED** — feed now uses live
  gos_GetMechFixBMVPMat4() not the flush-time snapshot. Static-capture cannot
  pan; verified by code-trace + no static regression. USER RE-TEST on live pan.
- **BUG 2 (fence) / BUG 3 (hangar washout)** — hold under field config; prior
  fixes (b911bc0d / 91051001) confirmed present, props render (walls/quonsets
  textured, fence brackets present) in field runway_overview + mc2_24. Layer
  index is bucket-relative/order-derived => tier-INVARIANT (deploy has only
  128/ tier dir; missing 512/ => whole-group fallback, GL==VK). No tier-driven
  layer shift.

### OPEN (documented, NOT fixed this session — for next session)
- **BUG 5 (2x sim speed, field env)** — NOT a fixed-step misfire:
  `MC2_SMOKE_FIXED_TIMESTEP` is the ONLY armer (gos_smoke.cpp:54); the field bat
  does NOT set it, and `MC2_DEBUG_STATE_DUMP` touches no timestep path. Sim dt =
  `frameLength = 1.0/frameRate` (mechcmd2.cpp:2217); frameRate = live wall-clock
  `1000/dt` (gameosmain.cpp:1957). Needs an instrumented sim-time-vs-wall run
  (field vs minimal env) to root-cause the 2x. SIM-SPEED FEATURE SEAM (user
  request, do NOT implement yet): the clean knob is `frameLength`
  (mclib/timing.cpp:20, set mechcmd2.cpp:2217) — a future MC2_SIM_SPEED/UI
  button scales it.
- **FAR-BAND BLACK — PARTIAL FIX this session (`gos_terrain_lod_chunk.cpp` gate
  bug fixed; deeper CSM-near shading defect still OPEN).** Root-caused via
  bisection (MC2_VK_TERRAIN_LIGHTING/DETAIL/DYNSHADOW/CSM toggled one at a time
  on overview_center): the VK-TERRAIN-CSM-NEAR-1 feed-population block
  (`gos_terrain_lod_chunk.cpp` ~line 964, `out->csmEnabled=1` path) was gated on
  `dynShadowGate` (the DYNSHADOW sub-gate) ONLY — despite its own comment
  claiming "MC2_VK_TERRAIN_CSM alone... now means THIS implementation", the code
  never actually checked `mc2vkgate::subGate("MC2_VK_TERRAIN_CSM")`. Since GL
  CSM defaults ON (`MC2_SHADOW_CSM` default-ON) and the umbrella defaults
  DYNSHADOW ON too, this fed real CSM cascade data to the VK terrain frag any
  time DYNSHADOW was armed, REGARDLESS of the CSM sub-gate's own value — a
  real, provable gating bug, now FIXED (added the missing `csmNearGate` check).
  Verified: bare-umbrella-minus-explicit-CSM-off now renders terrain correctly
  (`.claude/mech-parity/csm_near_field/farband_realfix/vk_umbrella_csmoff/`).
  **HOWEVER** the FIELD launcher recipe (and bare umbrella with no overrides)
  legitimately ARMS both DYNSHADOW and CSM together (by design), and the
  overview_center/oblique_horizon black-out STILL reproduces in that
  configuration post-fix (`vk_final/mc2_01_overview_center.png`) — so a SECOND,
  deeper defect lives inside the CSM-near shading path itself
  (`mv_calcDynamicShadowCsm`/`mv_csmProject`/`mv_csmSampleLayer` in
  `shaders/vulkan/terrain_tier1.frag`, or the C++ feed populated at
  `gos_terrain_lod_chunk.cpp` lines 964-982) when the near-cascade path is
  legitimately active — most likely `normalLight`/`uT.terrainLightDir`
  collapsing near-zero (line 708 `lit = base * normalLight * shadow` would
  blacken the WHOLE fragment, not just the shadow term, matching the
  near-total-blackout symptom rather than a milder shadow-only darkening).
  NOT YET ISOLATED further this session (ruled out: depth format
  D32_SFLOAT_S8_UINT vs GL_DEPTH24_STENCIL8 reinterpretation — tried both a
  D24_UNORM_S8_UINT export swap [driver rejects it, falls back to D32S8, no
  change] and a native-VK-clear fix on the shared depth attachment [no change]
  — depth is NOT the mechanism; isolating LIGHTING/DETAIL/DYNSHADOW-without-CSM
  all render terrain correctly, only CSM-near-armed reproduces the blackout).
  Overview MAD: unable to compute a numeric MAD this session (no golden/
  byte-identical GL reference existed for this exact bookmark+camera-jump
  timing; visual A/B via screenshots only). NEXT SESSION: instrument
  `computeNdotLBand`/`uT.terrainLightDir` and the CSM UBO fields with a
  one-shot stderr dump inside `terrain_tier1.frag` (or add a
  MC2_VK_TERRAIN_CSM_DEBUG viz mode) to see whether lightDir/normalLight or the
  CSM sample itself is the zero. Artifacts:
  `.claude/mech-parity/csm_near_field/farband_realfix/`,
  `.claude/mech-parity/csm_near_field/oobfog_isolate/` (bisection captures).

- **CSM-BLACKOUT-RECON-1 (this session) — BLOCKED on a broken deploy lane,
  root cause NOT isolated live.** Coordinator's UBO-write-collision hypothesis
  (terrain UBO grew 512->1024B, maybe two feed paths stomp each other) was
  checked FIRST as instructed and RULED OUT by static read: the per-patch UBO
  populate loop (`vulkan_terrain_island.cpp` ~2193-2264) zero-inits a fresh
  `TerrainFrameUBO u{}` every patch and does a full `slots[i]=u` struct
  assignment (no partial memcpy/stale-size aliasing); `sizeof(TerrainFrameUBO)`
  is used consistently for the descriptor range, `ensure_ubo_capacity`, and the
  dynamic-offset stride — no collision found. `check_ubo_layout.py` still
  passes (44 fields). Built the planned one-shot debug-viz instrumentation
  instead: a new UBO field `csmDebugMode` (int, repurposed the `_csmPad2` slot
  at UBO off 364, fed from env `MC2_VK_TERRAIN_CSM_DEBUG=1..5`) that early-outs
  `terrain_tier1.frag`'s lit compose to show `normalLight`(1) / `lightDir`(2) /
  `staticS`(3) / `dynS`(4) / `shadow`(5) in isolation
  (`GameOS/gameos/vulkan_terrain_island.cpp` UBO struct + populate site,
  `shaders/vulkan/terrain_tier1.frag` struct mirror + branch after the
  `lit = base*normalLight*shadow` line). Builds clean, `check_vk_shaders.py`
  34/34, `check_ubo_layout.py` OK, full mc2 build succeeds, deployed to 0.4c.
  **BLOCKED**: every VK launch this session (both the new debug probe and a
  vanilla re-run of the existing `field_oracle.py`, 3 attempts) hit
  `[VK_PRESENT] init: vkCreateSwapchainKHR failed. fail-soft.` ->
  `[RENDER_BACKEND] fail-soft: forcing GL for the session` — the whole run
  silently became a GL capture, invalidating all screenshots taken (the
  earlier "reproduction" in `.claude/mech-parity/csm_near_field/blackout_debug/`
  is GL, not VK; disregard those PNGs). `mcp__mc2-repo-intel__deploy_status`
  on `A:/Games/mc2-opengl/mc2-win64-0.4c` independently confirms
  `verdict=BROKEN-TREE`: "0.4c is a BROKEN tree (stock + modified exe both
  crash_silent / PAUSE-loop, 0 frames). Smoking it yields FALSE crash
  reports." — this is a pre-existing lane-health problem, not caused by this
  session's edits (confirmed by retrying the untouched `field_oracle.py`
  recipe with identical failure). Exe fingerprint DID match HEAD (7f396c2e)
  each time — the deploy itself was clean; the swapchain failure is
  environmental/lane-state, not a stale-exe issue. **NEXT SESSION**: get (or
  wait for) a healthy 0.4c (or a fresh, uncontended deploy dir) before
  re-running this probe; the instrumentation above is ready to use
  immediately — just set `MC2_VK_TERRAIN_CSM_DEBUG=1` (normalLight) first
  against a working VK backend at `overview_center`/`oblique_horizon` and
  read the pixel values (should be mid-gray `mix(0.35,1.20,NdotL)`, achromatic
  — any color tint or near-0 reading means the debug branch itself isn't
  executing on VK, which would point back at UBO wiring after all). Debug
  field is fully gated (env-off = 0 = zero behavior/perf change) — safe to
  leave in tree. Shadow-saga status: UNCHANGED from before this session — the
  deeper CSM-near-armed blackout defect is still open, now with ready-to-use
  instrumentation blocked only by lane health.
- **CSM-BLACKOUT-RECON-1 RESOLVED (2026-07-07 session) — blackout does NOT
  reproduce at HEAD `cf91cba5`; the "deep CSM-near shading defect" was a
  compound evidence artifact, not a shader bug.** Lane health CLEARED first:
  rebuilt+redeployed 0.4c at `cf91cba5` (manifest src_commit confirmed), plain
  VK launch on a visible window (wShowWindow=4) → `[VK_PRESENT] swapchain OK
  images=3 extent=1707x960` + present path UP, no fail-soft (last session's
  vkCreateSwapchainKHR failure was environmental, as suspected). Full
  MC2_VK_TERRAIN_CSM_DEBUG=0..5 ladder run at all mc2_01 bookmarks, every run
  with a used_vulkan receipt (488–496; GL control used_vulkan=0):
  `.claude/mech-parity/csm_near_field/blackout_debug/vk_dbg{0..5}/` + `gl_ref/`.
  FINDINGS: (1) the debug branch executes correctly on VK (achromatic grays →
  UBO wiring at off 364 GOOD); (2) NO term collapses — at the oblique_horizon
  "black band": normalLight=0.70, lightDir sane, staticS=0.67, dynS=1.00
  (CSM sample fully LIT), shadow=0.67; (3) the oblique_horizon dark band is
  PRESENT IN GL TOO (GL band mean 23/ch vs VK 17/ch — it is the stock look:
  dark slope+baked-shadow band), terrain-only MAD GL-vs-VK = 3.79/ch at
  oblique_horizon, 4.5–17/ch across all matched bookmarks — inside the
  documented near-detail residual, NO blackout-class divergence; (4) the
  "overview_center blackout" screenshots were captured at the WRONG CAMERA
  POSE under VK configs — cross-matching VK captures against all GL bookmarks
  shows VK's first bookmark (overview_center) lands on a water/fog-edge pose
  (best-match = edge_fog_corner_southwest) while every subsequent bookmark
  matches its own name; reproducible with settle=40 AND settle=80, VK-config
  runs only, deterministic. The prior session's post-fix "blackout still
  reproduces" capture (`vk_final/mc2_01_overview_center.png`) is this same
  first-bookmark wrong-pose artifact (dark fog-edge water view misread as
  terrain blackout). VERDICT: CSM-near path healthy at HEAD; no shader/feed
  fix needed, nothing landed beyond probe-script receipts. NEW OPEN ITEM →
  **VK-FIRST-BOOKMARK-POSE-1**: bookmark-sweep capture under VK gates captures
  bookmark #1 at a stale/wrong camera pose (GL runs are correct). Any
  single-bookmark VK evidence at "overview_center" from sweep captures is
  suspect until this is fixed; use later-in-sweep bookmarks or
  single-bookmark runs for VK evidence.
- **BUG 6? prop texture scramble** — coordinator saw live (pink slabs, floating
  textures, trunkless trees) DURING mc2_24 load on the Bug1+Bug4 exe. NOT
  reproduced at settled capture (props clean: walls/trees/buildings textured).
  Classified LOAD-TRANSIENT, not a static-reproducible regression; neither
  committed fix touches prop layers. USER cold-load re-check to confirm.

## ADJUDICATION-1 (2026-07-06) — contamination-proof fence + washout verdict

Method: deterministic bookmark-sweep capture (sim-frozen, fixed seed/timestep,
SW_SHOWNOACTIVATE=4 visible window) at an authored **airport bookmark**
(`c_rot110`: pos 3200,100,~400 / rot 110 / proj 28 / alt 1000 — frames hangar +
perimeter fence + runway, the user's scene class). GL ('off' set) vs VK
(canonical 'on' set = `MC2_VK_SHARED_GBUFFER=1 MC2_VK_SHADOW_INTEROP=1` +
`MC2_SHADOW_CSM/STATIC_PROP_PBR_V1/AMBIENT_V1/IBL_SH=1`). Receipts printed per
capture: **GL** vk_color_prop used_vulkan=0 (correct GL label); **VK**
used_vulkan=400–411, vk_draws=1, interop_active=1, vk2gl_sem_active=1,
inst_records=1006, type_runs=78, geom_verts=23628 (correct VK label — pixels
ARE VK, no fail-soft). Artifacts: `.claude/adjudication/{gl,vk_probe}/`,
`crop_{hangar,fence}_{gl,vk}.png`, `rdc/` (.rdc + XML), `vk_probe/packets.csv`.

**RenderDoc finding (contamination class 3, DIRECTLY OBSERVED):** a
`renderdoccmd capture -w` of the canonical VK-on config produced a capture whose
header is `<driver>OpenGL</driver>` with ZERO vk* chunks. The VK prop island is
HEADLESS (offscreen + vk2gl semaphore, no swapchain/present), so a present-based
GL RenderDoc hook cannot see its `vkCmdDrawIndexed`s. **Lesson: RenderDoc-under-GL
of a shared-gbuffer interop frame CANNOT prove the VK island — its draws are on
the VK queue with no present. Draw-level VK facts must come from an in-record
dump (added: `MC2_VK_PROP_PACKET_DUMP`) or a StartFrameCapture/EndFrameCapture
device-scoped bracket, NOT TriggerCapture.**

**FENCE — VERDICT: STILL BROKEN in canonical config (alternating panels).**
Own GL/VK pair at identical pose: GL = continuous chain-link; VK = alternating
panels (every other panel's mesh missing). Draw-level (`packets.csv`, 80 packets):
EVERY packet draws non-degenerate with exactly one valid push-constant layer
(`layerOff`/`layerOn`), zero "both -1", zero zero-count draws. **So the
alternation is NOT dropped draws / -1 layers — the per-packet layer fix
(b911bc0d / VK-FLIP-FIELD-REPORT-2) IS correct at the data level.** Root cause is
the alpha-ON group's **0.5 alpha-test discard** (`color_prop_tier1.frag:69
if (texc.a < 0.5) discard`) against the VK copy-once albedo ARRAY's mip/filter:
the minified chain-link alpha aliases below 0.5 on alternate panels at this
distance, where GL's LOD-chunk path keeps it. **INVALIDATES the prior "black-fence
FIXED" claim's completeness** — the black→visible fix (`max(vArgb,0.5)`) and the
per-packet-layer fix both landed, but neither addresses alpha-test panel dropout,
which is what the user actually sees.

**WASHOUT — VERDICT: STILL BROKEN, and it is NOT a fail-soft.** Hangar-crop mean
brightness (0..255): GL 123.8 vs VK 171.1 = **+47.3** (MAD 66.3); scene-wide VK
+24.5. Engagement probe (`MC2_VK_PROP_LIGHT_PROBE`): `lightingReady=1
haveLightFeed=1 iblShStrength=0.5 iblSh0=(1.71,1.77,1.65) lightDataBytes=3610800`
— **the real IBL/ambient feed DID engage; the hemisphere fail-soft is NOT taken.**
Kill-switch matrix (hangar bright): GL-on 123.8 / VK-on 171.1 / GL-off 121.2 /
VK-off 146.9 → GL's IBL adds +2.7, VK's IBL adds +24.2 (≈9× too strong), AND VK
is +25.7 over GL even with IBL OFF. Two stacked over-brightening bugs in the
VK prop lighting math (`color_prop_tier1.vert`:
`vArgb = seed + ambientTerm + iblTerm + kSunColor*ndl`): the real-feed branch
(`lightingReady!=0`) sums raw ObjectLights ambient + `evalShL2*0.5` +
hardcoded `kSunColor*ndl` with no exposure/weight matching GL. **INVALIDATES the
prior "washout FIXED (feed parity 91051001)" claim:** the prior washout tuning
targeted the FALLBACK hemisphere branch (comment in .vert), but canonical config
hits the REAL-FEED branch, which was never washout-tuned. "Verified fixed but
user still sees washout" = tuned the wrong branch.

**Historical claims invalidated:** (1) "fence fixed" — only black→visible +
layer-drop fixed; alpha-test panel dropout remains, user-visible. (2) "washout
fixed / IBL feed parity" — parity was measured on the fallback branch; the
engaged real-feed branch over-brightens hangar by +47. Both bugs reproduce in
the EXACT canonical config with VK-labeled receipts, so they are real, not
oracle artifacts.

**Fixes:**
- **Washout — PARTIAL fix landed, NOT cured.** `color_prop_tier1.vert` real-feed
  branch now scans ObjectLights for the real per-actor sun color (GL parity,
  static_prop.vert:414-438) instead of the hardcoded `kSunColor` stand-in.
  Re-captured (VK used_vulkan=363): hangar +47.3 → **+43.5** vs GL. Only a 3.8/255
  win — proves the phantom sun was NOT the dominant term (the real sun ≈ as bright
  as the constant). **Dominant washout cause still open:** VK/GL apply the same
  `albedo * vArgb` and the same summed terms, yet VK vArgb is +43 hotter. Leading
  hypothesis: GL attenuates prop ambient/sun by CSM shadow (`MC2_SHADOW_CSM=1` ON),
  the VK prop island does NOT shadow-attenuate its vertex-baked lighting → props
  render unshadowed/brighter. Next: instrument whether GL's v_argb includes a
  shadow factor the VK island drops (check static_prop.vert for calcShadow on the
  ambient/sun term vs the VK island's shadow interop wiring).
- **Fence — NOT fixed (root cause pinned).** Draw-level proven it's alpha-test
  panel dropout (`color_prop_tier1.frag:69 if (texc.a<0.5) discard`) against the VK
  copy-once array's mip chain, not a layer/draw drop. Fix path: alpha-to-coverage,
  preserve alpha in the array mips, or match GL's effective coverage — NOT landed.

Instrumentation left in tree, all env-gated default-off: `MC2_VK_PROP_PACKET_DUMP`
(per-packet draw params CSV), `MC2_VK_PROP_LIGHT_PROBE` (UBO lighting engagement),
`[ADJUDICATION_POSE v1]` one-shot pose dump in gos_visual_capture.cpp. Airport
bookmark authored at `.claude/adjudication/airport_bookmark.json` (c_rot110 frames
both bugs). Artifacts under `.claude/adjudication/`.

## ADJUDICATION-1 FOLLOW-UP (2026-07-06, this session)

**WASHOUT — ROOT-CAUSED AND FIXED.** Per-vertex CPU-side probe
(`MC2_VK_PROP_LIGHT_VERTEX_PROBE`, new, gated) dumped the REAL ObjectLights entries
the hangar-area instances (typeID=38) actually use: `numLights=1, ambient=(0.3,0.3,0.3),
sunFound=0` for every sampled instance — confirms no per-actor sun light feeds these
props at all (matches the prior sun-fix's own +3.8/255 result: removing the phantom
`kSunColor` barely moved the needle because GL's own scan also finds no sun here).
`iblSh0` DC coefficient is 1.71 — `evalShL2`'s `kSH_c4*sh[0]` term alone
(`0.886227*1.71≈1.51`) times `iblShStrength=0.5` is already ~0.76, and adding the 0.3
ambient pushes the raw sum past 1.0 before any clamp. **Root cause: VK's
`color_prop_tier1.vert` never clamped `vArgb`, unlike GL's `static_prop.vert:524
lit = clamp(lit + aRGBHighlight, 0.0, 1.0)`.** An unclamped >1.0 vArgb multiplies the
albedo brighter than GL's white-capped equivalent on every bright material (hangar
roofs). Fix: added the matching `clamp(..., 0.0, 1.0)` to `vArgb`'s final assignment
(`shaders/vulkan/color_prop_tier1.vert`). **Measured (canonical config, c_rot110,
template-matched hangar crop, same box both before/after within this session):
hangar delta +43.5 -> -0.6/255 (essentially matched); MAD 34.8 is structural/edge
noise, not a brightness bias.** Receipts: GL used_vulkan=0 (correct), VK
used_vulkan=420/421 frames (correct, real VK pixels, not fail-soft). Smoke: tier1 5/5
PASS both byte-off control and full canonical VK-on config, 0 crash, +0 destroy-delta.
Commit: (this session, see git log).

**FENCE — STILL OPEN, mip/aniso hypotheses RULED OUT with live evidence.** New
`MC2_VK_PROP_MIP_PROBE` diagnostic (gated, left in tree) confirms mip-count parity:
alpha-ON array `glTexMaxLevel=7` (GL) == `vkComputedMips=8` levels / `maxLod=7.0`
(VK) -- GL's own `GL_TEXTURE_MAX_LEVEL` and the VK import's computed chain length
match exactly, both texture groups (alpha-OFF 21 layers, alpha-ON 34 layers). Grep
of the entire renderer for `ANISOTROP*` returns zero hits on both the GL prop path
(`gos_static_prop_batcher.cpp`) and the VK sampler (`vulkan_color_prop_island.cpp`)
-- no anisotropic filtering anywhere, so aniso mismatch is not the cause either.
VK's mip chain is confirmed BYTE-COPIED from GL's own already-generated
`glGenerateMipmap` output (`glGetCompressedTexImage` per level, not re-derived) --
so VK samples the literal same BC7 bytes GL does, at the same LOD range, with the
same CLAMP_TO_EDGE + LINEAR_MIPMAP_LINEAR sampler state, same `fract(vUv)` UV
handling in both fragment shaders. With every concrete sampler/mip/UV divergence
eliminated, a bisect experiment settled it (same session, later):

**FENCE ALTERNATION — ROOT-CAUSED AND FIXED (same session, follow-up).**
Bisect ladder, each step build+deploy+capture with VK receipts (used_vulkan
409-420, GL reference unchanged):
1. Coordinator's fract-seam-derivative hypothesis (`textureGrad` with smooth
   gradients from the UNWRAPPED vUv): landed, deployed SPV verified to contain
   `OpImageSampleExplicitLod ... Grad` -- alternation UNCHANGED. Hypothesis
   DISPROVEN by direct experiment.
2. Forced `textureLod(.., 0.0)` diag: EVERY dropped panel reappeared.
   **Mechanism proven: the minified mip chain's alpha decays below the 0.5
   test (naive glGenerateMipmap box filter halves thin chain-link coverage
   per level), and VK's implicit LOD lands deep enough to cross it where GL's
   does not.** (Mip BYTES are identical -- byte-copied -- so the divergence is
   LOD selection, which is implementation-defined per API/compiler.)
3. Final fix (landed, `shaders/vulkan/color_prop_tier1.frag` alpha-ON branch):
   deterministic LOD computed from unwrapped-vUv gradients
   (`0.5*log2(max(dot(dx,dx),dot(dy,dy)))` in texel units), clamped to <=1.0
   for the alpha-test class (LOD<=1 proven panel-preserving by the bisect),
   sampled via explicit `textureLod`. No driver-implicit LOD dependence left.
   **Result: alternation GONE -- every panel present along the whole run at
   c_rot110 (crop_fence_vk_final.png). Fence-region MAD 29.9 -> 27.1.**
   Residual (documented, NOT the alpha bug): VK panels read slightly narrower
   with gaps vs GL's continuous ribbon -- consistent with the KNOWN-DEFERRED
   per-draw uvScale sub-region gap (VK samples with uvScale=1.0 assumed;
   GL applies uvScaleX/Y per draw, static_prop.frag:206). NEXT CONCRETE STEP:
   feed per-draw uvScaleX/Y through the packet push constant (same plumbing as
   packetLayerOff/On) and apply in the island frag; expect the gaps to close
   and fence MAD to drop to residual-class.
Hangar regression guard re-measured on the same capture: delta -0.5/255 (fix
intact). Smoke mc2_01 PASS both byte-off control and canonical VK-on state.

## FENCE-UVSCALE-1 (2026-07-06, this session) — CLOSES ADJUDICATION-1

**Fix landed.** GL uv-crop site confirmed: `shaders/static_prop.frag:206`
`vec2 uvSampled = fract(v_uv) * vec2(uvScaleX, uvScaleY)`, fed from
`PerDrawEntry.uvScaleX/Y` (`GameOS/gameos/gos_static_prop_batcher.h`), which are
computed per-PACKET in `finalizeGeometry()`
(`gos_static_prop_batcher.cpp:3844` `uvScaleXByPacket[pk] = u.w * invMaxW`,
same "packetsInGroup" loop that fills `layerForPacket`).

**Plumbing (mirrors the b911bc0d packetLayerOff/On pattern exactly):**
new per-packet sidecars `s_packetUvScaleX/Y` persisted alongside
`s_packetTexArrayLayer` in `finalizeGeometry()`, cleared in `onMapUnload()`,
exposed on `Mc2ColorPropMaterialFeed` (`packetUvScaleX/Y`,
`vulkan_shadow_island_live.h`) alongside `packetAlbedoLayer`. VK island
(`vulkan_color_prop_island.cpp`) copies them into `packetUvScaleX/Y` vectors
(default 1.0 = no crop, matching pre-fix behavior when the feed doesn't cover a
packet) at the same site that fills `packetLayerOff/On`. Push constant widened
8->16 bytes (`int layerOff, int layerOn, float uvScaleX, float uvScaleY`) --
well inside the Vulkan-guaranteed minimum `maxPushConstantsSize` of 128 bytes,
so no half-float packing or per-instance-SSBO detour was needed (justified:
8 extra bytes is noise against the 128B floor, and per-packet push constants
were already the established pattern for this exact kind of per-draw data).

**Shader (`shaders/vulkan/color_prop_tier1.vert/frag`):** `vUvScale` forwarded
flat VS->FS. Frag applies `fract(vUv) * vUvScale` verbatim in the alpha-OFF
branch (was `fract(vUv)` only). Alpha-ON branch (the fence's alpha-test class)
applies the same crop BEFORE computing the deterministic-LOD gradients from
this session's earlier alternation fix (`uvSampledOn = fract(vUv) * vUvScale`,
gradients taken from `uvSampledOn` not raw `vUv`) so the sub-region's actual
texel footprint drives LOD selection, not the full-layer one -- otherwise a
narrow sub-region would under-estimate LOD and reintroduce alpha-test aliasing
on exactly the packets this fix targets.

**Verified engaged:** new `MC2_VK_PROP_PACKET_DUMP` CSV (columns extended with
uvScaleX/Y) at c_rot110: 14/80 packets carry uvScale=0.5 (fence chain-link
post/arch sub-mesh packets, typeIDs 70/72/73/124/127/130/134/153/161/175/205/
210/213/216), confirming the plumbing reaches the exact packets the state file
predicted, not a no-op.

**Visual result:** VK fence ribbon is now continuous and full-width, matching
GL's panel coverage/spacing at c_rot110 (`crop_fence_gl_final2.png` vs
`crop_fence_vk_final2.png`, `.claude/adjudication/`) -- no narrowing, no gaps.

**MAD arithmetic note (important, avoids re-litigating a false regression):**
raw unaligned fence-crop MAD went 27.1 -> 28.6, which LOOKS like a regression
but isn't -- diagnosed as separate-process camera-pose jitter (~11px vertical
drift between GL/VK launches), present IDENTICALLY in the prior 27.1 baseline
capture (re-measured: prior pair's own best-match shift is dx=-12,dy=11,
aligned MAD 28.46, not 27.1 -- the originally-reported 27.1 was itself an
unaligned/lucky-crop number). Aligned (shift-corrected) apples-to-apples MAD:
prior 28.46 -> this session 27.54, a real but modest improvement, consistent
with only 14/80 packets carrying non-1.0 scale. **Lesson for future
bookmark-sweep MAD comparisons: always cross-correlate for a translation
offset before trusting a raw crop MAD delta as small as 1-2/255 -- separate-
process camera jitter alone produces deltas that size.**

**Regression checks (same c_rot110 capture pair):**
- Hangar brightness delta: GL 123.2 vs VK 120.4 = **-2.8/255** (within the
  existing -0.5..-0.6 residual band from the washout fix; no new regression).
- Fence visual: continuous ribbon, panel-preserving (screenshot pair above).
- Packet dump: zero degenerate draws, zero "both -1" layers (same invariant as
  the alternation-fix session).

**Smoke:** `MC2_DEBUG_STATE_DUMP=1` + tier1 (`mc2_01 mc2_03 mc2_10 mc2_17
mc2_24`, 30s each) against `A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe`
(sha `6366c922de88`, fingerprint-matched worktree HEAD): **byte-off control
5/5 PASS, +0 destroy-delta all missions; canonical VK-on config
(`MC2_VK_SHARED_GBUFFER=1 MC2_VK_SHADOW_INTEROP=1` +
`MC2_SHADOW_CSM/STATIC_PROP_PBR_V1/AMBIENT_V1/IBL_SH=1`) 5/5 PASS, +0
destroy-delta all missions.**

### ADJUDICATION-1 FINAL SCOREBOARD

| Bug | Adjudication-1 verdict | Follow-up verdict | This session (FENCE-UVSCALE-1) |
|---|---|---|---|
| Fence alternating panels | STILL BROKEN (alpha-test dropout) | ROOT-CAUSED + FIXED (deterministic clamped LOD) | n/a (already closed) |
| Fence narrow/gapped panels vs GL ribbon | not yet isolated | isolated as uvScale=1.0 assumption, deferred | **FIXED** (per-packet uvScale plumbed + applied; visually continuous/full-width, aligned MAD 28.46->27.54) |
| Hangar washout | STILL BROKEN, not fail-soft (+47.3) | ROOT-CAUSED + FIXED (unclamped vArgb, -0.6/255) | held (-2.8/255 this capture, same residual band) |

**ADJUDICATION-1 is CLOSED as of this session.** Both bugs it raised (fence,
washout) are root-caused and fixed; the one open residual the follow-up
session left on the table (fence uvScale sub-region gap) is now also fixed and
verified. No further action items outstanding from this adjudication.

## RUNG 8.4 - VK-POST-HZB-NATIVE-1 SHIPPED

Native HZB now builds the postprocess depth pyramid on the shared engine
VulkanContext under `MC2_VK_POST_HZB=1` when `MC2_HZB_BUILD=1`. The path samples
the shared scene depth directly and writes VK-owned R32F ceil-ladder pyramid
levels; the old GL `runHzbReduce()` body remains the fail-soft fallback/oracle.

Critical sync fix: the HZB pass must not advance only the shared depth resource
parity. Doing so desynchronizes the shared GBuffer triplet from terrain/prop
writers, whose submit retirement model is keyed off the color fence. The shipped
implementation acquires/releases color0, normal, and depth together, but records
barriers and samples only depth. Validation also required depth barriers to use
`VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT` for the shared
`D32_SFLOAT_S8_UINT` image.

Receipts:
- Focused native HZB smoke `mc2_01`: PASS, 168 frames, +0 destroys.
- Focused validation smoke `mc2_01`: PASS, 79 frames, +0 destroys; no HZB VUIDs.
- HZB health: attempted=79, used_vulkan=79, levels=11, vk_draws=11,
  copy_depth_ops=0, readback_ops=0, fallback_reason="".
- Probe path `MC2_HZB_PROBE=1`: PASS; native declined with
  fallback_reason="probe_enabled", attempted=0, used_vulkan=0.
- Tier1 native HZB (`mc2_01/mc2_03/mc2_10/mc2_17/mc2_24`, 20s): PASS 5/5,
  +0 destroys.

## RUNG 8.5a - VK-PRESENT-DIRECT-FBO-1 META PATH CONFIRMED

No code change in this checkpoint. The existing `MC2_VK_PRESENT_DIRECT_FBO`
implementation is the correct first 8.5 slice: GL composite, FORCE-43,
HUD, and ImGui stay unchanged but render into EnginePresentVk's shared present
FBO, letting `presentGLFrame()` skip copy-1. `CompositePresent` is not the path;
that region is explicitly a separate-window test contract.

Evidence gathered on 2026-07-07:
- Refreshed explicit deploy lane
  `A:/Games/mc2-opengl/mc2-win64-0.4c` from `build64_island/RelWithDebInfo`,
  manifest `src_commit=b52050ccdf71`.
- Exact-lane baseline smoke:
  `py -3 scripts/run_smoke.py --exe A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe --mission mc2_01 --mission mc2_24 --duration 30 --keep-logs`
  -> `2026-07-07T22-55-54`, PASS 2/2, fingerprint OK, +0 destroys.
- Exact-lane gate-on smoke:
  `MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1` with the same exe and
  missions -> `2026-07-07T22-57-19`, PASS 2/2, fingerprint OK, +0 destroys.
- Vulkan-backend minimized smoke:
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1`
  -> `2026-07-07T22-59-12`, PASS 2/2, engine context created on RX 7900 XTX,
  but present receipt is blind because the smoke harness minimizes the window:
  `fallback_reason="zero surface extent (window minimized?)"`,
  `present_frames=0`.

Follow-up evidence gathered on 2026-07-08:
- `cf02f587` added `scripts/run_visual_capture.py --show-window` and absolute
  bookmark-path handoff. Default remains minimized for ordinary visual baselines.
- Rebuilt + deployed explicit 0.4c lane from HEAD `45afd60d7416`
  (`build_fingerprint.h: dirty=0`, deploy manifest `src_commit=45afd60d7416`).
- Visible exact-lane run:
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1`
  with `run_visual_capture.py --show-window --bookmarks tests/visual/bookmarks/mc2_01.json`
  -> 11/11 PNGs present, deterministic sidecars, present receipt
  `present_frames=11101`, `validation_errors=0`, `readback_changed=1`,
  `fallback_reason=""`.
- `8fd4bea5` added `scripts/renderdoc_capture.py --exe` so Tier-5 captures can
  target the exact 0.4c lane instead of the hardcoded v0.4 deploy. RenderDoc 1.7
  proof under the same direct-FBO Vulkan env produced
  `.claude/vk-present-direct-visible/rdoc/direct_fbo_frame181.rdc`; engine hook
  logged `api_resolved`, `trigger_capture local_frame=180`, and
  `capture_complete local_frame=182`.

Verdict: 8.5a is now fully proven, not blind: copy-1 is skipped on a live Vulkan
present path with nonzero present frames and a RenderDoc capture.

8.5b sync ruling: do NOT naively render native composite directly into
EnginePresentVk's shared present image from `gosPostProcess::endScene()`.
`presentGLFrame()` already owns `vkImageResourceBeginFrame(&sharedImg)` and the
single GL->VK handoff for the final present. A native composite inserted before
GL HUD/ImGui would need VK->GL for HUD, then a second GL->VK for present; the
current `VkImageResource` semaphore/parity pair is not a safe two-handoff
mid-frame protocol. Next implementation must either design an explicit
present-image split with separate sync for composite->HUD and HUD->present, or
land a safer first runtime slice that targets shared scene color and leaves
present ownership unchanged.

## RUNG 8.5b-a - VK-POST-COMPOSITE shader asset prep

Asset-only prep for the native composite port. Added `shaders/vulkan/postprocess.vert`
and `shaders/vulkan/postprocess.frag` plus the CMake shader basename registration,
so the Vulkan build now bakes `postprocess.vert.spv` and `postprocess.frag.spv`.
The fragment shader ports the current GL composite semantics (scene sample,
exposure, FXAA gate/tunables, warm grade, vignette, ObjectIdDebug, Thermal, and
LowLight) into an explicit Vulkan descriptor layout:
`sceneTex` at set 0 binding 0, optional `u_objectIdTex` at binding 1, and one
std140 params block at binding 2.

Verification:
- `cmake --build build64_island --target mc2_vulkan_shaders --config RelWithDebInfo`
  PASS; emitted `build64_island/shaders/vulkan/postprocess.vert.spv` and
  `postprocess.frag.spv`.
- `spirv-val` PASS for both SPIR-V files.
- Full `mc2` RelWithDebInfo build PASS (only pre-existing C4267/LNK4199 warnings).

No runtime hook yet. Next slice should add `vulkan_post_composite_native.cpp`,
`gosPostProcess::runVkPostCompositeNative()`, debug-state health, `MC2_VK_POST_COMPOSITE`
allowlist/docs, and a visible-window proof path for present frames. Scout finding:
`run_smoke.py` still lacks allowlist entries for `MC2_VIEWMODE_FRAMEWORK`,
`MC2_VIEW_MODE`, and `MC2_OBJECT_ID_BUFFER`, so viewmode parity coverage will need
that harness fix when runtime testing ObjectIdDebug/Thermal/LowLight.

## RUNG 8.6a - VK-IMGUI-BACKEND-COMPILE-1 SHIPPED

Behavior-neutral first 8.6 slice. The Dear ImGui Vulkan renderer backend now
compiles into the existing `imgui` static library when `MC2_VULKAN=ON`, using the
project's existing Volk/Vulkan-Headers pattern (`IMGUI_IMPL_VULKAN_USE_VOLK` and
`Vulkan::Headers`). `GuiRuntime` remains OpenGL-backed; no runtime selection,
HUD path, or present ownership changed in this checkpoint.

Important vendoring finding: the tracked Vulkan backend had drifted newer than
the vendored core `imgui.h` (`IMGUI_VERSION_NUM 19180`) and referenced post-1.91.8
texture APIs (`ImTextureData`, `ImGuiBackendFlags_RendererHasTextures`,
`ImGuiPlatformIO::Textures`, renderer callback handlers). The fix was to align
only `imgui_impl_vulkan.cpp/.h` to the official `v1.91.8-docking` backend, matching
the core/vendor commit lineage instead of patching Dear ImGui internals.

8.6 runtime scout ruling: do not draw native HUD/ImGui into the shared present
image yet. The same mid-frame ownership issue documented in 8.5b applies:
postprocess/composite happens before GL HUD/ImGui, while `presentGLFrame()` owns
the single final GL->VK handoff. A safe runtime 8.6 slice needs either a proper
present-image split / second sync handoff or an ordering change where Vulkan owns
the full tail after composite.

Verification:
- `cmake --build build64_island --config RelWithDebInfo --target imgui` PASS.
- `cmake --build build64_island --config RelWithDebInfo --target gui_runtime mc2`
  PASS; only pre-existing C4267/C4996/LNK4199 warnings.

## RUNG 8.6b - VK-IMGUI-TAIL-SEAM-1 SHIPPED

Behavior-neutral tail contract for the eventual native ImGui/HUD renderer.
`EnginePresentVk::directPresentFrameArmed()` now exposes the only valid
precondition for a final-tail VK overlay attempt: the current frame's
`endScene()` successfully armed the direct-present shared FBO via
`presentDirectFbo()`. This intentionally does not begin a Vulkan render pass,
signal/wait semaphores, or move HUD/ImGui ordering; `presentGLFrame()` still owns
the single GL->VK handoff for the frame.

Call-chain recon, now treated as the 8.6 hard ordering contract:
- `gosPostProcess::endScene()` binds either FBO-0 or the direct shared present
  FBO.
- `gameosmain.cpp` then flushes GameOS HUD and calls `GuiRuntime::Render()`.
- The swap path later calls `EnginePresentVk::presentGLFrame()`, which performs
  the GL->VK signal and swapchain blit/present.

Hard ban remains: native HUD/ImGui must not write into the shared present image
before a real tail handoff split exists. The new query only prevents future code
from guessing at `directFbo_` state.

Verification:
- Rebuilt `build64_island` with clean fingerprint
  `sha=a96e1809f4f8 dirty=0`.
- Deployed exact 0.4c lane:
  `A:/Games/mc2-opengl/mc2-win64-0.4c`, manifest
  `src_commit=a96e1809f4f84928387de2a5d65ebecd11524544`.
- Vulkan/direct-FBO smoke:
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1`
  with `scripts/run_smoke.py --exe A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe --mission mc2_01 --mission mc2_24 --duration 20 --keep-logs`
  PASS 2/2, fingerprint OK, +0 destroys.

## RUNG 8.6c - VK-IMGUI-DECLINE-GATE-1

`MC2_VK_IMGUI=1` is now a real smoke-forwarded runtime gate, but still
behavior-neutral: `GuiRuntime::Render()` emits one `[VK_IMGUI]` receipt with
`requested=1`, `used_vulkan=0`, `direct_present_armed=<0|1>`, and
`fallback_reason="tail_handoff_not_split"`, then falls through to the existing
SDL2/OpenGL3 renderer. This proves the gate reaches the exact ImGui render site
without beginning a Vulkan render pass or changing ordering.

This is intentionally not a native renderer yet. The decline reason remains the
8.5b/8.6b contract: `presentGLFrame()` owns the single final GL->VK handoff, so
native ImGui cannot write the shared present image until the final-tail handoff is
split.

Verification:
- Build PASS at `7b4741ee` (`build64_island`, RelWithDebInfo), dirty=0 before deploy.
- Deployed exact 0.4c lane with manifest `src_commit=7b4741eeed0e6cd3cc4134323a525ab07473428e`.
- Gate-on minimized smoke:
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1 MC2_VK_IMGUI=1`
  with `scripts/run_smoke.py --exe A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe --mission mc2_01 --mission mc2_24 --duration 20 --keep-logs`
  PASS 2/2, fingerprint OK, +0 destroys.
- Receipt in both mission logs:
  `[VK_IMGUI] requested=1 used_vulkan=0 direct_present_armed=0 fallback_reason="tail_handoff_not_split"`.
  `direct_present_armed=0` is expected for minimized smoke because VK present
  fail-softs on zero surface extent; use visible capture for direct-FBO armed proof.
- Follow-up hygiene: `MC2_VK_IMGUI` registered in `RendererFeatureRegistry.h`;
  `docs/flags.auto.yaml` regenerated. `check-env-allowlist.sh` remains advisory-dirty
  from pre-existing harness-only vars, but reports zero engine `getenv` vars missing
  from the smoke allowlist.
- Visible-window proof at `2eecfdbf`:
  `run_visual_capture.py --show-window --mission mc2_01 --bookmarks tests/visual/bookmarks/mc2_01.json`
  with `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1 MC2_VK_IMGUI=1`
  wrote 11/11 deterministic bookmark PNGs under
  `.claude/vk-imgui-decline-visible-armed/`.
  Present path stayed live (`present_frames=6301`, `validation_errors=0`,
  `readback_changed=1`, `fallback_reason=""`) while ImGui emitted
  `[VK_IMGUI] requested=1 used_vulkan=0 ... fallback_reason="tail_handoff_not_split"`.
  The `direct_present_armed` field stayed 0 from the ImGui call site even with
  live VK present, so it is a conservative predicate only; do not treat it as
  proof that direct-FBO present was unavailable.

## RUNG 8.6d - VK-TAIL-HOOK-1

Behavior-neutral final-tail command-buffer seam landed. `MC2_VK_PRESENT_TAIL_HOOK=1`
adds a no-op image memory dependency inside `EnginePresentVk::presentGLFrame()`
after the final GL->VK handoff and before the swapchain blit. This is the exact
place native HUD/ImGui rendering must attach: the shared present image is
Vulkan-owned, still in `COLOR_ATTACHMENT_OPTIMAL`, and the existing present
command buffer already owns the final submit/present cadence. The GL->VK
semaphore wait stage expands to include `COLOR_ATTACHMENT_OUTPUT` only while
the hook is enabled.

Verification:
- Build PASS at `d710e56c` + dirty hook changes (`build64_island`, RelWithDebInfo).
- Deployed 0.4c lane; manifest `src_commit=d710e56c3622`.
- Visible-window proof:
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1 MC2_VK_PRESENT_TAIL_HOOK=1 MC2_VK_IMGUI=1`
  with `run_visual_capture.py --show-window --mission mc2_01 --bookmarks tests/visual/bookmarks/mc2_01.json`
  wrote 11/11 deterministic bookmark PNGs under `.claude/vk-present-tail-hook/`.
- `[VK_PRESENT]` final health:
  `present_frames=6301`, `direct_fbo_requests=6301`, `direct_fbo_grants=6301`,
  `copy1_blit_frames=0`, `copy1_skip_frames=6301`, `tail_hook_frames=6301`,
  `validation_errors=0`, `fallback_reason=""`.
- No-hook vs hook PNG byte compare: 9/11 byte-identical; the two differences
  were negligible (`cross_map_view`: 2 pixels / 1,638,720, max delta 9;
  `ridge_lowangle`: 24 pixels / 1,638,720, max delta 1). Treat hook as visually
  neutral. NEXT: replace the no-op dependency with the ImGui Vulkan draw-data
  render pass at this seam; do not attempt a second handoff from `GuiRuntime`.

## RUNG 8.6e - VK-IMGUI-NATIVE-1

Native ImGui renderer landed on the final-tail seam. `GuiRuntime` still owns the
ImGui context, SDL input backend, frame construction, and `ImGui::Render()`.
When `MC2_VK_IMGUI=1`, it does not initialize or call the OpenGL3 renderer
backend. Instead it uses the SDL2 Vulkan platform init, builds the CPU font
atlas, calls `EnginePresentVk::prepareImguiVulkanFrame()` before `ImGui::NewFrame()`,
then hands `ImGui::GetDrawData()` to `EnginePresentVk`. `EnginePresentVk`
initializes `ImGui_ImplVulkan` lazily, creates a LOAD/STORE render pass and
framebuffer over the shared present image (`VK_FORMAT_R8G8B8A8_UNORM`), and
records `ImGui_ImplVulkan_RenderDrawData()` inside `presentGLFrame()` after the
GL->VK handoff and before the swapchain blit.

Important lifecycle rule: `GuiRuntime::Shutdown()` calls
`EnginePresentVk::shutdownImguiVulkanRenderer()` before destroying the ImGui
context. `EnginePresentVk::destroy()` remains a no-op-safe backup, so the
renderer backend is not shut down after the context is gone.

Verification:
- Build PASS at `0be75ff2` + dirty native-ImGui changes (`build64_island`, RelWithDebInfo).
- Deployed 0.4c lane; manifest `src_commit=0be75ff2221b`.
- First native attempt crashed in `ImGui::NewFrame()` because the CPU font atlas
  was not built without the GL renderer. Fixed by `io.Fonts->Build()` on the VK
  renderer path.
- Second native attempt rendered every frame but crashed on shutdown because the
  ImGui context was destroyed before `ImGui_ImplVulkan_Shutdown()`. Fixed by the
  explicit present-owned renderer shutdown from `GuiRuntime::Shutdown()`.
- Clean visible proof:
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1 MC2_VK_PRESENT_TAIL_HOOK=1 MC2_VK_IMGUI=1`
  with `run_visual_capture.py --show-window --mission mc2_01 --bookmarks tests/visual/bookmarks/mc2_01.json`
  wrote 11/11 deterministic bookmark PNGs under `.claude/vk-imgui-native-clean/`.
- Proof lines:
  `[VK_IMGUI] requested=1 used_vulkan=1 fallback_reason=""`;
  final `[VK_PRESENT]` had `present_frames=6301`, `tail_hook_frames=6301`,
  `imgui_vk_ready=1`, `imgui_vk_frames=6301`, `copy1_blit_frames=0`,
  `copy1_skip_frames=6301`, `validation_errors=0`, `fallback_reason=""`.
- Shutdown-clean: no `Crash`, no `Forgot to shutdown Renderer backend`, no
  `[imgui-error]` in `.claude/vk-imgui-native-clean/mc2_01_capture.log`.
- Visual delta vs the 8.6d tail-hook baseline: 9/11 byte-identical; only the
  known tiny unstable views differed (`cross_map_view`, `ridge_lowangle`).

Post-commit verification at clean HEAD `4ed1ca21db54`:
- Minimized two-mission smoke with
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1 MC2_VK_PRESENT_TAIL_HOOK=1 MC2_VK_IMGUI=1`
  PASS 2/2 (`tests/smoke/artifacts/2026-07-08T13-32-28`), fingerprint OK,
  +0 destroys.
- Combined rung-8 ownership smoke with
  `MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1 MC2_VK_POST_FOG_NATIVE=1 MC2_VK_POST_SSAO=1 MC2_VK_POST_SCREENFX=1 MC2_VK_POST_HZB=1 MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_TAIL_HOOK=1 MC2_VK_IMGUI=1 MC2_HZB_BUILD=1 MC2_SSAO=1`
  PASS 2/2 (`tests/smoke/artifacts/2026-07-08T14-47-14`), fingerprint OK,
  +0 destroys. Because smoke starts minimized, present and ImGui correctly
  decline with `zero surface extent` / `vulkan_renderer_not_ready`; use the
  visible proof above for present/ImGui.
- Same combined smoke proves postprocess-native paths actually execute when
  `MC2_VK_SHARED_GBUFFER=1`: `VK_POST_FOG_NATIVE`, `VK_POST_SSAO_NATIVE`,
  `VK_POST_SCREENFX_NATIVE`, and `VK_POST_HZB_NATIVE` all report
  `substrate_live=1`, `used_vulkan>0`, `fallback_reason=""` on both `mc2_01`
  and `mc2_24`. HZB reports `copy_depth_ops=0 readback_ops=0`.

## RUNG 8.7 READINESS - NOT YET DELETABLE

Do not delete old interop scaffolding yet. The rung-8 ladder's final deletion
bar is stricter than current code:
- 8.5b native composite first runtime slice is landed, not complete. Under
  `MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1
  MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_POST_COMPOSITE=1`, Visual mode with FXAA
  off and a full drawable viewport now samples shared scene color0 in VK and
  writes the shared present image. Visible proof:
  `.claude/vk-post-composite-native-first-2/`, 11/11 deterministic `mc2_01`
  bookmarks, `[VK_PRESENT] composite_vk_frames=300`, `copy1_blit_frames=0`,
  `fallback_reason=""`; minimized smoke `tests/smoke/artifacts/2026-07-08T15-02-41`
  PASS 2/2, +0 destroys. Remaining composite branches still fall back to GL:
  FORCE-43/OOB-letterbox and viewmode/object-id/debug paths.
- 8.5c native composite FXAA is landed. The native Visual composite now ports
  the existing GL FXAA filter and tunables. Visible proof:
  `.claude/vk-post-composite-fxaa-native/`, `MC2_FXAA=1`, 11/11 deterministic
  `mc2_01` bookmarks, `[FXAA v1] enabled=1`, `[VK_PRESENT]
  composite_vk_frames=300`, `copy1_blit_frames=0`, `fallback_reason=""`.
  Minimized smoke with `MC2_FXAA=1` forwarded:
  `tests/smoke/artifacts/2026-07-08T15-12-17`, PASS 2/2, +0 destroys.
- 8.5d native composite FORCE-43 is landed. The native Visual composite now
  clears the shared present image black in VK and draws into the caller-supplied
  pillarbox viewport. Visible proof:
  `.claude/vk-post-composite-force43-native/`, `MC2_FORCE_43=1`, 11/11
  deterministic `mc2_01` bookmarks, `[VK_PRESENT] composite_vk_frames=300`,
  `copy1_blit_frames=0`, `fallback_reason=""`. Minimized smoke with
  `MC2_FORCE_43=1` forwarded: `tests/smoke/artifacts/2026-07-08T15-16-01`,
  PASS 2/2, +0 destroys.
- 8.5e native composite LowLight is landed. LowLight/NightVision mode 5 runs in
  the native composite using the same gain/tint math; no object-ID texture is
  needed. Visible proof: `.claude/vk-post-composite-lowlight-native/`,
  `MC2_VIEWMODE_FRAMEWORK=1 MC2_VIEW_MODE=5`, 11/11 deterministic `mc2_01`
  bookmarks, `[VIEWMODE v1] framework=1 initialMode=5`, `[VK_PRESENT]
  composite_vk_frames=300`, `copy1_blit_frames=0`, `fallback_reason=""`.
  Minimized smoke: `tests/smoke/artifacts/2026-07-08T15-20-02`, PASS 2/2,
  +0 destroys.
- 8.5f native composite ObjectIdDebug/Thermal is landed. The shared scene
  substrate now has `MC2_SHARED_SCENE_OBJECT_ID`, backed by `VK_FORMAT_R32_UINT`
  imported to `GL_R32UI`; native composite binds it as a `usampler2D` for mode 1
  and mode 3. Visible proofs:
  `.claude/vk-post-composite-objectid-native/` and
  `.claude/vk-post-composite-thermal-native/`, each 11/11 deterministic
  `mc2_01` bookmarks, `[VK_SHARED_SCENE_COLOR] objectId(MRT2) active: VK R32UI
  ... imported to GL_R32UI`, `[VK_PRESENT] composite_vk_frames=300`,
  `copy1_blit_frames=0`, `fallback_reason=""`. Minimized Thermal smoke:
  `tests/smoke/artifacts/2026-07-08T15-28-36`, PASS 2/2, +0 destroys.
- 8.6 native ImGui is landed, but GameOS HUD (`gos_RendererFlushHUDBatch()`) is
  still legacy GL. `MC2_UI_PASS_MEASURE=1` now emits `[VK_HUD_AUDIT v1]`
  batch shape lines for the native HUD port. Focused `mc2_01` smoke
  `tests/smoke/artifacts/2026-07-08T15-33-28` PASS 1/1 showed the tier1 HUD is
  not an untextured micro-slice: steady frames are `calls=95 quads=56 lines=18
  tris=14 text=7 textured=69 vertices=1106 alpha1=4 alpha3=91`. Native HUD
  therefore needs ordered textured quad + text replay and a gosTexture mirror,
  not just line/tri replay.
- `vulkan_postprocess_subgraph.cpp`, `vulkan_ssao_island.cpp`,
  `vulkan_hzb_island.cpp`, and the edge/oob fog islands are now bypassed by the
  native rung-8 gates during the verified shared-GBuffer smoke, but they still
  compile as default-off fallback/oracle paths.
- `gl_interop_view.*` and the Win32 external image memory/semaphore extensions
  are still required by the shared scene color/depth/present substrates while
  GL continues to produce composite/HUD pixels. The unused buffer interop seam
  (`vk_gl_shared_buffer.*` / `gl_interop_buffer_view.*`) was deleted after rung
  8.7j because no live path includes or calls it.

Current tail status:
- Native GameOS HUD replay and skip-GL proof are shipped; see
  `VK-RUNG8-LADDER-1.md` 8.6f+ through 8.6j.
- The remaining native composite diagnostic branch is now closed for the
  ownership-relevant CSM dynamic case: `MC2_SHADOW_DEBUG_MODE=dynamic` uses a
  native VK overlay when the shadow island exposes a same-frame CSM array view.
  Static/non-ready debug sources intentionally retain GL fallback.
- Present copy-2 has a VK-owned proof gate: `MC2_VK_PRESENT_SKIP_GL_HANDOFF=1`
  skips the GL->VK semaphore signal/wait when direct present is armed and the
  shared present image was produced by VK. Visible proof captured 3/3 building
  bookmarks in `.claude/vk-present-skip-gl-handoff/` and logged
  `gl_handoff_skip_frames=1`.
- Present native strict is now visible-proven across the full native final-tail
  stack (`MC2_VK_POST_COMPOSITE=1`, direct FBO, tail hook, HUD skip, ImGui
  Vulkan, skip GL handoff). Receipt:
  `.claude/vk-present-native-strict-visible-clean/`, 3/3 deterministic
  `mc2_01_buildings` bookmarks, `copy1_blit_frames=0`,
  `gl_handoff_skip_frames=2700`, `composite_vk_frames=2700`,
  `imgui_vk_frames=2700`, `fallback_reason=""`.
- Global GL/VK interop strict canary is live and currently fails as expected:
  `MC2_VK_NO_GL_INTEROP_STRICT=1` aborts at the first
  `glInteropSignalToVk` boundary. This is the next deletion-readiness proof to
  turn green; until then the Win32 external-memory/semaphore family remains live.
- Static shadow has a VK substrate gate: `MC2_VK_STATIC_SHADOW_MAP=1` backs
  `ShadowStaticMap` with exportable VK `D32_SFLOAT` imported into GL as
  `GL_DEPTH_COMPONENT32F`. GL still renders/samples it; native screen-shadow
  can later bind the same image without a copy.
- Static-only screen-shadow can now render natively under `MC2_VK_POST_SCREENFX=1`
  + `MC2_VK_STATIC_SHADOW_MAP=1`; it declines only when dynamic shadows require
  a path that is not yet VK-readable.
- CSM dynamic screen-shadow can now render natively when the direct VK CSM
  producers are armed (`MC2_VK_SHADOW_INTEROP=1`,
  `MC2_VK_CSM_ARRAY_INTEROP=1`, `MC2_VK_CSM_FULLMAP_INTEROP=1`). The
  `screen_shadow_csm` pipeline samples the same-frame VK CSM array/full-map
  views directly and skips the GL screen-shadow body under `MC2_SHADOW_ENABLE=1`.
  Debug classification and non-CSM dynamic stay GL fallback paths.
- Remaining deletion blocker: shared scene color/depth/present substrates still
  rely on `gl_interop_view.*` while GL can produce earlier scene pixels.
- ObjectIdDebug strict canary is green under the native final-tail stack:
  `MC2_VK_OBJECTID_DIRECT_CHAIN=1` skips the native composite's objectId MRT2
  interop handoff when the full composite direct-chain stack is armed. Correct
  Vulkan proof (after discarding a non-Vulkan `build64` false positive):
  `.claude/vk-nointerop-strict-visible-objectid-direct-clean/`, 11/11
  deterministic `mc2_01` bookmarks with `MC2_OBJECT_ID_BUFFER=1`,
  `MC2_VIEW_MODE=objectid`, `MC2_VK_NO_GL_INTEROP_STRICT=1`; log shows
  `sha=d890a2f2f1f0 dirty=0`, objectId VK `R32UI`, all
  terrain/prop/mech/water islands active, `composite_vk_frames=300`,
  `gl_handoff_skip_frames=300`, smoke PASS, and no strict abort. This is a
  deletion-readiness canary, not a full objectId MRT2 parity port.
- Native fog direct-chain strict canary is green:
  `.claude/vk-nointerop-strict-visible-postfog-direct-clean/`, 11/11
  deterministic `mc2_01` bookmarks, Vulkan backend selected, fog initialized on
  the shared engine context, `sha=35c56c4c362d dirty=0`,
  `VK_POST_FOG_NATIVE [HEALTH] attempted=662 used_vulkan=662 direct_chain=1
  used_interop=0 gl_skipped=1324`,
  `VK_PRESENT composite_vk_frames=600 gl_handoff_skip_frames=600`, water/prop
  `used_interop=0`, no fail-soft GL fallback and no `[VK_NO_GL_INTEROP_STRICT]`
  abort.
- Native SSAO direct-chain strict canary is green:
  `.claude/vk-nointerop-strict-visible-postssao-direct-dirty-ssaoon/`, 11/11
  deterministic `mc2_01` bookmarks with `MC2_SSAO=1`,
  `MC2_VK_POST_SSAO=1`, `MC2_VK_POST_SSAO_DIRECT_CHAIN=1`,
  `MC2_VK_NO_GL_INTEROP_STRICT=1`; log shows `SSAO v1 enabled=1 debug=0`,
  native SSAO initialized on the shared engine context, `VK_POST_SSAO_NATIVE
  [HEALTH] attempted=661 used_vulkan=661 direct_chain=1 used_interop=0
  gl_skipped=661`, fog still `used_interop=0`, present
  `composite_vk_frames=600 gl_handoff_skip_frames=600`, water/prop
  `used_interop=0`, and no `[VK_NO_GL_INTEROP_STRICT]` abort. Note:
  the first SSAO strict attempt without `MC2_SSAO=1` was only a crash regression
  (`attempted=0`) and is not the execution proof.
- Native screenfx + HZB direct-chain strict canary is green:
  `.claude/vk-nointerop-strict-visible-postscreenfx-hzb-direct-dirty-cleanrun/`,
  11/11 deterministic `mc2_01` bookmarks with screenfx, SSAO, fog, HZB, and
  native present all armed under `MC2_VK_NO_GL_INTEROP_STRICT=1`; log shows
  `VK_PRESENT composite_vk_frames=600 gl_handoff_skip_frames=600`, screenfx
  `attempted=1324 used_vulkan=1324 shadow_frames=662 shore_frames=662
  direct_chain=1 used_interop=0 gl_skipped=1324`, HZB `attempted=662
  used_vulkan=662 direct_chain=1 used_interop=0 levels=12 vk_draws=12
  copy_depth_ops=0 readback_ops=0`, fog/SSAO/water/prop all `used_interop=0`,
  no present fail-soft, and no `[VK_NO_GL_INTEROP_STRICT]` abort. The HZB path
  now defers native execution for the first observed extent/mip-count frame so
  the present swapchain handles resize before HZB allocates/renders.
- Native shadow direct-chain strict canary is green:
  `.claude/vk-nointerop-strict-visible-shadow-direct-clean-final/`, 11/11
  deterministic `mc2_01` bookmarks with dynamic CSM shadows, static shadow
  substrate, native CSM screen-shadow, post stack, HZB, composite, and native
  present all armed under `MC2_VK_NO_GL_INTEROP_STRICT=1`; log shows
  `sha=53311e5aced3 dirty=0`, `MC2_VK_SHADOW_DIRECT_CHAIN=1 -- VK shadow
  products stay VK-owned; GL semaphore wait is skipped`, `first VK shadow map
  produced ... (direct VK consumers only)`, first VK CSM array/full-map
  products, present `composite_vk_frames=6600 gl_handoff_skip_frames=6600`,
  no present fail-soft, and zero occurrences of `[VK_NO_GL_INTEROP_STRICT]`,
  `glInteropSignalToVk`, or `glInteropWaitFromVk`. Detailed post-health
  receipt remains the dirty pre-commit proof
  `.claude/vk-nointerop-strict-visible-shadow-direct-dirty-receipts/`, with
  screenfx `shadow_frames=6629 direct_chain=1 used_interop=0` and
  HZB/fog/SSAO/water/prop all `used_interop=0`.

## RUNG 8.6f - VK-HUD-NATIVE-QUEUE-1

`MC2_VK_HUD=1` now deep-copies the already-scaled ordered GameOS HUD batch from
`gosRenderer::flushHUDBatch()` into `EnginePresentVk` for final-tail replay. The
GL HUD replay remains active until the Vulkan consumer can render every call
kind; this slice proves the ownership handoff and exact tail ordering before
native ImGui.

Verification:
- Build: `cmake --build build64_island --config RelWithDebInfo --target mc2`
  passed; only the known `mechcmd2.cpp` C4267 and delay-load LNK4199 warnings.
- Minimized smoke with `MC2_VK_HUD=1`:
  `tests/smoke/artifacts/2026-07-08T15-38-51`, PASS 1/1, +0 destroys. As
  expected, present surface was zero extent, so queue counters stayed 0 there.
- Visible bookmark run:
  `.claude/vk-hud-queue-visible/`, 10/11 deterministic captures before the
  duration window ended. Runtime proof line: `[VK_PRESENT] created=1 ...
  tail_hook_frames=1 ... hud_vk_queued_frames=1 hud_vk_consumed_frames=1
  hud_vk_draws_last=85 imgui_vk_ready=1 imgui_vk_frames=1 ... fallback_reason=""`.
  Steady HUD audit remained `calls=95 ... textured=69 text=7`.
