# lighting-hud-residuals state

Assignment: slot06, claude/lighting-hud-residuals, base 8fc0c6c89.

Implemented: slot/thread-local HUD scratch; adjacent equivalent command merging
with complete-primitive and contiguous-range guards; registration-revision light
membership cache (replaces per-frame world-slot scan and linear membership).
Classification itself stays in the existing registration tags; the pointer cache
refreshes lazily after tag/removal/reuse/reset, including empty-slot untag.
Doctests cover state differences, alpha order, primitive boundaries, light
classification parity with the old slot scan and lifecycle invalidation.

Proof commands at the resulting source tip (results recorded outside git under
E:/mc2-archive-2026-10/proofs/lighting-hud-residuals and proofs/box/mc2-slot06):
- scripts/ninja_build.py mc2 -j6
- tools/build_contract_harnesses.py --run --unit --only test_light_pool_culling
  --only test_present_handoff --only test_world_light_tags
  --only object_light_row_harness -j6
- bash scripts/check-contracts.sh --rows 'lighting|present' -j6
- tools/lane_merge.py --lint HEAD
- tools/box_run.py --gpu 0 -- build unit smoke-lite

Continuation: resident terrain inputs and complete source dirty feed implemented.
MapData::setVertexHeight marks the changed absolute vertex; same-height writes
do not mark. calcLight advances a bulk epoch for normals and sun shadows;
calcWater/recalcWater/clearShadows, map replacement and PostcompVertex assignment
also invalidate. Craters/damage overlays do not write these vertex inputs;
any height deformation uses the same height setter. Light/camera/weather scalars
remain per-frame inputs and lighting compute still runs every frame.

TerrainVertexPack checks source revisions plus transient calcThisFrame bits,
source presence and XY before packing. Off-screen edits survive until re-entry.
Mission init resets the CPU pack and source feed. NativeProducers owns one
resident absolute-indexed input buffer per retired frame slot; each slot keeps
independent revision receipts, coalesces adjacent dirty vertices and acknowledges
only successful uploads. Zero-filled unvisited holes preserve the old window
semantics. The Vulkan shader reads absolute indices; its synthetic startup test
still uses a nonzero vertex offset. No island file was changed.

New test_terrain_vertex_dirty.cpp covers one edit = one range, clean = zero bytes,
adjacent merge, missed slot updates, random-edit/full-repack parity, moving window
re-entry, all input fields, lighting epochs and same-size mission reset.

Next: commit, Windows Ninja mc2 compile; box build/unit, lighting and shader/layout
contracts, smoke-lite (with sync validation for the resident-buffer handoff), lint.
Final receipts and exact SHA go in the external proof STATE.md, not a follow-up
documentation commit that would invalidate the proved tip.

No performance claim; weekly measurement belongs to the orchestrator.
