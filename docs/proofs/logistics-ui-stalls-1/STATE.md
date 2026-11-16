# logistics-ui-stalls-1 state
Assigned branch claude/logistics-ui-stalls-1; initial clean tip 9762cbe66.
ORCHESTRATOR-STEER.md supersedes launch conflict: at most 6 game starts, stop after first failed start. Allocation: baseline 1, candidate 1, smoke-lite 2, reserve 2. None used. Keep steer untracked for orchestrator.
Plan: independent fallback ownership/metadata + per-camera sampling guard, read-only label fix and asynchronous importer launch (separate commits), focused offline tests, final lint/build. Add baseline register/finalize zone. R2/R3 deferred until R1 measurements; no unmeasured optimization.
Sites: simplecamera.cpp:710/793/510; aedit.cpp:438/724/731; PlayerFrontEnd.cpp:58/89/447; mech3d_render.cpp:1131.
Preserve valid enc precedence, release failed types, balanced contexts. Missing camera must not touch shared texture. Never edit sibling-owned files, README, gates.
Implemented: fallback ownership/metadata, vehicle failure guard, per-camera sampling guard, read-only scrolling (7d335ebbd), async importer launch with future publication (1e7d5c610), selection register/finalize Tracy zone.
Offline: four LogisticsUI doctests, 43 assertions passed; added exit-handle cleanup test next. All 12 source enc stubs/plain INI/TGL pairs exist; hashes in archive orphan-source-check.json. Lane parity not yet established.
Route gap: tracy_capture_compare --via-menu boots the stock campaign automatically without synthetic input (scripts/smoke_lib/via_menu.py:1-18); it does not perform the named objective/purchase/reselect/lab route. menu_oracle needs visible desktop; forbidden here. No new harness per lane rules. R1 exact route and R6 pictures unproven; R2/R3 depend on R1.
Next: final offline tests, lint and scoped contracts, one PC build, isolated deploy/smoke. Do not claim 100ms, cold disk, or owner visual acceptance.
