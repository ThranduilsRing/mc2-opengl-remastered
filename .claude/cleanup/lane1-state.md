<!-- doc-status: current | reviewed:2026-10-13 -->
# Lane 1 state (builder on A:/Games/mc2-main, branch claude/nifty-mendeleev). Hand-over file for the next agent; keep current at every report.

Session 2026-10-13 morning (lane-1 Sonnet, about 2.5 h): the RA1 and cleanup-review fixes, the G2 campaign-save round trip across four exes, then step 4 batches B1.4a and B1.4b (the logistics model B). B1.5 was NOT started. Never pushed. Install `mc2-win64-v0.4` holds the exe of 2d68841a4 (B1.4b).
Lane 2 untouched (`tools/save_inspect.py`, `tools/saveload_roundtrip.py`, `scripts/background-launch-allow.json` were edited after `check_conflict` said clear). The scratch worktree `A:/Games/mc2-g2` used for the old exes is removed. Design and results: `cxx23-step4-b1-notes.md` sections 8b-8d; reviews: `cxx23-step4-b1-after-review-1.md`, `reviews/lane1-2026-10-13-b1.3-cleanup.md`.

## Commits this session (oldest first)
- 2c577a5ed RA1 F1 byte-offset save-noise mask (`tools/save_noise_mask.json`, `save_inspect.py diff --mask [--across-builds]`, `ranges`), RA1 F2 pilot-name sort probe (`tests/analyze/pilot_sort_probe.py`: 0 of 1953 pairs move), RA1 F4 + cleanup F6 (`objective_text::readChecked`, one over-long flag ID skips one entry, the limit is inclusive),
  cleanup F2 (callers.json), F3 (the call-site list records a SHA-1 per file; `--check` fails for a file edited since its rows were verified; `--remap` / `--drop` refresh it, `--seal` after a census), F5 (file_stem docs + tests), F7 (Team clamp / reject), F9 (ci_less deleted, total-order test), console_child for the git child (background_launch row green again). RA1 F5 NOT applied (does not compile: `up.icon` is a UIAssetName).
- e3a2940ad docs: notes 8b (A1-A6), 8c (corrections, unlisted behaviour changes, sort probe, mask), sections 5 / 6 / C3 corrected; LG09 of B1.3a is -22.
- ffc879042 B1.4a: `LogisticsMissionInfo` + pass-throughs + includers on `std::string`; `objective_text::readPlain` is the one FIT text reader; two pre-video log lines; `mc2::str::erase_first`; `tools/campaign_save_roundtrip.py`; `saveload_roundtrip.py resave --plant/--mask`. 25 edited files.
- 2d68841a4 B1.4b: logistics dialogs, `s_deployedMechSnapshot` as a member, `loadMech` / `LogisticsVehicle::init` readers, `logisticsdata.h` loses estring.h; `analyze_dangling --check` passes `-Wno-c++11-narrowing`. 9 edited files.
- (this commit) docs: notes 8d, this file.

## G2 result (notes 8d)
A campaign start populates the inventory (`InventoryCount = 3`, 52 pilots). The LogisticsData text packet of a campaign save at mc2_05 is byte-identical (68237 bytes) when written by 1266aa1a2, 45f39522a and 2d68841a4. Each of 45f39522a, ffc879042 and 2d68841a4 loads the 1266aa1a2 save (and the 45f39522a save) and re-saves identical logistics text and identical pilot text bytes.
A crafted save with a player variant (spelt differently in the inventory) re-saves identically under all four exes and differs from the crafted file in exactly two lines (the canonical `Variant` spelling, `Designer = FALSE`).

## Proof numbers
- B1.4a, B1.4b: `build_all_targets` rc 0 each; doctest 2794 passed (plain and ASan); `analyze_dangling --check` 17 and 6 TUs, 0 diagnostics, 0 without verdict; exact census 381 -> 335 -> 308; own_inventory 1 fell (the static vector), baseline written; ratchet LG06 +2 (`readPlain`'s `const char* key`, the FitIniFile C boundary) in 4a, 4 fewer in 4b, baselines rewritten with the reasons.
- `sim_determinism check` PASS after each: B1.4a mc2_03 2413 frames, mc2_24 3560, mc2_16 liveness 2446; B1.4b mc2_03 2416, mc2_24 3556, mc2_16 2447; mc2_10 PASS in both. First 1000 frames equal the baselines.
- Captures bay / purchase / loadout / launch / briefing viewed (`A:/Games/mc2-capture-evidence/b1-after-B1.4a/`, `b1-after-B1.4b/`): same text, lists and order; the only pixel differences are the idle mech animation (bbox about 294-350 x 287-345) and a fade of at most 21 of 255. Loadout shows `<missing-string>` placeholders in every capture, also before B1.3.
- tier1 5/5: `2026-10-13T08-59-02` on ffc879042, `2026-10-13T09-38-40` on 2d68841a4. Boot-log positive control: `missionbegin: campaign-start pre-video: cinema1.bik`, `logistics: pre-video after a won mission:` `none` x10 and `cinema2.bik` x1 in every campaign soak log of the new exes.
- Idiom KPI (`cxx17_ratchet --report` TOTAL, 21 idioms summed, committed tree): 16,217 at the session start -> 16,194 now (LG09 6,602 -> 6,588; LG05 506 -> 494; LG03 2,393 -> 2,392; LG06 1,643 -> 1,647).

## Remaining call-site table (exact deprecation census at 2d68841a4: 308 sites in 42 files; `.claude/cleanup/cxx23-step4-b1-callsites.txt`)
| batch | sites | files |
|---|---|---|
| B1.4 | 0 | done |
| B1.5 (front-end screens: mechlopedia 20, mechlabscreen 5, infowindow 5, mechlistbox 3, pilotreadyscreen 3, ...) | 41 | |
| B1.6 (editor model: Objective.cpp 43, ObjectiveDlg 26, Objective_actions 24, Objective_conditions 15, ...) | 135 | |
| B1.7 (editor data: CampaignData 10, ...) | 26 | |
| B6 (multiplayer screens) | 49 | kept, convert with the gui widgets |
| includers / gui widgets (gui/aanim 17, aedit 13, alistbox 6, asystem 3, editor/EditorData_save 9, ...) | 57 | |
By kind: Data 127, operator ECHARPTR 78, Format 69, `==` 10, MakeUpper 8, Compare 5, Remove 5, `!=` 3, Find / MakeLower / Insert 1 each.

## NEXT, in order
1. B1.5 front-end screens (`mechlopedia` H6 / H12 with the encyclopedia captures and the `encyclopedia_vehicles` STOP first, `infowindow` `%ld/%ld  %s` formats, `mechlabscreen`, `mechlistbox`, `pilotreadyscreen`, `salvagemecharea`, `missionbriefingscreen.h` objectiveModels). The mechlopedia sort at `mechlopedia.cpp:1328` needs the same stock-name probe as the pilot list (`pilot_sort_probe.py` is the template). Per batch: build, exact census, `--remap`/`--drop` + `--seal`, `cxx17_ratchet --write-baseline --worktree` with a reason, `own_inventory`, determinism, `campaign_save_roundtrip` chain, captures viewed, tier1.
2. G6: the editor `-mission` leg of the R12 probe on the OLD editor before B1.6 starts (`run_editor_smoke.py`, `-mission=<stock mission with objectives, flags, blurb> -smoke-save`), then B1.6 / B1.7 and the second Opus after-review (editor batches), B6.
3. Outside B1: `MissionBegin::updateSplash` consumes the campaign pre-video without playing it (notes 8b; an owner ruling); `GameOS/gameos/vulkan_context.cpp:109` `ctxlog(... env)` passes a `std::string` (lane 2, then add `gameos` to `/we4840`); the Mechlopedia vehicles tab STOP; `FitIniFile::readIdString` joins the first two lines of a multi-line value; saves carry vftable pointers and uninitialised `TacticalOrder` words (zero them at write time in the save-format arc); `LogisticsPilot::getAudio/getVideo/getIconFile` have no caller.
4. Tooling asks: a reviewed per-line marker for sanctioned C-boundary `const char*` parameters in `cxx17_ratchet` (RA1 section 8); `callers.json` should store `reference` root-relative (lane 2's `tools/`); the encyclopedia harness ignores `--campaign` / `--win-after-sec` (cleanup F10).

## Tool notes
- **Bash heredocs eat backslashes in Python** and a `sed -i` through the Bash tool STRIPS CR from CRLF files (it converted `missionselectionscreen.cpp` once): write any script with an escape using the Write tool, edit with `tools/dev/apply_edit.py edits.json` (the Write tool for the JSON; raw tabs are not valid JSON, write `\t`), and check `git ls-files --eol` after any other edit. Several files are CRLF (`logisticsdialog.*`, `logisticsmissioninfo.*`, `saveload.cpp`, `logistics.cpp`, `mainmenu.cpp`, `mpstats.cpp`, `missionbegin.h`).
- A command that both starts a background job and prints loses its own output in the tool result: run the foreground part in its own call.
- `analyze_dangling --check FILES --compile-commands build64-ninja/compile_commands.json` needs `vswhere` on PATH: `export PATH="$PATH:/c/Program Files (x86)/Microsoft Visual Studio/Installer"`.
- Exact census recipe: `py -3 <deprec.py> on` (marks the public API of `mclib/estring.h` `[[deprecated("B1")]]`, uncommitted), `py -3 tools/build_all_targets.py --root-only -j16 --keep-going > log` (about 2 min once the tree is warm), `py -3 tests/analyze/estring_callsites.py log --out /tmp/census.txt`, copy it over `cxx23-step4-b1-callsites.txt`, `--seal`, `git checkout -- mclib/estring.h`; the dev tree then rebuilds (about 6 min).
- Campaign round trip: `py -3 tools/campaign_save_roundtrip.py capture --out DIR --save-at 2400` (about 100 s, host lease), `... compare A B`, `... add-variant SRC DST`, `py -3 tools/saveload_roundtrip.py resave --mission mc2_05 --plant SAVE --save-at 1000000000 --load-at 1100 --out DIR`. A capture lands in mc2_04 or mc2_05 depending on timing; `MissionFileName` in the logistics text says which. Old exes: `git worktree add --detach A:/Games/mc2-g2 <sha>`, `py -3 scripts/configure_ninja.py --build` there (about 4 min), `py -3 scripts/deploy_payload.py --target game --ninja` from there; remove the worktree afterwards.
- Post-commit proof chain (about 25 min on an idle host): `py -3 -c "import sim_determinism as sd; sd.build_and_deploy()"` from tools/, determinism x4, campaign capture + three loads, captures, tier1. Do not edit or commit C++ while it runs (fingerprint gate); lane 2's tier1 holds the host lease and a capture waits for it.
- `cxx17_ratchet --check` reads the COMMITTED tree by default (use `--worktree`); `log_tag_census` / `callers.py` need `git add -N` for a new file; regenerate `tests/callers.json` after any script change; new scripts under `tools/` need a `tests/callers_allowlist.json` entry (and `scripts/background-launch-allow.json` when they launch the engine).

## Rules (unchanged)
- Commit by explicit pathspec after `git diff --cached --name-only`; `git commit -F <file>`; trailer `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Commit BEFORE build + deploy. Launches only through `scripts/run_smoke.py --deploy 0.4` / `tools/sim_determinism.py` / `tools/saveload_roundtrip.py` / `tools/campaign_save_roundtrip.py` / `scripts/mechbay_preview_harness.py` / `scripts/run_editor_smoke.py`, background-only, never `--no-lease`. Never kill cl/ninja by name. Never contort code for a ratchet (rewrite the baseline with the reason). Never touch GameOS/, RenderCore/, shaders/, Gates/ beyond a task's need, nor tests/unit/test_gate.cpp, nor gos_static_prop*, nor code/multplyr*.
