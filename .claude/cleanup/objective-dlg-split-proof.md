# Objective dialog text split

Base c14b6fbb7, branch claude/codex-2. No index writes or commits.

## Responsibility and preservation

- ObjectiveDlg.cpp keeps the MFC dialog, message map, list synchronization,
  resource-string controls and detail-view actions. It is 1329 lines, down
  from 1504. Its mc2str include remains because the dialog uses from_c.
- ObjectiveDlg_text.cpp owns the two existing plain-English formatters,
  objReadableCondition and objReadableAction, plus their format include.
  It is 180 lines. Both helper bodies are byte-identical to the base.
- ObjectiveDlg_text_internal.h is a 10-line declaration header using forward
  declarations and string. No class body, global state, initialization order,
  function signature or linkage changes. Every piece is below 1500 lines.
- The editor CMake source/header globs already cover the new files; no CMake
  source change is needed. The existing VS tree was explicitly regenerated
  before the successful build so the newly discovered source participates.
- The split follows giant-file-split-plan.md: complete responsibilities move
  intact. tools/split/allow/ObjectiveDlg.json permits only the extra includes,
  pragma and forward declarations; it permits no body edits or deletions.
  docs/split-map.md records the new piece through the existing generator.
- No FIT reader/writer, objective text rule, dialog command or log emitter is
  changed. No ratchet baseline or background-launch inventory is edited.

## Proof

- `py -3 tools/file_size_ratchet.py --check --worktree`: PASS. Editor max
  lines 1504 -> 1436, editor oversized files 1 -> 0; repo oversized count
  48 against baseline 49. The inherited code/missionbegin.cpp +7 warning
  remains, with no changes to that file. Baseline untouched.
- `py -3 tools/file_size_ratchet.py --check`: reads committed HEAD, so still
  reports the pre-split 1504-line file until the orchestrator commits. This
  sandbox cannot write the index. The worktree check is the proof of this
  proposed commit; rerun the plain command after committing.
- `py -3 tools/split/split_verify.py --base c14b6fbb7 --src editor/ObjectiveDlg.cpp --stem ObjectiveDlg --allow tools/split/allow/ObjectiveDlg.json --run-selftests`:
  PASS, zero findings. An independent exact comparison also confirms both
  formatter bodies match the base. No script-name consumers need remapping.
- `py -3 tools/split/split_map.py --check`: PASS after the new map entry.
- CMake `-S . -B build64`: PASS, refreshed existing VS build files. The first
  build had unresolved formatter symbols while using the stale source list;
  regeneration and the ordered rebuild resolve them.
- `py -3 tools/build_quiet.py --target EditRel`: PASS, exit 0, zero warnings,
  44.9 seconds, after regeneration.
- `py -3 tests/analyze/analyze_dangling.py --check editor/ObjectiveDlg.cpp editor/ObjectiveDlg_text.cpp editor/ObjectiveDlg_text_internal.h --compile-commands scratch/objective-dlg-compile_commands.json`:
  PASS, zero lifetime diagnostics in all 3 files, zero missing verdicts;
  all four seeded shapes caught and the clean twin silent. The normal
  RelWithDebInfo editor flags are used with real clang-cl via
  MC2_TOOL_CLANG_CL=scratch/e2-review-clang.cmd. The new header is self-contained.
- `py -3 tools/build_contract_harnesses.py --run --unit`: PASS, 20 harnesses
  and 2794 doctest cases, one expected-failure assertion and one skip.
- `py -3 tests/analyze/estring_callsites.py --check`: PASS, 131 sites. The
  split moves no EString use and changes no remaining census row.
- `py -3 tools/log_tag_census.py --check`: retains the exact inherited
  review-fix baseline diff documented in e2-closing-review-proof.md; the
  split adds no emitters or tag changes. That baseline remains untouched.
- `py -3 -u scripts/run_editor_smoke.py --exe scratch/editor-e0-install/Mission Editor.exe --prepare-stock-scratch --keep-logs`:
  PASS 40/40, exit 0, no retries, including Task A's MissionDefinitions and
  UnitBrain action-handler rows. All 24 stock FIT files are byte-identical
  with both frozen hashes matching. Report:
  scratch/editor-smoke/2026-10-13T17-27-54/report.md. The stock preparation
  and normal shared-host lease wait completed before the engine cases.
  Dependency manifest SHA-256 remains
  e0d9806424c5dddedef539b9293daca56036e68809cb779d967248e820b4f741.
- `git diff --check`: PASS. The ratchet baseline, frozen mission baseline,
  round-trip ledger and all lane-owned code/test paths have no diff.

Built and deployed editor SHA-256:
586140a797c7c4a82cbc030dcf4763e6fe00383bc10c5d54bb2c8339da698184.
Ratchet baseline SHA-256:
00fbe26544da7a517747f62b16d670904b1444484b9d98dacef6caaef0197585.
Frozen mission baseline SHA-256:
3347dc2f30fefb1606ad9e6608807c348dbc2023018142b37274568b097bd61b.
Ledger SHA-256:
c55a87c457288bc1cdaf22dcbdc02bec9a011954d98857a3ba376257ccc1b1ce.

## Proposed commit

```text
refactor(editor): split objective text formatting

Move readable condition/action formatters intact into their own source.
Keep MFC dialog responsibilities separate and refresh the split map.
Every piece is below 1500 lines; leave the ratchet baseline unchanged.

Unlisted behaviour changes:
- None. Formatter bodies, signatures and linkage are unchanged.
```

Exact pathspec:

```text
.claude/cleanup/objective-dlg-split-proof.md
docs/split-map.md
editor/ObjectiveDlg.cpp
editor/ObjectiveDlg_text.cpp
editor/ObjectiveDlg_text_internal.h
tools/split/allow/ObjectiveDlg.json
```

No files deleted. Existing LF endings preserved; all new files use LF.
docs/background-launch-inventory.md was already dirty and is left untouched.
