# Headless mission load: presentation dependencies and the seam

Lane claude/arch-headless-load. Design context: .claude/cleanup/sim-kernel-design.md, "Presentation dependencies and the headless runner".
Status: design plus the tick-budget exit (`--ticks N`). The no-GPU boot itself is NOT built; section 5 is the plan.

## 1. Where the boot touches presentation today

Order in gameosmain.cpp main(): `create_window` -> `initVulkanBackend` -> `gos_CreateRenderer` (gosRenderer, gosPostProcess, VK scene-readback
self-test) -> `GuiRuntime::Init` -> `gos_CreateAudio` -> `AssetScale::init` -> `Environment.InitializeGameEngine` (fonts, main-menu widgets,
textures) -> frame loop (`ExtractRenderSnapshot`, `draw_screen`, present). Mission load is `Mission::init` (code/mission_init*.cpp, mission.cpp),
run from the logic phase of the frame loop. Dependencies, by phase:

| Phase (Mission::init) | Presentation dependency | Does the sim read it? |
|---|---|---|
| initCoreSystems: `initTGLForMission` | TG pools, registers shape types with the render batchers | pool sizes only |
| initGameSystem*, initMissionFile, initTeams*, initPathfinding | FIT/ABL parse, no gos graphics | yes, all of it |
| initSkyAndTeamReset | sky number; skybox GenericAppearanceType (gamecam.cpp) | no |
| initTerrain | `land->init`: pak parse is sim (height, cost, overlays); terrain textures via gos_New*Texture, terrain-indirect and lighting-GPU buffers | height/cost/overlay yes; textures and lighting buffers no |
| initSpawnActors / initTerrainObjectsLoad | `appearanceTypeList->getAppearance` -> `*AppearanceType::init` loads TG_TypeMultiShape/TG_Shape, textures (mcTextureManager -> gos_NewTextureFromMemory/File), GPU batcher `registerType`; per object `Appearance::init` | yes, see section 2 |
| initGpuAndLighting | `gpu_cull::substrate_init/compute_init/readback_init`, `gos_terrain_lighting::mission_init`, `frameJobsInit` | no |
| initWeatherCamera | `Weather::init` (lightning/rain timers) and `GameCamera::init` (eye, light profile, sky) | weather deadlines yes; camera no, but `eye` is read by sim code (section 3) |
| initInterface | `MissionInterfaceManager::init`, tac map, mission screen FIT, widgets/fonts/textures, mouse cursor | selection and order ingress live here today: needed as a data object, not as widgets |
| initFinalizeGeometry | `refreshSimPose`, `snapshotMoverPoses`, `extractRenderSnapshots`; then `StaticProp::finalizeGeometry`, `GpuMechBatcher::finalizeGeometry`, light-bake prewarm, static shadow casters | first three yes (they seed the pose the sim reads); the rest no |
| Audio, any phase | `soundSystem->...` (objmgr_io 18 refs, missionbegin 9, mission.cpp 6, pause/resume in the clock path), `gos_CreateAudio`, Betty samples, video decoder | logical media completion must come from ticks (design doc), not the device |
| Per tick (not load) | `VkSceneRenderer::beginFrame` in `updateFrontEnd`, `userInput->update`, `eye->update` | camera/input feed `missionInterface->update` |

## 2. What the sim actually reads from appearances

Bounding box and radius (collision, selection, LOS); part/node layout and hardpoint positions (weapon origin, hit nodes, cockpit); animation
gesture durations and frame counts (gesture state machines, damage/fall timing); terrain footprint and `markMoveMap`/`markTerrain`;
`setObjectParameters`; `simPose`/`refreshSimPose`; `simulationOwned()`/`simulationAnimation()` (already split out by the sim-kernel work and
already hashed by SIMID under replay). It does NOT read texture handles, GPU vertex data, paint schemes, lighting, shadows, effects or decals.
Textures matter only indirectly: shape load can fail or substitute when a texture name cannot be resolved, so a data-only stub must keep the
same shape/FIT parse and skip only the texture/GPU half.

## 3. Suspected presentation reads inside the sim (firewall candidates)

By reading, not yet proven by a run; each needs a headless digest diff to confirm.
- `eye` (GameCamera) is a global read by sim code (update ordering in `Mission::updateFrontEnd`; weather/lightning use camera distance).
  Headless needs a camera object, or these reads must go through Environment (the design doc already says so for weather).
- `appearance->setObjectParameters(..., drawFlags, team, relation)` at load takes render-owned `drawFlags`.
- Anything keyed on render `frameLength` outside the tick clock (`frameRate < SMALL -> 4.0`); Mission::update banks it into fixed ticks, UI does not.
- Sound-driven state: `soundSystem->setIsPaused/clearIsPaused` in the clock path, `playBettySample` in MPlayer paths.

## 4. Proposed seam

Two stages. Stage 1 proves equality before stage 2 removes code.

Stage 1, null presentation (`--headless`, read once in main): skip `create_window`, `initVulkanBackend`, `gos_CreateRenderer`, `GuiRuntime`,
`gos_CreateAudio` and the draw/present half of the frame loop. Device access funnels through a few choke points to stub:
`gos_New*Texture`/`gos_DestroyTexture`/`gos_LockTexture` (counter handle; LockTexture returns a scratch buffer), `gos_CreateBuffer` and vertex
declarations, `gos_LoadFont` (glyph metrics for UI layout only), the `g_gos_renderer` pointer (guard or null-object), the
`GpuMechBatcher`/`StaticProp`/`gpu_cull`/`gos_terrain_*` init and finalize entry points (no-ops), and `soundSystem` (null object whose logical
durations come from asset headers). Appearances then run their normal `init`, so every value the sim reads is real and the digest cannot differ
by construction. Cost: tens of guard sites; the risk is stray `g_gos_renderer`/VK-context dereferences, found by running.

Stage 2, data-only appearance: once stage 1 is green, `getAppearance` under a load-mode flag creates the type with shape/FIT parse but no
texture/GPU/batcher registration. This removes the device dependency for real and is where the S1/S4 split (`simulationOwned()`,
`extractRenderSnapshots`) already points. No dummy bounds or hardpoints.

HUD: `MissionInterfaceManager` must split into an ingress/selection data part (stays) and a widget part (skipped headless). Largest single item;
overlaps the UI lanes.

Exit (done): `--ticks N` quits after N sim ticks, emits the clean summary and exits 0; `--duration` stays only the harness timeout.
Needs the fixed clock (`MC2_SMOKE_FIXED_TIMESTEP=1`, already set by the determinism runs). The budget is checked once per frame: exact at one
tick per frame (the harness clock), may overshoot under `--sim-speed N`. Run it via `run_smoke.py --ticks N`.

## 5. Remaining steps

1. Stage 1 behind `--headless`, driven by crashes: window/renderer/audio skip, texture/buffer/font stubs, guard `g_gos_renderer`.
2. Headless frame loop: no draw/present; run `Mission::update` directly through the same validating loader.
3. Proof: headless SIMID chain equals the windowed chain for mc2_03 and mc2_10 over 800-1000 ticks (`tools/sim_determinism.py compare`).
4. Stage 2 data-only appearance; split MissionInterfaceManager; Environment-owned weather; logical media completion from ticks.
5. Harness capability proving presentation never initialized, before dropping the desktop/GPU lease.
