# Community campaign import inventory

Inventory date: 2026-11-14. These are separate content editions, not interchangeable
engine downloads. User-owned mission, model and texture bytes are never shipped here.
**NOT RUN means unverified, not playable.** Historical starter-guide claims that all
campaigns play are not acceptance evidence for the current Vulkan build.

The lane imported all 23 locally supplied source packages and repeated each import
unchanged. Import validation does not certify gameplay. Mission observations below
use engine commit `3eb71ee7f09759e6bb028b8b4ed76e211df9c7a8`; the external proof is
`E:/mc2-archive-2026-10/proofs/mod-campaign-import-1/RESULT.html`.

| Campaign / edition | Download name or archive | Required download before importing | Current lane status |
|---|---|---|---|
| Magic: Exodus (original MC2) | Exodus_Campaign_11.exe | mc2_patch.exe plus campaign, installed together in a separate source folder | Import/repeat PASS; runtime NOT RUN |
| Magic: unofficial expansion / Carver V | magicsunofficialexpansion, Magic_MC2_archive.zip | Full expansion; the Carver V brain/purchase patches alone are incomplete | Import/repeat PASS; runtime NOT RUN |
| Omnitech Exodus | MCOsetup628.exe | Full Mech Commander Omnitech 628 | Import/repeat PASS; a_0_1 loads, smoke FAIL: 1 texture error |
| Omnitech MechWarrior 2 Mercenaries | MCOsetup628.exe (mw2mercs.fit) | Same full 628 download; select the second campaign FIT | Import/repeat PASS; mw2_0_1 loads, smoke FAIL: 1 texture error |
| Omnitech Carver V original | Carver5OForMCOsetup.exe | Omnitech 628 first | Import/repeat PASS; mc2_01 smoke PASS |
| Omnitech Modified Carver V | CarverVRedux.2.exe | Omnitech 628 first | Import/repeat PASS; acb_carver_01 crashes: null read in TerrainTextures::createTransition (terrtxm.cpp:1582); minidump retained |
| Omnitech Wolf Dragoons | WolfDragoonsForMCOsetup.exe | Omnitech 628 first | Import/repeat PASS; outreach smoke PASS |
| Omnitech Liberation of Volstand | VolstandForMCOsetup.exe | Omnitech 628 first | Import/repeat PASS; runtime NOT RUN |
| Omnitech Mercenaries Star | MercStarForMCOsetup.exe | Omnitech 628 first | Import/repeat PASS; runtime NOT RUN |
| Omnitech Desert Fox | DesertFoxForMCOsetup.exe | Omnitech 628 first | Import/repeat PASS; runtime NOT RUN |
| Omnitech Day of Heroes | DayOfHeroesForMCOsetup.1.exe | Omnitech 628 first | Import/repeat PASS; runtime NOT RUN |
| Omnitech Clan Eagle | ClanEagleForMCOsetup.exe | Omnitech 628 first | Import/repeat PASS; runtime NOT RUN |
| Omnitech Laskos Legion | Laskos Legion | Omnitech 628 first | NOT RUN: download absent locally |
| Omnitech Clan Wars | Clan Wars for Mech Commander Omnitech | Omnitech 628 first | NOT RUN: download absent locally |
| Wolfman Dead End (DEE) | wolfman-MC2X-DEE.exe | Full separate install; MC2X CVE-G compatibility base first | Import/repeat PASS; runtime NOT RUN |
| Wolfman Driftwood (DWE) | wolfman-MC2X-DWE.exe / MC2X DWE G (R03) | Same | Older edition import/repeat PASS; runtime and revised R03 NOT RUN |
| Wolfman The Houdini Project (HPE) | wolfman-MC2X-HPE.exe | Same | Import/repeat PASS; runtime NOT RUN |
| Wolfman Mother's Clan (MCE) | wolfman-MC2X-MCE.exe / MC2X MCE G | Same | Older edition import/repeat PASS; runtime and newer edition NOT RUN |
| Wolfman Rebirth of the White Company (RWC/WRC) | wolfman-MC2X-RWC.exe / wolfman-MC2X-WRC-G | Same | Older edition import/repeat PASS; runtime and newer edition NOT RUN |
| Wolfman Triple Campaign Edition (TCE) | wolfman-MC2X-TCE.exe | Same; includes Dark Rain, Pictures of a Rebellion, TangoMaster | Import/repeat PASS; runtime NOT RUN |
| Wolfman TangoMaster (TME-G) | wolfman-MC2X-TME-G | Full separate install; CVE-G base first | NOT RUN: standalone edition absent locally |
| Wolfman UpRise (URE-G) | wolfman-MC2X-URE-G | Same | NOT RUN: download absent locally |
| Wolfman Carver V (CVE-G) | MC2X-CVE-G | Full install; creates mc2x-compat | Import/repeat PASS; mc2_01 loads, smoke FAIL: 2 ammo errors; test map metropolitan loads with 7 ammo errors |
| Wolfman REAP | MC2X REAP G | Full install; CVE-G base first | NOT RUN: download absent locally |
| Wolfman REAP3 | MC2X REAP3 G | Same | Import/repeat PASS; runtime NOT RUN |
| Wolfman Corona Crisis / design test missions | MC2X (R44), MC2X (R70) | Full design install; CVE-G base first | NOT RUN: current downloads absent locally |
| Legacy Dark Rain / Pictures of a Rebellion / TangoMaster | Magic_MC2_archive.zip subfolders | Each complete campaign folder; CVE-G base first | All three import/repeat PASS; runtime NOT RUN; separate from TCE revisions |

Download catalogs (plain text; select the exact title above):

https://www.moddb.com/mods/mechcommander-omnitech/downloads
https://www.moddb.com/mods/wolfman-mc2x/downloads
https://www.wolfman-x.net/

Original Exodus archive instructions name these historical addresses; availability
is not guaranteed: http://www.mechwarrior.ws/downloads/mc2/Exodus_Campaign_11.exe
and http://www.mechwarrior.ws/downloads/mc2/mc2_patch.exe.
The Omnitech "Exodus Campaign - The Liberation Movement" download is a story
outline, not an additional playable campaign. Old Omnitech version numbers and
MC2X design releases are engine revisions, not additional campaign names.

See [player instructions](IMPORT-CAMPAIGNS.md). Per-import errors, first-mission
results and screenshots belong to the lane's external RESULT.html; absence of a
screen capture must not be interpreted as a successful mission launch.
