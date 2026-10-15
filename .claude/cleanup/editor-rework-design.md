# E1: a mission is live-playable and editable inside the editor

One SDL+ImGui application owns document, undo, tools and an isolated game session. The viewport shares Vulkan through EditorBridge; MFC and its message loop disappear. All terrain, objective, team, campaign, asset and export workflows must ship before the single final release, including cinematics and in-mission videos. C++23 values replace legacy containers, private heaps/pools and static owners.

## Document, identity and undo

EditorApplication owns platform/content/catalog services, EditorDocument, CommandHistory, ToolState, viewport and optional GameSession. The document owns terrain, entities, teams/players, objectives/actions/conditions, settings, authored camera/environment, brain sources/assignments and campaign graph.
Stable entity IDs survive insertion, position edits and export; preserve existing Part/Warrior indices and allocate new ones deterministically. References use IDs, never appearance/dialog pointers or nearest-position matching. Imported coordinates, Part control/profile/CSV identity and objective ModelType remain independent of catalog lookup/snapping; defaults apply only on explicit creation/type change.
One shared codec serves editor/runtime/cooks. Its lossless syntax owns original FIT bytes, ordering, prefixes, numeric lexemes, comments, whitespace, unknown valid optional syntax and absence; typed nodes reference syntax spans. Unchanged spans emit original bytes; edited nodes format deterministically. Unsupported required data rejects once before publication.
PAK retains original packet/compression bytes and dependency manifests; only edited packets recook. No-op and edit-undo saves reproduce FIT/PAK/ABL bytes, including optional extensions; semantic equality clears dirtiness against loaded values.
Readers bound sizes, dimensions, counts, arithmetic, finite coordinates, references and DSL depth/cycles. Missing assets report IDs and block affected play/export rather than substituting defaults. Publish validated FIT/PAK/ABL/sidecars as a recoverable transaction; failure retains the prior project and savepoint.
Commands own before/after values for entity CRUD, terrain deltas, paths, brains, objectives, localization and campaign links, including absent-versus-authored order ownership. Drags/multiselection coalesce; cancel restores values. History has a visible eviction budget and saved revision marker.
Appearance/nav/light/minimap/thumbnails/compiled scripts are revision-keyed derivatives. Jobs consume immutable snapshots; cancelled/stale results cannot publish. Selection/layout/orbit are ToolState, outside mission dirtiness/undo; explicit authored-camera edits enter both.

## Live-play loop

Play freezes a validated revision, resolved project/mod content fingerprint, compiled script identity and seed; the same loader builds an isolated GameSession/30 Hz kernel. Project content uses the normal resolver, without quick-saving or copying into an install.
Pause leaves tools responsive; paused Step executes one tick; Reset rebuilds the same snapshot/seed/queues. Stop preserves the current document/undo; Restart with edits takes a new snapshot. Display document/runtime revisions and pending edits, with separate authored picking and immutable runtime debugging.
Authoring remains editable/undoable during play but never mutates its kernel. Tactical orders use shared next-tick envelopes/receipts; camera/selection/speed are runtime controls. Lighting/material/visibility previews stay presentation-only unless committed.
Optional paused import of surviving runtime transforms requires an explicit reviewed ID/value diff as one command; missing IDs reject. Never implicitly adopt casualties, health, AI, terrain, rewards, RNG, ABL variables or mission results.
Play/pause/step/reset leave document/undo unchanged; step emits one digest and identical snapshot/seed/order replay matches every completed tick headlessly, including save/resume. Render caches have separate owners. Retire the external install-copy playtest path after embedded parity; an optional leased standalone comparison may remain.

## Patrol paths, brain assignment, FIT and ABL

Document PatrolPath owns stable point IDs/positions, once/loop and stance; BrainAssignment owns entity, mode, legacy module/cells or Enhanced definition and path/slot bindings. Imported routes retain provenance until edited; reusable paths export resolved points per unit, with names/IDs optional metadata.
Path tools create/name/duplicate, click/drag/insert/delete/reorder/reverse points, choose once/loop/stance, assign multiple units and preview direction/reachability. Brain tools resolve stock/mod/project modules, edit pilot/team/squad/cells and diagnose unit/module/signature errors with undo.
Authored routes retain Part-key format: [PartN] OrderType/OrderStance/WaypointCount/Waypoint{i}X/Y/Z, entering the TacticalOrder queue while preserving the assigned legacy brain and untouched bytes. An Enhanced OPORD wins only when the shared runtime eligibility predicate admits it; suppress the Part route atomically with one info line, never dual-dispatch.
Enhanced authoring covers archetype, switches, radii, weighted tactics, three OPORD slots/successors, targets and normalized BrainSpecial bodies. Export normalized [WarriorN] Brain { compatibilityMode="Enhanced"; PrimaryOPORD {...} } with deterministic IDs and the runtime parser/schema; retain document Z while the XY runtime schema derives elevation. Enforce the eight-point cap and supported verbs/modes explicitly, never truncate; widening requires coordinated readers/export.
Legacy assignment preserves Brain=<module stem>, NumCells/NumStaticVars and WarriorNCellI Cell/MemType/Value, including absence/default spelling. Modules resolve under data/missions/warriors/<stem>.abl through the project/mod overlay; assignment changes references/cells, not shared source.
Explicit legacy behavior clones an owned project module, compiles with the VM toolchain and assigns a unique stem. Validated patrol templates must preserve cell signature, coordinates, once/loop and stance through real movement. Source editing owns text/diagnostics/undo; unchanged ABL bytes survive and changed sources publish with FIT, regenerating ABX by source fingerprint. Stale ABX never exports; compile failure retains draft but blocks affected play/export. Runtime VM state is not authoring cells.
Arbitrary legacy-to-Enhanced conversion remains external: import normalized output as a validated undoable replacement with provenance, never convert on open/save. Literal scans are display-only until parser-backed. Required engine capabilities remain called-body composition, persistence, conditions/selectors, slot arbitration, successor/commander fallback and accepted verbs; read-only/fake sinks or gated-off execution cannot close this workflow.
Acceptance requires actual point order, once/loop behavior and save/reopen/replay; reassignment/undo restore source, cells, mode and route ownership.

## Full terrain editor: existing surface and required completion

All workflows require document commands, cancellation/undo/redo, save/reopen, displayed results and relevant runtime outcomes; handler reach alone is insufficient.

| Workflow | Required contract |
|---|---|
| Creation/generation | Terrain/size/height/noise presets, reproducible seed, preview, supported dimensions, heightmap import/export, bounded undoable resize/crop with reference relocation validation. |
| Sculpt | Raise/lower/smooth/elevation/flatten/hill/crater/pad; radius/strength/falloff, precision, protected masks, edge-safe feedback and coherent water/normals/nav rebuild. |
| Selection | Slope/elevation/type and vertex/object masks, combine/invert, terrain-region copy/paste and previews. |
| Materials | Mod-aware terrain/material palette; painting, roads/cement/damage overlays, tiling, transition inspect/purge, supported channels/assets, reversible UV preview and coherent Vulkan updates. |
| Water | Level/shallow/alpha/amplitude/frequency, shoreline/depth preview and visual/nav rebuild; preserve supported wave values without recreating an unused modal. |
| Environment | Owned fog/light/ambient/yaw/pitch and shadow values, preview/commit/cancel, revision-safe light/normal/tac-map regeneration, separate authored/view cameras. |
| Forests | Seeded scatter/erase, species/density/height/slope masks, definition load/save/rename, mod dependencies, foliage display and consistent export. |
| Gameplay/navigation | Mines, objects/links/drop zones, roads/bridges, collision/slope/unit-class passability from gameplay navigation, ground/hover/air overlays, path/LOS/patrol diagnostics. |
| Maps/export | Linked minimap and full FIT/PAK/MOVE/tac-map/sidecar export with incremental invalidation/reopen/play; quick-save does not establish full export. |

Imported heightmap/textures keep explicit content references/copies and fingerprints. The heightfield format does not support caves/overhangs/arbitrary mesh terrain.

## Migration batches and byte proof

B0 separates imported objective/nav/camera/Part identity from runtime/catalog-derived values and establishes lossless output. Acceptance requires fresh, byte-identical FIT and PAK roundtrips for all 24 stock missions, including mc2_22; all later batches preserve it. Retain historical input/dependency archives and roundtrip-baseline.json unchanged; never bless differing output by rebaselining.
Catalog-divergence regressions need fingerprinted fixtures that actually reproduce control/profile/CSV/ModelType changes; stock alone cannot prove them. Historical partial/non-stock bundles are failure controls, not the complete stock baseline; reconstruct their dependencies or pin equivalent controls without inventing provenance. No masks, float tolerances, semantic-only/decoded-packet passes or missing/short/blocked cases satisfy byte equality. Full export is graded separately from quick-save.

| Batch | Ownership/workflow outcome |
|---|---|
| B1 | Owned document/history replaces singleton data/object managers, pointer-copy actions, EList/EString and SYSTEM/HeapList; legacy UI temporarily consumes commands. |
| B2 | SDL shell/project/file/start/About/unit/building/player/chooser panels replace the MFC pump and HWND forwarding; EditRel stays an isolated reference until deletion. |
| B3 | Terrain/environment/forest commands replace dialog/brush globals and TinyGeom/TG pools through shared asset/render ownership. |
| B4 | Owned brain/path authoring and embedded shared-kernel play replace read-only caches and external-only playtest; engine capability dependencies must be completed by their owners. |
| B5 | Objectives/localization and nested conditions/actions/templates/flags/values/areas/text/resources/views become owned panels, with trigger/reward/video outcomes. |
| B6 | Campaign/group/mission/settings/user-text/file tools, asset viewer/browser/import/export and remaining inventory workflows ship, including graph/start/win/localization/mod assets. |
| B7 | Delete residual MFC/CString/DDX/message maps/resources-as-controls, private allocators/static owners and EditorGameOS/EditorGosRender/gameos_editor duplication; use shared platform/backend services. |

Retain existing workflow coverage and add real panel commands, undo/save/reopen and separate widget/render checks. Include input/accessibility and uncalled classes' data capabilities; model-only reach does not prove a panel. Every batch needs offline/RelWithDebInfo, relevant ASan/runtime/digest/Vulkan checks; final shared-link/firewall and portability proof cannot be inferred from SDL. Reference/new launches remain serial under the lease.

## Risks and owner questions

Lossless syntax/typed semantics, unknown mod blocks/duplicates/catalog overlays, shared ABL dependencies, stable export IDs, stale terrain/nav/GPU jobs, coexisting editor/game owners and crash-safe multifile publication remain risks; preserve field presence, revision checks, full content manifests and explicit ownership. Brain/kernel work is a shared prerequisite, never an editor stub.
Open: expose Hybrid with ABL GENERAL and Enhanced PLAYER/ALARM, or Legacy/Enhanced only? Preserve valid imported Hybrid content by default, but finish explicit arbitration before exposing Hybrid authoring.
