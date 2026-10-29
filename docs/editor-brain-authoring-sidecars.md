# Mission editor brain authoring sidecars

The editor owns the existing `<mission>_ai.fit` and
`<mission>_specials.fit` source files beside the mission FIT/PAK pair.
These are authoring sources, separate from the BRN1 save-game runtime
section. `code/save_brain_section.cpp` and its wire format are unchanged.

## Existing runtime formats

`_ai.fit` uses the existing FIT reader grammar, for example:

```text
FITini
[Brain]
st unitRef = "Warrior4"
l mode = 2
```

`unitRef` names the mission's one-based Warrior index, not a Part or an
editor entity ID. `mode` is 0 Legacy, 1 Hybrid, 2 Enhanced. The current
runtime reads a single `[Brain]` block; the editor does not invent numbered
blocks or claim support for multiple runtime assignments. Existing
`[Tactics]` weight entries may follow, as read by
`Mission::loadTacticWeights` in `code/mission_init_load.cpp`.

`_specials.fit` uses the existing TechScript source grammar, for example:

```text
TechSpecial {
    key = "example"
    type = "MissionSpecial"
    Body {
        STOP
    }
}
```

Production examples live in `tests/fixtures/brain_runtime/`, including
`mc2_17_ai.fit` and `mc2_01_varmission_specials.fit`. The specials parser
also accepts its existing functions, variables and FSM declarations.
Source editing preserves that grammar without converting it to mission
FIT fields or runtime memory cells.

`BrainExecutionPolicy` automatically selects TechBrain for sidecars found
in mod overlays; base-game sidecars alone are deliberately ignored.
`MC2_BRAIN_POLICY=legacy` remains the kill switch, and `techbrain` can
explicitly enable base-game fixtures. Saving next to base mission files
does not change these policy rules. Inline mission `WarriorN/Brain {}`
definitions retain their existing runtime eligibility and precedence.

## Editor ownership and persistence

The document owns each optional source byte string; absence and an empty
file are distinct. The mission brain sidecars panel edits mission-wide
source, with Apply and Remove commands. Undo/redo includes both sources,
dirty tracking compares against the saved sources, and history accounts
for their storage. Other structural edits preserve the sources verbatim.
Comments, unknown extensions, newline style and script text survive
Open/Save and Save As byte for byte. Authoring commands bound each source
to the document FIT byte limit and reject NUL bytes; they do not compile
TechScript or promise runtime validity for arbitrary source edits.

Save publishes the FIT, PAK and optional sidecars under the existing
transaction journal, stages and verifies bytes before publication, and
rolls all files back together on publication failure. Removing a source
removes the destination sidecar through that same backup transaction.
Save As uses the destination mission stem for both sidecar filenames;
Warrior references inside the source remain literal and are never
silently renumbered. Authors must update those references when changing
Warrior layout. Missing stock sidecars stay absent; an untouched stock
mission retains byte-identical FIT and PAK data.

Legacy ABL module/cell assignments, declarative brain definitions and
Part waypoint routes remain in the mission FIT. Stable path/point IDs
and names use the additive `EditorPathId`, `EditorPathName` and
`Waypoint{i}Id` fields; runtime route coordinates keep their existing
Part-key format. This packet introduces no new runtime sidecar keys,
binary schema, feature gates or embedded-play implementation.
