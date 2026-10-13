# Lane 2 wave 2 cleanup review (gate amnesty), range 642019a64..583a0c263, branch claude/lane2-s3

Reviewer: read-only Sonnet cleanup pass, 2026-10-12. Method: `git show`/`git diff` of every non-generated hunk, plus read-only re-runs at HEAD 583a0c263:
`gate_gen.py --check` OK, `log_tag_census.py --check` OK, `check-gl-shader-variants.py` 231/231, `gl_vk_feature_coverage.py --check` 0 new / 0 stale,
`cxx17_ratchet.py --check` OK ("31 fewer legacy sites than baseline"). No build was run (not permitted).

## Verdict: ACCEPT-WITH-FIX
The deletions preserve behaviour against the old gate defaults (checked row by row below), no deleted gate name has a live reader in engine code, and the
generated tables are consistent. Nothing needs reverting. The fixes: one lost VK-parity record (F1), dead-after-promotion leftovers (F2-F4, F6, F7), comments and
docs that now say the opposite of the code (F5), proof-hygiene gaps (F8).

## Findings, by severity

**F1 (HIGH, record loss): the promoted features are GL-only, and the only machine-checked VK-parity record for them was deleted with the gates.**
The brief asks that each promotion run on GL and VK. It does not, and a ledger row is wrong:
- Cliff triplanar / height-normal / cliff POM, night tags, fixture emission, base-light direction: no VK twin field or code exists (git grep over shaders/vulkan, vulkan_*.cpp,
  RenderCore finds none); `shaders/vulkan/terrain_tier1.frag:198-200` itself says "DEFERRED vs GL: ... cliff triplanar".
- Terrain POM: the ledger row says "VK has no POM". Wrong. `shaders/vulkan/terrain_tier1.frag:155-241` carries `mv_parallax`/`MATV_POM_PARAMS`, the legacy faux-view march
  ported "VERBATIM" from GL `chunkParallax()`, which 9bec41219 deleted. GL is now real-view-vector POM with a distance fade; VK is the deleted algorithm, always on. The comment
  at :155-163 cites a function and line range that no longer exist.
- `tools/vk_not_ported.allowlist` rows for MC2_TERRAIN_POM, _CLIFF_POM, _CLIFF_TRIPLANAR left with the gates (9bec41219, 1c4018bd9). The allowlist header says its `--flip-check`
  exists to stop a flip that "would change GL only"; deleting the gate sidesteps that guard instead of satisfying it. `.claude/VK-PARITY-GAPS.md` rows 9 and 11 still name the deleted
  gates; `vk-default-flip-checklist.md` has no row.
Fix: one VK-PARITY-GAPS row per GL-only promotion (real-view POM + fade, cliff triplanar/height-normal/POM, night tags + fixture emission, base-light dir 0.45, decal fill/colorblend,
building static shadow) naming the VK shader to change; correct the ledger row; fix the comment at `terrain_tier1.frag:155-163,198`; mirror the rows into the VK default-flip checklist.

**F2 (MEDIUM, now-dead logic): the fixture discriminator no longer does anything.** `kNightEmissiveScale == kFixtureEmissiveScale == 1.0`
(`gos_static_prop_batcher_internal.h:506-507`) and `static_prop.vert:507-510` computes `isFixture ? max(a, b) : a`, which is 1.0 on both branches. Dead as a consequence:
`isEmissiveFixtureType` (`..._state.cpp:255`, `..._internal.h:508`), the `hotPinkRGB.w` flag and `s_fixtureTypeCount` (`gos_static_prop_batcher_finalize.cpp:103-127`, with its
"[GPUPROPS] fixture-lights ... scale=" log), `u_fixtureEmissiveScale` (vert uniform, two `ProgramLocs` fields, two uploads), `kFixtureEmissiveScale`. Fix: delete them, set
`emissiveScale = u_nightEmissiveScale`, then make that a shader constant (the SSBO padding lane is free again; no struct change, so no UBO lockstep risk). Do with F6.

**F3 (MEDIUM): the building-shadow promotion left the "include buildings in the dynamic pass" arm alive but unreachable.** `mclib/txmmgr_render_passes.cpp:94`
`constexpr bool s_skipBldgInDynamic = true;` feeds `includeBldg = !true` (:114) and the cache-key machinery at :122-140 (`s_dynPropInstsIncludeBldg`), and is passed to
`flushShadow(true)` at :287 and :311. Dead: the `includeBuildings` parameter of `getDynamicPropShadowInstances` (`gos_static_prop_registry.h:210`, `.cpp:950-957`) and the
`skipStaticBuildingTypes` parameter of `flushShadow` (`gos_static_prop_batcher.h:288`, `..._shadow.cpp:152,280`). `bldgTriggerStaticShadowRebakeIfArmed` (`bdactor_static.cpp:711`) lost its
"IfArmed" meaning and two parameters (`recipeIndex`, `reason` are now unused; callers `bdactor.cpp:430`, `bdactor_static.cpp:738`): inline `gos_ResetStaticLightMatrix()` at both callers and
delete the declarations (`bdactor.cpp:396`, `bdactor_internal.h:182`). `gos_renderer_input_probe.cpp:178-184` has a comment claiming the engine passes `includeBuildings=true`.

**F4 (MEDIUM): `g_staticPropAlphaGeneration` (asked): delete it.** Bumped at `mclib/msl_shape.cpp:107-110`, read nowhere (git grep over GameOS, mclib, code, tools, tests). Remove: the
definition `gos_static_prop_batcher.cpp:62-66`, both externs (`gos_static_prop_batcher.h:555`, `..._internal.h:401`), the bump and its SP-BATCHER-ALPHASCAN-GATE-1 comment in
`msl_shape.cpp`, and the link stubs `tools/asset_viewer/asset_viewer_game_stubs.cpp:10`, `tools/tg_import_dump/tgdump_seams.cpp:22`. Proof needs `tools/build_all_targets.py` (the stub TUs).

**F5 (MEDIUM): comments and docs that are false or narrate the deleted gate** (both forbidden by the rules).
- `shaders/static_prop.vert:148-153` says the strength is 0.0 by default and the CPU uploads 0.0 "when MC2_PROP_BASE_LIGHT_DIR is unset". It is 0.45 always. Also
  `static_prop.frag:133,301` ("CPU uploads 0 unless MC2_STATIC_PROP_DEBUG_MATERIAL is set"), `:421`; `static_prop.vert:677,712,845,855,912` (IBL_NIGHT_SCALE / BASE_LIGHT_DIR as gates).
- `gos_postprocess.h:277-281` ("Seeded from MC2_POST_CAS ... at init", "gate OFF"); `bdactor_static.cpp:173`; `mclib/cliff_decal_tuning.h:5`; `txmmgr_render_passes.cpp:102`;
  `gos_static_prop_batcher.h:21,285`; `gos_renderer_input_probe.h:100`; `code/missiongui_uicmd_probe.cpp:263`; `shaders/include/terrain_height_normal.hglsl:24`;
  `gos_static_prop_batcher_state.cpp:226-232,336` (narrates "used to pass a daytime state").
- `docs/tier1_env_vars.md` still documents deleted gates as live: :201, :479 (BRIDGE_COMPARE, LEGACY_DISPATCH), :948 (BUILDING_SHADOW), :1100 and :1144-1158 (the SPFLUSH_COST_SPLIT and
  LIGHTBAKE_STABILITY triage recipes tell the reader to set env vars that no longer exist). `tests/telemetry/tag-registry.json:88-95,129-137,309,480` and the tag whitelists
  (`diagnostic_trace.cpp:28-57`, `scripts/mcp/mc2_render_state_server.py:873-875`) keep tags with no emitter.
- `docs/debug_state_schema.md:60` and `debug_state_dump.cpp:473` keep `MC2_SNAPSHOT_STATIC_PROP_BUILD` as a hard-coded `true` "kept constant for readers"; no reader exists (git grep).
  Delete the key. `snapshotDispatchDefault`/`legacyDispatch` are now exact complements (`..._seam.cpp:834-835`): keep one.
- `Gates/gate_table.cpp:601-603,982-985,990` orphan comments ("MUST STAY LAST ... enum value (62)", "StaticPropLightFreshness ... (61)", "DrawPacket v8 kill-switch",
  "STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1") sit above unrelated rows; delete.

**F6 (LOW-MEDIUM): dead off-arms and shells the deletions made trivially inlineable.**
- `u_glassDither` (`static_prop.frag:196`; uploads `..._flush_prep.cpp:474-475` and the legacy copy in `..._flush_dispatch.cpp`; VK `vulkan_color_prop_island.cpp:1523` + the `color_prop_tier1`
  UBO field) is always 1.0; the 0.0 arm is dead. Lockstep holds today; delete the GL and VK pair together, and drop the "(the engine uploads 1.0)" narration.
- `u_pomView.x/.w` uploaded (`gos_terrain_lod_chunk_draw.cpp:865` sends 1.0) but unread; `pomParams.y/.z` are frozen constants uploaded as uniforms while the cliff constants are shader `const`s
  (`terrain_lod_chunk.frag:424-436`, with mixed naming `cliffTriplanarStrength` vs `kCliffPomDepth`): make POM consistent and drop `s_locPomView`. `triTilt * cliffTriplanarStrength` (:1593)
  multiplies by 1.0. `static_prop.vert:749` keeps `u_propBaseLightDirStrength > 0.0` for a value that is always 0.45.
- Bare `{ ... }` scope shells where `if (gate)` used to be: `terrain_lod_chunk.frag` near :614, :1496, :1514, :1559; `static_prop.frag:489`; `..._flush_dispatch.cpp:173` and the next block (note the
  "Stage 1 / Stage 3" numbering gap); `..._flush_prep.cpp:332`; `txmmgr_render_phases.cpp` ~:517 (`{ {`). Remove and de-indent.
- `s_propBaseLightDirStrength` is an `extern const` defined in a .cpp for a literal (`..._state.cpp:217`); make it `inline constexpr` beside `kNightEmissiveScale`.

**F7 (LOW): dead functions and constants.** `GpuStaticPropBatcher::submitCachedInstanceRange` (`.h:367`, `..._seam.cpp:133`) and `persistentStaticTotalCount` (`.h:373`, `..._seam.cpp:164`) have no
callers; `buildV6Packets` (`..._dispatch.cpp:105`) now only resets counters (rename or fold into `initV6State`); `s_spBuildRetired` and `s_spBuildAttempted` are always 1 yet still flow into the
snapshot stats and the `[RENDER_SNAPSHOT]` line; unused `#include <intrin.h>` at `gos_static_prop_batcher_internal.h:92`, `gos_static_prop_registry_internal.h:24`, `mclib/txmmgr_internal.h:62`;
`} // end gate branch` in `gos_static_prop_registry.cpp` flush.

**F8 (proof completeness).**
- No commit message names its regenerated files (`Gates/gate_index.gen.h`, `docs/flags.auto.yaml`, `docs/gl-vk-feature-coverage.{json,md}`, `tools/gate_table.seed.json`, the allowlist, the census);
  they point to lane2-state.md, and six messages say "Tip proof ... is recorded with the last commit of the group" while the last of them (0d479ca9b) repeats the sentence instead of recording
  it. The generated files are consistent at HEAD (tool re-runs above), so this is traceability only.
- Documented deviation with a real bisect cost: 9bec41219..87d1a26ac were not built individually. `scripts/gl_shader_variants.json` is stale until c77939fc1; the log census is red from 415f2997c until
  2f8639820; `test_gate.cpp` `n > 1000` fails at 19d1cee73 until 434abb5a0. HEAD is proven; intermediate commits are not bisect-safe.
- Ratchets: not loosened (cxx17 baseline changes only its `head` line; the census baseline is 179 deletions, 0 additions; LG19 = 3 unchanged, LG20 baseline 136 vs actual 130). Not tightened
  either: `cxx17_ratchet --check` reports 31 sites of slack. Run `--check --tighten`.
- F-POSTOPT: only the all-on case was captured (99.9% change); no capture shows defaults (CAS off, mode 0) are pixel-identical to the pre-slice build. Bounds are at the consumer only
  (`gos_SetPostFxOptions` clamps; a NaN float passes the clamp); `CPrefs::load` stores raw values and `save()` writes them back. Clamp in `load`.
- Perf: POM on every chunk, cliff POM (24 steps x 3 planes) and the full-map static shadow rebake on every registered-building destroy are now unconditional. The REBAKE row existed as a kill-switch
  for destruction-heavy missions; no Tracy delta or destruction-heavy timing is cited. tier1 fps (149/85/105/132/128 vs 145/85/87/116/118 in wave 1) is a coarse proxy.
- Ledger: the F-SPREG row cites no capture (its A/B is the f25 set in the F-SPDISPATCH row; g1tip vs f25tip spans both); the F-POSTOPT row has no default-equality evidence.
- Table floor (`tests/unit/test_gate.cpp:312`, `n > 900`) is not meaningful: `REQUIRE(n == kGateCount)` already ties the table to the generated header and `gate_gen --check` ties both to the JSON `count`;
  900 tolerates a 10% silent loss and needs editing every wave. Delete the line, or assert against the JSON count.
- The seven remaining `MC2_TERRAIN_DECAL_{SCALE,OFFSET,LATERAL,LIFT,YAW,PITCH,TRACE}` rows now drive an always-on feature from env; freeze them next (0d479ca9b keeps them on purpose).

## Confirmed correct (verified in code or by tool, not from commit messages)
- 55 rows deleted, table 1055 -> 1000, JSON `count` 1000, none added; no deleted name has a live reader in GameOS, mclib, code, editor, shaders, tools, tests (docs/history excepted, see F5).
- Frozen constants equal the old rows: POM scale `gos_GetTerrainPOMScale()` 0.02, steps 16/8, fade 1500..3500; cliff 1.0 / 2.0 / 12 wu / 24 steps; night-tag emission 1.0 (old `TAGS=1` with empty SCALE
  resolved to 1.0), fixture 1.0, base-light dir 0.45, IBL night floor 0.25, SH 0.5, PBR 0.5, impostor 800, decal fill 0.2 / blend 0.35, glass dither 1.0, resample factor 1, CAS off / 0.5, tonemap 0.
- F-SPREG / F-SPDISPATCH: `useCachedBlob` keeps its old default meaning (PERSISTENT_BUCKETS was default ON); flush loop order, the snapshot-sole-owner decision (old default `retireLiveBuilder = true`),
  the structural live-build fallback (still reachable when the snapshot is unusable) and the draw loop are unchanged, so draw order is unchanged; the colors-fill skip and `firstColorOffset = 0` match the
  old default. The 179 removed census entries all belong to deleted emitters; no tag was added.
- RendererFeature 50 -> 42 is consistent: enum, `kFeatureTable` and its `static_assert` are generated together; `GuiRuntime/EditorInspector_sections.cpp:32` loops to `COUNT`;
  `tests/unit/test_rendercore.cpp` indexes by enumerator name (neither needed an edit). `RenderPassContract.h` carries `nullptr` for the deleted StaticPropOpaque gate.
- VK/GL lockstep: no deleted uniform has a VK UBO field, so no half-removed field exists; `glassDither` is kept on both sides; variant matrix 231/231.
- options.cfg: `PostCAS` (bool, FALSE), `PostCASSharpness` (float, 0.5), `TonemapMode` (long, out of 0..3 becomes 0) default to the old gate values; `load/save/applyPrefs` are symmetric;
  `gos_SetPostFxOptions` is safe before the post-processor exists and also live-updates it (`applyPrefs` runs at `mechcmd2.cpp:961`, `loadscreen.cpp:506`, `optionsarea.cpp:302`), a superset of "read once".
- Per lane2-state (not independently verifiable read-only): tip builds exit 0, tier1 5/5 with fingerprint OK at e796f71af, the last code commit; later commits are docs/state only
  (`git diff --stat 434abb5a0 583a0c263`: two .claude files).

## What the next lane 2 session must do first
1. F1: write the VK-parity rows and correct the ledger row and the VK shader comment (docs plus one comment, no build). It decides whether the GL deletion arc inherits a silent gap.
2. One cleanup commit for F2-F4, F6, F7 (all downstream of the same promotions): `build_all_targets.py`, harnesses + doctest, and a GL day/night A/B that must be 0 px apart from mech noise.
3. F5 comment/doc sweep in that same commit, beside the code it describes (no comment-only commit), including `docs/tier1_env_vars.md` and `tag-registry.json`.
4. `cxx17_ratchet --check --tighten`; drop the table-floor line; add a default-equality capture for F-POSTOPT; one Tracy A/B on a destruction-heavy mission against the merge-tip exe.
5. Then the open W2/W3 list in lane2-state.md (settings wave, MATERIAL_TEXTURES, KTX+BC7 VK path, TREE_DIAG_TRACE, VIEW_UNIFORMS, VK_VFX_TRACE, DECAL_* knobs).
Note: `tools/shader_reflect/expected` goldens for static_prop.* already DRIFT on unrelated SSBO changes at the merge tip, so they carry no signal for this wave.
