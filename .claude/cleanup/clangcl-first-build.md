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

## H2 triage — 2026-10-14 (clangcl-h2, base 92465c33b)

**Historical totals remain 3 errors, 21,503 warnings, 3,448 unique locations.** They describe the incomplete H1 NMake build, not the current migrated corpus and not three current compiler errors. The 3 allocation-probe runtime failures are separate again. H2 changes only the CAPTURE statement and this report; no build plumbing, allocation assertion, baseline, warning policy or generated inventory changes. **Correction reported to Fable/lane 1:** the requested single-physical-line `CAPTURE(path); CAPTURE(strict);` fails on Clang 23 because this doctest selects `__LINE__` for Clang >=22 (`doctest.h:556–558`), redefining `DOCTEST_CAPTURE_186`. The minimal portable fix places `CAPTURE(path);` and `CAPTURE(strict);` on **separate physical lines 186–187** (one net added line). This is the only departure from the requested one-line form; the edit remains confined to the approved CAPTURE statement. The rejected same-line diagnostic is retained in `.scratch/h2/clang-ibl-same-line-rejected.compile.log`.

### Errors first: ownership and minimal patches

| Finding | Cause and minimal portable correction | Owner / status |
|---|---|---|
| Additional current source defect: `tests/unit/test_ibl_registry_contract_harness.cpp:186` | `CAPTURE(path, strict)` passes two macro arguments to the single-argument macro at `tests/unit/doctest.h:3765`. Separate captures on separate physical lines retain both values and their names without generated-name collision. | Lane 1; only the approved CAPTURE statement edited. Both-compiler proof recorded below. |
| Newly observed current compile error: `tests/unit/test_job_system_harness.cpp:283:19` (plus consequent `:283:5`) | `CAPTURE(code, trace.string(), body)` passes three arguments to the same one-argument macro. Use three captures on three physical lines; a same-line replacement would collide under Clang's `__LINE__` fallback. | Lane 1; outside H2 claim, untouched. Exact owner hunk below; the two diagnostics are one source defect. |
| Historical compile error: `GameOS/gameos/gos_static_prop_batcher_shadow.cpp:897:17` | A redundant block-scope `extern` inside the exported C-linkage feed conflicts with the existing namespace-scope declaration at `GameOS/include/gameos.hpp:2345`. Remove the redundant declaration and use the included canonical declaration. | Lane 2; proposed hunk below, owner file untouched. |
| Historical compile error: `3rdparty/include/SDL2/SDL_endian.h:40:1` | SDL's old Clang prefetch shim defines `_m_prefetch`, now a compiler builtin in Clang 23.1.3. Its homemade `__PRFCHWINTRIN_H` also hides the compiler's intrinsic header. Apply the shim only when Clang lacks that builtin; leave modern intrinsics to the compiler. | H dependency/build lane; outside H2 claim. Proposed hunk below, owner file untouched. |
| Historical link error: locationless `mc2gl::gos_InvalidateRenderStateCache` (old standalone reference line 506); current test definition `tests/unit/test_glstate_guard_harness.cpp:24`, local declaration `GameOS/gameos/gl_state_guard.h:416` | The guard's block-scope extern inside `mc2gl` resolves differently between compilers. The real hook and test stub are global (`GameOS/include/gameos.hpp:2326`). Declare the global hook before the namespace and explicitly call it with `::`; a test-stub-only namespace change breaks MSVC. This is a namespace/linkage error on x64, where `__stdcall` is not distinct. | Lane 2 API owner, lane 1 verifies corpus. Proposed current-header hunk below; do not repair or resurrect the retired standalone harness. |

Exact owner hunks (proposals only):

```diff
--- a/tests/unit/test_job_system_harness.cpp
+++ b/tests/unit/test_job_system_harness.cpp
@@ -283,1 +283,3 @@
-    CAPTURE(code, trace.string(), body);
+    CAPTURE(code);
+    CAPTURE(trace.string());
+    CAPTURE(body);
```

```diff
--- a/GameOS/gameos/gos_static_prop_batcher_shadow.cpp
+++ b/GameOS/gameos/gos_static_prop_batcher_shadow.cpp
@@ -897,1 +896,0 @@
-    extern void gos_GetTerrainLightDir(float*, float*, float*);
```

```diff
--- a/3rdparty/include/SDL2/SDL_endian.h
+++ b/3rdparty/include/SDL2/SDL_endian.h
@@ -35,3 +35,4 @@
    so we define the needed '_m_prefetch' here as a pseudo-header, until the issue is fixed. */
 #ifdef __clang__
+#if !__has_builtin(_m_prefetch)
 #ifndef __PRFCHWINTRIN_H
@@ -44,2 +45,3 @@
 #endif /* __PRFCHWINTRIN_H */
+#endif /* !__has_builtin(_m_prefetch) */
 #endif /* __clang__ */
```

The nested test stays inside `#ifdef __clang__`, so MSVC need not parse `__has_builtin`. This is a temporary vendor compatibility patch for the dependency owner to review against its supported compiler floor; upgrading SDL to a version that removes the obsolete shim is the preferred dependency brief. No global compiler flag or builtin disabling is proposed.

```diff
--- a/GameOS/gameos/gl_state_guard.h
+++ b/GameOS/gameos/gl_state_guard.h
@@ -25,1 +25,3 @@
+void __stdcall gos_InvalidateRenderStateCache();
+
 namespace mc2gl {
@@ -415,4 +417,3 @@
     ~GlScopedRenderStateInvalidate() {
-        extern void __stdcall gos_InvalidateRenderStateCache();
-        gos_InvalidateRenderStateCache();
+        ::gos_InvalidateRenderStateCache();
     }
```

The global declaration matches `GameOS/include/gameos.hpp:2326` and the production definition. Isolated compile/link probes using a scratch copy of the real guard header pass on Clang and MSVC with this hunk. Unmodified header + global stub: Clang link fails, MSVC links; moving only the stub into `mc2gl`: Clang links, MSVC link fails. Therefore the lane-2 API hunk, not a test-only namespace change, is the portable route. No owner file was modified.

**Blocked targets are not extra errors.** H1 product/editor targets blocked by GameOS/imgui prerequisites have no successful compile/link claim. The present unit target also depends on `vfx_lifecycle_tests` (`tools/vfx_oracle/CMakeLists.txt:25`), so H1's 117-object standalone unit count does not describe H2. A fresh configure/build attempt, a successfully compiled TU, an emitted object and a linked target are different evidence levels. A timeout supplies no compiler verdict for untouched rules.

**Historical runtime only:** H1 clang-cl ran 2,837 cases: 2,834 passed, 3 failed, 3 skipped. The failures are `tests/unit/alloc_probe.cpp:18–23`, `:32/:36`, `:67`: allocation block/byte accounting, release/leak deltas and standard-owner allocation counts. Seven unexpected assertions plus the intentional should-fail assertion were reported. Lane 1 must establish the cause (optimization/elision is still only a hypothesis) and preserve allocation assertions and baselines. H2 does not relabel a build blockage as a new runtime result.

### Historical top ten, in occurrence order

Counts and sample coordinates below come from retained H1 `diagnostics.json`, checked against this checkout; they are not a new complete census. Estimates count warning-bearing files, plus explicitly named generator/type files where necessary; owners must inventory callers before any API or lifetime redesign. The complete per-class file lists and selected raw diagnostics are retained in `.scratch/h2/samples.json`. No suppression is applied in H2.

1. **`-Wmissing-field-initializers`: 13,720 occurrences / 980 locations / 1 generated file.** Verdict: generated-code noise for the sampled ordinary Bool rows: omitted `IndexEntry::parent` value-initializes an empty `string_view`, exactly the non-inheriting state. Owner: lane 2 (Gates generator). Brief: **“H2-W01: Emit complete gate index aggregates”**, estimate 3 files (generator, type/generator test, generated output; 1 warning-bearing file). Prefer generating the explicit empty parent, preserving inherited Tri entries and generator verification. If temporary suppression is needed, the generator may emit a Clang diagnostic push/ignored `-Wmissing-field-initializers`/pop around **only `kIndex`'s initializer in `Gates/gate_index.gen.h`**. Do not suppress all Gates targets or hand-edit generated rows. Samples: `Gates/gate_index.gen.h:16:55`, `:17:54`, `:18:60` — each **no runtime defect**: Bool entries default the unused parent to empty.

2. **`-Winconsistent-missing-override`: 3,369 / 83 / 2 files.** Verdict: modernization; the compiler has already verified that these declarations override. Owner: lane 1. Brief: **“H2-W02: Make appearance and artillery overrides explicit”**, estimate 2 headers (`code/artlry.h`, `mclib/bdactor.h`). Add `override` only to confirmed overrides; do not alter signatures, defaults, dispatch or ownership under this brief. No suppression proposed. Samples: `code/artlry.h:225:16` (`init(bool)`), `mclib/bdactor.h:353:16` (`init`), `mclib/bdactor.h:355:29` (`getAppearanceType`) — each **no demonstrated behavior defect**, missing intent/compiler checking only.

3. **`-Wformat-truncation`: 1,755 / 1,755 / 1 generated include.** Verdict: intentional test noise, not a reason to enlarge buffers. Owner: lane 1. Brief: **“H2-W03: Scope intentional printf truncation diagnostics”**, estimate 1–2 files (`tests/unit/test_printf_corpus.cpp`, optionally its generator). The corpus deliberately compares return lengths and eight-byte output from `snprintf` against the new formatter. Proposed suppression: Clang push/ignored `-Wformat-truncation`/pop **immediately around the inclusion of `printf_corpus_gen.inc` in that single test TU**, preserving all CHECKs and buffer sizes. No target-wide flag. Samples: `tests/unit/printf_corpus_gen.inc:28:18`, `:44:18`, `:60:18` — each **no defect**, intentional alpha/blue/green-mask truncation checks with matching return/content assertions.

4. **`-Woverloaded-virtual`: 1,075 / 35 / 22 files.** Verdict: real-bug candidate class mixed with intentional overloads; dispatch/signature audit comes before edits. Owner: lane 1. Brief: **“H2-W04: Audit hidden virtual overloads and dispatch contracts”**, estimate 22 warning-bearing headers plus callers/tests to be bounded by the owner. No suppression and no blanket `using Base::method`: importing overloads can change overload resolution. Samples: `code/artlry.h:227:16` — **no defect established**, type-ID initialization deliberately differs from `GameObject::init(FitIniFile*)`; `code/bldng.h:217:16` and `code/gate.h:275:16` — **real interface mismatch**, float setters hide `TerrainObject::setDamage(long)` (`code/terrobj.h:212`), so calls through that base signature select a different slot. Player-visible impact is **unproven** until callers and intended terrain damage semantics are audited; do not change integer/float behavior mechanically.

5. **`-Wdeprecated-non-prototype`: 438 / 146 / 15 vendor C files.** Verdict: third-party modernization noise under the existing C language mode, with a genuine future C23 build incompatibility. Owner: H dependency/build. Brief: **“H2-W05: Refresh Assimp's bundled zlib C prototypes”**, estimate 15 diagnostic files plus dependency metadata. Prefer an upstream dependency update; do not impose the project's C++23 policy on legacy vendor C. Temporary suppression, if needed: compiler-ID-Clang **and C-language-only** `-Wno-deprecated-non-prototype` as a **PRIVATE option of the bundled `zlibstatic` target in `3rdparty/assimp/contrib/zlib/CMakeLists.txt`**, repeated only for that same vendor target in standalone configurations. No directory/global option. Samples: `3rdparty/assimp/contrib/zlib/adler32.c:63:15`, `compress.c:22:13`, `crc32.c:126:16` — each **no current runtime defect shown**, K&R definitions with parameter types supplied on following lines; each is a C23 migration blocker if compiled in that mode.

6. **`-Wimplicit-exception-spec-mismatch`: 294 / 1 / 1 file.** Verdict: real declaration-contract defect. Owner: lane 2, coordinated with lane 1's allocation probe owners. Brief: **“H2-W06: Match the platform deallocator exception contract”**, estimate 1–2 files (declaration and definition audit). `GameOS/include/gameos.hpp:147:6` omits `noexcept` only for `PLATFORM_WINDOWS`, disagreeing with the CRT declaration in `vcruntime_new.h:65`. Correct both declaration and implementation consistently or retire the redundant declaration as part of the memory API work; do not suppress. **Only one distinct location exists**, so only one sample is possible: `GameOS/include/gameos.hpp:147:6` — **yes, a declaration defect**, no demonstrated throw/crash and no evidence that it causes the allocation-accounting runtime failures.

7. **`-Wunused-but-set-variable`: 137 / 40 / 36 files.** Verdict: mixed modernization and real-bug leads; a dead local is not proof that its initializer is disposable. Owners: lane 1 (mclib), lane 2 (GameOS). Brief: **“H2-W07: Audit dead stores and exhausted resource paths”**, estimate 36 warning-bearing files, split by owner before implementation. Preserve side effects and determine whether assertions compile out; use `[[maybe_unused]]` only for intentionally debug-only evidence, not as a class-wide cure. No suppression proposed. Samples: `GameOS/gameos/gameos_graphics_font.cpp:189:14` — **no defect established by this warning**, length is consumed only by compiled-out `gosASSERT`; retain the `S_snprintf` call and assess release rejection independently. `mclib/abldbug_print.cpp:205:10` — **no defect**, `cp` is incremented then ignored, while tokenization explicitly starts at `subscriptString + 1`. `mclib/ablenv.cpp:306:7` — **yes, adjacent real bounds defect found by inspection**: `fileHandle` is unused, and when all `MAX_USER_FILES` slots are in use the loop leaves `i == MAX_USER_FILES` and returns `files[i]` out of bounds (`:313`). Both callers already accept a null result; lane 1 should return null on exhaustion with the required diagnostic and test the full-table case. This is outside H2's claim.

8. **`-Wformat`: 107 / 61 / 15 files.** Verdict: real printf variadic contract defects, even where Windows gives long and int the same width. Owner: lane 1 for the 14 mclib files; H tool/build owner for `tools/mech_import_harness/mech_import_harness.cpp` (complete list in samples.json). Brief: **“H2-W08: Repair typed format contracts without output drift”**, estimate 15 warning-bearing files, split by owner. Prefer the existing typed formatting migration; minimal legacy fixes use specifiers matching the actual argument type, not casts hiding truncation. No suppression proposed. Samples: `mclib/cmponent.cpp:311:21` — **yes**, `%d` receives long `resourcePoints`; `mclib/color.cpp:60:32` — **yes**, `%d` receives loop index long; `mclib/ffile.cpp:560:146` — **yes**, `%u` receives DWORD/unsigned long. Current LLP64 width can mask symptoms; LP64 makes the long cases more serious. `%p` zero-padding elsewhere in this class also needs semantic review.

9. **`-Wnontrivial-memcall`: 98 / 19 / 11 files.** Verdict: real object-lifetime defects mixed with representation-preserving legacy math copies. Owner: lane 1 for the 10 mclib files; H tool/build owner for `tools/tg_import_dump/stubs.cpp`. Brief: **“H2-W09: Replace unconstructed storage and nontrivial byte operations”**, estimate 11 warning-bearing files plus affected type/allocator tests; preserve the sim's initialization bytes and checksums. No cast-to-void suppression, no mass `memset` deletion, no unproved substitution of `{}` for legacy zeroed storage. Samples: `mclib/assimp_importer.cpp:448:9` — **yes, lifetime defect**: raw TG vertex storage is zeroed without constructing its non-implicit-lifetime Stuff members (`Point3D` and `Vector3D` have user-provided default/copy constructors at `stuff/point3d.hpp:33/:42`, `stuff/vector3d.hpp:126/:137`); implicit creation of the aggregate does not start those member lifetimes. No observed model corruption is claimed. `mclib/cmponent.cpp:642:9` — **yes, lifetime defect**, raw storage for non-implicit-lifetime `MasterComponent` (user constructor/destructor) is used without object construction; the deliberate full zeroing prevents known uninitialized `disableLevel` behavior and must be preserved by any redesign. `mclib/mlr/mlrprimitivebase.hpp:251:25` — **no concrete behavior defect shown**, the copied `Vector2DOf<float>` currently contains only `x/y`, but its user copy constructor makes raw copying a fragile contract; use typed copying in its owner brief and retain bounds checks.

10. **`-Wnonportable-include-path`: 86 / 5 / 5 files.** Verdict: real portability defects hidden by Windows case folding. Owners: lane 2 (three GameOS files), lane 1 (`mclib/cliff_decal_tuning.cpp`), H platform/bridge lane proposed for `GameAdapters/VegetationAdapter.cpp` (Fable assigns that shared boundary; no new H2 claim). Brief: **“H2-W10: Match Stuff include spelling to tracked paths”**, estimate 5 files. Use tracked `mclib/stuff/stuff.hpp` spelling, with no header rename or case-insensitive lookup workaround; no suppression. Samples: `GameAdapters/VegetationAdapter.cpp:23:10`, `GameOS/gameos/gameos_graphics_material_internal.h:73:10`, `GameOS/gameos/gos_mech_batcher.h:8:10` — each **yes, case-sensitive build defect**, includes `Stuff/Stuff.hpp` while git tracks `stuff/stuff.hpp`. No claim of a successful Linux build follows from fixing these five includes.

### H2 retained proof and continuation

Fresh compiler probes, timeout outcomes, compile/link coverage, exact commands and portable-line validation are recorded below. Comprehensive recollection remains conditional on a working Ninja host; the historical top ten above is the bounded fallback required by the four-hour cut.

The original Ninja unit configure reached Clang's CXX ABI check and exceeded its 90 s deadline. The first scratch watchdog's `taskkill /T` cleanup was denied by the sandbox and its subsequent 30 s wait expired. Recovery enumerated and stopped only descendants of the two recorded probe PIDs (14780 and 74832) with psutil. The all-target Ninja configure was intentionally stopped after 68.06 s at the C ABI check during that recovery (observed termination code 15, **not a compiler failure**). The scratch watchdog now uses owned psutil descendants, records `status=timeout` with `native_exit=null`, and was independently checked against both a 1 s deadline and a child exiting 7. These are scratch proof changes, not build-driver changes. Both original stalled probes emitted ABI objects but no project objects and reached no project compile/link verdict.

Portable probes use `/MD /EHsc /O2 /Zi /DNDEBUG` plus Clang C++23 or MSVC `/std:c++23preview`, unique PDB endpoints, isolated objects and retained complete compiler commands. **Both compilers compile the final two-line IBL fix (native exit 0).** Original comma form: Clang exit 1 (macro arity plus consequent undeclared identifier), MSVC exit 0 with C4002 (it discards the second macro argument); this is one source defect, not two separate root causes. Same-line separate captures: Clang exit 1 (`DOCTEST_CAPTURE_186` redefinition), MSVC compiles. The SDL proposal compiles under both compilers. The explicit-global guard proposal links under both; none of these isolated proofs implies that the entire unit executable links or runs.

Root error replay reads the retained H1 compile commands, substitutes this worktree, selects the PCH-free editor GameOS TU and writes only isolated scratch outputs. Both current TUs reproduce the historical compile errors: shadow at `:897:17`, SDL at `:40:1`, native exit 1 each. The replay removes imgui's pre-existing `/W0` for observation and applies no warning suppression. These selected TUs are **reproductions**, excluded from fresh-census totals.

Outside-sandbox continuation (from **this worktree**, after the orchestrator reviews/applies owner patches; no recursive cleanup or index operation needed):

```powershell
py -3 .scratch/h2/watch.py --seconds 1800 --name outside-unit -- py -3 .scratch/h2/fresh_host.py --tag outside-unit-20261014 --unit-only --run
py -3 .scratch/h2/watch.py --seconds 10800 --name outside-census -- py -3 .scratch/h2/fresh_host.py --tag outside-census-20261014
```

Run serially. Choose a new tag/name if rerunning: `fresh_host.py` refuses existing trees, so an incremental no-op cannot count as recollection. It invokes the same preset/helper commands as the builders for root `all ui_editor`, all three standalones, and the current shared unit/VFX dependency; logs live in `.scratch/h2/<tag>/logs`. Each cache receives compiler verification. The unit run uses the real `build_contract_harnesses.build_and_run_unit_tests` entry point and only its selected tree's executable. Exact expanded configure/build commands are the first lines of the phase logs. Each invocation sets a unique `_MSPDBSRV_ENDPOINT_` before building. Timeout JSON is distinct from native configure/compiler/link/runtime failure; the watchdog stops only its own process tree. Do not compare fresh Ninja counts to H1's blocked NMake totals as if coverage matched.

Standard builder commands reproduced in H2 (the scratch watchdog supplies the bound and unique endpoint):

```powershell
py -3 .scratch/h2/watch.py --seconds 90 --name unit-ninja -- py -3 tools/build_contract_harnesses.py --preset clang-cl-dev --unit-only --keep-going --log-dir .scratch/h2/unit-ninja-stages
py -3 .scratch/h2/watch.py --seconds 90 --name all-ninja -- py -3 tools/build_all_targets.py --preset clang-cl-dev --reconfigure --keep-going --log-dir .scratch/h2/all-ninja-stages
py -3 .scratch/h2/watch.py --seconds 600 --name unit-nmake -- py -3 tools/build_contract_harnesses.py --preset clang-cl-nmake --unit-only --keep-going --log-dir .scratch/h2/unit-nmake-stages
py -3 .scratch/h2/watch.py --seconds 240 --name portable-final -- py -3 .scratch/h2/portable_probes.py
py -3 .scratch/h2/watch.py --seconds 240 --name root-replay -- py -3 .scratch/h2/replay_errors.py
py -3 .scratch/h2/watch.py --seconds 120 --name guard-global -- py -3 .scratch/h2/guard_proposal.py
py -3 .scratch/h2/watch.py --seconds 1800 --name all-nmake -- py -3 tools/build_all_targets.py --preset clang-cl-nmake --reconfigure --keep-going --log-dir .scratch/h2/all-nmake-stages
```

`py -3 .scratch/h2/pytest_presets.py` runs exactly `scripts/tests/test_build_compiler_presets.py -q --capture=sys -p no:tmpdir -p no:cacheprovider` with `tempfile.tempdir` set to `.scratch/h2/tmp`: **32 passed**. It edits no test file. No build plumbing was changed, so the out-of-claim suite needed no edit. `py -3 .scratch/h2/samples.py` retained the historical class/file/sample inventory. `py -3 .scratch/h2/summarize.py` retains current stage diagnostics and object coverage separately; its totals are explicitly partial, including interrupted-attempt occurrences, not a replacement for H1. Initial unit NMake attempt: watchdog timeout at 600.63 s, 131/131 VFX objects, 2/2 gate objects, 96/144 unit objects, VFX executable linked; unit executable not linked. No runtime result was obtained from that attempt.

Additional scratch proof commands: `py -3 .scratch/h2/watch.py --seconds 1 --name watchdog-timeout-proof -- py -3 -c "import time; time.sleep(30)"` records a timeout with no native exit; `py -3 .scratch/h2/watch.py --seconds 5 --name watchdog-exit-proof -- py -3 -c "import sys; sys.exit(7)"` records completed/native 7. The initial `--name portable` probe used the same portable matrix command before the same-line correction; per-child logs and the final matrix distinguish expected negative cases from successful fixes. `py -3 .scratch/h2/fresh_host.py --help` parses; `py -3 .scratch/h2/fresh_host.py --tag tmp` refuses the existing directory before invoking a compiler. `py -3 scripts/check-cxx-standard-floor.py` passes (9 standalone projects, 5 gate readers). `py -3 .scratch/h2/qa.py` passes original-H1 preservation, LF endings, exact CAPTURE-only source scope, tracked claim boundaries and `git apply --check --unidiff-zero` of the extracted owner patches; `git diff --check` passes.

Not run here: Git Bash contract board (forbidden), successful full Ninja compiler gate (host-blocked), full MSVC corpus build/run (the scoped CAPTURE TU and owner proposals were verified on both compilers), game/editor deployment or smokes (orchestrator commit/fingerprinted lane3 proof checkpoint). Allocation assertions and generated baselines/inventories remain untouched. No forbidden checkout or install was opened, built or edited.

### H2 final bounded outcome and retained artifacts

**Status: partial — scoped fix and historical top-ten triage complete; comprehensive fresh Ninja recollection remains host-blocked.** The NMake all-target attempt **completed** in 1,614.92 s with **native exit 1**, within its 1,800 s watchdog. Root and unit corpus failed; all three standalone tools built and linked. The unit continuation emitted the real `test_ibl_registry_contract_harness.cpp.obj` with the approved fix, then stopped on the newly discovered, unclaimed job-harness CAPTURE error. No full unit link or runtime was reached; later uncompiled corpus files, including the GL guard, remain blocked. The GL contract's link diagnosis comes from the isolated real-header probe, not a fabricated current full-unit linker outcome.

| Current project / target | Requested C/C++ compile rules | Objects emitted | Link / blockage |
|---|---:|---:|---|
| root: `all ui_editor` | 1,549 | 348 | GameOS linkage + SDL builtin errors; product/editor dependent targets blocked |
| `tg_import_dump` | 202 | 202 | executable linked |
| `gate_dump` | 3 | 3 | executable linked |
| `mech_import_harness` | 126 | 126 | executable linked |
| unit dependency `mc2_gate` | 2 | 2 | library linked |
| unit dependency `vfx_lifecycle_tests` | 131 | 131 | executable linked |
| unit `mc2_tests` | 144 | 126 | compile stops at `test_job_system_harness.cpp:283`; 18 un-emitted rules, no unit link |

Requested-rule sets are derived from generated `CMakeFiles/Makefile2` dependency reachability, not just the configured source inventory. Root configures 1,555 rules: `text_tool` (1), `editor_document_asan` (4) and `editor_document_tests_asan` (1) are outside `all ui_editor`. The unit project configures 282 rules: `test_render_contract_3` (4) and `mc2_modlint` (1) are outside the selected `mc2_tests` dependency closure. Compiler ID/ABI scratch outputs are excluded. Per-target counts, including zero-emission requests, are in `census-partial.json`.

The retained **partial attempt** dataset contains **4 compiler error lines, 26,386 warning occurrences, 3,530 unique file/line/column locations**: two root error causes, plus macro arity and consequent undeclared-identifier diagnostics for the **one** job CAPTURE defect. This includes the interrupted initial unit log and its continuation (including a retried TU), so it is **not a complete new clean census** and is not directly comparable to H1. The original 3 / 21,503 / 3,448 measurement and its ten-class order above remain intact. Raw stage logs retain compiler notes, excerpts, CMake/NMake messages and exact commands; isolated before/after probes are excluded from these totals.

The additional owner proposal was checked with `py -3 .scratch/h2/watch.py --seconds 120 --name job-proposal -- py -3 .scratch/h2/job_proposal.py`: the scratch copy with **three separate physical CAPTURE lines** compiles under both compilers (native 0 each). A source scan finds no other remaining comma-bearing CAPTURE invocation in unit `.cpp` files. No job source was edited. Lane 1 also owns the source-proven full-file-table overrun at `mclib/ablenv.cpp:313`; route its fix and any required BUGS ledger entry there, outside this claim.

Final proof index: `.scratch/h2/{all-nmake.json,unit-nmake.json,portable-final.log,root-replay.log,guard-global.log,job-proposal.log,census-partial.json,diagnostics-partial.json,samples.json,owner-hunks.patch,reviewed.patch,evidence-sha256.json,scratch-files.txt}`. Complete expanded commands and child exits are retained with the probes. `py -3 .scratch/h2/archive.py` writes the reviewed diff, log hashes, scratch inventory and portable evidence archive, including H1's original census/parser. The archive is `E:/mc2-archive-2026-10/clangcl-h2-2026-10-14-lane4.tar.gz`, with a `.sha256` sidecar. Archive contents and hash are checked after creation.

Scratch retained, **none deleted**: `.scratch/h2/`; `build-clangcl-tests/`, `build-clangcl-alltargets/`, `build-clangcl-nmake-{alltargets,tests,tgdump,gate-dump,harness}/`; the archive and checksum sidecar above. Every individual retained scratch/build file is listed in `.scratch/h2/scratch-files.txt`. Existing unrelated `.scratch` content and earlier build trees were left alone. Only this report and the approved IBL CAPTURE statement belong to the proposed commit; the orchestrator owns staging, committing and the later lane3 runtime checkpoint.
