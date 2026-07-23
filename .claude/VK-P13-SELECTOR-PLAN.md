# VK-P13-SELECTOR-PLAN v2 — birth the shared engine VK device + MC2_RENDER_BACKEND selector, prove VK present end-to-end (hidden window)

**Worktree:** `A:/Games/mc2-vk-interop-bridge` · **branch** `claude/vk-interop-bridge-1` · **HEAD** `08215704`
**Piece:** P13 (roadmap PHASE D, `.claude/VK-ARC-ROADMAP.md:150-165`; resequenced ahead of P6 per `.claude/VK-NEXT-MOVE-RECON.md:10`).
**v2 changes:** rescoped after adversarial + meta review — see the changelog at the very bottom. The visible-window swap is split OUT into a separate follow-on **P13b**; every "zero-copy" claim is struck (the mechanism is 2 GL/VK copies per frame); the persistent VK device is now a first-class standalone shared context with an explicit acceptance criterion; R2 volk-dispatch is a hard-enforced interlock; present-mode/pacing is specified.

**Scope of P13 (this piece):** land `MC2_RENDER_BACKEND` (values `gl`|`vulkan`, default `gl`) as an init-time selector; **birth the persistent, standalone engine VK device/context** (the durable vehicle P6/P8/P9/P14 all reuse); on `=vulkan`, prove VK present end-to-end by presenting the finished GL frame through a VK swapchain **to a HIDDEN VK window** (no user-visible display change yet); fail-soft to GL on any failure. Present is the *vehicle* that exercises the device+selector+interop; it is not yet the user's display.

**Explicitly deferred to P13b:** making the VK window visible, hiding the GL window, and forwarding input/mouse-grab across two windows (the R1 HIGH risk). See §7b.

---

## LEAD VERDICT (read this first)

1. **Is P13 (rescoped) tractable? YES, and now R1-free.** By presenting to a *hidden* VK window, P13 removes the single biggest risk (two-window input/focus/grab, R1) from the shipped slice entirely — R1 lives only in the deferred P13b. What P13 ships is the durable machinery: the selector, the **standalone shared engine VK device**, and a proven whole-frame GL→VK present path, all armed behind `MC2_RENDER_BACKEND=vulkan` and fail-soft.

2. **The durable deliverable is the DEVICE, not the present.** The persistent engine VK device must be born as a **first-class standalone shared context** (a `VulkanContext` / `getOrCreateEngineDevice()` vehicle owning instance+physical-device+device+VMA+volk device-table+descriptor pool), **NOT** owned internally by `VulkanPresentObject`. P6/P8/P9/P14 all reuse it (`.claude/VK-NEXT-MOVE-RECON.md:122-123`). If it is born present-object-internal, durability collapses and it gets re-birthed later. This is an explicit **T2 acceptance criterion** (§7).

3. **Present is NOT zero-copy — it is 2 copies/frame.** The GL default framebuffer (FBO-0) cannot be backed by `GL_EXT_memory_object` (external memory objects can only back app-created textures/renderbuffers, never the window-system default framebuffer). So the mechanism is: **(copy 1)** an extra GL full-screen copy FBO-0 → an app-created exportable RGBA8 interop texture, then **(copy 2)** VK `vkCmdBlitImage` shared-image → swapchain image. Every "zero-copy" framing from v1 is struck. The seam is still the proven P1–P3 *transport primitive*, but the frame boundary carries a real 2-copy tax (§3, §9).

4. **CHOSEN APPROACH: (A′) — keep the GL window + GL context AS-IS.** Do NOT make the visible window `SDL_WINDOW_VULKAN`. GameOS init does `SDL_GL_CreateContext` on the handed window immediately (`gos_render.cpp:362`); making it VK kills the init cascade. Instead the VK path additionally stands up the shared engine device + a `VulkanPresentObject` **hidden** window and presents the GL frame there. (Naive (A) — visible VK window + separate hidden GL window + full off-FBO-0 redirect — is deferred to P14. Prompt-option (B)-as-two-*visible*-windows is rejected; P13b is (B)-with-GL-window-hidden.)

5. **SINGLE BIGGEST RISK now: R2, the volk process-global device-dispatch collision** (R1 is deferred with P13b). `volkLoadDevice` writes a PROCESS-GLOBAL dispatch table; every postprocess island loads its own device into it (edge-fog runs live in `endScene`, `gos_postprocess.cpp:3798`). This is handled as a **hard-enforced interlock** in T4 (§6/R2), not advice.

**Honest framing after P13 ships:** *"VK present backend proven end-to-end + armed behind the `MC2_RENDER_BACKEND` selector; the shared engine VK device is live; user-visible display swap is the gated follow-on P13b."* NOT "Vulkan is a selectable display backend." (Point 8.)

**Per-frame cost acknowledgement:** every intermediate VK-selectable state through P13/P13b carries a **whole-frame present tax** — the 2 copies (FBO-0→shared GL copy + VK blit→swapchain) plus the interop signal round-trip — until P14 retires the GL frame and VK owns the render. This tax is accepted for the migration phase ("get to Vulkan, THEN optimize"). (Point 9.)

---

## 1. INIT-ORDER + SELECTOR

### 1.1 Where the backend is read
`MC2_RENDER_BACKEND` is read **before** window creation, at/above the `SDL_CreateWindow` site (`gos_render.cpp:306-307`). The top-level gate is greenfield (only per-region `MC2_RENDER_BACKEND_IFACE` exists today, `gos_postprocess.cpp:2733,2839,3836-3839`). Introduce `enum class RenderBackend { GL, Vulkan };` + a memoized `RenderBackend gos_selectedBackend()` (idempotent env read, like the `s_backendIface` lambda). Default `GL`; unrecognized/`=gl`/unset → GL (log any coercion).

### 1.2 How window creation branches (A′)
The **GL window + GL context + all downstream GL init are UNTOUCHED** (`gos_render.cpp:305-389`) — this is why A′ is safe. The only `=vulkan` action is, *after* GL init and the first FBO exist: stand up the standalone engine VK device (T2) + a `VulkanPresentObject` **hidden** window (T3). No GL window hide, no input change in P13 (that's P13b).

### 1.3 Fallback
Any `=vulkan` failure — device bring-up, `VulkanPresentObject::init()` false (already fail-softs, sets `fallback_reason`, `vulkan_present_object.h:87,105`), LUID mismatch, interop import failure — sets session-sticky `s_forceGL`, logs the reason, and the present path stays `SDL_GL_SwapWindow` (unchanged). No crash. Extends P15 (roadmap `:161`). **Ordering rule (point 7):** never hide/deviate the GL window until VK present is proven up — moot in P13 (hidden VK window, GL always visible) but binding for P13b.

---

## 2. PRESENT FLOW

### 2.1 Today's GL present path (mapped)
```
gameosmain.cpp:1621  draw_screen()  -> scene render + gos_postprocess::endScene()
                                        endScene composites PP tail into sceneFBO_ (RGBA16F),
                                        binds FBO-0 (gos_postprocess.cpp:3830,3843), blits final
                                        image + FORCE-43 pillarbox into FBO-0; HUD/ImGui draw into FBO-0 after.
gameosmain.cpp:1709-1720  SwapWindow -> graphics::swap_window(win)
gos_render.cpp:349-354    swap_window() -> SDL_GL_SwapWindow(rw->window_)   <-- THE PRESENT + swap-interval owner
```
`gos_RendererEndFrame()` (`gameos_graphics.cpp:7888`, called `gameosmain.cpp:577` inside `draw_screen`) is the renderer flush, NOT the present. The present is `swap_window`→`SDL_GL_SwapWindow` at `gameosmain.cpp:1720` — the single intercept point.

### 2.2 VK-mode present flow (the intercept) — explicit order (point 4)
Replace the single call at `gameosmain.cpp:1720`:
```
if (backend == GL || s_forceGL) {
    graphics::swap_window(win);                 // unchanged (owns swap-interval today)
} else {
    gos_vk_present_gl_frame();                  // NEW
}
```
`gos_vk_present_gl_frame()` runs, **all AFTER HUD+composite are in FBO-0** (point 4):
1. **[copy 1] FBO-0 → shared exportable RGBA8 interop texture** via `glBlitFramebuffer` (or `glCopyImageSubData`), sized to the **backbuffer/drawable extent including the FORCE-43 pillarbox** — NOT sceneFBO_'s scene resolution (§3.2). This captures exactly what SDL would have swapped, HUD included.
2. **glInteropSignalToVk** — GL signals `glToVkSem` (the existing P1.5 GPU semaphore).
3. **VK acquire → [copy 2] `vkCmdBlitImage` shared-image → acquired swapchain image (waits `glToVkSem`) → present** on the `VulkanPresentObject`'s hidden window. OUT_OF_DATE/SUBOPTIMAL handled by its internal rebuild.

### 2.3 What changes in VulkanPresentObject
It presents a teal-clear + fullscreen triangle today to its OWN window (`vulkan_present_object.h:8-12,89-90`). Changes:
- **New entry point** `bool acquireBlitPresent(const VkImageResource& src)` recording copy-2 (blit `src.image`→swapchain image, wait `glToVkSem`). Teal-triangle path stays for T0/self-test.
- **It uses the standalone shared engine device (T2), it does NOT own the device.** In v1 the present object owned instance/device/swapchain; in v2 the device/instance come from the `VulkanContext` vehicle and only the surface/swapchain/window/per-frame sync stay object-local. This is the durability fix (point 2).
- `init(spvDir, hidden=true, w, h)` — **hidden window for P13** (`hidden` param already exists), sized to the GL drawable.

---

## 3. THE INTEROP MECHANISM (NOT zero-copy — 2 copies/frame) — point 3

### 3.1 Mechanism
The shared final-frame color is an app-created **exportable** `VkImageResource` (raw dedicated `vkAllocateMemory` + `VkExportMemoryAllocateInfo`, `vk_image_resource.h:72-75`), imported to GL as a GL texture via `GlInteropView` (LUID-matched). Per frame:
- **copy 1 (GL):** FBO-0 → this shared GL texture (`glBlitFramebuffer`/`glCopyImageSubData`). *Required because FBO-0 (the window default framebuffer) CANNOT be `GL_EXT_memory_object`-backed — external memory objects only back app-created textures/renderbuffers.*
- signal `glToVkSem` (P1.5 GPU semaphore, no CPU stall).
- **copy 2 (VK):** `vkCmdBlitImage` shared image → swapchain image, waiting `glToVkSem`.

Sync via the double-buffered `glToVkSem[2]`/`vkToGlSem[2]`/`frameFence[2]` slots (`vk_image_resource.h:44-53`). The *transport primitive* is the proven P1–P3 seam; the *frame boundary* is a real 2-copy path, not zero-copy.

### 3.2 Format + source (HUD/pillarbox correctness)
- **Source = FBO-0 AFTER HUD** (RGBA8, the window backbuffer format). **sceneFBO_ is NOT viable** — it is RGBA16F and HUD-free (HUD/ImGui draw into FBO-0 after the composite; the oracle at `gameosmain.cpp:1652-1673` reads sceneFBO_ *because* it's HUD-free). Using sceneFBO_ would present a frame with no HUD.
- **Sized to the backbuffer/drawable extent INCLUDING the FORCE-43 pillarbox** (`gos_postprocess.cpp:3845+`), not the scene resolution. The presented VK image is then byte-equal to what `SDL_GL_SwapWindow` would have shown.
- Shared image format = **RGBA8**, matching FBO-0; copy-2 to a `B8G8R8A8` swapchain is format-compatible `vkCmdBlitImage`. No new format risk (this is the proven G1 RGBA8 transport; contrast P4 R32F / P5 depth).

---

## 4. TOOLING-FIRST — T0 headless harness (BUILD BEFORE ENGINE WIRING)

Tooling-Ledger row "present/window: headless swapchain/present + fallback smoke" is ❌ "build at P13 checkpoint" (roadmap `:220`). **T0 first, gates the rest.**

**`tools/vk_present_smoke/` (headless, seconds), modeled on `tools/vk_gl_interop_spike/`:**
1. **Shared-device + hidden-VK-window bring-up:** stand up the `VulkanContext` engine device once; create a hidden `SDL_WINDOW_VULKAN` + surface bound to it; assert both live. (Note: P13 does NOT need two *visible* windows — the R1 two-window rehearsal moves to P13b's harness. T0 here proves the shared-device→present-object wiring.)
2. **2-copy present:** GL renders a known pattern into an exportable `VkImageResource` (reuse spike transport), signals `glToVkSem`; VK acquires a swapchain image on the hidden window, blits (copy-2), presents. **K frames** (K≥8, both parity slots + fence reuse). Assert `present_frames==K`, `validation_errors==0`.
3. **Resize:** `handleResize(w2,h2)` mid-run; assert `resize_count` bumped, validation-clean.
4. **Fallback:** `MC2_VK_FORCE_FAIL` → `init()` false, `fallback_reason` non-empty, no crash, GL-fallback stub reachable.
5. **Present-mode/pacing sanity (point 6):** run in the chosen present mode (§6/R8), assert steady present cadence over K frames (no busy-spin / no unbounded queue growth).
6. **Validation-clean** under `MC2_VULKAN_VALIDATION=sync`.

Build variant `build64_island` / `-DMC2_VULKAN_ISLAND=ON`. CLI-only. Reused as the regression oracle for every later present change; flip the Ledger row ✅ on pass.

---

## 5. GATE + PROOF

**Gate:** `MC2_RENDER_BACKEND` ∈ {`gl`,`vulkan`}, default `gl`. Register + allowlist + document (`docs/tier1_env_vars.md` + VK arc gate list); pass `check-env-registry.sh`/`gate_status`.

**Proof matrix:**
1. **Default / `=gl` = byte-identical.** `cmp` the `=gl`/unset exe vs pre-P13 baseline on edge bookmarks (`tests/visual/bookmarks/*.json`). The selector adds one memoized env read + one present-site branch; the extra GL copy lives **only in the `vulkan` branch**, so `=gl` is untouched. Harness-masked `cmp`.
2. **T0 harness green** (§4) — primary offline iteration oracle, seconds.
3. **`=vulkan` presents a real mission frame to the hidden VK window:** launch `mc2_01` `=vulkan`; assert the VK swapchain presented a real, HUD-present frame (verify via the present object's readback of its swapchain image, compared to the GL FBO-0 capture — they should match since it's the same FBO-0 bytes). Oracle = **present-ownership + HUD-present + falls-back**, not a strict per-pixel gate (per-pixel match is a bonus check). `MC2_VULKAN_VALIDATION=sync` clean.
4. **Fallback works:** `=vulkan` + `MC2_VK_FORCE_FAIL=1` (or missing interop exts) → game runs on GL, logs reason, no crash.
5. **Pacing/present-mode (point 6):** confirm frame cadence under `=vulkan` matches the GL swap-interval intent — no stutter, tearing, or uncapped-FPS spin. A per-pixel oracle will NOT catch this, so T5 adds an explicit frame-time/cadence check (§6/R8): measure present interval over N frames vs the GL `MC2_VSYNC` setting; assert equivalent.
6. **R2 interlock proof:** launch `=vulkan` with a postprocess-interop island gate ON; assert the interlock fired (islands coerced OFF + logged, OR clean GL fallback), no dispatch-corruption crash.

**STOP-gate:** any parity failure on (1), crash on (3)/(4)/(6), T0 red, or pacing regression on (5) → `MC2_RENDER_BACKEND` stays default-`gl`, `vulkan` not shipped, diagnose don't paper over.

---

## 6. RISKS (rated)

| # | Risk | Rating | Mitigation |
|---|---|---|---|
| R2 | **volk process-global device-dispatch collision.** `volkLoadDevice` → PROCESS-GLOBAL table; each postprocess island loads its own device (edge-fog live in `endScene`, `gos_postprocess.cpp:3798`; recon `.claude/VK-NEXT-MOVE-RECON.md:28-42`). Present device + a live island in the same frame → dispatch points at whichever loaded last → wrong function pointers. | **HIGH (biggest in P13)** | **HARD-ENFORCED INTERLOCK, not advice (point 5):** T4 — if `MC2_RENDER_BACKEND=vulkan` AND any postprocess-interop island gate is on, **force those islands OFF (logged coercion) OR fail-soft to GL**; never ship on "supervised advice". Durable fix: P13's `VulkanContext` is the ONE engine device; when islands are re-parented onto it (P6), the collision is designed out. |
| R8 | **Present-mode / vsync / frame-pacing ownership shift (point 6).** Today `SDL_GL_SwapWindow` + `SDL_GL_SetSwapInterval` (`gos_render.cpp:376-378`, `MC2_VSYNC`) own pacing. The VK swapchain present mode now owns it. | **MED** | **Chosen present mode = FIFO** (vsync-equivalent, matches `MC2_VSYNC=1`); when `MC2_VSYNC` unset/0 (today's default = uncapped), use **MAILBOX if available else IMMEDIATE** to preserve "not rounded to 30/20/15". Map `MC2_VSYNC` → present mode explicitly. T0 step 5 + T5 (5) assert cadence equivalence. |
| R7 | **2-copy whole-frame present tax (point 3/9).** Copy-1 (GL FBO-0→shared) + copy-2 (VK blit→swapchain), both full-screen, every frame, + interop signal round-trip. NOT trivial (v1 undercounted). | **LOW–MED** | Accepted migration-phase cost (point 9); measured via Tracy at the present site + T5 pacing check; retired by P14 (VK owns render, GL frame gone). One full-screen copy each side is cheap vs the scene, but it is 2 copies, not 0. |
| R6 | **LUID / adapter mismatch** (GL vs VK GPU on hybrid). | **LOW–MED** | LUID match in the interop primitives (edge-fog:546-551, `GlInteropView`); fail-soft to GL on mismatch. |
| R5 | **Resize / drawable-size drift GL window ↔ VK swapchain.** | **MED** | `handleResize` (`:98`); T0 step 3; scale in `vkCmdBlitImage` or rebuild shared image on size change. |
| R3 | **GL passes assuming FBO-0.** | **NONE in P13** | A′ + copy-from-FBO-0 does not redirect any GL pass; the composite/HUD still target FBO-0 exactly as today. |
| R1 | **SDL two-window GL+VK coexistence + input/focus/mouse-grab.** | **HIGH — DEFERRED to P13b** | Not in P13 (hidden VK window, GL keeps input + visibility). See §7b: mouse-grab is on the GL window (`gos_render.cpp:343`); input-forwarding is real work, NOT just `SDL_HideWindow`. |

---

## 7. TASK BREAKDOWN — P13 (rescoped)

T0 first, gates the rest. `=gl` byte-identical throughout (extra copy is vulkan-branch-only).

| Task | What | Size | Supervised? |
|---|---|---|---|
| **T0** | `tools/vk_present_smoke/` headless harness (§4): shared-device + hidden-VK-window bring-up, 2-copy blit-present K frames, resize, forced-fail fallback, pacing sanity, validation-clean. Flip Ledger row ✅. **Before any engine edit.** | M | No |
| **T1** | `RenderBackend` enum + memoized `gos_selectedBackend()` env read at/above `gos_render.cpp:306`; inert on `gl`. Register+allowlist+document `MC2_RENDER_BACKEND`. Prove `=gl`/unset byte-identical (§5.1). | S | No |
| **T2** | **Birth the standalone shared engine VK device** as a first-class `VulkanContext` / `getOrCreateEngineDevice()` vehicle: owns instance + physical-device + device + VMA + volk device-table + descriptor pool. **ACCEPTANCE CRITERION (point 2): the device is NOT owned by `VulkanPresentObject`; the present object (and future P6/P8/P9/P14 consumers) receive it by reference. A grep for the device handle must show it living in the shared context, not a present-object member.** | L | **YES** (durable foundation; device unification) |
| **T3** | Present intercept at `gameosmain.cpp:1720` in the explicit order (§2.2): [FBO-0→shared exportable RGBA8 GL copy, backbuffer-sized incl. pillarbox] → [glInteropSignalToVk] → [`VulkanPresentObject::acquireBlitPresent(src)` on the **hidden** VK window]. Present object uses the T2 device. Reuse P1–P3 transport. | L | **YES** (first whole-frame interop + present intercept) |
| **T4** | Fail-soft + **R2 hard interlock** (point 5): any VK init/LUID/interop failure → session-sticky `s_forceGL`, log, revert to `swap_window`, no crash. AND: `=vulkan` + any live postprocess-interop island gate → force islands OFF (logged) or fail-soft to GL. `MC2_VK_FORCE_FAIL` proof knob. Present-mode mapping from `MC2_VSYNC` (§6/R8). | M | No |
| **T5** | Game confirmation (§5.3–5.6): `mc2_01` `=vulkan` presents real HUD-present mission frame to hidden VK window (readback-compare), validation-clean; forced-fail → GL fallback; **pacing/present-mode equivalence check** (point 6); R2-interlock proof; `=gl` unchanged. | M | No |

**Deliverable framing after P13:** "VK present backend proven end-to-end + armed behind `MC2_RENDER_BACKEND`; shared engine VK device live; display swap is gated follow-on **P13b**." (Point 8.)

---

## 7b. P13b — visible-window swap (DEFERRED follow-on, gated)

Makes the VK window the user's display. Carries the R1 HIGH risk. **Not in P13.**
- Un-hide the VK window; hide/move-offscreen the GL window (only AFTER VK present proven up — the point-7 ordering rule).
- **Input forwarding across two windows — NOT just `SDL_HideWindow`:** mouse-grab is set on the GL window (`set_mouse_grab(true)`, `gos_render.cpp:343`); SDL events (mouse/keyboard/resize/focus/alt-tab) the game logic reads must be routed from whichever window is visible/focused to the game's expected window. This is real work: re-target grab to the VK window, or run an event-forwarding shim, and reconcile `g_sdl_window` (`gos_render.cpp:342`) usage.
- P13b harness step: the two-*visible*-window bring-up + input/focus/grab rehearsal (the R1 rehearsal that P13's T0 skips).
- Fail-soft ordering (point 7): never deviate the GL window until VK present is proven up in the session.
- Only after P13b does the honest claim become "Vulkan is a selectable *display* backend."

---

## 8. Citations
- Window creation (GL-exclusive) + GL init + mouse-grab: `gos_render.cpp:305-335` (create), `:342-343` (`g_sdl_window`, `set_mouse_grab`), `:357-389` (GL context), `:376-378` (`MC2_VSYNC`/swap-interval).
- GL present: `swap_window`→`SDL_GL_SwapWindow` `gos_render.cpp:349-354`; call site `gameosmain.cpp:1710-1720`; `draw_screen` `gameosmain.cpp:1621`; renderer end (NOT present) `gameos_graphics.cpp:7888` / called `gameosmain.cpp:577`.
- Final composite to FBO-0 + FORCE-43 pillarbox + backend-iface seam: `gos_postprocess.cpp:3830-3844` (bind FBO-0), `:3845+` (pillarbox).
- sceneFBO_ RGBA16F + HUD-free (why NOT the blit source): `gos_postprocess.cpp:890-918,965,1030`; oracle reads sceneFBO_ not FBO-0: `gameosmain.cpp:1652-1673`.
- VulkanPresentObject (own window/swapchain, teal triangle, resize, fail-soft, `hidden` param): `vulkan_present_object.h:1-149` (esp. `:8-12,87,89-98,103-105`).
- Interop primitives (exportable, glToVkSem/vkToGlSem/frameFence): `vk_image_resource.h:26-75`.
- volk global-dispatch / per-island device finding: `.claude/VK-NEXT-MOVE-RECON.md:26-68,99-104,122-123`.
- Roadmap P13 + resequence + Tooling Ledger: `.claude/VK-ARC-ROADMAP.md:96-111,150-165,210-221`.
- Worktree HEAD `08215704`, branch `claude/vk-interop-bridge-1`.

---

## v2 CHANGELOG (what changed from v1, per adversarial + meta review)
1. **Rescoped:** P13 = R1-free machinery {T0,T1,T2,T3-to-hidden-window,T4+R2-interlock,T5}; visible-window swap split OUT to **P13b** (§7b). Headline reframed to "birth shared engine VK device + selector, prove VK present end-to-end (hidden window)".
2. **Durable device:** T2 now births a first-class standalone `VulkanContext`/`getOrCreateEngineDevice()` shared context with an explicit **acceptance criterion** — device NOT owned by `VulkanPresentObject`; reused by P6/P8/P9/P14.
3. **Struck every "zero-copy" claim.** Mechanism corrected to **2 copies/frame** (FBO-0→shared GL copy + VK blit→swapchain); FBO-0 cannot be `GL_EXT_memory_object`-backed; source sized to backbuffer incl. FORCE-43 pillarbox, AFTER HUD; sceneFBO_ ruled out (HUD-free).
4. **Explicit present-intercept order** at `gameosmain.cpp:1720`: [FBO-0→shared GL copy] → [glInteropSignalToVk] → [VK acquire/blit/present], all after HUD (§2.2).
5. **R2 = hard-enforced interlock** in T4 (force islands OFF or fail-soft to GL), not "supervised advice".
6. **Added present-mode/vsync/frame-pacing** (R8): FIFO for `MC2_VSYNC=1`, MAILBOX/IMMEDIATE for uncapped; pacing checks in T0 + T5.
7. **Fail-soft ordering** kept: never deviate GL until VK present proven up (moot in P13, binding in P13b).
8. **Honest framing** downgraded: "VK present proven + armed behind selector; display swap = gated P13b" — not "selectable display backend".
9. **Per-frame present-tax acknowledgement** added (2 copies + interop round-trip until P14 retires the GL frame).
10. R7 re-rated (2 copies, not "trivial"); R1 marked DEFERRED-to-P13b with the mouse-grab/input-forwarding detail.
