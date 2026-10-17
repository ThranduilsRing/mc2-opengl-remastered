<!-- doc-status: current | written 2026-10-17 (lane 2 session 50) -->
# GL deletion, step 3 and beyond: ordered plan (2026-10-17)

State at writing: VK parity rows 1-29 all closed (owner, 2026-10-17); step 1 (capture/tooling GL arms) and step 2 (debug views, diagnostics, object parity,
hitch, screenshot) done except two items the owner ruled KEEP/LATER (lost-draws tracker: goes with the GL interop; debug views: terrain `tessDebug` goes with
GL terrain). Facts that shape the order (checked in the tree, not from memory):

- The default backend is still GL: `render_backend_select.cpp` `resolveOnce()` returns GL on unset; `MC2_VK_SHARED_GBUFFER` (the umbrella) is
  `Exact1`, default false (`Gates/gate_table.cpp:1317`). VK is reached only by `MC2_RENDER_BACKEND=vulkan` + `MC2_VK_SHARED_GBUFFER=1`.
- Even under VK the engine still creates an SDL GL window + context and renders legacy `gos_*` draws into GL FBOs; "GL/VK interop" = that context plus the
  `glGetTexImage` / `glGetBufferSubData` readbacks feeding VK islands and the lost-draws tracker.
- `forceGLFallback` has 4 callers (init x2, frame loop x2). The owner ruling (2026-10-16): failed VK init shows a clear message and exits.

Order is by dependency. Size: S < 1 session, M 1-2, L 3+ (lane 2 Sonnet sessions).

| # | Step | Size | Proof | Blockers / owner questions |
|---|---|---|---|---|
| 3.1 | VK init failure is fatal: delete `forceGLFallback`, `g_mc2VkSessionForcedGL`, the 60 s present-retry budget and "not VK-owned" fallback; clear message + exit | S | build + doctest; VK tier1 under validation; a `MC2_VK_FORCE_FAIL=1` run shows the message and a non-zero exit; GL tier1 compiles/runs (GL code paths untouched) | none (owner ruling exists). DONE in this session. |
| 3.2 | VK default flip (D1): unset backend -> vulkan, umbrella default ON, `MC2_RENDER_BACKEND=gl` kept as the temporary opt-out; every tool that relied on "unset = GL" gets an explicit arm | M | tier1 5/5 + tier2 on the default binary with no env, validation 0; day + night A/B viewed; `verify.py` on the VK golden; flags.auto.yaml / registry agreement check | Q1 (owner): flip now with GL opt-out kept, or only at release? Editor exe shares `render_backend_select` and must stay GL (3.6) |
| 3.3 | Retire the GL half of the proof matrix: tier1/tier2 default to VK, drop the GL leg and the smoke `capture_backend` mismatch check, `check-gl-shader-variants`, `glsl_port_diff`, `gl_vk_feature_coverage`, `vk_not_ported.allowlist`, `tests/shader_variants` | S-M | pytest, `check-contracts.sh`, harnesses; each deleted tool had a consumer list in inventory section 12 | after 3.2 |
| 3.4 | Remove the GL/VK interop: ordered per readback consumer. (a) lost-draws tracker (owner: delete with interop), (b) `glGetTexImage` depth/normal/colour readbacks into VK islands, (c) `MC2_VK_*_GL_CANARY`, `MC2_VK_SCENE_GL_PRODUCER_QUERY`, `MC2_VK_*_INTEROP` gates, `vk_umbrella_light`/`MC2_VK_SHARED_GBUFFER` folded away (umbrella becomes unconditional), (d) the GL scene FBO that VK discards | L | per-consumer: grep row "zero `glGet*Tex/Buffer` calls outside X"; VK tier1 validation 0; capture oracle + viewed day/night | needs 3.2. Q2: the legacy `gos_*` draw path (`gameos_graphics_draw.cpp`, `vulkan_legacy_shape_island`) must be fully VK first: in-scene letterbox / fade quads that are not `IsHUD` (checklist section 1) |
| 3.5 | GL-only scene passes deleted whole-file in inventory order: post/sky (sec 2), VFX (6), shadows (1), mechs (5), props/cull/light grid (4), terrain+water (3), textures (8) | L x several | per batch: build, `build_all_targets`, doctest, tier1+tier2 on VK, VK captures viewed | 3.4 (producers must be backend-neutral first); inventory open questions 4, 5 (does the VK colour-prop feed need the GL cull's visible set; VK terrain island LOD scope) |
| 3.6 | Editor on VK only: the MFC editor viewport draws terrain through the GL path today; port to the editor-rework VK viewport (inventory target 2.10) | L | editor smoke (15/15 baseline) on VK; viewed capture | Q3 (owner): editor on VK is gated on the editor rework; is it in scope before GL deletion or does the editor keep a private GL until its rework lands? Without an answer, step 3.7 cannot finish |
| 3.7 | In-repo tools: `asset_viewer` (6.4k lines, 444 GL calls), `mc2fx_preview`, `mc2weapon_gui` | M | build_all_targets | Q4 (owner): port to VK or retire (inventory open question 2) |
| 3.8 | GL-only shaders: delete `shaders/*.vert/.frag/.comp` GLSL that no VK pipeline loads, `gl_shader_variants.json`, `shaders/spv` reflection of GL shaders (VK SPIR-V stays), `utils/shader_builder`, `gl_utils` | M | `check-spirv-*`, shader compile checks, binding-conflict query | after 3.5 |
| 3.9 | Present, renderer core, state guard: SDL GL context -> SDL Vulkan window, GLEW, `gl_state_guard`, GL bodies of the `gos_*` API, `RenderCore` GL tokens, `check-no-raw-gl-from-game.sh` | L | everything above + second VK best-practices review (queue item 14) as acceptance | all of the above |

Goldens: `CURRENT_GOLDEN` is already a VK set; GL golden sets were archived to E: in step 1.

## Done in this session
3.1 (see lane2-state.md, fiftieth section).

## Owner questions
1. Flip the default (3.2) before GL is deleted, keeping `MC2_RENDER_BACKEND=gl` as the opt-out, or hold the flip to the release step?
2. Is in-scene non-HUD draw (letterbox / fade quads) acceptable to port by marking them `IsHUD` (checklist section 1 sketch), so the legacy path has no GL-only draws?
3. Editor: may it keep a private GL until the editor rework, or must the VK viewport land before 3.9?
4. `asset_viewer`, `mc2fx_preview`, `mc2weapon_gui`: port or retire?
