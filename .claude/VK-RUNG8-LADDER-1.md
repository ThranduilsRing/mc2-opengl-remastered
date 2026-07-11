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

- 8.7ao — ITEM-4 SPLIT (mech + 4 post passes): DONE. Deleted the per-island GL↔VK
  semaphore-handoff (hybrid) branches in the mech island + the 4 native post passes
  (fog/SSAO/screenfx/HZB); retired their 5 `*_DIRECT_CHAIN` gates to deprecation no-ops;
  wired mech boot-decline + post native-or-decline. Commits `a0cba0c4`+`bc923366`, strict
  proof PASS at `bc923366 dirty=0`. See VK-ARC-STATE.md top block. ★BLOCKER: prop+water
  native co-write is disabled by a latent `!useInterop` run-guard (`04a15aec`); their
  item-4 + terrain + shadow are DEFERRED with the order recorded in VK-ARC-STATE.md.
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
- 8.5c - SHIPPED (VK-POST-COMPOSITE-FXAA-1). The native Visual composite no
  longer declines when `MC2_FXAA=1`; `postprocess_visual.frag` now ports the
  existing GL FXAA filter and uses the same tunables. Visible proof:
  `.claude/vk-post-composite-fxaa-native/`, `MC2_FXAA=1`, 11/11 deterministic
  `mc2_01` bookmarks, `[FXAA v1] enabled=1`, and final `[VK_PRESENT]
  present_frames=301 direct_fbo_grants=301 copy1_blit_frames=0
  copy1_skip_frames=301 composite_vk_frames=300 validation_errors=0
  fallback_reason=""`. Minimized smoke with FXAA forwarded after allowlist fix:
  `tests/smoke/artifacts/2026-07-08T15-12-17`, PASS 2/2, +0 destroys.
- 8.5d - SHIPPED (VK-POST-COMPOSITE-FORCE43-1). The native Visual composite no
  longer declines on FORCE-43/pillarbox viewports. VK now clears the shared
  present image black, then draws the scene into the caller-supplied narrowed
  viewport. Visible proof: `.claude/vk-post-composite-force43-native/`,
  `MC2_FORCE_43=1`, 11/11 deterministic `mc2_01` bookmarks, final
  `[VK_PRESENT] present_frames=301 direct_fbo_grants=301 copy1_blit_frames=0
  copy1_skip_frames=301 composite_vk_frames=300 validation_errors=0
  fallback_reason=""`. Minimized smoke with FORCE-43 forwarded:
  `tests/smoke/artifacts/2026-07-08T15-16-01`, PASS 2/2, +0 destroys.
- 8.5e - SHIPPED (VK-POST-COMPOSITE-LOWLIGHT-1). LowLight/NightVision
  viewmode (mode 5) now runs through the native composite; no object-ID import is
  needed. Visible proof: `.claude/vk-post-composite-lowlight-native/`,
  `MC2_VIEWMODE_FRAMEWORK=1 MC2_VIEW_MODE=5`, 11/11 deterministic `mc2_01`
  bookmarks, `[VIEWMODE v1] framework=1 initialMode=5`, and final
  `[VK_PRESENT] present_frames=301 direct_fbo_grants=301 copy1_blit_frames=0
  copy1_skip_frames=301 composite_vk_frames=300 validation_errors=0
  fallback_reason=""`. Minimized smoke:
  `tests/smoke/artifacts/2026-07-08T15-20-02`, PASS 2/2, +0 destroys.
- 8.5f - SHIPPED (VK-POST-COMPOSITE-OBJECTID-1). The shared scene substrate now
  includes MRT2 object-id as `VK_FORMAT_R32_UINT` imported to `GL_R32UI`.
  Native composite samples it for ObjectIdDebug (mode 1) and Thermal mech-hot
  classification (mode 3). Visible proofs:
  `.claude/vk-post-composite-objectid-native/` and
  `.claude/vk-post-composite-thermal-native/`, each 11/11 deterministic
  `mc2_01` bookmarks, `[VK_SHARED_SCENE_COLOR] objectId(MRT2) active: VK R32UI
  ... imported to GL_R32UI`, and final `[VK_PRESENT] composite_vk_frames=300
  copy1_blit_frames=0 fallback_reason=""`. Minimized Thermal smoke:
  `tests/smoke/artifacts/2026-07-08T15-28-36`, PASS 2/2, +0 destroys.
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
- 8.6f - HUD TEXTURE METADATA CHECKPOINT (VK-HUD-TEXTURE-METADATA-1). The
  `MC2_VK_HUD=1` queue path now carries GL texture id plus width/height/format
  metadata into the final-tail batch and reports `hud_vk_textured_last`.
  Visible proof `.claude/vk-hud-metadata-visible/` captured 11/11 deterministic
  `mc2_01` bookmarks under the full rung-8 env stack. Runtime receipt:
  `[VK_PRESENT] present_frames=301 direct_fbo_grants=301 copy1_skip_frames=301
  tail_hook_frames=301 composite_vk_frames=300 hud_vk_queued_frames=174
  hud_vk_consumed_frames=174 hud_vk_draws_last=95 hud_vk_textured_last=69
  imgui_vk_ready=1 imgui_vk_frames=301 validation_errors=0 fallback_reason=""`.
  Next HUD slice must add a content serial/mirror-ready signal because GL
  texture names can stay stable across `glTexSubImage2D`/`Unlock()` mutations.
- 8.6g - HUD TEXTURE SERIAL CHECKPOINT (VK-HUD-TEXTURE-SERIAL-1). `gosTexture`
  now owns a content serial, bumped at creation, `Unlock()` re-upload, D3F font
  atlas `glTexSubImage2D`, and compressed prebuilt registration. HUD queue
  records carry that serial and health reports serialized coverage. Visible
  proof `.claude/vk-hud-serial-visible/` captured 11/11 deterministic `mc2_01`
  bookmarks under the full rung-8 env stack. Runtime receipt:
  `[VK_PRESENT] present_frames=301 direct_fbo_grants=301 copy1_skip_frames=301
  tail_hook_frames=301 composite_vk_frames=300 hud_vk_queued_frames=170
  hud_vk_consumed_frames=170 hud_vk_draws_last=95 hud_vk_textured_last=69
  hud_vk_serialized_last=69 imgui_vk_ready=1 imgui_vk_frames=301
  validation_errors=0 fallback_reason=""`. Next HUD slice can key a VK mirror
  cache by texture handle + serial and decline/GL-fallback only cache misses.
- 8.6h - HUD COLOR-PROOF NATIVE DRAW (VK-HUD-COLOR-PROOF-1). Added
  `hud_color` SPIR-V plus a final-tail proof renderer under the stricter
  `MC2_VK_HUD_COLOR_PROOF=1` gate. It draws only untextured line/tri HUD
  batches natively into the shared present image before native ImGui; GL HUD
  replay remains active, so this validates the render pass, vertex upload,
  blend, coordinate transform, cleanup, and tail ordering without retiring the
  textured HUD path yet. Visible proof `.claude/vk-hud-color-proof-visible/`
  captured 11/11 deterministic `mc2_01` bookmarks under the full rung-8 env
  stack. Runtime receipt: `[VK_PRESENT] present_frames=301 direct_fbo_grants=301
  copy1_skip_frames=301 tail_hook_frames=301 composite_vk_frames=300
  hud_vk_queued_frames=178 hud_vk_consumed_frames=178 hud_vk_draws_last=95
  hud_vk_textured_last=69 hud_vk_serialized_last=69 hud_vk_color_frames=178
  hud_vk_color_draws_last=18 imgui_vk_ready=1 imgui_vk_frames=301
  validation_errors=0 fallback_reason=""`. Remaining HUD blocker: native
  textured quad/text mirror and GL replay skip.
- 8.6i - HUD TEXTURE-PROOF NATIVE DRAW (VK-HUD-TEXTURE-PROOF-1). Added
  `hud_tex` SPIR-V and a storage-buffer texture mirror keyed by HUD texture
  handle + content serial. The GameOS queue carries RGBA8 payloads for textured
  HUD records; the final-tail renderer expands textured quads/text to triangles,
  uploads changed serials into host-visible VK buffers, and samples them
  natively before Vulkan ImGui. GL HUD replay still remains active, so this is
  a proof/coverage rung, not final HUD retirement. Visible proof
  `.claude/vk-hud-texture-proof-visible/` captured 11/11 deterministic
  `mc2_01` bookmarks under the full rung-8 env stack. Runtime receipt:
  `[VK_PRESENT] present_frames=301 direct_fbo_grants=301 copy1_skip_frames=301
  tail_hook_frames=301 composite_vk_frames=300 hud_vk_queued_frames=187
  hud_vk_consumed_frames=187 hud_vk_draws_last=95 hud_vk_textured_last=69
  hud_vk_serialized_last=69 hud_vk_color_frames=187 hud_vk_color_draws_last=87
  hud_vk_textured_frames=187 hud_vk_textured_draws_last=69
  hud_vk_texture_uploads=22 imgui_vk_ready=1 imgui_vk_frames=301
  validation_errors=0 fallback_reason=""`. Remaining HUD blocker: order-accurate
  full-batch replay validation, then skip GL replay only when every queued draw
  is representable.
- 8.6j - HUD ORDERED NATIVE REPLAY PROOF (VK-HUD-ORDERED-REPLAY-1). The
  present-tail HUD recorder now walks the queued GameOS draw records once and
  emits a native command list in original order, switching between color-line,
  color-tri, and mirrored-texture pipelines as needed. GL HUD replay remains
  active. Visible proof `.claude/vk-hud-ordered-visible/` captured 11/11
  deterministic `mc2_01` bookmarks under the full rung-8 env stack. Runtime
  receipt: `[VK_PRESENT] present_frames=301 direct_fbo_grants=301
  copy1_skip_frames=301 tail_hook_frames=301 composite_vk_frames=300
  hud_vk_queued_frames=172 hud_vk_consumed_frames=172 hud_vk_draws_last=95
  hud_vk_textured_last=69 hud_vk_serialized_last=69 hud_vk_color_frames=172
  hud_vk_color_draws_last=87 hud_vk_textured_frames=172
  hud_vk_textured_draws_last=69 hud_vk_texture_uploads=22 imgui_vk_ready=1
  imgui_vk_frames=301 validation_errors=0 fallback_reason=""`. Remaining HUD
  blocker: gated GL replay skip and parity proof with the native path as sole
  HUD producer.
- 8.6k - HUD GL REPLAY SKIP (VK-HUD-SKIP-GL-1). Added
  `MC2_VK_HUD_SKIP_GL=1`: `gosRenderer::flushHUDBatch()` now skips legacy GL HUD
  replay only when the queued batch is fully representable by the ordered native
  renderer and the present-tail queue handoff succeeds. Native renderer coverage
  now includes invalid-texture/untextured quads, text records that arrive as
  triangle lists, and both alpha-blend plus alpha-one/zero opaque pipeline
  variants. Visible proof `.claude/vk-hud-skip-gl-visible-3/` captured 11/11
  deterministic `mc2_01` bookmarks under the full rung-8 env stack with GL HUD
  replay skipped. Runtime receipts: `[VK_HUD_SKIP v1] frame=120 draws=95
  reason="native_full_coverage"` and `[VK_PRESENT] present_frames=301
  direct_fbo_grants=301 copy1_skip_frames=301 tail_hook_frames=301
  composite_vk_frames=300 hud_vk_queued_frames=170 hud_vk_consumed_frames=170
  hud_vk_draws_last=95 hud_vk_textured_last=69 hud_vk_serialized_last=69
  hud_vk_color_frames=170 hud_vk_color_draws_last=95
  hud_vk_textured_frames=170 hud_vk_textured_draws_last=69
  hud_vk_texture_uploads=22 imgui_vk_ready=1 imgui_vk_frames=301
  validation_errors=0 fallback_reason=""`. This is the first verified native-only
  GameOS HUD producer path. Remaining tail blocker: decide whether to keep the
  CPU RGBA payload mirror as a proof-only bridge or replace it with mutation-time
  retained CPU mirrors before interop deletion/perf finalization.
- Combined rung-8 smoke with `MC2_VK_SHARED_GBUFFER=1` and gates 8.1-8.6
  forwarded PASS 2/2 at `tests/smoke/artifacts/2026-07-08T14-47-14`.
  It proves 8.1/8.2/8.3/8.4 native post paths execute together
  (`substrate_live=1`, `used_vulkan>0`, no fallback) on `mc2_01` and `mc2_24`.
  Minimized smoke cannot prove present/ImGui execution because the surface extent
  is zero; use the visible proof above for that.
- 8.6l - HUD RETAINED CPU MIRROR (VK-HUD-CPU-MIRROR-1). `gosTexture` now retains
  an RGBA8 CPU mirror at texture creation, writable `Unlock()`, and D3F font
  atlas upload time. The native HUD queue copies texture payloads from that
  retained mirror instead of issuing a replay-time GL `getTextureData()`
  readback for every textured HUD draw. Visible proof
  `.claude/vk-hud-mirror-visible/` captured 11/11 deterministic `mc2_01`
  bookmarks under the full rung-8 stack with GL HUD replay skipped. Runtime
  receipts: `[VK_HUD_SKIP v1] frame=120 draws=95 reason="native_full_coverage"`
  and `[VK_PRESENT] present_frames=301 composite_vk_frames=300
  hud_vk_queued_frames=174 hud_vk_consumed_frames=174 hud_vk_draws_last=95
  hud_vk_textured_draws_last=69 hud_vk_texture_uploads=22 imgui_vk_frames=301
  validation_errors=0 fallback_reason=""`. Smoke PASS 1/1 at
  `2026-07-08T16-21-39`.
- 8.7 - DELETE-READINESS CHECKPOINTS. Initial state: native
  ImGui, native composite Visual/FXAA/FORCE-43/LowLight/ObjectId/Thermal, and
  native GameOS HUD are now live under the full rung-8 stack. With
  `MC2_VK_HUD=1 MC2_VK_HUD_COLOR_PROOF=1 MC2_VK_HUD_SKIP_GL=1`, visible proof
  `.claude/vk-hud-skip-gl-visible-3/` reached `[VK_HUD_SKIP v1] frame=120
  draws=95 reason="native_full_coverage"` and `[VK_PRESENT] present_frames=301
  composite_vk_frames=300 hud_vk_queued_frames=170 hud_vk_consumed_frames=170
  hud_vk_draws_last=95 hud_vk_textured_draws_last=69 imgui_vk_frames=301
  validation_errors=0 fallback_reason=""`. HUD is no longer the blocker for
  GL draw retirement in the present tail.

  The following 8.7a+ slices retire old postprocess islands, add direct-chain
  ownership for geometry/post/shadow/present/UI, and culminate in the
  resource-strict 8.7af proof where the full native stack creates no exportable
  Win32 interop resources and performs no GL/VK semaphore handoffs. Hybrid
  fallback code remains for non-strict legacy gates, but it is outside the
  proved final native stack.
- 8.7a - RETIRED OLD POSTPROCESS SUBGRAPH (VK-RUNG8-SUBGRAPH-DELETE-1).
  Deleted `vulkan_postprocess_subgraph.cpp` and removed its CMake entry,
  standalone `MC2_VULKAN_POSTPROCESS_SUBGRAPH*` smoke/docs/registry exposure,
  debug-state health section, `gos_postprocess` object/routes, and R2 interlock
  entry. The optional region `MC2_POSTPROCESS_BACKEND=vk` selector now routes
  directly to rung-8.1 shared-GBuffer native fog (`runVkPostFogNative`) instead
  of the retired private-device subgraph. Shared interop substrate remains live
  and untouched. Verification: clean build at `f4032772` (`dirty=0`), clean
  deploy manifest `src_commit=f403277266b3`, full native-tail smoke PASS 1/1 at
  `2026-07-08T16-31-02`, and audit grep for the retired subgraph gates/symbols
  returned no matches.
- 8.7b - RETIRED OLD PER-PASS FOG ISLANDS (VK-RUNG8-FOG-ISLAND-DELETE-1).
  Deleted `vulkan_edge_fog_island.cpp` and `vulkan_oob_fog_island.cpp`, removed
  their CMake entries, runtime routes, `gosPostProcess` members, debug-state
  health sections, `MC2_VULKAN_EDGE_FOG_ISLAND` / `MC2_VULKAN_OOB_FOG_ISLAND`
  / `MC2_VK_INTEROP_BRIDGE` / selftest smoke/docs/registry exposure, and the
  now-obsolete engine-device R2 interlock entry. Fog precedence is now native
  shared-GBuffer fog (`MC2_VK_POST_FOG_NATIVE`) when armed and ready, else the
  original GL edge/oob fog chain. Shared scene interop primitives remain live
  for current geometry/post/present paths. Verification: committed
  `de2ec973`, clean build (`dirty=0`), clean deploy manifest
  `src_commit=de2ec973bb08`, full native-tail smoke PASS 1/1 at
  `2026-07-08T16-39-55`, and current-code grep for the retired gates/symbols
  returned no matches outside historical/template docs.
- 8.7c - RETIRED OLD SSAO/HZB PRIVATE-DEVICE ISLANDS
  (VK-RUNG8-SSAO-HZB-ISLAND-DELETE-1). Deleted `vulkan_ssao_island.cpp` and
  `vulkan_hzb_island.cpp`, removed their CMake entries, `gosPostProcess`
  members/teardown, debug-state health sections, smoke allowlist entries, and
  active flag/env docs for `MC2_VK_SSAO_INTEROP`,
  `MC2_VK_SSAO_DUMP_INPUTS`, `MC2_VULKAN_SSAO_ISLAND_FORCE_FALLBACK`, and
  `MC2_VULKAN_HZB_ISLAND_FORCE_FALLBACK`. The optional region-interface hooks
  are retained but now delegate to the shared-engine native SSAO/HZB paths
  (`MC2_VK_POST_SSAO`, `MC2_VK_POST_HZB`) with unchanged GL fallback on
  native decline. Shared scene interop primitives remain live and untouched for
  current native geometry/post/present paths. Verification: clean build
  (`dirty=0`) and clean deploy manifest for the slice commit, full native-tail
  smoke PASS 1/1 at `tests/smoke/artifacts/2026-07-08T16-51-55`; runtime
  receipts show native
  fog/SSAO/screenfx/HZB all `runtime_gate=1 substrate_live=1 used_vulkan>0`,
  HZB `copy_depth_ops=0 readback_ops=0`, `validation_errors=0`, and grep for
  retired live-code/docs symbols returns no matches outside ladder/history
  notes.
- 8.7d - RETIRED PROP TIER-0 RAW GATE
  (VK-RUNG8-PROP-TIER0-GATE-DELETE-1). Removed active public exposure for
  `MC2_VK_PROP_COLOR_INTEROP` from smoke passthrough, registry, generated flag
  docs, and tier docs. The color-prop island now arms only through the Tier-1
  shared-GBuffer gate (`MC2_VK_PROP_COLOR_TIER1` / umbrella) and the old raw
  gate returns false internally, so callers cannot select the side-image +
  GL-composite proof path. Runtime health was relabeled to
  `[VK_PROP_COLOR_TIER1]` to avoid confusing the live Tier-1 path with the
  retired gate. Verification: dirty-slice build/deploy and full native-tail
  smoke PASS 1/1 at `tests/smoke/artifacts/2026-07-08T17-02-44`; receipts show
  `TIER-1 shared-GBuffer co-write ACTIVE`, `[VK_PROP_COLOR_TIER1]
  used_vulkan=144 used_interop=144 interop_active=1 vk2gl_sem=1
  validation_errors=0 fallback_reason=""`, and grep for the retired env name
  returns no active matches. Follow-up: mechanically delete the unreachable
  Tier-0 branch/resources from `vulkan_color_prop_island.cpp`, then retire the
  separate water P9a side-image gate.
- 8.7e - RETIRED WATER P9a RAW GATE
  (VK-RUNG8-WATER-P9A-GATE-DELETE-1). Removed active public exposure for
  `MC2_VK_WATER_INTEROP` from smoke passthrough, registry, generated flag docs,
  and tier docs. The water island now arms only through the Tier-1 shared-GBuffer
  gate (`MC2_VK_WATER_TIER1`) and the old side-image gate returns false
  internally, so callers cannot select the offscreen + GL-composite P9a proof
  path. Runtime health was relabeled to `[VK_WATER_TIER1]`. Verification target:
  clean build/deploy/smoke after commit; receipts should show the full native
  tail unchanged and no active grep matches for the retired env/health name.
- 8.7f - DELETED PROP TIER-0 SCAFFOLDING
  (VK-RUNG8-PROP-TIER0-SCAFFOLD-DELETE-1). Mechanically removed the unreachable
  prop side-image/composite branch from `vulkan_color_prop_island.cpp`: raw gate
  helper, offscreen color/depth resources, local renderpass/framebuffer/pipeline,
  `make_depth`, GL composite helper, and the dead post-Tier-1 frame tail. The
  health ABI and Tier-1 descriptor/material/light paths remain intact. The
  retired `color_prop` shader bake was removed separately in `7d09867e`, and
  the retired source shader pair was deleted in the follow-up shader cleanup.
  Verification: `2e9a1868` clean build/deploy/smoke PASS 1/1 at
  `tests/smoke/artifacts/2026-07-08T17-21-27`; Tier-1 prop stayed active with
  zero forbidden readbacks. `6fb40818` then deleted the retired `color_prop`
  shader pair and verified deploy no longer ships `color_prop.*`.
- 8.7g - DELETED WATER P9a FRAME TAIL
  (VK-RUNG8-WATER-P9A-FRAME-TAIL-DELETE-1). Removed the unreachable legacy
  offscreen/depth-import/composite frame tail after the Tier-1 shared-GBuffer
  water path returns in `vulkan_water_island.cpp`. Verification:
  `e465d214` clean build/deploy/smoke PASS 1/1 at
  `tests/smoke/artifacts/2026-07-08T17-30-33`; `[VK_WATER_TIER1]`
  `used_vulkan=140 used_interop=140 validation_errors=0`, all forbidden color
  counters zero, and retired depth-in counters zero. Native fog/SSAO/screenfx/HZB
  remained live.
- 8.7h - DELETED WATER P9a SCAFFOLDING
  (VK-RUNG8-WATER-P9A-SCAFFOLD-DELETE-1). Removed the false raw P9a gate,
  offscreen shared color/GL view/FBO state, local gbuffer/depth import resources,
  non-Tier1 renderpass/framebuffer/blend branches, GL composite helper, and
  orphaned local image helpers from `vulkan_water_island.cpp`. Tier-1 now has a
  single shared-GBuffer init/render path. Verification: `2232b9be` clean
  build/deploy/smoke PASS 1/1 at
  `tests/smoke/artifacts/2026-07-08T17-35-19`; `[VK_WATER_TIER1]`
  `used_vulkan=138 used_interop=138 validation_errors=0`, all forbidden color
  counters zero, retired depth-in counters zero, and native post tail remained
  active. Grep for retired P9a symbols/resources in the water island returns no
  matches.
- 8.7i - CSM FULL-MAP DIRECT VK CONSUME
  (VK-CSM-FULLMAP-DIRECT-1). Added default-off `MC2_VK_CSM_FULLMAP_INTEROP`
  companion to `MC2_VK_SHADOW_INTEROP`: the shadow island renders the CSM
  catch-all/full-map cascade into a second exportable D32 target using the last
  cascade matrix and same static-prop caster set. Terrain binding 9 now binds
  that `VkImageView` directly when the same-frame producer succeeds, skipping
  the full-map cascade's per-frame `glGetTexImage` + `vkCmdCopyBufferToImage`
  import while leaving the near-cascade array copy path unchanged. Verification:
  dirty-slice build/deploy/smoke PASS 1/1 at
  `tests/smoke/artifacts/2026-07-08T17-51-19`; receipts show
  `CSM full-map target initialized 4096x4096`, `first VK CSM full-map produced`,
  and steady `VK_TERRAIN_COPY_STATS ... csmFullMapDirect=1`. Copy zone dropped
  from the prior CSM baseline range (~124-130 ms after warm-up) to ~110-115 ms
  after warm-up, consistent with full-map deletion while the large near-array
  import remains the next ownership target. Native fog/SSAO/screenfx/HZB,
  water, prop, terrain, and mech health remained active with empty fallback
  reasons in the same smoke.
- 8.7j - CSM NEAR-ARRAY DIRECT VK CONSUME
  (VK-CSM-ARRAY-DIRECT-1). Added default-off `MC2_VK_CSM_ARRAY_INTEROP`
  companion to `MC2_VK_SHADOW_INTEROP`: the shadow island renders each near CSM
  cascade into a layer of a VK-owned D32 2D array, and terrain binding 8 binds
  that array view directly when all near layers were produced in the same frame.
  Combined with 8.7i full-map direct consume, this removes the remaining terrain
  CSM CPU import path (`glCopyImageSubData` scratch + `glGetTexImage` +
  `vkCmdCopyBufferToImage`) from the hot frame. Verification:
  dirty-slice build/deploy/smoke PASS 1/1 at
  `tests/smoke/artifacts/2026-07-08T17-57-45`; receipts show
  `CSM array target initialized 8192x8192 layers=2`, `first VK CSM array
  produced`, steady `VK_TERRAIN_COPY_STATS ... csmArrayDirect=1
  csmFullMapDirect=1`, and post-warm-up `copies_ms` collapsing to ~0.001 ms.
  Terrain health in the same run dropped to `stage_us=120.7 total_us=1374.3`
  with `forbidden_getTexImage=0 forbidden_readPixels=0`; native
  fog/SSAO/screenfx/HZB, water, prop, and mech health remained active with empty
  fallback reasons.
- 8.7j follow-up - CSM DIRECT VISUAL PARITY FIX
  (VK-CSM-DIRECT-VISUAL-1). Targeted bookmark A/B found the direct near-array
  path visually wrong at first (`copy vs array_only`: aggregate coverage
  ~90.355%, mean_all ~58.676). Split proof isolated full-map direct as near
  neutral (`copy vs full_only`: aggregate coverage ~1.864%, with one exact
  zero mid shot) and the near array as the failure. The actual fix was the CSM
  array layer producer's Vulkan viewport convention: using a negative-height
  viewport made the VK array layer match the GL shadow texture orientation.
  Final proof on `tests/visual/bookmarks/mc2_01_buildings.json`: `copy vs
  both_direct` aggregate coverage `0.000%`, mean_all `0.0000` (all three
  building bookmarks exact zero). Tier1 smoke on explicit `0.4c` deploy passed
  5/5 at `tests/smoke/artifacts/2026-07-08T18-26-37`; after adding
  `MC2_SHADOW_CSM` to the smoke allowlist, CSM-enabled tier1 smoke passed 5/5 at
  `tests/smoke/artifacts/2026-07-08T18-30-44`; clean post-commit build/deploy
  (`sha=f10a87a2be3c dirty=0`) passed the same CSM-enabled tier1 gate at
  `tests/smoke/artifacts/2026-07-08T18-36-40`.
- 8.7k - NATIVE CSM SHADOW DEBUG OVERLAY
  (VK-SHADOW-DEBUG-OVERLAY-NATIVE-1). Added a diagnostic-only native overlay
  pass for `MC2_SHADOW_DEBUG_MODE=dynamic` when CSM is active and the shadow
  island has a same-frame VK CSM array view. Static and non-ready debug sources
  still fall back to the existing GL overlay, so the diagnostic never disappears
  and no new readback path is introduced. Verification: correct `v0.4c` deploy
  with `shadow_debug_overlay.{vert,frag}.spv`, diagnostic smoke PASS 1/1 at
  `tests/smoke/artifacts/2026-07-08T19-02-11`, kept-log minimized smoke PASS
  at `tests/smoke/artifacts/2026-07-08T19-04-37`, and visible bookmark proof
  `.claude/mech-parity/shadow_debug_overlay_native_visible/` captured 3/3
  building bookmarks. Runtime receipts: `[SHADOW_DEBUG] ... dynamic`,
  `first VK CSM array produced`, `[VK_PRESENT] created=1 ... copy1_skip_frames=5401
  composite_vk_frames=5400 shadow_debug_vk_frames=5400 ... fallback_reason=""`.
- 8.7l - VK-OWNED PRESENT HANDOFF PROOF
  (VK-PRESENT-SKIP-GL-HANDOFF-1). Added explicit default-off
  `MC2_VK_PRESENT_SKIP_GL_HANDOFF`: when direct present is armed and a native VK
  path produced the shared present image, `presentGLFrame()` skips the GL->VK
  semaphore signal/wait pair and waits only on swapchain acquire before copy-2.
  This proves the present blit can consume a VK-owned image without the GL
  producer handoff; it does not delete `gl_interop_view.*` yet because GL can
  still produce shared scene/present pixels on fallback paths. Verification:
  build/deploy to `mc2-win64-v0.4c`, minimized smoke PASS 1/1 at
  `tests/smoke/artifacts/2026-07-08T19-16-40` (env passthrough/stability; zero
  surface extent), and visible bookmark proof in
  `.claude/vk-present-skip-gl-handoff/` captured 3/3 building bookmarks with
  `[VK_PRESENT] VK-owned present image consumed without GL handoff
  (gl_handoff_skip_frames=1)`, native ImGui `used_vulkan=1`, HUD skip
  `reason="native_full_coverage"`, and `fallback_reason=""`.
- 8.7m - STATIC SHADOW VK SUBSTRATE
  (VK-STATIC-SHADOW-MAP-1). Adds default-off `MC2_VK_STATIC_SHADOW_MAP`:
  `shadowDepthTex_`/`ShadowStaticMap` can now be a VK-built exportable
  `D32_SFLOAT` image imported into GL as `GL_DEPTH_COMPONENT32F`. GL still
  renders the static shadow pass and GL screen-shadow still samples the same
  texture with the old compare state; this only gives the next native
  screen-shadow slice a same-image `VkImageResource` for static shadow sampling
  without adding a readback/copy. Fail-soft falls back to the plain GL
  `GL_DEPTH_COMPONENT24` allocation.
- 8.7n - STATIC-ONLY SCREEN-SHADOW NATIVE
  (VK-SCREEN-SHADOW-STATIC-NATIVE-1). `MC2_VK_POST_SCREENFX` now includes
  subgate `MC2_VK_POST_SCREENFX_SHADOW`; when `MC2_VK_STATIC_SHADOW_MAP=1` and
  `MC2_SHADOW_ENABLE` is off, the screen-shadow fullscreen multiply runs in VK
  before cloud/shoreline. It samples shared scene depth/normal plus the shared
  static shadow map and skips the GL screen-shadow body. It explicitly declines
  while dynamic object shadows need a non-static path.
- 8.7o - CSM SCREEN-SHADOW NATIVE
  (VK-SCREEN-SHADOW-CSM-NATIVE-1). Added a separate `screen_shadow_csm` VK
  shader/pipeline for the normal dynamic CSM screen-shadow path. When
  `MC2_SHADOW_ENABLE=1`, `MC2_VK_SHADOW_INTEROP=1`,
  `MC2_VK_CSM_ARRAY_INTEROP=1`, and `MC2_VK_CSM_FULLMAP_INTEROP=1` have produced
  same-frame VK CSM views, the native screen-shadow pass samples the shared
  static shadow map plus the VK CSM array/full-map directly and skips the GL
  screen-shadow body. Debug classification and non-CSM dynamic remain GL
  fallback paths.
- 8.7p - SHARED-SCENE STRICT CANARY
  (VK-SHARED-SCENE-STRICT-CANARY-1). Added default-off
  `MC2_VK_SHARED_SCENE_STRICT=1`, a verification-only deletion-readiness guard:
  if a requested shared scene color/depth/normal/objectId slot falls back to
  the plain GL allocation, the engine logs `[VK_SHARED_SCENE_STRICT]` and
  aborts instead of silently continuing on a GL substrate. This does not delete
  `gl_interop_view.*`; it prevents final-tail/Metal-gate proofs from passing
  through a hidden shared-scene fallback. Verification: build/deploy to explicit
  `mc2-win64-v0.4c` lane, strict full-tail minimized smoke PASS 1/1 at
  `tests/smoke/artifacts/2026-07-08T20-21-36`; manifest includes
  `MC2_VK_SHARED_SCENE_STRICT=1`, shared scene color/depth/normal/objectId are
  VK-backed, post fog/SSAO/screenfx/HZB report `substrate_live=1`,
  `used_vulkan>0`, and `fallback_reason=""`. Negative forced-fallback probe was
  inconclusive (timeout, no strict abort), so keep this as a healthy-path canary
  until a smaller deterministic fallback fixture exists.
- 8.7q - PRESENT NATIVE STRICT CANARY
  (VK-PRESENT-NATIVE-STRICT-CANARY-1). Added default-off
  `MC2_VK_PRESENT_NATIVE_STRICT=1`, a verification-only final-tail guard for
  visible/nonzero-surface runs. When armed, `EnginePresentVk::presentGLFrame`
  aborts if a nonzero-surface frame would run copy-1, consume a present image
  not marked VK-owned, or signal the GL->VK handoff instead of using the
  `MC2_VK_PRESENT_SKIP_GL_HANDOFF` path. Minimized zero-extent smoke remains a
  non-proof and does not trip the canary. This does not delete the present
  `GlInteropView` yet; it gives future visible bookmark/RenderDoc proofs a hard
  failure mode for hidden GL present-tail rescue. Visible proof initially caught
  a real resize/direct-FBO interlock bug: the first post-resize frame had a stale
  direct-FBO arm and aborted with `present_image_not_vk_owned`. Fixed by dropping
  exactly that resized direct-FBO present and requiring the next frame to re-arm
  the rebuilt FBO. Clean-commit verification after the fix:
  `.claude/vk-present-native-strict-visible-clean/`, 3/3
  `mc2_01_buildings` bookmarks present and deterministic. Final health line:
  `present_frames=2700`, `resize_count=1`, `direct_fbo_grants=2701`,
  `copy1_blit_frames=0`, `copy1_skip_frames=2700`,
  `gl_handoff_skip_frames=2700`, `tail_hook_frames=2700`,
  `composite_vk_frames=2700`, `hud_vk_consumed_frames=1975`,
  `imgui_vk_frames=2700`, `fallback_reason=""`. Post fog/SSAO/screenfx/HZB also
  reported `substrate_live=1`, `used_vulkan>0`, and `fallback_reason=""`.
- 8.7r - NO-GL-INTEROP STRICT CANARY
  (VK-NO-GL-INTEROP-STRICT-CANARY-1). Added default-off
  `MC2_VK_NO_GL_INTEROP_STRICT=1`, a final Metal-gate canary wired at the
  central `GlInteropView` adapter. When armed, any production
  `glInteropSignalToVk` or `glInteropWaitFromVk` logs
  `[VK_NO_GL_INTEROP_STRICT]` and aborts. This is expected to fail while the
  hybrid shared-scene/static-shadow/present substrates are still live; it becomes
  the hard pass/fail proof after the Win32 external-memory/semaphore family is
  deleted. Positive verification with the canary unset: adhoc `mc2_01` smoke
  PASS at `tests/smoke/artifacts/2026-07-08T20-41-00`. Negative verification
  with `MC2_VK_NO_GL_INTEROP_STRICT=1`: expected FAIL at
  `tests/smoke/artifacts/2026-07-08T20-41-34`; manifest contains the env var and
  `mc2_01.log:363` / crash evidence report
  `[VK_NO_GL_INTEROP_STRICT] abort op=glInteropSignalToVk`.
- 8.7s - TERRAIN NATIVE PREFIX CLEAR PROOF
  (VK-TERRAIN-NATIVE-PREFIX-CLEAR-1). Added default-off
  `MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR=1`. When armed with VK terrain Tier-1, the
  frame-entry shared scene color0, normal, and depth initialization moves from
  the legacy GL scene clear into the terrain render pass: color0 uses the same
  gameplay/menu clear color, normal clears to the `clearGBuffer1` sentinel
  `(0.5,0.5,1.0,0.0)`, and depth/stencil clear to reverse-Z far/0. The terrain
  island then skips its GL->VK and VK->GL semaphore handoff for those first-owner
  attachments. Verification: default full-tail smoke still PASS
  `tests/smoke/artifacts/2026-07-08T20-51-05`; new gate without strict canary
  PASS `tests/smoke/artifacts/2026-07-08T20-55-56`; new gate plus
  `MC2_VK_NO_GL_INTEROP_STRICT=1` expected FAIL
  `tests/smoke/artifacts/2026-07-08T20-56-29`, but after
  `[VK_TERRAIN_ISLAND] first TIER-1...`, with the next abort in
  `VK_COLOR_PROP_ISLAND`. Next deletion slice: make prop color a non-first
  shared-GBuffer VK owner without requiring GL interop semaphores, or introduce a
  shared-scene ownership scheduler that passes Vulkan ownership from terrain to
  prop/mech/water/post passes directly.
- 8.7t - PROP DIRECT CHAIN PROOF
  (VK-PROP-DIRECT-CHAIN-1). Added default-off `MC2_VK_PROP_DIRECT_CHAIN=1`.
  Requires `MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR=1` and VK prop Tier-1. When
  armed, prop color treats shared scene color0/normal/depth as already
  Vulkan-owned by the native terrain prefix, skips its GL->VK and VK->GL
  interop semaphore handoff, and still CPU-waits its submit fence before the
  next island. Positive verification with terrain+prop direct-chain and strict
  canary unset: PASS `tests/smoke/artifacts/2026-07-08T21-00-38`. Negative
  proof with `MC2_VK_NO_GL_INTEROP_STRICT=1`: expected FAIL
  `tests/smoke/artifacts/2026-07-08T21-01-14`, after
  `[VK_COLOR_PROP_ISLAND] first TIER-1...`, with the next abort in
  `VK_MECH_ISLAND`. Next deletion slice: chain mech shared-GBuffer co-write
  behind prop without GL interop semaphores.
- 8.7u - MECH DIRECT CHAIN PROOF
  (VK-MECH-DIRECT-CHAIN-1). Added default-off `MC2_VK_MECH_DIRECT_CHAIN=1`.
  Requires `MC2_VK_PROP_DIRECT_CHAIN=1` and VK mech Tier-1. When armed, the
  mech island treats shared scene color0/normal/depth as already Vulkan-owned by
  terrain+prop, skips its GL->VK and VK->GL interop semaphore handoff, and still
  CPU-waits its submit fence before the next island. Positive verification with
  terrain+prop+mech direct-chain and strict canary unset: PASS
  `tests/smoke/artifacts/2026-07-08T21-03-45`. Strict canary proof with
  `MC2_VK_NO_GL_INTEROP_STRICT=1`: PASS
  `tests/smoke/artifacts/2026-07-08T21-04-20`; manifest contains the strict env
  var, terrain/prop/mech all render, prop reports `used_interop=0`, and no
  `[VK_NO_GL_INTEROP_STRICT]` abort occurs. This is minimized-smoke proof only:
  present extent is zero and water is inactive, so final proof still requires a
  visible/full-tail strict run.
- 8.7v - WATER + COMPOSITE DIRECT CHAIN PROOF
  (VK-WATER-COMPOSITE-DIRECT-CHAIN-1). Added default-off
  `MC2_VK_WATER_DIRECT_CHAIN=1` and `MC2_VK_POST_COMPOSITE_DIRECT_CHAIN=1`.
  Water requires `MC2_VK_WATER_TIER1=1` plus `MC2_VK_MECH_DIRECT_CHAIN=1`; when
  armed, the water island treats shared scene color0/normal/depth as already
  Vulkan-owned by terrain+prop+mech, skips its GL->VK/VK->GL semaphore handoff,
  and still CPU-waits its submit fence before the composite. The visual
  composite direct-chain gate requires the full upstream
  terrain->prop->mech->water direct-chain env stack; for normal visual
  composition (non object-id view), it samples shared scene color0 as already
  Vulkan-owned, skips the scene-color GL interop handoff, and still CPU-waits
  its submit fence before present consumes the VK-owned image. Added
  `scripts/run_vk_nointerop_strict_visible.py` so the long gate stack is a
  repeatable harness instead of a hand-built shell command. Sidecar review
  caught an initial composite guard bug (checking only the water env flag);
  fixed by requiring the full upstream stack. Proofs: first visible strict run
  without `MC2_VK_WATER_TIER1=1` correctly aborted in legacy water; rerun with
  water tier1 advanced to composite; after composite direct-chain, full harness
  PASS at `.claude/vk-nointerop-strict-visible-harness/` with 11/11
  deterministic bookmarks and no `[VK_NO_GL_INTEROP_STRICT]` abort. Final
  exact-binary regression after the guard/pointer cleanup:
  `.claude/vk-nointerop-strict-visible-final-smoke/`, engine smoke PASS,
  7 deterministic bookmarks before duration, `composite_vk_frames=300`,
  `gl_handoff_skip_frames=300`, `VK_WATER_TIER1 used_interop=0`,
  `validation_errors=0`, no strict abort.
- 8.7w - COMPOSITE SCENE DIRECT FOR ALT MODES
  (VK-COMPOSITE-SCENE-DIRECT-ALT-MODES-1). Split the composite direct-chain
  decision so shared scene color0 skips its GL->VK/VK->GL handoff whenever the
  full terrain->prop->mech->water stack is armed, even when the selected
  composite view mode also needs objectId MRT2. The objectId attachment still
  synchronizes separately; this intentionally advances the strict canary to the
  real MRT2 ownership blocker instead of stopping on scene color first. Extended
  `scripts/run_vk_nointerop_strict_visible.py` with `--view-mode` and repeated
  `--env KEY=VALUE` so alternate-mode proofs are harnessed. Verification:
  LowLight strict run `.claude/vk-nointerop-strict-visible-lowlight/` reached
  engine smoke PASS with `MC2_VIEWMODE_FRAMEWORK=1`, `MC2_VIEW_MODE=lowlight`,
  `composite_vk_frames=300`, `gl_handoff_skip_frames=300`, and no strict abort.
  Normal visual strict regression after the split:
  `.claude/vk-nointerop-strict-visible-scenedirect-final-smoke/`, engine smoke
  PASS, 7 deterministic bookmarks before duration, no strict abort.
  ObjectIdDebug probe with `MC2_OBJECT_ID_BUFFER=1`:
  `.claude/vk-nointerop-strict-visible-objectid-scene-direct/`, expected FAIL
  after water at the remaining `glInteropSignalToVk` in the objectId branch.
  Next deletion slice: native ownership/writes for objectId MRT2 in the prop/mech
  paths, or retirement of that view-mode dependency.
- 8.7x - OBJECTID COMPOSITE DIRECT CANARY
  (VK-OBJECTID-COMPOSITE-DIRECT-CANARY-1). Added default-off
  `MC2_VK_OBJECTID_DIRECT_CHAIN=1`, guarded by the full composite direct-chain
  stack. When armed for ObjectIdDebug/Thermal composite modes, the native visual
  composite treats objectId MRT2 as already Vulkan-owned and skips the remaining
  objectId GL->VK/VK->GL semaphore handoff. The strict visible harness now includes
  the gate. Verification: first proof accidentally deployed the non-Vulkan
  `build64` lane and fail-softed to GL, discarded. Correct Vulkan proof used
  `build64_island/RelWithDebInfo` and deployed 266 files including
  `shaders/vulkan/*.spv`; after commit/rebuild, the clean receipt
  `.claude/vk-nointerop-strict-visible-objectid-direct-clean/` captured 11/11
  deterministic `mc2_01` bookmarks with
  `MC2_OBJECT_ID_BUFFER=1`, `MC2_VIEW_MODE=objectid`, and
  `MC2_VK_NO_GL_INTEROP_STRICT=1`. Log receipt: `sha=d890a2f2f1f0 dirty=0`,
  Vulkan backend selected,
  objectId(MRT2) active as VK `R32UI`, terrain/prop/mech/water islands all ran,
  `VK_WATER_TIER1 used_interop=0`, `VK_PROP_COLOR_TIER1 used_interop=0`,
  `composite_vk_frames=300`, `gl_handoff_skip_frames=300`,
  `gpu_mech_id_writes=4143`, smoke PASS, and no `[VK_NO_GL_INTEROP_STRICT]`
  abort. Correctness follow-up remains full native objectId MRT2 writes/clear in
  terrain/prop/mech/water if ObjectIdDebug pixel parity is promoted from canary to
  product requirement.
- 8.7y - POST FOG DIRECT CHAIN
  (VK-POST-FOG-DIRECT-CHAIN-1). Added default-off
  `MC2_VK_POST_FOG_DIRECT_CHAIN=1`, guarded by the full
  terrain->prop->mech->water direct-chain stack. When armed with
  `MC2_VK_POST_FOG_NATIVE=1`, the fog pair treats shared scene color/depth as
  already Vulkan-owned, skips both GL->VK and VK->GL handoffs, renders in place,
  and leaves the resources Vulkan-owned for native composite/present. The strict
  visible harness now includes both fog gates. Verification:
  `.claude/vk-nointerop-strict-visible-postfog-direct-clean/`, 11/11
  deterministic `mc2_01` bookmarks; Vulkan backend selected; fog initialized on
  the shared engine context; clean receipt `sha=35c56c4c362d dirty=0`;
  health receipt `attempted=662 used_vulkan=662 direct_chain=1 used_interop=0
  gl_skipped=1324`; present
  `composite_vk_frames=600 gl_handoff_skip_frames=600`; water/prop
  `used_interop=0`; no fail-soft GL fallback and no `[VK_NO_GL_INTEROP_STRICT]`
  abort. Next same-pattern candidates were SSAO, then screenfx/HZB direct-chain gates.
- 8.7z - POST SSAO DIRECT CHAIN
  (VK-POST-SSAO-DIRECT-CHAIN-1). Added default-off
  `MC2_VK_POST_SSAO_DIRECT_CHAIN=1`, guarded by the full
  terrain->prop->mech->water direct-chain stack. When armed with
  `MC2_SSAO=1` and `MC2_VK_POST_SSAO=1`, native SSAO treats shared scene
  color/normal/depth as already Vulkan-owned, skips all GL->VK and VK->GL
  handoffs, renders the VK-owned half-res AO intermediate, multiplies into
  shared color0 in place, and leaves the resources Vulkan-owned for later
  native fog/composite/present. The strict visible harness now includes the
  SSAO master gate and both native SSAO gates. Verification:
  `.claude/vk-nointerop-strict-visible-postssao-direct-dirty-ssaoon/`, 11/11
  deterministic `mc2_01` bookmarks; `SSAO v1 enabled=1 debug=0`; native SSAO
  initialized on the shared engine context; health receipt `attempted=661
  used_vulkan=661 direct_chain=1 used_interop=0 gl_skipped=661`; fog stayed
  direct-chain with `used_interop=0`; present `composite_vk_frames=600
  gl_handoff_skip_frames=600`; water/prop `used_interop=0`; no
  `[VK_NO_GL_INTEROP_STRICT]` abort. The prior
  `.claude/vk-nointerop-strict-visible-postssao-direct-dirty/` run omitted
  `MC2_SSAO=1`, so `attempted=0`; keep it only as a crash regression, not proof.
- 8.7aa - POST SCREENFX + HZB DIRECT CHAIN
  (VK-POST-SCREENFX-HZB-DIRECT-CHAIN-1). Added shared helper
  `mc2vkgate::postDirectChainUpstreamOn()` and default-off direct-chain gates
  `MC2_VK_POST_SCREENFX_DIRECT_CHAIN=1` and
  `MC2_VK_POST_HZB_DIRECT_CHAIN=1`. Screenfx now skips GL<->VK handoff at both
  native screen-shadow and cloud/shoreline sites when the full upstream
  terrain->prop->mech->water chain is armed; HZB does the same for color/normal/
  depth resources. Fog and SSAO now reuse the shared helper instead of carrying
  local copies of the upstream predicate. HZB also defers native execution for
  the first observed extent/mip-count frame so present swapchain resize happens
  before the HZB native allocation/render path. Verification:
  `.claude/vk-nointerop-strict-visible-postscreenfx-hzb-direct-dirty-cleanrun/`,
  11/11 deterministic `mc2_01` bookmarks; present `composite_vk_frames=600
  gl_handoff_skip_frames=600`; screenfx `attempted=1324 used_vulkan=1324
  shadow_frames=662 shore_frames=662 direct_chain=1 used_interop=0`; HZB
  `attempted=662 used_vulkan=662 direct_chain=1 used_interop=0 levels=12
  vk_draws=12 copy_depth_ops=0 readback_ops=0`; fog/SSAO/water/prop all
  `used_interop=0`; no present fail-soft and no `[VK_NO_GL_INTEROP_STRICT]`
  abort. Isolation receipts: screenfx-only clean at
  `.claude/vk-nointerop-strict-visible-screenfx-direct-dirty-isolate/`; HZB
  resize failure reproduced at `.claude/vk-nointerop-strict-visible-hzb-direct-dirty-isolate/`
  before the defer, then passed at
  `.claude/vk-nointerop-strict-visible-hzb-direct-dirty-defer/`.
- 8.7ab - SHADOW DIRECT CHAIN STRICT PROOF
  (VK-SHADOW-DIRECT-CHAIN-1). Added default-off
  `MC2_VK_SHADOW_DIRECT_CHAIN=1`: when the VK shadow/CSM producers are armed
  with same-frame native VK consumers, the shadow island skips its VK->GL
  semaphore signal/wait and keeps shadow products VK-owned. The dynamic
  single-map producer still renders/fences and exposes its direct `VkImageView`;
  under this proof gate it returns no GL texture handle so no GL sampler can
  silently consume the product. CSM array/full-map producers continue feeding
  terrain and native CSM screen-shadow through direct VK views. The strict
  visible harness now arms dynamic CSM shadows, static shadow substrate, native
  CSM screen-shadow, and the shadow direct-chain gate. Verification:
  `.claude/vk-nointerop-strict-visible-shadow-direct-clean-final/`, 11/11
  deterministic `mc2_01` bookmarks; log receipt `sha=53311e5aced3 dirty=0`,
  `MC2_VK_SHADOW_DIRECT_CHAIN=1 -- VK shadow products stay VK-owned; GL
  semaphore wait is skipped`; first VK shadow, CSM array, and CSM full-map
  products produced; present `composite_vk_frames=6600
  gl_handoff_skip_frames=6600`; zero occurrences of `[VK_NO_GL_INTEROP_STRICT]`,
  `glInteropSignalToVk`, or `glInteropWaitFromVk`. Detailed post-health receipt:
  `.claude/vk-nointerop-strict-visible-shadow-direct-dirty-receipts/`, screenfx
  `shadow_frames=6629 direct_chain=1 used_interop=0`, HZB/fog/SSAO/water/prop
  all `used_interop=0`.
- 8.7ac - RESOURCE-LEVEL NO-GL-INTEROP STRICT CANARY
  (VK-NO-GL-INTEROP-RESOURCE-STRICT-1). Added default-off
  `MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT=1` plus
  `scripts/run_vk_nointerop_strict_visible.py --resource-strict`. This is the
  stronger Metal-gate canary: it aborts on exportable Win32 `VkImageResource`
  creation and `glInteropViewCreate`, not only the production
  `glInteropSignalToVk`/`glInteropWaitFromVk` handoff calls. It is expected to
  fail until shared scene color/depth/static shadow/present stop allocating
  Win32 external-memory resources and importing GL memory objects.
- 8.7ad - STATIC SHADOW IMPORT OPTIONAL FOR CSM DIRECT STRICT STACK
  (VK-STATIC-SHADOW-IMPORT-OPTIONAL-1). The strict visible harness now forces
  `MC2_VK_STATIC_SHADOW_MAP=0` instead of letting the shared-GBuffer umbrella
  arm the old VK-backed GL-imported static shadow texture. Native screen-shadow
  now accepts `sStaticShadowRes == null` when CSM direct views are ready: binding
  2 gets a valid depth descriptor, `enableShadows=0` disables the static branch,
  and the CSM shader still samples direct VK CSM array/full-map views. Dirty
  proof: `.claude/vk-nointerop-strict-visible-static-off-dirty/`, 11/11
  deterministic `mc2_01`; no `VK_SHARED_STATIC_SHADOW` log lines; screenfx
  `shadow_frames=4687 direct_chain=1 used_interop=0 fallback_reason=""`; no
  `[VK_NO_GL_INTEROP_STRICT]` abort. This removes one exportable Win32 image +
  GL memory-object import from the strict stack; shared scene color/depth/present
  remain the resource-strict blockers.
- 8.7ae - STRICT HARNESS NOW COVERS NATIVE UI TAIL
  (VK-NO-INTEROP-STRICT-UI-TAIL-1). Extended
  `scripts/run_vk_nointerop_strict_visible.py` to arm `MC2_VK_HUD=1`,
  `MC2_VK_HUD_COLOR_PROOF=1`, `MC2_VK_HUD_SKIP_GL=1`, and `MC2_VK_IMGUI=1`
  alongside the existing post/composite/present direct-chain gates. This closes
  the prior proof gap where strict no-interop visible runs proved the native post
  stack but not the already-shipped native GameOS HUD and ImGui final-tail path.
  Clean deployed binary proof at `d5faad0feb69`:
  `.claude/vk-nointerop-strict-visible-ui-tail-long-dirty/`, 11/11 deterministic
  `mc2_01`; `[VK_HUD_SKIP v1] ... reason="native_full_coverage"` throughout;
  final present receipt reached `present_frames=8100`,
  `copy1_skip_frames=8100`, `gl_handoff_skip_frames=8100`,
  `tail_hook_frames=8100`, `composite_vk_frames=8100`,
  `hud_vk_consumed_frames=7258`, `hud_vk_color_draws_last=95`,
  `hud_vk_textured_draws_last=69`, `imgui_vk_ready=1`,
  `imgui_vk_frames=8100`, `fallback_reason=""`; no
  `[VK_NO_GL_INTEROP_STRICT]` abort and no `VK_SHARED_STATIC_SHADOW` lines.
- 8.7af - RESOURCE-STRICT FINAL STACK PROOF
  (VK-NO-GL-INTEROP-RESOURCE-STRICT-PASS-1). The full native geometry/post/
  composite/HUD/ImGui/present stack now passes the stronger resource-level
  Metal-gate canary with `MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT=1`,
  `MC2_VK_PRESENT_NO_GL_IMPORT=1`, and native direct-chain gates armed. Shared
  scene color/depth and present use VK-only allocations in this stack; static
  shadow import remains disabled, and shadow/CSM products stay VK-owned.
  Verification at clean deployed commit `04a15aec`: visible strict capture
  `.claude/vk-nointerop-resource-strict-checker-automated/`, 11/11
  deterministic `mc2_01` bookmarks. The reusable checker
  `scripts/check_vk_nointerop_log.py` passed against the capture log and
  harness-written `vk_strict_env.txt`: zero `[VK_NO_GL_INTEROP_STRICT]`,
  `glInteropSignalToVk`, `glInteropWaitFromVk`, `glInteropViewCreate`,
  `vkImageResourceCreate(exportable)`, present-native aborts, crash reports, or
  device loss; runtime receipts include `[VK_PRESENT]`, `composite_vk_frames=`,
  `gl_handoff_skip_frames=`, and `imgui_vk_frames=`.
- 8.7am - DELETED RUNG-7 GL-READBACK MECH FEED FALLBACK
  (VK-RUNG8-MECH-FEED-READBACK-DELETE-1, commit `66f5c3e9`). METAL-GATE-AUDIT-2
  8.7 deletion inventory **row 9** ("rung-7 GL-readback mech fallbacks, ~120 LOC").
  First real interop-scaffolding DELETION since 8.7h (8.7i-af were direct-chain
  proof/canary rungs). The native CPU mech feed (VK-MECH-FEED-NATIVE-1) is now the
  **only** mech feed path; the GL `glGetBufferSubData` readback fallback is gone.
  Changes: `gos_mech_batcher.cpp` populates `vboCpu`/`iboCpu`/`lightDataCpu` (and
  persists `s_vkVboNativeCopy`) whenever the VK mech island is active
  (`s_vkMechGateOn`) instead of only under the retired `MC2_VK_MECH_FEED_NATIVE`
  opt-in; `vulkan_mech_island.cpp` `upload_geometry_once` requires the CPU sources
  (declines+retries the frame if a normals rebuild transiently raced the accessor
  -- no VK-buffer alloc first), per-frame lighting is CPU-only (flat-lit that frame
  if unavailable), and both geometry readbacks + the per-frame LightsData SSBO
  readback else-branch were deleted. The FEED-AUDIT byte-compare stays
  (verification-only). **Fail-soft decision (audit risk #1):** the old per-frame GL
  rescue is replaced by explicit fail-hard-with-retry (geometry) / flat-lit-this-
  frame (lighting), never a silent GL fallback. **Gate deprecation (boundary d):**
  `MC2_VK_MECH_FEED_NATIVE` stays parseable but is a no-op; explicitly setting it
  prints a one-line `[VK_MECH_FEED] ... deprecated` note. Pure-GL product path
  untouched (all edits are `#ifdef MC2_VULKAN_ISLAND` / `s_vkMechGateOn`-gated /
  in VK-island-only accessors -- GL-neutral by construction). ~55 LOC net removed
  (both readback branches + the opt-in gate function; the accounting is smaller
  than the ~120 estimate because the native memcpy paths already existed).
  Verification at clean deployed commit `66f5c3e92076` (dirty=0, lane
  `mc2-win64-0.4c`, build `build64_island/RelWithDebInfo`):
  - **Strict no-interop proof re-run, both modes PASS** (hardened checker
    `check_vk_nointerop_log.py --expect-sha 66f5c3e92076`): handoff-strict
    `.claude/vk-nointerop-item9-proof/` VERDICT PASS (present_frames=900,
    `[VK_MECH_TIER1] used_vulkan=957 forbidden_getTexImage=0 forbidden_readPixels=0
    fallback_reason=""`, all islands `forbidden_*`=0); resource-strict
    `.claude/vk-nointerop-item9-proof-rstrict2/` VERDICT PASS (present_frames=900,
    resize_recreate_ok=1). (First resource-strict attempt hit the intermittent
    `VK_ERROR_NATIVE_WINDOW_IN_USE` swapchain flake -> present_frames=0 fail-soft;
    the island health lines were still fully native, and the retry cleared it.)
  - **Item-9 default-umbrella check** `.claude/vk-item9-umbrella-audit/` (mc2_24,
    `MC2_VK_SHARED_GBUFFER=1 MC2_VK_MECH_FEED_AUDIT=1`, no direct-chain gates, no
    FEED_NATIVE -- the config whose default behavior this flips): `mech geometry
    uploaded once: ... source=cpu-native` (was `gl-readback` pre-deletion),
    `FEED-AUDIT geometry: mismatch_buffers=0 (byte-identical)`, `FEED-AUDIT lights:
    byte-identical (mismatches=0)`, `[VK_MECH_TIER1] used_vulkan=216
    inst_records=122 geom_verts=14814 fallback_reason="" forbidden_*=0`.
  - **GL control:** GL-neutral by construction; canonical tier1 gates-off (pure GL)
    5/5 PASS +0 destroys.
  - **Canonical tier1 both states 5/5 PASS +0 destroys** on
    `mc2-win64-0.4c/mc2.exe`: gates-off (GL) and gates-on
    (`MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1 MC2_VK_MECH_FEED_AUDIT=1`).
- 8.7an - UMBRELLA DIRECT-CHAIN DEFAULT + TERRAIN BOOT-DECLINE
  (VK-UMBRELLA-DIRECT-CHAIN-DEFAULT-1). **ITEM-4 PREREQUISITE — DONE.** The
  `MC2_VK_SHARED_GBUFFER=1` umbrella now arms the 10 `*_DIRECT_CHAIN` gates by
  DEFAULT (strict/direct-chain is what umbrella-ON MEANS); the old hybrid
  interop-semaphore path is reachable only by an explicit per-gate `=0` override
  (item-4's deletion target). New resolver `mc2vkgate::directChainGate()` in
  `vk_shared_gbuffer_gate.h` (same precedence as `subGate`: explicit `1`/`0` wins,
  else umbrella). All 10 read sites flipped from `envTriState(...)==1` /
  `getenv(...)=='1'` to `directChainGate()`: terrain-prefix-clear
  (`vulkan_terrain_island.cpp`, `gameosmain.cpp`), prop
  (`vulkan_color_prop_island.cpp`), mech (`vulkan_mech_island.cpp`), water
  (`vulkan_water_island.cpp`), the 5 post passes (`gos_postprocess.cpp` +
  fog/ssao/screenfx/hzb native TUs), composite (`engine_present_vk.cpp`), shadow
  (`vulkan_shadow_island_live.cpp`); `postDirectChainUpstreamOn()` updated too.
  `MC2_VK_OBJECTID_DIRECT_CHAIN` stays explicit (view-mode canary, NOT a default
  member). **Item-2 boot-level native-or-decline (partial):** terrain first-owner
  now latches a SESSION decline on a PERMANENT init failure (past the context/
  shared-GBuffer readiness gates → spv/pipeline/resource build fail) with ONE
  receipt line instead of re-`new`+re-init per frame; TRANSIENT readiness gates
  keep the item-9 fail-hard-with-retry. **Umbrella stays DEFAULT-OFF (flip = user
  decision).** Runner `run_vk_nointerop_strict_visible.py --minimal-promotion`
  drops the 10 direct-chain gates to prove the umbrella arms them; checker H5
  relaxed to reconcile `MC2_VK_*_INTEROP=1` producer names via the umbrella
  (`MC2_VK_SHARED_GBUFFER=1`, not explicitly `=0`) as well as via explicit
  `MC2_VK_SHADOW_DIRECT_CHAIN=1`. **Item 4 is now UNBLOCKED** (the semaphore-
  handoff branches are no longer the default path when umbrella-ON). Remaining
  item-2 per-island boot-decline (prop/mech/water/post/shadow) is follow-up; item
  4's stated prerequisite (direct-chain promoted to default) is satisfied.

- 8.7aq - TERRAIN ITEM-4 CONVERSION — **ITEM 4 COMPLETE, ALL ISLANDS**
  (VK-RUNG8-ITEM4-TERRAIN-DELETE-1, commit `741b9e4f`). Deleted the terrain
  island's hybrid GL<->VK semaphore-handoff arm — the LAST island with one
  (mech `a0cba0c4`, 4 post passes `bc923366`, shadow `2b4160ae`, prop/water
  guard fix `c2c21682` preceded it). The run path lost the `!nativePrefixClear`
  arm (normal/depth BeginFrame, 3x glInteropSignalToVk, waitSems/signalSems +
  gpuC/N/D vk2gl plumbing, 3x glInteropWaitFromVk); the submit waits/signals no
  GL<->VK semaphores (fsC fence orders CPU). init renderpass is unconditionally
  CLEAR loadOp on color0/normal/depth; the GL-view readiness requirement was
  dropped (views still fetched = item-5 substrate, no longer consumed here).
  `MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR` RETIRED to a parseable no-op +
  deprecation note (item-9 pattern); its real meaning is the structural
  predicate `mc2vkgate::terrainNativeFirstOwnerOn()` (= terrain Tier-1 armed),
  rebound at all 4 consumer sites (gameosmain frame-entry clears + clearGBuffer1
  skip, propDirectChainRequested, composite direct-chain, postDirectChainUpstreamOn)
  — PREFIX_CLEAR clear semantics preserved exactly under the umbrella. No
  `MC2_VK_*_INTEROP` producer retired (CSM array/fullmap/shadow feed surviving
  native production); checker H5 unchanged. Verification at clean deployed
  `741b9e4faffb` (dirty=0, lane `mc2-win64-0.4c`, build `build64_island`):
  strict proof `check_vk_nointerop_log.py --expect-sha 741b9e4faffb` **VERDICT
  PASS** (`.claude/vk-item4-terrain-proof/`, 11/11 deterministic bookmarks,
  all islands `used_vulkan>0 forbidden_*=0`, terrain `used_vulkan=1022
  interop_active=0 vk2gl_sem=0 stage_us=3.3`, present/copy1_skip/gl_handoff_skip/
  composite/imgui all =900, copy1_blit=0). Explicit-override check
  (`.claude/vk-item4-terrain-override/`, `MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR=0`
  + fallback audit): deprecation note printed, island STILL native
  (`used_vulkan=1022`, zero strict aborts), audit `gl_fallback=0 edge_stitch=0
  captured_stitch=3` steady (unchanged vs pre-deletion receipt). GL control
  gates-off `--verify` pair: 10/11 byte-STABLE, lone `cross_map_view` DRIFT =
  the documented pre-existing GL pose nondeterminism (same as a0cba0c4's
  control). Canonical tier1 5/5 PASS both states +0 destroys (GL and
  `MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1`).

  Remaining 8.7 deletion inventory (ordered, dependency-aware; **item 4 DONE**):
  item 5 GL-import halves of shared scene resources, item 2 exportable half of
  `vk_image_resource`, item 1 `gl_interop_view.cpp` whole TU, item 7 terrain CSM
  copy-mode, item 8 present interop tail (Win32 surface/copy-2 SURVIVE), item 3
  wantInterop plumbing, item 10 ~20 vestigial gates. NOTE: prop/water hybrid
  arms still EXIST but are reachable only by explicit `MC2_VK_PROP_DIRECT_CHAIN=0`
  / `MC2_VK_WATER_DIRECT_CHAIN=0` debug overrides (their pure-dead-code deletion
  is the deferred low-risk cleanup from the 2026-07-10 guardfix block; they are
  the last glInteropSignal/Wait callers outside gl_interop_view/present). Item 6
  (static-shadow import) and the asset-feed readback layer (audit risk #2) stay
  by decision.

- 8.7ar - ITEM 5 DELETED SHARED-SCENE GL-IMPORT HALVES + PROP/WATER HYBRID ARMS
  (VK-RUNG8-ITEM5-SHARED-SCENE-DELETE-1, commits `7ee0494f` core + `11f9f7fc`
  prop/water + `12fe7c4b` deprecation-note fixup). METAL-GATE-AUDIT-2 8.7 inventory
  **row 5**. The shared scene color/normal/objectId + depth resources are now
  **VK-ONLY (non-exportable)**: deleted the exportable/`glInteropViewCreate`
  allocation branch, the D24S8-preference block (dead — GL never writes these
  images now, so VK-native D32S8 is the only/correct format), the `vkOnlyNoGlView`
  param + `GLuint out` + `HasGlView`/`GetGlView` accessors + the `view`/`viewValid`
  members, and the gos_postprocess GL-import FBO substitution machinery. The former
  `vkOnlyNoGlView` path is the ONLY path.
  - **gos_postprocess**: the shared scene arms ONLY under the full native-present
    stack (`mc2VkSharedSceneDirectResourceEnabled()`); otherwise it declines the
    WHOLE VK shared-GBuffer to pure GL (the GL FBO attachments are plain-GL
    placeholders, never GL-rendered — under item-5 a VK-scene/GL-present hybrid can
    no longer exist, so a config that can't run present-native renders pure GL).
    `destroyFBOs` deletes the placeholder + frees the VK image (HasGlView checks
    removed). MUST-SURVIVE kept: the pure-GL alloc paths + the `glCopyImageSubData`
    scene-color/depth snapshots.
  - **Fail-soft decision (audit risk #1)**: `mc2VkSharedSceneStrictFallback`
    redefined = the boot-decline canary. With no GL-import half, a create() failure
    means the ENTIRE VK shared-GBuffer session declines to pure GL (GL renders its
    own targets, the VK islands fail their VkImageResource-readiness and fall to GL,
    present composites the GL scene). `MC2_VK_SHARED_SCENE_STRICT=1` makes that
    decline a hard abort so Metal-gate proofs can't pass through a hidden decline.
  - **present (engine_present_vk.cpp)**: deleted the native composite's
    `!sceneDirectChain` GL-view hybrid arm (sceneView/objectIdView fetches +
    glInterop signal/wait); composite REQUIRES the shared scene already Vulkan-owned
    (direct-chain) or declines to GL. Present-image copy-2/Win32-surface tail
    (item 8) untouched.
  - **prop + water hybrid-arm deletion (item-3 trigger)**: removing `GetGlView`
    removed the last input to their `!directChain` arms, so they were deleted
    mirroring mech `a0cba0c4` / terrain `741b9e4f` (glInterop signal/wait, fsN/fsD
    BeginFrame, gpuC/N/D vk2gl plumbing, waitSems/signalSems out; submit
    waits/signals nothing; run-entry guards → `disabled || !inited`). Gates
    `MC2_VK_PROP_DIRECT_CHAIN` / `MC2_VK_WATER_DIRECT_CHAIN` RETIRED to parseable
    no-ops + one-shot deprecation notes fired at island init (item-9 pattern).
  - **terrain/mech/fog/hzb/screenfx/ssao**: the now-dead shared-scene GL-view
    fetches (assign-only post item-4) removed; screenfx keeps its item-6
    static-shadow view.
  - **checker** (`check_vk_nointerop_log.py`): FORBIDDEN gains the deleted
    GL-import receipts (`VK-backed GL_*`, `imported to GL_`); new
    `check_shared_scene_vk_only` asserts `(VK-only MRT0)`/`(VK-only depth)` when the
    shared scene is armed.
  - **Verification** at clean deployed `12fe7c4b85b3` (lane `mc2-win64-0.4c`, build
    `build64_island`): resource-strict visible proof
    `check_vk_nointerop_log.py --resource-strict --expect-sha 12fe7c4b85b3` **VERDICT
    PASS** (`.claude/vk-item5-proof-final2/`, 11/11 deterministic bookmarks, all H1
    forbidden strings 0 incl. the deleted GL-import receipts, all islands
    `forbidden_*=0`, ownership `used_vulkan` terrain 1022 / prop 1022 / mech 957 /
    water 1022, VK-only substrate receipts present, present/copy1_skip/gl_handoff_skip/
    composite/imgui all 900, copy1_blit=0). Explicit-override check
    (`.claude/vk-item5-override2/`, `MC2_VK_PROP_DIRECT_CHAIN=0
    MC2_VK_WATER_DIRECT_CHAIN=0`): both deprecation notes printed, zero strict aborts
    (the `=0` override is inert — the hybrid arm is gone). GL control gates-off
    `--verify` byte pair: 10/11 STABLE, lone `cross_map_view` DRIFT = the documented
    pre-existing GL pose nondeterminism (identical to item-4's control). Canonical
    tier1 both states PASS +0 destroys. First strict-proof attempts hit the known
    intermittent swapchain `NATIVE_WINDOW_IN_USE` (VkResult=-1000000001) flake
    (present_frames=0, islands still native); cleared on retry.

  **Zero-caller status for the NEXT items (2 and 1):** with item 5 done, the
  shared-scene GL-view family is gone. `glInteropViewCreate` /
  `vkImageResourceCreate(exportable)` callers remaining = **static shadow** (item 6,
  STAYS), the **shadow island dormant `!directChain` branch** (compiled-not-taken),
  and the **present image tail** (item 8). `glInteropSignalToVk/WaitFromVk` live
  callers = **present tail only** (prop/water were the last others — now deleted).
  Items 2 (`vk_image_resource` exportable half) + 1 (`gl_interop_view.cpp` whole TU)
  are NOT yet at zero callers because item 6 (static shadow) + item 8 (present) still
  use them; they clear after items 6/8. **Remaining 8.7 order: items 7, 8, then 6,
  then 2, 1, 3, 10** (2/1 gated behind 6+8).

- 8.7as - TERRAIN CSM COPY-MODE DELETE
  (VK-RUNG8-ITEM7-CSM-COPYMODE-DELETE-1). METAL-GATE-AUDIT-2 8.7 inventory **row 7**
  DONE. Commit `9e80f139`, deployed `mc2-win64-0.4c` @ `9e80f139cc27 dirty=0`.
  Deleted from `vulkan_terrain_island.cpp` the terrain CSM copy-mode: `copy_csm_array()`
  (per-frame `glGetTexImage(GL_TEXTURE_2D_ARRAY)` depth pull + `glCopyImageSubData` GL
  scratch depth array + staging + `vkCmdCopyBufferToImage`), the CSM full-map
  `copy_shadow_slot(perFrame)` fallback + its FBO save/restore, `csmBisectMode()` +
  all `MC2_VK_CSM_BISECT` probe branches, the `MC2_VK_CSM_COPYMODE=noread` branch, and
  the copy-only state (`csmArrayStage*`, `csmScratchTex/Dim/Layers`, `csmFullMapStage*`
  + destroys). `copy_csm_shadows()` is now DIRECT-VIEW-ONLY: it acquires the VK shadow
  island's csm-array (binding 8) + csm-fullmap (binding 9) `VkImageView`s and declines
  otherwise. ~150 LOC net deleted (211 del / 60 ins incl. comments).
  - **WARMUP DECISION (recorded):** the shadow island does NOT produce from frame 1
    (`first VK CSM array produced` is logged after a short warmup). Direct-only
    therefore takes an **explicit warmup decline**: on csm-not-built frames and the
    ~1-2 frames before the producer's first CSM product, `copy_csm_shadows()` returns
    false -> caller forces UBO `csmEnabled=0` -> frag on the legacy single-map
    dynshadow path (never a hole). This is NOT stale: all ready/external/direct flags
    are reset at the top of `copy_csm_shadows()` every frame and the terrain island
    resets the csm external views every frame, so a declined frame binds the 1x1
    dummies and the next ready frame re-points bindings 8/9 at the live views. This is
    the same mechanism PART A (2026-07-11) audited as ruling out a cross-frame stale
    latch; the deletion does not introduce one (it removes the copy fallback, keeping
    the reset-every-frame decline). Consistent with the user-sighted stale shadow being
    a non-reproduced per-launch warmup/ordering transient, NOT a copy-mode-fed latch.
  - **Gates:** `MC2_VK_CSM_COPYMODE` + `MC2_VK_CSM_BISECT` RETIRED to parseable no-ops
    with one-shot deprecation notes (item-9 pattern). `MC2_VK_CSM_ARRAY_INTEROP` /
    `MC2_VK_CSM_FULLMAP_INTEROP` KEPT — they gate the surviving direct producers
    (checker H5 unchanged: those names reconcile via the umbrella as before).
  - **MUST-SURVIVE kept:** `copy_shadow_slot` (static binding 5 = item 6; dynamic
    binding 7), `ensure_csm_array_image`/`ensure_dummy_csm` 1x1 dummies,
    `terrainPerFrameReadback` + `forbidden_getTexImage` counter (still load-bearing for
    the dynamic single-map fallback + checker trip-wire — its CSM call sites are gone,
    so a CSM readback can no longer increment it, which is correct). The parked
    `MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL` escape hatch is **UNTOUCHED** — it feeds
    the dynamic single-map slot (binding 7) via `copy_shadow_slot` in
    `gos_terrain_lod_chunk.cpp`, NOT the CSM near-array/fullmap copy-mode, so it does
    not ride copy-mode and does not die with it.
  - **Verification** (all at clean deployed `9e80f139cc27`): resource-strict visible
    proof `check_vk_nointerop_log.py --expect-sha 9e80f139cc27` **VERDICT PASS**
    (`.claude/vk-item7-csm-proof/`): 11/11 deterministic bookmarks incl.
    `ridge_lowangle` (shadow_cascade), all H1 forbidden 0 (incl. `PERFRAME_READBACK` 0),
    all islands `forbidden_getTexImage=0`, ownership terrain 1022 / prop 1022 / mech 957
    / water 1022, VK-only substrate, terrain `stage_us=2.3`, present/copy1_skip/
    gl_handoff_skip/composite/imgui = 900, copy1_blit=0, provenance dirty=False sha
    match. Shadow-island receipts: `CSM array target initialized 8192x8192 layers=2
    (direct terrain binding 8)`, `CSM full-map target initialized 4096x4096 (direct
    terrain binding 9)`, `first VK CSM array/full-map produced`. Explicit CSM direct
    counters (`.claude/vk-item7-copystats/`, `MC2_VK_TERRAIN_COPY_STATS=1`):
    `[VK_TERRAIN_COPY_STATS] copies_ms=0.0004 csmArrayReady=1 csmArrayDirect=1
    csmFullMapReady=1 csmFullMapDirect=1`. Zero CSM_COPYMODE/CSM_BISECT deprecation
    notes (gates unset in the proof). Canonical tier1 **5/5 PASS both states +0
    destroys** (GL `2026-07-11T00-51-05`; VK umbrella `2026-07-11T00-54-33`).
  - **Remaining 8.7 order:** items 8, then 6-decision, then 2, 1, 3, 10.
