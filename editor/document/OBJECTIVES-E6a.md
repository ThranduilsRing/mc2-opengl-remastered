# E6a objective scalars

This slice extends `MissionDocument::Objective`, its existing four lists and
`CommandHistory`. It edits scalar fields of existing objectives in EditorShell.
B5 remains incomplete. No objective CRUD, reference editing, condition/action
payload editing, template generation, string-table authoring or runtime outcome
proof is included.

Authority: `code/team.cpp` initializes team objectives; `code/objective.cpp`
`CObjectives::Read` and `CObjective::Read` define the runtime schema;
`code/objective_text.cpp` defines text defaults and activation-index aliases;
`code/objective_flags.cpp` defines team BooleanFlags. `editor/Objective.cpp`
`Construct`, `Read` and `Save` define authoring defaults and editor-only model
metadata. The inventory also includes CUserTextEdit and ViewConditionOrActionDlg
at `.claude/cleanup/editor-dialog-inventory.md:85`; current handlers are
`editor/Objective.cpp:616`, `editor/ObjectiveDlg.cpp:1120` and `:1265` at the base
commit `a6431a3ff`. There is no second objective model or MFC dependency here.

## Census before implementation

`py -3 editor/tests/objective_census.py` reads the actual lane3 stock FITs without
an editor launch. `editor/tests/objective-census.json` pins all 24 input SHA-256s,
every objective field and every species payload key. The proof runner checks
that census against fresh inputs. All 24 missions use objectives version 3;
173 objectives each contain the same 30 fields: the 26 authoring scalars below
and the four list counts. Stock has no populated team BooleanFlags and none of
the saved-runtime fields below.

| Stock condition species | Success entries | Failure entries |
|---|---:|---:|
| DestroyAllEnemyUnits | 10 | 0 |
| DestroyNumberOfEnemyUnits | 6 | 0 |
| DestroySpecificEnemyUnit | 301 | 6 |
| DestroySpecificStructure | 54 | 0 |
| CaptureOrDestroySpecificEnemyUnit | 12 | 0 |
| CaptureOrDestroySpecificStructure | 2 | 0 |
| CaptureSpecificUnit | 9 | 0 |
| CaptureSpecificStructure | 42 | 0 |
| MoveAnyUnitToArea | 18 | 0 |
| MoveAllSurvivingMechsToArea | 18 | 0 |
| BooleanFlagIsSet | 13 | 0 |
| ElapsedMissionTime | 2 | 0 |

| Stock action species | Success entries | Failure entries |
|---|---:|---:|
| SetBooleanFlag | 45 | 0 |
| PlayWAV | 4 | 0 |
| MakeNewTechnologyAvailable | 6 | 0 |

The legacy factory can author all twelve stock conditions and also
CaptureOrDestroyAllEnemyUnits, CaptureOrDestroyNumberOfEnemyUnits,
DeadOrFledAllEnemyUnits, DeadOrFledNumberOfEnemyUnits, DeadOrFledSpecificEnemyUnit,
GuardSpecificUnit, GuardSpecificStructure, MoveAllUnitsToArea and
MoveAllSurvivingUnitsToArea. Its three named EnemyUnitGroup species have no
factory implementation. The legacy action factory can author all three stock
actions plus PlayBIK, DisplayTextMessage and DisplayResourceTextMessage.
Stock coverage therefore establishes nothing about PlayBIK outcomes.

Stock unit payloads carry PositionX/Y, Commander, Group, Mate and CellX/Y;
structure payloads carry PositionX/Y, Commander and CellX/Y. Area predicates use
TargetCenterX/Y and TargetRadius; number predicates use Num; time uses Time;
flag conditions/actions use FlagID and Value. WAVFileName and PurchaseFileName
are the other stock action payloads. Every actual key, including species numeric
and string discriminators in each of the four lists, is in the census JSON.

## Scalar authoring schema

Values belong to the document. Each scalar has a canonical key, typed value and
optional stable source NodeId; absence carries a semantic default without adding
a declaration. Each objective and list entry retains its section NodeId through
scalar commands and undo/redo. Views own their drafts. There are no static owners.

| Keys | Type and default |
|---|---|
| Title | text, empty |
| Description | text, Title when absent |
| Priority | whole number, 1 |
| ResourcePoints | whole number, 0 |
| PreviousPrimaryObjectiveMustBeComplete | boolean, false |
| AllPreviousPrimaryObjectivesMustBeComplete | boolean, false |
| DisplayMarker | boolean, false |
| MarkerX, MarkerY | float, 0 |
| HiddenTrigger | boolean, false |
| ActivateOnFlag | boolean, false |
| ActivateFlagID | text, empty or decimal ActivateFlagIndex |
| ResetStatusOnFlag | boolean, false |
| ResetStatusFlagID | text, empty |
| TitleUseResourceString, DescriptionUseResourceString | boolean, false |
| TitleResourceStringID, DescriptionResourceStringID | whole number, 0 |
| ObjectiveModel | text, empty |
| ModelType | integer 0..255, 0 |
| BaseColor, HighlightColor, HighlightColor2 | signed 32-bit color bits, -1 |
| ModelScale | positive finite float, 1 |
| ModelID | editor-only integer -1..INT32_MAX, -1 |
| ModelID Version | editor-only nonnegative integer, 0 (legacy versions <=1 ignore ModelID) |

Whole numbers are 0..INT32_MAX because the game reads them into signed ints.
Declaration prefixes must match the authoritative reader: `ul` for Priority,
ResourcePoints, resource-string IDs and ModelID Version; `l` for ModelType,
colors and ModelID; `f` for coordinates and scale; `b` for switches; `st` for
text. ActivateFlagIndex is `ul` and the prerequisite alias is `b`, even when
shadowed. Objective version/list counts, numeric species and known integer
payloads also require `ul`. A different integer prefix rejects atomically with
one diagnostic.
Coordinates are finite and float-representable. Scale must also be at least the
smallest positive float. Text is at most 2,000 characters after FIT multiline
assembly, before the game's LF-to-CRLF conversion, and is never truncated.
The document and dynamic title/description buffers use real LF characters;
commands accept CRLF and normalize it to LF. Ctrl+Enter inserts a line break.
Literal backslash-n stays literal, matching `objective_text::toMemoryBreaks`.
Quotes, embedded NULs and lone CR characters are rejected for new scalar text.

FitIniFile joins the first two physical lines of a quoted value and appends LF
after later lines. Imported text reflects that reader behavior while retaining
all original syntax bytes. New multiline values have an empty opening line so
the first-line join consumes no authored text or break. Their physical breaks
are CRLF so the reader's RAM path retains consecutive empty lines too. The real
disk and RAM readers must recover the intended text, including leading, empty
and trailing lines and the 2,000-character boundary. Unedited multiline text
is byte-identical; a semantic no-op also preserves its spelling and redo.

`PreviousObjectiveMustBeComplete` supplies the previous-primary flag when the
canonical key is absent; edits retain that alias. `ActivateFlagIndex` supplies
ActivateFlagID when the canonical ID is absent; an edited ID adds the canonical
string while preserving the original index. Present aliases are validated even
when shadowed. ResetStatusFlagID has no index alias in the authoritative reader.
Resource modes and IDs are scalars referencing existing resources; editing them
does not rewrite a string table. Editing flag references generates no ABL/brain.
Model scalars are explicit values; E6a supplies no model-catalog picker or
ModelID-to-ObjectiveModel regeneration.

Saved runtime state is separate and opaque: IsActive, ActivationTime, IsResolved,
ResolvedStatus and ChangedStatus. TeamNBooleanFlags uses BooleanStringN and
BooleanValueN, stopping at the first missing ID in the game reader; it is also
preserved without an authoring command in E6a. None of these keys appears in the
authoring field selector. Objectives Version, NumObjectives, NumConditions,
NumActions, NumFailureConditions and NumFailureActions are structural metadata.
Version 2 assigns the unprefixed Objectives/ObjectiveN blocks to team 0; version
3 has TeamNObjectives/TeamNObjectiveM for eight teams. All four lists are owned.

## Preservation, unsupported entries and references

Unsupported string/numeric species and the three unimplemented group conditions
have an explicit monostate discriminator. The UI labels them unsupported and
preserved only; it supplies no executable preview. It never invokes the game's
dangerous unknown-string fallback. Species names must match the game exactly;
case variants remain unsupported. Missing blocks/discriminators, malformed
discriminators, bad list counts and malformed required known payloads reject
the entire candidate with one diagnostic. Unknown keys and unsupported payloads
retain valid FIT syntax byte for byte. A scalar command changes only its value
span or adds its absent declaration; all other content and the PAK stay intact.

Commands use validated candidate state and the existing bounded history. Rejected
edits leave bytes, dirty state, revision and executable history unchanged and log
once. No-op edits retain redo. Failed candidates do not consume new syntax IDs.
Undo restores exact original spelling, comments, unknown keys and unsupported
entries; redo restores the exact changed output. Structural objective commands
are absent, so E6a never copies, reorders or renumbers their opaque payloads.

E6b reference contract: retain coordinate lexemes separately from any resolved
document EntityId. Game predicates consume MoverWID/BuildingWID when present,
otherwise XY; neither WIDs nor XY are document IDs. Resolve only an unambiguous
match of the right entity kind. Missing/ambiguous matches remain explicit unresolved
references with original syntax, never the first coincident entity. Moving an
entity must update all *resolved* exports in the same command; deleting a referenced
entity must reject until the reference is explicitly changed/removed. Copying an
objective keeps its target IDs; copying an entity must not retarget objectives.
Part/Warrior renumbering must retain ID identity and original reference lexemes
unless the resolved target changed, with undo restoring both. Unsupported entry
blocks must travel unchanged except their section-name/count metadata during
future objective CRUD; unknown internal reference keys cannot be guessed or
renumbered. None of that reference/CRUD behavior is claimed by E6a: current
entity commands still preserve objective payload bytes without resolving targets.

## Proof and acceptance checkpoint

`py -3 editor/tests/run_document_proofs.py` builds document, ASan, shipping shell,
smoke shell, panel and `editor_objective_reader_tests` targets. The reader target
links the real FitIniFile and objective_text leaf code only into offline tests;
it checks the actual multiline widget saves and all scalar declaration types.
Normal/ASan tests assert independently specified
defaults; all 26 commands must change output, save/reopen with expected typed
values, undo to original bytes and redo. Rejection covers 2,000/2,001 text bounds,
invalid numerics/types, counts and missing payloads, retaining one diagnostic and
executable history. The corpus exports all 173 typed scalar models independently
of retained FIT bytes; Python compares them against a separate parser. Existing
strict 24-pair and nine frozen placement regressions remain mandatory.

`objectives_panel` uses real ImGui mouse/key events to choose the second objective,
edit each field and reach Apply/Undo/Redo/Save/Reopen. Its final save is changed;
reopen replaces deliberately dirtied state. The Python grader checks every
expected saved value and masks only the edited span to demand exact unrelated
bytes. Consistently wrong and no-op edited/redo artifacts must both fail.
The same PanelProof lives only in EditorShellSmoke and native panel tests.
`owned_objective_reload` requires the ResourcePoints widget artifact, validates
its input against the frozen stock mission and verifies every objective scalar
and list value after a legacy authoring reload/re-save. This proves no runtime
trigger, reward or video outcome.

After the orchestrator commits this diff, build that commit and deploy only to
lane3 with the approved payload harness; then run in PowerShell:

```powershell
$env:_MSPDBSRV_ENDPOINT_ = 'sol-e6a-accept-' + $PID
py -3 editor/tests/run_document_proofs.py
py -3 tools/build_contract_harnesses.py --run --unit
py -3 tools/build_quiet.py --target EditRel
py -3 scripts/deploy_payload.py A:/Games/mc2-opengl/mc2-win64-lane3 --target editor --allow-new-target --require-clean
py -3 scripts/deploy_payload.py A:/Games/mc2-opengl/mc2-win64-lane3 --target editor --exe-name 'Mission Editor Shell.exe' --allow-new-target --require-clean
py -3 scripts/deploy_payload.py A:/Games/mc2-opengl/mc2-win64-lane3 --target editor --exe-name 'Mission Editor Smoke.exe' --allow-new-target --require-clean
py -3 scripts/run_editor_smoke.py --exe 'A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe' --case objectives_panel --case stock_roundtrip --prepare-stock-scratch --keep-logs --exit-sec 3 --timeout 180 --out scratch/e6a-runtime
# Substitute the objectives_panel artifact directory reported above.
$e6aWidgets = 'scratch/e6a-runtime/<reported-run>/objectives_panel'
py -3 scripts/run_editor_smoke.py --exe 'A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe' --case owned_objective_reload --owned-objective-edit "$e6aWidgets/objective-ResourcePoints-edited" --prepare-stock-scratch --keep-logs --exit-sec 3 --timeout 180 --out scratch/e6a-legacy-reload
```

The orchestrator also runs `bash scripts/check-contracts.sh`; Git Bash is
forbidden in this task. Deployment, legacy runtime reload, rendered capture and
strict legacy 24/24 remain pending the committed/fingerprinted checkpoint. No
source file was staged, committed, deployed or deleted by this task.

The original combined ASan test process hit its 120-second bound once after an
earlier successful full run. This was a test timeout, not a compiler failure.
The runner now executes complementary core/stock and placement-live ASan suites,
each under the original 120-second process bound. It requires positive case
counts for both and their sum to equal the complete normal document suite;
all semantic and placement graders still run. No case or acceptance check is
omitted. Compiler probes retain their 300-second bounds.

## Follow-ups

- E6b: objective add/copy/delete/reorder and typed entity-reference import/export,
  including ambiguity, moves, deletes, renumbering and identity/undo proof.
- E6c: known success/failure condition/action payload editors and team BooleanFlags,
  then rare supported types and an explicit group-condition implementation policy.
- E6d: templates, localization/string-table authoring and model catalog selection,
  replacing CUserTextEdit, resource selection and remaining borrowed dialogs.
- E6e: separately claimed game-side trigger/reward/video instrumentation and
  fingerprinted actual outcomes, including synthetic PlayBIK coverage. B5 closes
  only when these explicit outcome requirements and the complete workflows pass.
