# VK-P14 RECON — "VK owns the renderer / retire the present tax"

**Worktree:** `A:/Games/mc2-vk-interop-bridge` · branch `claude/vk-interop-bridge-1` @ `8c1856c8`
**Scope of this recon:** honestly size P14 and find the smallest landable increment (if any). Read-only.

---

## VERDICT (lead)

**P14-full is the geometry-migration ENDGAME, NOT a single landable slice.** To make VK *own the frame*
and truly retire the present tax, VK must PRODUCE the final composited frame — which requires terrain +
static-prop COLOR + mechs + HUD all rendered in VK. Today only water (P9) + shadow-depth (P8a) are VK
regions; the entire visible scene is still GL. That is Phase C (P10/P11/P12) + HUD, i.e. many XXL slices.
Do **not** pretend P14-full fits this session.

**BUT there is one bounded, provable P14 increment that lands THIS session: the 2→1 copy present-path
optimization ("copy-1 elimination").** It removes the GL→shared-image blit (copy-1) by having the GL
renderer composite its **final frame directly into the shared exportable RGBA8 image's FBO**, leaving
only the VK→swapchain blit (copy-2). This is real P14 progress (it retires *half* the present tax) and
is provable with the existing present-verify readback + a present-frame counter, without porting one
triangle of the renderer. **Recommended slice: `MC2_VK_PRESENT_DIRECT_FBO` (default-OFF).**

**If copy-1 elimination proves infeasible on inspection (see risks §1), the honest fallback is: SCOPE
P14 + Tracy-measure the current hybrid frame + hand off.** Either way, the Tracy capture .bat below is
wanted so the next session sees where CPU+GPU time actually goes.

---

## 1. PRESENT-TAX ANATOMY (the 2 copies)

Single implementation: `GameOS/gameos/engine_present_vk.cpp`, `EnginePresentVk::presentGLFrame()`.
Called once per frame from `gameosmain.cpp:1834` (in the `SwapWindow.SDL` zone, replacing
`SDL_GL_SwapWindow` when `MC2_RENDER_BACKEND=vulkan` and the VK present path is `valid()`).

- **COPY-1 (GL blit, FBO-0 → shared exportable GL texture):** `engine_present_vk.cpp:478-497`.
  `glBindFramebuffer(GL_READ_FRAMEBUFFER, 0)` + `glReadBuffer(GL_BACK)` → `glBlitFramebuffer` into
  `s.copyFbo` (an FBO wrapping the imported shared texture, built at `:296-306`). Y-flipped
  (GL bottom-left → swapchain top-left). Cost class: **1× fullscreen RGBA8 blit** on the GL queue.
- **GL→VK hand-off:** `:503-504` — `vkImageResourceBeginFrame` + `glInteropSignalToVk` (GL leaves the
  shared image in COLOR_ATTACHMENT layout; a binary semaphore tells VK it's ready). Zero-copy seam.
- **COPY-2 (VK blit, shared image → swapchain):** `engine_present_vk.cpp:554-564`.
  `vkCmdBlitImage(sharedImg → scImg, VK_FILTER_LINEAR)` with layout barriers `:542-579`, submit `:603`,
  `vkQueuePresentKHR` `:624`. Cost class: **1× fullscreen RGBA8→BGRA8 blit** on the VK queue + present.

**WHY both exist:**
- GL owns the whole frame; it renders into **FBO-0** (window default framebuffer). FBO-0 is created by
  the OS/WSI for the GL context — it is **not** an exportable/shared image, so VK cannot read it directly.
  Hence copy-1 stages FBO-0 into an exportable image GL *can* render/blit into.
- Swapchain images are **VK-only** (owned by `vkCreateSwapchainKHR`, `:225`); GL cannot render into them.
  Hence copy-2 gets the frame from the shared image into the swapchain for present.

### Is 2→1 feasible WITHOUT porting the renderer? — YES for copy-1, NO for copy-2.

- **Copy-2 is NOT removable** while GL produces the frame: something must move GL-produced pixels into a
  VK swapchain image, and GL can't write swapchain images. Copy-2 is the irreducible GL→VK present bridge
  until VK renders the final frame itself (that's P14-full).
- **Copy-1 IS removable.** The shared image is already created with `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`
  (`:287`) and wrapped in a GL FBO (`s.copyFbo`, `:296-306`) that is `GL_FRAMEBUFFER_COMPLETE`. Today
  that FBO is only a blit *destination*. If the **game's final composite/HUD/pillarbox pass renders
  directly into `s.copyFbo`** instead of FBO-0, the shared image already holds the finished frame — copy-1
  disappears; only the interop signal + copy-2 remain.

  **Feasibility / risk:** MC2 does most scene work in `sceneFBO_` then composites/tonemaps/HUDs into
  **FBO-0** at swap (see `gameosmain.cpp:1748` "composites to FBO 0", and the post-process chain in
  `gos_postprocess.cpp`). Redirecting the *final* stage's draw target from FBO-0 to `s.copyFbo` is the
  crux. Two sub-risks: (a) the final-composite draw target is chosen deep in the GL path (FBO 0 hardcoded
  at several sites) — needs a single seam where "the framebuffer we present" is named; (b) **Y-flip**:
  copy-1 currently flips Y; rendering directly into the shared image means the VK copy-2 blit must flip
  instead (swap `region.srcOffsets`/`dstOffsets` Y in `:557/:560`), or the swapchain image comes out
  upside-down. (c) sRGB/format: FBO-0 vs the RGBA8 shared image color-encoding must match or colors shift.

  **Assessment:** removable *in principle*, MEDIUM implementation risk concentrated in "find the single
  final-composite target seam." If that seam is clean (one place that binds FBO 0 before HUD/pillarbox),
  the slice is small + provable. If the final composite is smeared across many hardcoded `glBindFramebuffer(…,0)`
  sites, it's larger than one session — fall back to scope+Tracy+handoff. **A 30-min code-trace of the
  final-composite target is the gating recon for committing to this slice.**

---

## 2. "VK OWNS THE RENDERER" SCOPE — what remains

To retire the tax fully, VK must PRODUCE the final frame. Regions VK-native today:
- **Water** — P9, `vulkan_water_island.cpp` (exportable **RGBA16F**, VK alpha-composites water over scene
  color, GL composites result onto MainColor). Gate `MC2_VK_WATER_INTEROP`.
- **Shadow depth** — P8a, `vulkan_shadow_island_live.cpp` (VK produces **D32** static-prop caster depth,
  GL samples it as the shadow map). Gate `MC2_VK_SHADOW_INTEROP`. Depth-only; **mechs' bone-skinned
  casters + CSM array still GL**.

Still **GL** (must become VK-native for VK to own the frame):
1. **Terrain** — P10. GPU-indirect pipeline (recipe/thin SSBOs, cement atlas). Roadmap flags it "biggest,
   last-but-one." XXL.
2. **Static-prop COLOR** — P11. (Shadow depth done; color pass not.) Large.
3. **Mechs** — P12. Needs a VK GPU-skinning path. Heavy/XXL.
4. **HUD / overlay / final composite / pillarbox** — the tail that today lands in FBO-0. Must be VK-drawn
   (or VK-composited) for VK to own present (roadmap "P14b HUD/overlay compositing routed").
5. **Full postprocess tail native** — P6/P7 (SSAO→BoxDecals→fog chained VK-native); today only 4 PP
   regions are on the seam, not chained.

**Shortcut?** None that's honest. The roadmap's own sequencing (Phase C serial geometry regions → P14)
is the path. The *only* legitimate near-term shortcut toward the tax is the **copy-1 elimination in §1**,
which halves the present cost without owning the renderer. Full ownership = every draw in VK = the endgame.
**Blunt size: P14-full is 3+ XXL geometry regions + HUD + composite routing. Not one slice, not one
session — it's the remainder of Phase C plus Phase D.**

---

## 3. SMALLEST LANDABLE P14 INCREMENT NOW

**Recommended: `MC2_VK_PRESENT_DIRECT_FBO` — copy-1 elimination (2→1 present copy).**
- **What:** when `MC2_RENDER_BACKEND=vulkan` AND this gate on, bind the game's final-composite/HUD pass to
  `EnginePresentVk`'s shared-image FBO (`s.copyFbo`) instead of FBO-0, so `presentGLFrame` skips the copy-1
  blit and goes straight to interop-signal + copy-2. Move the Y-flip from copy-1 into the copy-2 blit.
- **Why it's real P14:** it retires the *removable half* of the present tax and is a stepping stone toward
  VK-owns-present (the shared image becomes the frame's true render target, not a staging copy).
- **Proof (no new fixtures):** existing `verifyReadbackContent()` (`:658`) + `readback_changed` +
  `present_frames` counter prove the shared image still holds real, changing, correct frame content with
  copy-1 gone. A/B the gate-OFF vs gate-ON present against the golden bookmarks (present output is the
  final frame — the existing backend-compare bookmarks apply). Forbidden: any residual FBO-0→shared blit.
- **Default-OFF byte-identical** (gate-OFF exe unchanged); register + allowlist + document the gate.
- **STOP-gate:** if the final-composite target seam is smeared (many hardcoded FBO-0 binds) → the direct
  redirect isn't a clean one-seam change → **do NOT force it**; drop to the fallback below.

**Fallback if copy-1 elimination is not one-seam-clean:** the honest move is **SCOPE + MEASURE + HANDOFF**
— write this doc (done), run the Tracy capture (§4), and hand off P14-full as the Phase-C-remainder endgame
with the copy-1 idea parked as the first optimization once measured. **Do not fake progress by shipping a
present-path "cleanup" that doesn't remove a copy.**

**Gating recon before coding (30 min):** trace where the final composited/HUD/pillarbox frame's draw
target is bound (grep `glBindFramebuffer` + `, 0)` in `gos_postprocess.cpp` / `gameos_graphics.cpp` around
the composite/HUD/pillarbox tail; confirm one nameable seam). Verdict from that trace decides slice vs handoff.

---

## 4. TRACY CAPTURE TARGET (what the .bat should run)

**Goal:** capture the current HYBRID frame so the next session sees where CPU+GPU time actually goes —
GL scene render cost, the 2-copy present tax, and the water/shadow VK region cost when gated on.

- **Backend/present gates (present tax visible):** `MC2_RENDER_BACKEND=vulkan` + `MC2_NO_LAUNCHER=1`
  → routes through `presentGLFrame` (the 2-copy path). Without these you measure the plain GL swap, not
  the tax.
- **Region gates (VK geometry cost visible):** `MC2_VK_WATER_INTEROP=1` + `MC2_VK_SHADOW_INTEROP=1` so
  the water (RGBA16F) + shadow (D32) VK regions are live and show up as VK-queue work.
- **Scene/pose:** a **shadow+water-visible, heavy** scene. Use **mc2_01** (water present, tier1 member,
  known-good bookmarks incl. water + shadow) at a **wolfman-class zoom** (close, many props/mechs +
  shadows in frame). If a beefier water+shadow scene is preferred, mc2_24 (tier1, heavy) as a second pass.
- **Present-verify on for the capture:** `MC2_VK_PRESENT_VERIFY=1` (readback proves live content;
  `gameosmain.cpp:1831` throttles it to frames <600, every 60th — light).
- **HARD REQUIREMENTS for the VK present path to NOT fail-soft during capture:**
  - **Windowed, non-minimized** — a zero drawable extent makes `presentGLFrame` skip
    (`engine_present_vk.cpp:459`, `buildSwapchain:203`) and can `OUT_OF_DATE`/fallback to GL. Keep the
    window visible + focused for the whole capture.
  - **Tracy connects on 127.0.0.1** (TRACY always compiled in per `TRACY_ENABLE`; the capture tool must
    connect to localhost while the game runs foreground).
  - **Foreground / blocking run** (per WORKTREE-GUARD: background VK runs have died mid-run).
- **Zones to read afterward:** `SwapWindow` / `SwapWindow.SDL` (the present tax lands here —
  `gameosmain.cpp:1804/1815`); `Camera.UpdateRenderers` + `GameLogic.*` (GL scene render CPU); the VK
  region islands' own zones (water/shadow) if present. On GPU side (RGP/Tracy-GPU): copy-1 blit, copy-2
  blit, water composite, shadow depth pass.

**.bat should:** set the env gates above, launch the deployed VK-island exe
(`mc2-win64-v0.4c`, built `build64_island/RelWithDebInfo`) on mc2_01 windowed at a heavy zoom, run
foreground long enough for Tracy to capture a representative stretch (~30–60 s of steady-state), with the
window kept visible. Name it e.g. `.claude/tracy-capture-vk-hybrid.bat`. (Not written by this recon —
this doc specifies its contents; the user asked for the .bat next.)

---

## Evidence index (file:line)
- Present path + both copies: `GameOS/gameos/engine_present_vk.cpp` — copy-1 `:478-497`, hand-off
  `:503-504`, copy-2 `:554-564`, shared image usage flags `:285-292`, copy FBO `:296-306`, readback proof
  `:658-743`.
- Present call site / gate: `GameOS/gameos/gameosmain.cpp:1817-1847` (SwapWindow.SDL intercept),
  verify throttle `:1831`, "composites to FBO 0" `:1748`.
- Water region (RGBA16F, first geometry region): `GameOS/gameos/vulkan_water_island.cpp:1-25,385-405`,
  gate `MC2_VK_WATER_INTEROP` `:113`.
- Shadow region (VK-produces D32): `GameOS/gameos/vulkan_shadow_island_live.cpp:1-31,172,198`,
  gate `MC2_VK_SHADOW_INTEROP` `:116`.
- Roadmap sequencing (P10/P11/P12 → P14): `.claude/VK-ARC-ROADMAP.md` Phase C + Phase D.
