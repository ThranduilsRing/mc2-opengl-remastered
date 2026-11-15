Input-dragdrop-1 state (2026-11-14)

- Assigned branch/worktree verified clean; existing tip dfcb47099 contains assigned base f92dded5. No reset.
- Read CLAUDE.md, lane rules, alttab-selection brief and mech memory index.
- RCA: gos_input.cpp polls GetAsyncKeyState without focus gating; gameosmain.cpp drops SDL events only. Background/activation mouse clicks can therefore change selection.
- Drag investigation: UserInput retains pressed/held edges and normalized anchor; componentlistbox.cpp:265 requires selected item, held drag, pointer AND anchor inside item/list. Checking GUI bridge capture and coordinate transforms next.
- Plan: pin both failures offline in existing tests; smallest runtime fixes; final lint and one build; isolated E:/mc2-installs/input-dragdrop-1 deployment, lease-respecting XTX offscreen proofs if tooling supports the required interactions.
- Implemented press ownership, focus/activation quarantine, cancellation without synthetic mouse-up, and anchor-based inventory/diagram drag starts; cancelled diagram drags restore the component. Added focused regressions to test_input_capture_policy.cpp.
- PASS: input_capture_visibility contract (nightly selected row), diff check. Unit build pending; logs and continuation state: G:/mc2-tmp/slot02/input-*.log and E:/mc2-archive-2026-10/proofs/input-dragdrop-1/.
- PC lease status: owner's v0.4 mc2.exe PID 42128 is active. No game launched. Remaining: units, final lint/commit/build, isolated deployment and bounded host wait. Approved smoke runner has no existing component-drag/four-mech selection assertions; those integration claims remain unproven. Call budget 150.
- Orchestrator steer: general logistics drags included; DHS has a separate thermal-budget guard in mechlabscreen.cpp:2013 and logisticsvariant_stats.cpp:379. Cooling is chassis->maxHeat plus removable slot sink damage (stats.cpp:125); no Clan-specific removal ban. Remove weapons first if the remaining cooling would be insufficient. Owner's exact loadout not supplied, so its DHS cause cannot be confirmed.
- Merge promoted play revert 1c5b0783d before final build/deploy (required by steer to match deployed tree assets). Initial lane lint PASS; final receipts live in the external proof folder above, keeping this repository note stable for executable identity.
- General-drag review: mechlistbox.cpp:386 and mechbayscreen_update.cpp:270 already use press anchors. Pilot list and pilot-ready icons had the same moving-pointer restriction as components; applied the shared anchored predicate there too. No heat-sink gameplay rules changed.

- Runtime receipt: 67f5e0824 passed XTX smoke-lite mc2_01/mc2_10 with exact fingerprint, zero content errors; 26.9m lease wait. Offline final policy tests 7/37; initial full selected file 8/45. Mech-lab UI proof underway.
- New 23:20 steer: merge promoted play 36a2fe7fb (tree assets re-landed plus shader bake hash), rebuild/redeploy. Prior receipt stays tied to 67f5e0824. External result.md holds current status; finish requested four-mech focus proof if budget permits.
