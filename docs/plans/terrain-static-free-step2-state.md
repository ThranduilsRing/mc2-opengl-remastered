# terrain-static-free step 2

Assignment: slot04, claude/terrain-static-free, starting tip aaaf895a042.
Read prior slot08 receipt and integration plan sections terrain-static-free / 5.
Inspection: registration arms the tree bake latch; full update clears it;
prewarm shares the valid LOD0 slot across LODs; registered LOD selection does not
rearm it. Render recovery can invalidate on the shape cached-slot sentinel or
re-register after failed replay, arming the latch again. Prior mc2_24 census
has 48.6 tree bake updates/frame, no STATIC_REG_INVALID messages in full log.
Root cause proved: tree render still used bdForceDynamicDefault, which forces
NVIDIA through invalidate/re-register every static frame (buildings already use
the explicit gate). Sampled recovery slots 617/618/680/etc were valid, not pool
exhaustion. New source-sliced tree_bake_recovery doctest FAILED at 3bffd4a5e
on CHECK_FALSE(forcedRecovery<0>()), with the two queue tests passing.
Temporary diagnostic smoke at 9e70b616 passed; sampled tree bake count 65.34.

Implemented: explicit tree fallback flag, mutation-driven dedup/sorted-ID queue,
camera matrix / height epoch invalidation, spawn/pose/status/damage/team/power/
animation/falling/selection notifications, render-owned static frame stamps,
dynamic/effect requeue, dead terrain shadow walk removal. Sim order untouched.
Source review also preserves load-time recipes (consume pose dirtiness before
mission registration), keeps active light slots in presentation order, refreshes
on exact camera matrices/night changes, and removes the redundant full-scene
optional frame-jobs prepass. Pure statics leave the queue; lights/effects do not.
Covering queue doctests include drain-to-zero, repeated mark, height block,
reset and generator invalidation. Negative full box unit tier aborted in a job
system harness; targeted tree/queue selection isolated the expected failure.
Local queue-only harness passed (297 s, tool slowness: whole target rebuilt).
Final proof pending: box compile, targeted unit/contracts, census 10/24,
headless SIMID 01/03/10/24, smoke-lite and lint. Receipts go under
E:/mc2-archive-2026-10/proofs/box/mc2-slot04/<final-sha8>/.
Intermediate 6e0f76df: covering three contract rows and lane lint passed.
Git diff checks must recognize this repository's tracked CRLF source files:
core.whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol.
