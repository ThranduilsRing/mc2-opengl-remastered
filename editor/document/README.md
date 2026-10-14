# Owned mission document: Q3

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
unsupported compression is rejected. There is no packet edit command in this slice.

Mission name and author text commands own before/after values. Redo branching and
payload-capacity accounting enforce a bounded history, with an eviction count in
the shell. A document savepoint compares values, so undo across a save remains dirty
and undo back to the saved values becomes clean. Transient window/input state never
enters the history. The shell exposes the stored name and displays its localization
resource separately; it preserves the localization selection.
The name draft uses ImGui's growing `std::string` input adapter, preserving valid
names longer than 2,000 bytes through Apply and Save. The document's named 1 MiB
text limit still applies; an over-limit command is rejected without changing it.

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

Save stages and rereads the FIT/PAK pair before publication. A per-transaction journal
names both destinations and retained previous files. Failed publication rolls back;
pending or invalid journals block loading a potentially mixed pair. Interrupted
transactions require recovery using that journal and its retained previous files.
Successful saves retain previous versions and committed journals. A failed save
does not advance the document savepoint. No ABL or asset contents are rewritten.

Build `EditRel`, `EditorShell`, and `editor_document_tests` in RelWithDebInfo with a
unique `_MSPDBSRV_ENDPOINT_`. `editor_document_tests_asan` is an independent ASan
target, excluded from the default build. It copies its compiler's ASan runtime.
Deploy both editor executables to lane3 under the host lease. Then run:

```
py -3 scripts/run_editor_smoke.py --exe "A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe" --prepare-stock-scratch
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
loader. Brain-source authoring, packet recooking and project dependency resolution
belong to later workflow slices and are not claimed here.
