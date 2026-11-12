# Campaign red marker selection

Owner symptom: after mission 6 (Base Defense), red campaign-map mission icons
cannot be clicked to choose between missions. Keep markers red.

## Fixture and reproduction

Existing save: `C:/Users/Joe/.mechcommander2/savegame/after_mission_mc2_06.fit`.
Its General block identifies `data/campaign/campaign.fit`, Carver V Campaign,
group 5, one completed mission (`Mission0 = 0`, meaning `mc2_06`), current
mission `mc2_07` (Sector Occupation: Garrison), CBills 829060.
Earlier campaign groups contain mc2_01 through mc2_05; this save serializes
completion indices for the current group only, not their individual history.

Stock campaign extracted from the isolated seed's mission.fst: group 5 uses
MCL_CM_op1_6, has mc2_06, mc2_07, mc2_08 and hidden mandatory mc2_09, and requires
two completions plus the mandatory mission. Post-m6 available IDs are mc2_07,
mc2_08 and mc2_09; visible selectable red choices are mc2_07 and mc2_08.
The hidden mission stays hidden until the existing progression rule reveals it.
Availability and marker count come from campaign/save data, not the owner crop.

Load the save, open Campaign Missions, click mc2_08, then mc2_07, then repeat.
The title/description and persistent selected marker must follow each choice.
The completed mc2_06 marker and hidden mc2_09 must not become selectable.

## Cause and change

gui/logisticsscreen.cpp:384 routes a defs-backed operation screen to
GameOSPage::update instead of updating its legacy buttons. gui/UiDefs.cpp:560
sends clicks to that operation screen's inherited no-op handleMessage
(gui/asystem.h:133). MissionSelectionScreen::update waits for a changed legacy
isPressed state, so the click never reaches setMission.

The mission screen now presses an enabled marker under a real click in its
existing hit-test loop when the operation page uses defs. Existing dispatch,
unpressing the previous marker, mission mapping and campaign progression stay
in use. No marker artwork, color, campaign data or availability rules change.

## Offline proof

scripts/tests/test_campaign_marker_selection.py compiles the production update,
handleMessage and setMission bodies with stub engine services. The defs
operation page's no-op handler reproduces the actual broken boundary.
It covers mc2_08 -> mc2_07 -> mc2_08 -> mc2_07, completed/hidden rejection,
persistent selection, and the legacy fallback. This is an isolated input/dispatch
test; it does not claim to exercise the complete renderer or campaign loader.

Receipts: E:/mc2-archive-2026-10/proofs/campaign-red-icons-select-1/.
regression-before.log: FAIL, mc2_08 click leaves mc2_07 selected.
regression-final.log: PASS with production dispatch and mission-setting included.
Runtime capture and build receipts are maintained in that proof directory.
Continuation state: G:/mc2-tmp/slot05/notes/campaign-red-icons-select-1-state.md.
