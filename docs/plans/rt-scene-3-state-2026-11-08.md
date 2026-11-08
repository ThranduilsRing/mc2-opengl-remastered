# rt-scene-3 state, 2026-11-08 (NOT proved; handed back on budget)

Tip: see `git log -1` on claude/rt-scene-3 (after dafd94e00).

Done (code + offline proof): material table (MaterialRow, flags, cutout threshold), terrain row per tile, per-instance SBT offset = first row,
cutout geometries non-opaque and alpha-tested in rt_scene_probe.comp, `material result=` selftest line under MC2_VK_RT_SELFTEST=1,
scripts/rt_scene_probe_check.py (can fail; 5 pytest), docs/rt-scene-contract.md (explains the 1/8 XTX prop miss), rt doctests 35/35.

Runtime (XTX, sync validation, mc2_01 + mc2_10): smoke PASS, 0 validation errors, 0 SYNC-HAZARD; mover probes 3/3.
Selftest material result=FAIL on both maps (props 7/8 and 5/7, vegetation 2/4 and 3/4, terrain 4/4, false_holes 0).

Open bug: `FAIL prop build ... stage=tlas_record` (recordTlasBuild returns false) on most prop rebuilds, so the prop snapshot is stale and the
prop/vegetation probes miss. First cause found and fixed: the new material/uv buffers had odd sizes, VMA packed `rt_scene.instances` behind them
at a non-16-byte address (kInstanceAddressAlignment); padding to 256 bytes fixed the terrain build. Prop TLAS still fails: next step is to log which
check in recordTlasBuild/recordBuild fails (instance address % 16, scratch reserve) and, if alignment, allocate the instance buffer with
vmaCreateBufferWithAlignment(16) in OwnedBuffer instead of padding neighbours.

NOT RUN: 2070 selftest, smoke-lite without selftest, cxx17 ratchet, lane_merge --lint, A/B.
