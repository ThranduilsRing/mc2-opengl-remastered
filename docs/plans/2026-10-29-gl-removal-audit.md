# GL removal audit — 2026-10-29

Baseline: `348444096b7ce5bf24eed18ce343df73ba8292a3` (`claude/nifty-mendeleev`), audited in `mc2-lane9`.
Report only: no engine changes, build, deploy, captures, smoke, or runtime validation performed.
Verdict: **the owned GL execution backend is already removed at this baseline**; remaining work is retirement evidence and residue cleanup.
“Every” below covers tracked first-party engine/editor/tool sources, shader assets, gates and CMake; vendor SDK declarations are listed separately.
Counts are physical lines including comments/blanks at the baseline; whole-file counts do not imply whole files are deletable.

## 1. Remaining GL paths and callers

| Inventory / physical lines | Current runtime caller / disposition |
|---|---|
| `GameOS/gameos/gl*`: **0 files, 0 lines** | No caller. Old GL renderer/pass/loader files cited in retirement docs are absent. |
| GL context/loader / GL draw entry points: **0 executable call sites found** in owned C++ | Searches for SDL_GL, WGL, GLEW initialization, GL draw/bind/create and GL includes found comments/diagnostic text, not a context or dispatch owner. |
| Owned CMake GL linkage/context dependencies: **0 active matches** | No `OpenGL::`, `opengl32`, GLEW or ImGui OpenGL backend link/source dependency found. SDL's bundled API surface is not evidence of a GL context. |
| `GameOS/gameos/render_backend_select.cpp` **69**, `.h` **35** | Startup reads `MC2_RENDER_BACKEND` once; `gl`/`GL` only produce the ignored-value receipt (`.cpp:35-41`). `MC2_VK_SHARED_GBUFFER=0` also only warns (`:31-32`; one table row at `Gates/gate_table.json:724`). Fatal VK failure remains shared functionality (`:49`); do not delete the file wholesale. |
| `GameOS/gameos/gos_host_vk.cpp` **304** | Game and editor call `initVulkanBackend`; `:93-95` checks loaded module handles and prints `[NO_GL_RUNTIME] context=none`. This is a diagnostic, not a GL loader. Presentation uses VK or fatal failure (`:299`). |
| `GameOS/gameos/engine_present_vk.cpp` **2921** | Present initialization reads `MC2_VK_PRESENT_SKIP_GL_HANDOFF` (`:288-290`) only to warn that it is deprecated. No handoff remains. Gate: one row each in `Gates/gate_table.json:713`, `gate_rows_4.inc:153`, `gate_index.gen.h:724`; generated docs at `docs/flags.auto.yaml:5464`. |
| `RenderCore/RendererFeatureRegistry.h` **864** | GL-named capability fields (`:41-56`), Reverse-Z wording (`:198`) and diagnostic dump (`:827`) remain metadata. Repository searches found no external caller of `debugDumpFeatureRegistry`; not an active GL capability query. Preserve active feature entries. |
| `GameOS/gameos/mc2_hitch_trace.h` **158**, `.cpp` **188** | Live profiling retains GL-era labels/comments (`.h:6,34,42,59,147`); no GL wrapper/dispatch implementation. Rename only after checking log consumers. |
| `GuiRuntime/GuiRuntime_draw.cpp` **312** | `DrawUiImage` (`:115-135`) still calls its opaque image argument `glTextureName`; it passes an ImTextureID, not a GL call. OpenGL callback discussion (`:136-149`) is historical. |
| `editor/EditorGosRender.cpp` **946**; `tools/common/tool_vk_shell.h` **69** | Editor creates the native child window (`:547`), not a private GL context. Tool shell explicitly implements Vulkan surface/device/swapchain/ImGui (`:1-5`). GL mentions are historical. |

Other GL tokens in RenderCore contracts, GPU ABI comments, texture handles and `gos_*` CPU feeds are **not GL-only files**.
Examples: `ambient_contract.h`, `postprocess_subgraph.h`, `PipelineDesc.h`, `KtxLoader.h`, terrain/prop/mech batchers, texture decode and VK island comments.
Preserve their VK callers; name-based deletion would remove scene data, UI images or render contracts.
`editor/EditorGameOS.cpp:330` calls shared VK initialization and `:719` VK shutdown; its `:645,661` GL instructions are stale comments.
No live GL-only feature gate remains identified. `MC2_RENDER_BACKEND`, `MC2_VK_SHARED_GBUFFER` and `MC2_VK_PRESENT_SKIP_GL_HANDOFF` are compatibility diagnostics above.
`MC2_GLSTATEGUARD_LOG` / `_TERRAIN` are already retired (`Gates/retired_names.json:61-62`).
`MC2_GLTF_*`, `MC2_MECH_GLASS_*`, global-pool and glow knobs are asset/material controls, not OpenGL switches.

### Shader inventory (GLSL syntax is not a backend)

| Path / exact counts | Runtime consumer / retirement decision |
|---|---|
| `shaders/*.{vert,frag,comp}` directly at root: **0 files / 0 lines** | Former GL stages already gone. No GL runtime shader compiler/loader identified. |
| `shaders/vulkan/**`: **99 files / 6918 lines** | Native VK pipelines; CMake emits SPIR-V (`GameOS/gameos/CMakeLists.txt:351-496`). **Keep**, including `.glsl` includes. |
| `shaders/include/*.hglsl`: **17 files / 1079 lines** | Shared VK/ABI includes, not a GL-only shader family; consumers include `color_prop_tier1.vert:31,42,61-62` and `hzb_cull.comp:4-5`. Keep consumer-by-consumer. |
| `shaders/fixtures/material_gpu_contract.frag`: **35 lines** | Offline reflection fixture (`tools/shader_reflect/reflect.py:66-68,104-169`), no runtime caller; keep ABI proof. |
| `shaders/spv/`: **10 files**, six JSON **1861 lines**, four binaries **79560 bytes** | Package explicitly describes a native-VK offline verification subset (`spirv_package.json:11-22`). Consumed by shader-package/reflection/artifact/pipeline-key checks; not dead GL assets. Binary line counts are inapplicable. |

The 17 include files, with individual lines: `axis` 19, `edge_haze` 12, `gpu_abi_mech_instance.gen` 19,
`gpu_abi_object_lights.gen` 18, `gpu_abi_static_prop_instance.gen` 16, `light_pool_attenuation` 122,
`material_gpu` 57, `mech_render_flags` 4, `mech_shading` 86, `noise` 61, `object_light_pool` 120,
`render_contract` 166, `surface_material` 47, `terrain_depth_bias` 70, `terrain_light_pool` 132,
`terrain_mat_layers` 14, `water_shore_edge` 116 (all `.hglsl`).
SPV names: `mech.frag.default.dbbae7d3.spv` 42112 B, `mech.vert.default.dbbae7d3.spv` 21652 B,
`postprocess.frag.default.24eb2442.spv` 14632 B, `postprocess.vert.default.24eb2442.spv` 1164 B;
their JSON sidecars are 1070/547/84/64 lines; `spirv_index.json` 37 and `spirv_package.json` 59.
The offline builder's GL introductory comment is stale; its compilation/package logic targets Vulkan (`build_variants.py:12,216-217`).
Runtime islands load SPIR-V (`vk_island_util.h:43-49`, `engine_present_vk.cpp:320-325`), not GLSL; `MC2_VULKAN_SPV_DIR` stays live.
`MC2_SHADER_SPIRV` / `_FATAL` are retired (`retired_names.json:123-124`); `MC2_SHADER_HOT_RELOAD` is a legacy banner knob, not a GL compiler.
VK reload itself uses `shader_reload.request` (`vk_render_infra.cpp:414,562-563`); tool shaders use `MC2_TOOL_SHADER_DIR` (`tool_vk_gpu.cpp:183-191`).

Vendor-only GL declarations under `3rdparty/include/SDL2/`: `SDL_opengl.h` 2132, `SDL_opengl_glext.h` 13213,
`SDL_opengles.h` 39, `SDL_opengles2.h` 52, `SDL_opengles2_gl2.h` 656, `SDL_opengles2_gl2ext.h` 4033,
`SDL_opengles2_gl2platform.h` 27, `SDL_opengles2_khrplatform.h` 311. No owned include/caller found; leave vendor SDK intact.
Vendored ImGui Vulkan GLSL, GLM and glTF/GLB import code are Vulkan/math/content dependencies, not removable GL backends.
Historical diagnostic FIT strings and printf-corpus tests mention SDL_GL/WGL/GLEW but cannot execute those APIs.

## 2. Parity claims versus present evidence

| Record | Audit disposition / remaining obligation |
|---|---|
| `.claude/VK-PARITY-GAPS.md:1` | Authoritative latest header: **all 29 rows closed**, no open default parity gap. Body/footer contains older contradictory states (`:28,80`); do not reopen rows from those alone. |
| Rows 1-17, 19-28 | Closed by port, proof, no-default-gap, or reference decision: night pools, mech shading, ambient floor, shadow fade, cement/overlay, shore edge, fog, materials, vehicles, props, post, POM/cliffs, emission, building rebake, options, alpha/highlight/seams/shoreline. Rows 20/24 deliberately follow VK because GL shadow compare was wrong. |
| Row 18 (`:38`) | **Accepted deletion, no port**: cliff-wall mesh decal fill/colour blend; no stock content proof. Mods can contain MarbleCliff/custom decal instances: this is a compatibility decision to document, not proof of full custom-content parity. |
| Row 29 (`:49`, header) | Owner accepted blockier beam-head shape after missing-head/halo fixes. Historical row says GPU particles default OFF; current `Gates/gate_table.json:208` says ON and CPU routes retired. Preserve accepted residual; validate current default VFX rather than claiming an available MLR/GL arm. |
| Historical optional effects (`:60-71`) | Soft/lit/blackbody/distortion/age-fade particles, water reflection/shine/skytint/meander, billboard facing and optional material/post controls were not all default-parity promises. Reconcile active consumers before declaring supported or deleting knobs; vegetation is now ON (`gate_table.json:659`), not the old OFF claim. |
| `docs/known_issues.md:49-54,64-66` | July VK field-config says shadows OFF/no HDRI and GL-state contract missing. Stale as a GL blocker: VK shadow owner exists (`gameos_graphics_shadow.cpp:1`), HDRI uploads/draws (`vulkan_sky_island.cpp:176-233,471`). No fresh capture here certifies their current quality. |
| `known_issues.md:71,74,83-84,119-127,151-152` | Shadow cull/foliage alpha, shoreline/pan, editor picking/smoke and legacy MLR concerns remain references to triage; many cite removed GL paths. Current shadow island has alpha-array binding (`:199-224`). Treat as unverified issue claims, not demonstrated VK-vs-GL gaps. |
| `.claude/cleanup/vk-default-flip-checklist.md:19-43,49-58` | October 8/12 recon is superseded on backend choice, ports, capture source, GL fallback and GL A/B. Letterbox/fade/movie/FORCE_43, menus/HUD/video, minimize/retry/failure, goldens and validation remain useful acceptance cases, not open port assignments. |
| `.claude/cleanup/gl-deletion-step3-plan.md:47-57` | Native lighting/water producers were already recorded complete; its remaining private-GL editor blocker is superseded by current editor VK bring-up. Partial-alpha fade was explicitly unproven: keep an evidence obligation until a later receipt is located. |
| `docs/modding/renderer-feature-flags.md:19-25` | Still describes shadows/HDRI/GPU particles as OFF; current gates disagree. Update mod-facing supported features and shader override guidance together with retirement evidence. |

## 3. Ordered cleanup/sign-off packets (each <=1 workday)

These are proposed future changes, not changes or proofs performed by this audit. Split further if a packet exceeds one day.
For any behavior change: covering offline contracts/doctest first, RelWithDebInfo targeted build, then the domain proof below.
Use existing harnesses, lane install and JOBS budget; no new general-purpose framework or GL resurrection.
`ab.py` compares **VK baseline versus VK candidate** using `mc2_01_saved_205644` and `mc2_10_saved_204523`; view montage/crops, not only MAD.
For each render packet require `MC2_VULKAN_VALIDATION=1`, **0 validation errors / 0 VUID errors**, plus smoke-lite.
Tier1 / all-target merge acceptance belongs to the merge window; doc-only packets need only `git diff --check`.

| Order / budget | Change and dependency | Proof / acceptance |
|---|---|---|
| 1 / 0.5d | Reconcile known_issues, parity footer and default-flip/step3 docs against current ownership; preserve historical evidence and accepted rows 18/29. | Citations agree with source/gates; `git diff --check`; no invented parity reopenings. |
| 2 / 0.5d | Remove deprecated skip-handoff warning/gate and generated rows after enumerating scripts/config consumers; keep ignored-backend receipt if external launchers rely on it. | Gate generation/consumer checks, targeted build, startup receipt, smoke-lite, validation 0; VK day/night A/B if present behavior changes. |
| 3 / 0.5d | Clean GL capability/profiling/UI-handle wording in small independent edits; preserve ABI, opaque handles and log consumers. | Relevant registry/log/ABI contracts; editor and tool targets for shared declarations; smoke/validation/A/B for any routing change. |
| 4 / 1d | Close display acceptance evidence: partial-alpha fade over scene+HUD, movie/letterbox, FORCE_43, menu/video/cursor, resize/minimize recovery and fatal VK-init case. | Existing off-screen harness captures against pinned VK baseline, failure exit/message; day/night A/B, smoke-lite, validation 0. No GL comparison available in current exe. |
| 5 / 1d | Editor acceptance: stock terrain/props/shadows, generated terrain, selection/picking after resize, save/reopen. Keep shared GameOS CPU producers. | Existing editor smoke plus viewed viewport A/B, validation 0; identify pre-existing flaky cases explicitly. No desktop focus/cursor grab. |
| 6a-6f / <=1d each | One compatibility packet per installed mod: MC2X-CVE-G, MC2X-REAP3, Exodus, MagicExpansion, MechCommanderOmnitech, MCO-Carver5. Check custom decals, VFX, imported props/mechs and shader overrides. | Read-only original mod installs; own lane staging only. Load/save and short mission smoke; viewed relevant VK before/after captures, validation 0. Record row-18 loss/row-29 acceptance explicitly where applicable. |
| 7 / 0.5d | Remove only independently proven unused leftovers; retain native GLSL/SPV, ABI fixtures and vendor headers. Update retirement completion record. | Zero owned GL context/API/link consumers, package/reflection/binding checks remain green; merge-window tier1, editor/tool builds, validation 0 and reviewed captures referenced. |

Top five sign-off constraints (not five surviving GL backend implementations): stale evidence; gate/log consumer compatibility;
native shader/ABI assets mistaken for GL; mod coverage for accepted feature losses; editor/display acceptance receipts.
Static source audit cannot establish actual loaded DLLs, installed-mod behavior or current visual quality; those proofs remain future work.
Verification: `git diff --check`; existing `scripts/check-no-gl-compiler.py` PASS (owned GL calls/headers/loader/link census zero).
No installs created, no disk freed; no install directories left behind.
One topology scout timed out after 420 s; targeted source scans and the owned GL census supplied the replacement evidence.

## Time and token budget (approximate; overlapping scout work)

| Activity | Minutes | Token share |
|---|---:|---:|
| Compile/link | 0 | 0% |
| Contracts/static checks | <1 | 2% |
| Game runs | 0 | 0% |
| Lease/lane waiting | 0 | 0% |
| Deploy | 0 | 0% |
| Reading code/docs | 4 | 35% |
| Tool output/scouts | 7, overlapping | 35% |
| Thinking/editing | 3 | 28% |

Biggest avoidable cost: broad topology scout timeout; start with the existing zero-GL census and narrowly scoped follow-up questions.
