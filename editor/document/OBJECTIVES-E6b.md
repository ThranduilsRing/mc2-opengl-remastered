# E6b objective CRUD

This slice delivers objective add, copy, delete and reorder in the owned document
and SDL panel. Typed entity references are deferred in full to
[E6b-refs](OBJECTIVES-E6b-refs.md). Existing entity commands remain reference-unaware
until that slice. Payload/retarget editors, flags, templates, localization and
runtime trigger/reward/video outcomes remain E6c-E6e.

CommandHistory owns one structural command per operation. Add appends to the
chosen team, using [legacy constructor defaults](../Objective.cpp:184), the
default model definition (empty name, type zero, scale one), ModelID -1,
authoring ModelID Version 2 and four empty lists. Copy inserts immediately after
its source within that team. It copies all four lists, unsupported species,
unknown payloads, runtime state, auxiliary reference fields, nested extensions
and comments. Reference targets/lexemes remain unchanged. Delete removes that
objective's blocks and compacts only its team's objective indices. Reorder uses
a final index within the same team; the panel exposes Move up/Move down.

Reorder changes positional prerequisite meaning, as defined by
[the game reader/status iteration](../../code/objective.cpp:1684). All prerequisite
flags retain their values; no invented dependency rewrite is performed. The
panel states that consequence beside the order controls. Teams with zero
objectives still expose Add; version 2 exposes only team 0, version 3 all eight.

## Preservation and transactions

Unedited objectives' bytes remain exact **except renamed section headers and
necessary count values**. Logical order is the exported objective index, not
physical FIT block order. Only TeamNObjectives/NumObjectives (or version 2's
Objectives/NumObjectives) and affected section headers change. All four list
counts and payload bodies remain exact on copies/reorders; new lists have zero
counts. Root objective blocks and their suffixed payload/extension blocks travel
together; the numeric objective token has an exact boundary so objective 1 does
not capture objective 10. Nested blocks remain part of their root syntax span.
Unknown internal keys and numbers are never renumbered. Unrelated teams, mission
syntax and the entire PAK remain exact. Existing orphan-header collisions reject
rather than overwrite content.

Each candidate is decoded and validated before mutation. Surviving objective,
entry and field NodeIds and all EntityIds retain identity through section
renaming. Copies receive fresh syntax IDs. Existing bounded structural history
stores the complete reversible syntax/objective/entity state. No separate undo
stack or static-duration owner exists. A rejected command emits one owned
diagnostic and retains bytes, revision, dirty state, executable history and
future ID allocation. No-op reorder retains redo. FIT/node/history limits reject;
there is no truncation. Undo restores original spelling, comments and unknown
entries; redo restores exact changed output. Save/reopen uses the existing
transactional paired-file workflow.

## Offline proof

`py -3 editor/tests/run_document_proofs.py` retains all E6a normal/ASan,
24 stock FIT/PAK pair, nine frozen-placement, typed scalar and real reader gates.
CRUD document tests cover every command's changed output, save/reopen, exact
undo/redo, versions 2 and 3, populated multiple teams, empty teams, four lists,
unsupported payloads, stable IDs and interleaved entity/objective history.
Oversized copies, header collisions, invalid IDs/teams/indices and history limits
reject atomically once. The independent Python grader checks expected counts,
order, constructor defaults and all surviving block bodies. Native PanelProof
drives ordinary ImGui mouse/key events for add/copy/delete/up/down and the
Save/Reopen widgets. Both reorder directions are tested. No-op, consistently
wrong edited/redo pairs and missing artifacts must fail the gate.

`py -3 -B -m pytest scripts/tests/test_editor_smoke.py --capture=sys -p no:tmpdir -p no:cacheprovider -q`
checks the graders and reload CLI, including corrupt/missing FIT/PAK, fixture,
input and gate files, no-op and wrong-count legacy re-saves. The runtime CRUD
reload control binds **both** fixture FIT and PAK to the panel's input hashes
and input copies, independently grades expected counts/order/content, rechecks
the complete panel gate, then compares all objective fields after legacy reload.
Both re-saved PAKs must decode every packet; edited packet-1 records must match
a same-run, same-executable null-edit save of the unedited panel fixture.
This accounts for legacy position quantization; CRUD-only differences reject. Whole-archive
byte equality is not required: legacy quick-save repacks the archive, omits
derived movement/tactical-map packets and generates a fresh GUID. Corrupt or
missing re-saved PAKs reject before publishing reload artifacts, as do changed
placement records; storage encoding and record order may differ.
It does not use a legacy golden as the CRUD input oracle. Frozen placements
remain a separate regression gate.

Owner 2026-10-14 correction supersedes E6a's dialog bounds: signed 32-bit integers and finite floats (including zero/negative scales) are editable; `ul ResourcePoints = -55000` is a signed deduction ([read](../../code/objective.cpp:223), [conversion](../../mclib/inifile.cpp:471), [reward](../../code/logisticsdata_save.cpp:469), [addition](../../code/logisticsmissioninfo.h:73)). Integer overflow/wrap remains read-only with that reason; a nonempty model requires camera types 5/13/14/15 ([asserting consumer](../../code/simplecamera.cpp:811)). Scalar panel proof seeds a valid camera type and reload compares its own fixture.
Ignored optional scalars retain opaque bytes and warning "ignored by the game" ([guarded reads](../../code/objective.cpp:199), [optional reads](../../code/objective.cpp:1444), [text skip](../../code/objective_text.cpp:31)); the panel shows the game's fallback and offers "author this field as the game reads it". Only an explicit set writes its type/value; undo restores the original. Required-count/payload failures and unsafe DisplayMarker still reject once ([required count](../../code/objective.cpp:1501), [unsafe temporary](../../code/objective.cpp:1459)).
Objective census remains Exodus 0/328, MagicExpansion 0/173, Carver5 0/173, Omnitech 0/445 rejected blocks; Omnitech now has 176 ignored-line warnings, and the negative ResourcePoints value has no warning. All 68 full Omnitech FIT/PAK scratch copies save/reopen byte-identically without authoring.

## Orchestrator acceptance commands (after commit)

Build/deploy/runtime acceptance belongs to the orchestrator at the committed,
clean fingerprint. This task does not commit or deploy. In PowerShell:

```powershell
$env:_MSPDBSRV_ENDPOINT_ = 'sol-e6b-accept-' + $PID
py -3 editor/tests/run_document_proofs.py
py -3 tools/build_contract_harnesses.py --run --unit
py -3 tools/build_quiet.py --target EditRel
py -3 scripts/deploy_payload.py A:/Games/mc2-opengl/mc2-win64-lane3 --target editor --allow-new-target --require-clean
py -3 scripts/deploy_payload.py A:/Games/mc2-opengl/mc2-win64-lane3 --target editor --exe-name 'Mission Editor Shell.exe' --allow-new-target --require-clean
py -3 scripts/deploy_payload.py A:/Games/mc2-opengl/mc2-win64-lane3 --target editor --exe-name 'Mission Editor Smoke.exe' --allow-new-target --require-clean
py -3 scripts/run_editor_smoke.py --exe 'A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe' --case objectives_crud_panel --case objectives_panel --case stock_roundtrip --prepare-stock-scratch --keep-logs --exit-sec 3 --timeout 180 --out scratch/e6b-runtime
# Substitute the objectives_crud_panel artifact directory printed by that run.
$e6bPanel = 'scratch/e6b-runtime/<reported-run>/objectives_crud_panel'
foreach ($operation in 'add','copy','delete','up','down') {
    py -3 scripts/run_editor_smoke.py --exe 'A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe' --case owned_objective_crud_reload --owned-objective-crud-edit "$e6bPanel/objective-$operation-edited" --prepare-stock-scratch --keep-logs --exit-sec 3 --timeout 180 --out "scratch/e6b-reload-$operation"
    if ($LASTEXITCODE -ne 0) { throw "CRUD reload failed: $operation" }
}
```

The orchestrator also runs `bash scripts/check-contracts.sh` (Git Bash is
forbidden in this sandbox). Keep existing scalar owned_objective_reload and
strict 24/24 legacy round-trip gates. Neither legacy-reader reload nor panel
proof demonstrates runtime trigger/reward/video outcomes. Rendered captures
require the harness host lease; no direct editor/game launches are authorized.
