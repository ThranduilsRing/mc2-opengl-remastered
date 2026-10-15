# Owned mission document

`editor_document` owns FIT/PAK values without MFC, GameOS or legacy heap owners. SDL owns MissionDocument before CommandHistory so history cannot outlive the document; views own transient drafts. EditRel remains isolated.

## Formats, identity and history

Document-local EntityIds/NodeIds are not Part indices, packet indices or appearance pointers. They survive structural edits and undo/redo, are never recycled across history branches, and are reimported on reopen. Links use IDs; catalog lookup never replaces imported values.

Original FIT bytes own spans, including spelling, comments, whitespace, order, unknown fields and CRLF. Typed values retain absence, array cardinality and imported identity; unknown valid syntax stays opaque in a bounded tree. Value edits replace only their span; returning to the loaded value restores its spelling.

PAK packets retain decoded bytes and original encoded spans. Raw, file-within-file, zlib and classic LZW are supported; unsupported storage rejects. No-op/metadata saves retain the archive byte-exactly. Placements rewrite packet 1 deterministically as raw storage with updated offsets/checksum; other payloads stay exact, and undo restores original compression.

Candidates validate literals, types, finite floats, array/nesting limits, packet bounds/lengths/hashes, aggregate decoding, terrain dimensions, Parts and objectives before publication. Required metadata includes scenarioTuneNum, Blurb2, ScenarioScript, MissionName or its selected resource ID, and objective version/counts. Boolean payloads use readable FlagID (even empty), otherwise signed 32-bit FlagIndex; absent Value defaults true. Unreadable aliases remain byte-exact with the reader interpretation shown. Failure logs one escaped diagnostic; limits reject without truncation. Rejection preserves bytes, revision, dirty state, history and future IDs; no-ops retain redo.

Bounded history accounts for syntax, typed values and container capacities, supports interleaved text/structural commands and reports eviction. Structural state excludes unchanged terrain/packets. Savepoints compare values, so undo across a save stays dirty until saved values return. Only applied edits are dirty; drafts are not history. Mission names use growing buffers with a 1 MiB document limit and preserve localization selection.

## Entities and brains

Dense Part indices compact on deletion; surviving IDs, explicit Pilot assignments and authored Warrior/cell blocks remain stable. Alternative/group references remap atomically; deleted group members become zero without changing slots, and empty groups are valid. Malformed/out-of-range group references reject. Referenced alternatives/parents require a future graph-delete command. New Parts/Warriors append deterministically; creation permits 119 usable Warriors and supplies required fields, including SquadNum, 15 alternative indices and MyIcon 0.

TeamID is [-1,7]; -1 means no team. Placements share those bounds. Optional PositionZ/Rotation default to zero and must be finite when present. Pilot is [0,65535]; nonzero values must name a Warrior, while zero/absence is unresolved. Explicit Pilot alone selects a Warrior; position and Part number never select a brain.

Warrior scalar Brain/module, NumCells and legacy cells stay separate from its direct declarative Brain child. Lookups stay scoped; duplicate direct Brain children reject, repeated route/extension children remain valid. Legacy/Hybrid/Enhanced edits preserve unknown switches, comments, OPORDs and cells; compatibilityMode needs its own body line because the game skips Brain's opening line. Enhanced summaries expose three read-only OPORD slots, types and waypoint counts; runtime Hybrid arbitration is pending.

## File lifecycle

Save stages/rereads both files, then publishes through a transaction journal with retained previous versions. Failed publication rolls back without advancing the savepoint; pending/invalid journals block mixed-pair loads and require recovery from retained files. Committed journals/previous versions remain. No ABL or asset content is rewritten.

FileWorkflow owns paths and pending Open/Reopen/Close with Save/Discard/Cancel. Failed loads keep the active document; replacement resets document/history/panel state. Accepted close suppresses further edits immediately and terminates the production loop independently of proof completion. Failed close-time saves leave the dirty prompt pending. SDL owns its window, contexts and input without HWND forwarding.

Recents use `editor-recent.txt` unless `--recent-file` selects another store: UTF-8 absolute generic `.fit` paths, validated before normalization, at most 12 entries after successful opens/saves. Relative paths, controls, wrong extensions and oversized lists reject once. Paths/text never shorten on OS failure. Recent-store replacement precedes its in-memory commit; failure preserves both lists and reports `recent_updated=0 reason=recent_write_failed`. Document save success, active path/savepoint and undo/redo are independent of recent-write failure; without a store, recents are memory-only.

## Catalog and placement

FileResolver folds case/slashes, rejects traversal, excludes dot directories and replaces whole files by the first match:

| Priority | Source |
|---|---|
| 1 | Active mod's data |
| 2 | Its mod.json dependencies in declared order |
| 3 | Launcher dependencies |
| 4 | Base loose files |
| 5 | Base system.cfg FastFiles order |

Archive handles/dictionaries and resource indexes are instance-owned; FST bounds/hashes, compression and lengths validate. Invalid catalog rows log once and do not hide later valid rows; resource errors log separately. Validation covers consumed facts, not unrelated combat tuning.

Buildings.csv uses 12 columns; FitID selects object2.pak, whose ObjectTypeNum defines runtime family independently of CSV appearance type. Multiple CSV identities may share a FitID; stems must be unique. Specialized bridge/light/marker and authored enumeration facts survive. Variant names use `<stem>.csv` row `23 + 97*i`, column 2, indices 0..15: absence is optional, malformed present files reject the entry. Resource names use owned en_us FIT indexes/LegacyTextAlias mappings, decoded escapes and stem/name-ID fallbacks; external definitions/locales remain pending.

PlacementPolicy owns ordered player/enemy pilot tables and provenance; raw/bounded-zlib tables are valid, missing pilot FITs are skipped, malformed present inputs reject once. Profile identity selects pilots despite descriptions containing multiple resource IDs. Player identity determines CommanderID, PlayerPart, pilot/brain/profile; explicit team must equal PlayerN.DefaultTeam. Player 0 retains player defaults on another team; other players retain enemy defaults on team 0. Enemy cells are (2,1,150), (9,0,0), (28,0,0).

New units include damage, colors, variant, repair, signed brain counts/cells and a squad above the largest authored squad. Empty alternative cursors retain legacy export ordering. Alignable packet objects use player alignment; others use -1. Forest instances encode instance+1, never catalog forestGroup; ordinary objects use scale 0 regardless of catalog scale. Shell-owned resolver/catalog/policy supply placements; drafts remain view-owned. Chooser filters family and case-insensitive name/stem and exposes definition/variant provenance.

## Markers and chooser

Catalog-classified marker packets and NavMarker FIT counts/blocks must agree exactly; ordinary buildings/DropZones cannot own marker FIT records. XY ownership must be unique: unmatched/ambiguous matches and moves onto another marker reject. Moves update packet transforms and FIT XY atomically, retaining radius; delete removes both records, compacts counts/headers and preserves surviving IDs/unknown fields. Classification survives candidates, save verification and reload; undo/redo restore both streams exactly.

## Acceptance and open work

Shipping EditorShell excludes fixture replacement, synthetic input, tracking and capture; EditorShellSmoke/native widget tests alone own them. Independent grades require changed content, exact undo/redo, save/reopen and specific corruption controls; matching history bytes alone prove nothing. Frozen placement manifests are immutable: compare complete typed unit/cell/marker data and decoded placements, not archive encoding. Legacy saves may change only their final raw save-GUID packet where allowed; objective reload uses its own same-run null control. Fresh outputs and clean executable/input fingerprints are mandatory. Desktop-free ImGui events do not establish rendered output or runtime outcomes.

Objectives: [scalars](OBJECTIVES-E6a.md), [CRUD](OBJECTIVES-E6b.md), [deferred references](OBJECTIVES-E6b-refs.md). Pending: true New-mission/terrain factory with fresh history/IDs/drafts, remaining entity/brain authoring, shared runtime/cook codec, project dependencies/Start/About, map/terrain/embedded-play workflows, Vulkan rendering and legacy pump removal. Entity selection alone does not replace objective-reference choosers.
