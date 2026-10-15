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

Saved state (IsActive, ActivationTime, IsResolved, ResolvedStatus, ChangedStatus) and TeamNBooleanFlags stay opaque. The game reads BooleanStringN/BooleanValueN until the first missing ID. Version, objective counts and four list counts are structural: version 2 uses unprefixed team-0 blocks; version 3 uses TeamN blocks for eight teams.

## Preservation, unsupported entries and references

Species names match exactly; unsupported strings/numbers and unimplemented group predicates use monostate, never the game's unknown-string executable fallback. Valid unknown keys/payloads stay byte-exact. Scalar edits change only the chosen value/declaration and explicitly authored prefix; unrelated syntax and PAK bytes stay exact.

Candidates validate before mutation. Rejections log once and preserve bytes, revision, dirty state, history and future IDs; limits reject without truncation. No-ops retain redo. Bounded history restores exact original/changed bytes through undo/redo and save/reopen. [CRUD](OBJECTIVES-E6b.md) permits necessary header/count changes; [typed references](OBJECTIVES-E6b-refs.md) remain deferred, so entity commands do not follow objective targets.

## Acceptance

Independent typed decoding and expected changed outputs are required; passthrough alone proves neither semantics nor target resolution. Keep stock-pair, frozen-placement, real-reader, normal/ASan and widget gates. Reloads require fresh outputs and proven inputs; runtime trigger/reward/video outcomes need separate instrumentation.

## Follow-ups

| Work | Contract |
|---|---|
| E6b | [Objective CRUD](OBJECTIVES-E6b.md) |
| E6b-refs | [Typed references, identity and atomic entity edits](OBJECTIVES-E6b-refs.md) |
| E6c | Success/failure payload editors, team flags, rare types and group-condition policy |
| E6d | Templates, localization/string tables, model catalog selection and remaining text/resource dialogs |
| E6e | Game-side trigger/reward/video instrumentation and actual outcomes, including synthetic PlayBIK; required before B5 closes |
