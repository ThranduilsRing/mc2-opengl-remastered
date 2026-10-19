<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2 Architecture Map (orientation, not reference)

Branch `claude/nifty-mendeleev`, written 2026-10-08. Detail lives in `CLAUDE.md`, `docs/render-pipeline-map.md`
(June, partly stale: trust symbols, not line numbers) and the memory `INDEX-*.md` files. This page only
answers "where am I and where do I look".

## 1. Top-level directories

| Dir | Purpose | Size | Touch |
|---|---|---|---|
| `code/` | Game logic (the original MC2 `mechcmd2`): missions, mechs, AI/ABL brains, camera, logistics, UI screens | ~280 files | often |
| `mclib/` | Engine lib: terrain, TGL shapes, texture manager (`txmmgr.cpp`), ABL interpreter, gosFX, MLR | ~580 | often (render glue lives here) |
| `GameOS/gameos/` | Platform + renderer: SDL window, GL backend, GPU-driven batchers, postprocess, all `vulkan_*` | ~270 | often (main work area) |
| `RenderCore/` | Backend-neutral contracts: `IRenderBackend.h`, `DrawPacket.h`, pass/feature registries, `vulkan_contract.h` | ~50 | often (mostly headers) |
| `RenderWorld/`, `GameAdapters/` | Visibility/pick seam; adapters feeding Mech/Sky/StaticProp/Vegetation into RenderCore | small | sometimes |
| `GuiRuntime/`, `gui/` | ImGui layer (inspector, graphics options) / legacy in-game GUI widgets | small | sometimes |
| `HostServices/`, `EditorBridge/`, `Viewer/` | Host service interface, editor-to-game bridge, asset viewer exe | tiny | rarely |
| `shaders/` | GLSL (`.vert/.frag/.comp/.tesc/.tese`) incl. VK variants; deployed with the exe | ~210 | often |
| `editor/`, `ui_editor/`, `text_tool/`, `data_tools/` | Map editor (`EditRel`), UI editor, string tool, makefst/pak/aseconv | ~260 | rarely |
| `scripts/` | Python/bash: `check-*.py` contract rows, `run_smoke.py`, `deploy_payload.py`, bootstrap | ~330 | often |
| `tools/` | `repo_intel/`, `visual_verify/`, harness builder, Tracy compare, asset pipeline | ~890 | often |
| `tests/` | pytest, doctest, visual goldens (`tests/visual/goldens/reset-50331d5`), smoke fixtures | ~2700 | sometimes |
| `docs/` | Design/handoff/reference docs (`docs/archive/` is dead history) | many | sometimes |
| `data/`, `res/`, `mods/`, `examples/`, `run/` | Game data/config, resource DLL, mod samples, run helpers | mixed | rarely |
| `3rdparty/`, `Tracy/`, `ARM/` | Vendored libs (SDL, imgui, VMA, Tracy, ...) | ~1550 | never |
| `build64/`, `build64-h-*`, `build*`, `out/`, `_harness_out/`, `artifacts/`, `scratch/`, `staging/`, `screenshots/`, `graphify-out/` | Generated output | huge | never (not source) |
| `3rdparty.zip`, `brain_task_queue_test.obj` | Stray artifacts | - | never |

CMake targets: `mc2` (the game), `mc2_launcher`, libs `mclib`, `gui`, `rendercore`, `renderworld`, `gameadapters`,
`gui_runtime`, `editorbridge`, `mc2res_*`; tools `viewer`, `EditRel`, `ui_editor`, `text_tool`, `makefst`, `pak`,
`aseconv`, `makersp`. Build RelWithDebInfo only: `build64/RelWithDebInfo/mc2.exe`.

## 2. Frame lifecycle (GL spine, with VK islands alongside)

1. `GameOS/gameos/gameosmain.cpp` main loop: `Environment.DoGameLogic`, then the `Camera.UpdateRenderers` block.
2. Logic: `Mission::update` (`code/mission.cpp`) steps objects (`code/objmgr.cpp`), AI/brains, movement.
3. Render entry: `gos_RendererBeginFrame()` -> `UpdateRenderers()` (`code/mechcmd2.cpp`) -> mission render ->
   `GameCamera::render` (`code/gamecam.cpp`) -> `gos_RendererEndFrame()` (`gameos_graphics.cpp`). Optional seam:
   `RenderFrameDriver_RenderWorld` (`render_frame_driver.cpp`, gate `MC2_RENDER_FRAME_DRIVER`).
4. Key rule: `XXX::render()` only ENQUEUES. All GL fires in `MC_TextureManager::renderLists()` (`mclib/txmmgr.cpp`).
5. `GameCamera::render` order: HDRI sky -> shadow pre-passes (CSM + dynamic) -> `renderLists()` -> water fast path
   (`renderWaterFastPath`) -> GPU particles (gosFX) -> postprocess -> HUD batch -> ImGui.
6. Inside `renderLists()`: terrain indirect, static props, mechs, then legacy `ShapeRenderer` for vehicles/generics.
7. Present: `gosPostProcess` endScene (`gos_postprocess.cpp`), HUD `gos_RendererFlushHUDBatch`,
   `GuiRuntime::Render`, then swap.

| Stage | GL path | VK island (runtime default is GL; `MC2_RENDER_BACKEND=vulkan`) |
|---|---|---|
| Backend select | `render_backend_select.cpp` | same; `vulkan_context.cpp` creates the shared device |
| Sky | `GameAdapters/SkyRenderAdapter.cpp`, `gos_hdri.cpp` | `vulkan_sky_island.cpp` (`mc2_vk_sky_island_run`) |
| Terrain | `gos_terrain_lod_chunk.cpp`, `gos_terrain_indirect.cpp`, `mclib/terrain.cpp` | `vulkan_terrain_island.cpp` (`mc2_vk_terrain_island_run`, called from `gos_terrain_lod_chunk.cpp`) |
| Water | `gos_terrain_water_stream.cpp`, `Terrain::renderWaterFastPath` | `vulkan_water_island.cpp` (hooked in `gameos_graphics.cpp`) |
| Shadows | CSM pre-pass in `mclib/txmmgr.cpp` | `vulkan_shadow_island_live.cpp` (live VK shadow island) |
| Props/buildings/trees | `gos_static_prop_batcher.cpp`, `gos_static_prop_registry.cpp`, `gos_vegetation.cpp` | `vulkan_color_prop_island.cpp` (static buckets only) |
| Mechs | `gos_mech_batcher.cpp`, `GameAdapters/MechRenderAdapter.cpp` | `vulkan_mech_island.cpp` |
| Vehicles/legacy | `ShapeRenderer` in `renderLists()` | `vulkan_legacy_shape_island.cpp` |
| FX/particles | `gos_particle_bridge.cpp`, `gos_cardcloud_sim.cpp` | `vulkan_vfx_island.cpp` |
| Post | `gos_postprocess.cpp`, `gos_postprocess_blur.cpp` | `vulkan_post_{fog,hzb,screenfx,ssao}_native.cpp` |
| Present | SDL GL swap | `engine_present_vk.cpp` |

Islands share color/depth/GBuffer via `vulkan_shared_scene_color.cpp`, `vulkan_shared_scene_depth.cpp` and
`vk_shared_light_feed.cpp`, and are fed by GL-side "feeds" (e.g. `mc2_terrain_tier1_get_frame_feed`).

## 3. Where do I look for X

| X | Look at |
|---|---|
| Terrain draw / LOD / tessellation | `GameOS/gameos/gos_terrain_lod_chunk.cpp`, `gos_terrain_indirect.cpp`, `shaders/gos_terrain.frag` (+ `.tesc/.tese/.vert`) |
| Terrain data / textures / lighting | `mclib/terrain.cpp`, `mclib/quad.cpp`, `gos_terrain_lighting.cpp`, `terrain_material_lib.cpp` |
| Water | `gos_terrain_water_stream.cpp`, `shaders/vulkan/water_geom.frag`, `shaders/vulkan/water_cull_pack.comp.slang` |
| Props / buildings | `gos_static_prop_batcher.cpp`, `gos_static_prop_registry.cpp`, `GameAdapters/StaticPropRenderAdapter.cpp`, `code/bldng.cpp` |
| Trees / foliage | `gos_vegetation.cpp`, `GameAdapters/VegetationAdapter.cpp` |
| Mechs | `gos_mech_batcher.cpp`, `GameAdapters/MechRenderAdapter.cpp`, `code/mech.cpp`; memory `INDEX-MECH.md` |
| Shadows | `mclib/txmmgr.cpp` (caster pass), `GameOS/gameos/vulkan_shadow_island_live.cpp` (the shadow maps), `shaders/vulkan/shadow_mech.vert`; memory `shadow_coordinate_spaces`, `csm_dynamic_shadow_architecture` |
| Fog / sky | `shaders/vulkan/edge_fog.frag`, `shaders/vulkan/fog_oob.frag`, `shaders/hdri_skybox.frag`, `gos_hdri.cpp`, `SkyRenderAdapter.cpp` |
| Post-process | `gos_postprocess.cpp`, `RenderCore/postprocess_subgraph.h` |
| HUD / UI | `gos_RendererFlushHUDBatch` in `gameos_graphics.cpp`; `code/missiongui.cpp`, `gui/`, `GuiRuntime/` |
| Camera | `code/gamecam.cpp`, `mclib/camera.cpp`, `mclib/camera_frustum_math.h` |
| AI / brains / ABL | `code/brain_tactic_select.cpp`, `code/ablmc2.cpp`, `mclib/ablexec.cpp` |
| Gates / env vars | `RenderCore/RendererFeatureRegistry.h`, `docs/tier1_env_vars.md`, `docs/flags.auto.yaml`, `repo_query.py env --domain X` |
| GL/VK contracts | `RenderCore/vulkan_contract.h`, `RenderCore/RenderPassContract.h`, `scripts/check-contracts.sh` |
| Build / deploy | `CLAUDE.md`, `BUILD-WIN.md`, `scripts/bootstrap_worktree_build.py`, `scripts/deploy_payload.py` |
| Smoke tests | `scripts/run_smoke.py` (tier1 = mc2_01/03/10/17/24), `GameOS/gameos/gos_smoke.cpp` |
| Offline checks | `scripts/check-contracts.sh`, `tools/build_contract_harnesses.py --run --unit`, `scripts/tests/` |
| Visual captures / A-B | `tools/visual_verify/ab.py`, `tools/visual_verify/capture.py`, `GameOS/gameos/gos_visual_capture.cpp`, `docs/visual-verify.md` |
| Perf | `tools/tracy_capture_compare.py`, `docs/tracy-harness.md` |
| GPU debugging | `tools/renderdoc/`, `docs/renderdoc-automation.md`, `MC2_DEBUG_VIEW=N` |
| Repo intel (use before grepping) | `tools/repo_intel/repo_query.py` (`preflight`, `env`, `binding`), `tools/repo_intel/grep_tool.py`, `gate_status.py` |
| Mods / data | `docs/modding-guide.md`, `mclib/anim_override_registry.cpp`, `docs/asset-pipeline.md` |
| Dev loop without relaunch | `docs/dev-shell.md`, `GameOS/gameos/gos_dev_shell.cpp` |
| Latest status | memory `HANDOFF-2026-10-08b-vk-parity.md`, `docs/known_issues.md` |

## 4. Known traps (from memory must-reads)

- Work only in `A:/Games/mc2-main`; `mc2-opengl-src` is stale. Commit first, then build/deploy (exe embeds git sha). Never push.
- `render()` enqueues; GL fires in `renderLists()`. A new GPU-direct path hooked before the flush draws in the wrong order.
- Fast paths inherit GL state (blend, sampler, depth, VAO element buffer, program). Set everything explicitly; see
  memory `gpu_direct_renderer_bringup_checklist`.
- Uniforms: use `glProgramUniform*` (not `glUniform*`) with an explicit program; re-cache locations after a program swap;
  keep C++/GLSL UBO structs in lockstep (a stride bug broke mech shadows).
- Shaders (including compute) must be deployed alongside the exe or the game runs stale or missing code.
- `--target mc2` can skip mclib recompile; do a full relink after class-layout changes.
- VK work: view the capture yourself. MAD is not a pass signal (all buildings missing scored 0.62). A/B a day AND a night map.
  The VK shadow island is suspect (two mech-shadow attempts reverted); the color-prop island skips dynamic buckets.
- Per-frame GL readbacks (`glGetBufferSubData`, the 512MB CSM array) stall or corrupt; avoid.
- No new default-OFF gates (features ship ON or not at all). Mods (MC2X-CVE-G, Exodus, ...) must keep working.
