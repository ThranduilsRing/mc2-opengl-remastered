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

## Amendments (2026-10-17, lane 2 s52, from the orchestrator and the Astra post-modernization review)

### New step 3.4P (gate for the rest of 3.4 and all of 3.5): native VK producers for water / terrain lighting / culling
Several scene inputs that VK consumes are still produced by GL compute (11 GL-only compute shaders; no VK compute pipeline exists for them): water, terrain lighting, culling (cluster depth pyramid, light grid, terrain indirect compute). Closed visual-parity rows prove the VK picture matches, not that VK is independent of GL.
- Rule: do NOT delete a GL producer whose output a VK pass still consumes. Before each 3.5 batch (and the interop removal that touches it), list the producer's consumers (grep + `repo_query.py binding`); if any VK consumer exists, the batch first needs a native VK producer (VK compute pipeline or CPU producer) landed ON by default.
- Order: (1) culling / light grid / cluster depth pyramid, (2) terrain lighting, (3) water. Each: VK compute pipeline + SPIR-V, parity vs the GL producer's output (buffer diff or captured frame at a day and a night map, viewed), VK tier1 under validation 0, then the GL producer is deletable.
- Finding this session: the `glGetTexImage` calls still in the tree are GL-side self-tests/uploads (cluster depth pyramid, light grid, blur self-checks, texture finalize, atlas), NOT readbacks into VK islands (the island counters `g_forbidden.glGetTexImage` assert zero). So 3.4(b) is mostly already true; the real dependencies are the producers above.

### 3.4P status (s53)
Water cull/pack and terrain lighting are native VK compute (Slang) and ON by default under the umbrella; the water readback is gone. Proof: GL-vs-VK thin-record byte parity on three maps, lighting self-test vs a C++ transcription, captures viewed, tier1 validation 0. Remaining producers from the list (culling, light grid, cluster pyramid) have NO VK consumer (see lane2-state s53 findings), so they need no port and are deletable with their GL consumers in 3.5.

### 3.3 comparison tooling (note, not a restore)
3.3 deleted the GL-vs-VK feature-coverage / twin / source tools and the GL leg of the proof matrix. What remains for proving producer moves: `ab.py --skip-gl` / `capture_ab.py` (GL arm pinned `MC2_RENDER_BACKEND=gl`, valid while the opt-out exists), `bless.py --backend gl`. If a producer move needs a per-buffer GL-vs-VK diff, build it for that move; the deleted twin-map tools are in git history (commit before 04c35b397) if wanted. The GL opt-out must therefore stay until 3.4P is done.

### Fade / letterbox (3.2 ruling 2)
Marking the quads IsHUD can change compositing order. Proven: the letterbox bars (VK captures viewed, matches GL after the shrink exemption). UNPROVEN: the fade quad at partial alpha (needs an ABL `fadeToColor` capture; no bookmark triggers it). Add a bookmark or scripted fade before 3.9.

### Blocker found in s52: the editor links the whole GL scene family
`gameos_editor` (GameOS/gameos/CMakeLists.txt:445) compiles the same sources as `gameos` (minus gos_render.cpp), and the editor renders terrain/props/post through that GL code on its private GL (owner ruling 3). So every 3.5 whole-file deletion (post/sky, shadows, props, terrain, water) also removes the editor's renderer. 3.5 therefore cannot start before either (a) 3.6 (editor on its VK viewport), or (b) the editor gets its own frozen copy / a reduced GL path. Owner question 5 (open): is 3.6 pulled ahead of 3.5, or is the editor's GL set split out first? 3.4 as achievable now is finished: the lost-draws tracker, the producer query, the GL canary gates and the three INTEROP kill switches are gone; the remaining `glGetTexImage` calls are GL-side (uploads / self-tests), the umbrella cannot be unconditional while `MC2_RENDER_BACKEND=gl` exists, and the discarded GL scene FBO is the content of 3.5.
