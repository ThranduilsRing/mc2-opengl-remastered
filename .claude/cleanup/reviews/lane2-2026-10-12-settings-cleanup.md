# Lane 2 settings-wave cleanup review, range 2e52868f2..3a6838e21 (lane 2 commits only), branch claude/lane2-s3

Reviewer: read-only Sonnet cleanup pass, 2026-10-12 night. Method: `git show` of the seven lane 2 commits (eb8063307 de85936cd e8437015b 8589e3148 675c7555a e8a4ac64e 1c3ef0de9),
the code at HEAD 3a6838e21, and read-only re-runs: `gate_gen.py --check` OK, `log_tag_census.py --check` OK (1377 emitters), `file_size_ratchet.py --check` OK,
`cxx17_ratchet.py --check` OK but "2 fewer legacy sites than baseline", `check-capture-resolution.py --selftest` PASS, pytest `test_resolution_tier.py` + `test_manifest.py` 10 passed,
registry kinds recomputed from `Gates/gate_table.json` (eb8063307: 1000 rows, Feature 114, Trace 107; 1c3ef0de9: 980, Feature 100, Trace 101), `/we4840` confirmed in the
`build64-ninja/build.ninja` FLAGS of gameos, gameos_editor and gameos_main objects. No build, no launch.

## Verdict: ACCEPT-WITH-FIX
The table, the override grammar, the pre-window publish, the launcher editor, the resolution pin and the decal freeze are correct and well tested; no deleted gate name has a live reader.
The fixes: one contract gap (F1: CPrefs does not reject malformed bool/long/float text), one reproducibility gap the settings move created (F2), a launcher default that now persists (F3).

## Findings, by severity

**F1 (MEDIUM, contract): `CPrefs::load` rejects only NaN/inf/out-of-range; garbage text is accepted, and the pre-window loader parses differently.**
`code/prefs.cpp:30-62` (`loadRenderOptions`) reads through `FitIniFile::readId*`, which are lenient: `readIdLong` -> `atol` (`mclib/inifile.cpp:315`), `readIdFloat` -> `atof` (`:249`),
`readIdBoolean` -> `booleanToLong` (`:626`: anything not starting with F or 0 is TRUE). So `l TonemapMode = abc` loads as 0, `f PostCASSharpness = abc` and `f StaticPropIblSunRotateFloor = 0,45`
load as 0.0, `b ShadowCSM = banana` loads TRUE, all silently; the "garbage" verdict of the `event=rejected` line can only come from nan/inf. Meanwhile `cfg::loadFromText`
(`Gates/render_options_cfg.h:121`, used by `gameosmain_init.cpp:268`) uses the strict `setFromText` and skips those lines. Two parsers over one file: for `b ShadowCSM = F` FitIni says FALSE but the
early reader (the one `static const` that runs at renderer start-up) keeps TRUE; `stale_reader ... changed_since_start=1` fires, the cached value stays wrong.
Fix: one parser. In `CPrefs::load` read the file text once and call a `loadFromText` overload that appends `Reject`s (key, text, why), log them with `logRejectedOption`, delete `loadRenderOptions`
(one switch less: LG04 -1, and the two `char text[32]` conversions go). Decide and test the trailing-comment policy (`l TonemapMode = 2 ; AgX` is accepted by FitIni, rejected by the strict parser).
Add doctest cases for `abc`, `banana`, `0,45`, empty value, duplicate key.

**F2 (MEDIUM, reproducibility): options.cfg is now an unpinned input of every capture, Tracy trace, tier1 and smoke run, and nothing records it.**
The resolution pin exists because the install's options.cfg silently changed what a capture meant. The settings wave moved 14 look/lighting switches plus the 3 post-FX keys into that same file:
a capture on an install whose options.cfg says `StaticPropPbr = TRUE` (the launcher now writes that, see F3) differs from one on a clean install, and the quiet `[OPTIONS v1]` log says
nothing (`prefs.cpp:526`, by design). `debug_state_dump.cpp:476-477` dumps only IblSh and Pbr. Fix (smallest first): dump all 17 effective values in `debug_state_dump.cpp` so
`get_visual_settings` and capture sidecars carry them; then pin like the resolution (a helper next to `render_resolution.py` that `run_visual_capture.py` and `tracy_capture_compare.py` merge into
`MC2_OPTIONS_OVERRIDE` from the table defaults; `--env-declare` still overrides).

**F3 (MEDIUM, launcher default): two cfg rows default to ON while the engine default is OFF, and Launch now makes that permanent.**
`tools/mc2_launcher/mc2_launcher.cpp:232-233` `StaticPropPbr` and `StaticPropAmbient` keep `defaultVal "1"` (the old LAUNCHER-PREFERS-ON drift; engine default false). 12 of the 14 rows match, these
two do not, so "GUI defaults match engine defaults" holds only after the ShadowBoundedNearFit fix. Before, a launcher launch set an env var for that one process; now `DoLaunch` -> `SaveCfgRows()`
(`:2227`) and `--sync-options` write `TRUE` into the install's options.cfg, so every later shell/smoke/capture launch on that install runs PBR + ambient ON (feeds F2). The comment block at
`:204-231` and the one at `:162` still describe the env mechanism and cite deleted gate names and reader lines. Fix: owner ruling needed (the old comment itself says a default flip is not a lane
call); until then initialise cfg rows from `RenderOptions{}` so the file is not rewritten into something the engine did not choose, and rewrite those comments. An old launcher_env.json keeps
`MC2_SHADOW_CSM`-style keys that are now ignored (a user's old "CSM off" resets once): say so in the handoff.

**F4 (LOW-MEDIUM, process): the pin lands before the re-bless, and two docs still describe 4K.**
`check-capture-resolution.py` now refuses every 1080p capture against `tests/visual/goldens/reset-50331d5` (3840x2160): the visual golden compare is effectively off until the owner re-blesses.
`docs/perf-baseline-2026-10-11.md:5` ("3840x2160 drawable") and `docs/tracy-harness.md` do not say new traces are 1080p and not comparable with `release_assets/tracy-baselines`. Fix: one line in each.

**F5 (LOW): resolution harness edges.** (a) `tests/smoke/test_resolution_tier.py:41` proves the drivers read the pin by grepping their source for a string; test the env they build instead.
(b) `scripts/run_visual_capture.py:271` pins MC2_WIDTH and MC2_HEIGHT independently: `--env-declare MC2_WIDTH=2560` alone gives 2560x1080. Pin the pair or neither.
(c) `scripts/run_smoke.py:1543,1566` still name crash-evidence and ring_trace artifacts by `e.stem`, so the two `resolution` rows overwrite each other; use `label`.

**F6 (LOW): override channel and editor.** `Gates/render_options.h:108` `find` is case-sensitive while options.cfg keys are case-insensitive (`ShadowCsm=0` is rejected); an unknown key is logged
`why=garbage` (`prefs.cpp:~526`), say `unknown_key`. The editor (`DISABLE_GAMEOS_MAIN`, `editor/CMakeLists.txt:71`) never runs `initRenderOptionsFromConfig`, so it ignores options.cfg and
`MC2_OPTIONS_OVERRIDE` (the env gates used to work there): defaults only. Acceptable; record it in the ledger row.

**F7 (LOW): comments, counts, baselines.** `gos_static_prop_batcher_state.cpp:198` names `CPrefs::applyPrefs()` as the publisher; the one that matters is `gameosmain_init.cpp:268` (pre-window).
`CMakeLists.txt:859-861` says "every first-party target" for a seven-target list (RenderCore, GuiRuntime, GameAdapters, mc2_launcher are not in it). Commit messages 8589e3148 ("Feature 118 -> 105")
and 1c3ef0de9 ("Feature 105 stays") are wrong, measured 114 -> 100 (the lane already noted it in lane2-state; local history, no rewrite). `cxx17_ratchet --check` shows 2 sites of slack at HEAD:
run `--check --tighten`. `scripts/check-ab-kit.py:206` and `tools/class_light_band_harness/*.cpp` still name deleted gates (known to the lane).

**F8 (LOW, simplification).** Four `switch (kind)` copies (`typeLetter`, `setFromText`, `loadRenderOptions`, `saveRenderOptions`; the last two are why LG04 rose by 2): F1 removes one.
`prefs.cpp:511-531` has a function-local `static bool s_reported` and three `if (!s_reported)` guards; one block under a single `if` reads better. `render_options_cfg.h` spells
`mc2::options::detail::` eight times: add a namespace alias. Launcher `WriteTextFile` truncates then writes: write a temp file and `MoveFileEx` it. `test_gate.cpp` "read before the publish"
asserts under `if (!mo::g_published)`, so it depends on test order.

## Confirmed correct (checked in code, not messages)
- Table (`render_options.h`): 17 keys; defaults equal the deleted gate rows; bounds equal the old clamps; `setFloat` rejects NaN/inf; `from_chars` needs the whole token; a rejected value leaves the field untouched.
- Override grammar: commas and spaces trimmed, empty items skipped, `Key` without `=` rejected, `A=B=C` garbage, unknown key reported, never saved (`save()` writes `this->render`; the effective copy is local); tested.
- Launcher editor: `cfg::set` keeps the file's own EOL, replaces in place, adds inside `[MechCommander2]`, leaves a block-less file alone, prefix keys (`PostCAS`/`PostCASSharpness`) not confused (tested); engine `save()` writes CRLF via FitIni.
- Publish-before-window: every converted reader (`options::current()` in 14 files) is a function-local first-use read, none at namespace scope; `initRenderOptionsFromConfig()` runs right after
  `GetGameOSEnvironment` (`gameosmain.cpp:1065`), before any renderer init, so the one early reader (`gos_postprocess_gates.cpp:22`) now sees the file; the tier1 `2026-10-12T23-15-14` and resolution
  `23-26-10` logs hold no `[OPTIONS v1]` line (early_reads 0, changed 0). Post-FX goes through its own `gos_SetPostFxOptions` push, not a static cache.
- Launcher: exactly 14 `cfg` rows (ShadowCSM ShadowBoundedNearFit PropShadowReceive StaticPropPbr StaticPropAmbient StaticPropIblSh VfxSoftParticles VfxLitParticles SunAxisFix SunElevationFix
  StaticPropIblSunRotate StaticPropIblSunRotateFloor ShorelineLegacySuppress CpuPhongRetire); `DoLaunch` skips them for the environment; a value row is validated before it is written; a missing file or block is never invented.
- Matrices and `run_vk_field_check.py` moved to `MC2_OPTIONS_OVERRIDE`, no duplicate JSON keys; no remaining env reader of any deleted name outside comments, docs and history.
- Resolution: one constant (`render_resolution.py`), read by `run_visual_capture.py` and `tracy_capture_compare.py`; tier1 deliberately unpinned; manifest change is one tier word + one key (bad size dropped with a
  warning, tested); the tier fails unless the log has the exact line `gameosmain_init.cpp:397` prints; `select_missions` dedupes by stem, so `--mission mc2_01` still takes the tier1 row first.
- ctxlog: `vulkan_context.cpp:109` is `env.c_str()`; `/we4840` is really on gameos, gameos_editor, gameos_main.
- Decals: `kDefaultScale 1, Offset 8, Lateral 0, Lift 0, YawDeg 0, PitchDeg 0` equal the deleted rows' defaults; the face-frame matrix is CPU-built and shared by GL and VK; the lane commits touch no shader or
  `gpu_abi` file, so UBO lockstep is untouched.
- Baselines: cxx17 and file-size rewrites carry reasons in 1c3ef0de9 (mapdata.cpp 2631 -> 2632 for the include, GuiRuntime 1196 -> 1200), code not contorted; the removed VFX guardrail in `test_rendercore.cpp`
  is covered by the defaults test (`vfxSoftParticles`/`vfxLitParticles` false).

## Out-of-area edits (item 5)
Minimal and correct: `code/prefs.{cpp,h}`, one-line readers at `mclib/mapdata.cpp:2451`, `camera_update.cpp:203`, `txmmgr_render_phases.cpp:139` (+ two comments in `txmmgr_render_passes.cpp`), the T4 decal files in `mclib/`,
`code/missiongui_uicmd_probe.cpp` (comment). None is in lane 1's B1 file list (`cxx23-step4-b1-files.json`; `code/mpprefs.*` is another file). Lane 1 tip 9e8c63cf3 changed 41 files since the merge base c5156854d (logistics*, mech*, objective*, mc2str.h):
only two overlap lane 2's changes, listed below.

## What the next lane 2 session must do first
1. Merge nifty, regenerate (do not hand-merge) the generated files below, then `cxx17_ratchet --check --tighten`.
2. F1: one parser for CPrefs and the pre-window pass, with the tests. 3. F2: dump all 17 effective options in the state dump, pin them in the capture and Tracy drivers. 4. F3: initialise cfg rows from engine defaults, fix the comments.
5. Owner items first in the handoff: re-bless the golden set at 1920x1080 (compare refuses until then); LAUNCHER-PREFERS-ON for Pbr/Ambient; whether tier1 is pinned.

## Likely merge conflicts with lane 1
tests/callers.json, tools/cxx17_ratchet/baseline.json (changed on both sides), tests/unit/log_tag_baseline.json, docs/background-launch-inventory.md (lane 1 changed scripts/background-launch-allow.json), CMakeLists.txt (/we4840 list, lines 857-870, if lane 1 extends it), Gates/gate_table.json + gate_index.gen.h + RenderCore/RendererFeatureRegistry.h + docs/flags.auto.yaml + tools/gate_table.seed.json (generated). No lane 2 source file is in lane 1's B1 batches.
