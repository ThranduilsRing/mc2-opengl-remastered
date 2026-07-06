# VK-ARC-STATE — current state block (update every session)

HEAD: `6c9e4372` "feat(vk): VK owns terrain skirt/edge-stitch geometry (rung 4)" on
branch `claude/vk-interop-bridge-1`, worktree `A:/Games/mc2-vk-interop-bridge`.
Build dir `build64_island`. Deploy lane (literal): `A:/Games/mc2-opengl/mc2-win64-0.4c`.

## Gate inventory (umbrella + sub-gates)

- Umbrella: `MC2_VK_SHARED_GBUFFER=1` arms the family; explicit per-gate env
  overrides (`MC2_VK_TERRAIN_TIER1`, `MC2_VK_TERRAIN_ALL_LODS`,
  `MC2_VK_TERRAIN_SKIRTS`, `MC2_VK_TERRAIN_LIGHTING`, `MC2_VK_TERRAIN_DYNSHADOW`,
  `MC2_VK_TERRAIN_DETAIL`, `MC2_VK_TERRAIN_LODFADE`, `MC2_VK_PROP_TIER1`,
  `MC2_VK_MECH_TIER1`, ...) win when explicitly set 0/1.
- All default-OFF; umbrella-ON verified live this session (see FPS table below).

## Current oracle numbers (this session, mc2_01, ~20s live run, umbrella ON)

Two runs sampled via `tools/perf_snapshot.py`:

| run | FPS(proxy) | terrain total_us | prop total_us | mech total_us |
|---|---|---|---|---|
| 1 | 211.2 | 4734.0 | 1585.1 | 810.5 |
| 2 | 217.8 | 4592.1 | 1596.4 | 806.0 |
| 3 (12s) | 385.1 | 2596.9 | 1658.6 | 863.4 |

FPS is a coarse proxy (`1e6/max(total_us across islands)`), NOT a real frame
counter — the debug-state dump has no frame-count field today. If one gets
added, update `tools/perf_snapshot.py` first.

## Ladder position / next rung

VK-OFFLINE-PROOFS-1 (this session): 5 offline verification tools built +
validated live against the real deployed exe. See `tools/` section below.
Next rung: build a real `MC2_VISUAL_BOOKMARK_CAPTURE` bookmark file so
`tools/vk_feed_diff.py` runs can be TRUE golden-vs-current (today's live
validation showed non-determinism run-to-run without a bookmark: mech gait /
prop streaming / camera drift all still moving — see readyToDump() gate in
`GameOS/gameos/vk_feed_dump.h`).

## VK-lane tool section (agent-efficiency — cross-worktree doc awkward, kept here)

- **`tools/vk_feed_diff.py`** — golden-vs-current diff of `MC2_VK_FEED_DUMP`
  dumps (terrain/prop/mech feeds). `py -3 tools/vk_feed_diff.py golden.jsonl current.jsonl [--island NAME] [--allow-missing]`.
  Validated: identical files → exit 0; synthetic ALL_LODS-on-vs-off diff →
  reports exactly the added coarse patch, exit 1.
- **`GameOS/gameos/vk_feed_dump.h`** — the dump-side harness the 3 feed
  producers (`gos_terrain_lod_chunk.cpp`, `gos_static_prop_batcher.cpp`,
  `gos_mech_batcher.cpp`) call into. Env `MC2_VK_FEED_DUMP=<path>`. Gated by
  `readyToDump()`: fires only when the sim is frozen for a bookmark sweep
  (`mc2CaptureGaitFreezeActive()`) if a bookmark is configured; else fires on
  first successful feed with a one-time stderr determinism warning.
- **`tools/glsl_port_diff.py`** — verbatim-port prover for `//PORT-BEGIN:name`/
  `//PORT-END:name` marker pairs. Registry (`--all`) covers `calc_light`
  (`shaders/include/lighting.hglsl` vs `shaders/vulkan/mech_tier1.vert`),
  `lodNear` (`shaders/gos_terrain.frag` vs `shaders/vulkan/terrain_tier1.frag`),
  `skirtVertMath` (`shaders/terrain_lod_chunk.vert` vs
  `shaders/vulkan/terrain_tier1_skirt.vert`). All 3 REPORT AS DIVERGING after
  whitespace/comment normalization — real renames (identifier/variable-name
  changes only, semantics preserved), not silently fixed. See findings below.
- **`tools/check_vk_shaders.py`** — wraps `glslangValidator -V` (same tool +
  flags CMake's VULKAN-SHADER-TOOLCHAIN-1 target uses) over `shaders/vulkan/*.{vert,frag}`,
  errors-only. `py -3 tools/check_vk_shaders.py`. Validated: 34/34 clean;
  deliberate syntax error in `fullscreen.frag` detected then reverted.
- **`tools/check_ubo_layout.py`** — cross-checks C++ island UBO mirror structs'
  `// off N` comments against independently-recomputed std140 offsets parsed
  from the GLSL uniform block. Registry: terrain (`TerrainFrameUBO`), prop
  (`ColorFrameUBO`). Validated: both OK (36 + 10 cross-checked fields);
  deliberate off-by-931 error in `vulkan_terrain_island.cpp` caught then reverted.
- **`tools/perf_snapshot.py`** — PID-tracked launch + N-second wait +
  debug-state-dump island-timing read + exact-PID kill, one-line FPS+per-island
  report. `py -3 tools/perf_snapshot.py --exe <path> --mission mc2_01 --duration 20 --env MC2_VK_SHARED_GBUFFER=1`.
  Validated live (see FPS table above); never leaves an orphaned process
  (finally-block exact-PID taskkill).

## Verbatim-port findings (tools/glsl_port_diff.py --all)

All three registered ports (`calc_light`, `lodNear`, `skirtVertMath`) are
**functionally-equivalent but NOT textually verbatim** — cosmetic identifier
renames only (e.g. `lights_index`→`idx`, `n_dot_l`→`ndl`, `u_skirtDepth`→
`uT.skirtDepth`). No logic divergence found. Not "fixed" — recorded as findings
per the tool's own contract (see tool docstring).

## Session update convention

Whoever picks up VK work next: update the FPS table + ladder position above
before ending the session. Keep this file under ~60 lines.

## FIELD-CONFIG SESSION 2026-07-06 (commit a50c6033) — full-stack verification

Standing rule: every fix verifies under the FULL launcher field env
(`tracy-vk-compare.bat` + `MC2_STATICPROP_TEX_TIER=512`), not gate-isolated.
Oracle: `.claude/mech-parity/field_oracle.py` (field GL/VK bookmark sweep). NOTE:
capture window MUST be visible (`wShowWindow=4`), NOT minimized(7) — minimized =
zero surface extent = VK present fail-softs to GL and silently invalidates the
run. Islands (terrain/shadow/prop/mech) still render under present-fail-soft, so
composited pixels remain a valid VK oracle.

- **BUG 1 (terrain shadow wash) FIXED** — MC2_VK_TERRAIN_CSM full-map subset is
  net-negative at play distance (coarse full-map cascade vs GL's near cascade =
  camera-swimming blobs). Field GL-vs-VK MAD 28-31 (wash) -> ~5 (clean) on
  airport bookmarks. Double-gated: now also needs
  `MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL=1`. Real cure (near-cascade-array
  interop) still DEFERRED.
- **BUG 4 (mech screen-space freeze on pan) FIXED** — feed now uses live
  gos_GetMechFixBMVPMat4() not the flush-time snapshot. Static-capture cannot
  pan; verified by code-trace + no static regression. USER RE-TEST on live pan.
- **BUG 2 (fence) / BUG 3 (hangar washout)** — hold under field config; prior
  fixes (b911bc0d / 91051001) confirmed present, props render (walls/quonsets
  textured, fence brackets present) in field runway_overview + mc2_24. Layer
  index is bucket-relative/order-derived => tier-INVARIANT (deploy has only
  128/ tier dir; missing 512/ => whole-group fallback, GL==VK). No tier-driven
  layer shift.

### OPEN (documented, NOT fixed this session — for next session)
- **BUG 5 (2x sim speed, field env)** — NOT a fixed-step misfire:
  `MC2_SMOKE_FIXED_TIMESTEP` is the ONLY armer (gos_smoke.cpp:54); the field bat
  does NOT set it, and `MC2_DEBUG_STATE_DUMP` touches no timestep path. Sim dt =
  `frameLength = 1.0/frameRate` (mechcmd2.cpp:2217); frameRate = live wall-clock
  `1000/dt` (gameosmain.cpp:1957). Needs an instrumented sim-time-vs-wall run
  (field vs minimal env) to root-cause the 2x. SIM-SPEED FEATURE SEAM (user
  request, do NOT implement yet): the clean knob is `frameLength`
  (mclib/timing.cpp:20, set mechcmd2.cpp:2217) — a future MC2_SIM_SPEED/UI
  button scales it.
- **FAR-BAND BLACK** — far terrain band renders black/unlit silhouette under
  field env (confirmed in mc2_01 oblique_horizon, mc2_24 overview_center). Known
  far-band/lighting-state or a 5th interaction; recorded only.
- **BUG 6? prop texture scramble** — coordinator saw live (pink slabs, floating
  textures, trunkless trees) DURING mc2_24 load on the Bug1+Bug4 exe. NOT
  reproduced at settled capture (props clean: walls/trees/buildings textured).
  Classified LOAD-TRANSIENT, not a static-reproducible regression; neither
  committed fix touches prop layers. USER cold-load re-check to confirm.
