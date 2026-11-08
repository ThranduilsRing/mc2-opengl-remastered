# rt-scene-3 state, 2026-11-08 (TLAS record failure fixed; material probe aims correctly and PASSes on both GPUs)

Tip: see `git log -1` on claude/rt-scene-3. Runtime proof at a5d3af80e (probe fix d98b81513 + a5d3af80e); alignment fixes 7ad6b1395, b84cba758, 672c18249, d2fbf9bae.

Cause of the TLAS failure (diagnostic run at 7ad6b1395, XTX, mc2_01): `FAIL prop build gen=2282 types=0 instances=1160 stage=tlas_record why=instances count=995
addr=0x2021e8718 addr%16=8`. The driver reports a 4-byte alignment for host buffers, so VMA placed `rt_scene.instances` at addr%16 = 8 or 12.

Fix: `OwnedBuffer::create*` takes `minAlignment` (vmaCreateBufferWithAlignment). TLAS instance arrays use 16 (`kInstanceAddressAlignment`: static `rt_scene.instances`
at vk_rt_scene.cpp:369, `rt_scene.mover_instances` at :1268, the selftest instances), AS storage 256 and scratch minAccelerationStructureScratchOffsetAlignment.
The 256-byte padding of the material/uv tables and the +16 mover-instance offset are removed. Both TLAS paths check the address (vk_rt_acceleration.cpp:40 build,
:464 update). A failed record names its check on the FAIL line (`why=`). The rule is in docs/rt-scene-contract.md, "Buffer alignment rule".

Probe fix (d98b81513, a5d3af80e): the earlier probe FAILs were in where the probe aimed, not in the scene.
- Target = the type's largest triangle that has no coplanar twin (normals |dot| > 0.999, vertices within 0.05 of the plane, overlapping) and whose ray span
  (lift 0.25, length 0.5) sits 0.1 above a conservative CPU terrain height (the max of the four grid corners, `TerrainGrid`, filled under MC2_VK_RT_SELFTEST only).
  Up to 8 candidates per type and class; the first that passes the terrain test is used.
- A hit counts when instance, material row, layer and packet match. A different triangle of the same instance and row is logged as `material info` and counted
  in `prim_info`. Another instance or row stays a `material miss` and FAILs (scripts/rt_scene_probe_check.py checks the miss lines; pytest covers
  "same instance, different triangle = pass" and "different instance = fail").

Runtime (sync validation, MC2_VK_RT_SELFTEST=1, one run per GPU with both missions, at a5d3af80e):

| GPU | Map | Smoke | FAIL lines | Errors | Hazards | Material |
|---|---|---|---|---|---|---|
| XTX | mc2_01 | PASS | 0 | 0 | 0 | PASS props 8/8, veg 4/4, terrain 4/4, holes 53, false_holes 0, prim_info 0 |
| XTX | mc2_10 | PASS | 0 | 0 | 0 | PASS props 8/8, veg 4/4, terrain 4/4, holes 110, false_holes 0, prim_info 0 |
| 2070 | mc2_01 | PASS | 0 | 0 | 0 | PASS props 8/8, veg 4/4, terrain 4/4, holes 46, false_holes 0, prim_info 0 |
| 2070 | mc2_10 | PASS | 0 | 0 | 0 | PASS props 8/8, veg 4/4, terrain 4/4, holes 110, false_holes 0, prim_info 0 |

prim_info is 0 everywhere: with the twin filter the first triangle is also the one hit. The acceptance rule is kept for robustness.

Evidence: E:/mc2-archive-2026-10/proofs/rt-scene-3/a5d3af80e/.

NOT RUN: A/B, saveload, ms numbers, full tier1, mods.
