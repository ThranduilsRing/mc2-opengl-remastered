# rt-scene-3 state, 2026-11-08 (TLAS record failure fixed and proved; material probe still FAIL on probe-target geometry)

Tip: see `git log -1` on claude/rt-scene-3. Runtime proof at d2fbf9bae (fixes in 7ad6b1395, b84cba758, 672c18249, d2fbf9bae).

Cause (diagnostic run at 7ad6b1395, XTX, mc2_01): `FAIL prop build gen=2282 types=0 instances=1160 stage=tlas_record why=instances count=995
addr=0x2021e8718 addr%16=8`. The driver reports a 4-byte alignment for host buffers, so VMA placed `rt_scene.instances` at addr%16 = 8 or 12.

Fix: `OwnedBuffer::create*` takes `minAlignment` (vmaCreateBufferWithAlignment). TLAS instance arrays use 16, AS storage 256 and scratch
minAccelerationStructureScratchOffsetAlignment. The 256-byte padding of the material/uv tables and the +16 mover-instance offset are removed.
A failed record names its check on the FAIL line (`why=`, takeLastRecordFailure). The rule is in docs/rt-scene-contract.md, "Buffer alignment rule".
Doctest: batched scratch sub-ranges stay aligned and inside the reserve at any base (rt doctests 36/36). Probe rays now start 0.25 above the
target triangle instead of 2.

Runtime (sync validation, MC2_VK_RT_SELFTEST=1, one mission per run, at d2fbf9bae):

| GPU | Map | Smoke | FAIL lines | Errors | Hazards | Movers | Material |
|---|---|---|---|---|---|---|---|
| XTX | mc2_01 | PASS | 0 | 0 | 0 | 1/1 | FAIL props 5/8, veg 3/4, terrain 4/4, false_holes 0 |
| XTX | mc2_10 | PASS | 0 | 0 | 0 | 3/3 | FAIL props 6/7, veg 4/4, terrain 4/4, false_holes 0 |
| 2070 | mc2_01 | PASS | 0 | 0 | 0 | 1/1 | FAIL props 5/8, veg 3/4, terrain 4/4, false_holes 0 |
| 2070 | mc2_10 | PASS | 0 | 0 | 0 | 4/4 | FAIL props 5/7, veg 3/4, terrain 4/4, false_holes 0 |

Every remaining miss is in where the probe aims, not in the scene. The misses are the same on both GPUs.
- Terrain hits at t=0.25: the type's largest triangle is a ground plate coplanar with the terrain.
- Terrain hits at t=0.20: the plate is buried 0.05 under the terrain.
- Same instance, row, layer and packet but a different primitive at t=0.246/0.250: another triangle of the same mesh is coplanar or just in front.

Next step: choose the target as the largest triangle whose centroid is above the terrain and that has no coplanar twin. Alternatively, accept a hit that has the expected instance, row, layer and packet within 1e-2 of the lift distance.

NOT RUN: A/B, saveload, ms numbers.
