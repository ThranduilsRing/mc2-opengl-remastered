# G1a status (typed gate registry infrastructure, zero migrated sites)

Audited at HEAD d96d394ab (G1a commits c08ee6755, e113b7f45, d00f9d26d, then the lane-2 merge 8e7b4724c / c093dab70).
Design: `typed-gate-registry-design.md` v2 (sections 5-7), amendments in `typed-gate-registry-review.md` (d).
A launch hold (VPN incident) was in force during this pass: no game, editor, smoke or capture was started.

## Checklist

| # | Item | Verdict | Evidence |
|---|---|---|---|
| a1 | `Gates/gate.h` hot header: NTTP `get<"MC2_X">()`, `Tri` enum class (no bool), per-site `Rule` override, atomic slot | DONE | `gate.h`: one relaxed load of `g_slot[idx]`, resolve out of line, no function-local static; `Tri` is an `enum class`; `get<Name, Rule>` static_asserts Bool-only; `test_gate` case "Tri is an enum class" |
| a2 | `gate_cold.h` / `gate.cpp` (str, lookupName, invalidate/invalidateAll, markMainEntered, parseBool, dump) | DONE | `invalidateAll` snapshots pre-main slots, premain warning, `MC2_CONFIG_STRICT` ends the process with exit code 78 |
| a3 | `gate_table.cpp` constexpr table + static_asserts vs the generated index | DONE | 1139 rows; `gate_index.gen.h` from `tools/gate_gen.py --write`; an index row missing fails the normal build |
| a4 | Compiler-sourced truth: `gate_dump` emits `Gates/gate_table.json`, `gate_gen --check [--rebuild]` diffs | DONE | `gate_gen --check --rebuild` builds `tools/gate_dump`, reports "gate table is consistent"; contract row `gate_gen_check` |
| a5 | `gate_dump` / `gate_gen` spawn children without a console window | WAS BROKEN, fixed | `gate_table_gen.py` used a bare `CREATE_NO_WINDOW` variable; rows `background_launch` and `scripts_tests` were RED at HEAD. Fixed in dd9213cce (`nofocus.hidden_kwargs()`, inventory regenerated) |
| b1 | ONE `parseBool` / from_chars / Tri rule; `HostServices::EnvConfig::flag` delegates | DONE | `DefaultHostServices.cpp` calls `gate::parseBool`; `parseRow` uses whole-string `from_chars` for Int/Float and `strtol` only for `Rule::Prefix` rows |
| b2 | `tests/unit/test_gate.cpp` matrix | DONE | 26 TEST_CASEs: all Bool rules x the 8 probes, Std spellings, badvalue, per-site override, Int range Reject/Clamp, Prefix, Float, Enum, Tri, String, caching/invalidate, dump, premain, unregistered. Doctest is now 664/664 (638 before; +26) |
| c1 | gameosmain hooks | DONE | `markMainEntered` first in `main`; `invalidateAll` right after the launcher_env apply loop; `dumpConfig(stderr)` right after `mc2::log::init()` |
| c2 | EditorMFC `InitInstance` hooks | DONE | same three calls after `editor_set_default_env_vars()` |
| c3 | ModPicker `invalidate("MC2_ACTIVE_MOD")`; row is `reread` | DONE | `ModPicker.cpp`; table row flags `reread | passthrough` |
| c4 | Harness mains that `_putenv` call `invalidateAll` | DONE | brain_dispatch, render_state_contract, job_system |
| c5 | Dev shell `set_gate` invalidates (design 5.5, last bullet) | WAS MISSING, fixed | 8350a33d2 |
| c6 | `MC2_CONFIG_STRICT=1` forced by `run_smoke.py` | DONE | `FORCED_ENV` in `scripts/smoke_lib/runner.py` |
| c7 | `[CONFIG v1]` line in a real game log and editor log | PENDING LAUNCH HOLD | code path proven by the `test_gate` dump cases; no real log read yet |
| d1 | Standard floor: standalone projects C++20+, 17 dropped from the options | DONE | `cmake/Mc2Standalone.cmake`; `check-cxx-standard-floor.py` OK (23 standalone projects, 10 read gates); contract row `cxx_standard_floor` |
| d2 | `mc2_gate` linked into mc2, EditRel, tests, harnesses | DONE | root link line, `editor/CMakeLists.txt`, `mc2_link_gate()` in the harness and test projects; build links mc2, mc2_launcher, `Mission Editor.exe`, mc2_asset_viewer; harness build 16/16 |
| e1 | LG19 in the ratchet + tests + baseline | WAS BROKEN, fixed | The lane-2 merge kept a baseline written before LG19 existed: `cxx17_ratchet --check` failed (201 files 0 -> N). Re-written in 1c959080d. LG19 = **1391** (1397 at d00f9d26d; the 6 `MC2_ABL_TRACE` stub reads left with the ablmc2 split; no other idiom rose). `--check` OK, 37 ratchet tests pass |
| e2 | env_index / dead_gate_scan / gate_conflicts see `get<>` | DONE | d00f9d26d; `test_anti_churn.py`, `test_gate_conflicts.py` |
| e3 | `Gates` in include-firewall scope | DONE | `check-include-firewall.sh` SCOPE_DIRS |
| f | NTTP compile-cost probe | WAS UNDOCUMENTED, redone | below; tool is `.claude/cleanup/probe/nttp_probe.py` |

## NTTP probe

MSVC 14.44, `/O2 /Zi /std:c++23preview`, `/d1reportTime /Bt+`, 6 reps, one TU at a time. Sites come in four shapes in equal parts (lambda-static initialiser, namespace-scope initialiser, function template, macro). Each site is its own small function, so obj and PDB growth is mostly that function, not the gate read.

| TU | front end min / med (s) | back end min (s) | obj (bytes) | TU pdb (bytes) |
|---|---|---|---|---|
| synthetic, 0 sites | 0.325 / 0.337 | 0.006 | 21,643 | 126,976 |
| synthetic, 102 sites | 0.466 / 0.484 | 0.067 | 186,045 | 151,552 |
| synthetic, 1400 sites | 2.047 / 2.111 | 0.768 | 2,261,927 | 397,312 |
| `gos_postprocess.cpp` as is | 1.421 / 1.557 | 0.445 | 1,115,621 | 1,183,744 |
| plus `#include "Gates/gate.h"` only | 1.493 / 1.506 | 0.468 | 1,116,462 | 1,183,744 |
| plus 102 sites by name (NTTP) | 1.659 / 1.703 | 0.536 | 1,285,745 | 1,216,512 |
| plus 102 sites, plain `load(<idx>)` control | 1.508 / 1.558 | 0.506 | 1,279,291 | 1,200,128 |
| plus 102 sites, `size_t` NTTP index (the fallback form) | 1.542 / 1.565 | 0.514 | 1,283,515 | 1,200,128 |

Reading:
- The header costs about +0.05 s on the 1.4 s TU. 102 sites by name cost +0.24 s (+17%) front end and +33 KB TU pdb (+2.8%) over the untouched file.
- Most of that is the 102 appended function bodies (plain-load control: +0.09 s, +16 KB pdb). The name-as-NTTP part alone is about +0.10 to +0.12 s front end per 102 sites (about 1 ms per site) and +16 KB TU pdb (about 160 bytes per distinct name) versus the index form.
- Worst case, all 1400 sites in one TU: +1.7 s front end, +0.76 s back end. Real files hold at most 103 sites and the average is 7, so the per-TU line (about 2 s) is not reached.
- obj bloat: +170 KB for 102 sites is function-per-site overhead; a real migrated read replaces a `getenv` call and adds no function. It is not a measure of the gate read.
- Against the design's literal budget (at most +5% front end and +0.5% PDB on the 102-site TU) the synthetic probe is OVER (+17% / +2.8%), but it measures appended code, not a swap of 103 existing `getenv` calls for `get<>` calls. The honest test is the G1b pilot (`gos_terrain_patch_stream.cpp`, 12 sites, obj 504,961 bytes today) measured before and after. Recommendation: keep the readable name form; if the pilot shows more than about +5% obj or +0.1 s front end for 12 sites, switch the codemod to pre-resolved indices (`get<Idx>`). The fallback is NOT implemented; it needs an orchestrator decision.

## Verification at this tree (build only)

- Full build (Ninja, reconfigured): mc2, mc2_launcher, mc2_asset_viewer, EditRel link. mc2.exe built from 8350a33d2.
- `tools/build_contract_harnesses.py --run --unit`: built 16/16, 20 harnesses PASS, doctest 664 cases / 124,347 assertions, 0 failed.
- `gate_gen --check --rebuild` consistent; `cxx17_ratchet --check` OK; `check-cxx-standard-floor` OK; `check-background-launch` OK.
- `check-contracts.sh` (one run, before the hold): 165 pass, 2 fail (`background_launch`, `scripts_tests` via `test_check_background_launch`), both caused by `gate_table_gen.py` and fixed in dd9213cce; the two relevant pytest files pass after the fix. A full re-run is queued until the hold lifts.
- `gos_terrain_patch_stream.cpp.obj`: 504,961 bytes; the TU includes no Gates header and was not rebuilt, so unchanged by construction.
- Deploy to `mc2-win64-v0.4`: rc 0 (hygiene warning about pre-existing byproducts only).

## Launch-gated verification (hold lifted for lane 1; PIA crash dumps stayed 5, conhost.exe 33-34 at every step)

- Fixed on the way: the first lite smoke exited 78 on both missions. `MC2_CONFIG_STRICT=1` (forced by the runner) correctly caught `MC2_CURSOR_NO_PARK`, which the capture/smoke workflow sets ambient and which reached mc2.exe, as `unregistered=1`. The table lacked the names only harness Python reads. Registered 16 of them (owner `harness`): 009ed0e62. Index change rebuilt only 14 steps (few TUs include `gate.h`).
- Lite smoke at 009ed0e62: `[DEPLOY_FINGERPRINT] OK: exe sha=009ed0e62fcd matches worktree HEAD`; `result=PASS (2/2 passed)` (mc2_01 1393 frames 141 fps; mc2_10 887 frames 90 fps). Game log, both missions: `[CONFIG v1] summary total=1155 set=14 nondefault=12 unregistered=0 badvalue=0 premain_stale=0`. No `[LAUNCHER_ENV` line (only the `MC2_LAUNCHER_ENV_JSON=0` CONFIG row).
- Full `check-contracts.sh`: 167 passed, 0 failed.
- `build_contract_harnesses.py --run --unit`: 16/16 built, 20 harnesses PASS, doctest 664/664 (re-run after the table change).
- Editor: `run_editor_smoke.py --case outliner` PASS 1/1; `[CONFIG v1] summary total=1155 set=9 nondefault=9 unregistered=0 badvalue=0 premain_stale=0`. The runner now reports that line and `--keep-logs` saves the captured stderr (54e9088df).
- GL day capture `mc2_01_saved_205644`, `tests/visual/captures/g1a_after_gl` vs `g1a_before_gl` (taken 19:04 with the `MC2_LAUNCHER_ENV_JSON=0` harness, pre-G1a exe): NOT byte-identical (sha 275b765d vs 6ded58e4), MAD 0.02, 0.05% of pixels over 24. Viewed by the builder: terrain, water, sky and props identical; the only difference is one mech's animation pose. Two captures of the same post-G1a exe differ from each other by MAD 0.05 / 0.10% (`g1a_after_gl2`), so the delta is inside run-to-run noise. Montage: `tests/visual/captures/g1a_ab/mc2_01_saved_205644/montage.jpg`.

## Opus G1a review follow-ups

Done in the pilot window (31a4ebfbe, a66f02ebf and the commit before the pilot files):
- A: every engine library links `mc2_gate` PUBLIC; `ninja all EditRel` links makefst, pak, aseconv, makersp, mc2_asset_viewer, tests; `text_tool` still fails to link for its documented, unrelated reasons (`broken-targets.md`: 165 unresolved game-layer symbols). `tools/mc2fx*` are game-free targets that never link gameos or mclib.
- B: `verified` row flag (strict fails a bad value only on a verified row; `gate_gen --sync-verified` keeps the flag equal to "a typed reader exists", `gate_gen --check` enforces it); wrong-kind rows corrected; Enum default must be a token; UI_DEFS default; six clamp rows. A lite smoke with `MC2_POSTPROCESS_BACKEND=vk MC2_RENDER_BACKEND_REGION_IFACE=1` no longer exits 78.
- C: reread rows are never cached; first-read CAS; lock-free static_assert; bdactor invalidates around its `_putenv_s`; override counter matches `using enum Rule`.
- Also: `default_build_quiet` baseline knows `[CONFIG v1]`; `flag-registry-auto-gen.py` sweeps `get<>` readers (otherwise migrated gates vanish from `docs/flags.auto.yaml`).

Queued (not done): Enum rows return a typed `Token` enum class (no bool conversion) before G3; LG19 companion ratchet on `"MC2_` literals outside get/state/isSet/str helpers, including helper calls and `#define` lines, scanning tools/ and tests/; `markMainEntered` in harness mains; strict-mode design: the runner should drop ambient `MC2_*` names absent from the table instead of registering harness-only rows (16 owner `harness` rows exist now); test_gate cases for Int `"+1"` / overflow, Float `"1e999"` on a Std row, Prefix whitespace/sign, strict exit path (`_Exit(78)` in a child); NaN input on a Prefix float row is rejected to the default (the old atof readers kept NaN).

## G2 single truth (3b8c50deb, 42409bd35)

- `gate::get<>` on an Enum row returns `gate::Token` (enum class, no bool/int conversion); `reread` rows are parsed on every read (31a4ebfbe).
- Table rows carry `registry(Kind, featureId, order, defaultOn)` (353 rows; reverse-Z, which has no env var, is in `Gates/feature_only_rows.json`). `tools/gate_registry_gen.py` writes the `kFeatureTable`/`kAuxEnvVars` rows of `RendererFeatureRegistry.h` and `docs/flags.auto.yaml` v3 from `Gates/gate_table.json`; `gate_gen --write` regenerates, `gate_gen --check` fails when either is stale.
- Python tools read the table through `scripts/gate_table_reader.py` (env_index, gate_status, build_review_pack, gl_vk_feature_coverage). Deleted: `registry_parse.py`, `flag-registry-auto-gen.py`, `check-flag-registry-truth.py`, `check-flag-registry-drift.sh`, `test_gate_gen.py`, the one-shot seed importer.
- Board: 167 rows -> 163 (retired `registry_parser_selftest`, `flag_registry_generator_selftest`, `flag_registry_truth`, `flag_registry_truth_selftest`; `flag_registry_drift` now runs `gate_gen --check`).
- Registry docs are now the table's one-line `doc`; the long prose is `docs/gate-history.md` (a frozen archive, no generator).

## G3 terrain family (efffdac47 .. 191c74663)

- 7 files migrated (gos_terrain_height_tex, _mask_dispatch, _water_stream, _indirect, mclib/terrain.cpp, vulkan_terrain_island, gos_terrain_lod_chunk); LG19 1354 -> 1186, LG20 1626 -> 1469, doctest 684, contracts 162/162.
- `gate_codemod --apply --i-mean-it --files F` is real now (exact spans, comment-preserving, include outside #if blocks). `--family g3 --summary`: 13 sites left, all umbrella (G4).
- String rows read through `isSet<>` + `str<>`; `isNonEmpty<>` exists for the `v && v[0]` test. Tools taught typed readers: gl_vk_cpp_gates, cpu_gpu_cogating, check-delivery, check-lighting-divergence.
- Lessons: a planner "NO-CHANGE" is only as good as its 8 probes (exact-'0' readers vs Lead0Kill rows differ on 01/0.5); multi-gate statements (alias reads, `||` of two gates) must be read by a human; comments inside replaced lambdas must be re-emitted.
- Logging P2 was NOT done for these files except keeping the existing printf behind migrated gates; shape-identical conversion of the ~hundreds of terrain printf traces is its own slice.

## G4 VK umbrella (bf46620a7 .. 8450a4621)

- G4.0 (bf46620a7): `inherits("MC2_VK_SHARED_GBUFFER")` row option on the 25 Tri rows; `rowIsSane` Tri <=> inherits; `parentsAreValid` static_assert (parent exists, Bool Exact1, no parent of its own); `IndexEntry.parent`; `gate::effective<n>(bool parentSuppressed)` (no default) and consteval `hasParent<n>()`; JSON/index/yaml carry it. Differential `test_gate`: the verbatim old `envTriState`/`subGate` over {unset,"",0,1,yes,10,true} x the same for the umbrella x fail-soft, all 25 rows. The Opus G3 review fixes rode along (DIST_SCALE bound is `(double)0.05f`; the second ANTITILE raw reader reads the row; cogating reads the table from its root argument; `isNonEmpty<>` in the reader regexes).
- G4.1 (225c12fba): `mc2vkgate::armed<"MC2_VK_X">()` adapter; the 17 per-call readers (never cached, so a mid-session fail-soft disarms). Tools learn `armed<>`/`regionTier1Armed<>` as typed readers (12 more rows `verified`).
- G4.2 (89935dfd9): the tier1-and-shared-depth pair x 6 -> `regionTier1Armed<>`; `compositeDirectChainOn()` replaces two hand-copied composite predicates; `gos_mech_batcher.cpp` gate is lazy (`vkMechGateOn()`), the pre-main static initializer is gone (intended delta on the dev-only MC2_NO_LAUNCHER + launcher_env.json path).
- G4.3 (73f90e6d3): terrain readers; `terrainTier1Requested` caches `terrainNativeFirstOwnerOn()`.
- G4.4 (c88a83d33): the remaining `envTriState` expressions -> typed reads (Exact1/Is01 identical; the 8 `==0?0:1` kill switches are EDGE on "00"/"0x"), probe-vector doctest against the verbatim old body.
- G4.5a (c97559907): `gate_codemod --family g4 [--gate-prefix MC2_VK_]` (44 sites: 26 applied by the tool, 18 by hand); the three `*_GL_CANARY` rows become Lead1; `PRESENT_DUMP_PPM` and `PROP_PACKET_DUMP` become String rows; 6 vars leave the passthrough debt list.
- G4.5b (8450a4621): adapter ~70 lines, old helpers and prose deleted; `vk_umbrella_light.h` + `mclib/vk_umbrella_gate.cpp` for gamecam/objmgr/weaponbolt/appear; `gate_conflicts` Typed(effective) rule (25 members, 40 typed sites, 0 violations) + ledger `intentional-optin` (9 gates); `scripts/check-vk-umbrella-readers.py` (no direct `effective<"MC2_VK_`, 13 per-call groups stay uncached); `--minimal-promotion` drops every `inherits` row (13 in the strict stack) and the strict runner still PASSes; five level-valued gates are Enum rows (readers untouched, they are mclib/code slices); tier1_env_vars drift fixed; smoke marker bucket `config_premain_vk`.
- Counts: LG19 1186 -> 1144 (-42), LG20 1469 -> 1357 (-112; the design's bar was -62). Census: 5 conflicting gates (all ledgered), 0 unresolved, 0 umbrella violations. Doctest 684 -> 689, contract rows 162 -> 164.
- Proof per slice: strict no-interop runner (`--minimal-promotion --resource-strict --headless`, dev install) PASS before and after with the 8 `[VK_*] [HEALTH]` lines identical after masking numbers; `MC2_VK_SHARED_GBUFFER=1 MC2_VK_FORCE_FAIL=1` logs `[RENDER_BACKEND] fail-soft`, no VK HEALTH, and the capture equals plain GL (MAD <= 0.05); GL+VK day/night vs `g3_after` (GL <= 0.03, VK <= 0.03 MAD; montages viewed). No `[CONFIG v1] warn=premain name=MC2_VK_` in any strict run.
- TU cost (min of 11, compile front+back end): objmgr 3.69 -> 3.83 s (+3.6%), appear and gamecam unchanged, weaponbolt carries no Gates include. Including `Gates/gate.h` costs about +0.1 s per small translation unit, which is why those four use the out-of-line light header.
- Lessons: `gate_codemod` reads rows from HEAD but files from the working tree; `cxx17_ratchet` counts the COMMITTED tree unless `--worktree`/`--staged` is given (baseline written with `--staged` at commit time); `sed -i` on a CRLF file in this checkout flips the whole file to LF (use a python read/write with newline=""); a strict-runner FAIL with missing shutdown lines under a concurrent lane build is load, rerun before chasing it.
- Not done: full tier1 (needs the quiet window the coordinator holds lane 2 for); the five level gates' readers (code/warrior.cpp, mclib/txmmgr.cpp, cone_glow.cpp, bdactor.cpp) stay raw until their slices.
