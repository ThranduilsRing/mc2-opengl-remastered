<!-- doc-status: current | reviewed:2026-10-15 -->
# Final release checklist (the one release, 2026-10-15)

WHAT: everything that must be true before the single final release (owner endpoint 2026-10-11: full C++23 migration, full Vulkan, editor
rework, GL deleted after VK parity, multiplayer compiled out, then the project ends). One row per condition: owner, state at the tree of
`claude/lane7-release` (base nifty 06f3e7296, 2026-10-15), evidence.
WHY: the plans name the arcs; nothing names the finish line as a list a reviewer can tick. A row is DONE only with a sha or a run on this tree;
"open" includes "believed done, not re-proven here".

Owners: **L1** lane 1 (sim, C++23 steps 4-5, saves, determinism) · **L2** lane 2 (Vulkan) · **L6** lane 6 (backend-neutral producers) ·
**Sol** Codex track (editor rework, clang-cl, board red rows, gate amnesty) · **Own** the owner (rulings, shipping decisions) · **L7** this lane.

## 1. Build

| # | condition | owner | state | evidence / gap |
|---|---|---|---|---|
| B1 | One shipping config: RelWithDebInfo, MSVC, `/std:c++23preview`, Ninja tree, PCH as configured; `tools/build_all_targets.py` rc 0 at the release sha | L1 | open (green at each merge, never at a release sha) | handoff log 2026-10-15 verify at 834a57fe4 |
| B2 | `MC2_MULTIPLAYER=OFF` (compiled out) | L1 | DONE | `CMakeLists.txt:153` default OFF |
| B3 | `MC2_VULKAN=ON`, `MC2_VULKAN_ISLAND=ON` | L2 | DONE | `CMakeLists.txt:159,168` |
| B4 | Debug/trace options OFF in the shipped build: `MC2_ASAN`, `MC2_RNG_TRACE`, `MC2_SIM_TRACE(_ABL)`, Tracy | L1 | DONE as defaults | `CMakeLists.txt:123-148`; re-check the release configure cache |
| B5 | Editor in the payload: `MC2_EDITOR=ON` builds the reworked SDL+ImGui editor (MFC gone) | Sol | open | `editor-rework-design.md` E1; MFC editor still the shipping one |
| B6 | Second compiler (clang-cl): `clang-cl-dev` preset exists and H2 merged (492c2eb1a); a full clang-cl build plus doctest has not been shown green; ruling: clang-cl deferred to the Linux arc | Sol / Own | open, not a release gate unless the owner says so | `clangcl-first-build.md` (configure hung in the sandbox; MSVC green outside) |
| B7 | Warnings policy: today only `/we4840` on mc2, mclib, gui, EditRel, gameos, gameos_editor, gameos_main; no `/WX` on first-party targets; tests build `/W4 /WX-`. Release bar to rule: either a per-target `/WX` with a warning baseline or the current policy written down | Own, then L1 | open (ruling) | `CMakeLists.txt:860-869`, `tests/unit/CMakeLists.txt:221` |
| B8 | Floating point: no `/fp:` flag anywhere = MSVC `/fp:precise`; keep it (step 5 bit identity depends on it) | L1 | DONE (as default) | `git grep "fp:"` empty in CMake files |
| B9 | Ratchets all at or below baseline; KPI recorded at the release sha | L1 | open | KPI 15,819 at 2026-10-15 afternoon |
| B10 | `scripts/check-contracts.sh` all green at the release sha | Sol (red rows) | open | three unowned red rows assigned to Sol 2026-10-15 |

## 2. Payload (`scripts/deploy_payload.py --target game --ninja`, then `scripts/build_release.sh`)

| # | condition | owner | state | evidence / gap |
|---|---|---|---|---|
| P1 | Payload = exe + pdb + 5 ffmpeg DLLs + the whole `shaders/` tree + `shaders/vulkan/*.spv` from the build + run bats + `tools/terrain_gen/**` (editor target) + loose data rows (`data/tgl` GLB/ini, PBR ktx2, terrain layers, `terrain_materials.json`, `visual_tuning.json`, generated defs) + example mods; post-copy byte diff and `.deployed_manifest.csv` | L1 | DONE (mechanism) | `deploy_payload.py:1-30, 98-174, 257, 880-911` |
| P2 | After GL deletion the payload drops the GL shaders; `shaders/vulkan/*.spv` only (plus compute) | L2 / L6 | open | inventory section 12 |
| P3 | Engine zip carries `mc2-launcher.exe` and `mods/` | L1 | DONE in script, unproven by a zip install run | `build_release.sh:265-270`; `RELEASE-PROCESS.md` step 5 caution is stale |
| P4 | `scripts/test_release_zip_install.py` PASS on the release zips | Own (runs it) | open | `docs/release-checklist.md` |
| P5 | `delivery_installs` green AND graded on the release install alone | L2 (fix below) | partly DONE | see below |

**`delivery_installs` red row, cause.** The row was not caused by the shader. Every dev install carries `calcLightPools` in
`shaders/gos_tex_vertex_lighted.frag` (lane2, lane3, v0.4, v0.4c: 2 hits each, checked 2026-10-15). The row failed because the
`VEHICLE-LIGHT-GATHER-ALL-LEAVES` code evidence named `mclib/gvactor.cpp`, and split 6532c1ca9 had moved the call to `mclib/gvactor_render.cpp`: the source@sha
tier said NO for every install, so 0 installs delivered and the per-install text showed only the last reason, the stale frags of the four mod installs
(MC2-Exodus, MC2-MagicExpansion, MC2X-CVE-G, MC2X-REAP3, which are foreign builds). Fixed by 052945e26 (Sol, 2026-10-14: alternate path, test
`scripts/tests/test_delivery_code_evidence.py`); `py -3 scripts/check-delivery.py --quiet` exits 0 on this tree (3/10 installs deliver each fix).
**Proposed fix for lane 2 (remaining):** the pass bar is "any one install shows it", so the row can go green off a dev lane while the release install does not.
For the release, add `--install <release lane>` to the `delivery_installs` row (grade only the nominated install, every fix YES) and exclude the
mod roots from that row; the golden install v0.4c already fails 5 of 9 fixes (code not in its exe d662802367d8), so it must be re-deployed or retired before it is nominated.

## 3. Data and defaults

| # | condition | owner | state | evidence / gap |
|---|---|---|---|---|
| D1 | `MC2_RENDER_BACKEND` default = vulkan (today `""` = gl) and the umbrella `MC2_VK_SHARED_GBUFFER` folded into the default (today Feature, false) | L2 / Own | open | `Gates/gate_table.cpp:762,1323`; `vk-default-flip-checklist.md` |
| D2 | The 17 CPrefs render options ship at their ruled defaults: PostCAS off, PostCASSharpness 0.5, TonemapMode 0, ShadowCSM on, ShadowBoundedNearFit on, PropShadowReceive 0, ShorelineLegacySuppress on, CpuPhongRetire on, StaticPropAmbient on, StaticPropIblSh on, StaticPropIblSunRotate off, floor 0.45, StaticPropPbr off (owner 2026-10-13), SunAxisFix off, SunElevationFix off, VfxSoftParticles off, VfxLitParticles off | Own / L2 | DONE as table defaults | `Gates/render_options_cfg.h`; golden.json `capture_env.render_options` |
| D3 | Every option works on VK: rows 19 (CAS, tonemap) and 21 (SunAxisFix) FIXED, 22 (Pbr, Ambient) FIXED with residual, 20 (ShadowCSM, PropShadowReceive) PARTIAL | L2 | open (row 20) | `.claude/VK-PARITY-GAPS.md` |
| D4 | Shipped `options.cfg`: installs on this host carry `ResolutionX = 3840`; the release file must state the intended default size (desktop or 1920x1080) and the 17 options at D2 values | Own | open (ruling) | `mc2-win64-v0.4/options.cfg:16` |
| D5 | Feature gates: 100 Feature rows, 53 of them Bool default OFF (`docs/flags.auto.yaml`); each must be flipped ON, deleted, or reclassified as a debug/diagnostic switch the release never reads. Gate amnesty is PARKED (owner 2026-10-14) | Sol / Own | open | `gate-amnesty-wave3.md` (34 held) |
| D6 | `MC2_BRAIN_MISSIONFIT_OPORD` stays default OFF: the single named default-off shipping exception (owner 2026-10-15 early) | Own | DONE (ruling) | `Gates/gate_table.cpp:111` |
| D7 | Brain family (`MC2_BRAIN_DISPATCH`, `_RUNTIME`, `_RUNTIME_APPLY`, default OFF Feature rows): either default ON or ruled into the D6 exception; today two rulings conflict (single exception vs HOLD-L1) | Own | open (ruling) | `gate-amnesty-wave3.md` brain hold |
| D8 | Data the stock install must not carry: non-stock loose `data/art/Buildings.csv` removed (owner 2026-10-13) | Own | DONE on lane2 install; check the release install | `editor-roundtrip-ledger.md` owner finding |
| D9 | tier2 `asset_load_fail` on mc2_11/12/13/16/22/24: the install's `data/tgl` jaegermech override has no shape | L1 | open on v0.4; worked around on lane3 | `tier2-data-gap.md` |

## 4. Proofs (on the release exe, release install)

| # | condition | owner | state | evidence / gap |
|---|---|---|---|---|
| R1 | tier1 5/5 on the VK default (no backend env), 1080p pinned | L2 | open (VK tier1 runs under the umbrella env today) | `tests/smoke/smoke_missions.txt` 5 tier1 rows |
| R2 | tier2 24/24 missions | L1 | open (D9) | 24 tier2 rows |
| R3 | Editor round trip 24/24 byte-identical, on the reworked editor | Sol | DONE on the MFC editor (`editor-roundtrip-ledger.md`: 24 byte-identical); open on the rework | baseline `tests/smoke/editor/roundtrip-baseline.json` |
| R4 | Determinism: `sim_determinism.py check` on mc2_03, 10, 16, 24 against the committed baselines; mc2_16 save bytes differing under host load investigated first | L1 | open | baselines 153b23b2d; handoff 2026-10-15 afternoon |
| R5 | Save/load: `saveload_roundtrip.py` on the 5 baselines (mc2_01/03/10/16/24) plus `campaign_save_roundtrip.py` once; the `--first-save-only` weak mode retired when the S3 Archive writer replaces the legacy one | L1 | open (weak mode in force by ruling 2026-10-14) | `tests/saveload/baselines/` |
| R6 | VK validation: zero errors under `MC2_VULKAN_VALIDATION=1` on tier1 (`[VK_VALIDATION v1]`, bucket `vk_validation_error`) | L2 | believed DONE (f729ec7b9 cleared 09600 and 05137), re-prove at release | `docs/vk-validation.md` |
| R7 | Visual oracle: VK golden set current, `verify.py --golden current` all blessed regions PASS; day and night viewed | L2 / L7 | open | `tools/visual_verify/vk_golden_plan.md` |
| R8 | VFX parity oracle on VK (GPU particles to parity, then MLR retirement) | L2 / Sol | open | `vfx-parity-oracle.md`: acceptance protocol exists, no production logger emits it |
| R9 | ASan doctest once on the release exe; doctest and contract harnesses green | L1 | open at release sha | `tools/build_contract_harnesses.py --run --unit` |
| R10 | Step 5 bit-identity harness green for each step-5 batch, or `Stuff::` kept for sim by the exit rule | L1 | open | `step5-bit-identity-harness-design.md` |
| R11 | `BUGS-FOUND-FIXED.md` rows all closed or ruled shipped-known | Own | open | 39 table lines |

## 5. Docs for players

| # | condition | owner | state | evidence / gap |
|---|---|---|---|---|
| H1 | `RELEASE_README.md` rewritten for the final release: Vulkan requirement (Vulkan 1.2 + dynamic rendering, sync2, timeline semaphores, descriptor indexing, BDA, independent blend; drops Kepler and first-gen GCN), no OpenGL 4.3 line | L7 / Own | open (README says v0.5.2 and OpenGL 4.3) | `docs/vk-device-profile.md` |
| H2 | "What changed" notes per `RELEASE-PROCESS.md` section 2: rendering, editor, launcher, removed features (multiplayer compiled out) | L7 | open | |
| H3 | Saves: saves from any earlier build may not load in the final release; saves break freely until it ships; say so in the README and the launcher | Own | open | owner endpoint 2026-10-11 |
| H4 | Mod compatibility statement (readers clamp or reject malformed content with a log line, never crash) | L1 | open | owner ruling 5 |
| H5 | Known issues table regenerated from open parity rows and BUGS rows | L7 | open | |

## 6. Install layout

| # | condition | owner | state | evidence / gap |
|---|---|---|---|---|
| I1 | Flat folder: mc2.exe, mc2-launcher.exe, editor exe, DLLs, `shaders/`, `data/`, `mods/`, bats, `options.cfg`; the zips merge into one folder | Own | DONE (documented) | `RELEASE_README.md` install table; `docs/deploy-isolation.md` |
| I2 | Release lane is a fresh install directory, never the dev lane; `deploy_payload.py --verify-only --strict` clean | Own | open | |
| I3 | Zips-only install smoke (`test_release_zip_install.py`) and `release_install_manifest.csv` archived | Own | open | |

## 7. Versioning

| # | condition | owner | state | evidence / gap |
|---|---|---|---|---|
| V1 | Build from a clean committed tree: `[BUILD_FINGERPRINT v1]` sha, dirty=0 (`MC2FP1` literal); deploy refuses dirty under `--require-clean` | L1 | DONE (mechanism) | `cmake/GenerateBuildFingerprint.cmake` |
| V2 | Human version string: there is no semantic version in the exe; the release name lives in the lane folder, README title and tag. Rule the final name (e.g. v1.0) | Own | open (ruling) | `RELEASE-PROCESS.md` section 1 |
| V3 | Tag and git bundle on E: at the release sha; push only by the owner | Own | open | |

## 8. GL deletion gate (inventory section 13 order)

| # | step and its gate | owner | state |
|---|---|---|---|
| G1 | Capture and tooling GL arms: VK golden set current (R7), `capture_ab` on two VK arms, `vfx_parity_oracle` with a VK readback | L2 / L7 | open: plan written, `bless.py --backend` landed |
| G2 | Debug views: keep list ruled (inventory Q3), ported or deleted | Own / L2 | open |
| G3 | Post and sky: row 12 closed for the kept options | L2 | open |
| G4 | VFX: R8 | L2 | open |
| G5 | Shadows: rows 4 and 20 closed, VK static building map producer (row 17) | L2 / L6 | open |
| G6 | Mechs: row 2 closed; skinning / instance / shadow snapshot moved into a backend-neutral producer | L6 | open |
| G7 | Props, cull, light grid, vegetation: VK cull and light-grid build (also unblocks row 1) | L6 / L2 | open |
| G8 | Terrain and water: rows 1, 3, 5, 6, 7, 9, 14 closed | L2 | open |
| G9 | Textures: zero `glGetTexImage` / `glGetBufferSubData` callers (grep row) | L2 | open |
| G10 | Fail-soft without GL: what a refused VK device does (inventory Q1) | Own | open (ruling) |
| G11 | `asset_viewer`, `mc2fx_preview`, `mc2weapon_gui`: ported or retired (inventory Q2) | Own | open (ruling) |
| G12 | Vulkan best-practices acceptance review (queue item 14, second pass) before the delete | Own (Opus) | open |
| G13 | Editor viewport on VK (editor smoke stops needing the MFC GL viewport) | Sol | open |

## 9. Rulings this checklist needs (owner)
1. B7 warnings policy for the release (per-target `/WX` with baseline, or `/we4840` only).
2. D4 shipped `options.cfg` resolution.
3. D5/D7: what happens to the 53 default-off Feature rows with amnesty parked, and whether the brain family joins the OPORD exception.
4. V2 the final version name.
5. B6 whether clang-cl is a release gate (current ruling: deferred to the Linux arc, so no).
6. P5 which install is the nominated release install the delivery row grades.
