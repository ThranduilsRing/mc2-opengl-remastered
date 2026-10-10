<!-- doc-status: stale | reviewed:2026-10-11 -->
# Terrain gate deletion plan (default-OFF gates: delete ON-only code, keep OFF path)

Read-only recon, HEAD 7159006ef. All line numbers are PRE-EDIT. Within a file, apply edits
BOTTOM-UP or anchor on the quoted text, not the number. Every gate below is registered default-OFF
(RendererFeatureRegistry.h) and every reader was confirmed default-OFF at source.

## 0. Scope decisions (read first)

* IN scope (7): MC2_TERRAIN_LIGHTING_V1, MC2_TERRAIN_LIGHTING_V2, MC2_TERRAIN_NORMALS_FROM_HEIGHT,
  MC2_TERRAIN_CLIFF_DISPLACE (+ its child MC2_TERRAIN_CLIFF_DISPLACE_MAXTESS, Trace, delete with it),
  MC2_TERRAIN_AMBIENT_FLOOR_NIGHT, MC2_TERRAIN_CONCRETE_DETAIL_NORMAL, MC2_PROP_LOD_SHADOW_CAST.
* KEEP MC2_TERRAIN_CONTROLMAP_FILE and MC2_TERRAIN_OVERLAY_V2_FILE. Parents MC2_TERRAIN_CONTROLMAP
  and MC2_TERRAIN_OVERLAY_V2 are default-OFF, are NOT in this task, and their sidecar loaders in
  mclib/terrain.cpp:885-905 / 986-1005 stay. The _FILE readers (mclib/terrain_init.cpp:278, :996), the registry
  rows (RendererFeatureRegistry.h:1445-1466), check-env-registry.sh:438 allowlist line and
  run_smoke.py:1569/1575 stay untouched. Only stale comment cross-refs are n/a. Re-evaluate when the
  parents are scheduled.
* NOT removed (still live, do not touch): terrainNormalsFromHeightStrength on the CHUNK path
  (terrain_lod_chunk.frag:473/1513, gos_terrain_lod_chunk.cpp s_locNfhStrength :854/:1353/:3009,
  VK feed nfhStrength vulkan_terrain_island.{h:130,cpp:363,2571}, GameOS/gameos/gos_terrain_lod_chunk_draw.cpp:46),
  gos_Set/GetTerrainNormalsFromHeightStrength, terrain_nfh_strength_, the NfH strength UI slider,
  the R32F height texture + terrainHeightTex/terrainHeightParams uniforms + computeTerrainNormalFromHeight()
  (gos_terrain.frag still uses them for debug mode 10 :488-500 and the shadowN path :938-940),
  mclib/terrain_init.cpp:157 gos_uploadTerrainHeightTex (unconditional), MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR,
  MC2_TERRAIN_CLIFF_SHADOW_FLOOR, MC2_TERRAIN_AMBIENT_FLOOR (default ON), gos_TerrainAmbientDayGate().
* Follow-up candidate (NOT in this patch): after NFH-gate removal the R32F height texture is consumed only
  by gos_terrain.frag (legacy, MC2_TERRAIN_LOD_CHUNK=0, "vestigial") debug mode 10 + shadowN. Ask owner
  before retiring gos_terrain_height_tex.cpp.
* RendererFeature enum is indexed by kFeatureTable (static_assert COUNT). Deleting 4 enum rows
  (TerrainNormalsFromHeight=23, TerrainLightingV1=24, V2=25, PropLodShadowCast=47) forces RENUMBERING
  26..46 (-3) and 48..60 (-4), COUNT 61 -> 57, despite the "never renumber" comment (line 83). Audit says
  safe: no code/test uses numeric values; users are by name (tests/unit/test_rendercore.cpp 331-405 only
  name ViewUniforms/ReverseZ/TerrainTessellation/Ssao/Vfx*/GpuMechs/StaticPropIndirect/DrawPacketStaticPropV6),
  GuiRuntime/EditorInspector.cpp:356 loops 0..COUNT. FALLBACK if owner objects to renumbering: convert the 4
  rows to tombstones `{"MC2_FEATURE_REMOVED_<n>", nullptr, EnvVarKind::Retired, false, "[REMOVED] ..."}`
  (nullptr envVar is already legal: ReverseZ). Tombstones keep numbering, drop the env names.

## 1. Per-gate site list

### G1. MC2_TERRAIN_LIGHTING_V1 + G2. MC2_TERRAIN_LIGHTING_V2 (delete together; V2 only modulates V1)

GameOS/gameos/gameos_graphics.cpp
* 1795-1798 delete the 4 accessors setTerrainLightingV1Strength/get..., setTerrainLightingV2Floor/get...
* 2298-2311 delete members `terrain_lighting_v1_strength_` (2304) and `terrain_lighting_v2_floor_` (2311)
  with their comment blocks (2299-2303, 2305-2310). Keep terrain_nfh_strength_ and cliff member.
* 2356 + 2358 (TerrainUniformLocs), 2411 + 2413 (ThinTerrainLocs), 2658 + 2659 (overlay locs):
  delete `terrainLightingV1Strength`, `terrainLightingV2ShadowFillFloor` GLint fields (+ "// TERRAIN-LIGHTING-1/2" lines).
* 2511-2515 (terrainLocs_), 2586-2590 (thinTerrainLocs_), 5666-5667 (overlay `locs`): delete glGetUniformLocation lines.
* bindTerrainHeightTexUniforms 7120-7203: drop params `lightingV1StrengthLoc, lightingV1Strength,
  lightingV2FloorLoc, lightingV2Floor`; delete body 7159-7186 (V1 block + V2 block, incl. comments).
  Update doc comment 7115-7119 (mentions NFH env gate).
* 3 call sites: 7500-7510, 7642-7652, 11163-11173 (also see NFH): remove the 4 V1/V2 args
  (tl./L.terrainLightingV1Strength, terrain_lighting_v1_strength_, ...V2ShadowFillFloor, terrain_lighting_v2_floor_).
* 10794-10806 delete gos_SetTerrainLightingV1Strength/GetV1, gos_SetTerrainLightingV2Floor/GetV2.
* Comment at 11150-11158 ("NFH height tex, V1 hemi, V2 ...") reword.

GameOS/gameos/gos_terrain_lod_chunk.cpp
* 839-840 delete `extern float gos_GetTerrainLightingV1Strength(); ...V2Floor();`
* 849-850 delete `s_locLightingV1`, `s_locLightingV2`.
* 1348-1349 delete the 2 glGetUniformLocation lines.
* 2868-2876: delete s_v1Env/s_v2Env statics + 2 glUniform1f lines; rewrite lead comment 2868-2872 (drop V1/V2 wording).
* 57 comment `(e.g. s_v1Env/s_v2Env)` reword. 2892 comment "hemisphere fill ... default-OFF (MC2_TERRAIN_LIGHTING_V1)" reword.
* BEHAVIOR NOTE: this reader is `getenv(...) != nullptr` (any value, even "0", ENABLED V1 with strength 1.0 and V2 floor 0.3 on the LIVE chunk path),
  unlike the legacy helper (`e[0] && e[0] != '0'`). Anyone with MC2_TERRAIN_LIGHTING_V1=0 exported in a shell had V1 ON; after deletion they get OFF.

GameOS/gameos/visual_tuning_profile.cpp
* 17-18 and 32-33 delete the 4 externs. 356-357 delete the 2 `current[...]` writer lines.
* 163-168 REPLACE the two `applyKey` branches with a silent no-op retired-key branch:
  `else if (!strcmp(key,"terrainLightingV1Strength")||!strcmp(key,"terrainLightingV2Floor")) { /* retired, ignored */ }`
  (otherwise stale installed copies of data/visual_tuning.json log `[VisualTuning] unknown key ... -- ignored`; the fallback at :267-272 is warn-once, not fatal).
data/visual_tuning.json: delete `terrainLightingV1Strength` (line 5) and every `terrainLightingV2Floor`
  (lines 6, 15, 21, 27, 34, 40, 46 plus any later gaea_peaks_* / other mission blocks; grep to be exhaustive). Keep valid JSON (trailing commas!).
  deploy_payload.py:118 copies this file into installs; installed copies keep old keys (harmless with the retired-key branch).

shaders
* include/terrain_height_normal.hglsl:38-45 delete uniforms terrainLightingV1Strength, terrainLightingV2ShadowFillFloor (+comments); header comment lines 1-16 ("6 uniforms", force-zero wording) reword.
* gos_terrain.frag:1005-1036 delete V1 block incl. `hemiContrib` decl (1014) and `c.rgb += hemiContrib`; delete debug modes 43 (1038-1048) and 11 (1050-1065) (they would render black). Comments 1005-1013, 1038-1040 go with them.
* terrain_overlay.frag:449-480 delete the whole "TERRAIN-DECAL-LIGHTING-1a" V1 block (comment 449-461 + `if (terrainLightingV1Strength > 0.0) {...}`). Keep `c.rgb *= shadow;` at 447.
* terrain_lod_chunk.frag:468-469 delete the two uniform decls; 2099-2109 delete the `if (terrainLightingV1Strength > 0.0) {...}` block but KEEP `vec3 hemiContrib = vec3(0.0);` (2099: still `+=`ed by ambient-floor at 2184 and read by debug view 43 at 2206). Reword/trim stale prose 2079-2098 and 2113-2145 ("WHAT ... CANNOT DO", "WHY NOT JUST TURN ON MC2_TERRAIN_LIGHTING_V1": rewrite as history "the removed V1 hemisphere add was albedo-independent...").
* vulkan/terrain_overlay_tier1.frag:13 comment only.

GuiRuntime/GraphicsOptionsWindow.cpp
* 38-41 delete the 4 forward decls (V1/V2 get/set).
* 188 delete debug-mode row `{ 11.0f, "Hemi Additive", ...}` (kDiagnosticStart=8 index is unaffected).
* 546-673 UI rows: delete the whole "Lighting V1" (609-634), "Lighting V2" (636-671) sections, the `litGateOn/litV2GateOn` locals, and line 673 text -> `"Debug Mode 10 = height-normal RGB"`.
  Retitle SeparatorText at 553 ("Normals-from-Height + Lighting V1" -> "Normals-from-Height"); fix comment 546-552.

RenderCore/RendererFeatureRegistry.h
* Enum rows 139-143 (TerrainLightingV1 =24, V2 =25 + comments) delete.
* Table rows 522-537 (the `// TerrainLightingV1` and `// TerrainLightingV2` entries) delete (order must match enum).
* Prose edit: row MC2_TERRAIN_AMBIENT_FLOOR (line ~909) mentions MC2_TERRAIN_LIGHTING_V1 twice; reword to "the (removed) V1 hemisphere add". Line 528 NFH cross-ref goes with the deleted row.

scripts/ + tests
* scripts/registered-gate-passthrough-debt.txt: delete lines 65, 66 (V1, V2) AND 68 (NORMALS_FROM_HEIGHT). REQUIRED: the check is two-way, a debt entry that is no longer "missing" FAILS ("fixed; delete the line").
* scripts/capture_baseline.py:78-85 delete NFH/V1/V2 env keys (+comments 78, 82, 84).
* tests/smoke/matrices/terrain.json: delete entry `terrain_nfh_v1_v2` (13-19); edit `terrain_debug_mode_10` (22-27): drop the NFH env key and the "gate must be ON" sentence (mode 10 does not depend on the gate, see gos_terrain.frag:488). Update top `description` ("Terrain normals-from-height and lighting variants"). check-smoke-matrices.py (contract `smoke_matrices`) FAILS if a matrix env key is unregistered.
* scripts/check-lighting-divergence.py (contract `lighting_divergence`, pinned inventory!): remove from `ambient_fill_vec3` the 6 rows for gos_terrain.frag hemiGroundTint/hemiSkyTint (268-269), terrain_lod_chunk.frag hemiGroundTint/hemiSkyTint (274-275), terrain_overlay.frag hemiGroundTint/hemiSkyTint (276-277); fix comments at lines 8 ("FOUR hemisphere sky/ground fill pairs"), 129-131, 263-266, and docs/lighting-divergence-map.md:26,60,61,109-117,205. Keep terrain_lod_chunk ambGnd/ambSky and the 0.55->1.0 mix:shadow pin.
* tests/visual/baselines/*.json sidecars record `"MC2_TERRAIN_LIGHTING_V1": "1"` etc.: HISTORICAL RECORDS, do not rewrite. After capture_baseline.py stops recording these keys, new sidecars simply omit them; verify scripts/check-capture-provenance.py / check-capture-env-declared.py do not demand the old keys (they read manifests under tests/visual/captures|baselines; confirm by running).

docs
* docs/tier1_env_vars.md:774-775, 959-960 delete V1/V2; docs/trackv-lighting-consistency-opus-1.md:49-50, 117-118; docs/visual-tuning-profiles.md:35-36, 59-60, 161-170 (V2 floor column in the per-mission table; mark keys retired); docs/render-pipeline-map.md:99-100.
* docs/flags.auto.yaml: regenerate (section 3).

### G3. MC2_TERRAIN_NORMALS_FROM_HEIGHT (gate only; the strength uniform stays for the chunk path)

* gameos_graphics.cpp: 2352, 2407, 2656 delete `useTerrainNormalsFromHeight` GLint; 2506, 2581, 5664 delete its glGetUniformLocation.
  Legacy-program strength loc `terrainNormalsFromHeightStrength` (fields 2354, 2409, 2657; lookups 2508-2509, 2583-2584, 5665) becomes dead after the shader edit: delete it too
  (chunk path has its own s_locNfhStrength in gos_terrain_lod_chunk.cpp, unaffected).
  bindTerrainHeightTexUniforms: drop params `gateLoc, strengthLoc, strength`; delete 7143-7155 (`gateOn` computation, `glUniform1i(gateLoc..)`, strength upload). KEEP the height-tex bind (7131-7137), params upload (7138-7149), and cliff-floor block (7187-7203).
  Call sites 7500-7503, 7642-7645, 11163-11166: remove `*.useTerrainNormalsFromHeight`, `*.terrainNormalsFromHeightStrength`, `terrain_nfh_strength_` args.
  Keep setTerrainNormalsFromHeightStrength/get (1793-94, 10787-10792) and the member (2298).
* shaders: gos_terrain.frag:786-804 delete the `if (useTerrainNormalsFromHeight != 0 ...) {...}` block and its comment (keep `N.xy = clamp...`); terrain_overlay.frag (inside the V1 block, already removed);
  include/terrain_height_normal.hglsl:32 delete `useTerrainNormalsFromHeight`, 34-36 delete `terrainNormalsFromHeightStrength` (chunk frag declares its own at :473). KEEP terrainHeightTex/Params, u_terrainCliffShadowFloor, computeTerrainNormalFromHeight.
  terrain_overlay.frag then no longer calls the helper; the `#include` at :22 may stay (declares u_terrainCliffShadowFloor which overlay locs query) - minimal option: leave it.
* GraphicsOptionsWindow.cpp:550-552 comment; 556-570 delete `nfhGateOn` status block; 602 tooltip line `"Only effective when MC2_TERRAIN_NORMALS_FROM_HEIGHT=1."` -> "Scales the macro-slope tilt on the chunk terrain path.";
  605-607 delete `if (!nfhGateOn) TextDisabled(...)`. KEEP height-tex readout/resample combo/NfH strength slider (strength still drives live chunk path).
* RendererFeatureRegistry.h: enum 135-137 (TerrainNormalsFromHeight =23) + table 514-521.
* mclib/terrain_init.cpp:157 comment reword only (no code change).
* debt file line 68 (see above); capture_baseline.py:78-79; smoke matrix (above); tools/shader_reflect goldens (section 3).
* Docs: tier1_env_vars.md:772, docs/VISUAL-CAPTURE-MATRIX-1-DESIGN.md:49, render-pipeline-map.md:99 (that file wrongly says NFH is ON; registry/reader are OFF).
* ADVERSARIAL: debug mode 10 and the legacy shadowN path still need the height texture: do NOT stop uploading it.

### G4. MC2_TERRAIN_CLIFF_DISPLACE (+ _MAXTESS) - dead-ends the whole tess variant program

GameOS/gameos/gos_terrain_lod_chunk.cpp
* 174-192 delete SLICE-3a banner, `s_cliffTessGate`, `s_cliffTessWanted`, `s_cliffTessClamped` (191), `clampTess` (193-203).
* 205-208 delete `s_terrainTessProgram`, `s_locCliffTessLevel`.
* 210-232 delete `s_tessMirroredThisFrame` + the 10 `s_tessLoc*` statics (+ banner 210-216).
* 234-342 delete `mirrorTerrainUniforms` (whole function, incl. the CLIFF_TESS warn prints).
* 344-358 delete `resolveTessPatchLocs`.
* 1410-1437 delete the `if (s_cliffTessGate && !s_terrainTessProgram) {... makeProgram2("terrain_lod_chunk_tess", ...tesc/tese...)}` block (+ its comment).
* 1468-1478 delete the `if (s_terrainTessProgram != 0) {... deleteProgram("terrain_lod_chunk_tess") ...}` teardown block.
* 1636-1678 delete `mirrorTerrainPatchUniforms` (banner 1636-1645 included).
* 2120 delete `s_tessMirroredThisFrame = false;`.
* 3362-3396: keep ONLY `glDrawElements(GL_TRIANGLES, patch.indexCount, GL_UNSIGNED_SHORT, 0);` - delete the `useTess` decl, `if (useTess) {...} else {` and closing brace (incl. ZoneScopedN("Terrain.CliffTess"), glPatchParameteri).
* `glsl_program::makeProgram2`, GL_PATCHES VAO etc: confirm makeProgram2 has other callers before touching it (grep; only delete if tess-only).
Shader files (all reachable ONLY via the deleted program):
* DELETE shaders/terrain_lod_chunk.tesc and shaders/terrain_lod_chunk.tese (no CMake/script list references them; glob-scanners just stop seeing them).
* shaders/terrain_lod_chunk.vert: remove `#ifdef TERRAIN_TESS` arms, keep the `#else` arm: :99-101 (delete `out vec4 v_clip;`), :240-244 (`gl_Position = clipD;`), :387-391 (`gl_Position = clip;`). The frag has no TERRAIN_TESS/u_cliffTessLevel refs.
RendererFeatureRegistry.h: rows 2766-2778 (MC2_FEATURE_TERRAIN_CLIFF_DISPLACE and MC2_TUNE_TERRAIN_CLIFF_MAXTESS, both kAuxEnvVars; no enum entry).
scripts/run_smoke.py:2905-2910 delete the comment + both strings. docs/superpowers/{plans,specs}/*slice3* are historical, leave.
ADVERSARIAL: none in data/mods/scripts. GL_MAX_TESS_GEN_LEVEL / tess-capable GL context requirement disappears (no behavior change; gate was OFF). `[CLIFF_TESS 3a]` log tag disappears; grep showed no parser.

### G5. MC2_TERRAIN_AMBIENT_FLOOR_NIGHT

* gos_terrain_lod_chunk.cpp:2923-2984: delete the whole NIGHT-1 comment block (2923-2974) and `s_nightGateFloor`/`s_nightEnvSet` (2975-2976); replace dayGate computation (2977-2984) with
  `if (s_locAmbientDayGate >= 0) glUniform1f(s_locAmbientDayGate, gos_TerrainAmbientDayGate());` (keep rawDayGate local if the trace uses it).
  Trace fprintf at :2999-3006 (`MC2_TERRAIN_AMBIENT_FLOOR_TRACE`, kept gate): remove `raw_dayGate`, `night_floor`, `night_env_set` fields and their args (`rawDayGate, s_nightGateFloor, s_nightEnvSet`); no script parses the line (grep clean). Rename tag only if desired.
* 4273-4294: delete `s_nightEnvSetFlag`, `gos_TerrainAmbientNightFloor()`, `gos_TerrainAmbientNightFloorEnvSet()` (4273-4287). Rewrite `gos_TerrainAmbientDayGate()` (4289-4294) to `return gos_GetTerrainDaylightLevel();` (VK feed at :1957 and header :277 keep calling it; byte-identical since max(raw,0)==raw for raw>=0; confirm raw is never negative - it is a 0..1 level).
* gos_terrain_lod_chunk.h:275-276 delete the two NightFloor declarations (keep :277 DayGate).
* shaders: nothing to remove (frag only sees u_terrainAmbientDayGate). vulkan shaders unaffected.
* RendererFeatureRegistry.h:911-920ff: the comment + MC2_FEATURE_TERRAIN_AMBIENT_FLOOR_NIGHT row in kAuxEnvVars (no enum entry). Also scrub "...NIGHT" cross-refs inside the MC2_TERRAIN_AMBIENT_FLOOR row if any.
* scripts/run_smoke.py:1750-1755 delete the NIGHT comment + string (keep MC2_TERRAIN_AMBIENT_FLOOR and _TRACE).
* docs/tier1_env_vars.md:790-796 (NIGHT entry; also refs at 781-788 limitation note remain valid, reword the "can be reached with NIGHT" bits).
* ADVERSARIAL: scripts/check-lighting-divergence.py:181 pins gos_terrain_lod_chunk.cpp <-> MC2_TERRAIN_AMBIENT_FLOOR (unaffected, but re-run). tools/terrain_daylight_harness does not reference the night floor (grep clean).

### G6. MC2_TERRAIN_CONCRETE_DETAIL_NORMAL (OFF path = matNormalBoost.w forced to 0)

* gos_terrain_lod_chunk.cpp:918-932 delete `mc2TerrainConcreteDetailNormalEnabled()` (keep the `extern gos_GetTerrainMatNormalBoost` at 918 - used at 2777).
* 2786-2804: replace `if (!mc2TerrainConcreteDetailNormalEnabled()) nb[3] = 0.0f;` with unconditional `nb[3] = 0.0f;  // concrete detail normal permanently off (user ruling 2026-07-26)`; trim the long comment 2789-2803 to 3 lines.
  This keeps the shader byte-identical (zero multiplier) - Phase 1.
* Phase 2 (optional, perf only, same pixels): terrain_lod_chunk.frag:1494-1498 delete `if (pureConcrete > 0.001) {...textureGrad(matNormalArray...MAT_LAYER_PAINTED_CONC...)}` and :796-799 `if (w.w > 0.01) {...dN += w.w * matNormalBoost.w ...}` (both multiply by the now-zero .w). Needs a visual diff; skip if the dead-uniform `matNormalBoost.w` slider (GraphicsOptionsWindow.cpp:421-446 `s_boostConcrete`) is wanted for the legacy gos_terrain.frag path, which still uses the real value (GameOS/gameos/gameos_graphics_tparams.cpp:346/7604 upload terrain_mat_normal_boost_ unforced).
* RendererFeatureRegistry.h:2789-2800 (comment + MC2_FEATURE_TERRAIN_CONCRETE_DETAIL_NORMAL row, kAuxEnvVars).
* scripts/run_smoke.py:1796 delete string. docs/tier1_env_vars.md:867.
* ADVERSARIAL: GL-chunk only; VK terrain has no matNormalBoost (grep clean). Installs with data/terrain_layers/ shipped (matNormalArray loads) are the only ones where the flag mattered; OFF stays the default for everyone.

### G7. MC2_PROP_LOD_SHADOW_CAST

* mclib/bdactor_internal.h:22-31 delete the comment + `s_propLodShadowCast` lambda.
* mclib/bdactor.cpp:8215-8242: replace
  `if (!s_propLodShadowCast) { GpuStaticPropRegistry::setRecipeNoShadow(regIdx, true); }` with the unconditional call; rewrite the stale-rationale comment (8205-8239) to a 4-line note pointing at .claude/SHADOW-DECISIONS-1-STATUS.md (the exclusion rationale is documented there as false but benefit undemonstrated; call kept intentionally).
  Keep setRecipeNoShadow (gos_static_prop_registry.{h:176,cpp:2035}) and `noShadow` (registry.cpp:281/651/2089): still used.
* RendererFeatureRegistry.h: enum 231-235 (PropLodShadowCast =47) + table 706-715.
* scripts/run_smoke.py:1851-1855 delete comment + string. docs/tier1_env_vars.md:260.
* ADVERSARIAL: none in data/mods. Only one call site (TreeAppearance::registerStatic). Enum renumber needed (section 0).

## 2. Registry edits in one pass (RendererFeatureRegistry.h)

1. Remove enum rows: TerrainNormalsFromHeight, TerrainLightingV1, TerrainLightingV2, PropLodShadowCast; renumber all following explicit values, set COUNT = 57 (or use tombstones). 
2. Remove the 4 matching kFeatureTable rows IN THE SAME POSITIONS (table is positional; the static_assert at ~833 only checks length).
3. Remove kAuxEnvVars rows: AMBIENT_FLOOR_NIGHT (~911-920), CLIFF_DISPLACE + CLIFF_DISPLACE_MAXTESS (2766-2778), CONCRETE_DETAIL_NORMAL (2789-2800).
4. Keep CONTROLMAP_FILE / OVERLAY_V2_FILE rows. No test hardcodes COUNT (test uses COUNT symbolically).
5. Order vs code: delete ALL getenv readers first (or same commit). A reader left behind for a removed row makes `env_registry` FAIL (unregistered); the reverse (row left, reader gone) passes but leaves registry lies.

## 3. Generated artifacts

* docs/flags.auto.yaml: `py -3 tools/gate_gen.py --write` (deterministic, writes docs/flags.auto.yaml; full-file churn in `referenced_at` line numbers is expected). Then `py -3 scripts/check-flag-registry-truth.py` and `sh scripts/check-flag-registry-drift.sh` (advisory).
* tools/shader_reflect/expected/shaders__gos_terrain.frag__default.json (uniforms at ~223-240) and shaders__terrain_overlay.frag__default.json (~62-80): `py -3 tools/shader_reflect/reflect.py --update` (contract `shader_schema` reads these tracked goldens). terrain_lod_chunk.frag has no golden. Review the diff: only the removed uniforms (+ possibly now-unused height-tex uniforms on overlay) should go.

## 4. Adversarial risk register

1. Shell/mod env: operators exporting MC2_TERRAIN_LIGHTING_V1/V2/NFH/etc. silently lose effect. scripts/mc2_env_pin.py scrubs unknown MC2_* for run_visual_capture (loud, recorded as ambient_scrubbed). LIGHTING_V1/V2 "=0" on the chunk path previously ENABLED V1 (any set value); that corner flips to OFF.
2. Saved options/profile files: only data/visual_tuning.json carries keys (terrainLightingV1Strength, terrainLightingV2Floor); reader tolerates unknown keys with a once-per-key stderr warn, so keep the silent retired-key branch. No options-file / .ini / .cfg / mods/ hit for any gate or key (grep clean over data, mods, res, release_assets*, staging, examples).
3. Uniform locations queried elsewhere: V1/V2/NFH-gate uniforms are queried at gameos_graphics.cpp 2506-2515, 2581-2590, 5664-5667 and gos_terrain_lod_chunk.cpp 1348-1349; ALL must be removed with the shader decl (a leftover query just returns -1, harmless, but struct fields/helper args must be removed together or the helper signature will not compile). Chunk-path terrainNormalsFromHeightStrength (-> nfhStrength in VK feed/UBO off 232) MUST stay; do not "clean" it.
4. Debug modes: gos_terrain.frag modes 11 and 43 (hemi) go away; UI row 188 must go with them; mode 10 stays and no longer depends on any gate. Chunk debug view 43 (u_lightingDebugView) stays (ambient-floor feeds hemiContrib).
5. Pinned inventories that WILL go red if not updated together: registered-gate-passthrough-debt.txt (65,66,68), smoke matrix terrain.json, check-lighting-divergence.py rows + docs/lighting-divergence-map.md, shader_reflect goldens, flags.auto.yaml.
6. tests/visual/baselines/*.json and tests/smoke/artifacts/*.json record these env names as historical evidence: leave alone. Old baseline PNGs captured with V1/V2/NFH=1 cannot be reproduced after deletion; note in the commit.
7. Enum renumber (section 0) - only risk is out-of-tree code indexing kFeatureTable by int; none found in repo.
8. Build dirs (build64, build_prune, build64-tests, build64-h-*) are stale objects; do not grep results from them.
9. Another agent is concurrently building: do not start until it finishes; edits touch gameos_graphics.cpp (10k+ lines) and the registry header which many lanes edit - rebase against HEAD, re-resolve line numbers by anchor text.

## 5. Ordered apply checklist

1. Fresh tree on HEAD; record pre-change evidence: `py -3 tools/visual_verify/ab.py` day + night default-env captures (see step 14) into a scratch dir; run `sh scripts/check-contracts.sh` once to learn the pre-existing red set.
2. Shaders first: edit gos_terrain.frag, terrain_overlay.frag, terrain_lod_chunk.frag (V1 block, keep hemiContrib decl), include/terrain_height_normal.hglsl, terrain_lod_chunk.vert (TESS arms); delete terrain_lod_chunk.tesc/.tese.
3. gos_terrain_lod_chunk.cpp/.h: G1 uniforms, G4 (tess machinery), G5 (night floor + header decls), G6.
4. gameos_graphics.cpp: G1/G3 helper signature + 3 call sites + loc structs + accessors + gos_* wrappers.
5. visual_tuning_profile.cpp + data/visual_tuning.json.
6. GraphicsOptionsWindow.cpp UI + debug-mode row.
7. mclib/bdactor.cpp (G7), mclib/terrain_init.cpp:157 comment.
8. RendererFeatureRegistry.h (enum + table + aux rows), single pass.
9. scripts: registered-gate-passthrough-debt.txt, run_smoke.py (5 spots: NIGHT 1750-1755, CONCRETE 1796, PROP_LOD 1851-1855, CLIFF 2905-2910; leave CONTROLMAP_FILE/OVERLAY_V2_FILE), capture_baseline.py, check-lighting-divergence.py, tests/smoke/matrices/terrain.json.
10. Docs: tier1_env_vars.md, visual-tuning-profiles.md, lighting-divergence-map.md, trackv doc, render-pipeline-map.md, VISUAL-CAPTURE-MATRIX-1-DESIGN.md.
11. Regenerate: `py -3 tools/gate_gen.py --write`; shader_reflect.py --update.
12. Leftover sweep (must be empty outside tests/visual/baselines, tests/smoke/artifacts, docs/superpowers, .claude, graphify-out, build*):
    `grep -rnE "TERRAIN_LIGHTING_V[12]|TERRAIN_NORMALS_FROM_HEIGHT|TERRAIN_CLIFF_DISPLACE|AMBIENT_FLOOR_NIGHT|CONCRETE_DETAIL_NORMAL|PROP_LOD_SHADOW_CAST|terrainLightingV|useTerrainNormalsFromHeight|s_cliffTess|AmbientNightFloor" .`
    (expect hits only: the two kept _FILE vars are NOT in this pattern; chunk-path terrainNormalsFromHeightStrength is NOT in this pattern).
13. Build: engine + tests + harnesses per CLAUDE.md/BUILD-WIN.md (RelWithDebInfo build64 and build64-tests; include the GL shader load at startup - a GLSL compile error shows as `[TerrainLOD v1] shader program compiled OK` missing / shader_schema red).
14. Validate:
    * `sh scripts/check-contracts.sh` (watch: env_registry, flag_registry_truth, gate_default_agreement, registered_gate_passthrough (+selftests), smoke_matrices, lighting_divergence, shader_schema, doctest tier incl. RenderCore suite).
    * `py -3 scripts/run_smoke.py --tier tier1 --fail-fast` (and `py -3 scripts/run_smoke_matrix.py` terrain matrix: default + debug_mode_10).
    * `py -3 tools/visual_verify/ab.py --bookmarks <day_bookmark>,<night_bookmark>` before/after with NO env: expect pixel-identical (MAD 0) for all 7 gates since all were default-OFF (note chunk path ambient floor stays ON in both). Day: terrain+concrete pad bookmark (e.g. mc2_03 per MC2_TERRAIN_AMBIENT_FLOOR notes); night: one of mc2_05/mc2_10 (the maps where AMBIENT_FLOOR_NIGHT would have mattered). Also run a GL-vs-VK pass since VK reads nfhStrength/ambientDayGate from the shared producers.
    * Smoke-run `MC2_TERRAIN_DEBUG_MODE=10` once to confirm the height-normal view still renders.
15. Commit in 2-3 logical commits (shaders+engine; registry+scripts+goldens+docs); co-author trailer per session reminder.
