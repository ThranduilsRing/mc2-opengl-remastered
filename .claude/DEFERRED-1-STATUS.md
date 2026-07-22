# DEFERRED-SWEEP-1 status — worktree A:/Games/mc2-deferred-1, branch claude/deferred-sweep-1 (base nifty 994c3776)

Lane: `A:/Games/mc2-opengl/releases/mc2-win64-deferred-test` (NEW; base = rc1 copy 2026-07-16, robocopy 28351 files;
lane-identity files reset; deploys via scripts/deploy_payload.py --allow-new-target only). NEVER push. Do not touch
mc2-main / mc2-opengl-src trees. Lane registered in lane_registry as DEFERRED-SWEEP-1.

## Item 3 — cement pad proper asset fix — DONE (commit `2c3a9109`)

- **Reconstructed truth (docs were stale):** the "normal-map-as-albedo" claim inverted. On 2026-06-14 the on-disk
  `data/textures/mat6_normal.tga` (2048², matNormalArray layer 6 = MAT_LAYER_PAINTED_CONC) was literally swapped to a
  concrete ALBEDO (md5 307d1b5f; backup `mat6_normal.tga.orig_normal` = true normal map, md5 59d801b7) to serve the
  CEMENT-DETAIL-COLOR layer6-as-color experiment. `ead760df` (06-16) then made cement ALBEDO come from the cement
  diffuse atlas permanently (later absorbed by CEMENT-HARD-EDGE-1 — **no stopgap code exists anymore; retirement
  considered and REJECTED**), leaving every install (v0.4 AND rc1 — identical files, "fixed in rc1 not 0.4" was stale)
  decoding ALBEDO-as-NORMAL for cement relief (`terrain_lod_chunk.frag` ~1031, `gos_terrain.frag` ~720).
- **Fix:** true normal map repo-tracked at `data/textures/mat6_normal.tga` (narrow .gitignore allowlist) + shipped via
  `deploy_payload.py` `GAME_DATA_PAYLOAD`; docs/known_issues.md + docs/asset-pipeline.md rewritten. No code change,
  no new env vars.
- **Build:** full configure+build in-worktree (RelWithDebInfo, VS2022 CMake full path, MC2_IMGUI=ON) + mc2_launcher.
  Deployed to lane @ src_commit 2c3a9109 (clean, no -dirty).
- **Proof (mc2_03 airport apron, world (192,1830) — found by decoding pak Packet-0 terrainType; scratch bookmarks):**
  - BEFORE (lane deployed with `--preserve data/textures/mat6_normal.tga`, i.e. albedo-content file) vs AFTER
    (full deploy, true normal): chunk path (default) hashes changed on all 3 poses
    (`e733d80b→6b5386f2`, `bc73dfad→ccda590f`, `cce7e289→4b29459e`); delta max 15/255, 2-4% px >2 —
    subtle by design (cement is authored flat-lit; effect concentrates on transition-boundary shading).
    Eyeballed triptych: no regressions, pads keep authored tone.
  - Legacy path (MC2_TERRAIN_LOD_CHUNK=0): byte-identical before/after — legacy compile falls back to matNormal3
    (never samples layer 6). Expected.
  - Captures: `.claude/captures/item3_before_chunk/`, `item3_after_chunk/`, `item3_before_legacy/`,
    `item3_after_legacy/` (+ scout sets `item3_scout/`, `item3_hunt/`); witness bookmark JSON in scratchpad,
    poses recorded in capture sidecars.
- Note: v0.4 / rc1 installs still carry the albedo-content file — fixed mechanically by their next deploy_payload run
  (file now in GAME_DATA_PAYLOAD). This lane already carries the fix.

## Item 5 — POM slice 1 (per-layer heights via material lib) — DONE (commit `76f095cd`)

- **Verification (all on lane exe @76f095cd, clean fingerprint, `[TerrainLOD v1] shader program compiled OK`,
  `[TerrainMaterialLib] applied 49 keys` — was 45, +4 \*_height keys):**
  - Gate-OFF identity: mc2_24_pom_closeup default env → hashes `1936948e/3a6677de/c22ea0d4` BYTE-IDENTICAL to the
    pre-change exe baseline (`item5_gateoff_exe1_mc2_24/` vs `item5_gateoff_exe2_mc2_24/`). Default path untouched.
  - Gate-ON A/B (same exe): A = POM=1 (`item5_pomon_matliboff/`) vs B = POM=1+MATLIB=1 (`item5_pomon_matlibon/`)
    — hashes differ on all poses (matlib also moves tint/roughness; confounded for color, so:)
  - Heat-viz oracle (arc doc's own witness, u_diag 4096 = |pomOff|, tint-immune early-return):
    `item5_heat_A_matliboff/` vs `item5_heat_B_matlibon/` — large offset-DEAD (blue) ground regions in A turn
    live textured parallax (red) in B on mc2_24 desert. Exactly the "dirt fragments previously blank now light up"
    prediction. Heat HF-energy shifts (A 2.56→B 1.96 rot45) = displacement field structurally changed.
  - Deferred: snow height term (4-wide matWeights; snow separate scalar — needs march signature change, follow-up).
  - Note for arc doc: the pomScaleMat "2.5 concrete" literal is actually on the .z=DIRT slot; preserved VERBATIM.

## Item 5 log (superseded planning notes)

- Preflight: `slice-preflight POM-PERLAYER-HEIGHT-1` vs base 994c3776 → clean (exit 0, findings []).
- Premise verified: POM-1 + texremap ARE merged on base (terrain_lod_chunk.frag, terrain_material_lib.*,
  terrain_materials.json present).
- **Plan (locked):** `matHeight` vec4 (rock/grass/dirt/concrete) cloned from the gos_SetTerrainMatRoughness pattern:
  gameos_graphics.cpp members+setters+gos exports; terrain_material_lib.cpp JSON keys `*_height` (default 1.0);
  gos_terrain_lod_chunk.cpp upload beside matRoughness (same u_useMaterialLib block); frag: extend
  `chunkSampleDisplacement` with per-layer height multipliers + NEW dirt term, gated
  `(u_useMaterialLib != 0 && u_pomView.x > 0.5)` → inert unless BOTH gates on (gate-OFF-on-either byte-identical).
  **The 2.5 literal in `pomScaleMat = vec4(1.0,1.0,2.5,1.0)` stays VERBATIM** (note: it sits on the .z=DIRT weight
  slot, not concrete — docs' "concrete 2.5" is a slot misread; preserving the literal preserves behavior regardless).
  Snow term deferred (matWeights is 4-wide; snow is a separate scalar — documented deferral). No new env vars.
- Baseline banked (item-3 exe, gates OFF): mc2_24_pom_closeup hashes `1936948e / 3a6677de / c22ea0d4`
  (`.claude/captures/item5_gateoff_exe1_mc2_24/`) — post-change gate-OFF must reproduce these.
- NEXT: code edits → commit → build → deploy → captures (gate-off identity + POM-on matlib-off vs POM-on matlib-on
  dirt-displacement A/B on mc2_24) → then item 6.

## Item 6 — tree-foliage shadow alpha on coalesce/BC7-atlas path — DONE (commits `fddba31c`, `259eda79`)

- Repro findings: default config (coalesce ARMED, group RGBA8 arrays) tree shadows are ALREADY leaf-shaped on mc2_03
  (legacy per-texture binds still valid). `MC2_STATICPROP_BC7=1` probe: trees render SOLID BLACK in the color pass
  (separate pre-existing BC7 defect — captures `item6_probe_bc7_mc2_03/`) and shadows go chunky/solid.
- **Plan (locked):** extend `shadow_static_prop.frag` with `MC2_SHADOW_PROP_ATLAS` prefix-define variant
  (sampler2DArray u_texArr + u_texLayer + u_uvScale, `fract(v_uv)*u_uvScale` like the coalesce color/depth-prepass);
  second program `shadow_static_prop_alpha_atlas` in gameos_graphics.cpp; batcher: persist per-packet uvScale sidecars
  (mirror `s_packetTexArrayLayer` at gos_static_prop_batcher.cpp:3844), resolve per-packet array+layer
  (BC7 bucket via `s_packetBucketIndex`/`s_bucketArrays`, else group array via `type.alphaClass` →
  s_texArrayOn/Off) in BOTH `flushShadow` (~7965) and `drawDynamicPropShadows` (~8297); unresolvable packet →
  legacy-handle bind fallback (today's behavior). New killswitch `MC2_SHADOW_PROP_ALPHA_ATLAS` default ON
  (+ registry/tier1 docs/run_smoke passthrough/check-env-registry).
- Witness: mc2_03 overview bookmark (trees + shadows visible). A/B = new gate on/off on the same exe.
- **Implementation:** atlas shader variant + per-packet array/layer/UV-scale binding landed in `fddba31c`.
  Audit found the original proof trace only covered `flushShadow`, which can execute before coalesce reaches READY;
  `259eda79` adds the same one-shot trace to the later full-registry dynamic-prop caster where atlas use actually occurs.
- **Runtime A/B (same clean exe @259eda79):** gate ON PASS 2749 frames and emitted
  `[SHADOW_PROP_ATLAS v1] event=engaged pass=dynamicProp packets=557 buckets=0 bc7=0`
  (`tests/smoke/artifacts/2026-07-16T12-20-37/`); gate OFF PASS 2754 frames with no engagement marker
  (`2026-07-16T12-21-23/`). Prior deterministic default capture is PASS with all three bookmarks present at
  `.claude/captures/item6_after_default/` (commit fddba31c, exact exe fingerprint).
- **Final soak:** tier1 PASS 5/5, +0 destroys on every mission, 30 seconds each
  (`tests/smoke/artifacts/2026-07-16T12-24-31/`).

## Item 7 — Shadow stutter on >500-unit camera jumps — ALREADY FIXED, docs de-staled (no code change; commit `<pending>`)

Board entry: "Shadow stutter on >500-unit jumps (static world-fixed map, design ready)" (`.claude/DEFERRED-BOARD-2026-07-16.md` §A backlog, [MAIN]).

**Design source found:** `docs/architecture.md` Shadow Pipeline section (pre-edit) — "`shouldRenderShadows()` skips pre-pass when camera moves <500 units... **Planned:** Static world-fixed shadow map (render once at load)." Same phrase ("static world-fixed shadow map (design ready)") echoed in `docs/known_issues.md:45,67` and `docs/observations/2026-05-14-render-pipeline-matrix.md:143`.

**Finding: the "planned" fix already SHIPPED, 2 months before the still-open known_issues.md lines were even written.** Commit `e9b9aea7` (2026-05-16, "feat(shadow): static full-map shadow feed + Phase-1 camera-accum retire") retired the camera-windowed-accumulate prime block wholesale — `git log -S"shouldRenderShadows"` / `-S"cachedLightSpaceMatrix_"` both return zero hits in current code; the only remaining static-shadow trigger is the `gos_StaticLightMatrixBuilt()` mission-lifetime latch (`mclib/txmmgr.cpp` ~2692), reset ONLY by `Terrain::destroy` (new mission) or the RAlt+F3 re-enable path — never by camera position. `docs/known_issues.md:45,67` were added 2026-05-24/25 (`git blame`: `884b169e0`, `b0cf24edd`) — AFTER `e9b9aea7` — and were simply never reconciled back to the shipped fix. Classic stale-ledger case, same pattern as DEFERRED-BOARD item A#2.

**Empirical re-verification (no code touched; 2026-07-17, exe @`259eda79` deployed to `mc2-win64-deferred-test`):**
- New permanent regression-guard fixture `tests/visual/bookmarks/mc2_24_shadow_jump500.json`: two proven-good mc2_24 poses from `mc2_24.json` (`highangle_wide` (300,300) → `ridge_lowangle` (-400,-400), `shadow_cascade` coverage tag), placed back-to-back — one hard teleport of `sqrt(700²+700²) ≈ 989.9` world units, comfortably over the ~500u threshold, landing on a big terrain-ridge self-shadow.
- Ran `scripts/run_visual_capture.py` at `MC2_VISUAL_SETTLE ∈ {1,2,5,5(rerun),30,60}` (deterministic fixed-timestep clock). `jump_target` capture sha256 at settle `1,2,30,60` = **identical** (`1b16f6cc...`) — byte-for-byte, 0 changed pixels, `maxdelta=0` (numpy diff, `.claude/captures/shadowstutter_settle{1,2,30,60}/`). settle=5 (both runs) differed only in a ~246×18px strip in the extreme top-left corner (0.003–0.005% of pixels, maxdelta 8–11, not reproducible run-to-run) — consistent with a debug/HUD text overlay timing jitter, NOT scene/shadow content (confirmed: the jump_target frame itself, e.g. the ridge self-shadow visible in `.claude/captures/shadowstutter_settle60/mc2_24_jump_target.png`, is unaffected).
- Conclusion: the rendered shadow is **fully converged one frame after the hard cut** — no multi-frame pop, no settle-dependent shadow drift, at any settle value tested. This directly refutes the bug class as currently reproducible.

**Why no new gate:** the task's default posture (new env var, default OFF) assumes an in-progress behavior change. Here the "fix" is not a change I'm introducing — it is 2-months-shipped, unconditional, mission-lifetime-latched default behavior with no alternative code path left to gate against (the old camera-move-gated re-bake was deleted, not toggled, by `e9b9aea7`). Adding a synthetic env var would gate nothing real. The honest slice is: verify, add a permanent regression-guard fixture, de-stale the ledger.

**Docs de-staled this session:** `docs/known_issues.md` (both stutter lines, §Player-visible + §Shadows), `docs/architecture.md` (Shadow Pipeline "Caching"/"Planned" paragraphs), `docs/observations/2026-05-14-render-pipeline-matrix.md:143` (Cascade structure bullet). All cite `e9b9aea7` + this session's fixture.

**Deferred / out of scope:** the DEFERRED-BOARD-2026-07-16.md §A entry itself lives in `A:/Games/mc2-opengl-src/.claude/` — outside this worktree's write scope per the task's tree restriction. It needs the same one-line de-stale; flagging for the board owner rather than editing it directly.

**Not investigated further (no evidence found, out of scope for this slice):** whether some *other*, differently-shaped camera-jump artifact exists (e.g. terrain chunk-LOD pop-in, unrelated to shadows — `docs/superpowers/strategy/visual-regression-lab-architecture.md` §2.3 documents a known 1-frame chunk-LOD MVP latency, comfortably inside the default 30-frame settle window and not shadow-specific). If a *visual* (non-shadow) jump artifact is ever reported, it is a separate investigation.

## Item 8 — Water "HDR clipped" at odd resolutions — ROOT-CAUSED + FIXED (commit `337c2c1e`)

Board entry: "Water HDR clipped at odd resolutions" (`.claude/DEFERRED-BOARD-2026-07-16.md` §A backlog, [MAIN], suspect 480×270 reflection FBO).

**Repro matrix (mc2_01 shoreline, `tests/visual/bookmarks/mc2_01_water.json` water_overview_high + water_oblique_low, via `scripts/run_visual_capture.py`, lane exe):**

| Window size | Aspect | Framing anomaly vs 16:9 baseline | Pixel stats (near-white/near-black) |
|---|---|---|---|
| 1920x1080 | 1.778 | baseline | 0% white, 0% black (overview); 0% white, 12.5% black (oblique — terrain silhouette in-frame) |
| 1366x768  | 1.779 | none (matches baseline) | same as baseline |
| 1360x765  | 1.778 | none | same as baseline |
| 1707x960  | 1.778 | none | same as baseline |
| 3072x1500 | 2.048 | **YES** — oblique_low's content window shrinks from 85% to 70% of height and the terrain/sky silhouette drops out of frame entirely (same crop as the pure-water overview pose) | no near-white/near-black anomaly at this resolution specifically (the "very dark/blown/noisy" wording wasn't visually reproduced in THIS deploy — see HDRI caveat below) |

The 4 near-16:9 sizes were visually and statistically indistinguishable; only the one resolution far from 4:3/16:9 (3072x1500, the exact aspect from the original bug report) showed a framing difference — the honest signal that this is aspect-dependent, not a generic odd-resolution flake.

**Root cause:** `shaders/gos_terrain_water_mdi.frag`'s `dFdx(nz)` aspect-compensation term (`WATER-ASPECT-CORRECT-1`, `_ac = vpAspect * 0.75`) assumed the perspective frustum is unconditionally 4:3-locked while the viewport is native-res. That was true historically, but `UI-ASPECT-ANCHOR-1` (`mclib/camera.cpp`, already shipped) now builds the frustum at the REAL drawable aspect by default (`MC2_CAMERA_ASPECT_NATIVE` defaults ON). The shader's hardcoded `* 0.75` kept "correcting" a stretch that no longer exists, injecting an aspect-proportional distortion into the wave-reflection normal (nzGrad.x) that grows the further the window aspect departs from 4:3 (`_ac`=1.333 at 1920x1080, `_ac`=1.536 at 3072x1500 — both should be 1.0 under the now-default native-aspect frustum).

**Verified via the shader's own diagnostics** (confirms the geometry itself is NOT stretched, i.e. nothing for `_ac` to correct by default):
- `MC2_WATER_DEBUG_MODE=10` (built-in Aniso probe, `dFdx(WorldPos)`/`dFdy(WorldPos)` ratio): water_overview_high reads mid-gray ~103.6/255 at 1920x1080 and ~103.7/255 at 3072x1500 — statistically identical, resolution-independent.
- `MC2_WATER_ASPECT_DIAG=1` (pre-existing diagnostic, `mclib/camera.cpp`): reports `stretchX` != 1.0 at both sizes, but this diagnostic compares `Environment.screenWidth/Height` (still hardcoded 800x600 for the legacy 2D HUD canvas) against drawable size — it was never updated after UI-ASPECT-ANCHOR-1 changed the ACTUAL frustum build to use drawable aspect directly, so it's now measuring the wrong two values and is itself stale (not touched this session; flagging in case a future session wants to fix or retire it).

**Fix (3 files, commit `337c2c1e`):**
- `mclib/camera.cpp`: new global `g_cameraProjAspectNative`, set each frame in `Camera::setOrthogonal`'s perspective branch to whether THIS frame's frustum build actually used native aspect.
- `GameOS/gameos/gameos_graphics.cpp`: new uniform `u_waterProjAspect` uploaded in the water MDI bind block — native vpAspect (making `_ac`=1.0) when `g_cameraProjAspectNative`, else legacy 4/3.
- `shaders/gos_terrain_water_mdi.frag`: `_ac = vpAspect / u_waterProjAspect` (was hardcoded `* 0.75`) — general formula, correct in both the native (default) and legacy (`MC2_CAMERA_ASPECT_NATIVE=0`) cases.

**Before/after verification:**
- Default-path (SH-sky-only, no HDRI, `MC2_WATER_REFLECTION_RT` off — this deploy's actual default rendering config) at 1920x1080 and 3072x1500: byte-identical PNGs before/after the fix (`item8_water_*` vs `item8_AFTER_*` captures). Expected — see HDRI caveat.
- Native-vs-legacy A/B on the SAME (post-fix) exe, `MC2_WATER_DEBUG_MODE=9` + `MC2_WATER_REFLECTION_RT=1` (forces the high-frequency terrain-reflection-RT sampling path, which IS sensitive to `nzGrad`) at 3072x1500: different sha256 + visibly different reflected content between `MC2_CAMERA_ASPECT_NATIVE` unset (fixed/native, default) vs `=0` (legacy, recreates the pre-fix `_ac` formula exactly). Confirms the fix changes real rendered output in the predicted direction.
- **HDRI caveat (pre-existing, unrelated, NOT fixed this session):** this deploy's overcast-sky HDRI fails to load — `[HDRI_SKY v1] bc6h: sun_scan skipped reason=exr_load_failed ... mud_road_puresky_16k.exr` — because it's a large (16K) external-pack asset not installed locally (`docs/testing/ibl_hdri_external_pack.txt`, by design, not tracked in git). Water reflection therefore falls back to the smooth low-frequency SH-L2 sky term in every capture this session, which is comparatively insensitive to the `nzGrad` distortion the bug injects — explaining the byte-identical default-path result above. The bug (and this fix) should have materially more visible impact wherever the HDRI IS installed (the sun-disk reflection is high-frequency, much more sensitive to a distorted sampling direction) — closer to the original "very dark/blown/noisy" report. Not chased further; out of scope (asset install, not a code bug).
- Captures: `.claude/captures/item8_water_*` (before matrix), `item8_AFTER_*` (post-fix re-run), `item8_aniso_*` (Aniso probe), `item8_diag_*` (WATER_ASPECT_DIAG), `item8_reflcheck_native` / `item8_reflcheck_legacy` (native-vs-legacy A/B).
- Build+deploy: `337c2c1e` on lane `releases/mc2-win64-deferred-test`, fingerprint matches, no -dirty. tier1 5/5 PASS, +0 destroys (`2026-07-16T20-17-14`).

## Item 9 — First-launch black terrain (intermittent) — NOT REPRODUCED this session

Board entry: "First-launch black terrain (intermittent)" (`.claude/DEFERRED-BOARD-2026-07-16.md` §A backlog, [MAIN], repro via `tier1 --fail-fast`).

**Protocol:** own-PID-discipline cold-launch loop (never `--kill-existing`, never taskkill by image name — only manage PIDs launched by the hunt script itself), fresh `mc2.exe` process per attempt, mission `mc2_01` (tier1's first mission, matching the board's framing), real (non-fixed-timestep) clock so a genuine timing race isn't masked by the deterministic clock path, `MC2_LOAD_RACE_TRACE=1` set throughout per the task's own pointer to the 3d13c2de/3b70425b diagnostics.

**Pass 1 (15 attempts):** `scripts/run_visual_capture.py` single-frame eyeball capture (glReadPixels FBO readback, trigger frame 200, `MC2_VISUAL_CAPTURE_FRAME`/`_DIR`, no bookmark file so the free-capture path fires). All 15/15 present, all normal terrain: mean RGB [103.1-104.1, 99.7-100.8, 85.6-87.0], near-black fraction 0.04-0.05% — remarkably stable, no outlier. Captures: `.claude/captures/item9_coldlaunch/attempt{01..15}/`.

**Pass 2 (8 attempts):** the CANONICAL purpose-built oracle discovered in `GameOS/gameos/gameosmain.cpp` — `MC2_SCREENSHOT_AT_FRAME=200` + `MC2_SCREENSHOT_PATH=<tga>` (dumps the offscreen sceneFBO, works minimized, near-zero overhead) + `MC2_LOAD_RACE_TRACE=1`'s one-shot `LOADRACE verdict:` line (`docs/tier1_env_vars.md` documents `terrain_resident=Y/N` + `terrain_first_resident_frame` fields — exactly what this hunt needs). All 8/8 screenshots ground-truth-verified normal (same stable stats as pass 1). Captures: `.claude/captures/item9_loadrace/attempt{01..08}/`.

**Real finding (not the target bug, but confirmed and worth recording): `MC2_LOAD_RACE_TRACE`'s `terrain_resident`/`degraded` verdict fields are a FALSE NEGATIVE under the default renderer.** All 8 pass-2 runs printed `terrain_resident=N terrain_first_resident_frame=-1 degraded=YES` — looked like a hit at first — but:
- The ground-truth screenshot for every run showed completely normal terrain (verified via numpy/PIL pixel stats, matching pass 1's stable baseline).
- The SAME logs also carry the modern GPU-indirect/chunk path's own readiness signals — `[LOADRACE] atlas first-resident frame=0 gltex=135 scenarioTime=0.00` and `[LOADRACE] gpu terrain first-arm frame=0 scenarioTime=0.00 atlasTex=135 nodeIds=65` — firing at frame=0 on every single one of the 8 runs, zero variance.
- `mech_degrade_real=0`, `mech_ok_real=6`, `appr_null=0` on every run too (all 6 real mechs loaded fine; the 6 "mech degrade" events per run are benign preview-only, per the verdict's own `mech_degrade` vs `mech_degrade_real` split).
- Root cause: the `terrain_resident` probe (`mclib/txmmgr.cpp` ~3096-3130, inside `Render.TerrainSolid`) only iterates `masterVertexNodes[]` (the LEGACY per-quad path); under the default chunk-path renderer (`MC2_TERRAIN_LOD_CHUNK` default-ON since `a7b090be`) that array apparently never populates with terrain nodes, so `terrTotal` stays 0 and `fully = (terrTotal>0) && ...` is permanently false — a stale diagnostic, not evidence of a real race. Flagged as a follow-up (spawn_task `task_4bc93a0a`); NOT fixed this session (out of scope, and fixing a diagnostic doesn't touch the actual bug hunt).

**Verdict: NOT REPRODUCED, closing as such this session.** 23 total cold launches, two independent ground-truth-verified detection methods, zero black-terrain occurrences, zero anomalies in either the modern terrain-readiness signals or the mech-appearance-load signals. It's documented as intermittent — this session's sample came up clean, which doesn't rule out the bug, just this session's luck/environment. **Right ambush for next time:** `MC2_LOAD_RACE_TRACE=1` + `MC2_SCREENSHOT_AT_FRAME=<N>` + `MC2_SCREENSHOT_PATH` (native, cheap, minimized-safe) — watch the raw `atlas first-resident`/`gpu terrain first-arm` frame numbers and the screenshot itself. Consider more attempts, a genuinely cold machine/disk state (this session's launches were back-to-back on an already-warm machine), or cycling across more of tier1's 5 missions rather than just mc2_01.

### Item 9 follow-up — LOADRACE terrain-residency false negative — FIXED (commit `235b88f3`, 2026-07-16)

The `task_4bc93a0a` follow-up flagged above is done. Fix in `mclib/txmmgr.cpp` ~3096-3150 (the `MC2_LOAD_RACE_TRACE` block under `ZoneScopedN("Render.TerrainSolid")`): gated the residency probe on `mc2TerrainLodChunkEnabled()` (cached static bool, no new per-frame cost). Chunk/GPU-indirect path (default) now reuses the already-latched `g_loadRaceAtlasFirstFrame` / `g_loadRaceArmFirstFrame` globals — the same state driving `gos_terrain_indirect.cpp`'s own `[LOADRACE] atlas first-resident` / `[LOADRACE] gpu terrain first-arm` one-shot lines — two `long` compares, zero walk. The legacy `masterVertexNodes` walk is untouched, byte-identical to before, and now only runs when `MC2_TERRAIN_LOD_CHUNK=0`.

**8-cold-launch re-verification** (own-PID discipline, fresh process/attempt, `MC2_LOAD_RACE_TRACE=1` + `MC2_SCREENSHOT_AT_FRAME=200` + `MC2_SCREENSHOT_PATH`, mc2_01, lane exe @`235b88f3`):

| attempt | terrain_resident | terrain_first_resident_frame | degraded | mech_degrade_real | screenshot |
|---|---|---|---|---|---|
| 1-8 (all) | Y | 1 | no | 0 | present, rc=0 |

3 spot-checked screenshots (attempt01/04/08) pixel stats: `3840x2160 mean_RGB≈(104.2-104.3, 100.9-101.0, 87.1-87.3) near_black≈0.068-0.070%` — matches item 9's healthy-terrain baseline (mean RGB ~103-104, near-black <0.1%) and confirms no false positive.

**Legacy-path control (`MC2_TERRAIN_LOD_CHUNK=0`):** 3 runs (frame 200 and frame 2000/scenarioTime 14.24s) all report `terrain_resident=N degraded=YES`. Investigated whether this is a regression: **it is not.** `MC2_BUCKET_CENSUS=1` shows `legacy=0` at every frame under `MC2_TERRAIN_LOD_CHUNK=0` — `masterVertexNodes` never gets a single terrain-flagged entry in this build. Root cause: `TerrainQuad::setupTextures()` (`mclib/quad.cpp` ~706-1337) — the only code that would populate/mark those nodes resident — is `#ifdef MC2_IS_EDITOR`-gated out of game builds entirely, by commit `006800e5` ("compile-gate setupTextures to the editor build"), an ancestor of `235b88f3` and unrelated to this session. The exe's own runtime banner confirms this: `[8Z_VESTIGIAL] MC2_TERRAIN_LOD_CHUNK=0 opt-out is increasingly vestigial: ... the chunk-OFF path retains makeLists but has no production renderer.` So in a game build there is no "good run" to demonstrate `terrain_resident=Y` via the legacy opt-out — `N` is the correct, honest answer given zero populated nodes, not a fixable false negative. The legacy branch is preserved verbatim for the one build config where it CAN have a signal (`MC2_IS_EDITOR`, passability-grid path) but was not exercised here (out of scope — game exe only, per task). Documented in `docs/tier1_env_vars.md`'s `MC2_LOAD_RACE_TRACE` entry.

**tier1 re-verified:** PASS 5/5 (`2026-07-16T22-15-25`, lane exe @`235b88f3`, fingerprint OK, +0 destroys every mission).

**Files touched:** `mclib/txmmgr.cpp` (fix), `docs/tier1_env_vars.md`, `docs/known_issues.md`, `.claude/DEFERRED-1-STATUS.md` (this entry).

## Remaining

- [x] Item 3 complete (commit 2c3a9109, captures banked, verdict PASS)
- [x] Item 5 complete (commit 76f095cd, gate-off identity + dirt A/B banked)
- [x] Item 6 complete (commits fddba31c + 259eda79, runtime A/B + deterministic capture banked)
- [x] Item 7 complete — shadow stutter already fixed pre-session (`e9b9aea7`); docs de-staled + regression fixture added this session, no code change, tier1 re-verified below
- [x] Item 8 complete — water aspect-correction bug root-caused + fixed (commit `337c2c1e`), tier1 5/5 re-verified after fix
- [x] Item 9 complete — first-launch black terrain NOT REPRODUCED after 23 cold launches / 2 detection methods; documented + closed, no code change (found + flagged an unrelated stale-diagnostic bug in passing)
- [x] Item 9 follow-up complete — LOADRACE terrain-residency false negative FIXED (commit `235b88f3`); 8/8 cold launches now `terrain_resident=Y degraded=no`; legacy-path control found pre-existing (non-regressed) vestigial-renderer condition, documented
- [x] tier1 smoke PASS 5/5 (`2026-07-16T12-24-31`, re-verified `2026-07-16T20-17-14` post item-8 fix, re-verified `2026-07-16T22-15-25` post item-9-followup fix)
- [x] release lane registry entry already present for DEFERRED-SWEEP-1

## Batch 5 — three user-directed visual refinements (2026-07-22)

Deployed to lane @ src_commit f45167f7 (clean, no -dirty). Evidence: `.claude/captures/batch5/`.

### Item 1 — shadow donut — DONE (commit `38d5d8d9`, SHADOW-DONUT-FIX-1)
- **Root cause:** SHADOW-DIST-FADE-1's occluder-distance probe AVERAGED all 16 taps (4 Poisson x 4
  staircase depths). At silhouette EDGES half the ring samples sky -> estimate collapses -> no fade ->
  dark ring; interior gets full estimate -> full fade -> washed center = OUTLINE "donut".
- **Fix:** per-probe occlusion, dNorm = MAX over the 4 spatial probes (edges inherit the interior's
  estimate -> uniform blur+fade across the silhouette) + fade capped at new env
  `MC2_SHADOW_DIST_FADE_MAX` (default 0.65; 1.0 = old full erase) for the softer interior.
  All 3 shader variants lockstep; injected at all 4 compile sites.
- **Evidence:** `i1_defaults/` (crop_i1_defaults_shadowzone.png: filled soft silhouettes, no ring)
  vs `i1_max100/` (old-style full erase, far tips dissolve) vs `i1_max005/` (fade ~off). All three
  hash-distinct on all 3 mc2_10 poses.

### Item 2 — pad area light — DONE (commit `f45167f7`, PAD-AREA-GLOW-1)
- Cement-word tiles (binding-25 SSBO, VALID bit) + feathered spill (`MC2_PAD_AREA_GLOW_SPILL`,
  default 48wu, 3x3 tile distance-to-rect mask) get a mild uniform additive warm lift at night;
  nightFactor = 1 - max(missionFlatLight) premultiplied CPU-side -> day = exact (0,0,0) add =
  byte-identical. Chunk frag + overlay frag lockstep. Gate `MC2_PAD_AREA_GLOW` (strength) default
  ON 0.12; =0 kill.
- **Evidence** (mc2_05 pad clusters found by pak terrainType decode, `bm_mc2_05_pads.json`):
  `i2_max/` glow=3 absurd extreme (pads blown white, feathered corners visible);
  `i2_default/` vs `i2_off/` mean lift +[20,17,14] RGB on pad_big, 78% px changed; spill extremes
  `i2_spill4/` vs `i2_spill127/` differ in a 5.3% px edge band (feather knob works).

### Item 3 — mover light color — DONE (commit `033cb516`, MOVER-LIGHT-HALOGEN-1)
- Egg-yolk source: all three MOVER SpotLight_ registration sites hardcoded aRGB 0xffffff00 (pure
  yellow) — mech3d.cpp anubis pointLight + generalized path, gvactor.cpp. Buildings were already
  warm 0xffe8c870 (bdactor.cpp) — untouched.
- Now `mc2_spotlight_diag::mover_light_argb()`: default halogen 0xFFEED1, env
  `MC2_LIGHT_POOL_MOVER_COLOR=RRGGBB` (FFFF00 = legacy).
- **Evidence:** `i3_default/` vs `i3_legacy/` vs `i3_green/` (absurd 00FF00) all hash-distinct on
  mc2_05 lance pose; crop_i3_maxdelta_{default,green}.png shows an active mover spotlight glare
  flipping with the env (player-lance lights inactive at the early capture frame — user click-through
  still owed for the in-gameplay look).
