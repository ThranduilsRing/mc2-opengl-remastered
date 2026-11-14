Input-dragdrop-1 state (2026-11-14)

- Assigned branch/worktree verified clean; existing tip dfcb47099 contains assigned base f92dded5. No reset.
- Read CLAUDE.md, lane rules, alttab-selection brief and mech memory index.
- RCA: gos_input.cpp polls GetAsyncKeyState without focus gating; gameosmain.cpp drops SDL events only. Background/activation mouse clicks can therefore change selection.
- Drag investigation: UserInput retains pressed/held edges and normalized anchor; componentlistbox.cpp:265 requires selected item, held drag, pointer AND anchor inside item/list. Checking GUI bridge capture and coordinate transforms next.
- Plan: pin both failures offline in existing tests; smallest runtime fixes; final lint and one build; isolated E:/mc2-installs/input-dragdrop-1 deployment, lease-respecting XTX offscreen proofs if tooling supports the required interactions.
- Implemented press ownership, focus/activation quarantine, cancellation without synthetic mouse-up, and anchor-based inventory/diagram drag starts; cancelled diagram drags restore the component. Added focused regressions to test_input_capture_policy.cpp.
- PASS: input_capture_visibility contract (nightly selected row), diff check. Unit build pending; logs and continuation state: G:/mc2-tmp/slot02/input-*.log and E:/mc2-archive-2026-10/proofs/input-dragdrop-1/.
- PC lease status: owner's v0.4 mc2.exe PID 42128 is active. No game launched. Remaining: units, final lint/commit/build, isolated deployment and bounded host wait. Approved smoke runner has no existing component-drag/four-mech selection assertions; those integration claims remain unproven. Call budget 150.
