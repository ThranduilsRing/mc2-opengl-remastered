# Campaign globe continuation

Assignment: A:/Games/mc2-slot08, claude/campaign-globe, starting tip 5c0aea6a1.
Part A remains on claude/campaign-planet; commits here are part B only.

Implemented: deterministic sphere registration in tools/campaign_globe.py.
Thirteen lossless stock compositions, eleven unique poses; op3_3/op3_4 alias
op1_5. Mutual SIFT, proper-rotation RANSAC and one robust pose/projection solve
retain 474 correspondences across 13 edges (loops included). Edge means are
0.37-0.82 stock pixels. Perspective fit: centre (127.40,118.28), radius 102.86,
radius/distance 0.23172. The earlier orthographic assumption caused drift.

The cook reconstructs a 1024x512 de-lit composite surface/cloud design map,
with approximately 98.3% observed coverage and a separate coverage mask.
Opaque clouds cannot be reliably separated from hidden terrain. Reticles and
moon are excluded before fusion. New material detail is fixed in spherical
coordinates; thirteen 512px TGA compositions retain framing, moon and rim.
Existing mod lookup and stock fallback remain; full-image UVs prevent HD crops.

Initial independent stock/render SIFT means: 0.25-0.56 stock pixels, >=113
landmarks per view. Final cook uses a shared spherical detail bake (avoids
recomputing it for each view); final receipts supersede those preview figures.
The PC was CPU-saturated (Idle cooks received <1 CPU second over many wall
minutes), so final cooking and pytest moved to an isolated box Python venv.

Next: finish final-tip cook pytest/asset checks, box build/smoke and the three
mission-select base/tip captures, one Windows Ninja compile and final lint.
Record exact verdicts, SHA and NOT RUN items in the proof results, not another
tracked bookkeeping commit. No PC game runs or new PC install are needed.

Proofs: E:/mc2-archive-2026-10/proofs/campaign-planet/globe-*.
Temporary box cook staging and G:/mc2-tmp/campaign-globe-* must be removed
after receipts are copied back. Keep the reusable box/PC slot install only.
