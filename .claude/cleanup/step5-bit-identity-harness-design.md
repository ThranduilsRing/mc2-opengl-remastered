<!-- doc-status: current | reviewed:2026-10-15 -->
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
