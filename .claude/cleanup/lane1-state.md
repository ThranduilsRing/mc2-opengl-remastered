<!-- doc-status: current | reviewed:2026-10-13 -->
# Lane 1 state (builder on A:/Games/mc2-main, branch claude/nifty-mendeleev). Hand-over file for the next agent; keep current at every report.

Session 2026-10-12 night (lane-1 Sonnet): the cleanup-review fixes F1-F12, then step 4 batch B1.3 (logistics model A, split by header: variant+mech, then pilot). B1.4 was NOT started (design in
`cxx23-step4-b1-notes.md` section 8b). Never pushed. Install `mc2-win64-v0.4` holds the exe of 45f39522a (B1.3b). Lane 2 untouched. No Opus after-review requested (the orchestrator calls RA1 after B1.3).
Active work time about 3 h (the session clock jumped 7 h between two tool calls at 23:15 and 06:19, a suspended host, not work). Previous session: `git show 2b4e2f3f1:.claude/cleanup/lane1-state.md`.

## Commits this session (oldest first)
- ef9a9ab30 F1 `Team::Save` copies only `rosterSize` roster entries (the saved tail was uninitialised heap). TeamData confirmation below.
- ea995215d F2 (`<string>` outside the ESTRING_H guard), F3 (dead includes in objective.cpp), F4 (`aEdit::entry` via `from_c`), F5 (`ci_equal` deleted), F8 (`objective_text::read` rejects an over-long value with a log line, tested),
  F9 (`objective_text::readFlagID`, one FlagID-else-index rule, 3 copies -> 1, tested), F10 (history wording out of the comments). LG06 +2 (readFlagID key parameters), baseline rewritten with the reason.
- 1266aa1a2 F6 (`analyze_dangling.py` lost the `cl /analyze` leg: 20 s -> 8 s; exact tag set; exit 2 for a file with no compile_commands entry or that did not parse), F7 (`estring_callsites.py --check` + board row
  `estring_callsites`; gui/aedit.cpp and gui/asystem.cpp rows moved to their lines), F12 (`tests/callers.json` regenerated).
- c1a283422 B1.3a: `LogisticsChassis` / `LogisticsVariant` / `LogisticsMech` on `std::string`, `const std::string&` accessors, `setName(std::string)`, every variant / mech / chassis-file lookup `iequals` on a `from_c` key, the two bare `==`
  (inventory, modify) as `iequals`, mechlist variant order `ci_total_less`, `mc2::str::file_stem` (replaces the Find/Left/ReverseFind/Right chain at 3 sites; tested against it on the stock file names). 21 edited files. LG06 -2, LG09 -22 (6,627 -> 6,605; the first draft of this line said -49).
- 06ea51611 `mechbay_preview_harness.py`: `encyclopedia`, `encyclopedia_vehicles`, `encyclopedia_pilots` screens (boot from the main menu, no mission is played).
- 45f39522a B1.3b: `LogisticsPilot` on `std::string`; the fold-only site (`PMWChopper` / `pmwchopper`, `updateAvailability` and `appendAvailability`) and every pilot lookup `iequals`; pilot list order `ci_total_less`; display upper-casing `ascii_upper`.
  13 edited files. LG09 -3.
- (this commit) docs: notes (B1.3 results, section 8b B1.4 design, F1 sentence corrected), this file, the call-site list regenerated from a fresh deprecation build (exact census).

## TeamData confirmation (F1)
`Team::Save` bug: the two sentences "TeamData differs in the old exe too" (notes section 8, this file) were false. Measured: old exe, two runs: 0 TeamData bytes differ. After ef9a9ab30 (exe 1266aa1a2), two clean runs `f_a` / `f_c`
(`C:/Users/Joe/AppData/Local/Temp/b1save/`): 47 packets differ, MechWarriorData 39, MechData 6, CommanderData 2, no TeamData, no text packet. The same holds for the post-3a pair (`g_a` / `g_b`: 40 packets, MechWarriorData 38, CommanderData 2)
and the post-3b pair (`h_a` / `h_b`: 42 packets, MechWarriorData 40, CommanderData 2). Notes corrected. (Runs `f_b`, first `g_a` / `g_b` died on a loaded host with `heartbeat_freeze_play`: the quickload frame was not reached
within the 28 s window; the duration cap is 30 s, rerun when the host is quiet. A stale `save1.ims` from 2026-10-11 stays in a reused `--out` directory: always `rm -rf` it first.)

## B1.3 save-diff explanation (mc2_01, render frame 1000; pre-batch exe 1266aa1a2 vs 06ea51611 (3a) vs 45f39522a (3b))
Between any two of the three exes: the mission text packet (packet 2) is byte-identical; MechData (134) and GroundVehicleData (151) differ ONLY at bytes 2953-2955 and 3001-3003 of every record (3a vs pre) / 2954-2955 and 3002-3003 (3b vs 3a): the
saved vftable pointers of the embedded `MoverControl` (offset 2952) and `MoverDynamics` (offset 3000), which move with every link; plus 3020-3035 in one MechData packet (the known noise); MechWarriorData (38-41) and CommanderData (1-2) are
the noise set (they differ between two runs of one exe). Nothing else differs. (Correction 2026-10-13: logistics state IS in the mission save, as its last text packet: 52 pilot blocks, byte-identical across the three exes, so `LogisticsPilot::save` is covered; `VariantCount` and `InventoryCount` are 0 in a mission-boot save, so the variant and mech writers were covered only by reading until the campaign-save round trip of notes section 8c.)

## Proof numbers
- Pre-commit, each of c1a283422 and 45f39522a: `scripts/ninja_build.py` rc 0; `tools/build_all_targets.py` rc 0 (every stage); `build_contract_harnesses --run --unit` 20 harnesses pass, doctest 2782 passed / 0 failed / 1 skipped (plain), 2782 under
  `--asan`; `analyze_dangling --check` over the edited TUs (18, then 11): 0 diagnostics; `cxx17_ratchet --check --worktree` OK; `own_inventory --check` ok (454 types); `log_tag_census --check` OK; `estring_callsites --check` OK.
- `sim_determinism check` PASS after both batches: mc2_03 (2372 frames), mc2_10, mc2_24, mc2_16 (liveness, 2414 frames), first 1000 frames equal the committed baselines in every digest.
- Mechbay captures (frame 600, 640x480; before `A:/Games/mc2-capture-evidence/b1-before-B1.3/`, after `b1-after-B1.3a/`, `b1-after-B1.3b/`; viewed): bay, purchase, loadout, launch (pilot ready), encyclopedia (mechs), encyclopedia_pilots: same text,
  same lists and order; pixel differences only the idle mech animation (bbox 294-349 x 290-344) and a fade of at most 15/255 on loadout and launch. `encyclopedia_vehicles` STOPs in the old and the new exe alike (`String Id 45208, bufferLen 255`).
- tier1 5/5: `2026-10-12T22-57-34` on 06ea51611 (the first run `22-45-14` was 4/5, mc2_24 `heartbeat_freeze_play` with another lane's cl.exe running; mc2_24 had just passed determinism), `2026-10-13T06-42-35` on 45f39522a
  (mc2_01 140 fps, mc2_03 80, mc2_10 99, mc2_17 125, mc2_24 120).
- Idiom KPI (`cxx17_ratchet --report` TOTAL row, 21 idioms summed): 16,261 at the session start (baseline at 05011a47c) -> 16,236 now (LG09 6,627 -> 6,602; LG06 1,643 -> 1,643: -2 +2 from `readFlagID`'s key parameters).
- Call-site census (fresh deprecation build, exact): 381 sites in 56 files (was 498 / 71 at the session start): ECHARPTR 136, Data 130, Format 70, Compare 13, `==` 10, MakeUpper 8, Remove 7, `!=` 3, MakeLower 2, Find 1, Insert 1.
  Per batch: B1.4 60, B1.5 45, B1.6 (editor model) 135, B1.7 26, B6 50; includers / gui widgets the compiler forces: 65 in 13 files (gui/aanim 17, gui/aedit 13, editor/EditorData_save 9, ...). B1.3: 0 rows left.

## NEXT, in order
1. The grouped Opus after-review RA1 (B1.0-B1.3) is the orchestrator's call; do not request it from the lane.
2. B1.4a per `cxx23-step4-b1-notes.md` section 8b (`logisticsmissioninfo{,_progress,_query}` + header, the `LogisticsData` pass-throughs, forced includers `logistics.cpp`, `missionbegin.cpp`, `missionselectionscreen.cpp`, `missionresults.cpp`,
   `missionbriefingscreen.cpp`, `mainmenu.cpp`, `pausewindow.cpp`, `saveload.cpp`): the null-returning `const char*` getters return `std::string` by value (empty = none). Proof additionally: a campaign save before / after (`LogisticsMissionInfo::save/load`),
   a campaign-start boot whose log shows no `""` playback, determinism, tier1. Then B1.4b (`logisticsdialog`, the rest of `logisticsdata*.cpp`: 22 + ... remaining rows), B1.5, B1.6 / B1.7 (editor), B6.
   Per batch: build, `--drop`/`--remap` the call-site rows (`py -3 tests/analyze/estring_callsites.py --remap HEAD <edited files>`, then the board row), `cxx17_ratchet --write-baseline --worktree` with the reason, commit by pathspec.
3. Outside B1 (notes sections 8 and 8b): `GameOS/gameos/vulkan_context.cpp:109` `ctxlog(... env)` passes a `std::string` (lane 2, then add `gameos` to the `/we4840` list); the Mechlopedia vehicles tab boot STOP; `LogisticsPilot::getAudio/getVideo/getIconFile`
   have no caller; `FitIniFile::readIdString` joins the first two lines of a multi-line value; saves carry vftable pointers (byte diffs of two builds are noisy by construction).
4. Carried over: ABL int/real out-array slice, the 8 tight ABL debug strings (needs the owner), mc2_07 tier2 on an idle host, save round-trip baselines, the non-mover zero-vector question for ABL.

## Tool notes
- **Bash heredocs eat backslashes in Python too** (`"\\n"` became a real newline again twice; a `sed` with `\\b` silently did nothing and a census ran with the wrong regex). Write any script that carries an escape with the Write tool, use `chr(10)` in
  a heredoc, and edit with `tools/dev/apply_edit.py edits.json` (the Write tool for the JSON; CRLF-aware, all-or-nothing).
- A background build / run started with `run_in_background` and several `Monitor` waiters on one log all fire: start ONE waiter, or an `until grep -q ... ; do sleep 5; done` Bash with a timeout. Foreground `sleep` is blocked.
- `tools/saveload_roundtrip.py resave --mission mc2_01 --out DIR` (no `--duration` above 30) twice per exe; `rm -rf DIR` first; `--deploy` builds and deploys HEAD. Per-offset analysis of differing records: a few lines over `save_inspect.load`.
- Post-commit proof chain (about 45 min on an idle host): deploy (`py -3 -c "import sim_determinism as sd; sd.build_and_deploy()"` from tools/), saves x2, `sim_determinism check` mc2_03 / mc2_10 / mc2_24 and mc2_16 with `--env MC2_FIXTURE_ORDER_LIVENESS=1`,
  `scripts/mechbay_preview_harness.py <screen> --timeout 60 --frame 600 --out F` (MC2_CURSOR_GUARD_OFF=1) for bay, purchase, loadout, launch, encyclopedia, encyclopedia_pilots, then tier1. Do not build or commit C++ while it runs (the fingerprint gate).
- Exact census recipe (not committed): `[[deprecated("B1")]]` on the public API of `mclib/estring.h` (declarations only, no `friend` lines; the regex for the symbol operators must NOT end in `\b`), `py -3 tools/build_all_targets.py --root-only -j16 --keep-going > log`
  (about 12 min), `py -3 tests/analyze/estring_callsites.py log`, `git checkout -- mclib/estring.h`. Touching estring.h rebuilds the dev tree afterwards (about 8 min).
- `cxx17_ratchet --check` reads the COMMITTED tree by default: use `--worktree` / `--staged` before committing; `git add -N` a new file before `log_tag_census.py --check` / `callers.py`; regenerate `tests/callers.json` after ANY line shift in CMakeLists.txt or check-contracts.sh.
- Fingerprint: committing code after the build makes the deployed exe stale against HEAD; doc commits do not.

## Rules (unchanged)
- Commit by explicit pathspec after `git diff --cached --name-only`; `git commit -F <file>`; trailer `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Commit BEFORE build + deploy. Launches only through `scripts/run_smoke.py --deploy 0.4` /
  `tools/sim_determinism.py` / `tools/saveload_roundtrip.py` / `scripts/mechbay_preview_harness.py` / `scripts/run_editor_smoke.py`, background-only, never `--no-lease`. Never kill cl/ninja by name (`tools/kill_build_tree.py`). Never contort code
  for a ratchet (rewrite the baseline with the reason). Never touch GameOS/, RenderCore/, shaders/, Gates/ beyond a task's need, nor tests/unit/test_gate.cpp, nor gos_static_prop*, nor code/multplyr*.
