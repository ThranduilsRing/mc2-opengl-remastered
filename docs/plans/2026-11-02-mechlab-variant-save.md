# Mech Lab variant save dialog and pilot ready overlap (lane7, claude/mechlab-variant-save)

## Working flow (before fix)
Mech Lab, edit loadout, ACCEPT, SAVE VARIANT dialog: typing goes to the legacy aEdit, Enter or SAVE writes the variant. It worked blind: the v2 page mcl_sv.fit had the edit box visible=false and 0x00000000 fills.

## Root causes
- Transparency: mcl_sv.fit (generated in c49d0ab13) used 0x00000000 fills for header and back_1..5 and the list; mcl_sm.fit uses 0xff000000.
- Visibility: mcl_sv edit box visible=false with no colors. LogisticsVariantDialog also read the legacy edit and the mcl_sm list key.
- Pilot ready: the pilot panel is a v2 defs page on the ImGui layer, above the gos black cover rect in PilotReadyScreen::render. With a mech selected the pilot icons, borders, placement rects (magenta placeholders), pilot name ("asd") and skill texts bled through the mech info panel. Separate cause from the dialog; separate commit.

## Fixes
- 3489529dd: mcl_sv.fit opaque + visible edit box with caret colors; LogisticsVariantDialog uses the defs edit box, focuses it on open, title SAVE VARIANT, prompt ENTER VARIANT NAME. LogisticsSaveDialog untouched.
- pilot ready: hide pilot-panel defs widgets and clear pilot texts in PilotReadyScreen::setMech, restore in setPilot.
