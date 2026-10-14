# Owned mission document: Q3 and E3-a

`editor_document` is independent of MFC, GameOS, catalogs, and private heaps. The
SDL application owns one `MissionDocument` followed by its `CommandHistory`, so
history cannot outlive its document. Existing EditRel ownership remains isolated.
This is the document foundation and the B2 shell slice; dialogs, viewport tools,
terrain editing, and embedded play are subsequent work.

FIT sections and fields have stable document-local IDs. Owned typed values retain
absence, array cardinality, and imported Part identity. Terrain tiles, packet
placements, Part control/profile/CSV values, brain-section references, and objective
discriminators have typed document values. Team/player, camera, environment, and
brain fields remain in their owned typed sections. Enhanced brace blocks form a
bounded tree; unknown optional declarations remain opaque. No catalog lookup can
replace imported values. Entity IDs are distinct from exported Part indices and
packet record indices; links contain IDs rather than pointers to appearances.

The original FIT byte string owns every syntax span, including number spelling,
comments, whitespace, order, unknown fields and CRLF. Imported text semantics strip
CR as the existing reader does; unchanged syntax retains it. A changed field emits
only its value span. Returning to its loaded value restores the original span.
Every PAK packet owns its decoded bytes and its original encoded span; no-op and
metadata saves retain the whole original archive, including compression bytes and
offset table. Raw, file-within-file, zlib and classic LZW storage are supported;
unsupported compression is rejected. Building insert/delete/transform commands edit
placement packet 1. Changed placements use deterministic raw storage and an updated
offset table/checksum; all other encoded packet payloads remain byte-identical.
Undo restores the imported archive, including the original placement compression.

Mission name and author text commands own before/after values. Redo branching and
payload-capacity accounting enforce a bounded history, with an eviction count in
the shell. A document savepoint compares values, so undo across a save remains dirty
and undo back to the saved values becomes clean. Transient window/input state never
enters the history. The shell exposes the stored name and displays its localization
resource separately; it preserves the localization selection.
The name draft uses ImGui's growing `std::string` input adapter, preserving valid
names longer than 2,000 bytes through Apply and Save. The document's named 1 MiB
text limit still applies; an over-limit command is rejected without changing it.

E3-a adds `insertUnit(UnitPlacement)`, `insertBuilding(BuildingPlacement)`,
`erase(EntityId)` and `transform(EntityId, Transform)` to the production history.
Each structural command validates a complete candidate before changing the document.
History owns the before/after FIT syntax, entity IDs and placement bytes; unchanged
terrain and packet payloads remain outside history. The memory budget includes
syntax, typed values and string/vector capacity. Text and structural commands can
interleave, branch, evict, undo and redo across saves.

Entity and syntax IDs survive transforms, insertion, deletion and undo/redo. IDs
are document-local and never recycled within an open document, including after
undo followed by a new branch. Reopening imports a new set of document-local IDs.
New Part/Warrior export indices append deterministically. Runtime requires dense
Part indices, so deletion compacts later Part headers, alternative indices and group
`CommanderNGroup:G/Mates` references while
retaining their document IDs and explicit Pilot references. Referenced alternatives
or parents with alternatives require a future graph-delete command and reject
atomically. Warrior blocks and legacy cells remain authored when their Part is
deleted, preserving shared assignments and existing Warrior export indices.
Creation rejects a new Warrior beyond the runtime's 119 usable slots.
Deleted group members become zero slots; surviving members keep their slots and
unit identity. Empty groups remain valid because the runtime skips nonpositive
members (`code/mission_init_load.cpp:1154`, `code/mission_init_load.cpp:1157`).
Malformed group arrays or out-of-range positive references reject deletion atomically.

Inserted Parts include the runtime-required payload: SquadNum and the 15-element
IndicesOfAlternatives array (`code/mission_init_load.cpp:729`, `code/mission_init_load.cpp:737`);
ObjectNumber, ControlType, ControlDataType, ObjectProfile, Pilot, PositionX,
PositionY, Rotation and TeamID (`code/mission_init_load.cpp:762` through
`code/mission_init_load.cpp:817`); CommanderID (`code/mission_init_load.cpp:826`);
Active, Exists and MyIcon (`code/mission_init_load.cpp:867`,
`code/mission_init_load.cpp:871`, `code/mission_init_load.cpp:883`). MyIcon defaults
to zero. Optional runtime fields with fallback values remain optional.

A unit's explicit Part `Pilot` resolves its `WarriorN`; position and Part number
never select a brain. `Entity::warrior` names the root assignment, and `brain`
names only its direct declarative Brain child. `find(NodeId, key)` and
`childSection(NodeId, name)` stay in that scope. The legacy scalar Brain/module,
NumCells and WarriorNCellI declarations remain separate. Untouched Hybrid bodies,
comments, unknown keys, OPORDs and repeated nested extension blocks retain their
bytes. No compatibility-mode write API or panel is introduced in E3-a.

Load validates literals, scalar types, finite floats, array counts, nesting, packet
bounds, compression lengths, aggregate decoded bytes, terrain dimensions, Part
payloads and objective counts/discriminators/payloads before publishing a document.
One failed load produces one reason diagnostic. Required metadata follows the game
loader and the current editor preflight: scenarioTuneNum, Blurb2, ScenarioScript,
MissionName or its selected resource ID, objective version and team/objective counts.
The editor also requires authored coordinates for specific-object conditions.
Present Part TeamID must be an integer in [-1,7]: the no-team sentinel is used by
`code/mover.cpp:280`, and `code/dteam.h:16` fixes the eight-team table. Present
PositionZ/Rotation require scalar finite floats; absent values retain their zero
defaults. Legacy FlagIndex requires an integer in [0,INT32_MAX], preserving the
signed decimal-ID conversion in `code/objective_text.cpp:84` without wrapping.
Present Pilot must be a scalar integer in [0,65535]; nonzero references must name
an existing Warrior. A zero or absent Pilot has no resolved assignment. Multiple
direct Brain blocks under one Warrior reject as ambiguous; repeated route or
extension children remain valid. Placement TeamID uses the same [-1,7] bounds.

Save stages and rereads the FIT/PAK pair before publication. A per-transaction journal
names both destinations and retained previous files. Failed publication rolls back;
pending or invalid journals block loading a potentially mixed pair. Interrupted
transactions require recovery using that journal and its retained previous files.
Successful saves retain previous versions and committed journals. A failed save
does not advance the document savepoint. No ABL or asset contents are rewritten.

Build `EditRel`, `EditorShell`, and `editor_document_tests` in RelWithDebInfo with a
unique `_MSPDBSRV_ENDPOINT_`. `editor_document_tests_asan` is an independent ASan
target, excluded from the default build. It copies its compiler's ASan runtime.
Run the native document proof without launching a game/editor:

```
py -3 editor/tests/run_document_proofs.py
```

It copies and hashes exactly 24 fresh FIT/PAK pairs from lane3's stock archive,
builds both doctest targets and EditorShell with a unique PDB endpoint and bounded
build timeouts, and runs both native variants. `MC2_EDITOR_STOCK_DIR` selects those
copies; direct tests without it explicitly skip the corpus case. The corpus saves
all 24 pairs, compares every byte and checks text edit/undo. Both native variants
retain corrupted FIT output copies; the proof runner feeds unchanged and corrupted
outputs through the production smoke `compare_fit` byte grader and requires all
24 corruptions to be rejected. Structural fixtures cover changed output, save/reopen, original-byte
undo saves, redo, coincident units/buildings, IDs after compaction, absent fields,
raw/zlib/checksummed packets, nested scope and atomic rejection/eviction.

After the orchestrator commits, deploy both editor executables to lane3 under the
host lease. Repeat the unchanged Q3 runtime gate with explicit cases:

```
py -3 scripts/run_editor_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe" --prepare-stock-scratch --case stock_roundtrip --case workflows --case new_model --keep-logs --exit-sec 3 --timeout 180
```

Existing cases and the frozen baseline remain unchanged. The additional gate runs
exactly 24 fresh stock FIT/PAK pairs through the SDL shell and production codec.
It checks all output bytes, edit/save/reopen, undo/save, redo, malformed rejection,
an 8 KiB name in the actual ImGui input/Apply/save path, and a real corrupted-output
negative control. Missing, skipped, blocked, finding,
flaky or duplicate rows fail. Gate evidence includes executable SHA-256 and its
embedded HEAD fingerprint. Final acceptance repeats these proofs after the
orchestrator commits and deploys the clean build.

The runtime/cook shared-codec migration needs lane 1; this claim changes no shared
loader. E3-a adds document-command reach only: no inventory workflow gains panel
or rendered-widget reach. Unit/building/player panels and the three-mode brain
picker are E3-b. Brain-source authoring, terrain recooking and project dependency
resolution belong to later workflow slices.
