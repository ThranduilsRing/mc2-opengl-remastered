# Lane 2 morning-session cleanup review, range 9063b358a..38a248bf5 (lane 2 commits only), branch claude/lane2-s3

Reviewer: read-only Sonnet cleanup pass, 2026-10-13. Method: `git show` of dd2bf2345 c5b7e0843 c0158be25 e84671b99 159073e59 98fc6705b 38a248bf5, the code at HEAD, every sidecar and
`capture_run.json` under `.claude/l2cap/` (scripted), the lane install's tier1 `manifest.json`. Read-only re-runs: pytest `test_capture_backend` + `test_check_ab_kit` + `test_capture_ab` +
`test_launcher_legacy_env` 23 passed; `cxx17_ratchet --check`, `callers.py --check`, `check-background-launch.py`, `log_tag_census --check`, `gate_gen --check`, `file_size_ratchet --check`,
`check-capture-env-declared.py` all OK. No build, no launch (the C++ "20/20, 2789/2789" claims were not re-run).

## Verdict: ACCEPT-WITH-FIX
The honesty guard has no false-pass path I could construct, the umbrella fix is right, the launcher migration is correct for the 14 keys. The fixes: the guard sits only at capture time, so 21 old
GL-labelled-VK sets (and the verdicts quoting them) still read as VK (F1, F2); dead keys are dropped unlogged (F3); smaller manifest, comment and test gaps (F4-F10).

## Findings, by severity

**F1 (MEDIUM): `capture_ab compare` diffs a "-vk" set without checking it was Vulkan.** `tools/visual_verify/capture_ab.py:67-107` (`check_options`, `compare`) reads only `render_options`. The new `backend`
lives in `capture_run.json` (`capture.py:202`) and each sidecar, but nothing reads it at compare time and sets taken before c5b7e0843 have no field. A "VK before->after" line over two GL sets prints as normal.
Fix: `check_backends(root, before, after)` beside `check_options`: for `<tag>-vk` require `capture_run.json` `backend.requested == drew == "vulkan"`, else (older set) require the pose sidecar
`<set>/<bm>/<bm>.json` `readback_source == "vk_present"`; otherwise print `VK arm UNPROVEN (GL pixels)` and return 1 like the options refusal. `capture_ab.py:8` still says "VK via MC2_RENDER_BACKEND=vulkan" (no umbrella): name both variables.

**F2 (MEDIUM, evidence): earlier "VK" verdicts built on the 21 GL sets (list below) are void.** In `lane2-state.md`: s4 `:79` ("VK day 297 px, night 0"), f25 `:94` ("VK night 0 px"; its umbrella numbers are real), w1 `:113` ("VK day 227 /
night 474 px"; the `w1-*-vkarm` numbers are real), f21 `:118` (its "VK" arm), and any VK strip cited from the p1a-p8a sets of the 10-10 sessions. `:130` says "umbrella OFF"; it should say GL. Fix: one correction line on each bullet
(or in HANDOFF-LOG) so no later lane cites them. Related, not GL/VK: the `:94` umbrella strip compares `g1tip-vkarm` (no `MC2_SMOKE_FREEZE_ANIM`) with `f25tip-vkarm` (frozen), so its "one mech 1,646 px / 449 px" is the animation
phase of the unfrozen BEFORE set, not a regression signal; the `p7*`, `w2*`, `x2*`, `x3*` vkarm sets are also unfrozen (consistent inside a pair).

**F3 (MEDIUM-LOW, the stated gap): keys of deleted non-option gates are dropped silently.** `mc2_launcher.cpp:915-946` `MigrateLegacyEnvKeys` returns at `:940` when no legacy render key exists; the later `SaveEnvJson` (`:457` ->
`SaveGfxJson`, `fopen "w"`) writes only the rows, gfx keys and main-page keys, so any other `MC2_*` key vanishes with no record. Minimal fix (about 12 lines): before the rewrite scan the JSON text for `"MC2_[A-Z0-9_]+"\s*:` keys that are not
a `k_envVars[]` key, not `MC2_STATICPROP_TEX_TIER|MC2_SHADOW_MAP_SIZE|MC2_FPS_CAP|MC2_VSYNC`, not in `kLegacyKeys`; log `dropped_unknown=N names=a,b,c` (cap 8) in the `[LAUNCHER_ENV v1]` line and let `found + dropped_unknown > 0`
trigger the one-time rewrite, so each file is cleaned once with a record. Add a pytest case (`MC2_TERRAIN_POM` in; gone and named in the log). The lane install's own file has none today (checked): its non-legacy MC2_ keys are live rows or gfx keys.

**F4 (LOW): the `run_visual_capture` manifest does not say why it FAILed.** `scripts/run_visual_capture.py:477-513` `write_manifest` maps `backend.disagree` to `verdict FAIL` but stores neither `requested`, `drew` nor the messages (the sweep's `capture_run.json`
does, per pose). `capture.py:202-203` top-level `drew` is the first pose's value, so a mixed sweep reads "vulkan". Fix: put `result["backend"]` in the report `extra`; make `drew` the sorted set of per-pose values; add the backend to
`materialize_candidate`'s `set.json` so a blessed set says what drew it.

**F5 (LOW, comment): the harness tree model still says the ambient fill is off.** `class_light_band_harness.cpp:398-403` ("OFF by default ... exact no-op", ":1008-1013 killswitch idiom", ":325-333") contradicts the new header
(StaticPropAmbient is ON by default in `render_options.h`) and the claim that the tree direction term is exact: with the option on the tree band is an "ambient off" band too. Fix: reword to "ambient term omitted (option ON by default)", drop the stale
line anchors. The pin test (`:2688-2693`) covers the prop model only; say so in its comment.

**F6 (LOW): headless `--sync-options` rewrites the graphics keys from zeroed globals.** `mc2_launcher.cpp:3111-3116` calls `EnvVarsSetDefaults` but not `GfxSetDefaults`; `Migrate` then runs `LoadGfxJson` (`:942`) and `SaveEnvJson`. A file lacking
`MC2_STATICPROP_TEX_TIER` / `MC2_SHADOW_MAP_SIZE` gets tier index 0 written (GUI defaults are 2). Real files carry the keys, so this bites only a minimal or hand-made file. Fix: `GfxSetDefaults();` before `LoadEnvJson()` in that branch, plus an assertion in test 1.

**F7 (LOW, process): a ratchet baseline was loosened to absorb a foreign violation.** 159073e59 rewrote `docs/background-launch-child-baseline.json` `{}` -> `{"tests/analyze/estring_callsites.py": 1}`: lane 1's
`tests/analyze/estring_callsites.py:115` raw `subprocess.run(["git","diff",...])` without `console_child.child_kwargs()` (the conhost-storm rule). The message states the reason, but the move is to ask lane 1 for the one-line fix and keep the baseline empty. Orchestrator: route to lane 1, then re-tighten.

**F8 (LOW): the migration fails open.** `SaveGfxJson` (~`:404`) ignores a failed `fopen`; on a read-only install the legacy keys stay and a `migrated_legacy_keys` line is appended on every launch claiming success. Fix: return bool from `SaveGfxJson`
(or re-read) and log `event=migrate_failed`. The JSON rewrite is also truncate-then-write; `WriteTextFile` (atomic) exists and options.cfg already uses it.

**F9 (LOW, tests).** (a) The branch "requested gl but readback_source=vk_present" (`capture_backend.py:51`) has no test. (b) The exit-1 contract is tested through `backend_failures` only; no test drives `capture.main` or `run_visual_capture.main` (single and multi-run `return 1`).
(c) "New name wins" probes `launcher_env.json` only (`:926`); a key already in the install's options.cfg loses to the stale legacy value (rare, since the settings-wave launcher kept new names in the JSON): say so in the function comment or also probe `cfg::get`.
(d) The launcher tests skip when `build64-ninja/out/mc2_launcher/mc2-launcher.exe` is absent and never check it is current for HEAD.

**F10 (LOW, hygiene).** dd2bf2345 says "tightened" but the diff is the `head` stamp only (the ratchet was already at 0 fewer sites). The F3 VK evidence cited by VK-PARITY-GAPS row 22 (`.claude/l2cap/f3*-vk-*`, montage) is untracked scratch: say so in the row.

## GL-not-VK sets (`MC2_RENDER_BACKEND=vulkan`, no `MC2_VK_SHARED_GBUFFER`, every sidecar `readback_source: gl_fbo`) in `.claude/l2cap/`
f21-before-vk, f21-after-vk, f25tip-vk, f3on-vk, f3off-vk (the 00:02 sets; the 07:12+ `f3*-vk-{day,night}` are real VK), g1tip-vk, p1a-vk, p1b-vk, p2a-vk, p3a-vk, p4a-vk, p5a-vk, p6a-vk, p7a-vk, p8a-vk, s4base-vk, s4after-vk,
w1-before-vk, w1-after-vk, w1-head-vk, w2base-vk: 21 sets. `probe-vk-noumbrella` is the deliberate repro (`drew: gl`). Their `-vkarm` companions, `fix-*-vk`, `w1before-vk` and `x2*-vkarm` carry the umbrella and `vk_present`: real VK.
`capture_ab capture` before c0158be25 always produced the GL kind.

## Confirmed correct
- Engine: one `write_sidecar` serves both branches of `capture_to` (`gos_visual_capture.cpp:322-325`); `renderBackend()` returns GL once `forceGLFallback` ran (`render_backend_select.cpp`), so the forced-GL "present image not VK-owned" case writes `backend: gl`; a mid-run decline leaves the VK PNG undelivered (all_present fails).
- `capture_backend.requested` matches the engine spellings (vulkan/VULKAN/vk, else GL). `check`: vulkan+vulkan+gl_fbo fails; gl+gl passes; a missing field or sidecar fails; older sidecars read through `readback_source`. 11 tests = 9 functions + 2 parametrized.
- Exit codes: `run_visual_capture.py` returns 1 in single and multi-run mode (before `materialize_candidate`); `capture.py` returns 1 after writing `capture_run.json`; `capture_ab` ORs the arm return codes. `capture_cmd` pins the umbrella with the backend.
- `check-ab-kit.py` reads `Gates/gate_table.json` (980 rows); AB-7 bundle trimmed to the two live gates; both tests meaningful. `class_light_band_harness` is registered in `tools/run_contract_tests.py:60` (so deleting the stale "NOT REGISTERED" paragraph was right),
  reads `Gates/render_options.h`, is C++20 in CMake and target features, and its grammar test uses the real `setFromText`.
- Launcher: the 14 map entries equal the 14 non-post options of `render_options.h` in name and type (PropShadowReceive is the long 0..1 option, the floor is float 0..2); `JsonGetString` is quote-delimited so `..._ROTATE` does not match `..._ROTATE_FLOOR`;
  found = applied + kept + unmapped; a second run finds nothing and rewrites nothing; options.cfg still goes through `WriteTextFile`; exactly one log line; the allow row and inline comment for the new raw spawn are present.
- Proof: tier1 `2026-10-13T07-46-38` manifest: binary commit `98fc6705ba79`, dirty false, built 07:46:04, `source_head_matches_binary` true. The F3 VK captures' exe is `c5b7e0843952`, clean, built 07:12:07 (after the 07:11:35 commit), so the early pre-commit deploy was not the evidence.
  Regenerated files are named in each message; `Regenerated: none` for c0158be25 is true.

## What the next lane 2 session must do first
1. F1 + F2: the backend check in `capture_ab compare`; the GL-not-VK correction on the four `lane2-state.md` bullets; re-take an umbrella VK pair for s4/f25/w1 only if a VK claim from them is still needed.
2. F3 with F6 and F8 in the same launcher edit (one build, doctest, the new pytest cases).
3. F4, F5, F9, F10 (no build for F4/F9). Ask the orchestrator to route F7 to lane 1.
4. The owner items in the state file stay open (re-bless the 1080p golden set, rule the StaticPropPbr/Ambient default).
