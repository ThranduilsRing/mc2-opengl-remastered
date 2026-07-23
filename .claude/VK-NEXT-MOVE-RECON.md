# VK-NEXT-MOVE-RECON — highest-value next MIGRATION piece

**Worktree:** `A:/Games/mc2-vk-interop-bridge` · **branch** `claude/vk-interop-bridge-1`
**HEAD:** `15b30e5a` (P3 SSAO island color I/O on the seam)
**Done on seam:** P1 edge-fog, P1.5 VK→GL semaphore, P2 fog-subgraph, P3 SSAO — all COLOR boundaries, each island on its OWN VkInstance/VkDevice.
**Deferred (optimization / Phase F):** P5 depth zero-copy, P4 HZB (roadmap lines 84–94).

---

## RECOMMENDATION — do **P13 (backend-selector seam) FIRST**, in its minimal form.

Land `MC2_RENDER_BACKEND` as a real top-level init-time selector that defers SDL window
creation and creates `SDL_WINDOW_VULKAN` on the VK path (with a GL fallback), **before** VK
renders a whole frame. This is the single piece that converts the arc from "N proven
regions" into "Vulkan is a *selectable backend* (that currently renders nothing yet, falling
back to GL)". Every later geometry/present piece needs this seam anyway; landing it now
makes the goal reachable incrementally instead of as one XXL present surgery.

**Rank after that:** #2 = **P8 shadow** (smallest real geometry win, but blocked on a shared
device + depth-seam — see below). #3 = **P6 native subgraph growth** (LAST — its real gate,
device unification, is a bigger blocker than the roadmap implies).

---

## The blunt device-unification finding (this reorders the roadmap)

**Every postprocess island independently runs the full Instance→PhysicalDevice→Device→VMA
bring-up and tears it down.** Evidence — identical `vkCreateDevice` per island:

| Island | `vkCreateDevice` site |
|---|---|
| edge-fog | `vulkan_edge_fog_island.cpp:596` |
| oob-fog | `vulkan_oob_fog_island.cpp:469` |
| ssao | `vulkan_ssao_island.cpp:673` |
| subgraph | `vulkan_postprocess_subgraph.cpp:610` |
| hzb | `vulkan_hzb_island.cpp:493` |

Each stores its own `VkDevice device` in its own state struct (edge-fog:316, ssao:373, …)
and calls **`volkLoadDevice(s->device)`** (edge-fog:598) — which resolves into volk's
**process-global** device dispatch table. There is **no** shared/persistent device vehicle
in the skeleton (`grep` for `VulkanContext`/`sharedDevice`/`getOrCreateDevice` in
`vulkan_backend_skeleton.{h,cpp}` → nothing).

**Why it works today:** the islands are never live at the same time. In `endScene` only
edge-fog is dispatched (`gos_postprocess.cpp:3798 runEdgeFogVulkan()`); each island's flow is
create → volkLoadDevice → run → destroy, so the global dispatch is only ever pointed at one
live device.

**Why this blocks P6 (rung-3 chaining):** to chain SSAO→fog sharing ONE VkImage with NO GL
between, both passes must run on the **same VkDevice** in the same frame. With today's
per-island devices + volk-global dispatch, two live devices would clobber each other's
function pointers. So P6 is not "extend the fused subgraph"; it is **first build a shared
postprocess VK device/context** (own instance+device+VMA+descriptor pool, volk device table
managed once) and **re-parent edge-fog, oob-fog, ssao, and the subgraph onto it**. That is a
real refactor of 4 TUs' bring-up/teardown, not a slice. The roadmap (line 98) frames P6 as
"extend the fused subgraph to cover more contiguous passes" — that undersells it.

**Second P6 blocker — contiguity/BoxDecals is real.** Canonical PP order is
Shadow→Cloud→Shoreline→**SSAO→BoxDecals→EdgeFog**→OobFog→Composite. The shipped subgraph
fuses only EdgeFog+OobFog (`vulkan_postprocess_subgraph.cpp:1-2`, "Fuses the two shipped …
EdgeFog + OOB fog"). **BoxDecals sits between SSAO and fog and has NO VK island** (`grep
BoxDecal GameOS/gameos/vulkan_*` → empty; it is GL-only, `gos_postprocess.cpp:1518
drawBoxDecals`). So even after device unification, SSAO→fog is not contiguous: BoxDecals must
either be ported to a VK island first or the chain must skip it (and then it is not the
"whole contiguous region" rung-3 wants). **Net: P6 = device-unification refactor + a
BoxDecals port + the chain. Highest engineering cost of the three, and it advances
"selectable backend" the least** (it makes the PP tail prettier internally; the user still
can't select VK).

---

## Piece-by-piece readiness

### P13 — backend selector at window creation  ★ RECOMMENDED
- **State of the seam:** window is hardwired `SDL_CreateWindow(..., SDL_WINDOW_OPENGL|…)`
  at `GameOS/gameos/gos_render.cpp:306-307`. `MC2_RENDER_BACKEND` (top-level) **does not
  exist** — only per-region `MC2_RENDER_BACKEND_IFACE` / `_REGION_IFACE` routing exists
  (`gos_postprocess.cpp:2733,2839`). So the selector is greenfield but tiny.
- **Present machinery already exists:** `mc2vk::VulkanPresentObject`
  (`vulkan_present_object.{h,cpp}`) owns a real swapchain, presents N frames, handles
  resize/OUT_OF_DATE/SUBOPTIMAL (`vulkan_swapchain_present.cpp:1-23`) — but to a **separate**
  SDL_WINDOW_VULKAN window; full game-window present is explicitly ledgered as blocked on L7
  ownership (`:16-23`). That means the *present object* is proven; only the *game-window
  wiring* is missing.
- **Fail-soft pattern exists** (P15 ~90% done per roadmap:141): every island logs+returns
  false on any VkResult error and the caller falls back to GL — the exact shape P13's
  fallback needs.

### P8 — shadow region (smallest geometry win)  → #2
- **What exists:** `vulkan_shadow_island.cpp` is a pure **pipeline builder**
  (`mc2_vulkan_build_shadow_depth_pipeline(device, renderPass, vert, frag, out)`) — depth-only,
  instanced, set0=view/cascade UBO, set1=instance-ring STORAGE_BUFFER_DYNAMIC (:43-191). It
  takes a device+renderpass from a caller. Read side proved by comparesampler/csmarray
  fixtures (:1-7). **No device, no live backend-compare, no render-pass owner, no geometry
  input feed** yet (sub-slice B is unstarted).
- **Why it's not trivially on the seam:** it is the first **depth** region, and depth
  zero-copy is exactly what P5-T0′ proved is NOT a raw byte-alias on AMD (roadmap:84-94).
  The shadow *producer* is VK though, so it can OWN its depth in D32 and there's no D24S8
  import problem — but it still needs a shared device (same finding as P6) and new
  geometry-input machinery (instance ring feed, caster vertex buffer). Smallest **geometry**
  win of the two, but not smaller than P13.

### P9 — water region
- **Prep only:** WATER-REGISTRY-0 registered the live MDI water buffers with GpuBufferOwner
  (`83da3973`), WATER-MVP-ORACLE-1 proved column-major MVP + reverse-Z (`4d30d93e`). Live
  water GL path is `gameos_graphics.cpp:3181 renderWaterFastPath`. No VK water island exists.
  More machinery than shadow (MDI, blend, reflection). Not the smallest.

### P6 — native subgraph growth
- See device-unification + BoxDecals findings above. Biggest cost, least "selectable"
  progress. Do LAST of the three.

---

## Why P13 is highest-value toward the SELECTABLE-BACKEND goal
1. **It is the definition of the goal.** The user's stated Phase-D exit is "`MC2_RENDER_BACKEND=vulkan`
   … falls back to GL if unavailable." Nothing else on the board moves that bit; P6/P8 make
   VK render *more* while it remains unselectable.
2. **It unblocks every later geometry/present piece.** P8/P9/P14 all need a real
   engine-owned VK device + surface tied to the game window. Today islands each spin their
   own throwaway device (finding above). The selector seam is where the *one persistent
   engine VK device* should be born — the same device unification P6 needs. **Landing P13's
   device/context first pays down the P6 blocker too.**
3. **Cheap and low-risk in minimal form.** The present object already works; the window flag
   and env read are a handful of lines; fallback pattern exists.
4. **Incremental, honest.** Ship it as "selector + VK path creates a VK window and
   *presents a clear color / a GL-blit of the GL frame*, else falls back to GL" — a real
   selectable backend that renders nothing of its own yet, without the 8–14mo present surgery.

## Readiness — proven vs unproven
- **Proven:** VulkanPresentObject swapchain+present+resize (separate window); LUID/device
  selection bias (edge-fog:546-551); fail-soft fallback everywhere; zero-copy shared IMAGE
  transport (P1–P3) — which is the mechanism for the "VK-window shows a GL-blit" MVP.
- **Unproven / greenfield:** deferring `SDL_CreateWindow` until an env read; creating the
  game window as `SDL_WINDOW_VULKAN` without breaking the rest of GameOS init that assumes a
  GL context (`gos_render.cpp` GL context creation immediately after :307); one persistent
  engine VK device (vs per-island throwaway); a shared-image→GL-backbuffer blit (or
  surface-flip) into the REAL game window rather than a probe window.

## Oracle / proof approach
- **Offline first (roadmap standing order):** a headless **swapchain/present + fallback
  smoke** harness is the missing tool for this TYPE (Tooling Ledger line 203 marks it ❌
  "build at P13 checkpoint"). Build it before grinding: assert (a) VK path creates a window +
  presents K frames validation-clean, (b) forced-fail VK init → GL fallback, no crash, logged
  reason, (c) `MC2_RENDER_BACKEND` unset/`gl` → byte-identical to today (cmp gate-OFF exe).
- **Game confirmation:** launch with `MC2_RENDER_BACKEND=vulkan` → VK window presents a
  GL-blit of the mission frame at parity with the GL window (reuse existing edge bookmarks as
  the pixel source); `=gl`/unset → existing golden-parity unchanged. `MC2_VULKAN_VALIDATION=sync`
  clean.

## Main risk
GameOS init after `SDL_CreateWindow` (`gos_render.cpp:307+`) assumes a live GL context
(SDL_GL_CreateContext, GLEW/glad load, all FBO/shader setup). Creating a Vulkan window
*instead* risks a cascade of "no GL context" failures across gameos_graphics init. **Mitigation
for the MVP: keep the GL window + context as-is (renderer still GL), and have the VK path
create an ADDITIONAL VK swapchain that presents a shared-image blit — i.e. selector chooses
the *presenter*, not yet the *renderer*.** That defers the "GL init compiled-out on VK path"
half (the genuinely invasive part) to P14, and keeps P13 minimal + fallback-safe.

## Rough task breakdown (P13 minimal)
1. **T0 (tool):** headless swapchain/present + fallback smoke harness (Tooling Ledger ❌ → ✅).
2. **T1:** `MC2_RENDER_BACKEND` env read at init (`gl` default), plumb a `RenderBackend`
   enum to the window-creation site; unset/`gl` → byte-identical.
3. **T2:** on `vulkan`, keep GL window+context; additionally create the persistent engine VK
   device/context (extract the island bring-up into ONE shared vehicle — reused later by P6/P8).
4. **T3:** VK path presents a **shared-image→backbuffer blit** of the GL frame via
   VulkanPresentObject (reuse P1–P3 zero-copy transport) into a VK swapchain; oracle = pixel
   parity vs GL window on edge bookmarks.
5. **T4:** fail-soft — any VK init/LUID/interop failure → force GL for the session, log
   reason, no crash (extends P15). Register+allowlist+document `MC2_RENDER_BACKEND`.
6. **T5:** game golden-parity final confirmation (`=gl` unchanged; `=vulkan` blit at parity).

**Deferred to P14 (not P13):** GL-init-compiled-out on VK path; VK owning the renderer;
surface-flip native present; HUD/overlay compositing.

---

## What comes after (ranked)
- **#2 P8 shadow** — first real geometry region; smallest geometry machinery (depth-only,
  instanced, ring already scaffolded). Needs the shared engine device from P13-T3 + a
  render-pass owner + instance-feed; VK-owns-depth-in-D32 dodges the P5 D24S8 blocker.
- **#3 P6 subgraph growth** — only after the shared postprocess device exists (P13-T3 pays
  most of it down) AND BoxDecals is ported to a VK island. Highest cost, least selectable-goal
  progress; it's an internal-prettiness/optimization-adjacent piece, so it fits better once
  the backend is actually selectable.
