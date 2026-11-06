# W122 listbox ownership

Assignment: A:/Games/mc2-cxxgui, claude/w122-cxx-guifound,
base 10a95ed41ec025c27994746a880e1e955f60e59d.

Implemented owned AddItem on aListBox and its three derived implementations.
Owned input is destroyed on rejection; capacity is checked before releasing to
the existing raw storage/InsertItem. Sorting and listbox item deletion stay local.
Pilot review and promotion callers use make_unique/move. Drop/combo text builders
and their forwarding calls also use owned input. No static owners added.

Raw compatibility: aListBox still accepts borrowed rows (chatwindow,
mpgamebrowser) and legacy owned rows. MechListBox and LogisticsPilotListBox raw
overrides forward to their owned overloads, preserving sorting for old callers.
PilotListBox has no raw override: every caller was migrated. Drop/combo raw
overloads forward owned heap rows from the external callers below.

## Every remaining external raw item call (base-source line numbers)

- code/chatwindow.cpp:295 (borrowed array row)
- code/componentlistbox.cpp:501
- code/keyboardref.cpp:168,178
- code/logisticsdialog.cpp:650,695,717,733,1636,1666
- code/mechbayscreen.cpp:182
- code/mechbayscreen_mechs.cpp:92
- code/mechbayscreen_update.cpp:210,237
- code/mechlopedia.cpp:464,487,530,688,721,731,742,753,798,849,859,869,880,891,903,1037,1079,1112,1121,1133,1148,1159,1170,1180,1309,1312,1343,1533,1574
- code/mechpurchasescreen.cpp:322,347,566
- code/missionbriefingscreen.cpp:819,832
- code/mpgamebrowser.cpp:383 (borrowed array row)
- code/mphostgame.cpp:130
- code/mploadmap.cpp:259,261,266,408
- code/mpparameterscreen.cpp:1778,1879,1897,2153
- code/mpprefs.cpp:184
- code/mpsetuparea.cpp:98,102,148
- code/optionsarea.cpp:1550
- code/pilotreadyscreen.cpp:261,690,780
- code/salvagemecharea.cpp:178
- code/simplecomponentlistbox.cpp:63,77,164

String/resource AddItem overloads and commented-out calls are not raw item calls.

## State and next steps

Initial implementation e812aa3ee4b29d8368b58b2ef1b225ec1a89301b:
ratchet and own_inventory green; box build/unit/smoke-lite PASS.
unit-san: one IBL pointer-identity test fails, zero ASan/UBSan findings; neither
tests/unit nor RenderCore changed. Lint: file-size growth and stale ownership
inventory. Moved short ownership forwarding wrappers inline to fix file size;
derived inventory regeneration stays with the cluster per owner instruction.
Next: final-SHA box build/unit/unit-san/smoke-lite and lint, then box cleanup.
Exact receipts and final run-state live under
E:/mc2-archive-2026-10/proofs/box/mc2-cxxgui/<sha8>/.
Retain this worktree and proofs. No PC builds or installs created.
PC GUI captures owed: mech bay/purchase/salvage sorted lists, pilot ready sorted
list, pilot review dead/active rows, pilot promotion skills, dropdown/combo lists.
