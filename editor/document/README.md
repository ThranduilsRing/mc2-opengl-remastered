# Owned mission document: Q3, E3-a, E3-b and E4

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
builds both doctest targets, EditorShell and EditorShellSmoke with a unique PDB endpoint and bounded
build timeouts, and runs both native variants. `--stock-dir PATH` selects those
copies; direct tests without an accessible directory visibly skip the stock fixture
suite. `--byte-grade-dir PATH` selects the retained output directory, and the runner
passes `--require-stock-corpus` so missing inputs fail the strict proof. The corpus saves
all 24 pairs, compares every byte and checks text edit/undo. Both native variants
retain corrupted FIT output copies; the proof runner feeds unchanged and corrupted
outputs through the production smoke `compare_fit` byte grader and requires all
24 corruptions to be rejected. Structural fixtures cover changed output, save/reopen, original-byte
undo saves, redo, coincident units/buildings, IDs after compaction, absent fields,
raw/zlib/checksummed packets, nested scope and atomic rejection/eviction.

After the orchestrator commits, deploy EditRel, EditorShell and EditorShellSmoke to lane3 under the
host lease. Repeat the unchanged Q3 runtime gate with explicit cases:

```
py -3 scripts/run_editor_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe" --shell-exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor Smoke.exe" --prepare-stock-scratch --case stock_roundtrip --case workflows --case new_model --keep-logs --exit-sec 3 --timeout 180
```

Existing cases and the frozen baseline remain unchanged. The additional gate runs
exactly 24 fresh stock FIT/PAK pairs through the SDL shell and production codec.
It checks all output bytes, edit/save/reopen, undo/save, redo, malformed rejection,
an 8 KiB name in the actual ImGui input/Apply/save path, and a real corrupted-output
negative control. Missing, skipped, blocked, finding,
flaky or duplicate rows fail. Gate evidence includes executable SHA-256 and its
embedded HEAD fingerprint. Final acceptance repeats these proofs after the
orchestrator commits and deploys the clean build.

E3-b adds SDL unit, building and player panels. Unit and building selection uses
document IDs, including coincident placements. Placement defaults copy the selected
object; Place creates a new entity, while Apply transform/Apply team edit only the
selected ID. Player selection uses the stable PlayerN section ID; Apply alignment
writes its DefaultTeam. Selection/drafts stay outside history. Every mutation has
Undo/Redo reach, and the shell provides Save and Reopen saved mission.

Brain compatibility offers Legacy, Hybrid and Enhanced. A Part's explicit Pilot
selects its WarriorN, its direct Brain child receives compatibilityMode, and the
Warrior's scalar Brain receives the legacy ABL module reference. Absent blocks or
keys are inserted losslessly; edited switches occupy their own body line because
the game parser skips the Brain opening line. Other block bytes remain unchanged. Legacy cells,
comments, unknown switches and OPORD blocks survive. A one-line help message says
runtime Hybrid arbitration is not implemented. The Enhanced OPORD summary is read
only and displays the three authored slots, their types and waypoint counts.

The shipping EditorShell has no fixture replacement, synthetic input, widget tracking
or capture code. EditorShellSmoke is a separate test-only target; the runtime runner
uses it for `new_model` and panel cases with the same nofocus and host lease.
The offline runner checks proof markers are absent from the shipping binary and present
in the smoke host. Saved inline Legacy/Hybrid/Enhanced switches are tested through
the game's `parseMissionFitBrains`, and second round-trip FIT/PAK saves are read from disk.

`run_document_proofs.py` also builds/runs `editor_panel_tests`: ordinary ImGui mouse
and keyboard events click selection, transform/team apply, place/delete, all three
brain choices, the module apply button, Undo, Redo, Save and Reopen. Every operation requires a
changed FIT or PAK, a production save/read, original-byte undo saves and identical
redo saves. This desktop-free test does not claim a rendered capture.

Keep the Q3 command above unchanged. After the orchestrator commits, rebuilds and
deploys the three editor executables to lane3, add these rows (no baseline replacement):

```
py -3 scripts/run_editor_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe" --shell-exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor Smoke.exe" --prepare-stock-scratch --case unit_panel --case building_panel --case player_panel --case brain_modes --case panel_render --keep-logs --exit-sec 3 --timeout 180
```

Each row uses isolated FIT/PAK copies and a small authored fixture with stock terrain.
The brain fixture includes comments, unknown switches, OPORDs and legacy cells;
Legacy -> Hybrid is saved/reopened, and an existing Hybrid fixture is saved without
changing any bytes. The runner grades every edited/undone/redone pair and checks
executable/input fingerprints. `panel_render` is separate evidence: the SDL/GL
framebuffer is retained as `panels.bmp` (1440 x 960), for visual review. Widget and
operation facts alone cannot substitute for changed output or that capture.

Inventory panel reach now covers UnitSettingsDlg placement/team/transform and brain
assignment, BuildingSettingsDlg placement/team/transform, and PlayerSettingsDlg
identity/default-team selection. This is partial UnitSettings/BuildingSettings
workflow reach: type/pilot/squad/repair editing and building damage/forest editing
remain later work. ChooseUnitDlg/ChooseBuildingDlg objective-reference workflows
remain unreplaced; entity list selection alone does not implement those dialogs.

The runtime/cook shared-codec migration needs lane 1. Brain-source authoring,
terrain recooking, catalog resolution and project dependencies remain later slices.

E4 is a **B2 file-lifecycle slice**, not completion of B2. E3-a (`112f03674`)
supplies structural commands and stable identities; E3-b (`a91708219`) supplies
entity/player/brain panels and their widget proofs. The SDL application still
renders only ImGui panels through SDL/OpenGL 3.3. It has no map viewport, terrain
or entity scene renderer, Vulkan renderer, or embedded runtime. Pixel variety is
not evidence of displayed map correctness; Vulkan displayed results remain open.

`FileWorkflow` owns the active FIT path, open/save-as path drafts, the pending
transition and a bounded recent list. Open, Save, Save as, Reopen, Recent and Close
use the owned document. FIT/PAK publication retains the document's transaction
protocol. Save as changes the active destination only after success; subsequent
Save/Reopen and the entity panel buttons share that destination. Save preserves
undo/redo across the savepoint. A successful Open/Reopen adopts a validated
candidate, empties history and resets entity/player IDs, placement drafts and the
mission-name draft. Failed document I/O leaves live bytes, savepoint, history and path alone.
Open validates the candidate before a requested Save so a bad target cannot alter
the current savepoint. Save followed by Reopen of the active pair retains the
just-saved values. A failed Save keeps the pending transition and prompt open.

Dirty Open, Reopen, Recent and SDL window-close/quit requests offer Save, Discard
and Cancel in ImGui panels, without an OS dialog or nested message pump. Cancel
preserves edits and history. Discard adopts the target only after validation;
close exits only after an accepted decision. Accepted close immediately suppresses
editable document and entity UI, including queued input in that frame and a close
accepted by the file controls inside the document window. The shell already owns its SDL
window, GL context, ImGui context/backends and event loop; no HWND input forwarding
was introduced. Legacy EditRel and its input/pump remain isolated and unchanged.
Only applied document edits are dirty; panel drafts still require Apply.

Recent paths persist as UTF-8 in `editor-recent.txt` in the working directory;
`--recent-file` selects another store (the harness uses each case's scratch
directory). It retains at most 12 absolute paths, only after successful opens or
saves. Missing recent targets pass through the same dirty/failed-load handling.
Persisted entries must already be absolute UTF-8 generic paths with the exact
`.fit` extension; validation precedes lexical normalization. Relative paths,
other extensions, embedded controls and oversized lists reject the entire list
with one diagnostic. Growing strings preserve path
and text tails; native Unicode filesystem paths also remain UTF-8 in diagnostics
and transaction journals. Diagnostic path/control characters cannot create extra
log lines. OS path-length failures reject rather than shortening input. Recent
publication atomically replaces the store, then commits the candidate in memory.
A failed recent write leaves the previous memory and disk list intact and emits
exactly one `[EDITOR_FILES v1] recent_updated=0 reason=recent_write_failed` line
per failed update. It appends `; recent list update failed` to the operation status.
`save()` returns true exactly when document publication succeeds: it commits the
active path and document savepoint independently of recent-list publication,
preserves undo/redo, and never reports a recent failure as a document-save failure.
Without a configured store, the recent list commits in memory. Failed staging
files remain as proof artifacts; no `.previous` recent-list backups are created.

The 13 registered smoke-host file cases exercise real widgets, changed FIT output,
undo/redo, pair save/read checks, long stored names, Unicode/space-containing save-as
paths, recent reopening, and Save/Discard/Cancel for Open, Reopen and window close.
`file_failures` requires exactly five diagnostics: missing target, malformed FIT,
failed save-as, an untruncated invalid 8 KiB path, and a failed close-time save.
The last must keep the shell alive and the dirty prompt pending. Offline
`editor_panel_tests` also reloads the recent store and rejects malformed lists.
The smoke host posts a real SDL close event; the offline test requests the same
transition without claiming SDL or framebuffer coverage. Proof completion and
loop termination are separate assertions (`EDITOR_FILE_LOOP`), and close proofs
cannot terminate the loop. An offline negative control deliberately ignores
production close handling and must fail despite completed widget assertions.
Queued-click and locked-store negative controls cover close-frame edits and
recent-list publication failures without invalidating a successful document save.

Proof checkpoint: `py -3 editor/tests/run_document_proofs.py` builds normal and
ASan **document** targets plus shipping/smoke shells and the widget tests. The
strict 24/24 FIT/PAK byte gate and deliberate corrupted-FIT rejection are unchanged.
There is no shell ASan proof. The orchestrator commits this reviewed diff first,
then builds/deploys that commit to lane3 and runs the existing Q3/E3 commands above
plus this exact command under the harness lease (no baseline replacement):

```
py -3 scripts/run_editor_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe" --shell-exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor Smoke.exe" --prepare-stock-scratch --case file_save --case file_save_as --case file_recent --case file_open_save --case file_open_discard --case file_open_cancel --case file_reopen_save --case file_reopen_discard --case file_reopen_cancel --case file_close_save --case file_close_discard --case file_close_cancel --case file_failures --keep-logs --exit-sec 3 --timeout 180
```

Deferred: true New-map creation needs a validated new-mission/terrain/packet factory
in the document, a fresh history/savepoint and cleared IDs/drafts in all panels.
The current document only reads/decodes existing missions. The later catalog
chooser needs an owned, mod-aware catalog reader and complete UnitPlacement or
BuildingPlacement values passed to E3-a insert commands; EditorObjectMgr's
singleton is not an acceptable catalog owner. Start/About, project dependencies,
map viewport and lane-2 Vulkan integration, and legacy-pump/HWND-forwarding deletion
remain later B2 slices. This slice has no lane-1/G2b overlap.

Initial E4 checkpoint (2026-10-14, before review fixes):

- `py -3 editor/tests/run_document_proofs.py`: PASS at
  `scratch/e4-proofs/2026-10-14T08-41-34`; normal and document ASan each passed
  21 cases/114080 assertions, all 24 original pairs and all 24 corrupted-output
  negative controls. Both shell targets built; shipping proof-marker isolation passed.
- After strengthening failure-state assertions and adding malformed recent-list
  controls, `py -3 tools/build_quiet.py --target editor_panel_tests` and
  `py -3 tools/build_quiet.py --target EditorShellSmoke`: PASS with unique
  `_MSPDBSRV_ENDPOINT_` values (`sol-e4-final-<pid>`, `sol-e4-smoke-final-<pid>`).
- Using stock copies in `A:/Games/mc2-codex/scratch/e4-proofs/2026-10-14T08-41-34/stock`,
  `build64/out/editor/RelWithDebInfo/editor_panel_tests.exe --no-breaks`: PASS,
  3 cases/106 assertions, including the 13 workflow scripts. Production `grade_file`
  also accepted all 13 real widget-test output directories. No desktop was used.
- `py -3 -B -m pytest scripts/tests/test_editor_smoke.py --capture=sys -p no:tmpdir -p no:cacheprovider -q`:
  PASS, 44 tests/162 subtests, including corrupted/no-op/truncated/missing-proof controls.
- `py -3 scratch/e4_run_contracts.py` ran
  `py -3 tools/build_contract_harnesses.py --run --unit` with a unique endpoint and
  a 300-second outer bound: PASS, 3115 native cases (4 existing skips), four Python
  suites. The release-tree suite explicitly did no deployment checks because no
  release root was configured. No compiler timeout occurred.
- `git diff --check`: PASS. Bash `scripts/check-contracts.sh`, clean committed
  all-target build/deploy, and fingerprinted lane3 runtime rows were not run:
  those remain the orchestrator's required commit/build/deploy checkpoint. No index
  writes, commits, deployment, editor launch or game launch occurred here.

Retained scratch: `scratch/e4_run_contracts.py`, `scratch/e4-contracts.log`,
`scratch/e4-proofs/2026-10-14T08-34-39` (initial widget-driver failure) and
`scratch/e4-proofs/2026-10-14T08-41-34` (PASS), plus test-created children of
`scratch/e4-widget-tests`, `scratch/e4-recent-tests`, `scratch/e3b-widget-tests`,
`scratch/e3a-document-tests`, `scratch/q3-document-tests` and
`scratch/editor-smoke-tests`. Failed-publication/journal and recent-list backups
are retained with their case artifacts. The contract harness also retains its
normal generated test/build artifacts. No scratch cleanup was performed.

E4 review-fix checkpoint (2026-10-14, uncommitted diff):

- `py -3 editor/tests/run_document_proofs.py`: final PASS at
  `scratch/e4-proofs/2026-10-14T08-58-19`; all five targets built with zero warnings.
  Normal and document ASan each passed 21 cases/114080 assertions, strict 24/24
  unchanged stock pairs and 24/24 corrupted-output controls. Widget/file tests
  passed 6 cases/185 assertions, including queued-click and loop-termination
  negative controls and a Windows sharing lock that prevents recent publication.
  Shipping/smoke proof-marker isolation passed. The earlier review-fix run at
  `scratch/e4-proofs/2026-10-14T08-55-38` also passed; one ignored-result warning
  was then removed and the complete proof rerun. No timeout occurred.
- `py -3 -B -c "import tempfile, pytest; tempfile.tempdir='A:/Games/mc2-codex/scratch'; raise SystemExit(pytest.main(['scripts/tests/test_editor_smoke.py','--capture=sys','-p','no:tmpdir','-p','no:cacheprovider','-q']))"`:
  PASS, 45 tests/174 subtests, including missing, duplicate and contradictory
  termination/completion evidence controls.
- `py -3 scratch/e4_fixes_grade_files.py`: production `grade_file` accepted
  13/13 actual widget-test output directories. The first direct grader invocation
  was blocked by missing `shell.log`: offline tests write `widgets.log`. The
  wrapper copies that unchanged evidence to `shell.log` before grading.
- `py -3 scratch/e4_fixes_contracts.py` runs
  `py -3 tools/build_contract_harnesses.py --run --unit` with a unique endpoint,
  writable TEMP/TMP and a 300-second outer bound: PASS, 3115 native cases
  (4 existing skips, deliberate expected-failure assertions) and all four Python
  suites. Release-tree checks were explicitly no-ops without a configured root.
- `git diff --check`: PASS. Bash check-contracts and the committed build/deploy/
  fingerprinted lane3 runtime checkpoint remain the orchestrator's work.
  No index writes, commits, deployment or engine/editor launches occurred here.

Additional retained scratch: the two review-fix `e4-proofs` directories above,
`scratch/e4_fixes_grade_files.py`, `scratch/e4_fixes_contracts.py`,
`scratch/e4-fixes-contracts.log`, and new children of the previously listed test
artifact roots (including the 13 copied `shell.log` files). Nothing was deleted.

E5a adds an instance-owned file resolver and object catalog. No chooser, placement
policy, insert-command fields or shipping shell flow changes are included.

- `FileResolver` mounts the active mod's `data/`, then its `mod.json` dependencies
  in declared order, then launcher-supplied dependencies, then base loose files,
  then the archives in base `system.cfg`'s `[FastFiles]` order. Keys fold case and
  slashes. The first matching file replaces the entire lower-priority file.
  Files under dot directories are excluded. Traversal keys are rejected.
  Archive handles and decoding dictionaries are local; no engine VFS owner or
  cache is imported. FST table bounds, stored hashes, raw/zlib/Microsoft LZW data
  and decoded lengths are validated.
- Enumeration is `data/art/Buildings.csv` (the production 12-column schema),
  with FitID selecting a packet in `data/objects/object2.pak`.
  `[ObjectClass] ObjectTypeNum` determines the runtime family; CSV appearance
  type remains a separate fact. Bridge, spotlight and marker specializations are
  retained. FitIDs can have multiple CSV identities (stock drop zone/streetlight
  114); duplicate stems are rejected. The group, name ID, special type, tactical
  map bitmask, scale, footprint, hover and line-of-fire facts are retained.
- `data/objects/<stem>.csv` supplies variant names from row `23 + 97*i`, column 2,
  for indices 0..15. Optional absence means no named variants. An unreadable or
  malformed present variant file rejects that entry. Variant provenance is owned.
- Resource names come from the owned en_us FIT string index and explicit
  `LegacyTextAlias` mappings, with numeric legacy strings as fallback. The index
  covers the install's text/debug/game-menu/viewer-menu roots through the resolver.
  Escaped FIT strings are decoded; callbacks own their index. Missing text uses
  the legacy stem/name-ID fallback. No resource DLL or MC2Strings static owner is
  used. External `MC2_DEFS_ROOT` and other locale selection are outside this slice.
- Catalog validation covers the facts consumed by the catalog, rather than
  unrelated combat tuning. Each invalid row emits one `EDITOR_CATALOG_REJECT`
  line and later valid rows continue. Packet damage is isolated per row.
  Malformed string entries have separate `EDITOR_RESOURCE_REJECT` diagnostics.
  PlacementPolicy owns team/player profiles, pilot/brain selection, unit
  colors/damage/variant and self-repair values (E5b-1).

Offline proof commands (all builds are RelWithDebInfo; the runners bound children
and use a unique `_MSPDBSRV_ENDPOINT_`):

```
py -3 editor/tests/run_document_proofs.py
py -3 tools/build_quiet.py --target editor_catalog_probe
py -3 editor/tests/run_catalog_proofs.py
py -3 scripts/run_editor_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe" --case catalog_stock --case catalog_mod --catalog-mods A:/Games/mc2-opengl --catalog-mod MC2X-CVE-G --out scratch/e5-catalog-harness
py -3 -c "import tempfile, pytest; tempfile.tempdir='A:/Games/mc2-codex/scratch'; raise SystemExit(pytest.main(['scripts/tests/test_editor_smoke.py','--capture=sys','-p','no:tmpdir','-p','no:cacheprovider','-q']))"
py -3 tools/build_contract_harnesses.py --run --unit
```

`catalog_stock`/`catalog_mod` are distinct offline harness cases; they never launch
an engine and never change the stock runtime cases' mod refusal/child clearing.
The mod case mounts MC2X-CVE-G's existing data over lane3's stock install.
The Python oracle independently indexes loose layers and decodes configured FSTs;
every resolved CSV/PAK/variant file must match its bytes, source and layer.
An archive-only fixture hard-links the stock FSTs into scratch and exercises
Buildings.csv and variant fallback without loose shadows. Known mod assertions
cover FitID 2's provenance and FitID 252's changed decoded definition/rejection;
stock's malformed MilCheckPoint row must remain rejected. Counts are recomputed
from entries, not trusted from the probe summary. These are catalog/resolution
proofs, not new-placement or mod runtime placement proofs.

The legacy reference case is separate from the existing `workflows` case. It
uses the old BuildingBrush/action manager with requested team 0 to identify and place Anubis 53,
APC 117, Barrels 263, AutoCannonCamo1 525, Birch1 252, D_HeavyGate 157, bridge 607,
Spotlight 68 and Nav1 290, independently from one saved baseline. Every placement
must undo, redo, save, disappear from memory and reopen at the identified position.
Legacy non-alignable objects retain neutral team -1. New units are identified by
ObjectNumber and XY because the legacy writer sorts Parts by player and squad.
The grader requires exactly one changed unit/packet placement, complete unit
fields and pilot/brain defaults, and exact identified object numbers. Acceptance
also reads the pinned frozen files and compares every inserted Part/Warrior/cell
value, decoded building record and marker FIT record. Ordinary
building records encode forest ID -1 as integer 0 and scale as integer 0; these
goldens do not claim a non-default forest-instance reference.

After the orchestrator commits/builds/deploys the reviewed source to lane3, run
these exact stock runtime commands (the harness owns the host lease; no bypass):

```
py -3 scripts/run_editor_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe" --shell-exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor Smoke.exe" --prepare-stock-scratch --case stock_roundtrip --case workflows --case new_model --keep-logs --exit-sec 3 --timeout 180
py -3 scripts/run_editor_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe" --prepare-stock-scratch --case legacy_catalog_reference --compare-legacy-reference editor/tests/goldens/e5a-stock --keep-logs --exit-sec 3 --timeout 180
```

The one-time capture command was separate from acceptance and is now spent:

```
py -3 scripts/run_editor_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe" --prepare-stock-scratch --case legacy_catalog_reference --freeze-legacy-reference editor/tests/goldens/e5a-stock --keep-logs --exit-sec 3 --timeout 180
```

It refuses the existing directory before launching. Never rerun it to replace
the corpus. The nine FIT/PAK pairs and `placements.json` are committed in
2e525e45a, with byte-exact attributes in 090e4f938. The comparison target
is the instrumented, still-legacy editor, with semantic FIT fields and decoded
packet-1 placement records. Whole edited PAK byte equality is not a parity claim:
the owned writer deliberately stores edited placements raw.

E5b-1 placement policy and proofs:

- `PlacementPolicy` owns ordered good/enemy pilot tables and each resolved FIT
  provenance. Missing pilot FITs are skipped; malformed present inputs reject the
  whole policy with one diagnostic. Raw and bounded zlib pilot tables are accepted.
  Pilot descriptions may contain multiple numeric resource IDs; unrelated skill
  tuning is outside this policy's reader. No legacy static owner is imported.
- `PlacementChoice.player` is the CommanderID and determines PlayerPart, pilot,
  PBrain versus DredAttack01, and mech/vehicle profiles. Its explicit `team` must
  equal PlayerN.DefaultTeam and becomes a unit's TeamID. Thus player 0 on team 3
  retains player defaults, while player 2 mapped to team 0 retains enemy defaults.
  Alignable packet objects retain the chosen player alignment, as the legacy
  building writer does; non-alignable objects serialize -1.
- Unit insertion writes damage, all three colors, variant, self-repair, signed
  brain counts and complete cells. Enemy defaults are cells (2,1,150), (9,0,0),
  (28,0,0). New squads follow the largest authored squad. Empty alternative
  cursors match legacy export ordering; existing Parts/Warriors remain authored.
- `forestInstance` is an instance ID, never CatalogEntry.forestGroup. It encodes
  as instance + 1; ordinary objects write scale 0 regardless of catalog scale.
  Markers also add their owned xPos/yPos/radius FIT record. The frozen animated
  Nav1 preview saved rotation 4.5; parity replays that recorded transform explicitly.
- The shell owns the resolver/catalog/policy and wires existing placement buttons
  through it whenever asset inputs are mounted. `--base`, `--mods`, `--mod` select
  those inputs; player identity has its own placement input. Synthetic panel/file
  smoke fixtures retain their primitive insert path. The chooser, browse/search
  UI and its rendered interaction proofs belong to E5b-2.
  At the E5b-1 checkpoint, marker transform/delete buttons were disabled (superseded by E5b-2 below). Document commands rejected them
  before any mutation with one `[EDITOR_ENTITY_REJECT v1]` diagnostic per attempt
  and reason `marker_fit_sync_required`, including unchanged or invalid transforms.
  FIT XY references protect imported/reopened markers without a catalog; coincident
  packet entities are conservatively protected because those references carry no
  object identity. Marker FIT synchronization belongs to E5b-2 with the chooser.
- `run_document_proofs.py` verifies pinned manifest/baseline fingerprints before
  building. Normal and ASan native tests independently place all nine families,
  undo/redo/save/reopen, then Python compares the outputs to the frozen corpus.
  Six saved player/enemy/remapped-team cases resolve expected pilots independently.
  A saved MC2X-CVE-G FitID 2 placement reports the overriding object2.pak and all
  consumed sources; independent resolution verifies their bytes, layers and paths.
- `test_placement_proofs.py` rejects same-count field corruption, changed brain
  cells, extra cells, altered marker FIT records and decoded building corruption.
  It accepts equivalent raw packet storage and tests refusal to freeze again.
- Native marker rejection tests retain FIT/PAK bytes, revision, saved state, history
  budget and an existing redo branch before and after reopen. ImGui clicks on both
  disabled marker buttons cannot activate commands. `owned_marker_reload` copies
  the native owned marker save into lane3, loads it through the legacy reader,
  requires a clean autoload/save, then compares re-saved FIT marker values and
  decoded packet records. Missing placement, failed autoload and altered records
  reject; this control is explicit and never replaces a frozen reference.

E5b-1 offline acceptance (runtime acceptance above remains the orchestrator's
committed build/deploy checkpoint):

```
py -3 editor/tests/run_document_proofs.py
py -3 -B -c "import tempfile,pytest; tempfile.tempdir='A:/Games/mc2-codex/scratch'; raise SystemExit(pytest.main(['scripts/tests/test_editor_smoke.py','editor/tests/test_catalog_proofs.py','editor/tests/test_placement_proofs.py','--capture=sys','-p','no:tmpdir','-p','no:cacheprovider','-q']))"
py -3 tools/build_contract_harnesses.py --run --unit
```

E5b-1 reviewed offline checkpoint (2026-10-14): normal and ASan each pass
40 cases/114789 assertions, strict stock 24/24 unchanged FIT/PAK pairs and
24/24 corrupted FIT controls. Both saved-output grades pass all nine frozen
families, six independent player-policy cases and the actual mod placement's
required provenance files. Widget tests pass 7 cases/194 assertions; shipping
proof markers remain isolated. All five builds have zero warnings. Python
grading passes 61 tests/339 subtests. Contract --run --unit passes 3115 native
cases (four existing skips and deliberate negative controls) and four Python
suites; release-tree checks remain explicit no-ops without a configured root.

E5b-1 P1 proof artifacts: `scratch/e4-proofs/2026-10-14T15-12-35`.
Legacy runtime acceptance remains the orchestrator's committed build/deploy
checkpoint. After deploying that commit to lane3, run this positive reload
control against the native owned output (the ASan output is independently saved).
The control refuses any install except lane3 or a dirty/absent/stale executable
fingerprint, and records the accepted executable and input hashes:

```
py -3 scripts/run_editor_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe" --prepare-stock-scratch --case owned_marker_reload --owned-marker-placement scratch/e4-proofs/2026-10-14T15-12-35/editor_document_tests-run-placements/marker-rejected --keep-logs --exit-sec 3 --timeout 180
```

This runtime control has grader failure controls but has not been launched from
the uncommitted tree. The orchestrator also runs the fingerprinted shell/runtime
proofs above; offline parity does not replace that checkpoint.
No timeout occurred. Evidence: `scratch/e4-proofs/2026-10-14T14-49-54`,
`scratch/e5b1-proofs/2026-10-14T14-53-40` (contracts), and
`scratch/e5b1-proofs/2026-10-14T14-56-12` (final strengthened grading).
The orchestrator still owns Bash check-contracts, commit/build/deploy and the
fingerprinted lane3 runtime commands above. No frozen file or generated baseline
was changed; no index write, deployment, engine launch or deletion occurred.

E5a offline checkpoint (2026-10-14, uncommitted):

| Family | Stock | MC2X-CVE-G overlay |
|---|---:|---:|
| Mech | 32 | 33 |
| Vehicle | 32 | 31 |
| Building | 282 | 318 |
| Turret | 17 | 17 |
| Tree | 42 | 4 |
| Gate | 9 | 9 |
| Bridge | 1 | 1 |
| Light | 1 | 1 |
| Marker | 2 | 2 |
| Rejections | 2 | 4 |

- Final document proof: `scratch/e4-proofs/2026-10-14T10-59-20`. Normal and ASan
  each passed 31 cases/114187 assertions, strict 24/24 unchanged FIT/PAK pairs
  and 24/24 corrupted FIT controls. Widget tests passed 6 cases/185 assertions;
  shipping/smoke marker isolation passed. All five builds had zero warnings.
- Catalog/resolver fixtures passed 10 cases/107 assertions. Real install oracle
  comparisons passed stock 34/34, archived-stock 34/34 and MC2X-CVE-G 62/62.
  Both standalone and distinct catalog harness commands passed; artifacts live
  under `scratch/e5-catalog-proofs` and `scratch/e5-catalog-harness`.
- Python smoke grading tests passed 48 tests/181 subtests, including immutable
  reference, incomplete pilot/brain/unit data, reordered Parts, no-op outputs,
  wrong identities, missing/duplicate/false operation facts and integer forest/
  scale controls. The nine real reference outputs remain pending runtime proof.
- `scratch/e5_build_final.py` built EditRel and the probe with 300-second bounds
  and a unique PDB endpoint. EditRel had three existing resource macro warnings
  (`IDI_ICON1`, `IDSAVE`, `IDC_DELETE`); the probe had zero. Raw warning evidence
  is `scratch/e5-warning-details.log`. No compiler timeout occurred.
- `scratch/e5_run_bounded.py` ran `tools/build_contract_harnesses.py --run --unit`:
  PASS, 3115 native cases (4 existing skips and deliberate expected failures),
  plus four Python suites. Release-tree deployment tests were explicitly no-ops
  without a configured root. `git diff --check` passed.
- Bash check-contracts, index writes/commits, deployment and fingerprinted engine/
  editor runtime cases were not run, per the orchestrator checkpoint. No lane-owned
  source files or generated baselines were edited; no scratch cleanup occurred.

E5a pre-commit review fixes (2026-10-14, uncommitted):

- The legacy grader removes the identified insertion and compares canonical
  multisets of all existing Parts and Warriors, including pilot associations and
  brain cells. Part/Warrior renumbering and alternative export references retain
  their semantic identity. Building insertions must preserve the same unit state.
  Replacement, field changes and missing existing units are rejected.
- Saved units must carry the requested TeamID, CommanderID and PlayerPart values,
  a nonblank pilot profile and the team-0 PBrain defaults (zero cells/static vars).
  Empty/blank/wrong brains, wrong alignment and invalid defaults are rejected.
- Resolver coverage derives required files from independently mounted inputs and
  Buildings.csv stems. Every present enumerated variant is required even if the
  probe omits it. Missing report entries, missing output files and byte corruption
  fail the proof; its denominator comes from this independent expected set.
- Catalog definitions accept storage type 1 (FWF) as raw, matching decodePak and
  the legacy reader. Resource blocks use balanced braces outside quoted strings,
  including escaped quotes, indented endings and multiple same-line entries.

Proof commands and retained evidence:

```
py -3 -B scratch/e5a_fixes_proof.py negative
py -3 -B scratch/e5a_fixes_proof.py verify
py -3 -B scratch/e5a_fixes_proof.py contracts
py -3 -B scratch/e5a_fixes_proof.py python-negative
py -3 -B -c "import tempfile,pytest; tempfile.tempdir='A:/Games/mc2-codex/scratch'; raise SystemExit(pytest.main(['scripts/tests/test_editor_smoke.py','editor/tests/test_catalog_proofs.py','--capture=sys','-p','no:tmpdir','-p','no:cacheprovider','-q']))"
git diff --check
```

- Removing each fix made its negative control fail. Native FWF and brace controls
  exited 1 after successful zero-warning builds; source bytes were restored in
  finally blocks. Independent Python mutations separately prove preservation,
  saved alignment, empty-brain rejection, variant omission and byte corruption.
  Evidence: `scratch/e5a-fixes/2026-10-14T11-14-42`,
  `2026-10-14T11-18-31` and `2026-10-14T11-20-55`.
- `verify` ran `py -3 editor/tests/run_document_proofs.py`: PASS, normal and ASan
  each 33 cases/114200 assertions, 24/24 unchanged stock FIT/PAK pairs and 24/24
  corrupted FIT controls. Widget tests passed 6 cases/185 assertions; shipping/
  smoke marker isolation passed. All five target builds had zero warnings.
  Document artifacts: `scratch/e4-proofs/2026-10-14T11-15-09`.
- `verify` rebuilt `editor_catalog_probe` with zero warnings, then ran
  `py -3 editor/tests/run_catalog_proofs.py --out scratch/e5a-fixes/2026-10-14T11-15-09/catalog`:
  PASS, independently required stock 34/34, archive-only stock 34/34, mod 62/62
  files identical in bytes and provenance. Family counts/rejections remain those
  in the E5a table above. Logs/commands: `scratch/e5a-fixes/2026-10-14T11-15-09`.
- Final Python suite: PASS, 51 tests/223 subtests, including reversed existing
  Part/Warrior numbering, pilot reassignment/type changes and every new control.
  An initial test run failed on a missing test-only `re` import; it was corrected.
- `contracts` ran `py -3 tools/build_contract_harnesses.py --run --unit`: PASS,
  3115 native cases (4 existing skips and deliberate expected-failure controls)
  and four Python suites. Release-tree checks remained explicit no-ops without
  a release root. Evidence: `scratch/e5a-fixes/2026-10-14T11-17-26`.
- No compiler/test timeout occurred. Bash check-contracts and the committed
  build/deploy/fingerprinted lane3 runtime checkpoint remain the orchestrator's
  work. No index writes, commits, baseline regeneration or engine launches here.

Additional retained scratch: `scratch/e5a_fixes_proof.py`, the evidence directories
above, and new children of `scratch/e5a-fix-resolution-tests`,
`scratch/editor-smoke-tests`, `scratch/e5-catalog-tests`,
`scratch/q3-document-tests`, `scratch/e3a-document-tests`,
`scratch/e3b-widget-tests`, `scratch/e4-widget-tests`, `scratch/e4-recent-tests`
and `scratch/editor-reader-tests`, plus normal generated build/test artifacts.
No scratch cleanup or tracked deletion occurred.
`scratch/e5a-fixes-artifacts.json` lists retained scratch files created/updated;

## E5b-2: marker synchronization and the SDL chooser

Marker transform and delete are enabled. A structural command publishes FIT and
packet changes together; undo and redo restore both exact byte streams. Transform
retains radius and changes the packet's full transform plus FIT XY. Delete removes
both records, decrements both counts, renumbers surviving FIT blocks and preserves
their stable node IDs and unknown fields. The document owns NavMarker object IDs
from the resolved catalog; ordinary buildings and DropZone entries cannot own a
NavMarker FIT record. Initial loads, structural candidates, staged save verification
and file reloads retain that classification. FIT block count, NumNavMarkers and
catalog-classified packet marker count must agree exactly. The owned reader rejects
missing blocks, malformed counts/radii, unmatched XY and ambiguous marker matches
atomically with one diagnostic.
Ambiguous XY ownership and moves onto another marker reject before mutation; FIT
contains no packet ID with which to choose a radius owner safely.

The chooser uses the E5a catalog and the E5b-1 placement policy. It filters by family
and case-insensitive display name or stem, previews ID/family and enumeration,
definition and variant provenance, and places using the current player's DefaultTeam.
Its drafts and selection belong to EntityPanels; authored changes belong to history.
The Entity properties button switches back to the unit/building/player panels.

`chooser`, `chooser_mod`, `chooser_player`, `chooser_render` and `marker_panel` are
smoke-host cases. They reach ordinary ImGui widgets using input events, grade saved
undo/redo bytes, grade changed browse/search results, save through the file workflow
and reopen a changed in-memory mission through the discard dialog. The nine stock
chooser placements compare complete fields with the immutable E5a corpus. Every
case requires byte equality of edited/redone FIT and PAK with its same-placement
e5_reference artifacts, and of the final widget-save pair with its last placement.
The fixture pair also equals the same-baseline e5_reference_before pair. The mod
case grades FitID 2 and independently required source layers/paths; player 2 proves
enemy defaults despite DefaultTeam 0. Native widget tests run the same cases without
GL and are explicitly graded as rendered=0. `chooser_render/panels.bmp` is separate
runtime visual evidence, never a substitute for the changed-output proof.

Comparison rules per chooser case:
- `chooser`: widget/reference bytes equal for all nine families; complete typed
  unit/Warrior/cell fields, marker FIT values and decoded placements equal the
  frozen legacy corpus semantically (export formatting and compression may differ).
- `chooser_render`: widget/reference bytes equal; the widget's complete inserted
  unit fields equal the frozen stock mech semantically.
- `chooser_mod`: widget/reference bytes equal; the widget's complete inserted unit
  fields equal the frozen mech semantically with catalog FitID/CSV identity replaced
  by the independently resolved mod identity; resolved source layers/paths must agree.
- `chooser_player`: widget/reference bytes equal; widget output retains authored
  units and satisfies independently resolved player 2 pilot/brain/cell/team defaults.
  The frozen player 0 placement is not the same semantic placement.

The proof runner and smoke harness retain a corrupted VariantNumber in edited and
redone widget FITs for every chooser case, also corrupting the final widget save
for single-placement cases. References stay clean, and matching edited/redone bytes
cannot satisfy the widget/reference grade. A marker FIT-only position change must
also fail its semantic grade before the original files are restored.
Reader desync, ambiguous references,
destination collisions, count changes and exact history are also native tests.
No frozen golden, generated baseline, index or lane-owned source is changed.

Offline proof commands (the document runner builds all five targets with bounded
compiler probes and a unique PDB endpoint, then runs normal and ASan):

```powershell
py -3 editor/tests/run_document_proofs.py
py -3 editor/tests/run_catalog_proofs.py
py -3 -m pytest scripts/tests/test_editor_smoke.py editor/tests/test_placement_proofs.py editor/tests/test_catalog_proofs.py --capture=sys -p no:tmpdir -p no:cacheprovider -q
$env:_MSPDBSRV_ENDPOINT_ = 'sol-e5b2-contract-' + $PID
py -3 tools/build_contract_harnesses.py --run --unit
git diff --check
```

After the orchestrator commits, builds and deploys EditRel, EditorShell and
EditorShellSmoke into lane3, these are the exact smoke-host commands. Bash
check-contracts remains the orchestrator's command. Run the first command to create
the artifacts consumed by the two independent legacy reload controls; none replaces
or regenerates a frozen reference. Each reload prepares stock data again because
legacy editor runs write terrain texture composites (`.txm`) and lists (`.lst`)
into lane3's data directory. The dependency audit still rejects non-stock content.
The legacy baseline FIT and PAK content stay byte-exact; only the six-packet PAK's
final raw 16-byte save GUID may differ, since each legacy save calls `CoCreateGuid`.
Inspect the separate chooser render capture.

```powershell
$e5b2Editor = 'A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe'
$e5b2Host = 'A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor Smoke.exe'
py -3 scripts/run_editor_smoke.py --exe $e5b2Editor --shell-exe $e5b2Host --prepare-stock-scratch --case stock_roundtrip --case new_model --case legacy_catalog_reference --compare-legacy-reference editor/tests/goldens/e5a-stock --case chooser --case chooser_mod --case chooser_player --case marker_panel --case chooser_render --keep-logs --exit-sec 3 --timeout 180 --out scratch/e5b2-runtime
$e5b2Artifacts = (Get-ChildItem scratch/e5b2-runtime -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
py -3 scripts/run_editor_smoke.py --exe $e5b2Editor --prepare-stock-scratch --case owned_marker_reload --owned-marker-action moved --owned-marker-placement "$e5b2Artifacts/marker_panel/marker-transform-edited" --keep-logs --exit-sec 3 --timeout 180 --out scratch/e5b2-moved-reload
py -3 scripts/run_editor_smoke.py --exe $e5b2Editor --prepare-stock-scratch --case owned_marker_reload --owned-marker-action deleted --owned-marker-placement "$e5b2Artifacts/marker_panel/marker-delete-edited" --keep-logs --exit-sec 3 --timeout 180 --out scratch/e5b2-deleted-reload
```

Runtime execution, legacy reload and visual capture are pending the required
committed build/deploy checkpoint. Scratch is retained under `scratch/e4-proofs`,
`scratch/e5b2-widget-tests`, `scratch/e5-catalog-proofs`, `scratch/e5b1-negative`,
`scratch/e5-catalog-tests`, `scratch/editor-smoke-tests`, the existing document/file/
widget test scratch roots and `scratch/editor-reader-tests`; the contract log is
`scratch/e5b2-contract.log`. No files were deleted. The marker save-corruption bug
row belongs in the orchestrator's BUGS-FOUND-FIXED.md after the commit SHA is known.

Offline checkpoint: `scratch/e4-proofs/2026-10-14T15-44-58` passes normal and
ASan (40 cases/114736 assertions each), 24/24 stock pairs, 9/9 frozen placements,
all five real widget cases (7 panel/file cases/199 assertions overall), mod/current
player grading and both actual-output negative controls. Shipping proof isolation
passes; all five builds have zero warnings. Python passes 64 tests/349 subtests.
Catalog resolution matches 34/34 stock, 34/34 archive-only and 62/62 mod inputs.
Contract --run --unit passes 3115 native cases (four existing skips and deliberate
negative controls) plus four Python suites; release-tree checks remain explicit
no-ops without a configured root. Intermediate compiler/widget/grader failures
were corrected; no compiler timeout occurred. `scratch/e5b2-artifacts.json` lists
retained scratch files created or updated in this task; no scratch was deleted.
`scratch/e5a-fixes-reviewed.patch`, `scratch/e5a-fixes-changes.json`,
`scratch/e5a-fixes-pathspec.txt` and `scratch/e5a-fixes-commit-message.txt` retain
the reviewed diff and the single proposed commit. Scratch is excluded from it.

Review-fix checkpoint: `scratch/e4-proofs/2026-10-14T16-08-13` passes normal and
ASan (40 cases/114766 assertions each), all five native widget cases, 24 stock
pairs, nine frozen placements and all four chooser widget-output corruption
controls plus the marker desync control. Python passes 66 tests/351 subtests;
catalog resolution remains 34/34 stock, 34/34 archive-only and 62/62 mod inputs.
All six editor target builds have zero warnings. Shared contracts pass 3115 native
cases (four existing skips and deliberate negative controls) and four Python
suites; release-tree checks remain no-ops without a configured root. No compiler
timeout occurred. `scratch/e5b2-review-proof.txt` records commands and pending
orchestrator proofs; `scratch/e5b2-review-reviewed.patch`, `-pathspec.txt`,
`-commit-message.txt` and `-changes.json` retain the single reviewed commit package.
`scratch/e5b2-review-artifacts.json` lists retained scratch files. None were deleted.

E6a adds [existing-objective scalar editing](OBJECTIVES-E6a.md) to the owned model
and SDL panels. That note contains the complete stock/legacy census, authoring
and runtime schemas, opaque-entry/reference policy, E6b..E6e follow-ups and exact
post-commit acceptance commands. Offline checkpoint
`scratch/e4-proofs/2026-10-14T20-01-52`: 47 document cases/121479 assertions;
ASan 41 core + 6 placement cases with the same 121479 assertions; 24 unchanged
stock pairs; independently decoded values for 173 objectives; all 26 scalar
widget operations; seven panel/file cases/203 assertions; shipping isolation.
All six editor proof targets build. Python passes 71 tests/364 subtests.
The real disk/RAM FIT readers pass two cases/581 assertions, including multiline
widget saves, empty/trailing lines, literal backslash-n and the 2000-character
boundary. All scalar prefixes and aliases are validated against those readers;
mismatches reject once without changing bytes/history. Multiline import keeps
original bytes; edits preserve the intended game text through save/reopen/undo.
Shared contract `--run --unit` passes 3115 native cases and all four Python
suites; the legacy `EditRel` build also passes with zero warnings.
Exact species names match the game; case variants remain opaque. Rejected
commands also preserve subsequent syntax-ID allocation.
One combined ASan test run timed out at 120 seconds; complementary suites now
retain that process bound and prove complete coverage. No compiler timed out.
Rendered lane3 smokes, strict legacy 24/24 and owned-objective legacy reload
remain at the orchestrator's committed/fingerprinted checkpoint. No deploy or
commit was performed. `scratch/e6a-resume-reviewed.patch`,
`scratch/e6a-resume-changes.json`, `scratch/e6a-resume-pathspec.txt`,
`scratch/e6a-resume-commit-message.txt` and `scratch/e6a-resume-proof.txt` form
the current handoff package; `scratch/e6a-resume-artifacts.json` lists retained
scratch files. No source files were deleted.
