# VK-P13b-VISIBLE-RECON — sizing the R1 risk for "make the VK-presented frame VISIBLE"

**Worktree:** `A:/Games/mc2-vk-interop-bridge` · **branch** `claude/vk-interop-bridge-1` · **HEAD** `3e737f56`
**Scope of P13b:** P13 shipped — `MC2_RENDER_BACKEND=vulkan` presents the real game frame end-to-end to a **HIDDEN** `SDL_WINDOW_VULKAN` (`engine_present_vk.cpp:346-348`). The user sees nothing. P13b makes it user-visible and playable. The plan flagged this R1-HIGH and deferred it (`VK-P13-SELECTOR-PLAN.md:9,138,159-166`).
**Read-only w.r.t. game source.** Citations are `file:line`.

---

## ★ VERDICT (read first)

**P13b is TRACTABLE and closer to LOW–MED than R1-HARD — because this codebase's input is almost entirely window-agnostic, and because approach (b) the same-HWND raw-Win32-surface trick is *very likely to work*.** The plan's R1-HIGH rating was written assuming classic "two-window SDL input forwarding" surgery. That surgery is largely **unnecessary here**:

1. **Input is NOT read through SDL per-window event state on Windows.** Buttons come from `GetAsyncKeyState` (`gos_input.cpp:117-128`), keyboard modifiers from `GetAsyncKeyState` (`gos_input.cpp:225`), keyboard state from global `SDL_GetKeyboardState` (`gos_input.cpp:182`). These are process/OS-global and do NOT care which window is focused. The ONLY genuinely window-relative input is **mouse position**, which flows from `SDL_MOUSEMOTION` events (`gameosmain.cpp:482-483` → `input::handleMouseMotion` → `mi->x_/y_`, read in `gameos_input.cpp:42-63`). So "input forwarding" collapses to "get mouse-motion coordinates relative to the visible window" — one surface, not the whole input stack.

2. **The same-HWND trick (approach b) most likely ELIMINATES the two-window problem.** A raw `vkCreateWin32SurfaceKHR` on the GL window's existing HWND is technically permissible — Win32 HWNDs are not API-locked, and VK presentation via the swapchain does not touch the GL pixel format / HDC that WGL owns (see §3). If it works, there is **one window, already visible, already owning all input** — no second window, no input forwarding, no focus/grab retarget. P13b becomes: swap the surface-creation call, keep everything else. **This is the recommended path and it is a quick win if the ~1-hour offline probe (§6) passes.**

3. **Even the fallback (approach a: show VK window / hide GL window / retarget) is MED, not HIGH**, precisely because of finding 1 — the only real work is re-pointing `g_sdl_window` (`gos_render.cpp:20,342`) + mouse-grab (`gos_render.cpp:107-116`) + mouse-motion at the VK window and pumping its events in the main loop instead of the drain-and-discard at `engine_present_vk.cpp:423`.

**Bottom line:** Not a big supervised piece. Recommend: run the same-HWND offline probe FIRST (§6, ~1 hr). If green → P13b is a small slice (§5, approach b). If the probe fails → approach (a), a MED slice, still no wholesale input-stack rewrite. The R1-HIGH label should be **downgraded** in the roadmap.

---

## 1. WINDOW / INPUT / SIZE / FOCUS COUPLING — the surface P13b must redirect

The game's coupling to the GL window handle is far thinner than the plan assumed. Full inventory:

| Concern | Site | Reads the GL window? | P13b impact |
|---|---|---|---|
| **Mouse buttons** | `gos_input.cpp:117-128` (`GetAsyncKeyState(VK_LBUTTON…)`) | **NO** — Win32 global | none |
| **Keyboard keys** | `gos_input.cpp:182` (`SDL_GetKeyboardState`) | **NO** — process-global scancode array | none |
| **Keyboard modifiers** | `gos_input.cpp:225-237` (`GetAsyncKeyState`) | **NO** — Win32 global | none |
| **Mouse POSITION** | `SDL_MOUSEMOTION` → `handleMouseMotion` → `mi->x_/y_`; read `gameos_input.cpp:42-63` | **YES (window-relative)** | ★ the one real coupling — must arrive relative to the *visible* window |
| **Mouse warp / set-pos** | `gameos_input.cpp:120-144` (`SDL_WarpMouseInWindow(g_sdl_window,…)`) | **YES** (`g_sdl_window`) | retarget to visible window |
| **Mouse grab (clip cursor)** | `gos_render.cpp:107-116` → `apply_mouse_grab(g_sdl_window,…)`; Win32 `ClipCursor` from that window's HWND `gos_render.cpp:62-103` | **YES** (`g_sdl_window` HWND) | retarget HWND to visible window |
| **Grab re-assert on focus/resize/move** | `gameosmain.cpp:458,463,472,477` (`set_mouse_grab`/`refresh_mouse_grab`) | via `g_sdl_window` | follows `g_sdl_window` retarget |
| **SDL event loop** | `gameosmain.cpp:415-493` (`process_events`, global `SDL_PollEvent`) | **NO** — polls the process event queue, not a window id | pumps *all* windows; no per-id filter |
| **Focus tracking / throttle** | `gameosmain.cpp:1308-1311` (`SDL_GetWindowFlags(g_sdl_window)` for MINIMIZED/HIDDEN) | **YES** (`g_sdl_window`) | ★ if GL window is hidden, this reads HIDDEN → throttles the loop to 10 ms sleeps (`:1314`). Must point at the visible window. |
| **Drawable size (viewport, mouse normalize)** | `gos_render.cpp:433-434,697` (`SDL_GL_GetDrawableSize`/`SDL_GetWindowSize` on `rw->window_`) | **YES** (GL window) | fine to keep reading GL window if it still exists (approach a); in approach b it IS the visible window |
| **Present / swap** | `gos_render.cpp:349-354` (`SDL_GL_SwapWindow`) vs the P13 VK intercept | GL window | unchanged path selection |
| **`g_sdl_window` global** | declared `gos_render.cpp:20`, set `:342`, consumed by all of the above + `gameosmain.cpp:233`, `gameos_input.cpp:119`, `gos_input.cpp` | — | **the single redirection lever for approach (a)** |

**Key structural facts:**
- The SDL event loop (`gameosmain.cpp:420`) is **global** — `SDL_PollEvent` drains the process queue regardless of window. Events for a second (VK) window arrive here automatically; there is no window-id filter to change. `SDL_MOUSEMOTION.motion` carries `windowID` and window-relative x/y, so if the VK window is the one the OS routes motion to, coordinates are already correct.
- The P13 present path currently **steals** the VK window's events with a drain-and-discard (`engine_present_vk.cpp:423`, `while(SDL_PollEvent(&ev)){}`). For P13b this drain **must be removed** or the main loop will never see the VK window's mouse-motion. (In approach b there is no VK window, so this is moot — delete it.)
- On Windows, buttons+keys are read from OS-global state, so a focused-but-"wrong" SDL window still yields correct clicks/keys. This is why the classic SDL two-window input-forwarding nightmare **does not apply** to MC2.

---

## 2. THE THREE APPROACHES

### (a) Show VK window + hide GL window + retarget input to the VK window
Un-hide the VK window (`engine_present_vk.cpp:348` drops `SDL_WINDOW_HIDDEN`, or call `SDL_ShowWindow`), hide/offscreen the GL window, and re-point the input coupling at the VK window. Concretely:
- Repoint `g_sdl_window` (`gos_render.cpp:342`) to the VK `SDL_Window*` after present init, so grab/warp/focus-flags all follow (`gos_render.cpp:107-116`, `gameos_input.cpp:136`, `gameosmain.cpp:1308`).
- Remove the event drain at `engine_present_vk.cpp:423` so the main loop's `process_events` sees VK-window motion.
- Keep GL context + GL window alive but hidden (GL still renders into FBO-0; only presentation moved). **The GL window being HIDDEN trips the throttle at `gameosmain.cpp:1310` — must switch that flag-read to the VK window** (follows the `g_sdl_window` retarget for free).
- **Risk:** MED. Two SDL windows coexisting (GL + VK) is the R1 the plan feared, but the input half is nearly free here. Real residual work = the `g_sdl_window` retarget + throttle-window fix + mouse-motion routing + alt-tab/focus behaviour with a hidden GL window still in the process.

### (b) ★ Same HWND — raw `vkCreateWin32SurfaceKHR` on the GL window's HWND (NO second window)
Do **not** create any VK window. Pull the existing GL window's HWND via `SDL_GetWindowWMInfo` (already used at `gos_render.cpp:64-72` for the grab) and build the VK surface with a raw `vkCreateWin32SurfaceKHR(hinstance, hwnd)` instead of `SDL_Vulkan_CreateSurface` (`engine_present_vk.cpp:355`). The swapchain then presents to the **same visible window that GL owns and that already has all input/focus/grab**. Nothing about input, focus, grab, `g_sdl_window`, or the event loop changes. See §3 for the feasibility ruling — **this is the recommended path.**

### (c) Recreate the single window as `SDL_WINDOW_VULKAN` at init + a hidden GL-context window — REJECTED (re-confirmed fatal)
GameOS init calls `SDL_GL_CreateContext(rw->window_)` on the handed window **immediately** (`gos_render.cpp:362`), and `SDL_GL_MakeCurrent` (`:368`). `SDL_GL_CreateContext` requires the window to have been created with `SDL_WINDOW_OPENGL` (`gos_render.cpp:307`). A `SDL_WINDOW_VULKAN` window has no GL-capable pixel format → `SDL_GL_CreateContext` fails → the entire GL init cascade (`gos_render.cpp:357-389`, GPU identity, all downstream GL) dies. The plan's rejection stands (`VK-P13-SELECTOR-PLAN.md:21,37`). Splitting into a hidden GL-context window + visible VK window is just approach (a) with the visibility inverted, and buys nothing over (a). **Reject.**

---

## 3. ★ FEASIBILITY RULING ON (b) — same-HWND raw-Win32 VK surface

**Ruling: LIKELY WORKS. This is the highest-leverage move and should be probed first.**

Why it is permissible:
- **A Win32 window is not API-locked.** `SDL_WINDOW_OPENGL` only tells SDL to choose a GL-capable pixel format for the HWND and lets WGL create a context on its HDC. It does not forbid other GPU APIs from targeting the same HWND. `vkCreateWin32SurfaceKHR` takes a bare `HWND` + `HINSTANCE` and knows nothing about SDL's window flags — SDL's `SDL_WINDOW_VULKAN` requirement is an **SDL-side** guard on `SDL_Vulkan_CreateSurface`, not a Win32 or Vulkan constraint. Bypassing SDL's wrapper with the raw entry point sidesteps that guard entirely.
- **GL and VK own different things on the HWND.** WGL owns the **pixel format + HDC + GL context**. The VK swapchain presents via the **DWM/WSI path** (`IDXGI`/`vkQueuePresentKHR`), which composites the swapchain images to the HWND surface. Presentation is a WSI/compositor operation, not a GDI/HDC draw, so it does not conflict with the GL pixel format that WGL set. In practice, VK-on-a-GL-HWND is a known-workable pattern on Windows (the reverse — GL interop presenting via a VK swapchain on a GL-created window — is exactly what interop demos do).

Residual risks (all LOW, all caught by the §6 probe):
- **R-b1 — swapchain image-count / present-mode interaction with an already-composed HWND.** When GL is *also* swapping to the same HWND you'd get a fight; but in P13b **GL stops presenting** (the P13 intercept already replaces `SDL_GL_SwapWindow` — `VK-P13-SELECTOR-PLAN.md:57-65`), so only VK presents. GL renders into FBO-0 (an offscreen/backbuffer target) and never swaps. No double-present. **Confirm GL never calls `SDL_GL_SwapWindow` in `=vulkan` mode** (already true per the P13 branch at `gameosmain.cpp:1720`).
- **R-b2 — `SDL_GL_SwapWindow` implicitly touches the HWND even when we skip it.** We don't call it in VK mode, so N/A.
- **R-b3 — surface loss on the shared HWND.** Standard OUT_OF_DATE/resize handling already exists (`engine_present_vk.cpp:477-483,584-588`); the same-HWND surface uses the identical rebuild path.
- **R-b4 — HINSTANCE retrieval.** `SDL_GetWindowWMInfo` gives `info.win.hinstance` + `info.win.window` (already pulled at `gos_render.cpp:64-74`); trivial.

**If (b) works, P13b is: (1) add a `vkCreateWin32SurfaceKHR`-on-GL-HWND surface-create branch in present init, (2) create the swapchain sized to the GL drawable, (3) delete the hidden-window creation + the event drain, (4) do NOT hide the GL window. No input/focus/grab code changes at all.** That is a genuinely small slice.

---

## 4. RANKING + RECOMMENDATION

| Rank | Approach | Effort | Risk | Notes |
|---|---|---|---|---|
| **1 (recommended)** | **(b) same-HWND raw Win32 surface** | **S** | **LOW** (pending §6 probe) | one window, zero input work; probe first |
| 2 (fallback) | (a) show VK / hide GL / retarget `g_sdl_window` | M | MED | only if (b)'s probe fails; input work is small thanks to §1 |
| — (rejected) | (c) recreate window as VULKAN + hidden GL | — | fatal | `SDL_GL_CreateContext` needs `SDL_WINDOW_OPENGL` (`gos_render.cpp:362,307`) |

**Recommendation:** Run the §6 offline probe for (b). Green → ship (b). Red → ship (a).

### Task breakdown

**Path (b) — same-HWND (recommended):**
- **P13b-b-T0 (offline probe, §6):** standalone: create a `SDL_WINDOW_OPENGL` window + GL context, then `vkCreateWin32SurfaceKHR` on its HWND, build a swapchain, present K color-cycling frames while GL renders (but does NOT swap). Assert present succeeds, validation-clean, window visibly shows the VK frames. **Gate for the rest.**
- **P13b-b-T1:** in `EnginePresentVk::init`, add a surface-create branch: when a "use existing HWND" flag/gate is set, pull HWND+HINSTANCE from the GL window's `SDL_GetWindowWMInfo` and call `vkCreateWin32SurfaceKHR` instead of creating the hidden window (`engine_present_vk.cpp:340-357`). Pass the GL `SDL_Window*` into `init`.
- **P13b-b-T2:** delete the event-drain (`engine_present_vk.cpp:423`) for this path; do not hide the GL window; ensure GL stays non-swapping in `=vulkan`.
- **P13b-b-T3:** resize — feed `SDL_GL_GetDrawableSize(glWindow)` into `presentGLFrame` (already the resize input, `:414-440`); no `SDL_SetWindowSize` on a hidden window needed.
- **P13b-b-T4:** proof run (oracle below).

**Path (a) — two-window fallback:**
- P13b-a-T1: un-hide VK window + hide/offscreen GL window (ordering rule `VK-P13-SELECTOR-PLAN.md:165`: only after VK present proven up in-session).
- P13b-a-T2: retarget `g_sdl_window` → VK window post-init (drives grab/warp/focus-flags/throttle for free).
- P13b-a-T3: remove the VK-window event drain (`engine_present_vk.cpp:423`) so `process_events` sees VK motion.
- P13b-a-T4: verify mouse-motion `windowID`/coords land relative to the VK window; adjust the drawable-normalize denominators (`gameos_input.cpp:42-43`) to the VK window's drawable if it differs.
- P13b-a-T5: alt-tab/focus/minimize behaviour with a live-but-hidden GL window; proof run.

### Oracle (visible run — both paths)
1. `mc2_01 =vulkan`: the game window is **visibly showing the real HUD-present mission frame** (not a hidden window, not the GL window).
2. **Input works:** mouse moves the cursor and picks units correctly (mouse-motion coords land in the right window); WASD/keys and mouse-buttons drive the game (already global-sourced, should be free).
3. **Focus/alt-tab:** alt-tab away and back — grab releases/re-asserts (`gameosmain.cpp:472-478`), loop does not wedge, no throttle-lock (`gameosmain.cpp:1308-1314` reads the *visible* window).
4. **Resize:** resize the window — swapchain + shared image rebuild (`engine_present_vk.cpp:427-440`), frame stays correct, no validation errors.
5. **GL fallback intact:** `=gl`/unset unchanged and byte-identical; `=vulkan`+`MC2_VK_FORCE_FAIL` reverts to a visible GL window (`s_forceGL`).
6. `MC2_VULKAN_VALIDATION=sync` clean across the run.

---

## 5. OFFLINE / HEADLESS DE-RISK BEFORE A VISIBLE GAME RUN

- **★ Same-HWND probe (the decisive one, ~1 hr, path b):** a standalone tool (model on `tools/vk_present_smoke/`) that creates a real `SDL_WINDOW_OPENGL` + GL context, then `vkCreateWin32SurfaceKHR` on that HWND, builds a swapchain, and presents K frames of a color cycle while GL renders-without-swapping. Pass = present succeeds + validation-clean + the window visibly cycles color. This single test rules (b) IN or OUT and decides whether P13b is a quick win. It is *not* fully headless (needs a visible window to confirm present), but it is standalone (no game, seconds to build/run).
- **Input-agnosticism assertion (static, path a de-risk):** a grep-backed check (already done in §1) that no game input path other than mouse-motion/warp/grab reads a specific SDL window — confirms approach (a)'s input work is bounded. Encode as a `check-*.sh` guard so a future edit that adds a per-window input read trips it.
- **Two-window event-routing probe (path a only):** extend `tools/vk_present_smoke/` to create BOTH a GL and a VK window, and assert `SDL_MOUSEMOTION.window` id routing + that a single global `SDL_PollEvent` loop drains both — the R1 rehearsal the P13 T0 skipped (`VK-P13-SELECTOR-PLAN.md:164`).
- **Throttle-flag check:** assert `SDL_GetWindowFlags` on whichever window is designated visible does NOT report HIDDEN/MINIMIZED in the steady state (guards the `gameosmain.cpp:1310` throttle-lock trap).

---

## Citations
- P13 present object (hidden window, event drain, resize, fail-soft): `engine_present_vk.cpp:346-357,414-440,423,477-483,584-588`; `engine_present_vk.h:74-88`.
- GL window creation (`SDL_WINDOW_OPENGL`), grab, `g_sdl_window`: `gos_render.cpp:305-307,313,337-345`.
- GL context requires the GL window (kills approach c): `gos_render.cpp:357-389` (esp. `:362,368`).
- Mouse grab / ClipCursor from `g_sdl_window` HWND (+ `SDL_GetWindowWMInfo` HWND/HINSTANCE source for approach b): `gos_render.cpp:62-103,107-116`.
- SDL event loop is global (no window filter) + focus/grab handling: `gameosmain.cpp:415-493`.
- Loop throttle reads `g_sdl_window` HIDDEN/MINIMIZED flags: `gameosmain.cpp:1308-1314`.
- Input is Win32/global, not per-window (buttons/keys/mods): `gos_input.cpp:117-128,182,225-237`.
- The ONE window-relative input (mouse position via motion events + warp): `gameosmain.cpp:482-483`; `gameos_input.cpp:33-63,120-144`.
- Drawable-size queries on the GL window: `gos_render.cpp:433-434,697`.
- Present intercept + swap site: `gos_render.cpp:349-354`; plan `VK-P13-SELECTOR-PLAN.md:57-65,159-166`.
