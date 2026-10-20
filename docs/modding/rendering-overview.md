<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2 OpenGL — Rendering Overview

Target audience: contributors adding new render features or understanding how a frame is produced.

## Frame pipeline (in order)

Each frame runs the following passes:

| # | Pass | Code entry point | Notes |
|---|------|-----------------|-------|
| 1 | Camera/light uniforms | `GameCamera::render()` (`code/gamecam.cpp`) | MVP, `gos_SetWorldToClipGL()` |
| 2 | Skybox | `GameAdapters::Sky::renderHdri()` | HDRI (MC2_HDRI_SKY=1) or black |
| 3 | Terrain solid | `land->render()` | Tessellated quads, PCF shadow |
| 4 | Craters | `craterManager->render()` | Decal batch |
| 5 | Objects (queue) | `ObjectManager->render()` | Mechs, static props, FX shape nodes — queued here, drawn at flush |
| 6 | Water surface (CPU path) | `land->renderWater()` | Default path; `projectZ()`-displaced |
| 7 | Shadow queue | `ObjectManager->renderShadows()` | Shadow geometry queued for pre-pass |
| 8 | **Flush gate** | `mc2::VkSceneRenderer::renderLists(mc2::renderSubmissions())` (`mclib/vk_scene_renderer.cpp`) | Uploads UBOs + light SSBO; shadow pre-pass; all master-node draws; post-process composite |
| 9 | Water surface (GPU path) | `land->renderWaterFastPath()` | Opt-in (`MC2_RENDER_WATER_FASTPATH=1`); runs after depth ready |
| 10 | GPU particles | `Batcher::Instance().Flush()` | Opt-in (`MC2_GPU_PARTICLES=1`) |
| 11 | Legacy FX | `theClipper->RenderNow()` | MLR particles, gosFX; post-shadow |
| 12 | Weather | `weather->render()` | Rain/snow overlay |
| 13 | HUD | screen-space batch | Post-process darkening applied |

The flush gate at step 8 is where all previously-queued geometry actually reaches the GPU. Code that calls GL draw commands before this point is queuing; code that must observe final depth must run after it.

## Render contract

Every render path belongs to one of three authoritative submission spaces:

- **World-space (Bucket A):** GPU projects via clipPos. Terrain (A1), grass (A2), static props, mechs.
- **Projected-space (Bucket B):** CPU `projectZ()` by design. Water (B1), cursor/picking helpers (B2).
- **Screen-space (Bucket C):** HUD/text/UI (C1).

Bridge paths (Bucket D) are active but being retired. New render features must land in A, B, or C — not bridge. See `docs/render-contract.md` for the full contract and exit criteria for remaining D paths.

## Key source files

| File | Purpose |
|------|---------|
| `code/gamecam.cpp` | Frame loop, render-call sequence, camera/light uniform upload |
| `GameOS/gameos/gameos_graphics.cpp` | GL state machine, draw dispatch, uniform caching |
| `GameOS/gameos/gos_postprocess.cpp` | FBO management, bloom, shadow map binding, post-process composite |
| `mclib/vk_scene_renderer.cpp` | Flush gate: `renderLists()`, master-node arrays, shadow pre-pass |
| `mclib/terrain.cpp` | Terrain `render()`, slim reduction loop, quad admission |
| `mclib/mech3d.cpp` | Mech appearance rendering (engine side; 5139 lines) |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | GPU static-prop + mech batcher, instance SSBO |
| `GameOS/gameos/gos_terrain_water_stream.cpp` | Water fast-path SSBO orchestration |

## Shader map

Shaders live in `shaders/`. Includes (shared library code) are in `shaders/include/`.

| Subsystem | Vertex | Fragment | Notes |
|-----------|--------|----------|-------|
| Terrain | `gos_terrain.vert`, `.tesc`, `.tese` | `gos_terrain.frag` | PBR splat, POM, PCF |
| Shadow depth | `shadow_terrain.vert/.tesc/.tese` | depth-only | One variant per object class |
| Static props | `static_prop.vert` | `static_prop.frag` | GPU-instanced PBR |
| Mechs | `mech.vert` | `mech.frag` | Articulated, instance SSBO |
| Water (CPU) | `gos_tex_vertex.vert` | `gos_tex_vertex.frag` | Projected-space intentional |
| Particles | `particle_billboard.vert` | `particle_billboard.frag` | View-aligned, ring-buffer |
| Post-process | `postprocess.vert` | `postprocess.frag` | Bloom + tonemap + FXAA |
| Debug prims | (none: drawn on the ImGui overlay) | (none) | World-space lines projected to the window, not depth-tested |

Key include files:
- `shadow.hglsl` — `calcShadow()` variable-tap Poisson PCF
- `lighting.hglsl` — shared PBR lighting model
- `material_gpu.hglsl` — GPU material table contract
- `scene.hglsl` — scene UBO layout (camera, lights, time)
- `render_contract.hglsl` — debug assertions (active under `MC2_RENDER_CONTRACT_ASSERT=1`)

## Where to add a new effect

1. Decide the submission space (A/B/C). Water-displaced geometry → B. Everything else → A.
2. Add a shader pair in `shaders/`. Include `scene.hglsl` for camera uniforms.
3. Queue draws in `GameCamera::render()` before the flush gate, or after if depth is needed.
4. Register a `MC2_*` feature gate (see `docs/modding/renderer-feature-flags.md`) and default it off.
5. Add the gate to the "Renderer Features" ImGui panel so it's controllable at runtime without restart.
6. Run the tier-1 smoke gate (`scripts/run_smoke.py --tier tier1 --duration 30`) before merging.

## Further reading

- `docs/render-contract.md` — render contract design + bucket definitions
- `docs/observations/2026-05-25-pipeline-master-index.md` — detailed subsystem map
- `docs/tier1_env_vars.md` — full MC2_* env var reference for contributors
- `docs/modding/debugging-render-issues.md` — using ImGui panels to diagnose problems
- `docs/modding/renderer-feature-flags.md` — feature flag quick-reference for modders
