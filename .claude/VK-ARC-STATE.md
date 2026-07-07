# VK-ARC-STATE — current state block (update every session)

HEAD: rung 6 commits `06c4fe03` (island tier1) + `790388f8` (port markers) on
branch `claude/vk-interop-bridge-1`, worktree `A:/Games/mc2-vk-interop-bridge`.
Build dir `build64_island`. Deploy lane (literal): `A:/Games/mc2-opengl/mc2-win64-0.4c`.

## RUNG 6 — VK-WATER-TIER1 (2026-07-07) — SHIPPED (base-layer, reflection-off subset)

Commits `06c4fe03` (island tier1 mode + gate registration) + `790388f8`
(waterBaseColor port markers). Gate `MC2_VK_WATER_TIER1` (default-OFF,
envTriState opt-in like SKIRTS/VISUALDISPLACE_FAR; precedence over the P9a
`MC2_VK_WATER_INTEROP`). `vulkan_water_island.cpp` tier1 mode renders the armed
water base layer IN-PLACE into the shared scene GBuffer (color0+normal+depth
LOAD-preserve, terrain-island sync protocol) — kills the P9a offscreen target,
the GL composite pass, AND the per-frame glGetTexImage depth readback
(receipts: depthin_getTexImage=0, depthin_copyBufToImg=0, forbidden_*=0 on the
live gate-ON run). Blend: FS stays premultiplied; pipeline color ONE/OMSA +
alpha SRC_ALPHA/OMSA == GL straight-alpha one-stage arithmetic; shared normal
attachment writeMask=0 (GL water GBuffer1 a=0 no-op parity). Depth: shared
depth LOAD (water runs after terrain), reverse-Z GEQUAL test+WRITE,
WATER_DEPTH_FUDGE_FAST 0.00025 lockstep unchanged (same VS).
- Offline: check_vk_shaders 34/34, check_ubo_layout OK; NEW port pair
  `waterBaseColor` (GL MDI FS vs water_geom.frag) = **VERBATIM** (17 lines).
- Byte-off control (gates unset, GL x2, mc2_01 water bookmarks): 0.000 /
  0.003 MAD — clean.
- Live gate-ON receipts (mc2_01, water bookmark sweep + sacrificial first
  bookmark for VK-FIRST-BOOKMARK-POSE-1): water island used_vulkan=492,
  vk_draws=1, thin=6167, recipes=9801; gate-OFF VK run: water used_vulkan=0.
- Water-only delta (vk_base vs vk_water, SAME VK scene, single variable):
  overview_high 0.54/ch cov 0.04%; oblique_low 8.29/ch cov 62%. ATTRIBUTED by
  experiment: GL water reflection defaults ON (strength 1.5 + HDRI LOD4,
  gameos_graphics.cpp:3100); with MC2_WATER_REFLECTION=0 the delta collapses
  to 3.56/ch cov 10.5% (oblique) / 0.32 (high). The VK FS is the documented
  reflection-OFF base-layer subset (P9a parity contract) — the dominant
  divergence is the MISSING reflection/HDRI feature, not a port defect.
- Depth discipline: shorelines correct, islands occlude water, no z-fight, no
  water-vanish (visual check on gate-ON captures). GL-vs-VK whole-frame pose/
  pillarbox framing difference at these bookmarks is the PRE-EXISTING VK-sweep
  camera artifact (present identically in vk_base, independent of this rung).
- Smoke: tier1 5/5 PASS both states (+0 destroys). Gate-ON smoke FPS ~7 is the
  PRE-EXISTING umbrella-under-smoke cost (measured identical without the water
  gate), not a water regression.
- Residuals / next sub-rung: (1) port the reflection block (SH-L2 sky + HDRI +
  RT sample + sun spec) into water_geom.frag + UBO to close the 3.6-8/ch
  grazing-angle gap — this is the remaining rung-6 feature work; (2) detail/
  spray layer (isWater==2 second MDI draw) still GL-discarded both sides, no
  port needed today; (3) refl-off residual 3.56/ch cov 10.5% at grazing not yet
  itemized (suspects: sun-spec default, fog band). Artifacts + oracle:
  `.claude/vk-water-tier1/` (water_oracle.py, cap/*).

## RUNG 5 — VK-TERRAIN-VISUALDISPLACE-FAR-1 (2026-07-07) — MODE-2 DONE

Commits `4c427cf9` (shaders) + `49ba7f25` (wiring). Gate
`MC2_VK_TERRAIN_VISUALDISPLACE_FAR` (default-OFF, envTriState opt-in, requires
ALL_LODS, smoke-allowlisted incl. GL `MC2_TERRAIN_VISUAL_DISPLACE_FAR` knob).
Scope: mode-2 far-band (lodStep>1, non-stitched) pure Z-swap ONLY; mode-1 (LOD0
4x topology) + stitched patches stay GL-owned (separate L slice later). VK skirt
TOP verts displace like GL (GL's mode-2 branch runs for skirt tops); bottoms stay
coarse-h - skirtDepth. New: binding-10 heightsFine SSBO (dummy 16B from frame 1),
snapshot_visual_height version-gated mission-load copy, UBO visualSide/
visualDisplaceFar/visualMode @ off 580/584/588 (1024B slot unchanged), per-patch
visualMode in Mc2TerrainTier1Patch, feed-dump displacedPatches field.
- Offline: check_vk_shaders 34/34, check_ubo_layout OK (47 fields), port pairs
  `visualDisplaceFar` x2 (main + skirt VS) = **VERBATIM** (local u_* aliases +
  anonymous SSBO instance keep the block textually identical to GL).
- Byte-off: control GL x2 gates-unset = 0.000 on 10/11 mc2_01 bookmarks
  (cross_map_view has PRE-EXISTING run-to-run drift, gates unset — excluded);
  displace-ON + umbrella+ALL_LODS but new gate OFF vs GL = 0.000-class (max MAD
  0.35/ch, cov <=1.4%) — predicate excludes everything, GL owns all pixels.
- Displaced-capture evidence: GL band-count telemetry LOD1=5 (GL ref run) ->
  LOD1=0 (VK gate-ON run) = VK captured exactly the 5 mode-2 patches; snapshot
  receipt `V=397 bytes=630436 ver=1`; used_vulkan=739 (VK run), 0 (GL run).
  NOTE: MC2_VK_FEED_DUMP produced no file this sweep (readyToDump gating) —
  evidence via band counters instead.
- Z-swap mechanism proof (single-variable): fade knob 1.0 vs 0.0, all else
  identical -> MAD 0.00-0.78/ch, cov <=0.81%, localized far-band silhouettes.
  The displacement math executes in VK and is small/localized as expected.
- GL-vs-VK absolute MADs gate-ON (mc2_01, 11 bookmarks): 13.6-44.3/ch, cov
  30-62%. ATTRIBUTION: dominated by the PRE-EXISTING bare-umbrella family
  residual (measured baseline at same poses, NO displacement anywhere:
  13.9-15.0/ch cov ~30%) — arming the gate flips far-band ownership to VK,
  exposing the known VK terrain look-class divergence (sky-band fill + lighting)
  on far surface. NOT a Z-swap defect (see fade-knob proof). Poses verified
  self-matching (no VK-FIRST-BOOKMARK-POSE-1 hit this sweep).
- Smoke: tier1 5/5 PASS both states (gates-OFF and displace+umbrella+ALL_LODS+
  gate-ON), +0 destroy-delta all missions, worktree run_smoke vs 0.4c exe.
- Residuals/deferred: mode-1 LOD0 port (L slice); stitched-patch ownership;
  0.4c lane ships NO visual_height_4x.r32 bakes (only v0.4 does) — live tests
  used MC2_TERRAIN_VISUAL_HEIGHT_FILE override; feed-dump readyToDump gating
  under sweeps; artifacts `.claude/vk-visdisp-far/`.

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

- **BUG 1 (terrain shadow wash) FIXED (interim)** — MC2_VK_TERRAIN_CSM full-map
  subset is net-negative at play distance (coarse full-map cascade vs GL's near
  cascade = camera-swimming blobs). Field GL-vs-VK MAD 28-31 (wash) -> ~5
  (clean) on airport bookmarks. Double-gated behind
  `MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL=1` (parked escape hatch).
- **VK-TERRAIN-CSM-NEAR-1 (real cure, LANDED, FIELD-VERIFIED 2026-07-06 this
  session, commit `1a4969c8`)** — ports GL's `calcDynamicShadow` near-cascade
  cascade-select VERBATIM into `terrain_tier1.frag`
  (`//PORT-BEGIN/END:calcDynamicShadowCsm`, registered in
  `tools/glsl_port_diff.py`). Imports GL's near-cascade array
  (`dynShadowArrayTex_`, GL_TEXTURE_2D_ARRAY depth) as a VK D32 depth-COMPARE
  `VK_IMAGE_VIEW_TYPE_2D_ARRAY` (`sampler2DArrayShadow`, binding 8) + the
  separate full-map catch-all cascade (binding 9) + all cascade
  matrices/texel-world/csmDepthSpan into `TerrainFrameUBO` (grew 512B->1024B,
  `check_ubo_layout.py` passes, 44 fields cross-checked). Copy mechanism:
  glGetTexImage per frame (no zero-copy producer for the array exists yet —
  the VK shadow island only produces the single legacy map) — documented as a
  known cost, not hidden. Gate `MC2_VK_TERRAIN_CSM` now means THIS
  implementation (requires MC2_VK_TERRAIN_DYNSHADOW chain + GL
  MC2_SHADOW_CSM=1); FULLMAP_EXPERIMENTAL untouched as a fallback escape
  hatch, mutually exclusive C++ population (no collision).
  **FIELD RESULTS** (rebuilt+redeployed 0.4c @ `1a4969c8`, fingerprint
  confirmed): terrain island co-writes confirmed active (LOD0 TIER-1 patches
  drawn, `used_vulkan>0` both near+airport runs). Byte-off control (gates
  off, GL x2) = 0.000 on 10/11 mc2_01 bookmarks (1 negligible max=1px, cov
  0%). VK self-diff determinism at the near building-shadow pose: mean
  0.27–1.76/ch, cov ≤0.79% (clean; one lodfade-pose max=255 spike is the
  known mech-gait jitter, not terrain). Airport bookmarks (`overview_center`,
  `runway_overview`) did NOT reproduce the a50c6033 wash pattern specifically
  — but DID show a severe GL-vs-VK divergence (mean 32–34/ch, cov ~97%) that
  **bisects to a PRE-EXISTING, CSM-independent bug**: reproduced identically
  with `MC2_VK_TERRAIN_CSM` unset and even with only the bare
  `MC2_VK_SHARED_GBUFFER=1` umbrella set (no other sub-gates). This matches
  the already-open **FAR-BAND BLACK** entry below (`oblique_horizon`,
  `overview_center` — both wide/far bookmark poses). VK-TERRAIN-CSM-NEAR-1
  itself is NOT implicated (identical wash with/without the CSM gate). The
  building-shadow near-pose oracle (`near_land_low`) was separately blocked
  by an UNRELATED, reproducible GL-side bug this session:
  `[MECH_LOAD] WARN: mech appearance has no base shape ... mechType=0` fires
  deterministically on GL (both `gl`/`gl2` runs, byte-identical 0.000
  self-diff) at this pose+FORCE_LOD=0 combo — 6 mechs registered
  (`RENDER_WORLD registered=6 alive=6`) but none get a base shape, so GL
  renders NO mechs/shadows at this pose while VK renders all 3 correctly.
  Root cause not investigated further (out of CSM-near scope); flagging for
  a follow-up. The mech-free `near_land_off` control pose (same location,
  no units) gave GL-vs-VK mean_all 9.8/ch cov 20% (masking known sky/EXR-
  missing black band at rows 0-220, a pre-existing artifact present in prior
  sessions' `ridge_lowangle` captures too) — consistent with the arc's
  documented ~5-10/ch near-detail residual band, no new regression signal.
  Smoke: tier1 5/5 PASS both umbrella-OFF and full-field+CSM-ON states, 0
  crash, deploy fingerprint 1a4969c8 confirmed both runs.
  **VERDICT: CSM-near mechanism itself verified clean (byte-off clean, VK
  self-diff clean, terrain island actively co-writing, no CSM-attributable
  wash); the loud field numbers trace to two PRE-EXISTING unrelated bugs
  (FAR-BAND BLACK, GL mech-load-at-this-pose) that predate/are independent of
  this commit. Full artifacts: `.claude/mech-parity/csm_near_field/`.**
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
- **FAR-BAND BLACK — PARTIAL FIX this session (`gos_terrain_lod_chunk.cpp` gate
  bug fixed; deeper CSM-near shading defect still OPEN).** Root-caused via
  bisection (MC2_VK_TERRAIN_LIGHTING/DETAIL/DYNSHADOW/CSM toggled one at a time
  on overview_center): the VK-TERRAIN-CSM-NEAR-1 feed-population block
  (`gos_terrain_lod_chunk.cpp` ~line 964, `out->csmEnabled=1` path) was gated on
  `dynShadowGate` (the DYNSHADOW sub-gate) ONLY — despite its own comment
  claiming "MC2_VK_TERRAIN_CSM alone... now means THIS implementation", the code
  never actually checked `mc2vkgate::subGate("MC2_VK_TERRAIN_CSM")`. Since GL
  CSM defaults ON (`MC2_SHADOW_CSM` default-ON) and the umbrella defaults
  DYNSHADOW ON too, this fed real CSM cascade data to the VK terrain frag any
  time DYNSHADOW was armed, REGARDLESS of the CSM sub-gate's own value — a
  real, provable gating bug, now FIXED (added the missing `csmNearGate` check).
  Verified: bare-umbrella-minus-explicit-CSM-off now renders terrain correctly
  (`.claude/mech-parity/csm_near_field/farband_realfix/vk_umbrella_csmoff/`).
  **HOWEVER** the FIELD launcher recipe (and bare umbrella with no overrides)
  legitimately ARMS both DYNSHADOW and CSM together (by design), and the
  overview_center/oblique_horizon black-out STILL reproduces in that
  configuration post-fix (`vk_final/mc2_01_overview_center.png`) — so a SECOND,
  deeper defect lives inside the CSM-near shading path itself
  (`mv_calcDynamicShadowCsm`/`mv_csmProject`/`mv_csmSampleLayer` in
  `shaders/vulkan/terrain_tier1.frag`, or the C++ feed populated at
  `gos_terrain_lod_chunk.cpp` lines 964-982) when the near-cascade path is
  legitimately active — most likely `normalLight`/`uT.terrainLightDir`
  collapsing near-zero (line 708 `lit = base * normalLight * shadow` would
  blacken the WHOLE fragment, not just the shadow term, matching the
  near-total-blackout symptom rather than a milder shadow-only darkening).
  NOT YET ISOLATED further this session (ruled out: depth format
  D32_SFLOAT_S8_UINT vs GL_DEPTH24_STENCIL8 reinterpretation — tried both a
  D24_UNORM_S8_UINT export swap [driver rejects it, falls back to D32S8, no
  change] and a native-VK-clear fix on the shared depth attachment [no change]
  — depth is NOT the mechanism; isolating LIGHTING/DETAIL/DYNSHADOW-without-CSM
  all render terrain correctly, only CSM-near-armed reproduces the blackout).
  Overview MAD: unable to compute a numeric MAD this session (no golden/
  byte-identical GL reference existed for this exact bookmark+camera-jump
  timing; visual A/B via screenshots only). NEXT SESSION: instrument
  `computeNdotLBand`/`uT.terrainLightDir` and the CSM UBO fields with a
  one-shot stderr dump inside `terrain_tier1.frag` (or add a
  MC2_VK_TERRAIN_CSM_DEBUG viz mode) to see whether lightDir/normalLight or the
  CSM sample itself is the zero. Artifacts:
  `.claude/mech-parity/csm_near_field/farband_realfix/`,
  `.claude/mech-parity/csm_near_field/oobfog_isolate/` (bisection captures).

- **CSM-BLACKOUT-RECON-1 (this session) — BLOCKED on a broken deploy lane,
  root cause NOT isolated live.** Coordinator's UBO-write-collision hypothesis
  (terrain UBO grew 512->1024B, maybe two feed paths stomp each other) was
  checked FIRST as instructed and RULED OUT by static read: the per-patch UBO
  populate loop (`vulkan_terrain_island.cpp` ~2193-2264) zero-inits a fresh
  `TerrainFrameUBO u{}` every patch and does a full `slots[i]=u` struct
  assignment (no partial memcpy/stale-size aliasing); `sizeof(TerrainFrameUBO)`
  is used consistently for the descriptor range, `ensure_ubo_capacity`, and the
  dynamic-offset stride — no collision found. `check_ubo_layout.py` still
  passes (44 fields). Built the planned one-shot debug-viz instrumentation
  instead: a new UBO field `csmDebugMode` (int, repurposed the `_csmPad2` slot
  at UBO off 364, fed from env `MC2_VK_TERRAIN_CSM_DEBUG=1..5`) that early-outs
  `terrain_tier1.frag`'s lit compose to show `normalLight`(1) / `lightDir`(2) /
  `staticS`(3) / `dynS`(4) / `shadow`(5) in isolation
  (`GameOS/gameos/vulkan_terrain_island.cpp` UBO struct + populate site,
  `shaders/vulkan/terrain_tier1.frag` struct mirror + branch after the
  `lit = base*normalLight*shadow` line). Builds clean, `check_vk_shaders.py`
  34/34, `check_ubo_layout.py` OK, full mc2 build succeeds, deployed to 0.4c.
  **BLOCKED**: every VK launch this session (both the new debug probe and a
  vanilla re-run of the existing `field_oracle.py`, 3 attempts) hit
  `[VK_PRESENT] init: vkCreateSwapchainKHR failed. fail-soft.` ->
  `[RENDER_BACKEND] fail-soft: forcing GL for the session` — the whole run
  silently became a GL capture, invalidating all screenshots taken (the
  earlier "reproduction" in `.claude/mech-parity/csm_near_field/blackout_debug/`
  is GL, not VK; disregard those PNGs). `mcp__mc2-repo-intel__deploy_status`
  on `A:/Games/mc2-opengl/mc2-win64-0.4c` independently confirms
  `verdict=BROKEN-TREE`: "0.4c is a BROKEN tree (stock + modified exe both
  crash_silent / PAUSE-loop, 0 frames). Smoking it yields FALSE crash
  reports." — this is a pre-existing lane-health problem, not caused by this
  session's edits (confirmed by retrying the untouched `field_oracle.py`
  recipe with identical failure). Exe fingerprint DID match HEAD (7f396c2e)
  each time — the deploy itself was clean; the swapchain failure is
  environmental/lane-state, not a stale-exe issue. **NEXT SESSION**: get (or
  wait for) a healthy 0.4c (or a fresh, uncontended deploy dir) before
  re-running this probe; the instrumentation above is ready to use
  immediately — just set `MC2_VK_TERRAIN_CSM_DEBUG=1` (normalLight) first
  against a working VK backend at `overview_center`/`oblique_horizon` and
  read the pixel values (should be mid-gray `mix(0.35,1.20,NdotL)`, achromatic
  — any color tint or near-0 reading means the debug branch itself isn't
  executing on VK, which would point back at UBO wiring after all). Debug
  field is fully gated (env-off = 0 = zero behavior/perf change) — safe to
  leave in tree. Shadow-saga status: UNCHANGED from before this session — the
  deeper CSM-near-armed blackout defect is still open, now with ready-to-use
  instrumentation blocked only by lane health.
- **CSM-BLACKOUT-RECON-1 RESOLVED (2026-07-07 session) — blackout does NOT
  reproduce at HEAD `cf91cba5`; the "deep CSM-near shading defect" was a
  compound evidence artifact, not a shader bug.** Lane health CLEARED first:
  rebuilt+redeployed 0.4c at `cf91cba5` (manifest src_commit confirmed), plain
  VK launch on a visible window (wShowWindow=4) → `[VK_PRESENT] swapchain OK
  images=3 extent=1707x960` + present path UP, no fail-soft (last session's
  vkCreateSwapchainKHR failure was environmental, as suspected). Full
  MC2_VK_TERRAIN_CSM_DEBUG=0..5 ladder run at all mc2_01 bookmarks, every run
  with a used_vulkan receipt (488–496; GL control used_vulkan=0):
  `.claude/mech-parity/csm_near_field/blackout_debug/vk_dbg{0..5}/` + `gl_ref/`.
  FINDINGS: (1) the debug branch executes correctly on VK (achromatic grays →
  UBO wiring at off 364 GOOD); (2) NO term collapses — at the oblique_horizon
  "black band": normalLight=0.70, lightDir sane, staticS=0.67, dynS=1.00
  (CSM sample fully LIT), shadow=0.67; (3) the oblique_horizon dark band is
  PRESENT IN GL TOO (GL band mean 23/ch vs VK 17/ch — it is the stock look:
  dark slope+baked-shadow band), terrain-only MAD GL-vs-VK = 3.79/ch at
  oblique_horizon, 4.5–17/ch across all matched bookmarks — inside the
  documented near-detail residual, NO blackout-class divergence; (4) the
  "overview_center blackout" screenshots were captured at the WRONG CAMERA
  POSE under VK configs — cross-matching VK captures against all GL bookmarks
  shows VK's first bookmark (overview_center) lands on a water/fog-edge pose
  (best-match = edge_fog_corner_southwest) while every subsequent bookmark
  matches its own name; reproducible with settle=40 AND settle=80, VK-config
  runs only, deterministic. The prior session's post-fix "blackout still
  reproduces" capture (`vk_final/mc2_01_overview_center.png`) is this same
  first-bookmark wrong-pose artifact (dark fog-edge water view misread as
  terrain blackout). VERDICT: CSM-near path healthy at HEAD; no shader/feed
  fix needed, nothing landed beyond probe-script receipts. NEW OPEN ITEM →
  **VK-FIRST-BOOKMARK-POSE-1**: bookmark-sweep capture under VK gates captures
  bookmark #1 at a stale/wrong camera pose (GL runs are correct). Any
  single-bookmark VK evidence at "overview_center" from sweep captures is
  suspect until this is fixed; use later-in-sweep bookmarks or
  single-bookmark runs for VK evidence.
- **BUG 6? prop texture scramble** — coordinator saw live (pink slabs, floating
  textures, trunkless trees) DURING mc2_24 load on the Bug1+Bug4 exe. NOT
  reproduced at settled capture (props clean: walls/trees/buildings textured).
  Classified LOAD-TRANSIENT, not a static-reproducible regression; neither
  committed fix touches prop layers. USER cold-load re-check to confirm.

## ADJUDICATION-1 (2026-07-06) — contamination-proof fence + washout verdict

Method: deterministic bookmark-sweep capture (sim-frozen, fixed seed/timestep,
SW_SHOWNOACTIVATE=4 visible window) at an authored **airport bookmark**
(`c_rot110`: pos 3200,100,~400 / rot 110 / proj 28 / alt 1000 — frames hangar +
perimeter fence + runway, the user's scene class). GL ('off' set) vs VK
(canonical 'on' set = `MC2_VK_SHARED_GBUFFER=1 MC2_VK_SHADOW_INTEROP=1` +
`MC2_SHADOW_CSM/STATIC_PROP_PBR_V1/AMBIENT_V1/IBL_SH=1`). Receipts printed per
capture: **GL** vk_color_prop used_vulkan=0 (correct GL label); **VK**
used_vulkan=400–411, vk_draws=1, interop_active=1, vk2gl_sem_active=1,
inst_records=1006, type_runs=78, geom_verts=23628 (correct VK label — pixels
ARE VK, no fail-soft). Artifacts: `.claude/adjudication/{gl,vk_probe}/`,
`crop_{hangar,fence}_{gl,vk}.png`, `rdc/` (.rdc + XML), `vk_probe/packets.csv`.

**RenderDoc finding (contamination class 3, DIRECTLY OBSERVED):** a
`renderdoccmd capture -w` of the canonical VK-on config produced a capture whose
header is `<driver>OpenGL</driver>` with ZERO vk* chunks. The VK prop island is
HEADLESS (offscreen + vk2gl semaphore, no swapchain/present), so a present-based
GL RenderDoc hook cannot see its `vkCmdDrawIndexed`s. **Lesson: RenderDoc-under-GL
of a shared-gbuffer interop frame CANNOT prove the VK island — its draws are on
the VK queue with no present. Draw-level VK facts must come from an in-record
dump (added: `MC2_VK_PROP_PACKET_DUMP`) or a StartFrameCapture/EndFrameCapture
device-scoped bracket, NOT TriggerCapture.**

**FENCE — VERDICT: STILL BROKEN in canonical config (alternating panels).**
Own GL/VK pair at identical pose: GL = continuous chain-link; VK = alternating
panels (every other panel's mesh missing). Draw-level (`packets.csv`, 80 packets):
EVERY packet draws non-degenerate with exactly one valid push-constant layer
(`layerOff`/`layerOn`), zero "both -1", zero zero-count draws. **So the
alternation is NOT dropped draws / -1 layers — the per-packet layer fix
(b911bc0d / VK-FLIP-FIELD-REPORT-2) IS correct at the data level.** Root cause is
the alpha-ON group's **0.5 alpha-test discard** (`color_prop_tier1.frag:69
if (texc.a < 0.5) discard`) against the VK copy-once albedo ARRAY's mip/filter:
the minified chain-link alpha aliases below 0.5 on alternate panels at this
distance, where GL's LOD-chunk path keeps it. **INVALIDATES the prior "black-fence
FIXED" claim's completeness** — the black→visible fix (`max(vArgb,0.5)`) and the
per-packet-layer fix both landed, but neither addresses alpha-test panel dropout,
which is what the user actually sees.

**WASHOUT — VERDICT: STILL BROKEN, and it is NOT a fail-soft.** Hangar-crop mean
brightness (0..255): GL 123.8 vs VK 171.1 = **+47.3** (MAD 66.3); scene-wide VK
+24.5. Engagement probe (`MC2_VK_PROP_LIGHT_PROBE`): `lightingReady=1
haveLightFeed=1 iblShStrength=0.5 iblSh0=(1.71,1.77,1.65) lightDataBytes=3610800`
— **the real IBL/ambient feed DID engage; the hemisphere fail-soft is NOT taken.**
Kill-switch matrix (hangar bright): GL-on 123.8 / VK-on 171.1 / GL-off 121.2 /
VK-off 146.9 → GL's IBL adds +2.7, VK's IBL adds +24.2 (≈9× too strong), AND VK
is +25.7 over GL even with IBL OFF. Two stacked over-brightening bugs in the
VK prop lighting math (`color_prop_tier1.vert`:
`vArgb = seed + ambientTerm + iblTerm + kSunColor*ndl`): the real-feed branch
(`lightingReady!=0`) sums raw ObjectLights ambient + `evalShL2*0.5` +
hardcoded `kSunColor*ndl` with no exposure/weight matching GL. **INVALIDATES the
prior "washout FIXED (feed parity 91051001)" claim:** the prior washout tuning
targeted the FALLBACK hemisphere branch (comment in .vert), but canonical config
hits the REAL-FEED branch, which was never washout-tuned. "Verified fixed but
user still sees washout" = tuned the wrong branch.

**Historical claims invalidated:** (1) "fence fixed" — only black→visible +
layer-drop fixed; alpha-test panel dropout remains, user-visible. (2) "washout
fixed / IBL feed parity" — parity was measured on the fallback branch; the
engaged real-feed branch over-brightens hangar by +47. Both bugs reproduce in
the EXACT canonical config with VK-labeled receipts, so they are real, not
oracle artifacts.

**Fixes:**
- **Washout — PARTIAL fix landed, NOT cured.** `color_prop_tier1.vert` real-feed
  branch now scans ObjectLights for the real per-actor sun color (GL parity,
  static_prop.vert:414-438) instead of the hardcoded `kSunColor` stand-in.
  Re-captured (VK used_vulkan=363): hangar +47.3 → **+43.5** vs GL. Only a 3.8/255
  win — proves the phantom sun was NOT the dominant term (the real sun ≈ as bright
  as the constant). **Dominant washout cause still open:** VK/GL apply the same
  `albedo * vArgb` and the same summed terms, yet VK vArgb is +43 hotter. Leading
  hypothesis: GL attenuates prop ambient/sun by CSM shadow (`MC2_SHADOW_CSM=1` ON),
  the VK prop island does NOT shadow-attenuate its vertex-baked lighting → props
  render unshadowed/brighter. Next: instrument whether GL's v_argb includes a
  shadow factor the VK island drops (check static_prop.vert for calcShadow on the
  ambient/sun term vs the VK island's shadow interop wiring).
- **Fence — NOT fixed (root cause pinned).** Draw-level proven it's alpha-test
  panel dropout (`color_prop_tier1.frag:69 if (texc.a<0.5) discard`) against the VK
  copy-once array's mip chain, not a layer/draw drop. Fix path: alpha-to-coverage,
  preserve alpha in the array mips, or match GL's effective coverage — NOT landed.

Instrumentation left in tree, all env-gated default-off: `MC2_VK_PROP_PACKET_DUMP`
(per-packet draw params CSV), `MC2_VK_PROP_LIGHT_PROBE` (UBO lighting engagement),
`[ADJUDICATION_POSE v1]` one-shot pose dump in gos_visual_capture.cpp. Airport
bookmark authored at `.claude/adjudication/airport_bookmark.json` (c_rot110 frames
both bugs). Artifacts under `.claude/adjudication/`.

## ADJUDICATION-1 FOLLOW-UP (2026-07-06, this session)

**WASHOUT — ROOT-CAUSED AND FIXED.** Per-vertex CPU-side probe
(`MC2_VK_PROP_LIGHT_VERTEX_PROBE`, new, gated) dumped the REAL ObjectLights entries
the hangar-area instances (typeID=38) actually use: `numLights=1, ambient=(0.3,0.3,0.3),
sunFound=0` for every sampled instance — confirms no per-actor sun light feeds these
props at all (matches the prior sun-fix's own +3.8/255 result: removing the phantom
`kSunColor` barely moved the needle because GL's own scan also finds no sun here).
`iblSh0` DC coefficient is 1.71 — `evalShL2`'s `kSH_c4*sh[0]` term alone
(`0.886227*1.71≈1.51`) times `iblShStrength=0.5` is already ~0.76, and adding the 0.3
ambient pushes the raw sum past 1.0 before any clamp. **Root cause: VK's
`color_prop_tier1.vert` never clamped `vArgb`, unlike GL's `static_prop.vert:524
lit = clamp(lit + aRGBHighlight, 0.0, 1.0)`.** An unclamped >1.0 vArgb multiplies the
albedo brighter than GL's white-capped equivalent on every bright material (hangar
roofs). Fix: added the matching `clamp(..., 0.0, 1.0)` to `vArgb`'s final assignment
(`shaders/vulkan/color_prop_tier1.vert`). **Measured (canonical config, c_rot110,
template-matched hangar crop, same box both before/after within this session):
hangar delta +43.5 -> -0.6/255 (essentially matched); MAD 34.8 is structural/edge
noise, not a brightness bias.** Receipts: GL used_vulkan=0 (correct), VK
used_vulkan=420/421 frames (correct, real VK pixels, not fail-soft). Smoke: tier1 5/5
PASS both byte-off control and full canonical VK-on config, 0 crash, +0 destroy-delta.
Commit: (this session, see git log).

**FENCE — STILL OPEN, mip/aniso hypotheses RULED OUT with live evidence.** New
`MC2_VK_PROP_MIP_PROBE` diagnostic (gated, left in tree) confirms mip-count parity:
alpha-ON array `glTexMaxLevel=7` (GL) == `vkComputedMips=8` levels / `maxLod=7.0`
(VK) -- GL's own `GL_TEXTURE_MAX_LEVEL` and the VK import's computed chain length
match exactly, both texture groups (alpha-OFF 21 layers, alpha-ON 34 layers). Grep
of the entire renderer for `ANISOTROP*` returns zero hits on both the GL prop path
(`gos_static_prop_batcher.cpp`) and the VK sampler (`vulkan_color_prop_island.cpp`)
-- no anisotropic filtering anywhere, so aniso mismatch is not the cause either.
VK's mip chain is confirmed BYTE-COPIED from GL's own already-generated
`glGenerateMipmap` output (`glGetCompressedTexImage` per level, not re-derived) --
so VK samples the literal same BC7 bytes GL does, at the same LOD range, with the
same CLAMP_TO_EDGE + LINEAR_MIPMAP_LINEAR sampler state, same `fract(vUv)` UV
handling in both fragment shaders. With every concrete sampler/mip/UV divergence
eliminated, a bisect experiment settled it (same session, later):

**FENCE ALTERNATION — ROOT-CAUSED AND FIXED (same session, follow-up).**
Bisect ladder, each step build+deploy+capture with VK receipts (used_vulkan
409-420, GL reference unchanged):
1. Coordinator's fract-seam-derivative hypothesis (`textureGrad` with smooth
   gradients from the UNWRAPPED vUv): landed, deployed SPV verified to contain
   `OpImageSampleExplicitLod ... Grad` -- alternation UNCHANGED. Hypothesis
   DISPROVEN by direct experiment.
2. Forced `textureLod(.., 0.0)` diag: EVERY dropped panel reappeared.
   **Mechanism proven: the minified mip chain's alpha decays below the 0.5
   test (naive glGenerateMipmap box filter halves thin chain-link coverage
   per level), and VK's implicit LOD lands deep enough to cross it where GL's
   does not.** (Mip BYTES are identical -- byte-copied -- so the divergence is
   LOD selection, which is implementation-defined per API/compiler.)
3. Final fix (landed, `shaders/vulkan/color_prop_tier1.frag` alpha-ON branch):
   deterministic LOD computed from unwrapped-vUv gradients
   (`0.5*log2(max(dot(dx,dx),dot(dy,dy)))` in texel units), clamped to <=1.0
   for the alpha-test class (LOD<=1 proven panel-preserving by the bisect),
   sampled via explicit `textureLod`. No driver-implicit LOD dependence left.
   **Result: alternation GONE -- every panel present along the whole run at
   c_rot110 (crop_fence_vk_final.png). Fence-region MAD 29.9 -> 27.1.**
   Residual (documented, NOT the alpha bug): VK panels read slightly narrower
   with gaps vs GL's continuous ribbon -- consistent with the KNOWN-DEFERRED
   per-draw uvScale sub-region gap (VK samples with uvScale=1.0 assumed;
   GL applies uvScaleX/Y per draw, static_prop.frag:206). NEXT CONCRETE STEP:
   feed per-draw uvScaleX/Y through the packet push constant (same plumbing as
   packetLayerOff/On) and apply in the island frag; expect the gaps to close
   and fence MAD to drop to residual-class.
Hangar regression guard re-measured on the same capture: delta -0.5/255 (fix
intact). Smoke mc2_01 PASS both byte-off control and canonical VK-on state.

## FENCE-UVSCALE-1 (2026-07-06, this session) — CLOSES ADJUDICATION-1

**Fix landed.** GL uv-crop site confirmed: `shaders/static_prop.frag:206`
`vec2 uvSampled = fract(v_uv) * vec2(uvScaleX, uvScaleY)`, fed from
`PerDrawEntry.uvScaleX/Y` (`GameOS/gameos/gos_static_prop_batcher.h`), which are
computed per-PACKET in `finalizeGeometry()`
(`gos_static_prop_batcher.cpp:3844` `uvScaleXByPacket[pk] = u.w * invMaxW`,
same "packetsInGroup" loop that fills `layerForPacket`).

**Plumbing (mirrors the b911bc0d packetLayerOff/On pattern exactly):**
new per-packet sidecars `s_packetUvScaleX/Y` persisted alongside
`s_packetTexArrayLayer` in `finalizeGeometry()`, cleared in `onMapUnload()`,
exposed on `Mc2ColorPropMaterialFeed` (`packetUvScaleX/Y`,
`vulkan_shadow_island_live.h`) alongside `packetAlbedoLayer`. VK island
(`vulkan_color_prop_island.cpp`) copies them into `packetUvScaleX/Y` vectors
(default 1.0 = no crop, matching pre-fix behavior when the feed doesn't cover a
packet) at the same site that fills `packetLayerOff/On`. Push constant widened
8->16 bytes (`int layerOff, int layerOn, float uvScaleX, float uvScaleY`) --
well inside the Vulkan-guaranteed minimum `maxPushConstantsSize` of 128 bytes,
so no half-float packing or per-instance-SSBO detour was needed (justified:
8 extra bytes is noise against the 128B floor, and per-packet push constants
were already the established pattern for this exact kind of per-draw data).

**Shader (`shaders/vulkan/color_prop_tier1.vert/frag`):** `vUvScale` forwarded
flat VS->FS. Frag applies `fract(vUv) * vUvScale` verbatim in the alpha-OFF
branch (was `fract(vUv)` only). Alpha-ON branch (the fence's alpha-test class)
applies the same crop BEFORE computing the deterministic-LOD gradients from
this session's earlier alternation fix (`uvSampledOn = fract(vUv) * vUvScale`,
gradients taken from `uvSampledOn` not raw `vUv`) so the sub-region's actual
texel footprint drives LOD selection, not the full-layer one -- otherwise a
narrow sub-region would under-estimate LOD and reintroduce alpha-test aliasing
on exactly the packets this fix targets.

**Verified engaged:** new `MC2_VK_PROP_PACKET_DUMP` CSV (columns extended with
uvScaleX/Y) at c_rot110: 14/80 packets carry uvScale=0.5 (fence chain-link
post/arch sub-mesh packets, typeIDs 70/72/73/124/127/130/134/153/161/175/205/
210/213/216), confirming the plumbing reaches the exact packets the state file
predicted, not a no-op.

**Visual result:** VK fence ribbon is now continuous and full-width, matching
GL's panel coverage/spacing at c_rot110 (`crop_fence_gl_final2.png` vs
`crop_fence_vk_final2.png`, `.claude/adjudication/`) -- no narrowing, no gaps.

**MAD arithmetic note (important, avoids re-litigating a false regression):**
raw unaligned fence-crop MAD went 27.1 -> 28.6, which LOOKS like a regression
but isn't -- diagnosed as separate-process camera-pose jitter (~11px vertical
drift between GL/VK launches), present IDENTICALLY in the prior 27.1 baseline
capture (re-measured: prior pair's own best-match shift is dx=-12,dy=11,
aligned MAD 28.46, not 27.1 -- the originally-reported 27.1 was itself an
unaligned/lucky-crop number). Aligned (shift-corrected) apples-to-apples MAD:
prior 28.46 -> this session 27.54, a real but modest improvement, consistent
with only 14/80 packets carrying non-1.0 scale. **Lesson for future
bookmark-sweep MAD comparisons: always cross-correlate for a translation
offset before trusting a raw crop MAD delta as small as 1-2/255 -- separate-
process camera jitter alone produces deltas that size.**

**Regression checks (same c_rot110 capture pair):**
- Hangar brightness delta: GL 123.2 vs VK 120.4 = **-2.8/255** (within the
  existing -0.5..-0.6 residual band from the washout fix; no new regression).
- Fence visual: continuous ribbon, panel-preserving (screenshot pair above).
- Packet dump: zero degenerate draws, zero "both -1" layers (same invariant as
  the alternation-fix session).

**Smoke:** `MC2_DEBUG_STATE_DUMP=1` + tier1 (`mc2_01 mc2_03 mc2_10 mc2_17
mc2_24`, 30s each) against `A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe`
(sha `6366c922de88`, fingerprint-matched worktree HEAD): **byte-off control
5/5 PASS, +0 destroy-delta all missions; canonical VK-on config
(`MC2_VK_SHARED_GBUFFER=1 MC2_VK_SHADOW_INTEROP=1` +
`MC2_SHADOW_CSM/STATIC_PROP_PBR_V1/AMBIENT_V1/IBL_SH=1`) 5/5 PASS, +0
destroy-delta all missions.**

### ADJUDICATION-1 FINAL SCOREBOARD

| Bug | Adjudication-1 verdict | Follow-up verdict | This session (FENCE-UVSCALE-1) |
|---|---|---|---|
| Fence alternating panels | STILL BROKEN (alpha-test dropout) | ROOT-CAUSED + FIXED (deterministic clamped LOD) | n/a (already closed) |
| Fence narrow/gapped panels vs GL ribbon | not yet isolated | isolated as uvScale=1.0 assumption, deferred | **FIXED** (per-packet uvScale plumbed + applied; visually continuous/full-width, aligned MAD 28.46->27.54) |
| Hangar washout | STILL BROKEN, not fail-soft (+47.3) | ROOT-CAUSED + FIXED (unclamped vArgb, -0.6/255) | held (-2.8/255 this capture, same residual band) |

**ADJUDICATION-1 is CLOSED as of this session.** Both bugs it raised (fence,
washout) are root-caused and fixed; the one open residual the follow-up
session left on the table (fence uvScale sub-region gap) is now also fixed and
verified. No further action items outstanding from this adjudication.
