# Editor round-trip ledger (E0)

Summary: 0 byte-identical, 0 lossless-only, 23 semantic; 1 unverified of 24.

Stock inputs come from mission.fst; differing loose overrides are rejected. Each row loads the stock PAK through -mission and writes a separate PAK/FIT through -smoke-save (quick save). This proves the FIT writer and editor reload payload, not a full MOVE/tac-map export.

Diff size = unequal bytes at matching offsets + the absolute length difference. Byte equality is unmasked; the field census explains changes without blessing them.

Evidence: `scratch/editor-smoke/2026-10-13T11-19-02` (input FIT, written FIT, unified text diff, SHA-256 JSON and report).

Campaign representatives: group 0: mc2_01; group 1: mc2_02; group 2: mc2_03; group 3: mc2_04; group 4: mc2_05; group 5: mc2_06; group 6: mc2_10; group 7: mc2_11; group 8: mc2_14; group 9: mc2_18; group 10: mc2_19; group 11: mc2_23; group 12: mc2_24.

| Mission | Byte-identical yes/no | Diff size | Explanation |
|---|---|---:|---|
| mc2_01 | no | 37830 | FINDING: semantic; classes: Cameras, NavMarker coordinates, Part CSV spelling, Part control/profile, Objective condition coordinates. 41 altered, 0 added, 0 removed fields; see evidence. |
| mc2_02 | no | 64071 | FINDING: semantic; classes: Cameras, Part CSV spelling, Part control/profile, Objective condition coordinates. 53 altered, 0 added, 0 removed fields; see evidence. |
| mc2_03 | no | 38275 | FINDING: semantic; classes: Cameras, Part control/profile, Part CSV spelling. 18 altered, 0 added, 0 removed fields; see evidence. |
| mc2_04 | no | 109252 | FINDING: semantic; classes: Cameras, NavMarker coordinates, Part CSV spelling, Part control/profile, Objective ModelType. 66 altered, 0 added, 0 removed fields; see evidence. |
| mc2_05 | no | 37828 | FINDING: semantic; classes: Cameras, Part CSV spelling, Part control/profile, Objective condition coordinates. 48 altered, 0 added, 0 removed fields; see evidence. |
| mc2_06 | no | 53395 | FINDING: semantic; classes: Cameras, Part CSV spelling, Part control/profile, Objective condition coordinates. 50 altered, 0 added, 0 removed fields; see evidence. |
| mc2_07 | no | 57445 | FINDING: semantic; classes: Cameras, Part control/profile, Part CSV spelling. 76 altered, 0 added, 0 removed fields; see evidence. |
| mc2_08 | no | 26071 | FINDING: semantic; classes: Cameras, Part control/profile, Objective condition coordinates. 14 altered, 0 added, 0 removed fields; see evidence. |
| mc2_09 | no | 56556 | FINDING: semantic; classes: Cameras, Part CSV value, Part control/profile, Part CSV spelling, Objective condition coordinates. 42 altered, 0 added, 0 removed fields; see evidence. |
| mc2_10 | no | 62435 | FINDING: semantic; classes: Cameras, Part CSV spelling, Part control/profile. 38 altered, 0 added, 0 removed fields; see evidence. |
| mc2_11 | no | 23032 | FINDING: semantic; classes: Cameras, Part CSV spelling, Part control/profile, Objective condition coordinates. 10 altered, 0 added, 0 removed fields; see evidence. |
| mc2_12 | no | 71784 | FINDING: semantic; classes: Cameras, Part control/profile. 62 altered, 0 added, 0 removed fields; see evidence. |
| mc2_13 | no | 48661 | FINDING: semantic; classes: Cameras, Part control/profile, Objective condition coordinates. 60 altered, 0 added, 0 removed fields; see evidence. |
| mc2_14 | no | 37196 | FINDING: semantic; classes: Cameras, Part CSV spelling, Part control/profile, Objective condition coordinates. 11 altered, 0 added, 0 removed fields; see evidence. |
| mc2_15 | no | 76226 | FINDING: semantic; classes: Cameras, Part CSV spelling, Part control/profile, Objective condition coordinates. 42 altered, 0 added, 0 removed fields; see evidence. |
| mc2_16 | no | 59309 | FINDING: semantic; classes: Cameras, Part CSV spelling, Part control/profile, Objective condition coordinates. 35 altered, 0 added, 0 removed fields; see evidence. |
| mc2_17 | no | 45693 | FINDING: semantic; classes: Cameras, NavMarker coordinates, Part CSV spelling, Part control/profile. 23 altered, 0 added, 0 removed fields; see evidence. |
| mc2_18 | no | 75147 | FINDING: semantic; classes: Cameras, Part CSV spelling, Part control/profile, Objective condition coordinates. 37 altered, 0 added, 0 removed fields; see evidence. |
| mc2_19 | no | 80934 | FINDING: semantic; classes: Cameras, Part CSV spelling, Part control/profile, Part CSV value, Objective condition coordinates. 41 altered, 0 added, 0 removed fields; see evidence. |
| mc2_20 | no | 64730 | FINDING: semantic; classes: Cameras, NavMarker coordinates, Part CSV spelling, Part control/profile, Part CSV value. 50 altered, 0 added, 0 removed fields; see evidence. |
| mc2_21 | no | 41955 | FINDING: semantic; classes: Cameras, Part control/profile, Objective condition coordinates. 9 altered, 0 added, 0 removed fields; see evidence. |
| mc2_22 | unverified | — | FINDING: editor save blocks at the 78-turret warning (limit 64); timeout confirmed in scratch/editor-smoke/2026-10-13T11-45-05. No FIT written; classes unverified. |
| mc2_23 | no | 95786 | FINDING: semantic; classes: Cameras, Part CSV spelling, Part control/profile, Part CSV value, Objective condition coordinates, Objective ModelType. 69 altered, 0 added, 0 removed fields; see evidence. |
| mc2_24 | no | 68544 | FINDING: semantic; classes: Cameras, Part control/profile, Part CSV value, Objective condition coordinates. 39 altered, 0 added, 0 removed fields; see evidence. |

## Writer findings

The summary counts **missions**, not fields. All 23 completed comparisons contain
semantic value changes. None is lossless-only. The seven classes below cover all
934 altered fields (no added or removed keys). The observed diffs contain no
number-spelling-only, key-order, or explicit-default-only changes. Decimal values
that differ, however slightly, are semantic; a small delta is not proof of
gameplay equivalence. Writers were not patched.

| Class | Fields | Representative input → written | Classification and cause (writer file:line) |
|---|---:|---|---|
| Cameras | 46 | mc2_01 `[Cameras]`: `f NewScale = 0.444542` → `f NewScale = 0.821212`; `f ZoomMax = 1.000000` → `f ZoomMax = 2.000000` | **Semantic:** saved camera scale and maximum zoom change. `editor/EditorData_save.cpp:770` calls `Camera::save`; `mclib/camera.cpp:797` and `:801` write live `newScaleFactor` and `zoomMax`. The load/init path resets maximum zoom at `mclib/camera.cpp:350` and recomputes scale from altitude/pitch at `:478`, replacing the FIT values. Both fields change in every completed mission. |
| Part control/profile | 698 | mc2_01 `[Part10]`: `ul ControlDataType = 2` → `ul ControlDataType = 1`; `st ObjectProfile = "pv20500"` → `st ObjectProfile = "PM101100"` | **Semantic:** vehicle control/profile becomes mech control/profile (349 pairs). `editor/EditorObjects_unit.cpp:79` chooses arguments by `dynamic_cast<GVAppearance*>`; `:82` selects the mech profile; `:172` and `:173` emit them. The existing Part control/profile fields are not retained by `Unit::load` (`:202`). This runtime's catalog remaps several stock IDs to MECH, so this is catalog-dependent writer behavior, not a pure-stock-dependency result. |
| Part CSV spelling | 119 | mc2_01 `[Part10]`: `st CSVFile = "LRMC"` → `st CSVFile = "Lrmc"` | **Lossless spelling on this Windows/case-insensitive lookup path:** same identifier ignoring case. `editor/EditorObjects_unit.cpp:175` obtains the current catalog filename; `:176` writes it. `editor/EditorObjectMgr.h:389` returns the resolved building's `fileName`. All case-only replacements observed are LRMC/Lrmc. This class does not excuse control/profile changes in the same Part. |
| Part CSV value | 20 | mc2_09 `[Part10]`: `st CSVFile = "Swarmlrmc"` → `st CSVFile = "ClanLRMC"` | **Semantic:** a different authored CSV identifier, not just casing. Same catalog-based writer at `editor/EditorObjects_unit.cpp:175` and `:176`. `editor/EditorObjectMgr.cpp:845` resolves the first matching FIT ID; this runtime contains both the old vehicle and a mech entry for ID 548. Observed in mc2_09, mc2_19, mc2_20, mc2_23, mc2_24. Shared ID is not proof that the CSVs are semantically identical. |
| NavMarker coordinates | 4 | mc2_01 `[NavMarker0]`: `f yPos = -789.333313` → `f yPos = -789.333496` | **Semantic numeric drift:** a different coordinate value. `editor/EditorObjects_pilot.cpp:515` writes the live appearance position. `editor/EditorObjectMgr_io.cpp:692` relies on the binary building packet instead of reading NavMarker FIT fields; loading at `:156` calls `addBuilding`, whose cell-center position is chosen at `editor/EditorObjectMgr.cpp:626` and `:629`. The FIT decimal is replaced by the live PAK/cell-coordinate representation. Observed in mc2_01, mc2_04, mc2_17, mc2_20. |
| Objective condition coordinates | 45 | mc2_01 `[Team0Objective1Condition0]`: `f PositionY = -277.333344` → `f PositionY = -277.333496` | **Semantic numeric drift:** the condition position is replaced by the resolved entity's position. `CSpecificStructureObjectiveCondition::Read` matches a live building near the input coordinates (`editor/Objective_conditions.cpp:223`); Save writes that building's coordinates at `:242` and `:243`. The analogous unit writer is `:110` and `:111`. These writers do not preserve the original coordinate value. Condition species and cell references are unchanged in the observed diffs; small drift is still not numeric identity. |
| Objective ModelType | 2 | mc2_04 `[Team0Objective1]`: `l ModelType = 5` → `l ModelType = 13` (also mc2_23 `[Team0Objective3]`) | **Semantic:** serialized type changes from vehicle to mech. `editor/Objective.cpp:543` resolves ModelID through the current catalog, `:548` derives type from the resolved ID's high byte, and `:559` writes it. `ModelID` is retained by Read at `:395`, while input ModelType is not; the active catalog's Storm ID 200 has a MECH entry. It is not integer formatting. |

### Runtime dependency qualification

Mission PAK/FIT inputs are stock archive bytes, but the runtime is
`scratch/editor-e0-install`, including its loose `data/art/Buildings.csv`.
That file is **not** the stock archive catalog. Its SHA-256 is
`86153dd5c4793672d35033ce4e8e90a871df8b1e131ccde7e8d61ab1cf083ae9`;
the `art.fst` catalog hash is
`3a1b449d5e4336c7e2345ebd577181fcc7f9f41107cbc1ffa6da7e0ff9b6ebce`.
The loose file contains Infantry as MECH (line 171), Lrmc ID 201 as MECH
(line 475), ClanLRMC ID 548 as MECH (line 466), and Storm ID 200 as MECH
(line 479), alongside older vehicle entries. `getBuildingFromID` chooses the
first match in its grouped catalog (`editor/EditorObjectMgr.cpp:837`). The
control/profile, CSV-value and ModelType findings are therefore qualified by
this catalog. Neither catalog nor writer was modified to hide differences.

### mc2_22 save-block finding

Both `2026-10-13T11-19-02` and isolated confirmation
`scratch/editor-smoke/2026-10-13T11-45-05/report.md` timed out. Both logs show
successful autoload followed by `EditorObjectMgr::save packet 1`; rendering
continues through frame 120, but GUID/FIT-save milestones and summary never
occur. This is a blocked save, not a slow FIT comparison or dead rendering loop.

Read-only decoding of stock mc2_22 PAK packet 1 gives 2,503 buildings and 78
turrets. `editor/EditorObjectMgr_io.cpp:111` tests the 64-turret limit and
`:115` calls `AfxMessageBox`, before returning to the rest of mission save.
This path does not honor smoke modal suppression. The modal warning is the
source-and-packet-supported cause inferred from the last log milestone;
no dialog was clicked or dismissed to manufacture a round trip. This is
existing editor behavior, not a new probe defect. The writer is unchanged;
mc2_22 has **no byte comparison**. Its campaign group is nevertheless covered
by completed representative mc2_19.

### Workflow probe corrections and evidence

The first run's raw panel facts contain **two** failed probes: PaintMaterial
and ObjectivesSummary. Its parser reported only PaintMaterial. The parser now
lists every failed/missing panel.

PaintMaterial selected material 0 on a positive-material stock vertex.
`mclib/mapdata.cpp:1447` only changes material for positive Type values; zero
refreshes it. The probe now chooses positive material 1 or 2 and requires both
paint and undo. The first confirmation (`2026-10-13T11-46-29`) shows
`PaintMaterial reached=1 ok=1` and exposes the objectives failure.

The standalone objectives probe called the collection writer without the
`[Objectives Version]` envelope required by its reader
(`editor/Objective.cpp:801`). The probe now supplies Version 3, matching normal
mission export (`editor/EditorData_save.cpp:976`). The writer and reader remain
unchanged. Final confirmation
`scratch/editor-smoke/2026-10-13T11-51-53/report.md` is **PASS (1/1)**: all seven
panel facts are `reached=1 ok=1`, fresh mission PAK/FIT output exists, and the
authored objective/condition/flag reads back equal before live objectives are
restored. Raw evidence is `workflows.stderr.txt` alongside that report. Elapsed
623.4 seconds includes the host-lease wait; it is not editor execution time.

### Proof and scope

- `py -3 tools/build_quiet.py --target EditRel`: rebuilt twice in this worktree,
  RelWithDebInfo, exit 0 (three warnings); last rebuild 13.4 seconds. Copied the
  resulting editor to the private runtime before final workflows confirmation.
  Build and deployed executable SHA-256 both equal
  `b83584d075d85b2461d4655e7598bbc7d0ca500cca549ccf7622a29760e0f1a0`.
- `py -3 scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --case workflows --keep-logs`:
  final PASS 1/1, seven successful facts. The intermediate run failed objectives
  and correctly remained red until its missing version envelope was repaired.
- `py -3 scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --case stock_roundtrip:mc2_22 --timeout 180 --keep-logs`:
  timeout confirmed; no FIT comparison. Recorded 288.4 seconds includes lease
  waiting before the 180-second execution timeout.
- `py -3 tools/build_contract_harnesses.py --run --unit`: final PASS 20/20
  harnesses and 2,794 doctest cases.
- `py -3 -B -m pytest scripts/tests/test_editor_smoke.py -q -p no:tmpdir -p no:cacheprovider`:
  PASS 20 tests and 28 subtests. Coverage includes multiple failed panels,
  semantic vs lossless classification, unknown/default fields, retained writer
  notes, missing outputs and a single-stock-case rerun.
- `py -3 scripts/check-editor-game-render-context-parity.py --quiet` and
  `py -3 scripts/check-editor-pass-contract-parity.py --quiet`: exit 0; these
  are the direct Python commands for the existing editor board rows.
- `git diff --check`: clean. Existing 15/15 base rows and 23/24 stock comparisons
  are from the orchestrator's `2026-10-13T11-19-02` evidence, not a claimed fresh
  full-suite pass. Combining that evidence with confirmations yields 39/40
  completed rows; mc2_22 remains red. All 13 campaign groups have a completed
  representative. Full MOVE/tac-map export remains outside the quick-save row.
- `bash scripts/check-contracts.sh` could not run: Git Bash is blocked in this
  sandbox. The existing `scripts_tests` board row collects the new parser tests;
  no board registry file was edited. Full board proof remains the orchestrator's.
- Git index writes/commits are forbidden here. Files are uncommitted for the
  orchestrator, including this ledger. No engine code, dependency catalog or
  mission writer was changed.

### Artifact decisions inherited at resume

`tests/smoke/editor/` was already absent when this follow-up started; no fixture
from it was adopted and no file was deleted in this follow-up. The unavailable
session's individual deletion list cannot be reconstructed from remaining files.
Historical references still exist in `lane2-state.md` to reports
`2026-10-12T10-28-51`, `2026-10-12T17-58-12`, `2026-10-13T00-46-32` under that
directory. The earlier task's **nothing references deleted artifacts** claim
is therefore not established. That document is outside this task's allowed edits;
the orchestrator must reconcile references or recover the artifacts/decision
list. New run evidence stays under ignored `scratch/`.
