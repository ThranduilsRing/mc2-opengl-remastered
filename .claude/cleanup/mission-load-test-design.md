<!-- doc-status: current | reviewed:2026-10-11 -->
# Offline mission-load test — design (2026-10-09; full headless Mission::init NO-GO, staged data+ABL-compile target GO ≈ 2 sessions, modest payoff)

Recon only. Nothing was edited, built or written to git. I did run `llvm-nm` (read-only) on `A:/Games/mc2-main/build64` objects. They are from the existing build, which may lag HEAD d628e1374.

## 0. Verdict

- **Running `Mission::init` headless over 24 missions: NO-GO** within about 2 builder sessions. Section 2 gives the evidence.
- **A staged, narrower target is GO in about 2 sessions.** It loads each mission's data through the real file, FIT and PAK loaders. It also compiles every mission's real ABL scripts with the real ABL front end. It runs in seconds, with no window, GL or VK.
- **The premise is weaker than the review states.** The 89 surviving smoke reports (`tests/smoke/artifacts/*/report.json`, 2026-10-05 to 10-09) have 298 mission rows. 290 passed. All 8 failures were `heartbeat_freeze_play` (7) or `timeout` (1), on mc2_01, mc2_10 and mc2_17. There were no `crash_no_summary` or load-phase failures.
- The last 188 commits (since 08-10) contain no mission-data crash fix by keyword.
- `crash_silent` is documented as environmental. The `crash_no_summary` bucket cannot be sized from current artifacts, since older ones were purged.
- Real load-crash classes from memory and git are:
  - tacmap short read (8b4e143e)
  - ABL compile fatals (ASAN-3-ABL-COMPILE-FATAL-1)
  - the mod ABL VM crash (OMNITECH-ABL-EXECEXPRESSION-1)
  - the missing stock `jaegermech.tgl` in mc2_24
- The proposed target covers the data and ABL classes. It does **not** catch init-order or GL-state bugs such as the `Mission::load` versus `Mission::init` mirror class. Those are GPU-subsystem inits and cannot run headless.

## 1. Existing headless entry points

| Asset | What it does | Loads a mission? |
|---|---|---|
| `tests/unit` → `mc2_tests` (doctest) | Links real `mclib/{file,inifile,packet,ffile,fastfile,lzdecomp,lzcomp,err,csvfile,paths}.cpp` + `GameOS/src/platform_str.cpp` + zlib. `heap_shim.cpp` supplies `UserHeap` (malloc-backed), `systemHeap`, `Environment={}`, `InternalFunctionStop/Pause`, `crashbundle_*`, `EnterWindowMode` and similar stubs. | No. It covers FST round-trip, ini and csv parsing, and path ladders on synthetic fixtures. |
| `tools/*_harness` (objmgr, logistics_csv, terrain_daylight, brain_dispatch, …) | Header-only leaf logic, or `brain_dispatch_harness` with hand-written stub headers that shadow engine headers. | No. objmgr_contract_harness links only `objmgr_watch_policy.h`, not `objmgr.cpp`. |
| `mclib_standalone` (`MC2_BUILD_ENGINE_STANDALONE`, default OFF) | A link-time boundary instrument. `main()` returns 0. It links `/WHOLEARCHIVE` over mclib, gosfx, mlr, stuff, **gameos (full GL)** and windows. | No, and it cannot be a base. It proves only "no `code/` leaks". The GL gameos is linked wholesale. |
| `HostServices/` | Four interfaces: `IFileSystem`, `ILogger`, `IClock`, `IConfig`. Pure contract. | No. There is no texture or GPU seam, and the engine does not consume it for load. |
| `tests/smoke/brain_task_queue_test.cpp` | A one-header `cl` unit test of `BrainTaskQueue`. | No. |
| `gameos_editor` | `gameos` minus `gos_render.cpp`, still GL. | The editor loads missions through its own forked path, with a real GL context. |

No null renderer or texture-manager stub exists. `MC2_HEADLESS` and similar names are not found in code.

Wiring note: `scripts/check-contracts.sh` is a source-only board. It never builds, and cmake or exe rows are classed as serial. The C++ test tier is `scripts/run-unit-tests.sh` (`build64-tests`), policed by `check-test-tier-coverage.py`. A new target belongs there, not as a build-and-run row in `check-contracts.sh`.

## 2. Mission-load call chain and the cut

| Step | Class |
|---|---|
| `Mission::init` (`code/mission.cpp:2903-5234`, 2300 lines): `initABL`, `gamesys.fit`, `WeaponRanges`, `Team::init`, `SensorManager`, `craterManager`, then `ObjectManager->init` | mostly data, but `GpuMechBatcher::onMapLoad`, `StaticProp::beginMission*` and `Mech::beginMission` come first and are GL/VK-coupled |
| FST open, `.fit` via `FitIniFile`, `.pak` via `PacketFile` | **pure data** (already linkable in `mc2_tests`) |
| `Terrain::init` / `load` / `primeMissionTerrainCache` (`mclib/terrain.cpp` 638-1830) | **GL-coupled inline**: ~20 `gos_TerrainLodChunk_Upload*`, `gos_uploadTerrainHeightTex`, `gos_SetMapHalfExtent`, `BuildDenseRecipe`, `gos_terrain_surface::GenerateForMission`, `mask_dispatch::Init` |
| `ObjectManager->loadObjectType` / `loadTerrainObjects` / `registerStaticPropsForMissionLoad` | game class (`code/`), pulls TG shapes plus `mcTextureManager` |
| `mcTextureManager::loadTexture` (`mclib/txmmgr_texture.cpp:165`) | CPU only (LZ cache). GPU handle creation is deferred to `get_gosTextureHandle` (~5856). But the manager's init and ctor call `gos_CreateBuffer` and `gos_LightDataSsbo_Upload`. |
| ABL: libs `orders/miscfunc/corebrain/mc2xcore.abx`, scenario `.abl`, warrior brains | **ABL core is pure data** (see 3) |
| `weather->init`, `eye->init`, `MissionInterfaceManager` (GUI), sound, logistics | GL/GUI/SDL-coupled. Never include these. |

**Measured link closure.** I ran an `llvm-nm` simulated link over the `build64` objects.
- The transitive closure of `terrain.obj`, `txmmgr.obj`, `objmgr.obj` or `mission.obj` is identical: the whole game.
  - 104 `code/` TUs, 97 mclib, 51 mlr, 38 stuff, 18 gosfx, 10 particles, plus gui, gameadapters and others.
- `Mission` lives in `code/`, so no engine library can hold a `Mission::init`-equivalent.
- The non-gameos world imports **703 distinct symbols from 60 gameos TUs**. Top importers:
  - `gameos_graphics` 161
  - `gos_mech_batcher` 78
  - `gos_static_prop_batcher` 48
  - `gos_terrain_indirect` 45
  - `gameos` 44
  - `gos_static_prop_registry` 27
  - particle bridge 21
  - postprocess 21
  - about 45 more TUs with 18 or fewer each
- Roughly 450-500 of those are GL-bound class methods and globals (batchers, terrain streams, FBO state). A null backend would be 500 hand-written stubs. Even then `Terrain::init` and the batchers would hit unmodelled GL state.
- There is no intermediate "terrain and objects only" stage, because those roots pull the same closure.
- The "null GL" shim idea (a no-op `wglGetProcAddress`) is unsound. Shader compile and link status would fail, and those paths are fatal.

## 3. Staged target: `mission_load_tests`

A new doctest exe at `tests/mission_load/`. It is separate from `mc2_tests`, so ABL's large static state stays out of that suite and the doctest count-lock census.

**ABL core is a clean cut.** The 14 `mclib/abl*.cpp` TUs have a closure of exactly those 14. Their only non-CRT unresolved symbols are:
- `S_strlwr` and `S_snprintf`: already in `platform_str.cpp` in `mc2_tests`.
- `crashbundle_append`: already shimmed.
- `mc2AblNotePeakDepth(int)`: a one-line stub (the real one is `code/warrior.cpp:20`).
- No `gos_*` symbols at all.
- ABL source I/O goes through `ABLFile::openCB` etc. callbacks, so the test supplies `File`-backed callbacks.

**Builtin table.** The 290 `ABLi_addFunction("name", isOrder, "params", "ret", execFn)` lines in `code/ablmc2.cpp:8003+` are one-liners.
- Compile resolution needs only name, signature and a dummy function pointer, not the real `exec*` functions.
- The standard routines in `ablxstd.cpp` register themselves.
- Generate `tests/mission_load/abl_builtins.inc` with `scripts/gen-abl-builtin-table.py` by scraping uncommented lines.
- Add `--check`, run as a `check-contracts` row, to catch drift or a count mismatch. It is cheap and source-only.
- Do not move the registrations. Giant-file split plan item #1 notes that registration order is state (`NumStandardFunctions++`, fixed table). After that split, re-point the generator at the table file.

**Data.**
- The 24 `mc2_01..24` stems live in `mission.fst`: 1346 members, 276 `.fit`, 44 `.pak`, 955 flat `data/missions/*.abl`, plus `.abx` and `.abi`. The ladder is mod → loose → FST (`file.cpp`), per `stock_missions.py`.
- The wrapper `scripts/check-mission-load.py` finds the FST with `stock_missions.find_deployed_fst()`.
  - Roots: `A:/Games/mc2-opengl/mc2-win64-v0.4` and `mc2-win64-v0.4c`.
  - It verifies the md5 digests against `stock_missions.md5`, then runs the exe with `cwd` set to a fresh empty temp dir (no loose shadowing) and `--fst <abs path>`.
  - Read-only against the install; the test writes nothing.
  - If no FST is found it **fails loudly** (a `--allow-missing` flag is opt-in). This repo treats silent skips as defects.
- `run_smoke.py` has no default exe and uses deploy leases. The test needs only the FST, not an exe.

**Per mission** (mirrors `mission.cpp:3790-3880`):
1. `FastFileInit(mission.fst)`.
2. `FitIniFile::open` of `data/missions/mc2_NN.fit`, enumerating every block.
3. Open `mc2_NN.pak` as a `PacketFile` and `readPacket` every packet (exercises LZ and zlib decompression and table integrity).
4. ABL: `ABLi_init` with the generated table.
5. `ABLi_loadLibrary` for `orders.abx`, `miscfunc.abx` and `corebrain.abx` if present.
6. `ABLi_preProcess` of `[Script] ScenarioScript`.
7. `ABLi_preProcess` of each warrior `Brain` named in the `.fit`.
8. Assert `numErrors == 0`, then `ABLi_close`, `FastFileFini` and registry reset.

**Seams and globals.**
- Seams are already free: the `File` ladder, the `ABLFile` callbacks and the ABL init callbacks are function pointers, so nothing needs to become virtual.
- Re-init per mission: `ABLi_close` and `ABLi_init`, as `closeABL` and `initABL` do in the game. Also `FastFileFini` and the cwd/`missionPath` globals.
- Install an `ablFatalCallback` that throws or records, so a fatal becomes a doctest failure with the message.
- If ABL statics leak between missions, the wrapper falls back to one process per mission via `--test-case=`.
- Do not link `txmmgr`, `terrain`, `objmgr`, `Mission`, `Logistics`, GUI or sound.

**Time budget.** This is data and compile work, with no GPU. I estimate 24 missions in under 10 s (unmeasured, inside the 60 s ceiling). Smoke takes about 4.8 s of load per mission plus about 2 s of engine init, plus the play window.

## 4. Risks

- **Hidden GL in loaders** is the reason for the cut. `Terrain::init` uploads directly, and `mcTextureManager`'s ctor creates buffers. None of that is attempted here.
- **Static singletons.** ABL has module registry (max 1024), symbol/code heaps and `level`/`sourceFile` statics. Per-mission `ABLi_close`/`init` mitigates this. A fallback to process-per-mission is documented in section 3.
- **File writes.** None expected. The ABL debugger callbacks and file CBs are not hooked. The cwd is empty, so any stray write lands in scratch.
- **Stock data may not compile cleanly.** If some stock `.abl` errors under compile-only, that is a finding. Pin those in a ledger, `tests/mission_load/known_abl_errors.json`, matching the repo's KNOWN-row discipline. A ledger entry that matches nothing is itself a failure.
- **Compile-flag drift.** `heap.cpp` is known not to compile standalone. `mc2_tests` builds at C++20 with `LINUX_BUILD`, `PLATFORM_WINDOWS`, `USE_ASSEMBLER_CODE=0`. The ABL TUs are light (libc and `ablXXX.h`). Slice 1 is the proof.
- **Compile-only does not run the ABL VM.** The OMNITECH exec-time crash class stays in smoke.
- **ABL `#include`/library paths** relative to `data/missions/` are unverified. Slice 2's first run reveals them.
- **Coupling to `ablmc2.cpp`.** If the giant-file split moves the registration table, the generator regex or path breaks. Keep the `--check` row so that breaks loudly, and sequence the generator re-point right after the split.

## 5. Slices (one build cycle each)

- **S4.1, data load (about 0.5 session).**
  - Add `tests/mission_load/{CMakeLists.txt,mission_load_main.cpp,test_mission_data.cpp}`, reusing `../unit/heap_shim.cpp` and the same link frontier as `mc2_tests`.
  - Add `scripts/check-mission-load.py` (FST resolve, md5, cwd isolation, no silent skip).
  - Wire into `run-unit-tests.sh` and `check-test-tier-coverage` (new `add_executable` must be named by a runner).
  - Exit: 24/24 stems pass FST+FIT+PAK, in under 5 s.
  - Negative control: a corrupted in-memory copy of one `.fit` or `.pak` fails.
  - Missing FST exits non-zero.
- **S4.2, ABL compile (about 1 session).**
  - Add the 14 `abl*.cpp` TUs, `abl_test_host.cpp` (callbacks, fatal capture, `mc2AblNotePeakDepth` stub) and the generated `abl_builtins.inc`.
  - Add `scripts/gen-abl-builtin-table.py` with `--check` and `--selftest`, plus a `check-contracts` row for the generator check only.
  - Exit: all 24 missions' libs, scenario and warrior brains compile with `numErrors == 0` (or a pinned ledger).
  - Negative control: an injected undefined identifier gives `numErrors > 0`.
  - A second pass compiles all 955 stock `.abl` as an inventory metric (library versus module handled).
  - Total runtime under 15 s.
  - The builtin-table check fails when a builtin is added or removed in `ablmc2.cpp` without regenerating.
- **S4.3, optional (about 0.5 session).**
  - Cross-reference check that every `[Parts]` object number resolves to a definition in the object/`tgl` tables.
  - This is the jaegermech class (mc2_24 `allow_asset_load_fail`).
  - It needs the object-table path knowledge. Check `tools/mc2data/stock_corpus.py` first, since a Python check may already decode this.
  - Exit: it reproduces the known mc2_24 gap as a pinned KNOWN row.

## 6. GO / NO-GO

- **S4.1 + S4.2: GO, about 1.5-2 builder sessions.** The unit link frontier already exists, and the ABL cut was measured and is tiny. The main uncertainty is the first compile run of the ABL TUs inside `mc2_tests`-style flags, plus any pre-existing stock ABL errors. Note the payoff is modest today, per the smoke data above. Its value is mostly mod-facing (ABL compile fatals, pak/FIT parse) and as a tripwire for the modernization program.
- **Full `Mission::init`-equivalent headless run: NO-GO.**
  - The engine's closure is the entire game. `Mission` is in `code/`, and 703 gameos symbols over 60 GL-bound TUs would have to be stubbed.
  - Stubbing the batchers and terrain streams would still not execute the GPU-state init-order logic that the memory notes name as the actual recurring load bugs.
  - If someone wants full-fidelity load coverage, the realistic route is a hidden-window `MC2_LOAD_ONLY=1` exit-at-`mission_ready` mode in the real exe. That needs a device and about 8-9 s per mission serially, about 3.5 minutes for 24, and it is a smoke-tier row, not an offline one.

Relevant files:
- `A:/Games/mc2-main/.claude/cleanup/{opus-modernization-review.md,offline-checks-plan.md}`
- `A:/Games/mc2-main/tests/unit/{CMakeLists.txt,heap_shim.cpp}`
- `A:/Games/mc2-main/code/mission.cpp` (2903-5234, 3790-3880)
- `A:/Games/mc2-main/code/ablmc2.cpp` (7945, 8003+)
- `A:/Games/mc2-main/mclib/{abl*.cpp,terrain.cpp,txmmgr.cpp}`
- `A:/Games/mc2-main/tools/mc2data/stock_missions.py`
- `A:/Games/mc2-main/scripts/{run-unit-tests.sh,check-test-tier-coverage.py}`
- `A:/Games/mc2-main/tools/engine_standalone_stub.cpp`
