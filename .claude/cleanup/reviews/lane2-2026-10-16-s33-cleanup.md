# Lane 2 session 33 cleanup review (f170964d8..9ddea7e06)

## Verdict
PASS with should-fixes. The dead-code claim holds, the deletion is clean, and the ungated check tests a member read with a failing case. Documentation has two stale leftovers.

## Must-fix
None.

## Should-fix
1. `shaders/terrain_lod_chunk.frag:822-825` (cement-word layout comment) still documents `bits19:16=edge-blend open-border mask` as live. After 9c528ddfe no writer or reader exists. Mark the bits unused or drop them from the layout. Same stale wording: `shaders/gos_terrain.frag:457,467` (names killswitch MC2_RUNWAY_EDGE_BLEND as "unchanged") and `GameOS/gameos/gos_terrain_indirect_cement.cpp:160,227` ("19:16 already spoken for"). The last one reserves bits for nothing.
2. `.claude/VK-PARITY-GAPS.md` line 1 still says the capture `gl_fbo` arm stays "until row 27 has its same-exe GL oracle run". Row 27 records that oracle (s32-gl vs s33-gl, 0 px). Reword the sentence.
3. Timer design note (lane2-state lines 26-34): the files are named by role, not path. Only `vk_gpu_timestamps.h`, `gos_render_pass_timer` and `mc2vkts::GpuTimer` are named. The "8 files" are never listed. Add the island source paths.

## Nits
- `UNGATED_FEATURES` is a hand-kept tuple with one entry. The ungated census is manual by design, so say that in the comment.
- The Total line spells out the full row history. Fine, but long.

## Checked-OK
- (1) The cement-word writers are in `gos_terrain_indirect_cement.cpp`. Grep over code/ mclib/ GameOS/ shaders/ for 16-19 bit packing finds no writer. The only `>>16` hits are colour channels and edgeStitch. 0d9d0e55a deleted the pack pass (ledger names the bits-19:16 pack pass).
- (2) The diff removes the whole dilate block, its debug class 4 branch and the legend text. Every uniform and local it used (`u_cementWUPT`, `u_cementGridSide`, `u_cementAtlasSeamFix`, `u_missionFlatLight`, `cementWordsF`, `cementBypassDiag`) is still used by the main and diag-connect branches. rTileUV, rGridSide and the other `r*` locals were block-local. Nothing is orphaned.
- (3) `reads_member` matches `.member` in comment-blanked, include-expanded VK text. A struct declaration does not match. `test_ungated_feature_is_ported_only_where_the_twin_reads_the_member` asserts NOT_PORTED/HIGH for declared-only and PORTED for a read. The allowlist entry turns stale once ported.
- (4) New comments are WHAT+WHY, with no journal text. The replacement comment in the solid-cement branch is one line.
- (5) Row 27 (closed) and row 28 (open) agree across the gaps file, the PARITY STATE header (closed 1-17 and 19-27; open 18 and 28), the "Total: 28 rows, open 18, 28" line, and lane2-state session 33.
- (6) The timer note is self-consistent: a pool per frame in flight (2), reset at the start of the slot's first command buffer, resolve of the other slot without WAIT, drops counted as `dropped=`. It gives the reader contract, the hand-off API, the frame-stats scope and a proof criterion.
