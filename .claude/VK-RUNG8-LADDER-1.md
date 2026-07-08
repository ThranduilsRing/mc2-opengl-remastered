# VK-RUNG8-LADDER-1 — postprocess / UI / present ownership sub-ladder

Rung 8 of the VK full-ownership ladder (recon: `.claude/VK-FULL-OWNERSHIP-RECON-1.md` §C.8).
Goal: VK owns the post-scene tail (postprocess passes → composite/tonemap → HUD/ImGui →
present), interop scaffolding becomes deletable, perf tax inverts, Metal/MoltenVK gate opens.

Derived from the ACTUAL GL chain in `GameOS/gameos/gos_postprocess.cpp` endScene tail
(order: HZB/cluster-pyramid → screen-shadow → cloud-shadow → shoreline → SSAO →
box-decals → edge-fog → oob-fog → backbuffer bind → letterbox/FORCE-43 →
composite/tonemap/FXAA/viewmode → HUD → ImGui → present) and the existing VK art:

- `vulkan_postprocess_subgraph.cpp` (P2): fused edge+oob fog — but on a PRIVATE
  VkInstance/VkDevice, with per-frame depth `glGetTexImage` CPU readback ("P5" never done)
  and 2× `glCopyImageSubData` color round-trips per frame.
- 4 old interop islands (hzb / ssao / edge-fog / oob-fog) — same old-arc pattern
  (own device and/or CPU readback oracles).
- P14 shared GBuffer: `sceneColorTex_`/`sceneNormal`/`sceneDepthTex_` ARE VK images on the
  shared ENGINE VulkanContext (`vulkan_shared_scene_color/depth.cpp`) under the
  `MC2_VK_SHARED_GBUFFER` umbrella — terrain/prop/mech/water islands already co-render
  into them in place.
- `engine_present_vk.cpp`: VK present + `MC2_VK_PRESENT_DIRECT_FBO` (copy-1 already
  eliminated; copy-2 irreducible until VK produces every pixel).

## Sub-ladder (ordered, smallest verifiable first)

| # | Sub-rung | Size | Gate | Kills | Notes |
|---|---|---|---|---|---|
| 8.1 | **Fog pair NATIVE (edge+oob) on the engine context, in-place shared GBuffer** — new `vulkan_post_fog_native.cpp`; one render pass, 2 draws, samples shared scene depth directly, blends into shared scene color0 LOAD/STORE. Reuses proven `edge_fog.*`/`fog_oob.*` SPIR-V verbatim. | S-M | `MC2_VK_POST_FOG_NATIVE` (envTriState opt-in, default-OFF, new-rung pattern) | Per-frame depth `glGetTexImage` readback + staging upload + 2× `glCopyImageSubData` color copies + the subgraph's whole private VkInstance/VkDevice (path bypassed; TU deletable at 8.7) | **THIS SESSION.** First rung-8 pass where VK reads the shared GBuffer and writes the shared output with ZERO copies. Metal-clean (frag sampling + alpha blend only). |
| 8.2 | **SSAO native port** (ssao gen + apply on engine context, shared depth in / shared color0 multiply out; retire `vulkan_ssao_island` readback pattern) | M | `MC2_VK_POST_SSAO` | SSAO island's depth readback/copies; second old-arc island | Two passes (ssao target + apply). Needs one VK-owned intermediate R8 image (device-local, non-shared — no interop). Metal-clean. |
| 8.3 | **Screen-shadow + cloud-shadow + shoreline native** (three multiplicative fullscreen passes, same shape as 8.1) | M | `MC2_VK_POST_SCREENFX` (sub-gates per pass for bisect) | Nothing interop-wise (GL-only passes today) — but removes 3 more GL passes from the tail so the GL tail shrinks toward composite-only | Shaders need verbatim VK ports (+ glsl_port_diff registration). Shadow-map sampling: shadow map still GL-generated until ladder rung 2 (shadow ownership) — sample via the existing zero-copy shadow seam (`vulkan_shadow_island_live`). |
| 8.4 | **HZB / cluster-depth-pyramid native compute** | M | `MC2_VK_POST_HZB` | hzb island copies; frees the last depth CONSUMER that isn't VK | Compute; MoltenVK-clean (no geometry/tess). Consumer-less today beyond SSAO — sequence after 8.2. |
| 8.5 | **Composite/tonemap/FXAA/viewmode + letterbox/FORCE-43 native → writes the present image directly** | L | `MC2_VK_POST_COMPOSITE` | The `presentDirectFbo` GL composite; after this VK produces every SCENE pixel — copy-2 becomes a swapchain blit of a VK-owned image (perf inversion point #1) | Big verbatim port (postprocess.frag family: exposure, sunset grade, FXAA, viewmode debug branches, OID buffer). Needs shared color0 as INPUT + swapchain image as output. |
| 8.6 | **HUD + ImGui on VK** (`imgui_impl_vulkan`; new `vulkan_hud_island.cpp` for the GameOS 2D batch) | L (own arc) | `MC2_VK_HUD` | The last GL draws in the frame | **Perf inversion point #2 / flip-ready:** after 8.6 GL issues zero draws; present tax gone. ImGui VK backend is MoltenVK-mature. |
| 8.7 | **Interop scaffolding deletion** — delete `vulkan_postprocess_subgraph.cpp`, 4 old islands, `MC2_VULKAN_POSTPROCESS_SUBGRAPH*` gates; `VK_KHR_EXTERNAL_MEMORY_WIN32`/`EXTERNAL_SEMAPHORE_WIN32` + `vulkan_shared_scene_*` GL-import halves become deletable | M | (deletion, no gate) | ALL remaining interop copies at once — no GL side left | **Metal gate opens here:** the Win32-only external-memory/semaphore family is the one hard MoltenVK blocker in the tail; everything else in 8.1-8.6 is VK 1.1-core + frag/compute only. |

Sequencing: 8.1 → 8.2 → (8.3 ∥ 8.4) → 8.5 → 8.6 → 8.7. Each gated default-OFF,
fail-soft to the GL pass, independently oracle-able (fog/SSAO poses + byte-off control
+ tier1 both-states).

Metal-gate blockers found: only the Win32 interop extension family (dies at 8.7) and
present-mode matrix beyond FIFO (UNVERIFIED, baseline FIFO safe). No geometry shaders,
no tessellation anywhere in the tail.

## Status

- 8.1 — SHIPPED; see VK-ARC-STATE.md "RUNG 8.1" section.
- 8.2 — SHIPPED (VK-POST-SSAO-NATIVE-1, commits 658107ae/786f6999, gate
  `MC2_VK_POST_SSAO`); see VK-ARC-STATE.md "RUNG 8.2" section. Old SSAO island
  becomes a DELETE candidate at 8.7.
- 8.3 — SHIPPED (VK-POST-SCREENFX-NATIVE-1, gate `MC2_VK_POST_SCREENFX`);
  screen-shadow/cloud-shadow/shoreline native on shared engine context.
- 8.4 — SHIPPED (VK-POST-HZB-NATIVE-1, gate `MC2_VK_POST_HZB`);
  native HZB builds the ceil-ladder pyramid on the shared engine context, samples
  shared depth in-place, preserves shared-GBuffer color/normal/depth parity by
  acquiring the full triplet, and reports zero copy/readback ops. Probe mode
  (`MC2_HZB_PROBE=1`) intentionally declines with `probe_enabled`.
- 8.5a - META PATH CONFIRMED (VK-PRESENT-DIRECT-FBO-1, gate
  `MC2_VK_PRESENT_DIRECT_FBO`). Existing implementation is the correct first
  8.5 slice: keep GL composite/FORCE-43/HUD/ImGui unchanged, render them into
  the shared present FBO, and skip copy-1. Exact-lane smoke on
  `A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe` at `b52050ccdf71`:
  baseline `2026-07-07T22-55-54` PASS 2/2, gate-on
  `2026-07-07T22-57-19` PASS 2/2. Vulkan-backend minimized smoke
  `2026-07-07T22-59-12` reached `MC2_RENDER_BACKEND=vulkan` and created the
  engine context, but present reported `fallback_reason="zero surface extent
  (window minimized?)"`, `present_frames=0`. Follow-up `cf02f587` added
  `run_visual_capture.py --show-window`; visible exact-lane capture on
  2026-07-08 (fingerprint `45afd60d7416`) wrote 11/11 `mc2_01` bookmarks and
  reported `present_frames=11101`, `validation_errors=0`,
  `readback_changed=1`, `fallback_reason=""`. RenderDoc harness proof landed
  with `8fd4bea5`: `renderdoc_capture.py --exe ...0.4c/mc2.exe` produced
  `.claude/vk-present-direct-visible/rdoc/direct_fbo_frame181.rdc` under
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1`.
  Next real coding slice = 8.5b native composite, but NOT by naively beginning
  the shared present `VkImageResource` inside postprocess: `presentGLFrame()`
  owns its `vkImageResourceBeginFrame()` parity advance. Either design a proper
  present-image split with a second GL->VK handoff after HUD/ImGui, or stage the
  first runtime hook against shared scene color. `CompositePresent` remains a
  separate-window test contract.
- 8.5b - FIRST RUNTIME SLICE SHIPPED (VK-POST-COMPOSITE-VISUAL-1). Under
  `MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1
  MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_POST_COMPOSITE=1`, Visual mode with FXAA
  off and a full drawable viewport now renders the final composite in
  EnginePresentVk: VK waits on the shared scene color, samples it, writes the
  shared present image, CPU-waits that submit, then legacy GL HUD can continue.
  This deliberately does not begin the shared present `VkImageResource` mid-tail;
  `presentGLFrame()` still owns the final present handoff. FORCE-43, FXAA,
  object-ID/viewmodes, and not-ready cases decline to the GL composite.
  Visible proof: `.claude/vk-post-composite-native-first-2/`, 11/11
  deterministic `mc2_01` bookmarks, `[VK_PRESENT] present_frames=301
  direct_fbo_grants=301 copy1_blit_frames=0 copy1_skip_frames=301
  composite_vk_frames=300 validation_errors=0 fallback_reason=""`. Minimized
  smoke: `tests/smoke/artifacts/2026-07-08T15-02-41`, PASS 2/2, +0 destroys
  (minimized present extent cannot exercise composite).
- 8.6a - SHIPPED (VK-IMGUI-BACKEND-COMPILE-1). Behavior-neutral infrastructure:
  `imgui_impl_vulkan.cpp/.h` are aligned to the vendored ImGui 1.91.8 docking
  core and compile into `imgui` under `MC2_VULKAN=ON` using Volk +
  `Vulkan::Headers`. `GuiRuntime` still uses SDL2/OpenGL3 at runtime. Runtime
  HUD/ImGui Vulkan remains blocked on the same present-image handoff split noted
  for 8.5b; do not draw into the shared present image mid-frame until that split
  exists.
- 8.6e - SHIPPED (VK-IMGUI-NATIVE-1, commit `4ed1ca21`). Native ImGui now
  renders through `imgui_impl_vulkan` at the final-tail present seam when
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1
  MC2_VK_PRESENT_TAIL_HOOK=1 MC2_VK_IMGUI=1`. Clean visible proof:
  `.claude/vk-imgui-native-clean/`, 11/11 deterministic bookmarks,
  `[VK_IMGUI] requested=1 used_vulkan=1 fallback_reason=""`, and final
  `[VK_PRESENT] imgui_vk_ready=1 imgui_vk_frames=6301 validation_errors=0`.
  Post-commit minimized smoke `tests/smoke/artifacts/2026-07-08T13-32-28`
  PASS 2/2 at clean HEAD `4ed1ca21db54`.
- Combined rung-8 smoke with `MC2_VK_SHARED_GBUFFER=1` and gates 8.1-8.6
  forwarded PASS 2/2 at `tests/smoke/artifacts/2026-07-08T14-47-14`.
  It proves 8.1/8.2/8.3/8.4 native post paths execute together
  (`substrate_live=1`, `used_vulkan>0`, no fallback) on `mc2_01` and `mc2_24`.
  Minimized smoke cannot prove present/ImGui execution because the surface extent
  is zero; use the visible proof above for that.
- 8.7 - BLOCKED, not a safe deletion yet. Native ImGui is done and native
  composite has its first Visual/full-viewport slice, but native GameOS HUD
  (`MC2_VK_HUD`) is not landed and composite still has GL fallback coverage for
  FORCE-43, FXAA, object-ID/viewmodes, and diagnostics. Old postprocess islands
  are bypassed by shared-GBuffer native gates but remain fallback/oracle code.
  Win32 GL interop primitives remain required by shared scene color/depth/present
  while GL continues to draw HUD and fallback composite pixels. Delete only after
  native HUD plus the remaining composite branches are live and verified.
