# load-objects-cut-1 state

Implementation base: 9762cbe662f4a4018866dcea6a84fc2cb45ce806. Owner baseline: 7032a81c7.
Worktree: A:/Games/mc2-slot12; branch: claude/load-objects-cut-1; BOXSLOT 5.
Budget: 150 tool calls, three engine starts total. Starts used: 0.

Plan: attribute objmgr_io.cpp without changing iteration; scoped offline proof and lint;
box build/deploy; one warm stock mc2_01 Tracy baseline using existing --via-menu mode;
stop optimization below 4000 ms for L5+L6, otherwise fix only demonstrated in-scope causes;
reserve two starts for final smoke-lite. Archive receipts before releasing resources.

Existing startup capture: tools/tracy_capture_compare.py:403 starts recorder before game
when --via-menu is selected (:598); scripts/smoke_lib/via_menu.py:45 boots stock campaign.
Set MC2_DIAG_TAGS=LOAD_LAPS and 3840x2160. Default post-load mode is unsuitable.
countObject (code/objmgr_io.cpp:132) loads types before addObjectLoop (:224);
aggregate addObject timing is instance initialization, not necessarily type-cache misses.
R2 memo already exists: mclib/content_ir.cpp:48, mclib/vfs_index.cpp:310-377;
restored-mtime invalidation is tested at tests/unit/test_vfs_content_digest.cpp:102.
No digest change without measured evidence.

Preflight: clean, assigned branch/root and pinned HEAD match. repo_query reports harness
extraction ambiguous; canonical CLAUDE.md and SLOTS.md commands read directly.
BLOCKED-ON-BASE: box lane_merge --lint HEAD exits 1 in tools/lane_merge.py:1000,
before any lint row. Its shared temporary clone attempts to smudge 3rdparty.zip;
LFS object c8a9425bad3eb8adba4461f3dbea98a1f925b8c1590feea81aa47fd11dc971aa
is absent remotely. No tooling fix or retry; route to orchestrator. Temporary clone self-cleaned.
Offline at 2c3c9a5c98: cxx17 ratchet PASS; scoped box unit PASS (13 cases, 95 assertions,
one expected intentional failure). Filters: objmgr_contract_harness/*, same-size edit*,
mod override*, duplicate FST key*. No registry-content identity test identified.
R1, R3-R6 NOT RUN due to required lint blocker; R2 memo already implemented,
its existing invalidation tests PASS, runtime digest ms NOT RUN. No perf claim.
Build/deploy NOT RUN. Starts used: 0/3. Cold matrix, night baseline, owner mods,
12 unspecified poses NOT RUN per brief. Archive: E:/mc2-archive-2026-10/proofs/load-objects-cut-1/RESULT.html.
Next: orchestrator repairs missing-LFS lint prerequisite, then continue at this lane tip.
No runtime behavior optimization shipped; owner acceptance pending. Keep incremental box
build/test/install trees and source for continuation; release PC slot after archiving this note.
