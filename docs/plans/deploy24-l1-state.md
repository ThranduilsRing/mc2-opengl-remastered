# deploy24 lane 1

Assignment: slot05, claude/deploy24-l1-logistics-saves, requested base f269b312d;
starting tip 8fc0c6c89 includes the two required Linux build fixes.

Plan: central SP/MP capacity; insertion/enumeration/trim; 10005 explicit campaign
records with 10004 reconstruction untouched; campaign/mission FIT cap; keep the
current UI at twelve until lane 2 pages both deployment screens.

Proof pending: deployment doctests and campaign record fixtures; box build/unit,
smoke-lite, mc2_01/03/10/24 determinism, saveload, targeted contracts, final lint;
one final Windows Ninja compile. No PC install is needed. No agents spawned.

Implemented: shared capacity and FIT cap policy, logistics insertion/enumeration/
trim, explicit 10005 inventory records, preflight before destructive clearing,
10004 reader and save-browser compatibility, twelve-widget UI insertion guards.
Added codec/policy doctests, real 10004 fixture (76 inventory mechs), and production
logistics doctests in the existing engine-linked seam for insertion/trim/failure
preservation. Trimming uses inventory directly to avoid lazy-init recursion.

Next: commit, box build/unit + deployment seam + requested runtime rows, final
Windows compile and lint. Proof results and any remaining steps are maintained
outside git in E:/mc2-archive-2026-10/proofs/deploy24-l1/state.md so receipts can
refer to the exact code tip. Lane 2 replaces the current ICON_COUNT admission
guards and twelve-slot reinitialization in BOTH Mech Bay and Pilot Ready with
global paged slot mapping; do not expose page two until placement/bar lanes land.
