# E6b-refs: typed entity references

References resolve at document read; structural edits preserve their ordered candidates and entity identities or refuse once. Specific-target creation requires a unique eligible target in the same command. Entity copy has no command and is out of scope. Passthrough and scalar census results do not prove resolution.

## Authority and identity

The game reader (`code/objective.cpp`) tries MoverWID/BuildingWID first, then XY only when WID cannot be read. WIDs are runtime watch IDs, not EntityIds or Part indices. Without a trustworthy snapshot mapping, readable WIDs remain export-identical, unresolved runtime-only references with no XY fallback; a snapshot importer needs its own contract.

XY uses float conversion and strict relative tolerance `abs(a-b)/max(abs(a),abs(b)) < pow(10,-FLT_DIG)*10`; both zeros match. Resolve only one eligible entity of the correct kind. Missing/ambiguous matches stay distinct, retain syntax and log once per affected entry; never choose the first match or use auxiliary fields as tiebreakers.

| Species | Kind | XY alignment filter |
|---|---|---|
| DestroySpecificEnemyUnit, DeadOrFledSpecificEnemyUnit | unit | none; base enemy filter disabled |
| CaptureOrDestroySpecificEnemyUnit, CaptureSpecificUnit | unit | different team from objective alignment |
| GuardSpecificUnit | unit | same team |
| DestroySpecificStructure, CaptureOrDestroySpecificStructure, CaptureSpecificStructure, GuardSpecificStructure | building | none |

`editor/Objective_conditions.cpp` supplies legacy lookup and derives Commander/Group/Mate/CellX/Y on save. Preserve auxiliary lexemes separately: they are not identity. Runtime rules override legacy first-match lookup. Stock actions have no references; future action species need separate reader evidence. Unsupported species and unknown internal keys stay unchanged, never guessed or renumbered.

## Ownership and atomic commands

Extend document-owned ObjectiveEntry with kind, explicit status, optional EntityId and source NodeIds/lexemes; retain XY/auxiliary spelling after resolution. Use StructuralState/CommandHistory and budget all dynamic reference storage; limits reject without truncation. No static registry or parallel model.

Resolve only after final EntityId assignment at document read. Structural candidates carry status, target identity and source lexemes without re-resolution; remap source NodeIds too. Noncontiguous IDs must not resolve by accidental equality with export indices.

| Command | Required behavior |
|---|---|
| Resolved move, including marker transform | Refuse any change to reference candidates; reference XY changes only through explicit resolution. |
| Referenced delete, including marker/chooser paths | Reject once with bytes, revision, dirty state, history and future IDs unchanged; never delete conditions implicitly. Removing the referring objective may unblock deletion; undo restores coherent references. |
| Part/Warrior renumber | Retain EntityId identity and reference lexemes unless the resolved target changes. |
| Objective copy / entity insertion | Copy reader status by entry identity; refuse insertion that changes any existing reference candidates. |
| Objective reorder | Preserve prerequisite flags. |
| Unresolved/ambiguous entry | Refuse entity edits that gain, lose or reorder candidates; explicit resolution is a validated authoring command. |

Explicit resolution accepts only Missing/Ambiguous entries, writes changed XY lexemes only, and replaces the carried status/target after a unique game-rule match. RuntimeOnlyWid/Unsupported entries cannot be resolved; runtime watch IDs block entity insertion/deletion/renumbering because their mapping has no document identity. Rejection, including history-budget failure, logs once and consumes no state or IDs.

## Required proof

Use an independent target oracle for kinds, identities, statuses and species filters; deliberately wrong targets must fail. Cover strict tolerance boundaries, zero/negative/adjacent floats, duplicate/missing candidates, runtime-only WIDs, wrong kind/alignment, noncontiguous IDs and unknown keys. Scalar census grading is insufficient.

Require exact unedited exports, atomic unit/building/marker moves, rejected deletes, copies, renumbering, interleaved entity/objective undo/redo/save/reopen and history-budget rejection. Keep stock-pair, frozen-placement and complete normal/ASan gates. Reader guarantees cover owned code, not engine hardening.

Pending: payload/retarget editors and flags (E6c), templates/localization (E6d), and actual trigger/reward/video outcomes (E6e).
