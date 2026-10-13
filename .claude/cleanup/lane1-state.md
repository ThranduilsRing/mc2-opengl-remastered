<!-- doc-status: current | reviewed:2026-10-12 -->
# Lane 1 state (builder on A:/Games/mc2-main, branch claude/nifty-mendeleev). Hand-over file for the next agent; keep current at every report.

Session 2026-10-12 evening (lane-1 Sonnet, cap 6 h, ~3.5 h used): step 4 batch B1 (EString -> std::string outside the gui widgets): the Opus before-review applied (R1-R12), B1.0 the harness batch,
B1.1 runtime objectives, B1.2 the texture-cache static string (deleted). Never pushed. Install `mc2-win64-v0.4` (the editor from this HEAD is deployed there too). Lane 2 untouched: the one finding in
its file (`GameOS/gameos/vulkan_context.cpp:109`) is reported below, not edited. No Opus after-review requested (it comes after B1.3, RA1).
Previous session (review fixes F1-F6, ABL string safety): `git show afa99eaf6:.claude/cleanup/lane1-state.md`.

## Commits this session (oldest first)
- de7e5008f docs: R1-R12 applied to `cxx23-step4-b1-notes.md` (H1 restated, H13-H15 added, simulation sentence replaced, batches reordered: objectives first, logistics split A/B, mp screens to B6, the TGA cache deleted
  not converted, the cap counts EDITED files, save proof = old exe twice + new exe + `save_inspect diff`, editor proof = `-mission` + `-smoke-save` .fit byte diff, no printf-template formatter, bridge as `const char*` +
  `const std::string&` folded at B6, /analyze corrected, grouped after-reviews, the old-exe probe) and `cxx23-step4-b1-files.json` (67 files: 56 in B1.1-B1.7, 11 mp screens in B6).
- db9b1d4fa B1.0a. `mclib/mc2str.h` (`mc2::str::from_c`, `iequals`, `icompare`, `ci_less`, `ci_equal`, `ci_total_less`, `ascii_lower/upper`); `tests/unit/test_estring.cpp` characterizes the old class
  (H1-H14: null `Data()` only when never assigned / emptied, `""` non-null, `EString(nullptr)` empty, equality and order ignore case through lstrcmpi, MakeUpper code page, inclusive-end index conventions, COW
  pointer outlives its source); `tests/unit/test_mc2str.cpp` + fixture `stock_names.txt` (7046 stock names): `iequals` agrees with the old compare on every name and case variant, 20 adjacent order differences
  (digit/dot after underscore/hyphen: lstrcmpi is a word sort). LG06 +1 (`from_c`).
- 432b4ae03 + f78ce17d8 B1.0c. `tests/analyze/analyze_dangling.py` + seeded `dangling_seed.cpp` / `dangling_clean.cpp` + board row `analyze_dangling` (10 s). Measured: `cl /analyze` lifetime rules catch NONE of the
  `c_str()` shapes on this host; clang-cl catches all four seeded shapes (A `-Wdangling-gsl`, B/B2 `-Wreturn-stack-address`, C `-Wdangling-assignment-gsl`), clean twin silent. The fix commit routes the two spawns
  through `console_child.child_kwargs` and registers the script in `scripts/background-launch-allow.json` (the first commit had left `background_launch` and `scripts_tests` red).
- 5e02d2347 B1.0b. `.claude/cleanup/cxx23-step4-b1-callsites.txt`: 598 call sites in 76 files from a `[[deprecated]]` build of the old API (never committed); 419007717 drops the rows B1.1/B1.2 converted: **498 sites in 71 files now**
  (operator ECHARPTR 207, Data 130, Format 70, Compare 35, operator== 13 ...; per batch B1.3 15, B1.4 99, B1.5 80, B1.6 135, B1.7 26, B6 50; 93 more sites in 19 includer / gui files).
  `tests/analyze/estring_callsites.py` regenerates from a build log or removes rows (`--drop FILE[:LINE]...`).
- 3a851f497 B1.0e. gui bridge: `aText::setText`, `aTextListItem::setText` and `aEdit::setEntry` take `const char*` and `const std::string&` next to the EString form, `aEdit::entry()` returns `std::string`;
  `tests/unit/test_gui_bridge.cpp` checks the overload resolution at compile time (a literal, `const char*`, `std::string`, EString and `setText(0L)` each resolve to one overload; no `string_view` overload).
- b3512f76a docs: B1.0 results (the probe, the call-site list, the gate verdict).
- e5600b260 B1.1. `code/objective.{h,cpp}` have no EString: new leaf TUs `code/objective_text.{h,cpp}` (`read` optional, `write` always writes the key, `readFields` = the missing-key rule: no Description shows the
  Title, no ObjectiveModel is empty, over-long values are cut and logged, the break conversion no longer drops a trailing character) and `code/objective_flags.{h,cpp}` (`CBooleanArray`, insertion order, `iequals`);
  accessors return `const std::string&`; 46 dead virtuals deleted (`Description()` of 21+7 classes, 13 `InstanceDescription()`, `DescriptionOf*Species`; no caller in the repo); model fields initialised (they wrote stack
  garbage); `tests/unit/test_objective_text.cpp` (8 cases). objective.cpp 2657 -> 2349 lines, objective.h 802 -> 615. Forced edits: controlgui_objectives (3), controlgui_update (1), missionbriefingscreen (2).
- 5d640f3f5 B1.2. The TGA last-load cache and its `new EString` deleted (7 statics, the extern, the delete): EString new/delete 1/1 -> 0/0, own_inventory baseline rewritten.
- 61d0bf3f2 B1.0f. `/we4840` for mc2, mclib, gui, EditRel (C4840 is off by default: with it on, the trees had one site in the editor, `EditorData_terrain.cpp:100`, an MFC CString through sprintf, now cast, and one
  in gameos, below). c4bd334ba docs: B1.1/B1.2/B1.0f results and four findings in the notes.

## Proof numbers (HEAD c4bd334ba plus the state-file commit)
- doctest 2781 test cases passed / 0 failed / 1 skipped (2757 before; +24: 9 characterization cases, 6 mc2str, 1 gui bridge, 8 objective_text), plain and under ASan.
- `tools/build_all_targets.py`: rc 0 (root tree incl. editor and mc2fx, three standalone projects, 16 harnesses, unit tier), zero C4840. `scripts/ninja_build.py` rc 0.
- `tools/own_inventory.py --check`: ok (454 types, EString gone). `cxx17_ratchet --check`: OK after the baseline rewrites (LG06 +1 from_c, +1 objective_text key parameters; LG03 -4, LG05 -1, LG09 -7 in total).
- `sim_determinism check` mc2_03 / mc2_10 / mc2_24 / mc2_16 (liveness): PASS on the final exe, first 1000 frames match the committed baselines in every digest (also after B1.1).
- tier1 5/5 `2026-10-12T21-01-42` on exe `c4bd334ba` (mc2_01 135 fps, mc2_03 78, mc2_10 98, mc2_17 124, mc2_24 118). The earlier B1.1 run (dirty tree) `2026-10-12T20-29-56` was 5/5 too.
- editor smoke `2026-10-12T21-24-54`: PASS 15/15. Board (`check-contracts.sh`): 170 passed, 1 failed (`script_callers`, stale after the CMakeLists line shift; regenerated and committed with this file).
- Save proof B1.1 (mc2_01, render frame 1000; `/tmp/b1save` is scratch, the numbers are here): the noise set of the OLD exe (two runs) is TerrainObjectData byte 340 (191 packets), MechWarriorData 41, MechData 6 (offsets 3020-3035),
  CommanderData 2, and the objective model fields. Old vs new differs by: text packet 2 = two added `st ObjectiveModel = ""` lines (the two nav-marker objectives) and four stack-garbage model fields now 0, nothing else; MechData 134 +
  GroundVehicleData 151 = bytes 2953-2955/3001-3003 of every record, the saved vftable pointers of the embedded `MoverControl` (offset 2952) and `MoverDynamics` (offset 3000), which move with every link; everything else
  (MechWarriorData, CommanderData, TeamData, TerrainObjectData byte 340) is inside classes that also differ between two runs of one exe. The new exe's objective text packet is identical run to run.
- The old-exe probe (stock data): the only fold-only match is `LogisticsData::updateAvailability` (`logisticsdata_availability.cpp:245`, the pilot file name `Compare(pilotName, 0)`: `pmwchopper` against `PMWChopper`, 15 hits a
  run): a bare `==` there leaves the stock PMW pilots unavailable. No ordering-only site reached; stale-reader hits only on quickload (`ObjectiveModel`, the nav-marker objectives). Full text: notes section 8.

## NEXT for the next owner, in order
1. **B1.3 logistics model A** (`code/logisticsvariant.h`, `logisticsvariant_stats.cpp`, `logisticsmech.h`, `logisticsmechdisplay.cpp`, `logisticspilot.h`, `logisticspilotlistbox.cpp`; includers the compiler forces:
   `logisticsvariant.cpp` 6, `logisticsmech.cpp` 3, `logisticspilot.cpp` 3, and `logisticsdata_availability.cpp` because the pilot header changes, 11 includers of `logisticspilot.h`, 10 of `logisticsmech.h`, 7 of
   `logisticsvariant.h`). Rules C1-C14 of the notes. THE hazard: `(*pIter).getFileName().Compare(pilotName, 0)` in `logisticsdata_availability.cpp:245` must become `mc2::str::iequals` (the probe shows the stock PMW pilots depend
   on the fold); the 35 `Compare(x, 0)` lookups -> `iequals`, sort sites (`logisticspilotlistbox.cpp:330`) -> `ci_total_less` with the order stated in a comment, the `MakeLower` file names (H12) -> `ascii_lower`, the
   `getCurrent*` getters (C7). Proof: lifetime gate over the TUs (`py -3 tests/analyze/analyze_dangling.py --check <files> --compile-commands build64-ninja/compile_commands.json`), doctests of the converted lookups on a case-variant
   fixture, `MC2_BOOT_TO_BAY` / `MC2_BOOT_TO_MECHLAB` captures viewed (`scripts/mechbay_preview_harness.py bay|purchase|loadout`; the BEFORE images (bay, purchase, loadout, 640x480 TGA, this HEAD
   c4bd334ba) are in `A:/Games/mc2-capture-evidence/b1-before-B1.3/`; take the after images with the same harness command `MC2_CURSOR_GUARD_OFF=1 py -3 scripts/mechbay_preview_harness.py <screen> --timeout 60 --frame 600 --out <file>`), determinism check, `build_all_targets`, ASan doctest, tier1. After it: `py -3 tests/analyze/estring_callsites.py --drop <converted files>`.
2. After B1.3: the grouped Opus after-review (RA1) is the orchestrator's call; do not request it from the lane.
3. B1.4 (logistics model B + saveload + dialog; split at `logisticsdialog` past 40 edited files), B1.5 (screens), B1.6 / B1.7 (editor: `-mission=<stock> -smoke-save=<out>` byte diff old vs new editor), B1.8, then B6.
4. For lane 2 / the orchestrator: `GameOS/gameos/vulkan_context.cpp:109` passes a `std::string` (`env`) to the printf-style `ctxlog("... '%s' ...", env)`: prints garbage on an unrecognised validation preset (`env.c_str()`);
   then add `gameos` to the `/we4840` list in the root CMakeLists.txt.
5. Findings outside B1 (notes section 8): `FitIniFile::readIdString` joins the first two lines of a multi-line value without a break; `CPlayBIK` / `CPlayWAV` prefix `"./"` on every load and save the prefixed path;
   records are saved as raw images including vftable pointers (a byte diff of two builds is noisy by construction: mask or zero them in the save-format guard work); `readIdString` leaves a value of exactly the buffer length unterminated.
6. Carried over from the previous session: ABL int/real out-array slice (the 16 `open` census rows), the 8 tight ABL debug strings (needs the owner), mc2_07 tier2 on an idle host, save round-trip baselines, the non-mover
   zero-vector question for ABL.

## Tool notes
- **Bash heredocs eat backslashes in Python too** (`"\\n"` became a real newline three more times this session, a bare `py -3 - <<EOF` with an empty body hangs on stdin). Write any script that carries an escape with the Write tool
  or with the Edit tool; for byte-exact CRLF-aware edits a scratch helper (`eol_edit.edit(path, old, new)`: detects the file's own ending, refuses mixed files) worked well; `tools/dev/apply_edit.py` is the committed equivalent.
- Deprecation census recipe (not committed, by design): mark `[[deprecated("B1")]]` on the public API of `mclib/estring.h` (conversion operator, `Data`, every comparison operator, `Compare`, `Make{Upper,Lower}`, the index functions,
  `Format`; a byte-level regex over the class body, CRLF), `py -3 tools/build_all_targets.py --root-only -j16 --keep-going > log`, `py -3 tests/analyze/estring_callsites.py log`, `git checkout -- mclib/estring.h`. 850 steps, about 12 min.
- Old-exe probe recipe (not committed): a patch applied by script to `mclib/estring.cpp` (`Compare` logs a fold-only match with `CaptureStackBackTrace` RVAs and the two strings) and `code/objective.cpp` (`sReadIdString` logs a miss and
  the stale text), build, `deploy_payload.py --target game --ninja`, `run_smoke.py --mission X --duration 28 --deploy 0.4 --keep-logs`, resolve RVAs with `llvm-symbolizer --obj=build64-ninja/mc2.exe --relative-address`.
- Save proof recipe: `py -3 tools/saveload_roundtrip.py resave --mission mc2_01 --out DIR` twice with the OLD exe (build it first and keep its saves) and once with the NEW exe, then `py -3 tools/save_inspect.py diff a/save1.ims b/save1.ims`;
  per-offset analysis needs a few lines over `save_inspect.load` (the byte offsets inside a record class); `clang-cl -Xclang -fdump-record-layouts-complete` names the member at an offset.
- `deploy_payload.py --target editor` needs `--build-dir <tree>/out/editor --build64-root build64-ninja --allow-stale-exe` when the editor was built by `build_all_targets` (it compares against the dev tree's objects).
- `build_all_targets.py` rebuilds in 3 min when nothing but a few TUs changed; a global compile flag change rebuilds every tree (dev 3 min, all-targets 13 min).
- Several `Monitor`/`until` waiters on one log all fire when it ends: start one.
- `grep -c $'\r' file` matches every line here: use Python (`b.count(b"\r\n")`) or `git ls-files --eol`. Working copies of `.claude/cleanup/*.md` are CRLF only sometimes; `eol_edit` handles both.
- `cxx17_ratchet --check` reads the COMMITTED tree by default: use `--worktree` / `--staged` before committing, rewrite the baseline with `--write-baseline --worktree` and say why in the message.
- `git add -N` a new file before `log_tag_census.py --check` / `callers.py` (both read `git ls-files`); regenerate `tests/callers.json` (`py -3 tools/callers.py`) after ANY line shift in CMakeLists.txt or check-contracts.sh.
- Fingerprint: committing code after the build makes the deployed exe stale against HEAD; doc commits do not (the runner reported the exe built from the previous doc commit as matching).

## Rules (unchanged)
- Commit by explicit pathspec after `git diff --cached --name-only`; `git commit -F <file>`; trailer `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
  Commit BEFORE build + deploy. Launches only through `scripts/run_smoke.py --deploy 0.4` / `tools/sim_determinism.py` / `tools/saveload_roundtrip.py` / `scripts/mechbay_preview_harness.py` / `scripts/run_editor_smoke.py`,
  background-only, never `--no-lease`. Never kill cl/ninja by name (`tools/kill_build_tree.py`). Never contort code for a ratchet (rewrite the baseline with the reason). Never touch GameOS/, RenderCore/, shaders/, Gates/
  beyond a task's need, nor tests/unit/test_gate.cpp, nor gos_static_prop*, nor code/multplyr*.
