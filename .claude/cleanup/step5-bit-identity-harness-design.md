<!-- doc-status: current | reviewed:2026-10-17 -->
# Step 5 bit-identity harness: design

WHAT: the harness that step 5 (`Stuff::` math -> glm) opens with, per the owner ruling of 2026-10-11 and the meta-review section E bar: the
same inputs through the old and the new code must give byte-identical outputs (save files, mission exports, logs with masked noise), and the
simulation migrates only if they do. Built on the existing tools; names what is missing, the proof per batch and the cost per run.
WHY: a math swap that changes one rounding changes a pathfinding tie, a hit roll or a mover position, and every later save and determinism
baseline drifts. "Looks the same" is not a proof; identical bytes are.

## 1. Oracles that exist today

| tool | proves | strength | weakness for step 5 |
|---|---|---|---|
| `tools/objhash.py` (hash / compare, `--per-symbol`) | an edit left an object's code, data and relocations unchanged | exact, cheap (no launch) | a math swap changes code by design; useful only for the no-op batches (header moves, `using` aliases, deletion of dead `Stuff::` functions) |
| `tools/sim_determinism.py` (`[SIMID v1]` graded / visual / strict chains, `check`, `--baseline`, `pairs-until-fail`) | the sim is identical frame by frame, run to run and against a committed chain (mc2_03, 10, 16, 24) | the strict chain digests mover state per sim frame | covers 4 missions, 30-60 s; mc2_16 save bytes differ under host load (open, 2026-10-15) so the oracle is not yet stable there |
| `tools/saveload_roundtrip.py` + `tests/saveload/baselines/` (5 missions) | a mission-boot save has the committed masked digest | whole-state bytes | `--first-save-only` weak mode in force until the S3 Archive writer; raw vtable/pointer bytes are masked (BUGS 30-31) |
| `tools/save_inspect.py diff --mask tools/save_noise_mask.json --across-builds` | two saves differ only inside the known noise | names the first differing packet, record class and byte | the mask is by record class; the R6 review asks for byte-offset masks |
| `tools/campaign_save_roundtrip.py` | the logistics writers round-trip after a campaign start | covers pilots, inventory, campaign state | pilot rolls differ between independent captures; compare a save with its own re-save only |
| editor round trip (`scripts/run_editor_smoke.py`, `tests/smoke/editor/roundtrip-baseline.json`) | 24/24 stock missions load and re-save byte-identical | unmasked bytes of FIT/PAK | exercises the writer, not the sim; the MFC editor today, the rework later |
| `tools/log_identity.py` | two logs print the same line shapes and tag sets | catches lost/renamed lines | masks every number, so it cannot see a math change; shapes only |
| doctest (`mc2_tests`, 2.8k cases) | unit behaviour | fast | no `Stuff::` vs glm comparison exists |

## 2. What is missing

1. **M1 math corpus (doctest).** `tests/unit/test_stuff_glm_identity.cpp`: for every `Stuff::` operation the sim calls (Point3D, Vector3D, UnitVector,
   LinearMatrix4D, AffineMatrix4D, Matrix4D, Origin3D, YawPitchRoll, UnitQuaternion, Normal, Plane, Line, ExtentBox, angle wrap) and for the
   composite chains the sim actually runs (transform chains in `Stuff::` row-vector order through `axis_convert.h`, `MultiTransformShape`,
   mover heading/velocity integration, pathfinding distance and line-of-sight math), compute old and new over a seeded corpus and compare with
   `std::bit_cast<uint32_t>` per float. Corpus: 10^5 random finite inputs per op from a fixed seed plus edge values (0, -0, denormals, 1e±30,
   exact multiples of pi, unit axes, degenerate matrices). Composite inputs are RECORDED, not random (M2). Built with the game's flags (`/fp:precise`,
   no `/fp:contract`, same `/arch`), in the same TU configuration, so FMA contraction cannot differ between the two sides.
2. **M2 call recorder.** A build option `MC2_MATH_RECORD` (off by default, like `MC2_SIM_TRACE`) that writes the argument tuples of the top
   ~40 sim call sites (by call count from one mc2_03 + mc2_10 run) to a binary file; the corpus replays them. Without it, the corpus tests
   operations the sim never feeds (the meta-review §D.5 requirement: replay real call sequences, not element-wise ops).
3. **M3 sim-file list.** A committed list of the TUs that are SIM (feed `[SIMID]`, saves, ABL) versus RENDER-only, generated from the include
   firewall of sim-s1 (5e005d910) plus a grep; the exit rule needs it: sim files migrate only on a bit-identical corpus, render files may
   migrate on a capture proof.
4. **M4 byte-offset save mask.** Turn `save_noise_mask.json` record-class entries into offset ranges measured by `save_inspect.py ranges`
   over two runs of one exe (R6 of the B1 after-review); a step-5 batch must show "0 bytes outside the mask", and a class-wide mask can hide a
   math change.
5. **M5 old/new pairing runner.** `tools/bit_identity.py run --ref-exe OLD --exe NEW --missions ...`: one command that runs the four oracles below
   on both exes with the same seed, env and install data fingerprint and prints one verdict line per oracle. The pieces exist
   (`sim_determinism.py --ref-exe` passthrough landed in f418ee189; saveload `resave --plant`); the wrapper and the single report do not.
6. **M6 a stable determinism baseline on mc2_16** (the load-dependent save drift of 2026-10-15) before any step-5 batch starts; otherwise
   a step-5 diff on mc2_16 cannot be attributed.
7. **M7 glm vendored** in `3rdparty/glm` pinned by sha, with every `GLM_FORCE_*` define in one header (`mclib/math/glm_config.h`), included
   before any glm header (meta-review E.a).

## 3. Proof shape per batch

Step 5 batches follow the step 4 shape (one subsystem, at most 25 files, before-review). Each batch declares its class; the proof is per class.

| batch class | example | proof (all on the same seed, install, data fingerprint) |
|---|---|---|
| A no-op | delete unused `Stuff::` functions, header moves, aliases | `objhash compare` IDENTICAL or LAYOUT-ONLY for every object of the touched targets; nothing else |
| B render-only | camera, appearance, VFX, shadow matrices in render TUs (M3 list) | `objhash` limited to the touched TUs changes; `sim_determinism check` x4 missions identical; VK goldens `verify.py --golden current` PASS, day and night viewed; `tools/gpu_abi_gen.py` output unchanged |
| C sim | mover, pathfinding, weapons, collision TUs | M1 corpus bit-identical for every op and recorded chain the batch touches (zero mismatches, not a tolerance); `sim_determinism check` mc2_03/10/16/24 strict chains identical to the committed baselines; `saveload_roundtrip` 5 baselines identical (masked, M4 offsets, 0 bytes outside); `save_inspect diff --across-builds` old exe save vs new exe save at the same `save_at`: 0 bytes outside the mask; campaign round trip once; tier1 5/5 |
| D data writers | anything the editor or mission export calls | editor round trip 24/24 byte-identical against `roundtrip-baseline.json`; `log_identity` shape-identical on mc2_03 and mc2_24 |

Failure rule (owner, step 5): a class C batch whose corpus or chain differs by one bit is not committed as sim; it is split into the
render-only part (class B) and the sim part stays on `Stuff::`, with the TU recorded in the M3 list as "sim, Stuff kept". No tolerance, no
re-recorded baseline to absorb it. A baseline re-record is allowed only when the change is a declared fix (BUGS row) with its own proof.

Evidence per batch: one `.claude/cleanup/proofs/step5-bN.md` with the commands, the verdict lines of M5, the corpus mismatch count (0), the
objhash table, the report directories, and the capture montages viewed.

## 4. Cost per run (estimates from current tools; measure on the first batch)

| oracle | wall time | host cost |
|---|---|---|
| M1 corpus (doctest, ~40 ops x 10^5 + recorded chains) | under 1 min after build | one doctest build in the existing unit tree (2-job cap) |
| `objhash` before/after | 1-2 min (hash only) | none beyond the build |
| `sim_determinism check` x4 missions at 30 s, two runs each | ~8 runs x 60-90 s = 10-12 min | one launch at a time under the host lease |
| saveload 5 baselines + one across-builds diff | ~5 x 90 s = 8 min | lease |
| campaign round trip (once per sim batch) | ~3-4 min | lease |
| tier1 5/5 | ~5 min | lease |
| editor round trip 24 missions (class D only) | ~15 min | lease, editor launch background-only |
| captures for class B (three runs of 10 poses for a re-bless is NOT per batch; per batch one run + verify) | ~4 min | lease |

Typical class C batch: build (~15 min cold, ~2 min warm at 2 jobs) + ~30 min of launches, all serialized on the host lease. Class A: ~5 min,
no launch. One full M5 pairing run (both exes, class C set) ~60 min because every launch oracle runs twice.

## 5. Order
M7 glm vendored -> M3 sim-file list -> M1 corpus over the ops (random inputs) -> M2 recorder + recorded chains -> M6 mc2_16 stable -> M4 offset mask ->
M5 runner -> first class A batch -> class B render batches -> class C sim batches, one subsystem at a time, each against the M5 report.
Owner: lane 1 (sim and saves); the corpus and runner can be a Python/doctest-only lane; glm vendoring is a build change (2-job cap).

## 6. Opening corpus and pending verdict

`tests/unit/test_stuff_glm_identity.cpp` adds 100,000 seeded cases (MT19937 seed `0x53545546`;
float bits generated without standard-library distributions), plus signed zero, unit axes, denormals,
minimum/maximum floats, tiny/huge values, pi boundaries, singular and near-singular matrices.
Each `[STUFF_GLM]` row reports operation, sim/render tag, sample count, mismatches and classification.
`identical-on-corpus` means zero mismatches; `not-identical` means at least one, including NaN payloads
and signed zeros. Only component arithmetic is asserted identical; all other operations characterize
the candidate without failing merely because its semantics or rounding differ. Comparator negative
controls distinguish signed zeros and adjacent floats. Invalid legacy domains (zero-length normalization,
small divisors, small affine determinants) are excluded from those operations, not counted as matches.

| Operations (row-name prefixes retained) | tag | identity expectation | measured mismatches |
|---|---|---|---|
| vector.negate/add/subtract/scale/component-multiply/component-divide; point.add/subtract | sim-side | required identical | point.add 0/100305, point.subtract 0/100305, vector.add 0/100305, vector.component-divide 0/16477, vector.component-multiply 0/100305, vector.negate 0/100305, vector.scale 0/100305, vector.subtract 0/100305 |
| vector.add-scaled/combine/lerp/cross/dot/length-squared/length/approximate-length | sim-side | measure rounding and evaluation order | vector.add-scaled 0/100305, vector.approximate-length 0/100305, vector.combine 0/100305, vector.cross 0/100305, vector.dot 0/100305, vector.length 0/100305, vector.length-squared 0/100305, vector.lerp 49044/100305 |
| vector.scalar-divide | sim-side | expected not-identical: reciprocal multiply vs division | vector.scalar-divide 55150/100293 |
| vector.normalize; unit-vector.normalize | sim-side | measure reciprocal sqrt rounding | unit-vector.normalize 0/33974, vector.normalize 0/33974 |
| scalar.sqrt/approximate-sqrt/sin/cos/tan/atan2/asin/acos; angle.sincos-sin/sincos-cos | sim-side (conservative) | measure wrapper/overload precision | angle.sincos-cos 0/100289, angle.sincos-sin 0/100289, scalar.acos 78/100289, scalar.approximate-sqrt 0/100289, scalar.asin 2221/100289, scalar.atan2 3/79895, scalar.cos 70/100289, scalar.sin 62/100289, scalar.sqrt 0/100289, scalar.tan 13/100289 |
| angle.wrap-static/wrap-member | sim-side (conservative) | expected not-identical: boundary and reduction rules | angle.wrap-member 99500/100289, angle.wrap-static 99517/100289 |
| chain.path-distance/mover-integration/line-of-sight-step | sim-side | synthetic call sequences, measure | chain.line-of-sight-step 0/17555, chain.mover-integration 0/100305, chain.path-distance 0/100305 |
| axis-convert | render-only | required identical | axis-convert 0/100305 |
| vector.affine-transform/inverse-transform; point.affine-transform; chain.row-transform | render-only | measure row-vector transforms | chain.row-transform 0/100000, point.affine-transform 0/100000, vector.affine-transform 787/100000, vector.inverse-transform 23088/100000 |
| affine.multiply/inverse; linear.multiply/inverse; matrix.multiply | render-only | measure association and inversion | affine.inverse 72102/75000, affine.multiply 2164/100000, linear.inverse 100000/100000, linear.multiply 0/100000, matrix.multiply 0/100000 |
| matrix.inverse | render-only | expected not-identical: `det < SMALL` uses `1e30f`, including negative determinants | matrix.inverse 88463/100000 |
| quaternion.multiply/normalize | render-only | reversed product order adapted; normalize reconstructs w, expected not-identical | quaternion.multiply 0/100000, quaternion.normalize 61809/100000 |

Sim evidence: `code/mover.cpp` Cross/Normalize, `mover_path.cpp` distance/LOS,
`gvehicl_update.cpp` integration, `weaponbolt_update.cpp` velocity/lead. Point3D inherits vector arithmetic.
Render evidence: `mclib/camera_matrix.cpp` inverses, `camera_project.cpp`, `gvactor_update.cpp`
quaternions; `code/weaponbolt_render.cpp` and effect-only sections of `weaponbolt_update.cpp` transform chains.
Tags describe these operations' observed uses, not permission to migrate whole mixed update TUs.
Measured (seed 0x53545546, GLM 1.0.1 scalar, /fp:precise): 47 ops; 30 identical-on-corpus; sim-side 24/34 identical. Not identical (mismatches/samples): affine.inverse 72102/75000; affine.multiply 2164/100000; angle.wrap-member 99500/100289; angle.wrap-static 99517/100289; linear.inverse 100000/100000; matrix.inverse 88463/100000; quaternion.normalize 61809/100000; scalar.acos 78/100289; scalar.asin 2221/100289; scalar.atan2 3/79895; scalar.cos 70/100289; scalar.sin 62/100289; scalar.tan 13/100289; vector.affine-transform 787/100000; vector.inverse-transform 23088/100000; vector.lerp 49044/100305; vector.scalar-divide 55150/100293. Sim-side differing: angle.wrap-member, angle.wrap-static, scalar.acos, scalar.asin, scalar.atan2, scalar.cos, scalar.sin, scalar.tan, vector.lerp, vector.scalar-divide. Identical on a synthetic corpus is not migration approval: no sim operation is approved until recorded-chain and SIMID proof.
Zero-mismatch sim rows become candidates only after recorded-chain and strict SIMID proof; any mismatch
keeps that operation on Stuff in sim. This opening corpus does not implement M2/M3 or cover every
geometry API in M1 (Origin/Plane/Line/ExtentBox); it grants no migration permission for those APIs.

Vendoring action for the orchestrator: take upstream GLM **1.0.1**, commit
`0af55ccecd98d4e5a8d1fad7de25ba429d60e863`, from https://github.com/g-truc/glm;
copy its unmodified `glm/` directory and `copying.txt` (dual MIT/Happy Bunny; use MIT) into
`3rdparty/glm/`, retaining the commit pin in its README. This matches the stb/tinyexr header-only
precedent. Download here was blocked by network access; no partial vendor tree was created.
Unit CMake exposes those headers as `glm::glm`, or accepts an exact 1.0.1 installed CMake package;
otherwise it explicitly warns that the corpus was not built. No configure-time network fetch.
`tests/unit/stuff_glm_config.h` owns the sole `GLM_FORCE_PURE` definition and checks the version;
move this policy into `mclib/math/glm_config.h` when production first adopts GLM (M7 remains pending).
Both sides compile with `/fp:precise`, no contraction option, and the same default architecture;
the orchestrator must verify their flags against the game before treating results as migration proof.

Orchestrator proof: reconfigure the existing unit build tree, build `mc2_tests` RelWithDebInfo,
then run `mc2_tests.exe --test-case="*Stuff/GLM identity*" --no-skip` and retain every `[STUFF_GLM]` row.
The independent smoke checksum already exists: `py -3 tools/sim_determinism.py check mc2_03
--install lane3 --duration 30` (one shell line; repeat for mc2_10/16/24). Compare strict reference chains before
accepting sim changes; do not re-record a baseline to absorb a math mismatch. No runtime files changed.

## 7. Port log (lane 1, session 21, 2026-10-17)

Production GLM lives in `mclib/math/` (glm_config.h: the one configuration; mc2_math.h: exact wrappers; stuff_glm_bridge.h: Stuff<->glm
until the last Stuff type goes). The corpus now asserts 39 required rows at 0 mismatches (the 30 identical ops, 8 exact wrappers, the bridge).
Finding: the 1-ULP trig rows were the HARNESS include order. The gameos/mclib/mc2 PCH puts <cmath> first, so there Stuff::Sin binds sinf
(== glm::sin); only the non-PCH libraries (stuff, mlr, gosfx, the editor) bind the double CRT, which sinD/cosD/acosD/atan2D reproduce.
- S5.1 c9ff5eebf infra + wrappers + harness asserts (class A: no production user).
- S5.2 6d07a8e3c render-only: all 23 `Invert` and 16 `MultiplyByInverse` calls outside Stuff -> glm::inverse (camera, TG/MSL transform, shadow
  corners, gosFX, MLR). Stuff's OBB/Plane/Normal keep Stuff::Invert (OBB is collision geometry: not ported without a sim proof).
- S5.3 3f83f2a52 class A: callerless Stuff math deleted (Arcsin, Tan, Round_Float_To_Word, the MultiplyByInverse family, Vector3D
  Get*Component, AlignWithVector, Plane::CalculateX/Y/Z, Line3D::FindEnd, Ray3D::GetClosestPointTo/SetOrigin, OBB::BuildSphere).
- S5.4 22a0e8228 the last external callers of the differing ops (gosFX Sin/Cos/Lerp, cursor and terrain-light scalar divide) on the wrappers.
- S5.5 a7585245a (lane 1 s22) types with no caller outside Stuff deleted bottom-up: Origin3D and Motion3D (their conversion overloads in
  Matrix4D/AffineMatrix4D/LinearMatrix4D/Point3D/EulerAngles/YawPitchRoll/UnitQuaternion and the Note/Page notation accessors), and Degree
  (its one user, the MLR spotlight reader, keeps the identical deg * Radians_Per_Degree). Census of external uses that decides the next
  types: Ray3D 0 (base of Line3D), YawPitchRoll 0 (Stuff-internal EulerAngles notation I/O), SinCosPair 3, Line3D 8, Normal3D 8,
  ExtentBox 9, RGBColor 12, Sphere 14, Radian 17, Plane 18, UnitVector3D 23, AffineMatrix4D 31, OBB 61, Vector2DOf 126, UnitQuaternion 127,
  EulerAngles 212, Matrix4D 229, RGBAColor 265, Vector4D 277, Vector2DScalar 423, LinearMatrix4D 458, Point3D 710, Vector3D 2149.
- S5.6 36ecb4215 (lane 1 s22) Stuff::SinCosPair deleted: its 12 Stuff-internal sites (matrices from EulerAngles/YawPitchRoll, Euler/YPR
  from LinearMatrix4D, UnitQuaternion from a rotation vector, GetAngle, Multiply(q,t), Subtract, Vector3D from UnitQuaternion and
  YawPitchRange) call the same cosf/sinf and Arctan in the same translation unit; the gosFX tube profile (render-only) uses glm::sin/cos.
  The corpus sincos rows now assert sinf/cosf == glm::sin/cos (0 mismatches). Proof: sim pair identical to the pre-session reference,
  saveload x2 = baselines, capture pair viewed, 38 REQUIRED rows 0 mismatches, build_all_targets -j2.
- S5.7 0f485f8ab (lane 1 s22) Ray3D folded into Line3D (its only user was Line3D's base): origin/direction are Line3D members in the
  same order, Project and the plane distance moved verbatim; the hidden or callerless Ray3D functions and Find_Closest_Approach deleted.
- S5.9 (lane 1 s23) Stuff::YawPitchRoll deleted: its one real use was the notation-file UnitQuaternion entry (yaw, pitch, roll in
  degrees). The two conversions it carried (the AffineMatrix4D::BuildRotation(YawPitchRoll) matrix and the YawPitchRoll-from-matrix
  extraction) are now `Quaternion_From_Yaw_Pitch_Roll` / `Yaw_Pitch_Roll_From_Quaternion` in rotation.cpp, verbatim; the YawPitchRoll
  notation accessors (Note/Page), the matrix/Euler/quaternion constructors and assignments, Small_Enough/Close_Enough/Lerp/Normalize,
  the stream I/O and Spew went with it (no caller). Phase A corpus: both directions equal the class on 20000 samples incl. both +/-90
  degree pitch branches (0 mismatches); the output digest 4785bc9cd9dfa939 is the REQUIRED row after the deletion.
- S5.10 (lane 1 s23) Stuff::Radian deleted: angles are Scalar radians. EulerAngles/YawPitchRange members, the Matrix4D perspective
  field of view, the MLR spotlight spread, the light-manager spread and the gosFX tube deviation are Scalar; EulerAngles::Normalize
  and the quaternion-from-rotation-vector half angle call mc2::math::wrapAngleMember / wrapAngleStatic (corpus-exact); angle.cpp is
  gone (the Radian Lerp had no caller: EulerAngles::Lerp already used the Scalar Stuff::Lerp). The corpus wrap rows now compare the
  wrappers with the verbatim Radian::Normalize bodies kept in the test.
- S5.11 (lane 1 s23) Stuff::Line3D deleted with its only user, the MLR CastRay family (MLRShape::CastRay had no caller; the
  virtual MLRPrimitiveBase::CastRay and its MLR_I_PMesh / MLR_I_TMesh overrides went with it). Normal3D keeps its Plane use.
- S5.12 (lane 1 s23) Stuff::Normal3D deleted: its one user left was Plane::normal, now a UnitVector3D (same layout; the
  constructors, the normalizing assignment from a Vector3D and the dot product are UnitVector3D's own). Its transform members had no
  caller (Multiply(Normal3D, AffineMatrix4D) even computed an inverse it never used). Normal3DClassID stays in the class-ID enum.
- S5.13 (lane 1 s24) Stuff::YawPitchRange deleted: the polar triple is `mc2::math::YawPitchRange` (camera direction, gosFX and
  GPU-particle spawn samples, the LAB_ONLY MLR bird view) and its one used conversion, Vector3D = YawPitchRange, is
  `mc2::math::polarToVector` (verbatim sinf/cosf; glm::euclidean has another axis convention). The three-argument constructor is
  kept so the spawn sites' three Random::GetFraction() arguments keep a constructor call. The vector-to-polar direction had no
  caller and went with the type. Corpus row exact.polar-to-vector (REQUIRED) compares it with the verbatim legacy body.
- S5.14 (lane 1 s24) Stuff::ExtentBox deleted: the four gosFX bounds accumulators (debris, point, spinning, tube clouds) use
  `mc2::math::Aabb` (glm min/max vec3) and `unite`, which is ExtentBox::Union bit for bit with glm::min/max called with the
  arguments REVERSED (Stuff Min(a, b) = a < b ? a : b = glm::min(b, a); the order decides +0/-0 and NaN). Callerless and gone with
  it: MLRPrimitiveBase::GetExtend, the Plane ExtentBox tests, Find_Planes_Of_Boxes, Use_Scalar_In_Sorted_Array, the ASCII
  reader. Corpus rows exact.box-union-min/max (REQUIRED) against the style.hpp Min/Max macros.
- S5.15 (lane 1 s24) Stuff::Sphere deleted: `mc2::math::Sphere` (glm center + radius, 16 bytes as before) holds the gosFX debris
  piece spheres (streamed as a Point3D then a Scalar, unchanged bytes) and the tube profile bounds. No arithmetic moved: the
  Sphere union, the Plane sphere tests and Sphere = OBB had no caller and went with the type.
- S5.16 (lane 1 s24) Stuff::Plane deleted: the MLR mesh face planes (back-face test, light-map projection) are
  `mc2::math::Plane` (glm normal + offset, 16 bytes; never streamed, rebuilt from the coordinates). buildPlane, distanceTo and
  isSeenBy are the Stuff bodies verbatim (difference, cross, the one-reciprocal Normalize as `normalizeExact`, dot as written);
  the AlignLocalAxisToWorldVector and light-vector sites convert through the bridge. Callerless and gone: Plane::Multiply, the
  OBB tests and OBB::Intersects(Plane). Corpus rows exact.normalize, exact.plane-normal/offset/distance (REQUIRED) against
  the Stuff UnitVector3D/Vector3D operations.
- S5.17 (lane 1 s25, 2026-10-18; the step 5 tail, design E:/mc2-archive-2026-10/plans/step5-matrices-design-2026-10-18.md) every
  remaining Stuff math type deleted in one change set (25 commits of <= 25 files, built together): Vector3D, Point3D, UnitVector3D
  -> glm::vec3; LinearMatrix4D, AffineMatrix4D, Matrix4D -> glm::mat4 holding the Stuff ROW-convention matrix (entry (r, c) at
  m[c][r], translation in row 3, column 3 (0, 0, 0, 1) for an affine one: GLM_FORCE_CTOR_INIT); UnitQuaternion -> glm::quat
  (x, y, z, w storage, built by quatXYZW); EulerAngles -> mc2::math::Euler; OBB -> mc2::math::Obb. Method: a clang-query
  inventory of every member call, operator and non-copy constructor of those classes over the 288 TUs that name them (PCH flags
  stripped), rewritten by AST range (macro arguments by their source text), then the type names lexically (strings, comments and
  enum members skipped). Owner ruling 2026-10-18: P/R by default, exact only for byte formats (E1 anim-cache quaternion bytes,
  E2 48-byte matrix / 64-byte OBB stream records in mclib/stuff/glmstream.hpp, E3 12-byte vectors in the save *Data structs).
  Convention adapters (mclib/math/mc2_xform.h) keep the Stuff bodies verbatim; corpus tests/unit/test_glm_math_identity.cpp
  (10 REQUIRED rows vs the legacy bodies, the convention guard with its negative control, the E rows) replaces
  test_stuff_glm_identity.cpp; lint row glm_convention. RESULT: the sim pair stayed IDENTICAL to the cd53af7df reference on
  mc2_03/24/10/16 over the whole windows and every baseline passed (no re-record); saveload x2 = baselines. The R relaxations
  (glm::normalize for the quaternion Normalize, quat_cast for quaternion-from-matrix, 4-term mat4 products) did not move a
  single SIMID digest in the windows run. Left on Stuff: glmstream.hpp (E2 records, the Axes enum and axis macros), Vector4D
  (on glm::mat4: MultiplyPoint / MultiplyVector / MultiplyAffine), Note/Page vector entries on glm::vec3, the scalar helpers.
Proof every batch: sim pair mc2_03/mc2_24 identical to the pre-port reference (exe 579b60e2c) and to the committed baselines; saveload mc2_03
fb63e85ac66966cf and mc2_01 d46e3e5e30d26b15 = baselines; S5.2/S5.4 GL/VK capture pair day mc2_01 + night mc2_10 viewed.
Left on Stuff: the types themselves (Vector3D ~2,000 uses outside stuff/, Point3D, the matrices, UnitQuaternion, angles) and every op whose
callers are Stuff-internal. Next: port by TYPE bottom-up (a header batch per type converting members to glm with the bridge at the boundary),
not by op: op-by-op porting cannot delete a type.

## 8. Numerical contract (lane 1 s22, orchestrator addition 2026-10-17)

- WHAT "bit-identical" promises: the same exe repeats itself, across job-system worker counts and across save/load (SIMID chain,
  saveload digests). It does NOT promise identity across compilers (MSVC vs clang-cl presets) or platforms.
- PINNED in `mclib/math/glm_config.h` (included by `mc2_math.h`; compile-time, fails the build on change): GLM 1.0.1, scalar
  (`GLM_CONFIG_SIMD == GLM_DISABLE` via GLM_FORCE_PURE), no aligned gentypes, no forced intrinsics/FMA.
- PINNED per command line (s23, replaces the s22 `_M_FP_*` preprocessor guard, which ran only in TUs that included GLM and was
  bypassed under clang-cl): `cmake/Mc2SimFpContract.cmake` adds `/fp:precise` to every first-party TU (root project after the vendored
  subdirectories, every standalone project through Mc2Standalone.cmake) and under clang-cl `/clang:-ffp-contract=off` (clang-cl's
  `/fp:precise` alone means `-ffp-contract=on`, verified with `clang-cl -###`); configure fails when the global flags carry
  `/fp:fast|contract|strict`, `-ffast-math`, `-ffp-model=fast|strict` or `-ffp-contract=fast|on`. `scripts/check-sim-fp-contract.py`
  (check-contracts row `sim_fp_contract`) checks every code/, mclib/ and GameOS/ entry of the compile database (last `/fp:` is precise,
  no forbidden flag, clang-cl last `-ffp-contract=` off) and the CMake sources; `--selftest` and the pytest are the rejection probes.
- Retained exact wrappers (mc2_math.h), one line each:
  - `transformPoint/transformVector`: row-vector p*M (Stuff convention); glm's `M*p` is the column convention, a different product.
  - `divide(v, s)`: one float reciprocal then three multiplies, as Stuff::Vector3D::Divide; glm `v / s` divides each component (1 ULP).
  - `lerp(a, b, t)`: a + t*(b-a) per component; glm::mix is a*(1-t) + b*t, which cancels differently.
  - `wrapAngleStatic`: double fmod into (-Pi, Pi] (-Pi -> +Pi), as Radian::Normalize(Scalar); no glm equivalent with that interval.
  - `wrapAngleMember`: as above into [-Pi, Pi] (-Pi stays), as Radian::Normalize(); the two intervals differ on purpose.
  - `sinD/cosD/acosD/atan2D`: the DOUBLE CRT function rounded to float, which is what Stuff::Sin/Cos/Arccos/Arctan bind in the non-PCH
    libraries (stuff, mlr, gosfx, editor); glm::sin(float) is sinf, 1 ULP apart on ~0.1% of inputs.
