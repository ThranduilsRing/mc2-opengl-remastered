# vk_present_smoke — headless VK swapchain/present + fallback smoke (VK-P13 T0)

Standalone, headless proof that the **VK present-of-a-shared-image machinery**
works end-to-end in seconds, **without the game**. This is task **T0** of
`.claude/VK-P13-SELECTOR-PLAN.md` — the tooling-first gate that flips the
Tooling-Ledger row `present/window: headless swapchain/present + fallback smoke`
from ❌ to ✅ **before any engine wiring**.

## What it proves

Per run it:

1. Stands up a VK instance + device + a **hidden** `SDL_WINDOW_VULKAN` + surface
   (headless — no user-visible window).
2. Creates a swapchain on that hidden window (**FIFO** present mode — vsync
   equivalent; logs whether MAILBOX / IMMEDIATE are also available, per plan R8).
3. Creates a source `VkImage` — the **"GL frame" stand-in** — and fills it each
   frame with a deterministic rolling color (`vkCmdClearColorImage`).
4. Per frame: **`vkCmdBlitImage`** source → acquired swapchain image → transition
   to `PRESENT_SRC` → **`vkQueuePresentKHR`**. Asserts present succeeds every frame.
   `K = 32` frames (≥ 8: exercises both per-image semaphore parity slots + fence
   reuse; the L5 one-render-semaphore-**per-swapchain-image** fix is mirrored here).
5. **Resize** mid-run (frame 16): `SDL_SetWindowSize` + swapchain recreate, keeps
   presenting, validation-clean across the recreate. Asserts `resize_count ≥ 1`.
6. **Fallback**: an independent VK bring-up with a forced `vkCreateDevice` failure
   (impossible device extension) — confirms a clean fail verdict + `fallback_reason`,
   **no crash / no hang** → the GL-fallback branch is reachable.
7. **Validation (sync preset)** enabled — counts ERRORs, must be **0**.

Exit `0` = PASS (with a `[VERDICT] PASS …` line); nonzero = FAIL with what failed.
Result saved to `P13-T0-result.log`.

## What it does NOT prove

- **Not** the engine intercept at `gameosmain.cpp:1720` (that's T3).
- **Not** the copy-1 GL FBO-0 → shared-exportable-texture path, nor the GL→VK
  interop semaphore round-trip (T3; a pure-VK-present smoke needs no GL side —
  a VK-filled image blitted to the swapchain is sufficient to prove present+blit).
- **Not** HUD/pillarbox correctness, nor per-pixel parity vs the GL frame (T5).
- The source image here is a plain device-local `VkImage`, **not** the exportable
  interop `VkImageResource` — the transport primitive is proven separately by
  `tools/vk_gl_interop_spike`. This harness proves the **swapchain + blit +
  present + resize + fallback** half.

## Relationship to the engine code

It **mirrors** (does not link) the swapchain/acquire/present/resize logic of
`GameOS/gameos/vulkan_present_object.{h,cpp}` (the L5 present object), including
its per-image render-semaphore discipline. The present object is
volk + vk-bootstrap and engine-coupled (inert unless `MC2_VULKAN`, depends on
`vulkan_backend_skeleton.h` for shader loading, and render-passes a fullscreen
triangle rather than blitting) — so per the plan's guidance the **minimal**
swapchain logic is copied into this standalone TU, swapping the triangle draw for
the `vkCmdBlitImage` P13 actually needs. Deps: **only SDL2 + vulkan-1** (raw
`vulkan.h`), same standalone pattern as `tools/vk_gl_interop_spike`.

## Build & run

From this directory (VS BuildTools cmake path):

```bat
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" -G "Visual Studio 17 2022" -A x64 -B build64
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
```

Run (validation on by default; `sync` preset unless overridden):

```bat
set MC2_VULKAN_VALIDATION=sync
build64\RelWithDebInfo\vk_present_smoke.exe
```

## Env knobs

| Var | Effect |
|---|---|
| `MC2_VULKAN_VALIDATION` | `sync` (default) / `core` / `off`. `sync` catches layout/hazard bugs. |
| `MC2_VK_NEG_TEST` | Negative-test mode (proves the gate has teeth) — see below. |

`MC2_VK_FORCE_FAIL` is the engine-side knob name the fallback path models; here the
fallback sub-test always runs (forces `vkCreateDevice` failure internally) so no
env is needed to exercise it.

## Negative test (the gate has teeth)

`MC2_VK_NEG_TEST` deliberately breaks the present so sync-validation fires; the
harness must then report the break was DETECTED:

- `skipblit`  — skip the blit + the swapchain layout transitions → present an
  `UNDEFINED` image → sync-validation error.
- `badlayout` — blit but never transition to `PRESENT_SRC` (present while still
  `TRANSFER_DST`) → validation error.

```bat
set MC2_VK_NEG_TEST=skipblit
build64\RelWithDebInfo\vk_present_smoke.exe   :: -> "NEGATIVE-TEST PASS: ... DETECTED ... the gate has teeth"
```

With no knob the harness PASSes (val_errors=0); with a knob it detects the break.
That round trip proves the validation/verdict gate is not a rubber stamp.

## How to extend

- **Present modes** (R8): swap `s.presentMode` to MAILBOX/IMMEDIATE (availability
  is already probed + logged) and add a cadence/frame-time assertion for the T5
  pacing check.
- **Real interop source**: replace the plain source `VkImage` with an exportable
  `VkImageResource` (`GameOS/gameos/vk_image_resource.h`) + the GL fill for a
  fuller copy-1→copy-2 rehearsal ahead of T3.
- **Reused as the regression oracle** for every later present change — run it (and
  its negative modes) after touching swapchain/present code.
