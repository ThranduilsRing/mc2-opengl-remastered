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

## Item 6 — tree-foliage shadow alpha on coalesce/BC7-atlas path — RECON DONE, code pending

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

## Remaining

- [x] Item 3 complete (commit 2c3a9109, captures banked, verdict PASS)
- [ ] Item 5: code → commit → build → deploy → captures (gate-off identity vs `1936948e/3a6677de/c22ea0d4` + dirt A/B)
- [ ] Item 6: code → commit → build → deploy → captures (shadow A/B gate on/off)
- [ ] tier1 smoke at end: canonical command + `--exe` at lane + `MC2_SMOKE_REQUIRE_FINGERPRINT=0`
- [ ] release lane registry entry at session end
