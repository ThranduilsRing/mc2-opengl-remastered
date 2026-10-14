# E2 closing-review fixes before B1.7

Base db270b3f0, branch claude/codex-2. These fixes are a separate first
commit; the B1.7 conversion remains the second commit. No git index writes.

## Review findings and resulting behavior

- Oversized strings in objective FIT blocks fail document loading before
  clearing the current mission. The preflight measures the source directly,
  before the legacy FIT reader's fixed buffer, and emits one versioned line
  naming mission, block/field and length. Missing fields keep their existing
  defaults. No FIT writer changes. B1.6's earlier load-success behavior and
  proof wording are corrected in e2-b1.6-proof.md.
- Editor objects have an instance ID distinct from their catalog type ID.
  New instances receive distinct IDs; undo copies retain the original ID.
  Refresh undo resolves by that ID, so coincident units with the same type
  and squad recover their own authored definitions. The counter is a scalar,
  not a static-duration owner; IDs are editor-session state, never FIT bytes.
- All EDITOR_DOCUMENT and EDITOR_DEFINITIONS emitters use v1. The workflow
  parser requires the new rejection and duplicate-unit facts and rejects old
  tags, unsuccessful facts, or extra rejection diagnostics. Tests preserve
  the existing allowance for repeated identical panel facts.
- Both design documents now record the owner ruling: ABL camera getters
  return the last scripted camera value. Stock mc2_01, mc2_10 and tut_01
  save/restore around cutscenes; mc2_01 restoring scripted rotation at its
  mid-mission read is an accepted difference. This is a documentation fix.

## Proof

- `py -3 tools/build_quiet.py --target EditRel`: PASS, exit 0, 55 warnings,
  46.0 seconds. Initial build exposed a missing mc2str include; corrected.
- `py -3 scripts/run_editor_smoke.py --exe scratch/editor-e0-install/Mission Editor.exe --case workflows --prepare-stock-scratch --keep-logs`:
  PASS 1/1 after the ordinary host-lease wait. Report:
  scratch/editor-smoke/2026-10-13T16-35-53/report.md. The deployed binary is
  copied from the final RelWithDebInfo editor build, never run directly.
- The workflow log proves duplicate_unit_refresh_undo_redo=ok=1, and for
  Title, Description, ActivateFlagID and ResetStatusFlagID each 2001-byte
  fixture records rejected=1 unchanged=1 ok=1 and exactly one mission/field/
  length diagnostic. Serialized FIT bytes, live terrain/unit identities,
  dirty status and undo/redo availability remain unchanged; retained redo
  and undo also execute successfully. Other malformed-literal checks pass.
  MissionDefinitions and UnitBrain retain reached=1, authored=1, undo=1,
  saved=1 and reloaded=1, with their action-handler coverage labels.
- `py -3 tests/analyze/analyze_dangling.py --check` over all 18 edited C++
  files, with scratch/e2-review-compile_commands.json and the real clang-cl
  scratch/e2-review-clang.cmd context: PASS, zero diagnostics, zero missing
  verdicts; all four seeded shapes caught and clean twin silent.
- `py -3 tools/build_contract_harnesses.py --run --unit`: PASS, 20 harnesses
  and 2794 doctest cases, one expected-failure assertion and one skip.
- `py -3 tools/build_contract_harnesses.py --run --unit --asan`: PASS,
  2794 cases. No C++ files under tests changed. PowerShell's stderr redirect
  marked the first wrapper invocation failed despite doctest success;
  repeated with explicit exit $LASTEXITCODE, exit 0.
- `py -3 -B -m pytest scripts/tests/test_editor_smoke.py -q -p no:tmpdir -p no:cacheprovider`:
  PASS, 30 tests and 37 subtests. The initial duplicated fixture failure was
  corrected to duplicate panel facts while retaining single rejection logs.
- `py -3 tools/cxx17_ratchet/cxx17_ratchet.py --report --worktree --jobs 1`:
  TOTAL sum 16197, identical to B1.7 before and after. Baseline untouched.
- `py -3 scratch/e2_review_proof.py --census after`: PASS, all 722 target/TU
  entries, 131 sites in 24 files and zero failed TUs. B1.7 remains 173 -> 131
  sites, with all 26 listed and 16 forced editor sites removed. No new sites.
  `py -3 tests/analyze/estring_callsites.py --check`: PASS, 131 sites.
- `py -3 scripts/run_editor_smoke.py --exe scratch/editor-e0-install/Mission Editor.exe --prepare-stock-scratch --keep-logs`:
  PASS 40/40, exit 0, no retries, after the normal host-lease wait. Report:
  scratch/editor-smoke/2026-10-13T16-39-46/report.md. All 24 stock comparisons
  are byte-identical (diff_size=0) with both frozen hashes matching. The new
  duplicate-unit and four oversized-field proofs pass again. Task A's
  MissionDefinitions and UnitBrain action-handler rows have reached=1,
  authored=1, undo=1, saved=1 and reloaded=1. Stock dependency manifest:
  e0d9806424c5dddedef539b9293daca56036e68809cb779d967248e820b4f741.
- Built and deployed editor SHA-256 both equal
  8dafd00f6840674785d028ae92c495dbf84a7ffdda9a9cc0511264758bd7ee60.
  Frozen baseline SHA-256 remains
  3347dc2f30fefb1606ad9e6608807c348dbc2023018142b37274568b097bd61b;
  ledger remains c55a87c457288bc1cdaf22dcbdc02bec9a011954d98857a3ba376257ccc1b1ce.
- `py -3 scratch/e2_review_patches.py --verify`: PASS; the two ordered patches
  reproduce all 26 changed paths without writing the index. LF is preserved
  and git diff --check passes. Protected paths have no diff.
- `py -3 tools/build_all_targets.py`: attempted during B1.7; stalled at
  Ninja's Re-checking globbed directories probe and was interrupted. The
  orchestrator runs it. Bash board not run: Git Bash is forbidden in this
  sandbox. Prior board doc_links/manifest_schema findings are pre-existing.

## Log-tag baseline requested diff

`py -3 tools/log_tag_census.py --check` exits 1 for the intended changes.
1393 emitters/1067 distinct become 1396/1069. No baseline file is written;
the orchestrator owns its adoption. Exact changed rows (all stderr):

| Tag | Keys | Baseline | Now |
|---|---|---:|---:|
| EDITOR_DEFINITIONS | preservation_commands_undo_reload_malformed | 1 | 0 |
| EDITOR_DEFINITIONS | stage,field,ok | 1 | 0 |
| EDITOR_DEFINITIONS | stage,ok | 5 | 0 |
| EDITOR_DEFINITIONS v1 | preservation_commands_undo_reload_malformed | 0 | 1 |
| EDITOR_DEFINITIONS v1 | stage,field,ok | 0 | 1 |
| EDITOR_DEFINITIONS v1 | stage,field,rejected,unchanged,ok | 0 | 1 |
| EDITOR_DEFINITIONS v1 | stage,ok | 0 | 6 |
| EDITOR_DOCUMENT | (none) | 2 | 0 |
| EDITOR_DOCUMENT v1 | (none) | 0 | 2 |
| EDITOR_DOCUMENT v1 | mission,field,length,rejected | 0 | 1 |

The parser now consumes the versioned EDITOR_DOCUMENT/EDITOR_DEFINITIONS
tags. B1.7 alone leaves log tags unchanged; these changes belong entirely
to the review-fix commit and its new regression proofs.

## Proposed first commit

```text
fix(editor): reject oversized text and undo by instance

Preserve the open document when objective text is oversized; measure and
reject it before the legacy reader. Restore refresh definitions by stable
instance identity. Add rejection/state and duplicate-unit workflow proofs,
version their tags/parsers, and record the scripted camera owner ruling.

Unlisted behaviour changes:
- Oversized objective text now fails document loading and preserves the
  open state, rather than becoming absent/defaulted on a successful load.
- Refresh undo/redo restores each coincident unit's authored definition.
- Document/definition diagnostics are versioned; smoke requires the proofs.
- Camera wording documents the owner ruling; no camera runtime code changes.
- Stock FIT output bytes and frozen round-trip records remain unchanged.
```

Exact first-commit pathspec:

```text
.claude/cleanup/cxx23-step4-save-schema-design.md
.claude/cleanup/e2-b1.6-proof.md
.claude/cleanup/e2-closing-review-proof.md
.claude/cleanup/sim-kernel-design.md
editor/AuthoredMissionState.h
editor/EditorData_terrain.cpp
editor/EditorObjects.cpp
editor/EditorObjects.h
editor/MissionDefinitionCommands.cpp
editor/MissionDefinitionSmoke.cpp
editor/Objective.cpp
scripts/run_editor_smoke.py
scripts/tests/test_editor_smoke.py
```

The sole shared path is EditorData_terrain.cpp: the first commit adds only
the mc2str include and pre-clear objectiveTextLengths call. All other hunks,
including the mc2fmt include and metadata reader conversion, belong to B1.7.
scratch/e2-review-fixes.patch and scratch/e2-b17-only.patch preserve this
split without changing the index; the second patch applies after the first.
B1.7's exact 14-path list and proposed message remain in e2-b1.7-proof.md.
The orchestrator separately adopts log_tag_baseline.json in the first commit.
