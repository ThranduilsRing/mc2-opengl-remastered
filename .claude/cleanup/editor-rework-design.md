# E1: a mission is live-playable and editable inside the editor
Author a patrol, assign its brain, press Play, and watch the same game simulation execute it.
One SDL+ImGui application owns the document, undo history, tools and isolated play session.
The viewport uses the game's Vulkan backend through EditorBridge; MFC and its message loop disappear.
Terrain authoring, objectives, teams, campaign links and export are required workflows.
The asset viewer and remaining tools ship too, after the live-play and terrain slices.
Play/pause/step/reset never overwrite authored state; document edits enter the next explicit play snapshot.
Old-editor save drift is a defect: all 24 stock missions must round-trip byte-identically.
C++23 owned values replace EString/EList, private heaps and static-duration owners; invalid readers log and reject.
Design only, 2026-10-13; audit pinned to task A commit `254751f58`, independent of concurrent E-W edits.

## Measured editor debt

Scope: the 229 committed `editor/` C/C++ source/header files at `254751f58`; CMake bridge counted separately.
These are lexical occurrences, including comments/strings, not AST call-site or lifetime counts. Files are unique per row.
Owner counts are explicit allocation expressions inspected in source; MFC counts are declarations, not `CDialog` tokens.

| Debt | Count | Files / interpretation | Removal batch |
|---|---:|---|---|
| Editor-created UserHeap | 2 | 1 file; SYSTEM and TinyGeom; 3 UserHeap/Ptr tokens | B1/B3 |
| Other legacy allocation owners | 1 HeapList + 5 TG pools | 1 file; guiHeap declared, zero creation sites | B1/B3 |
| gos heap/allocator references; Win32 private heap calls | 12; 0 | 4; 0 files; shared MLR/gosFX heap selection is included | B3/B7 |
| EString | 252 | 18 files | B1-B6 |
| EList | 80 | 29 files | B1-B6 |
| MFC-derived classes | 42 direct + 1 indirect | 41 direct-declaration files; TacMapTGA derives through TGAWnd | B2-B7 |
| CDialog classes | 37 | 36 files; includes CAboutDlg | B2-B7 |
| BEGIN_MESSAGE_MAP | 33 | 31 files; some dialogs have no map | B2-B7 |
| CString; DDX bindings | 157; 162 | 45; 24 files; DDX_Text subset 93 in 21 files | B2-B6 |
| Win32 path surface | 91 | 16 files; MAX_PATH plus named path/search/dialog APIs below | B2/B6/B7 |
| Win32 process/thread/handle surface | 33 | 8 files; named APIs below | B2/B6/B7 |
| gameos_editor duplicate compilation | 131 base TUs | Shared base list minus gos_render.cpp; conditional Vulkan TUs additional | B7 |
| EditorGameOS + EditorGosRender bridge | 2,170 lines | 929 + 1,241 lines; separate from EditorBridge module | B2/B7 |

Measured command: the following PowerShell stdin command (read-only; reads committed blobs, so E-W cannot change the count):

```powershell
@'
import re, subprocess
sha = '254751f58'
names = subprocess.check_output(['git','ls-tree','-r','--name-only',sha,'editor'],text=True).splitlines()
names = [n for n in names if n.endswith(('.cpp','.c','.h','.hpp'))]
names += ['GameOS/gameos/CMakeLists.txt']
texts = {n: subprocess.check_output(['git','show',sha+':'+n]).decode('utf-8','replace') for n in names}
src = {n:t for n,t in texts.items() if n.startswith('editor/')}
rules = {
 'UserHeap tokens': r'\bUserHeap(?:Ptr)?\b', 'UserHeap owners': r'\bnew\s+UserHeap\b',
 'HeapList owners': r'\bnew\s+HeapList\b',
 'TG pools': r'\bnew\s+TG_(?:VertexPool|GOSVertexPool|DWORDPool|ShadowPool|TrianglePool)\b',
 'EString': r'\bEString\b', 'EList': r'\bEList\b',
 'MFC direct': r'\bclass\s+\w+\s*:\s*(?:public\s+)?(?:CDialog|CWinApp|CFrameWnd|CWnd|CStatic)\b',
 'MFC indirect': r'\bclass\s+\w+\s*:\s*(?:public\s+)?TGAWnd\b',
 'Dialogs': r'\bclass\s+\w+\s*:\s*(?:public\s+)?CDialog\b',
 'Message maps': r'\bBEGIN_MESSAGE_MAP\s*\(', 'CString': r'\bCString\b',
 'DDX': r'\bDDX_\w+\s*\(', 'DDX_Text': r'\bDDX_Text\s*\(',
 'Win32 paths': r'\b(?:MAX_PATH|GetModuleFileName[AW]?|GetCurrentDirectory[AW]?|SetCurrentDirectory[AW]?|CreateDirectory[AW]?|FindFirstFile[AW]?|FindNextFile[AW]?|FindClose|GetFileAttributes[AW]?|GetFullPathName[AW]?|SHBrowseForFolder[AW]?|SHGetPathFromIDList[AW]?|GetOpenFileName[AW]?|GetSaveFileName[AW]?)\b',
 'Win32 process': r'\b(?:CreateProcess[AW]?|ShellExecute(?:Ex)?[AW]?|CreateThread|TerminateThread|TerminateProcess|WaitForSingleObject|CloseHandle)\b',
 'gos heap refs': r'\bgos_(?:CreateMemoryHeap|DestroyMemoryHeap|PushCurrentHeap|PopCurrentHeap|Malloc|Free)\b',
 'OS heap calls': r'\b(?:HeapCreate|HeapDestroy|HeapAlloc|HeapFree|VirtualAlloc|VirtualFree)\s*\('
}
print('source files', len(src))
for name, pattern in rules.items():
 print(name, sum(len(re.findall(pattern,t)) for t in src.values()), sum(bool(re.search(pattern,t)) for t in src.values()))
b = texts['GameOS/gameos/CMakeLists.txt'].split('set(SOURCES ',1)[1].split('\n    )',1)[0]
print('gameos_editor base TUs', len(re.findall(r'^\s+\S+\.(?:cpp|c)\s*$',b,re.M))-1)
print('bridge LOC', sum(len(texts['editor/'+n].splitlines()) for n in ('EditorGameOS.cpp','EditorGosRender.cpp')))
'@ | py -3 -
```

Ownership evidence: `Editor.cpp` creates HeapList/SYSTEM at 279-288 and TinyGeom/five pools at 668-687; shutdown manually destroys them at 808-896.
SYSTEM defaults to 134,217,728 bytes, TinyGeom to 65,536,000; guiHeapSize=1,023,999 is a setting, not a third allocated heap.
MLR/gosFX heap push/pop is shared-engine debt, not another editor-created UserHeap. Ordinary STL allocations are permitted; private heaps/pools are retired.
`EditorObjectMgr::instance`, `EditorData::instance`, `EditorNavLayer::sInstance`, `MissionFitBrainSource::s_brains`, panel caches and bridge window/context globals are static-owner migration sites.
`EditorGameOS.cpp` supplies InitGameOS/pumping/input/frame/audio shims; `EditorGosRender.cpp` embeds an SDL/GL child HWND and forwards Win32 mouse messages.
`GameOS/gameos/CMakeLists.txt:431-460` recompiles the shared sources with editor-specific defines; `editor/CMakeLists.txt` still requires MSVC, dynamic MFC, WinMain, GLEW/OpenGL and Windows libraries.
Replace these lifetimes with application/session members and shared backend services, not an adapter that preserves MFC/global ownership.

## Document, identity and undo

`EditorApplication` owns PlatformServices, ContentResolver, AssetCatalog, EditorDocument, CommandHistory, ToolState, viewport and optional GameSession.
The document owns terrain tile values, entities, teams/players, objectives/actions/conditions, mission settings, authored camera/environment, brain sources/assignments and campaign graph.
Use `std::string`, `std::vector`, typed IDs/enums/variants, `std::optional` for absence and `std::expected<T, LoadError>` for validating readers; polymorphic resources have one explicit owner.
Entity IDs are stable and distinct from exported Part/Warrior indices; references are IDs, never pointers into appearances or dialog copies. Preserve existing indices; allocate new ones deterministically.
Appearance, nav meshes, lighting, minimap, thumbnails and compiled scripts are revision-keyed derived data. Catalog defaults apply only on explicit creation/type change.
Imported coordinates, Part control/profile/CSV identity and objective ModelType remain document values independent of catalog lookup and viewport snapping.
One shared mission codec serves editor, runtime loader and cooks. A lossless syntax tree owns original FIT bytes, order, typed prefixes, number lexemes, comments, whitespace, unknown fields/blocks and absence.
Semantic typed nodes reference syntax spans; commands dirty only affected nodes. Unchanged spans emit exactly their original bytes; new/changed values use the engine schema's deterministic formatting.
PAK ownership includes original packet/compression bytes and dependency manifests: untouched packets retain their encoded bytes; edited packets recook deterministically. Recompression alone cannot satisfy byte identity.
No-op save and edit-undo-save reproduce original FIT/PAK/ABL bytes, including unknown valid extensions. Resetting semantic values clears their changes against the loaded value, not merely a monotonic dirty flag.
Reader limits cover file/packet sizes, dimensions, counts, index arithmetic, finite coordinates, references and DSL depth/cycles. Reject malformed/unsupported required data with one reason log before publishing a document/session.
Valid unknown optional syntax is preserved opaquely; unavailable assets report diagnostics and block affected play/export rather than substituting catalog values silently.
Save prepares the complete FIT/PAK/ABL/sidecar set in staging, verifies it, then publishes a recoverable transaction; failure retains the previous committed project and undo savepoint.
Commands carry owned before/after values: entity CRUD, terrain region deltas, path point edits, brain assignment/body edits, objectives, localization and campaign links.
Brush drags and multi-selection edits are single transactions; cancellation restores before-values. Undo/redo includes absent-versus-authored order ownership, fixing the class exposed by task A's orderAuthored probe.
History has a memory budget with visible oldest-command eviction and a saved revision marker. Derived jobs consume immutable revision snapshots; cancellation/stale results never overwrite newer edits.
Selection, panel layout and camera orbit are ToolState; they neither dirty the mission nor enter mission undo. Explicit authored-camera edits do both.

## Live-play loop

1. Finish the active command, validate the current document and freeze a PlaySnapshot: typed mission init, original/project content, resolved mod overlay fingerprint, compiled script identity, root seed and revision.
2. The SAME runtime loader builds an isolated GameSession/SimKernel from that snapshot. In-memory project content uses the normal resolver; Play neither quick-saves the document nor copies files into a game install.
3. The editor's SDL pump presents the same RenderWorld/Vulkan services; GameSession advances the 30 Hz kernel in `sim-kernel-design.md`. Render caches are owned separately from document and sim state.
4. Pause stops ticks while tools/presentation respond. Step is enabled while paused and executes exactly one tick. Reset destroys the session and rebuilds the SAME snapshot/seed, including initial queues/receipts.
5. Stop returns to the document at its CURRENT revision with undo intact. Runtime casualties, movement, objectives, rewards, RNG and ABL variables never write back automatically.
6. Restart with edits explicitly freezes the current revision into a new session; keep Reset separate so replay of the original snapshot remains reproducible.

| While playing or paused | Editable state | Effect |
|---|---|---|
| Document authoring | Terrain, placements/transforms, patrol points, brain assignments/source, teams, objectives, settings, campaign links; full undo/save | Changes the document and edit viewport immediately; active simulation remains tied to its snapshot. Show both revision numbers and pending-edit status. |
| Runtime control | Selection, camera, pause/step/speed and issued tactical orders | Transient session state only; commands use the same OrderEnvelope/TacticalOrder queue as game/ABL, canonical next-tick admission and receipts. |
| Presentation preview | Editor lighting/material/visibility previews and overlays | Preview state only unless committed as a document command; never affects the sim digest. |
| Import runtime result | Proposed explicit paused capture of selected surviving transforms | ID/value diff becomes one document command after review; no implicit health/AI/terrain/mission-result adoption. Missing/deleted IDs reject with a log. |

Author a new patrol while the sim runs, then Restart with edits and watch it; initial Play after authoring consumes it immediately. Structural edits never mutate a live kernel behind its queues.
A patrol cursor/FSM/HP/order debugger reads immutable runtime snapshots; the editing panel reads authored values. Separate edit/play views prevent picking a moved sim unit as a different authored entity.
Proof: document and undo hashes remain unchanged for play/pause/step/reset alone; each step has one digest; reset plus identical order replay repeats all digests.
The snapshot, seed, content fingerprint and order envelopes replay headlessly with identical authoritative digest at EVERY completed tick, including queued orders and save/resume.
The editor retains cinematic and in-mission video playback; windowed campaign-start/won-mission/in-mission `[VIDEO v1] play` proofs remain required when relevant hosts change.
The existing EditorPlaytest external save/copy/launch path is not the embedded loop; retire its install-copy and global/session control after embedded parity, retaining an optional harness-managed standalone comparison.

## Patrol paths, brain assignment, FIT and ABL

Current evidence: UnitBrainPanel authors order/stance/points with ModifyUnitOrderAction; raw ABL and declarative Brain panels are read-only.
`Unit::save` writes `[PartN] OrderType/OrderStance/WaypointCount/Waypoint{i}X/Y/Z` only when orderAuthored; `rg -n 'OrderType|OrderStance|WaypointCount' code` finds no loader for those editor keys.
The game's `Mission::loadMissionFitOpord` consumes `[WarriorN] Brain{}` OPORDs; `brain_missionfit_oporbd.h` caps routes at eight XY points and currently matches some editor imports by position.
Therefore task A's waypoint save/reload is editor persistence evidence, not proof that Play executes the saved route. Remove nearest-position matching in favor of stable entity-to-warrior identity.

Model: `PatrolPath{PathId, ordered WaypointIds/positions, once|loop, stance}`; `BrainAssignment{EntityId, mode, legacyModule?, cellInitializers, enhancedDefinition?, path/slot bindings}`.
Track imported legacy/DSL routes as provenance-linked views until explicitly edited. Reusable document paths embed their resolved points per assigned unit at export; path names/IDs can live in optional editor metadata.
UI: create/name/duplicate path, click points in the viewport, drag/insert/delete/reorder, reverse, choose once/loop and stance, assign to selected units, preview direction and reachable segments.
The brain picker filters resolved stock/mod/project modules; multi-unit assignment, pilot/team/squad editing and parameter cells are undoable. Missing module/signature errors show asset and unit IDs.
Enhanced authoring edits archetype, switches, radii, weighted tactics, three OPORD slots/successors, targets and normalized BrainSpecial bodies; diagnostics use the same schema/parser as the runtime.
Persist an enhanced assignment in `[WarriorN] Brain { compatibilityMode = "Enhanced"; ... PrimaryOPORD { type = Patrol; loop = true; Waypoint { index = 0; x = ...; y = ... } ... } }`.
Emit the authoritative normalized engine DSL with the shared brace-block writer; map IDs to WarriorN deterministically. Keep Z in the document/terrain projection; current XY schema derives elevation from gameplay terrain.
Initially validate the engine's eight-point cap and supported modes/verbs, with an explicit limit error rather than truncate; widening the runtime schema is a separate coordinated change.
Import the old Part order keys into the owned model, preserving their original bytes until edited; an explicit migrated route writes the runtime-consumed Brain form and retires its redundant editor-only keys.
Legacy assignment preserves `[WarriorN] Brain=<module stem>, NumCells, NumStaticVars` and `[WarriorNCellI] Cell/MemType/Value` with absent/default spelling retained.
Its module asset is `data/missions/warriors/<stem>.abl`, resolved through the same mod/project overlay. Assigning an existing brain changes the FIT reference/cell data, not that shared ABL file.
To author a legacy patrol, create an owned project ABL module from a validated patrol template using the engine ABL vocabulary (startPatrolPath literal points and corresponding patrol behavior), compile it with the same VM toolchain, then assign its unique module stem.
The complete template's cell signature, coordinates, once/loop and stance must be proved by runtime movement; generating an array alone is insufficient. Shared stock/mod modules are cloned before source editing.
ABL source editing owns full source text, compiler diagnostics and undo; preserve original .abl bytes on no edit, publish changed .abl with its FIT assignment, and invalidate/regenerate ABX by source fingerprint.
Never emit an ABX built from stale ABL; compile failures retain the authored draft but prevent starting/exporting that invalid snapshot. Runtime VM instances/cursors/vars are not authoring cells.
Enhanced paths serialize to FIT DSL; legacy paths serialize to owned ABL plus FIT assignment. The engine DSL is authoritative: arbitrary legacy ABL-to-DSL conversion belongs to an EXTERNAL tool, never an editor heuristic converter.
Import external normalized output as a validated undoable replacement with provenance; no automatic conversion on open/save. Derived legacy literal scans can remain display-only until replaced by parser-backed views.
Required engine prerequisites from `abl-brain-status.md`: called-body composition, persistence, conditions/selectors, slot arbitration, successor/commander fallback and accepted verb families; expose capabilities honestly until finished.
B4 cannot close with read-only panels, fake order sinks, gated-off patrol execution or unimplemented hierarchical modes. Supported behavior ships default-ON and has a live outcome test.
Acceptance: new path + brain assignment -> Play reaches points in order -> loop/once behavior -> save/reopen/Play reproduces it; reassignment and undo restore prior source, cells, mode and route ownership.

## Full terrain editor: existing surface and required completion

“Exists” below means source/UI exists, not that smoke proved the dialog. Task A reaches PaintMaterial's production action; none of the 37 MFC dialogs was instantiated by its headless probes.

| Workflow / current dialogs and tools | Existing capability | Required for full-featured closure |
|---|---|---|
| TerrainDlg, MapSizeDlg, HeightDlg, FractalDlg; MapGeneratorDialog | Terrain/dimension/height/noise selection; generation and terrain import | One creation panel; reproducible seed/presets, generation preview, supported-size validation, heightmap import/export, undoable bounded resize/crop with reference relocation checks. |
| HeightBrush, FlattenBrush, StampBrush; SingleValueDlg | Raise/lower/smooth, assign elevation, flatten, hill/crater/pad, radius/strength and stroke actions | Unified sculpt tools, falloff/strength controls, numeric precision, protected selections/masks, cancel/undo/redo for all strokes and consistent water/normals/nav updates. |
| SelectSlopeDialog, SelectTerrainTypeDlg, SelectionBrush | Slope/type and vertex/object selections | Visible slope/elevation/type masks, selection combine/invert, copy/paste terrain regions, edge-safe brush feedback and previews. |
| TerrainBrush, OverlayBrush; TilingFactorsDialog; transition menus | Material painting, roads/cement/damaged overlays, tiling, transition inspect/purge | Shared terrain/material palette with mod assets; reversible overlay/tiling/transition edits, UV/material preview, coherent Vulkan GPU updates; author supported material channels/assets. |
| WaterDlg; WaveDlg (no caller found) | Level/shallow/alpha, amplitude/frequency; wave legacy fields | One undoable water panel with shoreline/depth preview and coherent visual/nav rebuild; retain supported WaveDlg values without recreating an unused modal. |
| FogDlg, SunDlg | Fog, light/ambient/yaw/pitch, shadow/refractalize actions | Owned environment values, live preview and commit/cancel; revision-safe light/normal/tac-map regeneration, authored vs viewport camera distinction. |
| ForestDlg, EditForestDlg; ScatterBrush/FoliageRender | Forest definitions/density/height/random placement; foliage overlays | Seeded reversible scatter/erase, species palette, density/height/slope masks, load/save/rename, mod-aware dependencies and consistent export/reload. |
| MineBrush, building/link/drop-zone brushes; EditorNavLayer | Mines, objects/links/drop zones; water-derived ground/hover/air overlay | Undoable gameplay overlays, roads/bridges connectivity, collision/slope/unit-class passability from SAME gameplay navigation; path/LOS probes and patrol reachability diagnostics. |
| EditorTacMap, terrain/data save tools | Minimap/navigation, PAK quick-save; separate MOVE/tac-map export | Viewport-linked minimap; reliable full FIT/PAK/MOVE/tac-map/sidecar export, incremental invalidation and reopen/play verification; quick-save alone does not prove full export. |

Imported heightmap/texture assets remain content assets, with explicit copies/references and fingerprinted dependencies. Do not promise caves/overhangs or unsupported arbitrary mesh terrain in the heightfield mission format.
Every row needs an authored-result, undo/redo, save/reopen, displayed-result and relevant runtime outcome proof; smoke reaching a handler is not widget/render proof.

## Migration batches and byte proof

Ruling 2026-10-13 overrides old “preserve writer output” acceptance. Task A (`254751f58`) has ZERO byte-identical missions: 99 changed fields = 48 Cameras + 4 nav coordinates + 47 objective coordinates.
The pinned BEFORE bundle (`73b111f33`, `tests/smoke/editor/old-editor-2026-10-13/`) has 23 comparisons, no mc2_22, and a rejected non-stock Buildings.csv overlay.
Read-only JSON census: 934 altered keys = Cameras 46, nav 4, objective coordinates 45, Part ControlDataType 349, ObjectProfile 349, CSVFile 139, objective ModelType 2.
Its .json input/output hashes and .diff before/after values prove those failure classes existed; they do NOT establish the stock environment or a 24-mission acceptance baseline.
Task A's exact stock input hashes and dependency audit supply that baseline, including mc2_22. Preserve both records unchanged; never bless a new differing-output hash as the fix.

| Batch | Redesign / delete | Difference classes fixed; extra proof |
|---|---|---|
| B0: preservation contract, integrate E-W | Separate imported semantic values from runtime/catalog/appearance; shared lossless mission codec. Substeps below form ONE batch. | Close every known writer class; batch exits only with 24/24 byte-identical FIT AND PAK. Concurrent E-W is an input to review, not an assumed completed fix. |
| B0a | Own authored objective predicate and NavMarker coordinates; bind IDs separately from coordinate storage. | Task A's 47 objective + 4 nav keys and pinned 45 + 4: saved values equal original input lexemes, never snapped object coordinates; explicit edits change only intended fields. |
| B0b | Separate authored Cameras block from live editor eye/orbit/zoom. | Task A's 48 camera keys and pinned 46: preserve NewScale/ZoomMax and all untouched camera bytes; camera movement then save stays identical. |
| B0c | Preserve Part ControlDataType/ObjectProfile/CSVFile and objective ModelType; defaults only on explicit type change. | Pinned 349 + 349 + 139 + 2 keys: replay those exact original inputs with a fingerprinted divergent-catalog fixture; no-op output stays original. Stock rerun alone has zero such deltas and cannot prove this fix. |
| B1: document/history | Replace EditorData/EditorObjectMgr singleton ownership, pointer-copy actions, EList/EString domain values, SYSTEM/HeapList. | 24 byte pairs plus edit-undo-save, malformed-reader corpus and lifetime/ASan proof; retain old UI only as temporary command consumers. |
| B2: SDL shell and unit panels | Replace EditorMFC/MainFrame/EditorView/EditorInterface message pump; project/file/start/About, unit/building/player/chooser panels; remove HWND child input forwarding. | 24 byte pairs; selection/place/team/transform UI actions plus undo/reload and Vulkan displayed results; startup/shutdown/input ownership. |
| B3: terrain | Replace terrain/environment/forest dialogs and brush global state with document commands; delete TinyGeom/five pool owners through shared asset/render ownership migration. | 24 byte pairs; each terrain row's mutation, undo/reload/export/render/nav outcome; preserve unchanged PAK packets. |
| B4: brain/path and embedded play | Owned ABL/DSL authoring; shared kernel session and queue; replace read-only brain caches and external-only playtest. | 24 byte pairs + live patrol/assignment proofs + headless per-tick equality, pause/step/reset/save-resume; finish required engine brain gaps with its owning lane. |
| B5: objectives/localization | Replace ObjectivesDlg/ObjectiveDlg/template/flag/value/area/text/resource/view dialogs and nested borrowed collections. | 24 byte pairs; REAL panel commands/undo/save/reopen plus trigger/reward/video outcomes; upgrade task A's model-only reaches. |
| B6: campaign/settings/tools | Replace Campaign/Group/Mission/MissionSettings/UserText/file dialogs; complete asset viewer/browser/import/export and remaining tools. | 24 byte pairs; graph link authoring, campaign start/win, videos, localization, mod-resolved assets and full export; campaign smoke must become panel/action reach. |
| B7: delete residual platform debt | Delete _AFXDLL/mfcplatform.hpp/CString/DDX/message maps/resources-as-controls, remaining static owners/private allocators and EditorGameOS/EditorGosRender/gameos_editor fork. | 24 byte pairs; all-targets shared-link/backend proof, include firewall, zero remaining editor MFC/heap owners, all workflows green; portability builds when platforms are available. |

B0 substeps may have partial diagnostic improvements while under construction; none counts as an accepted migration batch until ALL 24 byte pairs pass. Every later batch must preserve that strict gate.
B2 introduces an SDL-only target over the owned model; legacy EditRel remains an isolated reference executable until B7 removes its sources/target. Unmigrated dialogs are not embedded in the new application's pump.
Partial workflow availability is migration staging, not a release exception: every inventory workflow is complete before the single final release, and reference/new launches remain serial under the host lease.
The pinned bundle is the per-class negative control: its old output remains different from its original input, while the new writer must equal that input in a fingerprinted fixture reproducing the failure class.
The older bundle lacks task A's complete dependency manifest: verify its input hashes against archived originals and reconstruct/hash its divergent catalog, or pin an equivalent regression fixture reproducing its catalog changes without claiming an exact historical runtime reconstruction.
Do not label that catalog control “stock.” No mc2_22 history is invented; task A supplies the missing stock comparison.
Archive source bytes/dependencies stay fixed. No masks, decimal tolerances, semantic-only passes, packet-decoding-only comparisons or “FINDING” rows may satisfy the new acceptance.
Keep historical `roundtrip-baseline.json` unchanged; add a strict input-equals-output gate with all 24 required fresh comparisons. Transition acceptance must deliberately fail the old differing-output floor, not casually --rebaseline it away.
The existing runner grades quick-save FIT and requires a PAK file, but does not compare PAK bytes. Extend its existing stock row to capture/compare PAK bytes too, full export separately, missing/short/blocked cases fail.
Retain the original 15 rows and task A workflow facts; each replacement panel adds production-command + undo/redo + save/reopen reach and a separate rendered/widget check. No legacy dialog reach is implied.
Future implementation command (NOT run for E1; after strict gate extension, use a committed private-runtime executable and normal host lease):

```text
py -3 scripts/run_editor_smoke.py --exe "scratch/editor-e0-install/Mission Editor.exe" --prepare-stock-scratch --case stock_roundtrip --case workflows --keep-logs --exit-sec 3 --timeout 180
```

Each implementation batch also requires offline contracts/doctest first, RelWithDebInfo build/all-targets, relevant ASan/runtime/digest and Vulkan visual proof. Orchestrator runs Bash board and commits; never push.
Task E1 only writes this file; no build/editor/game launch, index mutation or edits to E-W/other lane paths. No runtime fixes or byte-equality successes are claimed here.

## Risks and owner questions

- Lossless syntax plus typed semantics can diverge: a single codec, field-presence tracking and edit-undo byte controls prevent hidden normalization; never save derived caches as authored fields.
- Unknown mod blocks, duplicate FIT keys, catalog overlays and shared ABL assets need explicit resolution/validation; copying only mission files misses dependencies and can damage mods.
- Stable IDs must survive spawn-position edits, unit insertion and export index allocation; position-based brain/objective matching produces wrong assignments and is removed.
- Brain completion and sim-kernel extraction are prerequisites, not editor-local stubs. Their lane coordination and current eight-point cap constrain B4; no hierarchy-completion claim based on the fake sink.
- Terrain changes invalidate gameplay and render data differently; revision-stamped work and deterministic cooks prevent stale nav/GPU results and preserve unaffected packets.
- Embedded game/editor owners must coexist without shared mutable globals; ASan, repeated play-reset-open-close and per-tick digests expose leaks, aliasing and RNG/queue contamination.
- FIT/PAK/ABL multi-file publication needs crash recovery; injected write/rename/compile failures must leave a coherent last committed project.
- MFC deletion can lose workflow/input/accessibility coverage. Map all inventory entries, including uncalled classes' data capabilities, and prove displayed tools; Linux/macOS remain future proof, not inferred from SDL.

1. Should Hybrid ship with ABL GENERAL plus enhanced PLAYER/ALARM slots, or should authored missions choose Legacy/Enhanced only? Default proposal: preserve valid existing Hybrid content, finish explicit arbitration before exposing Hybrid authoring; this changes the B4 runtime/assignment contract.
