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
build timeouts, and runs both native variants. `MC2_EDITOR_STOCK_DIR` selects those
copies; direct tests without it explicitly skip the corpus case. The corpus saves
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
- With `MC2_EDITOR_STOCK_DIR=A:/Games/mc2-codex/scratch/e4-proofs/2026-10-14T08-41-34/stock`,
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
