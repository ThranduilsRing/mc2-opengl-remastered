# Cleanup review: lane 1 B5.1 (TextureRef, releaseNode, extras deletion)

Commits read: e4ebb4e95, ef0086131, c331752b5, fb4e05000, af4a3f87c, 25a8a6aa0, 05d1c1daa. Read-only; no build, no launch.

## Verdict
ACCEPT-WITH-FIX. 0 blockers. TextureRef and releaseNode are correct; the 12 conversions keep behaviour except two deliberate changes the commit names.

## Must-fix
None.

## Should-fix
S1. The gosimage.cpp fix is a real bug and has no BUGS-FOUND-FIXED row (grep of the file for gosimage / removeTexture finds nothing). Before: `removeTexture(mcTextureNodeIndex)` passed a NODE index to an API that matches `gosTextureHandle`, so it released nothing, or decremented an unrelated node whose GL name equalled the index (possible early free of another user's texture). Needs a row; proof is by reading, no repro. Not written by this review.
S2. Unguarded sites. controlgui_init.cpp (2 loops), gvactor_geometry.cpp, utilities.cpp (`StaticInfo::~StaticInfo`) call `releaseNode(handle)` with no `if (handle)`. A holder that never loaded carries 0, which is a valid node index, so it releases node 0's reference. The old code did the same through node 0's GOS handle, so behaviour is preserved, but now it is a visible one-liner: add the `if (handle)` guard, or state in F2 that node 0 is reserved. mech3d_init.cpp is safer (0xffffffff, refused with the log line).
S3. `b5_asset_census.py` does not reject malformed input with a log line: a missing/unreadable list under `--check` (`open(LIST)`), a tracked file deleted in the working tree, or a git failure (`check=True`) raise a traceback. Wrap in try/except, print one line, return 2. `latin-1` decoding means no decode crash.
S4. Add a doctest that `ManagerTextureRelease` with a null `mcTextureManager` is a no-op (the process-shutdown path); the current test only covers `BasicTextureRef` with a fake releaser.

## Nits
N1. `releaseNode` on `numUsers == 0` returns silently; one MC2_LOG_ONCE would surface a double release.
N2. `kNoTextureNode` is 0xffffffff but many holders use 0 as "none"; adopting a 0 into a TextureRef in B5.1c needs a decision written in F-rules.
N3. c331752b5 is a 2,776-line CRLF rewrite; correct, noisy by design.

## Checked-OK
1. TextureRef: no static-duration owner exists (grep: only the typedef and the test). Move ctor exchanges the source to kNoTextureNode; move-assign checks self, resets then steals; the moved-from reset sees kNoTextureNode and releases nothing; copy deleted; dtor, reset, release() are noexcept; releaser is `noexcept` and guards null manager.
2. releaseNode: bounds check against MC_MAXTEXTURES with one log line, no-op at 0 users (the old wrap is gone), `destroy()` frees GOS texture, name and data heaps, then `init()`. This matches or exceeds the old removeTexture, which freed the name only when textureData existed.
3. Conversions: the eleven handle sites passed a node to `get_gosTextureHandle` only to look it up again; direct node release equals the old effect minus an upload thrown away (named in the commit). gosimage corrected (S1). mc2fx stub follows. `removeTexture` has no remaining declaration or caller.
4. Extras deletion: grep finds no `addTerrainExtra`, `vertexNodeExtras`, `numExtras` or `TerrainExtra` store in txmmgr; remaining `gos_TERRAIN_EXTRA` uses are the live per-draw buffers in quad_draw.cpp and mapdata.cpp, not the node stores. No save/load reference to the stores (they were per-frame, cleared in clearArrays), so no skip reader is needed. The earlier review's S3 (per-frame zero-fill) and S2 (C-style casts) are resolved by the deletion.
5. Comments in texture_ref.h, releaseNode and the txmmgr.h edits are WHAT+WHY. The two stale dated notes were removed. 05d1c1daa lowers the journal baseline 6497 to 6462 consistently.
6. Census: sealed list of 400 rows became 388 with 25a8a6aa0; the summary and `--check` diff logic are sound apart from S3.
7. Before-review F1..F12: concrete enough. F1-F4 are done; F5 (vector member, sized once, bounded counts), F7 (unique_ptr registry, numUsers unload kept), F8 (teardown order beside the declaration), F9, F12 are testable by grep or doctest. Weaker: F6 and F8 name no proof for the Sensor-shape deleter; give B5.2 a doctest that pins the get/get/remove/remove transitions first (invariant 5).

## Opinion: is an Opus before-review needed?
Not for B5.2/B5.3 as written, provided the invariant-5 doctest lands first and the mission-reload delta (H2) is run, because every change is mechanical (buffer to vector member, raw owner to unique_ptr) and the proof is by-domain. Spend an Opus pass only on B5.3's registry/teardown order (F7/F8), where a use-after-free is possible and a shape can be shared across appearance types; a Sonnet after-review per sub-batch is enough elsewhere.
