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
- 8.7 - PARTIAL DELETE-READINESS, not a safe all-interop deletion yet. Native
  ImGui, native composite Visual/FXAA/FORCE-43/LowLight/ObjectId/Thermal, and
  native GameOS HUD are now live under the full rung-8 stack. With
  `MC2_VK_HUD=1 MC2_VK_HUD_COLOR_PROOF=1 MC2_VK_HUD_SKIP_GL=1`, visible proof
  `.claude/vk-hud-skip-gl-visible-3/` reached `[VK_HUD_SKIP v1] frame=120
  draws=95 reason="native_full_coverage"` and `[VK_PRESENT] present_frames=301
  composite_vk_frames=300 hud_vk_queued_frames=170 hud_vk_consumed_frames=170
  hud_vk_draws_last=95 hud_vk_textured_draws_last=69 imgui_vk_frames=301
  validation_errors=0 fallback_reason=""`. HUD is no longer the blocker for
  GL draw retirement in the present tail.

  Remaining blockers are narrower and concrete. The shared scene
  color/depth/normal substrate still uses Win32 GL/VK external-memory and
  semaphore handoffs for current native geometry/post paths, so
  `gl_interop_view.*`, `vk_image_resource.*`, and `vulkan_shared_scene_*` are
  not yet deletable. Old postprocess island/subgraph TUs and gates are bypassed
  by the shared-GBuffer native gates and are candidates for a separate fallback
  and oracle cleanup slice, but deletion must remove the CMake entries,
  `gos_postprocess` routes, debug-dump health sections, smoke allowlist entries,
  and docs together.
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
  Verification target: clean build/deploy/smoke after commit with Tier-1 prop
  receipts still showing shared-GBuffer co-write and zero forbidden readbacks.
