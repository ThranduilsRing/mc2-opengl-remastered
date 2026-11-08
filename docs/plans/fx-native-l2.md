# Native FX playback (L2)

Assignment: slot05, `claude/fx-native-l2`, L1 base `76d814250`.
No game callers, legacy-directory changes, installs or game launches.

Final manifest (16 files; hard cap 20):
- `mclib/fx/fx_{runtime,emitters,tubes,geometry}.{h,cpp}` (8).
- root and unit `CMakeLists.txt` (2).
- `tests/unit/test_vfx_native_playback.cpp`, `test_vfx_frozen_driver.cpp`,
  `test_vfx_frozen_clouds.cpp` (3).
- `tools/mc2fx/native_checks.py`, `tools/vfx_oracle/lifecycle_driver.cpp` (2).
- this state note (1).

Implemented: independent lifetime/event traversal, particle births/integration,
all tube profiles, mesh/card/triangle/point packets and PointLight samples.
Instances borrow immutable L1 records and a caller-owned FX random stream.
Const frame extraction has no random interface. Packets retain root-to-leaf
material references for L3 composition; mesh primitives remain L1 data.

Oracle: all 904 stock definitions, three modes/seeds each; nine controlled
classes, eight profile/alignment variants and four scenarios each. Compare
bit-exact recursive state, event/name order, full RNG table/draw count, ages,
lifetimes, bounds, lights, generated vertices/colors/UVs, submitted particle
positions/UVs/topology, tube indices and actual mesh transforms/scale/paint.
Controlled cases exercise nested effects, equal-time events, fractional births,
dead-slot reuse, loops, explicit normalized ages, seed overrides, Start while
executing, mid-life Stop/Kill and two camera positions. No game caller changes.

Identical stock classes: Effect, PointCloud, ShardCloud, CardCloud, ShapeCloud,
EffectCloud, Card, Shape, Tube and DebrisCloud. Controlled fixtures also prove
PertCloud and PointLight (absent from stock). No unmatched concrete class.
Abstract ParticleCloud/SpinningCloud/Singleton cannot be instantiated.

Preserved legacy details: explicit MSVC RNG argument order; scalar Lerp's
weighted formula; old-age event dispatch; EffectCloud DrawInfo forwarding;
Debris dead-slot bounds traversal; tube U-range analysis/load repair.
The full stronger oracle passed before the final name/Kill assertions and
material-chain metadata. Stock work is split into four existing doctest
subcases to fit the shared child's fixed 60-second timeout; no timeout change.

Next: commit locally, then prove the exact tip with Ninja mc2/fx_native,
fx_native_boundary/glm_convention/sim_fp_contract rows, fx_native/fx_playback
plus existing frozen/lifecycle doctests, cxx17 ratchet and one lane lint.
Evidence: `E:/mc2-archive-2026-10/proofs/fx-native-l2` (final receipts/logs).
No installs, game launches, copied exes or new scratch tools. Retain proof logs
and reused build trees. L3 owns resource/state composition and submission;
L4/L5 own caller migration. Visual/mod integration remains their merge proof.
