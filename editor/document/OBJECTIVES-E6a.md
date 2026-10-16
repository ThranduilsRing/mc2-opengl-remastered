# Objective scalars

The game reader (`code/objective.cpp`, `objective_text.cpp`, `objective_flags.cpp`, `team.cpp`) defines types, ranges, aliases and team initialization. Legacy `editor/Objective.cpp` supplies authoring defaults and model metadata, not the runtime schema.

## Inventory

`editor/tests/objective-census.json` pins stock input hashes, fields, species and payload keys. Stock actions have no entity references. The legacy factory's three EnemyUnitGroup predicates are unimplemented; unknown species remain opaque.

## Scalar authoring schema

The document owns typed values, optional source NodeIds and absent-field defaults; views own drafts. Objective/entry IDs survive edits and undo/redo. Catalog lookup never replaces authored values.

| Fields | FIT type; default |
|---|---|
| Title | `st`; empty |
| Description | `st`; Title |
| Priority | `ul`; 1 |
| ResourcePoints | `ul`; 0 |
| PreviousPrimaryObjectiveMustBeComplete, AllPreviousPrimaryObjectivesMustBeComplete, DisplayMarker, HiddenTrigger, ActivateOnFlag, ResetStatusOnFlag, TitleUseResourceString, DescriptionUseResourceString | `b`; false |
| MarkerX, MarkerY | `f`; 0 |
| ActivateFlagID | `st`; empty or decimal ActivateFlagIndex |
| ResetStatusFlagID | `st`; empty, no index alias |
| TitleResourceStringID, DescriptionResourceStringID | `ul`; 0 |
| ObjectiveModel | `st`; empty |
| ModelType | `l`; 0 |
| BaseColor, HighlightColor, HighlightColor2 | `l`; -1 |
| ModelScale | `f`; 1 |
| ModelID | `l`; -1, editor metadata |
| ModelID Version | `ul`; 0, editor metadata; legacy versions <=1 ignore ModelID |

Integers use the game's signed 32-bit range, including negative ResourcePoints: successful objectives add that signed amount to C-Bills. Floats must be finite and float-representable; zero/negative scales are valid. Nonempty models require camera type 5, 13, 14 or 15. Integer conversion overflow stays byte-exact and read-only with its reason.

Game-ignored fields retain bytes and an "ignored by the game" warning. The panel shows the game's fallback; "author this field as the game reads it" writes the type/value only on explicit Apply, including an explicitly chosen default. Unsafe DisplayMarker prefixes, required-count failures and malformed required payloads reject once. Specific-object conditions require authored XY.

Text is limited to 2,000 characters after FIT assembly, before LF-to-CRLF conversion, without truncation. Drafts use LF and normalize CRLF; literal backslash-n stays literal. New values reject quotes, NUL and lone CR. FIT joins the first two physical string lines: authored multiline text needs an empty opening line and CRLF for lossless disk/RAM reads, including empty/trailing lines. Untouched text keeps its spelling.

PreviousObjectiveMustBeComplete supplies the previous-primary flag when the canonical key is absent/ignored; edits retain the alias. ActivateFlagIndex supplies a missing/ignored ActivateFlagID; ID edits add the canonical string without changing the index. Resource/flag IDs never rewrite string tables or ABL; model fields never regenerate one another.

Saved state (IsActive, ActivationTime, IsResolved, ResolvedStatus, ChangedStatus) stays opaque. TeamNBooleanFlags is byte-exact and read-only: show reader initialization, skipped oversized IDs, ignored tails and first case-insensitive duplicate precedence. Version 2 uses ObjectiveN roots/success conditions and Team0ObjectiveN for other lists; version 3 uses TeamN blocks for eight teams.

## Preservation, unsupported entries and references

Species names match exactly; unsupported strings/numbers and unimplemented group predicates use monostate, never the game's unknown-string executable fallback. Valid unknown keys/payloads stay byte-exact. Scalar edits change only the chosen value/declaration and explicitly authored prefix; unrelated syntax and PAK bytes stay exact.

Candidates validate before mutation. Rejections log once and preserve bytes, revision, dirty state, history and future IDs; limits reject without truncation. No-ops retain redo. Bounded history restores exact original/changed bytes through undo/redo and save/reopen. [CRUD](OBJECTIVES-E6b.md) permits necessary header/count changes; completed [typed references](OBJECTIVES-E6b-refs.md) follow resolved entity targets atomically.

## Acceptance

Independent typed decoding and expected changed outputs are required; passthrough alone proves neither semantics nor target resolution. Keep stock-pair, frozen-placement, real-reader, normal/ASan and widget gates. Reloads require fresh outputs and proven inputs; runtime trigger/reward/video outcomes need separate instrumentation.

## Outcome observation contract (v1; hooks and runtime proof pending)

- Extracted trace: one `OBJECTIVES/1 ` + UTF-8 JSON object per LF line; JSON escaping preserves quotes, backslashes and newlines. Maximum 16,384 bytes excluding LF; reject malformed/oversized records once, without truncation or continuing to a pass.
- Exact record keys: `mission,seq,id,frame,time,kind,cause,data`. `seq` starts at 1 without gaps; `id` is a run-unique occurrence ID (1–96 ASCII letters/digits or `_.:-`), never reused on reset. `frame` is a nonnegative simulation frame; `time` is finite absolute mission seconds. Both are nondecreasing in a single run without rewind.
- `mission` is lowercase SHA-256 of ASCII `MC2-OBJECTIVES/1` plus one NUL byte, then unsigned 64-bit little-endian FIT byte length, exact FIT bytes, PAK byte length and exact PAK bytes. Retain both inputs and their individual hashes; record other mounted inputs, seed and timestep in the proof manifest.
- Kinds: `resolve,reset,activate,dispatch,sink,complete`. State `data` records team/original FIT objective ordinal, old/new state and traversal identity when relevant; dispatch `data` has exactly `team,objective,list,entry,species,payload,result`, with original zero-based FIT ordinals, `success`/`failure` list and exact decoded payload. Never compact identities past skipped entries.
- `cause` is null or an earlier occurrence ID. Dispatch links its resolution occurrence; sink evidence links its dispatch occurrence and names the sink/stage/result in `data`. One dispatch per resolution/list/entry; reset → new resolution → new dispatch permits legitimate replay even within one frame. Dispatch return alone proves no sink outcome.
- Final `complete` has `data:{"count":N,"result":...}` counting all preceding records and stating the terminal result or fixed horizon. Missing completion, unknown/extra events, duplicate IDs/keys, nonfinite values and trailing records fail.
- `tools/objective_outcomes.py TRACE EXPECTED` consumes an extracted trace and independent JSON oracle: exact keys `version:1,mission,events,before,timing`. Nonempty `events` lists exact `{id,kind,cause,data}` records, including completion; array order is irrelevant. All events must match exactly once; `before` lists strict causal ID pairs and `timing` maps IDs to inclusive absolute-second `[min,max]` assumptions (explicit empty containers allowed).
- Oracles encode setter → activation → resolution in a later traversal, previous-primary prerequisites, and primary-failure traversal stop; they must not invent frame schedules. Time predicates compare absolute mission time, not elapsed activation time. Exact state/traversal contents, causal edges and declared bounds express the fixture's assumptions; the comparator does not emulate objective evaluation.
- Authoring prerequisite: E6c-3 must add owned payload commands and a C++ fixture-authoring helper in `editor_document_tests`; it is not a Python binding. No generated FIT or generic `replace()` bypass. The full mission family and media remain deferred.
- Deferred proof commands (PowerShell, configured VS tree): `$env:_MSPDBSRV_ENDPOINT_="e6e-$PID"; cmake --build build64 --config RelWithDebInfo --target editor_document_tests`; `& ./build64/editor/RelWithDebInfo/editor_document_tests.exe --test-case="objective outcome fixture authoring"`. That named helper/test is required new work, not an existing capability; it must use commands to save/reopen and assert independently specified intended contents plus retained FIT/PAK hashes, not equality alone.
- Synthetic comparator tests establish only comparator behavior. C-Bill settlement, playback and technology availability verdicts remain pending runtime hooks and the post-mission logistics harness; B5 stays open.

## Follow-ups

The [outcome package](../../tests/smoke/objectives_e6e/manifest.txt) retains two command-authored missions, media recipes/hashes and independent family expectations (`objective_outcomes.py TRACE expected.json --mission e6e_win|e6e_loss`). Its native save/reopen proof runs in the stock-fixture suite; synthetic negatives cover predicate/activation/setter/media omissions. Family v2 binds occurrence IDs, integer traversal/balance ranges and mounted path suffixes before applying the v1 comparator, causal/time bounds and signed settlement deltas. Runtime acceptance still requires lane3 logistics progression and the missing `movie/decoded_frame/accepted` receipt; no dispatch-only pass is provided.

E6c closed: four-list payload authoring and species-selectable creation, read-only team-flag initialization/resolution, per-entry unknown-species quarantine and skipped-group display. Team-flag authoring and `_RemoveStructure` authoring remain deferred; bytes stay exact. Templates/localization remain E6d; actual trigger/reward/video outcomes remain E6e.
Unknown condition/action strings quarantine that entry with the actual DestroyAllEnemyUnits/PlayBIK fallback shown.
Group predicates remain preserved and shown as skipped; numeric species fallback applies only to success conditions.
Quarantined/skipped entries allow explicit deletion and whole-objective CRUD, but no individual edit/reorder; refuse tail awakening.

| Work | Contract |
|---|---|
| E6b | [Objective CRUD](OBJECTIVES-E6b.md) |
| E6b-refs | [Typed references, identity and atomic entity edits](OBJECTIVES-E6b-refs.md) |
| E6c | Success/failure payload editors, team flags, rare types and group-condition policy |
| E6d | Templates, localization/string tables, model catalog selection and remaining text/resource dialogs |
| E6e | Game-side trigger/reward/video instrumentation and actual outcomes, including synthetic PlayBIK; required before B5 closes |
