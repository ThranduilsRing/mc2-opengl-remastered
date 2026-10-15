# Objective CRUD

Add, copy, delete and reorder each publish one validated structural command through bounded history. The SDL panel exposes all operations, including Add for empty teams. [Scalar rules](OBJECTIVES-E6a.md) apply; [references](OBJECTIVES-E6b-refs.md), payload editors, localization and runtime outcomes remain separate work.

| Operation | Contract |
|---|---|
| Add | Append to the chosen team with legacy constructor defaults, empty model/type 0/scale 1, ModelID -1, ModelID Version 2 and four empty lists. |
| Copy | Insert after the source in its team; retain lists, comments, extensions, unknown payloads, runtime state and target lexemes. |
| Delete | Remove the objective's blocks and compact only its team's indices. |
| Reorder | Use the final index within the same team; preserve prerequisite flags, so positional prerequisite meaning changes. The panel warns beside the controls. |

## Preservation and transactions

Version 2 supports team 0; version 3 supports eight teams. Exported indices define logical order independently of physical block order. Root blocks, suffixed payloads/extensions and nested syntax travel together; numeric boundaries distinguish objective 1 from objective 10.

Unedited bytes stay exact except necessary objective-count values and renamed headers. Copied/reordered list counts and bodies stay exact; new lists are empty. Unknown internal keys/numbers are never renumbered. Unrelated teams and the PAK stay exact. Header collisions reject instead of overwriting orphans.

Candidates validate before publication. Surviving objective/entry/field/entity IDs retain identity; copies get fresh syntax IDs. History owns reversible syntax/objective/entity state, without another undo stack or static owner. Rejection logs once and preserves bytes, revision, dirty state, history and future IDs. Limits reject without truncation; no-op reorder retains redo. Undo/redo restore exact bytes; save/reopen uses paired-file transactions.

## Validation

Independent grades require expected counts, order, defaults and surviving bodies across versions, multiple/empty teams, all lists and interleaved entity history. Matching edited/redo artifacts alone prove nothing: no-op, wrong, corrupt, missing and stale artifacts must fail. Keep stock-pair, frozen-placement, typed-model, real-reader, normal/ASan and actual ImGui-event gates.

Reload binds fixture FIT/PAK to the panel's input hashes/copies and validates the panel gate and expected edit. Every re-saved packet must decode; edited placements must equal a same-run, same-executable null save retaining the unedited fixture's objectives. This permits legacy position quantization, repacking, omitted derived movement/tactical packets, record reordering and a fresh GUID, but no CRUD-induced placement changes. Frozen placements remain an independent gate.

## Acceptance

Acceptance requires clean executable fingerprints and fresh scalar, CRUD and null-reload outputs. Panel/reload evidence does not establish runtime trigger/reward/video outcomes. Rendered evidence requires the harness host lease.
