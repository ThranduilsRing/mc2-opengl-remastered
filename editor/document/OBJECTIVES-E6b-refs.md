# E6b-refs: typed entity references (deferred)

This is the separately named follow-up to complete E6b objective CRUD. It is a
design contract, not delivered reference functionality. E6b copies objective
payloads verbatim, including all target lexemes; no CRUD operation retargets a
reference or rewrites an internal payload key. Existing entity moves/deletes
remain reference-unaware until this whole follow-up is implemented. An opaque
round trip or a scalar census does not establish target resolution.

## Authority and identity

The runtime tries MoverWID or BuildingWID first, then XY if that WID declaration
cannot be read. WIDs are runtime watch IDs, never document EntityIds or exported
Part indices. See [unit WID/XY reading](../../code/objective.cpp:312).
The fallback compares each float coordinate with a strict relative tolerance:
`abs(a-b)/max(abs(a),abs(b)) < pow(10,-FLT_DIG)*10`; both zeros match. Preserve
float conversion and the strict boundary in tests, including zero, negative
coordinates and adjacent representable values. See
[the actual comparison](../../code/objective.cpp:247).

The species determines the kind and eligible alignment, rather than imposing
one blanket enemy filter. The base specific-enemy-unit reader currently has
its enemy filter disabled. CaptureOrDestroySpecificEnemyUnit and
CaptureSpecificUnit require a mover whose team differs from the objective's
alignment; GuardSpecificUnit requires the same alignment. See
[enemy filter](../../code/objective.cpp:451) and
[guard filter](../../code/objective.cpp:695). The current XY lookup table is:

| Species | Placed kind | Alignment filter |
|---|---|---|
| DestroySpecificEnemyUnit, DeadOrFledSpecificEnemyUnit | unit | none (disabled in base reader) |
| CaptureOrDestroySpecificEnemyUnit, CaptureSpecificUnit | unit | different team |
| GuardSpecificUnit | unit | same team |
| DestroySpecificStructure, CaptureOrDestroySpecificStructure, CaptureSpecificStructure, GuardSpecificStructure | building | none |

Building predicates inherit or override the
[specific structure reader](../../code/objective.cpp:358); the capture override
also uses an [unfiltered building XY lookup](../../code/objective.cpp:627).
Recheck this table against the readers when implementing, with deliberate
wrong-kind, wrong-alignment and wrong-target tests. Never select the first of
two eligible XY matches.

The legacy authoring lookup is in
[Objective_conditions.cpp](../Objective_conditions.cpp:103), not Objective.cpp.
Its [save path](../Objective_conditions.cpp:136) derives Commander, Group, Mate
and CellX/Y from the entity. These auxiliary lexemes are preserved separately;
they are neither identity nor an XY ambiguity tiebreaker. The game reader wins
when its rules differ from that editor's first-match authoring lookup. Stock
actions have no entity references; future action species need separate evidence.

Saved-runtime WID support is explicitly limited: initial delivery preserves an
authored WID as an unresolved runtime-only reference, without falling back to XY
when the WID is readable. There is no trustworthy WID-to-placed-entity map in
the authoring FIT/PAK. Mapping saved-runtime IDs requires a separately specified
runtime snapshot importer; it is not inferred from Part, packet or EntityId.
Such a WID remains export-identical and explicitly unresolved in the UI.
Missing XY candidates and ambiguous XY candidates also retain distinct explicit
statuses and original syntax, with one diagnostic per affected entry on import.

## Ownership and atomic commands

Extend the document-owned ObjectiveEntry with reference kind, resolution status,
optional EntityId and source NodeIds/lexemes. There is no static registry or
parallel objective model. Keep original coordinate and auxiliary field syntax
even after successful resolution. Unsupported species and unknown internal
reference keys travel unchanged; never guess or renumber their values.

Use the existing StructuralState and CommandHistory. Account for all new dynamic
reference storage in history cost and reject a command exceeding the budget;
never truncate. Candidate decoding currently precedes final EntityId assignment
in [MissionStructure.cpp](MissionStructure.cpp:56) and
[its identity reassignment](MissionStructure.cpp:81). Either resolve after
reassignment or remap every reference target alongside the entities. Tests must
use deleted/noncontiguous IDs so an accidental equal numeric ID cannot pass.
Syntax NodeId remapping must cover reference source fields too.

The existing [E6a reference contract](OBJECTIVES-E6a.md:160) fixes these policies:

- A resolved entity move follows atomically, updating every referring XY export
  within the same entity transaction, including the existing marker transform
  path. Preserve auxiliary lexemes unless a separately specified authoring
  operation changes them. Undo restores both entity and reference spelling.
- A resolved referenced-entity delete rejects once with bytes, revision, dirty
  state, ID allocation and executable history unchanged. Include existing marker
  delete and chooser paths. Removing a referring objective can unblock deletion;
  undo must restore both coherently. Do not inherit legacy
  [condition removal on invalidation](../Objective.cpp:872).
- Part/Warrior renumbering retains EntityId identity and original reference
  lexemes. Objective copy retains the source's targets; entity copy does not
  retarget objectives. Objective reorder preserves prerequisite flags.
- Unresolved and ambiguous imports remain explicit, without guessed targets.
  Entity edits must not silently resolve an ambiguity or retarget an unresolved
  entry. Any later explicit resolution is its own validated authoring command.

## Required proof

Import all 24 stock missions/173 objectives with independently specified target
EntityIds, kinds, statuses and species filters. Byte-identical unedited exports
are necessary but insufficient. Use a separate resolver oracle and make a
deliberately wrong target fail. The current
[typed census](../tests/objective_census.py:22) checks scalar fields only.
Assert tolerance boundaries, duplicates, empty candidate sets, runtime-only
WIDs, species alignment and kind mismatches. Unknown internal keys remain exact.

Exercise unit/building/marker moves, referenced deletes, objective and entity
copies, renumbering, and interleaved entity/objective undo/redo/save/reopen.
Every rejection asserts exactly one diagnostic and unchanged bytes, revision,
dirty state, executable history and future ID allocation. Limits reject rather
than truncate. Include storage in bounded-history tests. Retain strict 24/24
FIT/PAK pairs, nine frozen placements and the complete normal/ASan coverage.
Owned-code malformed-reader guarantees do not expand into engine hardening.

Payload/retarget editors and flags, templates/localization, and actual runtime
trigger/reward/video outcomes remain E6c, E6d and E6e respectively.
