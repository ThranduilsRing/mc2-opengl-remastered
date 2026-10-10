<!-- doc-status: current | reviewed:2026-10-10 -->
# Legacy subsystem audit (read-only)

Base: `A:/Games/mc2-main` HEAD `214e8c45e` (branch `claude/nifty-mendeleev`). The dead-asm merge `10f623597` landed mid-audit and the counts below are re-measured after it. Every file cited was clean in the worktree; the only dirty files were other agents' `.claude/cleanup/*.md`. TU membership comes from `build64-ninja/compile_commands.json` (1220 TUs, generated 09:53) cross-checked against the CMake lists, because that file predates the 10:39 `msl_*`/`mission_init_load` splits. Nothing was built, run or launched. `[unverified]` marks a claim that only a build or a run can settle.

Corrections to the brief:
- **`USE_GOS_HEAP` is DEFINED.** `mclib/heap.h:15-17` defines it under `LINUX_BUILD`. Every `#ifndef USE_GOS_HEAP` body in heap.cpp is dead: 13 regions, ~1.3–1.4k lines depending on how nesting is counted, including the x86 `_asm` at heap.cpp:59-86.
- **MLR is not a software rasteriser.** It transforms and clips gosFX geometry, then submits gos vertex batches to the GL renderer. It is still the default VFX draw path: `MC2_GPU_PARTICLES` defaults to false (`Gates/gate_table.cpp:437`), and `kDefaultDisabled=false` in `mclib/mlr/mlr_gate.cpp`.
- **glm is not vendored.** No include of it exists and there is no `vcpkg.json`, so C++23 step 5 (Stuff→glm) means adding a dependency.
- **Three math types coexist:** `Stuff::` (MC2 space), `GameOS/gameos/utils/vec.h` (`vec4`/`mat4`, render side), and raw `float[16]` plus the gpu_abi structs.
- **`HostServices/` is test-only.** `DefaultHostServices.cpp` is linked only by `tests/unit/CMakeLists.txt:105`, and there are 0 production users.
- **Some sources are invisible to a basename-in-CMake scout.** `code/logmain.cpp` and `code/debugging.cpp` are unbuilt, but their basenames appear in `CMakeLists.txt:570` and `mclib/CMakeLists.txt:10`. `scout-unbuilt.md` also excluded the glob-covered `mlr/`/`stuff/` dirs, which `tools/mc2fx/CMakeLists.txt:14-17` globs. The dead-asm series spent effort on 4 unbuilt MLR TUs: `10f623597` removed 70 lines each from `mlrprimitive.cpp` and three siblings.

## (A) Ranked opportunities

| # | Subsystem / files | Verdict | Superseded by / replacement | Callers (dir:count) | Contract notes | Tests | Net −LOC | Risk | Step |
|---|---|---|---|---|---|---|---|---|---|
| 1 | Unbuilt MLR TUs `mclib/mlr/{gospoint,mlrindexedpolymesh,mlrindexedprimitive,mlrloadobj,mlrpolymesh,mlrprimitive,mlrvertex}.cpp` + `mlrprimitive.hpp`,`mlrpolymesh.hpp`,`mlrvertex.hpp` | DELETE | nothing (v1/v2 MLRShape streams already `STOP` at `mlrshape.cpp:65`; v3 uses `MLRPrimitiveBase`) | 0 compiled. `mlrshape.cpp:66-` is inside a comment. mc2fx excludes them by name (`tools/mc2fx/CMakeLists.txt:19-27`, "include missing mlrstuff.hpp") | none; not in `mclib/mlr/CMakeLists.txt` | all-target build; `mlr_leaves_gated` unaffected | −6,449 | very low | precedes step 1 |
| 2 | Stuff self-tests, 25 `mclib/stuff/*_test.cpp` (+ `filestreammanager_test.cpp`, unbuilt) and the `TestClass()` decls in 30 Stuff headers | DELETE (port anything worth keeping into doctest) | `tests/unit` doctest | `TestClass` has 0 callers outside `*_test.cpp`. The files are compiled into lib `stuff` (`stuff/CMakeLists.txt:8-77`) | none | build; doctest 704 | −5,395 | very low | precedes 1 |
| 3 | Dead Stuff cluster: `database`, `notationfile/page/note`, `filestream`(+Directory)/`filestreammanager`, `safechain/safesocket`, `table`, `tree`, `slot`, `namelist`, `matrixstack`, `average`, `motion`, `polar`, `auto_container/auto_ptr/initialized_ptr` | DELETE | `Stuff::MemoryStream` + mclib `File` (the real `.fx` load path, `code/mechcmd2.cpp:1033-1044`) | outside stuff: `FileStream` 1 (`mlrlookuplight.cpp:236`), `Page` only in the MLR light ctors (no callers), the rest 0. Kept Stuff only needs `SortedChain` (via `Hash`, used by `gosimagepool.hpp:54`) | FileStream reads are already fatal: `gos_GetFile`/`gos_Find*` `STOP` (`gameos_fileio.cpp:153-202`) | build all + `ENABLE_MC2FX` mc2fx dump of `mc2.fx` identical; tier1 | ~−9,000 of 11,735 [unverified until build] | low | precedes 1 |
| 4 | `heap.cpp` `#ifndef USE_GOS_HEAP` bodies + `GameOS/include/imagehlp.h` (included only at heap.cpp:24, dead) | DELETE | live gos branch | n/a (preprocessor-dead) | as dead-asm series: `cl /EP` identical | build; doctest `heap_shim` | −1,335 to −1,402, plus 1,799 | very low | joins dead-asm series |
| 5 | `EWCharString` (`mclib/ewcharstring.*`) | DELETE | — | 0 (built: `mclib/CMakeLists.txt:163`) | `UNICODE` is defined only for assimp/zlib TUs | build | −2,177 | very low | precedes 1 |
| 6 | `ECharString` (`mclib/echarstring.*`) | CONSOLIDATE → `EString` | `EString` (char-only while `UNICODE` is off) | code:1 (`objective.cpp`), editor:7 | the two diff only in wide-char branches | build; tier1 (objectives text) | −2,163 | low | 0 / with 4 |
| 7 | `code/logmain.cpp` (813; includes non-existent `<gosscript/...>`), `code/debugging.{cpp,h}` (176; dup of `mclib/debugging`, same guard `DEBUGGING_H`), `mclib/stuff/extentpoly.cpp` (380) | DELETE; fix the stale comment at `CMakeLists.txt:570` (the fingerprint banner is `code/mechcmd2.cpp:213`) | `mclib/debugging.cpp`, `mechcmd2.cpp` | 0 compiled | logmain was still edited 2026-06 (`d16c15bc2`): agents keep patching dead code | build | −1,369 | very low | precedes 1 |
| 8 | MLR classes never registered: `mlr_terrain.*`, `mlr_terrain2.*` (`InitializeClass` commented at `mlr.cpp:174-175,279-280`), `mlrlookuplight.*` | DELETE | — | outside mlr: 0 | a stream naming these ClassIDs already fails, since there is no class data | build; mc2fx dump | −1,876 | low | 0 |
| 9 | MLR lights + light map: `mlrlight`, `mlrambient/infinite/infinitewithfalloff/point/spotlight`, `mlrlightmap` | INVESTIGATE → DELETE | — | outside mlr: 0. All 3 gosFX draw sites pass `activeLights=NULL, nrOfActiveLights=0` (`gosfx/shape.cpp:324`, `shapecloud.cpp:325`, `debriscloud.cpp:829`) | `_L_` mesh classes must still load (data-driven ClassIDs), so stub their `Lighting()`. `mlrclipper.cpp:501,558` reference `MLRLightMap` | mc2fx dump; VFX capture A/B (missile/PPC scene) | ~−3,500 | medium | 0 |
| 10 | `HostServices/` (contract + default impl, 250 lines) | DELETE or adopt (owner Q1) | `Gates` (IConfig), `gos_GetElapsedTime` (IClock), mclib `File` (IFileSystem), `mc2log` (ILogger) | production 0; `tests/unit/test_host_services.cpp` only | appears in firewall scope (`check-include-firewall.sh:22`) and the `file_size`/`dup_lines` dir lists | doctest; `include_firewall` | −370 | very low | 0 |
| 11 | `GameOS/gameos/utils/logging.{h,cpp}` (`log_*`) | CONSOLIDATE → `utils/mc2log.h` `MC2_LOG` | mc2log facade | 60 sites in `gos_render.cpp`, `shader_builder.cpp`, `editor/EditorGosRender.cpp`, `mclib/terrain_internal.h` | keep stderr for WARN/ERROR (logging.cpp ~50) | doctest `test_mc2log` | −75, 1 API | low | step 3 |
| 12 | gos file stubs that `STOP` (`gos_DoesFileExist/CreateDirectory/Find*/GetFile`, `gameos_fileio.cpp:153-202`) + 3 tool stub copies (`asset_viewer/engine_stubs.cpp:139`, `mc2fx/mc2fx_stubs.cpp:98`, `tg_import_dump/stubs.cpp:139`) | DELETE after #3 | `gos_FileExists` (`access()`) | only Stuff FileStream/Directory call them | — | build tools | −120 | low | after 3 |
| 13 | `GameOS/gameos/utils/{camera,matrix}.{h,cpp}` + `gl_utils` `draw_mesh_*` / `make_mesh_from_file`; `GameOS/include/{mfcplatform,filestream}.hpp` | DELETE | mclib `Camera`, `axis_convert`, `RenderCore` | `struct camera` is never instantiated (includes at `gameosmain.cpp:186`, `text_tool/main.cpp:12` are vestigial). `glu_*` are used only by `utils/camera.cpp`. `filestream.hpp` is shadowed by the same-dir Stuff header for every includer | — | build all incl. `text_tool` | ~−600 [unverified] | low | 0 |
| 14 | `LZDecomp(dest,src,srcLen)` unbounded 3-arg overload (`lzdecomp.cpp:217`, `lz.h:14`) | DELETE | the 4-arg `destMax` overload | 0 | removes the `srcLen*16+4096` guessed bound | doctest `test_fst_roundtrip` | −25 | very low | 0 |
| 15 | `UserHeap`/`HeapManager` | MIGRATE → thin facade, then `std::vector` | malloc (already: `gos_Malloc`/`gos_Free` = malloc/free, `gameos.cpp:226-277`) | `systemHeap->`/`Malloc`/`Free`: mclib 369, code 192, editor 14 sites; `HeapManager` (VirtualAlloc) only `bitflag.cpp:45,245` | heap destroy does NOT free outstanding blocks today (`gos_DestroyMemoryHeap`, `gameos.cpp:196`), so no arena semantics remain | lifetime harness (step 4); tier1 | −400 after #4 | low | step 4 |
| 16 | Legacy CPU projected shadows `mclib/tgl_shadows.cpp` + `RenderShadows` chain (`msl_shape.cpp:357`, `mech3d_render.cpp:1215`, `gvactor_render.cpp:25`, `genactor.cpp:1082`) | INVESTIGATE → DELETE | CSM (`gos_IsModernShadowPathActive`, `gameos_graphics_tparams.cpp:491`) | mech/gv return early when CSM is live; `genactor.cpp:1082` has no such guard | reachable if the shadow material fails to load, or in hosts without `g_gos_renderer` (editor? [unverified]) | tier1; goldens `reset-50331d5` | ~−700 | medium | 0 (after proof) |
| 17 | `EString` (`mclib/estring.*`, 2,096 lines) | MIGRATE → `std::string` + `std::format` | std | code:46, editor:16, gui:7, mclib:3 files | non-atomic COW refcount (`estring.h:205-246`). `Format()` hand-sizes `%` specs, then `vsprintf` (`estring.cpp:847`) | build; tier1 UI text | −2,000 | medium | steps 3/4 |
| 18 | `EList` (`mclib/elist.h`, 1,463) | MIGRATE → `std::list`/`std::vector` per site | std | 52 files | insertion order and iterator-stability semantics per site | per-subsystem | −1,400 | medium | step 4 |
| 19 | `Stuff::` math → glm | KEEP for now (INVESTIGATE step 5) | glm not vendored; `GameOS utils/vec.h` is the render-side type | `Stuff::` 5,430 uses (code 1301, mclib 3755, editor 314) | Stuff is the MC2-space truth (row-vector). `axis_convert.h` templates over Stuff types. `/fp:precise` default with no FP flags, so a swap must keep op order or accept gameplay float drift | `test_xform_convention`; `axis_swizzle_ratchet`; goldens | ? | high | step 5 |
| 20 | gosFX + MLR clip/submit vs GPU particles | KEEP | GPU particles are default-off (`MC2_GPU_PARTICLES`) and sample gosFX's CPU-advanced age (`MC2_VFX_AGE_SAMPLE`) | gosFX is the simulation truth. The `mlr_gate.h` "A4 deletes mlr/" plan and `docs/vfx-oracle-coverage.md` ("gated OFF by default") are stale | `.fx` binary = mod content | — | 0 | — | — |
| 21 | Gate `MC2_VK_STATIC_SHADOW_MAP` ("DEPRECATED no-op"); `CMakeLists.txt:772-777` Viewer TEMP block (`Viewer/` dir absent) | DELETE | — | — | — | `check-contracts` gate rows | −30 | very low | gate program |

## (B) Deletion clusters

- **C1 unbuilt-TU cluster** (#1, #7, `extentpoly`, `filestreammanager_test`).
  - Roots: the 10 MLR files plus `code/logmain.cpp`, `code/debugging.*`, `mclib/stuff/extentpoly.cpp`.
  - Dependents: none compiled. Stale references survive in `data/defs/debug/source_diagnostics.fit` (48 `firstSourceFile=` metadata lines), the mc2fx exclusion list, and the ratchet baselines.
  - Closed because: no target compiles them; `logmain` cannot compile (missing `gosscript`).
  - Must prove: default build plus every opt-in target (`ENABLE_MC2FX`, `_PREVIEW`, `MC2WEAPON_GUI`, `EditRel`, `ui_editor`, `tests/unit`) still configures and links.
- **C2 Stuff I/O + container cluster** (#3, #12).
  - Roots: `Database`, `NotationFile/Page/Note`, `FileStream/Directory/FileStreamManager`, `SafeChain`, `Table`, `Tree`, `Slot`, `NameList`, `MatrixStack`.
  - Dependents: `mlrlookuplight` (C3), the MLR light `Page` ctors (C3), `stuff.cpp` init (FileStreamManager, `SlotLink`/`SafeChainLink`/`FileStream::InitializeClass`), `memorystream.cpp:147` `Check_Object(FileStreamManager::Instance)`, and the `extentbox.hpp:29` forward decl.
  - Closed because: the only external reader is dead MLR code, and its backend (`gos_GetFile`) already `STOP`s.
  - Must prove: the 3 tool stub TUs link; the mc2fx `.fx` dump is byte-identical; tier1 passes.
- **C3 MLR unreachable features** (#8, #9).
  - Roots: `MLR_Terrain*`, `MLRLookUpLight`, `MLRLightMap`, `MLRLight` family.
  - Dependents: `MLRClipper` light-map draw (`mlrclipper.cpp:501,558`), `MLRSortByOrder` (`mlrsortbyorder.cpp:310,376`), `_L_` mesh `Lighting()` paths, `mlr.cpp` Initialize/Terminate.
  - Closed because: no producer of lights exists (`MLRLight::Make` has 0 callers; all draw-info light lists are NULL).
  - Must prove: no `mc2.fx` shape stream embeds light or lightmap ClassIDs (mc2fx dump with a ClassID histogram), and a VFX A/B capture.
- **C4 heap.** #4, then #15.
  - Roots: the `#ifndef USE_GOS_HEAP` bodies and `imagehlp.h`; then `HeapManager` (BitFlag) and `UserHeap`'s own bookkeeping.
  - Closed because: `USE_GOS_HEAP` comes from `LINUX_BUILD`, which every target defines (CMakeLists.txt:187).
  - Must prove: `cl /EP` identical for #4. For #15, nothing calls `heapReady()`, `getHeapName()` or `walkHeap()` expecting non-trivial results; `heapName` is already `NULL` (heap.cpp:301 vs :393).
- **C5 string classes.** #5 is closed (0 users). #6 is closed after 8 files switch type. #17 is open; it needs per-site review of `Format` specs and COW aliasing.

## (C) Execution order

1. **C1 + #5 + #14 + #21** first. These are pure deletions, so the cxx17, dup_lines and file_size baselines only go down. Land them before the clang-tidy safe set (step 1) so fix-its and comment passes are not spent on dead files. The dead-asm series already spent effort on C1, and logmain shows comment/feature edits landing in dead TUs.
2. **#4** as the next file of the dead-asm series, with the same `cl /EP` proof. Then **#2** and **#7**.
3. **C2 (#3, #12)**, then **#8**, then the **#9** investigation. All of these precede step 1, so tidy does not spend fix-its on files about to be deleted.
4. **#6** with step 1. **#11** at step 3 (the logging facade). **#10** whenever the owner rules (Q1).
5. **#15, #17, #18** inside step 4 (ownership), using the lifetime harness. Do #15 only after #4.
6. **#16** after a renderer-less-host check (Q4). **#19** at step 5, only after the vendoring decision (Q2).
7. **Owner's comment pass.** Deleting a file drops its entries from `comment_guard_restores.json`, `cite_guard` and `docs/split-map.md`. Run `cite_guard` and update `source_diagnostics.fit` metadata in the same commit, or leave it and record it (Q5).

## (D) UB / memory-safety repairs in kept code

| file:line | issue | fix (size) |
|---|---|---|
| `mclib/inifile.cpp:192-197` | The `[blockId]` copy writes `blockId[count-1]` (`char[50]`, `inifile.h:57`) before the `count >= 49` check. It also never stops at `'\0'`, so a long or unterminated block name in a mod `.fit` overflows the heap. `:203` `sprintf` of `line` into `char error[256]` | Bound inside the loop; use `snprintf` (≈8 lines) |
| `mclib/stuff/memorystream.cpp:96-101` | `ReadBytes` bounds check exists only under `_ARMOR`, which no TU defines. A truncated or malformed `.fx` (mod content) is read out of bounds | Always-on remaining check that returns or STOPs (≈5 lines). Valid data is unaffected |
| `mclib/packet.cpp:270,421` | `packet` is `int` (the comment at :419 is stale). A negative index other than -1 reads `seekTable[packet]` out of bounds | `packet < 0 \|\|` (2 lines) |
| `GameOS/gameos/gameos.cpp:226` | Replaced global `operator new` returns malloc's NULL instead of throwing. That is UB, because the compiler assumes the result is non-null | `if (!p) throw std::bad_alloc()` or abort (3 lines) |
| `editor/CampaignDialog.cpp:240`, `DlgFileOpen.cpp:133`, `Objective_actions.cpp:127,360`, `UnitSettingsDlg.cpp:516,715`, `mclib/stuff/filestream.cpp:502` | `delete` on `new[]` | `delete[]`, or `std::vector`/`std::string` (7 lines) |
| `editor/EditorInterface.cpp:475` | `delete` on a `gos_Malloc` block. It works only because both resolve to malloc/free | `gos_Free` (1 line) |
| `mclib/estring.cpp:847` (`echarstring.cpp:858`) | `Format` pre-sizes by parsing `%` specs by hand (`ll`/`z`/`j` unhandled), then `vsprintf`. Possible overflow | `vsnprintf(nullptr,0)` sizing (−60 lines). Moot after #17 |
| `mclib/mlr/mlr_gate.cpp:32,51` | Unsynchronised read of `g_initialized` (double-checked lock without atomics) | Function-local static or `std::atomic` (3 lines) |
| `GameOS/gameos/gameos.cpp:284-292` | Lazy `s_detRngChecked` latch without atomics. It is a race only if `gos_rand` runs on a `MC2_FRAME_JOBS` worker [unverified: none seen] | `std::atomic`/function-static (3 lines) |
| `mclib/heap.cpp:301 vs 393` | The `heapName` allocation leaks and is then nulled on the live gos branch | Remove the allocation or keep the name (2 lines) |
| `mclib/timing.h:8` vs `GameOS/gameos/utils/timing.h:1` | Same include guard `TIMING_H`: the second header included is silently skipped. Also `OBJECTIVE_H` and `MAINMENU_H` (code vs editor) | Rename guards / `#pragma once` (4 lines) |
| `mclib/mlr/mlrlinecloud.cpp:189-190,372` | `*(int*)&MLRClippingState` punning | `std::bit_cast` or an accessor (3 lines) |

## (E) Keep unchanged, and why

- **FST/`.pak`/`.fit` I/O** (`mclib/file.cpp`, `ffile.cpp`, `fastfile.cpp`, `packet.cpp`, `inifile*.cpp`, `csvfile.cpp`) and **`fst_hash.cpp` `elfHash`**. The on-disk index stores this hash, mod install tools mirror it (`tools/mod_install/fst_listing.py`), and `data_tools` (makefst/pak/aseconv) write the formats. Repair only (D).
- **`lzdecomp.cpp`/`lzcomp.cpp` 4-arg zlib + classic LZW.** These decode stock Microsoft-packed FSTs and texture LZ blobs (`txmmgr_texture.cpp:604`).
- **gosFX (`mclib/gosfx`) + `Stuff::MemoryStream` + `RegisteredClass` + MLR clip/state/cloud.** The `mc2.fx` binary is mod content. This is the default VFX path, and the GPU-particle spec library is derived from it.
- **`mclib/pqueue`, `sortlist`, pathfinding (`move_*`), `RandomNumber`/`gos_rand` (CRT `rand()%32768`), `det_rng.h` (gated `MC2_DETERMINISTIC_RNG`), `Stuff::Random` R250.** Tie-ordering and RNG-call order decide gameplay and save/replay outcomes.
- **Stuff math C fallbacks.** These are the live code (`SqrtApproximate`→`sqrt`, `OneOverApproximate`→`1/f`, `scalar.hpp:108-145`; matrix C loops after the `USE_ASSEMBLER_CODE` blocks). They already differ from the 1999 asm approximations; do not "restore" those.
- **`MC2Strings` + the `gos_OpenResourceDLL` fallback** (`gameos_res.cpp:71`). Stock installs boot without `data/defs`.
- **mclib `UserInput`** layered over `gos_input`/SDL. These are layers, not duplicates.
- **mc2log + `mc2_diag::writeEvent`** (JSONL trace for smoke/oracles). They have different consumers, so do not merge them.

## (F) Open questions for the owner

1. Is `HostServices/` a seam you still intend to adopt (asset viewer, Blender bridge), or should it go? It has 0 production users.
2. Step 5: will glm be vendored? If not, step 5 should become "keep `Stuff::` as MC2-space types; unify only render-side `vec.h`".
3. Is gameplay float drift acceptable for any math replacement (saves or replays across builds)? Or must `Stuff::` math stay bit-identical?
4. Do the editor, asset viewer or any tool host run without the CSM shadow material? That decides whether the CPU projected-shadow path (#16) is reachable.
5. `data/defs/debug/source_diagnostics.fit` stores `firstSourceFile` paths. Should deletions rewrite them, or are they historical metadata?
6. Must mods still be able to ship MLR lit/lightmapped shapes or `MLR_Terrain` streams inside `.fx`? (This is #9's proof.)
7. Should `EString` stay as an ABI for the `gui/` widgets, or migrate with them?
