# Editor round-trip ledger (E0)

Summary: 0 byte-identical, 0 lossless-only, 24 semantic; 0 blocked of 24.

Owner finding: dev `mc2-win64-v0.4` AND golden `mc2-win64-v0.4c` ship non-stock loose `data/art/Buildings.csv`, SHA-256 `86153dd5c4793672d35033ce4e8e90a871df8b1e131ccde7e8d61ab1cf083ae9`. Neither install was changed. The stock art.fst entry is `3a1b449d5e4336c7e2345ebd577181fcc7f9f41107cbc1ffa6da7e0ff9b6ebce`.

Dependency manifest: `scratch/editor-smoke/2026-10-13T12-52-11/dependencies.json`; SHA-256 `e0d9806424c5dddedef539b9293daca56036e68809cb779d967248e820b4f741`. Frozen exact-byte baseline: `tests/smoke/editor/roundtrip-baseline.json`; changes fail unless explicitly rebaselined.

Stock inputs come from mission.fst; all resolved archive entries and loose data are fingerprinted, and non-stock loose overrides are rejected. Each row loads the stock PAK through -mission and writes a separate PAK/FIT through -smoke-save (quick save). This proves the FIT writer and editor reload payload, not a full MOVE/tac-map export.

Diff size = unequal bytes at matching offsets + the absolute length difference. Byte equality is unmasked; the field census explains changes without blessing them.

Evidence: `scratch/editor-smoke/2026-10-13T12-52-11` (input FIT, written FIT, unified text diff, SHA-256 JSON and report).

Campaign representatives: group 0: mc2_01; group 1: mc2_02; group 2: mc2_03; group 3: mc2_04; group 4: mc2_05; group 5: mc2_06; group 6: mc2_10; group 7: mc2_11; group 8: mc2_14; group 9: mc2_18; group 10: mc2_19; group 11: mc2_23; group 12: mc2_24.

| Mission | Byte-identical yes/no | Diff size | Explanation |
|---|---|---:|---|
| mc2_01 | no | 12 | FINDING: semantic; classes: Cameras, NavMarker coordinates, Objective condition coordinates. 4 altered, 0 added, 0 removed fields; see evidence. |
| mc2_02 | no | 10 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 3 altered, 0 added, 0 removed fields; see evidence. |
| mc2_03 | no | 6 | FINDING: semantic; classes: Cameras. 2 altered, 0 added, 0 removed fields; see evidence. |
| mc2_04 | no | 9 | FINDING: semantic; classes: Cameras, NavMarker coordinates. 3 altered, 0 added, 0 removed fields; see evidence. |
| mc2_05 | no | 16 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 5 altered, 0 added, 0 removed fields; see evidence. |
| mc2_06 | no | 10 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 3 altered, 0 added, 0 removed fields; see evidence. |
| mc2_07 | no | 5 | FINDING: semantic; classes: Cameras. 2 altered, 0 added, 0 removed fields; see evidence. |
| mc2_08 | no | 10 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 4 altered, 0 added, 0 removed fields; see evidence. |
| mc2_09 | no | 22 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 7 altered, 0 added, 0 removed fields; see evidence. |
| mc2_10 | no | 7 | FINDING: semantic; classes: Cameras. 2 altered, 0 added, 0 removed fields; see evidence. |
| mc2_11 | no | 22 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 7 altered, 0 added, 0 removed fields; see evidence. |
| mc2_12 | no | 7 | FINDING: semantic; classes: Cameras. 2 altered, 0 added, 0 removed fields; see evidence. |
| mc2_13 | no | 17 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 6 altered, 0 added, 0 removed fields; see evidence. |
| mc2_14 | no | 17 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 6 altered, 0 added, 0 removed fields; see evidence. |
| mc2_15 | no | 13 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 4 altered, 0 added, 0 removed fields; see evidence. |
| mc2_16 | no | 11 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 4 altered, 0 added, 0 removed fields; see evidence. |
| mc2_17 | no | 10 | FINDING: semantic; classes: Cameras, NavMarker coordinates. 3 altered, 0 added, 0 removed fields; see evidence. |
| mc2_18 | no | 13 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 4 altered, 0 added, 0 removed fields; see evidence. |
| mc2_19 | no | 24 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 8 altered, 0 added, 0 removed fields; see evidence. |
| mc2_20 | no | 10 | FINDING: semantic; classes: Cameras, NavMarker coordinates. 3 altered, 0 added, 0 removed fields; see evidence. |
| mc2_21 | no | 20 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 7 altered, 0 added, 0 removed fields; see evidence. |
| mc2_22 | no | 13 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 4 altered, 0 added, 0 removed fields; see evidence. |
| mc2_23 | no | 9 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 3 altered, 0 added, 0 removed fields; see evidence. |
| mc2_24 | no | 10 | FINDING: semantic; classes: Cameras, Objective condition coordinates. 3 altered, 0 added, 0 removed fields; see evidence. |

## Writer findings

The stock rerun changes 99 fields: 48 camera fields, 4 NavMarker coordinates
and 47 objective-condition coordinates. There are no added or removed keys.
All three classes are semantic value changes; different decimal coordinates
are not identical values. Mission writers were not repaired.

| Class | Fields | Representative input → written | Writer and cause |
|---|---:|---|---|
| Cameras | 48 | mc2_01 `[Cameras]` `f NewScale = 0.444542` → `f NewScale = 0.821212` (also `ZoomMax` 1 → 2) | `editor/EditorData_save.cpp:770` calls `Camera::save`; `mclib/camera.cpp:797` and `:801` emit live scale/zoom. Initialization resets maximum zoom at `mclib/camera.cpp:350` and derives scale from altitude/pitch at `:478`. |
| NavMarker coordinates | 4 | mc2_01 `[NavMarker0]` `f yPos = -789.333313` → `f yPos = -789.333496` | `editor/EditorObjects_pilot.cpp:515` writes live appearance position. `editor/EditorObjectMgr_io.cpp:698` deliberately omits FIT NavMarker loading; the binary building record is loaded and snapped to a cell center by `editor/EditorObjectMgr.cpp:626`. |
| Objective condition coordinates | 47 | mc2_01 `[Team0Objective1Condition0]` `f PositionY = -277.333344` → `f PositionY = -277.333496` | `editor/Objective_conditions.cpp:223` resolves a structure by position; `:242` and `:243` write the resolved structure's coordinates. Unit conditions similarly write live unit coordinates at `:110` and `:111`. The original decimal value is not preserved. |

### E-track findings retained; no writer repair

`Unit::save` re-derives Part ControlDataType/ObjectProfile from the current
appearance/catalog at `editor/EditorObjects_unit.cpp:79-86`, then writes those
values at `:172-173`; `Unit::load` does not preserve the stored control/profile.
`CObjective::Save` resolves ModelID through the current catalog at
`editor/Objective.cpp:543`, derives ModelType at `:548`, and writes it at `:559`.
The stored ModelType is not retained. These are writer preservation defects
even though the stock catalog produces no control/profile or ModelType change
in this rerun. The previous overlay's vehicle→mech changes are disqualified
as stock proof. CSVFile is also taken from the current catalog at
`editor/EditorObjects_unit.cpp:175-176`; the stock rerun has no CSV change.

The stronger UnitBrain probe exposed an editor-action undo defect: its snapshot
omitted orderAuthored, leaving persistence ownership changed after undo. The
snapshot now captures/restores that flag (`editor/Action.h:209`,
`editor/Action.cpp:647-678`); the probe verifies the previous ownership flag,
order type, stance and waypoint count, then saved/reloaded ownership and values.
This action repair does not alter the catalog-dependent mission writer findings.

### Stock environment and reproduction

The private runtime is `scratch/editor-e0-install`. All eight configured FSTs
and all 7,721 resolved archive entries are SHA-256 fingerprinted. Its 7,722
loose files are stock archive bytes except the two stock-derived files below.
The manifest records each loose path, loose hash, archive container hash and
archive-entry hash (or null when no archive entry exists). Unknown loose data
or a differing shadow fails before launch unless its exact hash has a reason.
The runner clears mod selection and disables launcher JSON overrides.

- `data/objects/object2.pak`: required loose stock-derived PacketFile, absent
  from FSTs. SHA-256 `dda3fd6c8e03e21fb88dd409fba3afd355f2f58fd34c9801ea9ab17a1f0d2488`.
  All 1,188 decoded packets match `mc2srcdata-fresh/objects/object2.pak`
  (SHA-256 `eb46be8f57acb49f52d8951af25725fd50a21f55ec66273c2be6e657bebaec28`,
  also byte-identical in mc2-srcdata); only packet compression differs.
  Checked with `py -3 scratch/inspect_editor_object_pak.py`: zero changed packets.
- `data/objects/badpilots.csv`: SHA-256
  `d7d84d0f79afd52c6f73072f41be8c7203a72b53c3cd018a9548b77487ea9fd8`.
  Stock misc.fst carries 44 compressed bytes and declares 44 uncompressed bytes,
  so the generic equal-size extraction returns the zlib payload. Decoding it
  yields the original four-name, 44-byte stock pilot catalog, byte-identical
  to mc2srcdata-fresh/objects/BadPilots.csv. No gameplay value was authored.

Both hash-pinned reasons are adopted in the baseline's stock_allowlist.
`--prepare-stock-scratch` removes/replaces overlays ONLY below this worktree's
`scratch/` runtime, records changes in preparation.json, and materializes stock
entries for direct-file and directory-enumeration consumers. It cannot touch
the dev or golden installs. The original loose tree remains preserved at
`scratch/editor-e0-overlay-backup`. Runtime-generated TXM/LST caches are removed
before the next proof; they are not silently exempted by the dependency audit.

Baseline creation used the full suite with `--rebaseline --prepare-stock-scratch`
and the two explicit stock-derived allowances. Reproduce the frozen gate with:

```text
py -3 scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --prepare-stock-scratch --case stock_roundtrip --keep-logs --exit-sec 3 --timeout 180
```

### Headless warning and workflow evidence

mc2_22 has 78 turrets. Its existing limit warning now honors g_cliSuppressModals
only: the default IDOK is taken and one warning fact names
`IDS_TOO_MANY_TURRETS default=IDOK`. Normal sessions still call AfxMessageBox.
The log appears twice in combined evidence because stderr and startup.log
carry the same IPC fact. mc2_22 writes a real comparison: 13 differing bytes,
four changed fields (two cameras and two objective-condition coordinates).

The workflow row invokes five production action paths: the material brush,
the SAME BuildingBrush activated by AssetBrowser, Inspector alignment,
UnitBrain waypoint placement, and mission quick-save/reload. Authored values
are checked, undo restores their preceding values, redo authors them again,
and a saved mission is reloaded after undo. Reload checks material, object ID
and position, team, order type, waypoint count and waypoint coordinates.

Objectives and campaign links remain explicitly `coverage=model-only reached=0`.
Their authored models Save/Read equal; snapshot restoration verifies model
undo. The read-only summaries do not become panel reaches by being opened.
The parser requires coverage, reach, authored, undo, saved and reloaded facts
and rejects absent, conflicting or unsuccessful facts. Inventory rows state
the same limits; none of the 37 legacy MFC dialogs is claimed instantiated.

### Artifact decisions

| Artifact | Decision and reason |
|---|---|
| `tests/smoke/editor/roundtrip-baseline.json` | ADOPT: all 24 stock input/output SHA-256 pairs and identical/differs/blocked outcomes, archive identities and two stock-derived allowances. E2's exact written-FIT floor, no masks; missing comparisons or any change fail. Only explicit --rebaseline may rewrite it, after all 24 successful comparisons, printing each change. |
| `tests/smoke/editor/old-editor-2026-10-13/.gitattributes` and `report.md` | RETAIN: orchestrator adopted them in 73b111f33 during this redo as historical before evidence. They describe the rejected non-stock environment and are not the frozen stock baseline. |
| `tests/smoke/editor/old-editor-2026-10-13/mc2_{01..21,23,24}.{written.fit,json,diff}` (69 files) | RETAIN: same historical bundle, qualified as rejected non-stock proof; no mc2_22 comparison exists in it. No file in this bundle was edited or deleted by this redo. |
| Earlier untracked report directories `2026-10-12T10-28-51`, `2026-10-12T17-58-12`, `2026-10-13T00-46-32` | Already absent at task start. This redo cannot reconstruct their individual prior deletion list. Historical lane2-state.md references remain outside the allowed paths; owner must reconcile them. |
| `scratch/editor-smoke/`, preparation/inspection helpers and original overlay backup | TRANSIENT: ignored evidence and reproducibility scratch, not adopted fixtures. Full resolved dependencies and comparison artifacts remain in the run directory named above. |

No tracked file was deleted by this redo. No engine code or mod/dev/golden
install was edited. Commits/index writes and Git Bash are forbidden here;
the orchestrator stages/commits the proposed paths and runs the full Bash board.

### Proof commands and results

- `py -3 tools/build_quiet.py --target EditRel`: final RelWithDebInfo rebuild
  PASS, exit 0, zero warnings, 42.5 seconds. A prior concurrent relink failed
  with LNK1201 writing the PDB while the editor was running; retry after the
  smoke exited succeeded. Copied only to the private scratch runtime. Build
  and scratch executable SHA-256 both equal
  `c61f21ba55d537c5f1badebcdeed15c4cff31684bfcd058d4da479d56ff908bf`.
- `py -3 -u scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --stock-allowlist scratch/editor-stock-allowlist.json --prepare-stock-scratch --rebaseline --ledger .claude/cleanup/editor-roundtrip-ledger.md --keep-logs --exit-sec 3 --timeout 180`:
  initial adoption PASS 40/40 in `2026-10-13T12-29-18`; printed all 24 new
  baseline pairs and archive identities. This was explicit baseline creation.
- `py -3 -u scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --prepare-stock-scratch --case stock_roundtrip --case workflows --keep-logs --exit-sec 3 --timeout 180`:
  frozen verification PASS 25/25 in `2026-10-13T12-43-22`, without rebaseline.
- `py -3 -u scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --prepare-stock-scratch --ledger .claude/cleanup/editor-roundtrip-ledger.md --keep-logs --exit-sec 3 --timeout 180`:
  FINAL rebuilt editor PASS 40/40 in `2026-10-13T12-52-11`, no retries.
  Original rows 15/15, stock rows 24/24 exact frozen pairs, workflow row 1/1.
  All three successful runs have the SAME dependency manifest hash reported
  at the top. Final preparation recorded 3,934 derived caches/generated loose
  files removed/replaced in that run's preparation.json, before auditing.
- `py -3 tools/build_contract_harnesses.py --run --unit`: final PASS 20/20
  harnesses and 2,794 doctest cases (one expected-failure control, one skipped).
- `py -3 -B -m pytest scripts/tests/test_editor_smoke.py -q -p no:tmpdir -p no:cacheprovider`:
  final PASS 27 tests and 28 subtests. Negative controls cover stock catalog
  shadows, unknown metadata, reason/hash-pinned allowances, scratch boundary,
  stale intermediate saves, missing comparisons, input/output/outcome/archive
  baseline drift, malformed baseline JSON, model-only reach and absent facts.
- `py -3 scripts/check-editor-game-render-context-parity.py --quiet` and
  `py -3 scripts/check-editor-pass-contract-parity.py --quiet`: PASS, exit 0.
- `py -3 scripts/check-background-launch.py`: FAIL only because
  `docs/background-launch-inventory.md` is stale. Its regeneration is outside
  this task's allowed paths; no new launch-policy violation was reported.
- `git diff --check`: clean. Edited tracked files and the new source/test/JSON
  fixture retain LF. No engine source was changed.
- `bash scripts/check-contracts.sh`: NOT RUN; user forbids Git Bash in this
  sandbox. Full board proof and the stale generated inventory refresh remain
  with the orchestrator. The existing scripts_tests row collects these tests;
  no board registry was changed. Git add/commit/rm/stash were NOT run because
  git index writes are forbidden; the baseline fixture awaits staging.

Early diagnostics remain red in scratch reports: `12-15-24` lacked the
pre-archive effects bootstrap, `12-16-57`, `12-17-36` and `12-22-36` lacked
the required loose Object2 PacketFile; `12-24-41` reached workflows and a
real mc2_22 comparison but correctly failed the missing baseline; `12-27-07`
rejected unrecognized generated caches. None was counted as final proof.
