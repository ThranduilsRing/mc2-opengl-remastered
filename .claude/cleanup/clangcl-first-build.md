<!-- doc-status: current | reviewed:2026-10-13 -->
# H1: clang-cl first-build census

Diagnostic preparation on `claude/clangcl-preset`, baseline `78d63070cde040bda8e4d4da569e62bbc09ba676`, Windows host DESKTOP-P1N69B5, 2026-10-13 CDT.
Design read with `git show d4895e56d:.claude/cleanup/platform-module-design.md`; the design is intentionally absent from this branch.
**Partial: no source fixes, no X1 batch closed, no compiler/runtime parity claim.** The sandbox MSVC attempt was blocked as recorded below; the orchestrator ran the unchanged default `py -3 tools/build_all_targets.py` outside the sandbox with this diff applied (worktree mc2-s1, base 3445d656f-era nifty, 2026-10-13 ~20:00): every stage built, 16/16 harnesses (log: scratchpad H1-msvc.txt).

## Presets and selection

`clang-cl-dev` configures Ninja and builds `all ui_editor`; `msvc-ninja-dev` is an explicit separate MSVC Ninja tree. Omitting `--preset` preserves the original MSVC generators, flags and directory names.
`clang-cl-nmake` is an explicit diagnostic alternative, with its own trees, because even a minimal Ninja echo command hangs in this sandbox. It is serial and does not silently replace the Ninja preset.
Every selected project receives the same toolchain, C++23, RelWithDebInfo and generator. Configure evidence verifies Clang plus the MSVC frontend and the actual clang-cl executable (including C where enabled); mismatch/missing evidence fails the stage.
Root presets enable the editor and all three opt-in tools. Native contracts now share one `tests/unit/mc2_tests` configure/build; the local selection index maps `--only` names to doctest case prefixes, and unknown requests fail. Retained Python suites run directly. Preset `--run` uses only the selected corpus tree's executable.
Root PCHs and outputs are compiler-private. Standalone and corpus trees never share caches or outputs with MSVC or the alternate generator. Existing prebuilt dependency libraries remain the same imported inputs; this task does not migrate their providers. The tables and logs below remain the dated pre-migration measurement, including the former standalone native harness stages.

```text
cmake --preset clang-cl-dev
cmake --build --preset clang-cl-dev
py -3 tools/build_all_targets.py --preset clang-cl-dev --reconfigure --keep-going --log-dir <scratch>
py -3 tools/build_contract_harnesses.py --preset clang-cl-dev --keep-going --run --unit --log-dir <scratch>
```

Direct CMake commands require an initialized x64 VS environment; Python entry points initialize it. LLVM discovery follows `MC2_TOOL_CLANG_CL`, `LLVM_ROOT`, PATH and the installed LLVM location in docs/host-tools.md. Missing LLVM is fatal.
The root and standalone `/std:c++23preview` overrides now apply only to compiler ID MSVC; root `/MP` is MSVC-only. Clang's actual commands contain `-clang:-std=c++23`, with no lowered standard or broad warning suppression.

## Host blockage and limits

The initial logging-wrapper quote failure was fixed before the source census; its configure logs are retained in `A:/Games/mc2-s1/build-h1-logs/clangcl/` (21 files).
The subsequent Ninja configure identified Clang 23.1.3 for both C/C++ and emitted the C ABI object, then hung with an idle Ninja and no compiler child. Owned stalled processes were stopped; no other lane process was touched. Evidence: `build-h1-clangcl-attempt1.log`, `clangcl-attempt1/root.configure.log`, the retained CMakeConfigureLog.yaml and the minimal `ninja-echo.ninja`/`ninja-probe.ninja` reproductions. The build preset itself fails with an unavailable cache after interrupted configure.
This matches the symptom described in the [upstream Windows sandbox named-pipe fix](https://github.com/openai/codex/commit/06f3b4836ac6e4fecc1d75a09b8a9fa369b16635), but the precise local token/pipe cause was not independently proven. No sandbox policy or ACL was changed.
The default MSVC command also hung in two Ninja compiler checks; a scratch watchdog stopped only its idle Ninja descendants after 45 seconds, allowing the remaining stages to report. tg_import_dump and mech_import_harness subsequently returned MSBuild failure after Checking Build System, without a source diagnostic. gate_dump, 16/16 harnesses and the unit build succeeded. Historical sandbox attempt; MSVC green was then established outside the sandbox by the orchestrator (see the status line above).
NMake `/K` continued at the requested top level, but CMake's recursive target prerequisites prevented many downstream compile/link rules after failed libraries. The table exposes those blocked requests. These diagnostic counts are a lower bound, not an exhaustive census of all 1,537 configured root compile rules. Ninja `-k 0` must be rerun when the host works.

## Requested/configured/compiled/linked/blocked

Objects are distinct successfully emitted C/C++ object paths from compile_commands.json, compared with configured requests. Compiler ID/ABI probes are excluded. Linked artifacts below exclude compiler-check executables; static library successes do not imply product linkage.
The unit stage requests mc2_tests and its mc2_gate dependency, matching the existing builder. Unit-project test_render_contract_3 and mc2_modlint are configured but outside that requested stage for both compilers; they are not silently counted as compiler successes.

| Stage | Requested | Configured | Objects emitted/requested | Executable linked | Blocked |
|---|---|---|---:|---|---|
| `root` | all, ui_editor | yes: Clang 23.1.3 | 371/1537 | no | GameOS language linkage and SDL builtin conflict; product and ui_editor dependencies blocked |
| `tg_import_dump` | tg_import_dump | yes: Clang 23.1.3 | 202/202 | tg_import_dump.exe | none |
| `gate_dump` | gate_dump | yes: Clang 23.1.3 | 3/3 | gate_dump.exe | none |
| `mech_import_harness` | mech_import_harness | yes: Clang 23.1.3 | 126/126 | mech_import_harness.exe | none |
| `contract_smoke_harness` | contract_smoke_harness | yes: Clang 23.1.3 | 1/1 | contract_smoke_harness.exe | none |
| `shader_contract_harness` | shader_contract_harness | yes: Clang 23.1.3 | 3/3 | shader_contract_harness.exe | none |
| `render_state_contract_harness` | render_state_contract_harness | yes: Clang 23.1.3 | 6/6 | render_state_contract_harness.exe | none |
| `objmgr_contract_harness` | objmgr_contract_harness | yes: Clang 23.1.3 | 1/1 | objmgr_contract_harness.exe | none |
| `ibl_registry_contract_harness` | ibl_registry_contract_harness | yes: Clang 23.1.3 | 3/3 | ibl_registry_contract_harness.exe | none |
| `render_pass_table_harness` | render_pass_table_harness | yes: Clang 23.1.3 | 1/1 | render_pass_table_harness.exe | none |
| `logistics_csv_harness` | logistics_csv_harness | yes: Clang 23.1.3 | 1/1 | logistics_csv_harness.exe | none |
| `camera_frustum_harness` | camera_frustum_harness | yes: Clang 23.1.3 | 1/1 | camera_frustum_harness.exe | none |
| `mech_texname_harness` | mech_texname_harness | yes: Clang 23.1.3 | 1/1 | mech_texname_harness.exe | none |
| `icon_atlas_harness` | icon_atlas_harness | yes: Clang 23.1.3 | 1/1 | icon_atlas_harness.exe | none |
| `timing_math_harness` | timing_math_harness | yes: Clang 23.1.3 | 1/1 | timing_math_harness.exe | none |
| `job_system_harness` | job_system_harness | yes: Clang 23.1.3 | 5/5 | job_system_harness.exe | none |
| `terrain_daylight_harness` | terrain_daylight_harness | yes: Clang 23.1.3 | 1/1 | terrain_daylight_harness.exe | none |
| `object_light_row_harness` | object_light_row_harness | yes: Clang 23.1.3 | 1/1 | object_light_row_harness.exe | none |
| `class_light_band_harness` | class_light_band_harness | yes: Clang 23.1.3 | 1/1 | class_light_band_harness.exe | none |
| `glstate_guard_harness` | glstate_guard_harness | yes: Clang 23.1.3 | 3/3 | no | link: undefined mc2gl::gos_InvalidateRenderStateCache |
| `mc2_tests` | mc2_tests | yes: Clang 23.1.3 | 117/117 | mc2_tests.exe | runtime: 3 allocation-probe cases fail (build/link succeeded) |

Root target detail (all configured targets, including requests with zero emitted objects):

| Target | Objects emitted/requested | Compile status |
|---|---:|---|
| `EditRel` | 0/123 | blocked/incomplete |
| `aseconv` | 0/4 | blocked/incomplete |
| `assimp` | 107/107 | all objects emitted |
| `editorbridge` | 0/1 | blocked/incomplete |
| `fx_trace` | 2/2 | all objects emitted |
| `gameadapters` | 4/4 | all objects emitted |
| `gameos` | 44/163 | blocked/incomplete |
| `gameos_editor` | 0/161 | blocked/incomplete |
| `gameos_main` | 0/3 | blocked/incomplete |
| `gosfx` | 0/19 | blocked/incomplete |
| `gui` | 0/12 | blocked/incomplete |
| `gui_runtime` | 0/8 | blocked/incomplete |
| `imgui` | 6/8 | blocked/incomplete |
| `makefst` | 0/4 | blocked/incomplete |
| `makersp` | 0/4 | blocked/incomplete |
| `mc2` | 0/197 | blocked/incomplete |
| `mc2_asset_viewer` | 0/36 | blocked/incomplete |
| `mc2_gate` | 2/2 | all objects emitted |
| `mc2_launcher` | 0/1 | blocked/incomplete |
| `mc2_tglloader` | 0/80 | blocked/incomplete |
| `mc2fx` | 0/133 | blocked/incomplete |
| `mc2fx_corelib` | 0/132 | blocked/incomplete |
| `mc2fx_preview` | 0/1 | blocked/incomplete |
| `mc2res_64` | 0/2 | blocked/incomplete |
| `mc2weapon_gui` | 0/2 | blocked/incomplete |
| `mclib` | 175/175 | all objects emitted |
| `mlr` | 0/53 | blocked/incomplete |
| `model_override_tests` | 0/2 | blocked/incomplete |
| `pak` | 0/4 | blocked/incomplete |
| `particles` | 10/10 | all objects emitted |
| `rendercore` | 4/4 | all objects emitted |
| `renderworld` | 2/2 | all objects emitted |
| `static_natural_skip_policy_tests` | 0/1 | blocked/incomplete |
| `stuff` | 0/46 | blocked/incomplete |
| `text_tool` | configured, not requested (EXCLUDE_FROM_ALL, text_tool/CMakeLists.txt:11) | 0/1 | not part of `all ui_editor` |
| `ui_editor` | 0/5 | blocked/incomplete |
| `windows` | 0/10 | blocked/incomplete |
| `zlibstatic` | 15/15 | all objects emitted |

Complete per-target commands, compiler paths, link attempts and surviving artifacts: `A:/Games/mc2-s1/build-h1-logs/stages.json`.

## Compiler diagnostics

Counts include compiler/linker diagnostic lines from the 21 NMake configure/build log pairs, once per occurrence. Notes, excerpts, summaries, NMake wrapper errors and CMake policy/deprecation messages are excluded from compiler totals and retained separately in census.json. Paths are slash-normalized, dot segments removed and case-folded for Windows identity. A unique location is (physical file, line, column), independent of severity/class/TU/stage. Locationless diagnostics have no invented source location.

**Totals: 3 errors, 21503 warnings, 3448 unique locations; 1 locationless occurrences.**

The large format-truncation class is the intentional eight-byte snprintf comparison in tests/unit/test_printf_corpus.cpp and printf_corpus_gen.inc, not thousands of newly discovered runtime defects. Missing-field warnings point to generated Gates/gate_index.gen.h; any fix belongs in the generator. Both remain counted without suppression.

| Class | Errors | Warnings | Occurrences | Unique locations | Fix batch | Owning lane(s) |
|---|---:|---:|---:|---:|---|---|
| `-Wmissing-field-initializers` | 0 | 13720 | 13720 | 980 | X1 batch 2 registry-generator diagnostics; no hand edits to generated header | lane 2 |
| `-Winconsistent-missing-override` | 0 | 3369 | 3369 | 83 | X1 batch 2; ownership/MLR follow-up B8/R2 | lane 1 |
| `-Wformat-truncation` | 0 | 1755 | 1755 | 1755 | X1 batch 2 test-floor triage; retain intentional truncation corpus | lane 1 |
| `-Woverloaded-virtual` | 0 | 1075 | 1075 | 35 | X1 batch 2; ownership/MLR follow-up B8/R2 | lane 1 |
| `-Wdeprecated-non-prototype` | 0 | 438 | 438 | 146 | X1 batch 2 compiler/dependency diagnostics | H build/dependency lane |
| `-Wimplicit-exception-spec-mismatch` | 0 | 294 | 294 | 1 | X1 batch 2; memory API retirement S2 | lane 2 |
| `-Wunused-but-set-variable` | 0 | 137 | 137 | 40 | X1 batch 2 compiler/dependency diagnostics | lane 1, lane 2 |
| `-Wformat` | 0 | 107 | 107 | 61 | X1 batch 2; C++23 B1 formatted-text migration | H build/dependency lane, lane 1 |
| `-Wnontrivial-memcall` | 0 | 98 | 98 | 19 | X1 batch 2 compiler/dependency diagnostics | H build/dependency lane, lane 1 |
| `-Wnonportable-include-path` | 0 | 86 | 86 | 5 | X1 batch 2; header/type migration batch 4 | lane 1, lane 2 |
| `-Wunused-function` | 0 | 71 | 71 | 51 | X1 batch 2 compiler/dependency diagnostics | H build/dependency lane, lane 1, lane 2 |
| `-Wunused-variable` | 0 | 58 | 58 | 57 | X1 batch 2 compiler/dependency diagnostics | lane 1, lane 2 |
| `-Wdeprecated-declarations` | 0 | 49 | 49 | 17 | X1 batch 2 compiler/dependency diagnostics | H build/dependency lane, lane 1, lane 2 |
| `-Wformat-truncation-non-kprintf` | 0 | 47 | 47 | 47 | X1 batch 2 compiler/dependency diagnostics | lane 1 |
| `-Wdeprecated-copy-with-user-provided-copy` | 0 | 31 | 31 | 5 | X1 batch 2 compiler/dependency diagnostics | lane 1 |
| `-Wunused-const-variable` | 0 | 28 | 28 | 26 | X1 batch 2 compiler/dependency diagnostics | lane 1 |
| `-Wchar-subscripts` | 0 | 26 | 26 | 13 | X1 batch 2 compiler/dependency diagnostics | lane 1 |
| `-Wsign-compare` | 0 | 20 | 20 | 20 | X1 batch 2 compiler/dependency diagnostics | lane 1 |
| `-Wmisleading-indentation` | 0 | 18 | 18 | 18 | X1 batch 2 compiler/dependency diagnostics | lane 1 |
| `-Wdelete-non-abstract-non-virtual-dtor` | 0 | 14 | 14 | 14 | X1 batch 2; ownership/MLR follow-up B8/R2 | lane 1 |
| `-Wswitch` | 0 | 12 | 12 | 12 | X1 batch 2 compiler/dependency diagnostics | lane 1 |
| `-Wmissing-designated-field-initializers` | 0 | 11 | 11 | 11 | X1 batch 2 compiler/dependency diagnostics | lane 2 |
| `-Wunused-but-set-global` | 0 | 9 | 9 | 9 | X1 batch 2 compiler/dependency diagnostics | lane 1, lane 2 |
| `-Wunused-value` | 0 | 8 | 8 | 3 | X1 batch 2 compiler/dependency diagnostics | lane 1 |
| `-Wlogical-op-parentheses` | 0 | 4 | 4 | 4 | X1 batch 2 compiler/dependency diagnostics | lane 1 |
| `-Wmacro-redefined` | 0 | 4 | 4 | 2 | X1 batch 2 compiler/dependency diagnostics | H build/dependency lane, lane 2 |
| `-Wunused-private-field` | 0 | 4 | 4 | 4 | X1 batch 2 compiler/dependency diagnostics | lane 2 |
| `-Winfinite-recursion` | 0 | 2 | 2 | 1 | X1 batch 2 compiler/dependency diagnostics | lane 1 |
| `-Wmicrosoft-cast` | 0 | 2 | 2 | 2 | X1 batch 2 compiler/dependency diagnostics | lane 1 |
| `-Wsometimes-uninitialized` | 0 | 2 | 2 | 1 | X1 batch 2 compiler/dependency diagnostics | lane 1 |
| `-Wcomment` | 0 | 1 | 1 | 1 | X1 batch 2 compiler/dependency diagnostics | editor arc |
| `-Wextern-initializer` | 0 | 1 | 1 | 1 | X1 batch 2 compiler/dependency diagnostics | lane 2 |
| `-Wreorder-ctor` | 0 | 1 | 1 | 1 | X1 batch 2 compiler/dependency diagnostics | lane 2 |
| `-Wunused-parameter` | 0 | 1 | 1 | 1 | X1 batch 2 compiler/dependency diagnostics | lane 1 |
| `builtin-definition` | 1 | 0 | 1 | 1 | X1 batch 2 compiler/dependency diagnostics | H build/dependency lane |
| `language-linkage` | 1 | 0 | 1 | 1 | X1 batch 2 compiler/dependency diagnostics | lane 2 |
| `undefined-symbol` | 1 | 0 | 1 | 0 | X1 batch 2 compiler/dependency diagnostics | lane 2 (GameOS declaration), H harness lane (stub/link closure) |

Batch numbers above are the design's implementation recipes, not completion claims. Lane 1 owns code/mclib/unit diagnostics; lane 2 owns GameOS/Gates/render diagnostics; editor diagnostics belong to the editor arc. H build/dependency owns vendor and toolchain compatibility. UI/bridge ownership routes are proposals for the orchestrator, not new source claims.

| Directory | Errors | Warnings | Occurrences | Unique locations |
|---|---:|---:|---:|---:|
| `gates` | 0 | 13763 | 13763 | 995 |
| `mclib` | 0 | 4137 | 4137 | 374 |
| `tests/unit` | 0 | 1802 | 1802 | 1802 |
| `3rdparty/assimp/contrib/zlib` | 0 | 438 | 438 | 146 |
| `mclib/gosfx` | 0 | 345 | 345 | 5 |
| `mclib/stuff` | 0 | 345 | 345 | 11 |
| `gameos/include` | 0 | 297 | 297 | 2 |
| `gameos/gameos` | 1 | 120 | 121 | 24 |
| `code` | 0 | 95 | 95 | 32 |
| `mclib/mlr` | 0 | 70 | 70 | 1 |
| `gui` | 0 | 49 | 49 | 13 |
| `3rdparty/stb` | 0 | 30 | 30 | 30 |
| `tools/mech_import_harness` | 0 | 5 | 5 | 5 |
| `3rdparty/tinyexr` | 0 | 3 | 3 | 3 |
| `tools/tg_import_dump` | 0 | 2 | 2 | 2 |
| `3rdparty/include/sdl2` | 1 | 0 | 1 | 1 |
| `<driver>` | 1 | 0 | 1 | 0 |
| `editor` | 0 | 1 | 1 | 1 |
| `gameadapters` | 0 | 1 | 1 | 1 |

Top 10 files by occurrences:

| File | Errors | Warnings | Occurrences | Unique locations |
|---|---:|---:|---:|---:|
| `gates/gate_index.gen.h` | 0 | 13720 | 13720 | 980 |
| `mclib/bdactor.h` | 0 | 3315 | 3315 | 65 |
| `tests/unit/printf_corpus_gen.inc` | 0 | 1802 | 1802 | 1802 |
| `gameos/include/gameos.hpp` | 0 | 294 | 294 | 1 |
| `mclib/mech3d.h` | 0 | 180 | 180 | 3 |
| `mclib/stuff/filestream.hpp` | 0 | 156 | 156 | 1 |
| `mclib/stuff/memorystream.hpp` | 0 | 156 | 156 | 1 |
| `mclib/csvfile.h` | 0 | 108 | 108 | 2 |
| `mclib/camera.h` | 0 | 90 | 90 | 1 |
| `3rdparty/assimp/contrib/zlib/deflate.c` | 0 | 84 | 84 | 28 |

Per-stage diagnostics:

| Stage | Errors | Warnings | Unique locations |
|---|---:|---:|---:|
| `root` | 2 | 5004 | 571 |
| `tg_import_dump` | 0 | 257 | 184 |
| `gate_dump` | 0 | 0 | 0 |
| `mech_import_harness` | 0 | 155 | 155 |
| `contract_smoke_harness` | 0 | 0 | 0 |
| `shader_contract_harness` | 0 | 4 | 4 |
| `render_state_contract_harness` | 0 | 4 | 4 |
| `objmgr_contract_harness` | 0 | 0 | 0 |
| `ibl_registry_contract_harness` | 0 | 4 | 4 |
| `render_pass_table_harness` | 0 | 0 | 0 |
| `logistics_csv_harness` | 0 | 0 | 0 |
| `camera_frustum_harness` | 0 | 0 | 0 |
| `mech_texname_harness` | 0 | 0 | 0 |
| `icon_atlas_harness` | 0 | 0 | 0 |
| `timing_math_harness` | 0 | 0 | 0 |
| `job_system_harness` | 0 | 5 | 5 |
| `terrain_daylight_harness` | 0 | 0 | 0 |
| `object_light_row_harness` | 0 | 0 | 0 |
| `class_light_band_harness` | 0 | 0 | 0 |
| `glstate_guard_harness` | 1 | 4 | 4 |
| `mc2_tests` | 0 | 16066 | 2983 |

Unclassified occurrences: **0**. Every unclassified record is retained verbatim in `census.json` and `diagnostics.json`, with its original log and log-line number; none is dropped.
All diagnostic occurrences (including duplicates), build-system messages, class/directory/file/stage aggregates and parser: `A:/Games/mc2-s1/build-h1-logs/{diagnostics.json,census.json,census.py}`.

## Additional runtime probe (outside compiler totals)

The compiled clang-cl unit tier was run through build_contract_harnesses.py with the same preset and exact tree: native exit 1, 2,834/2,837 cases passed, 3 failed, 3 skipped. All failures are in tests/unit/alloc_probe.cpp: new/delete block/byte accounting (lines 18, 19, 22, 23), leaked/new/release deltas (32, 36), and standard-owner allocation count (67). Seven unexpected assertions fail; the eighth reported failed assertion is the corpus's intentional should_fail case. No assertions, baselines or optimization flags were changed.
Root cause is unproven here. Allocation elision under optimization is a hypothesis, not a claim. Route this runtime class to lane 1, X1 batch 2 compiler-test baseline and S2 allocation/ownership proof. MSVC passes the same 2,837 cases. This is additional evidence that the second-compiler gate remains open.
Full runtime output: `A:/Games/mc2-s1/build-h1-logs/clangcl-unit.run.log`; complete cached configure/build logs: `build-h1-logs/clangcl-unit-runtime/mc2_tests.configure.log` and `mc2_tests.build.log`.

## Toolchain and C++23 probe

| Tool/component | Measured version |
|---|---|
| clang-cl and lld-link | 23.1.3, LLVM commit 0d261d1ca552c95a8f007e061c787ac7132fbcbc, x86_64-pc-windows-msvc |
| CMake | 3.31.6-msvc6 |
| Ninja | 1.12.1 |
| NMake / MSVC cl | 14.44.35225.0 / 19.44.35225.0 |
| VC toolset directory | 14.44.35207 |
| MSVC STL | 143; _MSVC_STL_UPDATE=202503 |
| Windows SDK | 10.0.26100.0 |
| Vulkan SDK discovered by root | 1.4.350.0 |
| Python | 3.14.0 |

Scratch C++23 probe compiled and linked with clang-cl `/MD /EHsc -clang:-std=c++23`, then returned 0: `__cplusplus=202302`, `__cpp_lib_expected=202211`, `__cpp_lib_format=202304`, formatted value 23. Its static assertion enforces C++23. Source/object/executable and compile/link/run logs are retained in build-h1-logs. No game/editor was launched.

## Proof commands and results

| Command | Result |
|---|---|
| `git show d4895e56d:.claude/cleanup/platform-module-design.md` | Design read; no design file copied into branch. |
| `py -3 tools/repo_intel/repo_query.py preflight --expect-branch claude/clangcl-preset --expect-root A:/Games/mc2-s1` | Branch/root clean and correct initially; harness command extraction ambiguous in canonical doc. |
| `py -3 tools/build_all_targets.py --preset clang-cl-dev --reconfigure --keep-going --log-dir A:/Games/mc2-s1/build-h1-logs/clangcl` | Initial new-wrapper quoting error in every configure; fixed, logs retained. |
| Same command with `--log-dir .../clangcl-attempt1` | C/C++ Clang IDs confirmed; Ninja host hang at C ABI probe; owned process tree stopped. |
| `cmake --list-presets`; `cmake --build --preset clang-cl-dev` (under vcvars64) | Three presets parse; build preset returns 1 because interrupted configure left no usable cache. |
| Ninja `-f build-h1-logs/ninja-echo.ninja -v` and compiler probe | Minimal echo and compiler-output capture stall; direct clang-cl compilation succeeds; owned stalled probes stopped. |
| `py -3 -m pip install ninja --target build-h1-logs/ninja-package --disable-pip-version-check` | No package available in restricted environment; temporary directories retained. |
| `py -3 tools/build_all_targets.py --preset clang-cl-nmake --reconfigure --keep-going --log-dir A:/Games/mc2-s1/build-h1-logs/clangcl-nmake` | All 21 projects requested and attempted; measured results above; source failures reported, no source fixes. |
| `py -3 build-h1-logs/ninja-watchdog.py` running exactly `py -3 tools/build_all_targets.py` | Native exit 1: root, tg_import_dump, mech_import_harness failed; gate_dump, 16 harnesses, unit build succeeded. |
| `py -3 tools/build_contract_harnesses.py --unit-only --run` (native exit verified through logging wrapper) | Exit 0; 2,837 doctest cases passed, 3 skipped; one intentional expected-failure assertion is reported as expected. |
| `py -3 tools/build_contract_harnesses.py --preset clang-cl-nmake --unit-only --run --keep-going --log-dir A:/Games/mc2-s1/build-h1-logs/clangcl-unit-runtime` (native exit verified) | Cached configure/build succeeded; native runtime exit 1, 3 alloc_probe cases failed / 7 unexpected assertions; compiler census excludes this cached rerun. |
| `py -3 -B -m pytest scripts/tests/test_build_compiler_presets.py -q -p no:tmpdir -p no:cacheprovider` | 22 passed; propagation, trees, default commands, failure continuation, no runtime fallback, frontend evidence, preset scope and quoting/cwd covered. |
| `py -3 scripts/check-cxx-standard-floor.py`; `git diff --check`; `git ls-files --eol <edited files>` | Green; existing LF endings preserved. |
| `py -3 scripts/check-background-launch.py --quiet` | Exit 1, two failures in unchanged scripts/tests/test_board_selftests.py: 40 spawns above baseline and dynamic-spawn allowlist; no own-file failure. |
| `py -3 tools/callers.py --check`; `py -3 build-h1-logs/caller-diff.py` | Inventory stale in 31 existing rows; only the retired native contract runner's caller row changes due to H1 at this unstaged state. Expected diff retained in caller-diff.json; tracked inventory not regenerated. |
| `py -3 build-h1-logs/census.py`; `py -3 build-h1-logs/stage-inventory.py` | Aggregation and all-stage compiler/object/link inventory retained as JSON. |
| `py -3 build-h1-logs/render-report.py`; `py -3 build-h1-logs/archive-proof.py` | Report saved with the editor tool; full logs/compiler evidence archived on E:, checksum sidecar retained. |

Cannot run: `bash scripts/check-contracts.sh` (Git Bash forbidden); commit/index operations (orchestrator-only); successful Ninja gate/default MSVC all-target proof in the sandbox (host blockage; the orchestrator's outside run passed). No deploy or smoke was needed or attempted; no forbidden install or source tree was opened or edited.

## Retained scratch paths

All paths below are under `A:/Games/mc2-s1/` unless absolute. No scratch was deleted.
Full scratch-file index: `A:/Games/mc2-s1/build-h1-logs/scratch-files.txt`.
Portable evidence archive (logs, census/parser, compiler caches/evidence and this report): `E:/mc2-archive-2026-10/clangcl-h1-2026-10-13.tar.gz`; checksum sidecar: `E:/mc2-archive-2026-10/clangcl-h1-2026-10-13.tar.gz.sha256`.

| Stage | Tree | Full configure log | Full build log |
|---|---|---|---|
| `root` | `A:/Games/mc2-s1/build-clangcl-nmake-alltargets` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/root.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/root.build.log` |
| `tg_import_dump` | `A:/Games/mc2-s1/build-clangcl-nmake-tgdump` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/tg_import_dump.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/tg_import_dump.build.log` |
| `gate_dump` | `A:/Games/mc2-s1/build-clangcl-nmake-gate-dump` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/gate_dump.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/gate_dump.build.log` |
| `mech_import_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/mech_import_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/mech_import_harness.build.log` |
| `contract_smoke_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-contract_smoke_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/contract_smoke_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/contract_smoke_harness.build.log` |
| `shader_contract_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-shader_contract_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/shader_contract_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/shader_contract_harness.build.log` |
| `render_state_contract_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-render_state_contract_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/render_state_contract_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/render_state_contract_harness.build.log` |
| `objmgr_contract_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-objmgr_contract_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/objmgr_contract_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/objmgr_contract_harness.build.log` |
| `ibl_registry_contract_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-ibl_registry_contract_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/ibl_registry_contract_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/ibl_registry_contract_harness.build.log` |
| `render_pass_table_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-render_pass_table_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/render_pass_table_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/render_pass_table_harness.build.log` |
| `logistics_csv_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-logistics_csv_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/logistics_csv_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/logistics_csv_harness.build.log` |
| `camera_frustum_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-camera_frustum_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/camera_frustum_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/camera_frustum_harness.build.log` |
| `mech_texname_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-mech_texname_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/mech_texname_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/mech_texname_harness.build.log` |
| `icon_atlas_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-icon_atlas_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/icon_atlas_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/icon_atlas_harness.build.log` |
| `timing_math_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-timing_math_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/timing_math_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/timing_math_harness.build.log` |
| `job_system_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-job_system_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/job_system_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/job_system_harness.build.log` |
| `terrain_daylight_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-terrain_daylight_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/terrain_daylight_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/terrain_daylight_harness.build.log` |
| `object_light_row_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-object_light_row_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/object_light_row_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/object_light_row_harness.build.log` |
| `class_light_band_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-class_light_band_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/class_light_band_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/class_light_band_harness.build.log` |
| `glstate_guard_harness` | `A:/Games/mc2-s1/build-clangcl-nmake-h-glstate_guard_harness` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/glstate_guard_harness.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/glstate_guard_harness.build.log` |
| `mc2_tests` | `A:/Games/mc2-s1/build-clangcl-nmake-tests` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/mc2_tests.configure.log` | `A:/Games/mc2-s1/build-h1-logs/clangcl-nmake/mc2_tests.build.log` |

Other retained scratch: `build-clangcl-alltargets/` (interrupted Ninja configure), `build-alltargets/`, `build64-tgdump/`, `build64-gate-dump/`, `build64-harness/`, the former per-contract MSVC trees, `build64-tests/`, `.ninja_lock`, `build-h1-*.log`, and all `build-h1-logs/` sources/logs/JSON/probe artifacts. The default proof is `build-h1-logs/msvc-default.log`; unit proof is `build-h1-logs/msvc-unit.run.log`; versions are `build-h1-logs/versions-0.probe.log` through `versions-6.probe.log`.
Pip scratch retained under `C:/Users/Joe/AppData/Local/Temp/`: `pip-build-tracker-g8ayjtxg`, `pip-target-ab6p4khy`, `pip-ephem-wheel-cache-c4i_gkup`, `pip-install-n45e_d5j`.

## Remaining gate work

1. Default MSVC: passed outside the sandbox (orchestrator). The full Ninja clang-cl gate still needs a working host; NMake counts cannot close the compiler gate.
2. Lane 2 owns the linkage mismatch; H dependency/build owns the SDL builtin conflict; lane 2 and the harness owner route glstate_guard_harness's missing mc2gl::gos_InvalidateRenderStateCache symbol (referenced at tools/glstate_guard_harness/glstate_guard_harness.cpp:506). Subsequent fixes belong to X1 recipe batch 2, outside H1.
3. Lane 1 owns the three clang-cl allocation-accounting runtime failures; establish their cause and restore the proof without changing assertions merely to fit the compiler.
4. Regenerate caller/background inventories at integration as ruled; do not loosen checks or silently omit product/editor targets.
5. Future X1 gates still require both compiler test tiers, deployed tier1/editor/video/mod proofs and authoritative sim/save parity; none is claimed here.
