# Visual surface consumers — 2026-11-08

Assignment: slot01, claude/visual-surface-consumers. Assigned base f269b312d;
checkout starts at 8fc0c6c89 (Linux fixes included).

Implemented: shared bilinear bake sampler with sim fallback; grass, tree and
building draw transforms and light rebakes; decals; conservative camera clearance
over all bake cells under the near-plane bounds, rechecked after repointing.
Gameplay positions, grounding, LOS, picking, far terrain and water are untouched.

Proof pending: sampler/ridge doctest and legacy height corpus, covering contracts,
box build/unit/smoke-lite/headless determinism+saveload, final Windows Ninja and
lint. Baseline box task 1 requests saved_205644 and owner_lowwide captures.
New bookmark staging was skipped because tests/visual/bookmarks is ignored;
the matching cliff/islet/max-zoom fixture is instead generated in box proof output.
An inadvertently queued missing-bookmark row must be reported as skipped/failed.
Then capture matching final poses, compare HZB on/off at max zoom, inspect montage.
Keep receipts outside git under E:/mc2-archive-2026-10/proofs/box/mc2-slot01/.
No PC install or scratch files created yet.
