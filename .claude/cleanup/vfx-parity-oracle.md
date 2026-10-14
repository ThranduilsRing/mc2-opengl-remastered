# G2 mission-start crash repair after ff6ba111c

Root cause: **mixed gosFX C++ ABIs in the deployed executable**. G2 inserted an
unused borrowed stream pointer into Effect and ExecuteInfo, enlarging every derived
cloud. The crash binary linked newly compiled Effect/CardCloud against stale
ParticleCloud/SpinningCloud objects. This is not evidence of a new allocator pairing
or a particle birth buffer owner. No new particle allocation was added by G2.

Read-only evidence retained under `E:/mc2-archive-2026-10/`:

- `vfx-oracle-g2/post/mc2_03/a/mc2_03.log` prints startup fingerprint ff6ba111cfad,
  but other linked translation units and the crash provenance retain 5636f22bc357.
  Its manifest records SOURCE-HEAD-MISMATCH. A current startup fingerprint alone
  did not establish a coherent rebuild.
- The crash dump's CodeView GUID is C618F90B-5C97-4288-89D5-0AC4CF8FD7AB, age 5,
  exactly matching `build64/RelWithDebInfo/mc2.pdb` (93,409,280 bytes). That PDB
  contains both Effect sizes 208/216, ParticleCloud 232/240 and CardCloud 264/272.
- In `build64/out/mclib/gosfx/gosfx.dir/RelWithDebInfo`, Effect/CardCloud objects
  date from 22:15:38; ParticleCloud/SpinningCloud and the other gosFX objects date
  from 12:34. Disassembly shows the old ParticleCloud constructor storing its
  DynamicArray at `this+0xd8`, with length at `this+0xe0`. The new CardCloud
  destructor destroys the array at `this+0xe0`: it frees the old array length as
  an address. The new CardCloud constructor also derives its channel pointers
  from that wrong location. See `g2-lifecycle-inspect.log` and
  `g2-lifecycle-pdb-layout.log`; the matching minidump remains in the orchestrator's
  lane3 crash bundle `crash_2026-10-13_221715`.
- `STOP[PACKET_OOB] packet=0 maxPackets=0` also occurs in the passing pre-G2
  mc2_03 log. It is not specific evidence for this regression. The sim-determinism
  manifest's env delta is precisely MC2_DETERMINISTIC_RNG=1,
  MC2_SMOKE_FIXED_TIMESTEP=1 and MC2_SIM_IDENTITY_TRACE=1; no extra fixture env
  appears there. Those settings alter the seed/mission progress, not class layout.

Repair within the claim: remove the unused Effect/ExecuteInfo stream fields and
propagation API, restoring the pre-G2 layouts and legacy birth sequence. Keep the
standalone Stuff stream value type, reset/state/count probe, and CardCloud's live
particle draw path and RNG audit. All birth transcripts now explicitly say legacy;
the oracle cannot infer a presentation owner. The presentation stream is still
**not wired**: implementing the RenderWorld session owner/reset/save-load lifecycle
requires the owner files listed below. No global, side table, allocator workaround,
GPU promotion or MLR removal is introduced. The prior injection description below
is superseded by this repair.

Regression: the real gosFX/MLR/particles/Stuff source closure is linked into a
game-free doctest executable. Its platform sinks reuse the existing mc2fx shim;
the RNG sink uses the same deterministic kernel as gos_rand, with state owned by
the test fixture. Fatal STOPs abort. The normal mc2_tests corpus runs this child
in two fresh hidden processes: MC2_GPU_PARTICLES=0 and =1, with oracle/tube/shape
and GPU-sim gates off. Each uses the three sim-determinism gates above and the
mc2_03 mission seed. Tests assert the pre-G2 ABI sizes (including ExecuteInfo=40),
then construct real CardClouds at capacities 1/32/128, start and birth particles,
and expire/delete them through the real parent Effect::Execute child-removal path.
The GPU arm redraws each live frozen state eight times, including producer Flush,
and compares the full legacy RNG table/index/count after each iteration. The ABI
assertion would reject the G2 pointer addition; a coherent fresh lifecycle test
alone would not reproduce a mixed-object executable. The renderer bridge is a
sink, so this test does not establish GL rendering, culling or visual parity.

Proof: native corpus passed 3,113 cases (3 skipped), plus the four Python contract
harnesses; the final wrapper recorded raw Python rc=0 in 78s. Its ten intentional
expected-failure assertions are marked as such by doctest, which ends SUCCESS.
The release-tree harness still skips installed-tree validation without a
release root. The full ASan corpus **failed**, 3,112 passed / 1 failed / 3 skipped,
at `class_light_band_harness/mission_sh_map_is_functionally_but_not_literally_empty`,
`tests/unit/test_class_light_band_harness.cpp:1424`: 24 pointer-identity assertions.
The same lighting case fails alone (53 assertions, 24 failed) without running the
VFX driver. This is not a full-green ASan checkpoint. It needs **lane 2 / RenderCore**
and **lane 1 / unit-test** ownership: `RenderCore/IblShCoeffs.h:35` and
`RenderCore/IblShRegistry.h:38` define internal-linkage constexpr tables, while
`:90` exposes an external inline lookup returning a reference. Separate table
addresses under ASan are consistent with that linkage problem; no lighting files
were edited. A pre-G2 ASan run was not available, so no pre-existing-failure claim
is made solely from this isolation result.

The new VFX driver passes alone in the ASan mc2_tests binary (8 assertions), and
the real lifecycle executable passes both arms natively and under ASan: 2 cases,
31 CPU assertions / 55 GPU assertions, including 24 frozen-draw/Flush state checks.
Strict child ASAN_OPTIONS are detect_leaks=0:halt_on_error=1:print_stacktrace=1.
The instrumented executable imports clang_rt.asan_dynamic-x86_64.dll and ASan
load/store/allocation hooks. No ASan memory diagnostic occurred in these probes.
Oracle pytest: 71 passed. RNG probe: compile/run rc=0; known sequence, reset,
restore, count, range and legacy isolation passed. CRLF C++ and LF docs/build/test
registration retained; the CRLF-aware whitespace check passes. No compiler timeout
or C1041 occurred in these unit builds. Early test integration attempts failed for
GetHead/BeginFrame (wrong test API names) and a missing Psapi link, then were fixed.

Exact proof commands, all from A:/Games/mc2-s3:

```powershell
py -3 tools/build_contract_harnesses.py --run --unit
py -3 tools/build_contract_harnesses.py --run --unit-only --asan
py -3 E:/mc2-archive-2026-10/g2-lifecycle-proof.py
py -3 E:/mc2-archive-2026-10/g2-lifecycle-run.py
py -3 E:/mc2-archive-2026-10/g2-lifecycle-run.py --asan
py -3 E:/mc2-archive-2026-10/g2-lifecycle-isolate.py
py -3 -B tools/vfx_oracle/probe_rng.py --out E:/mc2-archive-2026-10/g2-lifecycle-rng
py -3 -c "import tempfile,pytest; tempfile.tempdir='E:/mc2-archive-2026-10'; raise SystemExit(pytest.main(['--capture=sys','-p','no:tmpdir','-p','no:cacheprovider','tools/test_capture_vfx_oracle.py','tools/test_vfx_rng_evidence.py','tools/test_vfx_parity_oracle.py']))"
py -3 E:/mc2-archive-2026-10/g2-lifecycle-inspect.py
& 'C:/Program Files/LLVM/bin/llvm-pdbutil.exe' pretty --native --classes --class-definitions=layout '--include-types=^gosFX::(Effect|ParticleCloud|SpinningCloud|CardCloud)$' build64/RelWithDebInfo/mc2.pdb
& 'C:/Program Files/LLVM/bin/llvm-readobj.exe' --coff-imports build64-tests-asan/RelWithDebInfo/vfx_lifecycle_tests.exe
git -c core.whitespace=cr-at-eol diff --check
```

The retained proof wrapper runs the exact native harness command with merged
stdout/stderr and records its raw Python return code. It avoids PowerShell's
NativeCommandError conversion of intentional diagnostic stderr in redirected logs.
The isolate script expects lighting rc=1 and VFX driver rc=0; its own rc=0 means
both isolated outcomes matched, not that the lighting assertion passed.

The only tests/unit change registers the explicitly requested lifecycle doctest;
all its implementation and closure definition live in tools/vfx_oracle.

The orchestrator must **recompile the complete gosFX dependency closure**, build
the committed fingerprint and launcher, deploy lane3 and rerun strict pre/post
simulation and camera proofs. Build dependency/provenance hardening needs its
build-system owner; no unclaimed production build files were edited. Baselines
were not regenerated, and no game, deployment or capture was run here. SIMID does
not cover every presentation allocation or render call; zero-frame crashes and
mixed-object binaries are outside the conclusions of a matching simulation chain.
For the clean production rebuild, the suggested orchestrator command is the real
VS CMake `--build build64 --clean-first --config RelWithDebInfo --target
build_fingerprint mc2 mc2_launcher -- /m:1`; it was not run here. Neither deployment
nor runtime simulation/camera proof was authorized for this repair turn.

Retained scratch under E:/mc2-archive-2026-10 (no files deleted):

- `g2-lifecycle-{inspect,run,proof,isolate}.py`
- `g2-lifecycle-{inspect,pdb-layout,unit,unit-final,unit-attempt3,unit-pass,unit-verified,asan,pytest,native-arm0,native-arm1,asan-arm0,asan-arm1,asan-isolated-lighting,asan-isolated-driver}.log`
- `g2-lifecycle-rng/{compile-command.json,compile.log,run.log,rng_probe.exe,rng_probe.pdb}`
- Existing build64-tests and build64-tests-asan outputs updated; build64 production
  objects/PDB and the installed crash evidence were read only.

Past failed approaches not repeated: placeholder draw-time random resimulation,
billboard/2-profile PPC substitutes, GPU promotion from effect-free captures, the
static-update-skipped force-spawn scene, and speculative May heap workarounds.
The repair follows the matching dump/object ABI evidence instead.

# Earlier G2 build repair after 91f45c0e1 (historical)

Source fix verified in the real VS Stuff project; full engine build remains **blocked by
C1041 PDB locks** after the requested single serial retry. No deployment, capture or commit.

`mclib/stuff/random.cpp` now contains the small VFX LCG directly: unsigned u32 recurrence
`state = state * 1664525u + 1013904223u`, then `(state >> 17) & 0x7fff` divided by 32768.
This preserves the existing sequence, reset/state/count and algorithm/version contract
without a Stuff dependency on the higher-layer `mclib/det_rng.h`. No include paths changed
in any production target. The standalone probe now includes `random.hpp` through
`mclib/stuff`, excluding the parent `mclib` include path that masked the original defect.

Proof commands from `A:/Games/mc2-s3`:

```powershell
& 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' --build build64 --config RelWithDebInfo --target build_fingerprint mc2 mc2_launcher
& 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' --build build64 --config RelWithDebInfo --target build_fingerprint mc2 mc2_launcher -- /m:1
py -3 tools/build_contract_harnesses.py --run --unit
py -3 -B tools/vfx_oracle/probe_rng.py --out E:/mc2-archive-2026-10/g2-91f45-build-fix-rng
git -c core.whitespace=cr-at-eol diff --check
```

Both VS commands returned 1 solely for C1041 on `mclib.pdb` and `gosfx.pdb` in other
translation units. The first compiled random.cpp and produced stuff.lib; the retry also
reported stuff.lib successfully. Neither reported C1083. The requested targets as a whole,
including the launcher, are not proved built. No further build retry or process termination.

Contracts returned 0: 3,112 doctest cases passed, 3 skipped (97 s), and all four Python
harness commands returned 0. The release-tree harness explicitly skipped real installed-tree
validation because no release root was configured. Intentional child-test failures in the
doctest log are expected; the enclosing corpus ended SUCCESS. RNG probe compile/run returned
0 with the narrowed includes, retaining the known 9340/32768 and 21221/32768 first draws
for root seed 42, reset/restore, draw-count, range and legacy-stream isolation checks.
Whitespace check passed; CRLF C++ and LF Python/report retained.

Retained scratch: `E:/mc2-archive-2026-10/g2-91f45-build-fix-vs.log`,
`g2-91f45-build-fix-vs-serial.log`, `g2-91f45-build-fix-contracts.log`, and
`g2-91f45-build-fix-rng/{compile-command.json,compile.log,run.log,rng_probe.exe,rng_probe.pdb}`
under that archive root. Existing build64/build64-tests outputs updated; no files deleted.
The earlier part-1 checkpoint below retains its broader G2 ownership and runtime gaps.

# G2 part-1 checkpoint (before 91f45c0e1): partial

Worktree `A:/Games/mc2-s3`, branch `claude/vfx-oracle`, base `5563945e0`.
No commits, index writes, deployments, game launches, particle-default changes or MLR deletion.
This checkpoint contains a reviewed scoped diff and offline proof. The orchestrator must commit,
build the VS tree and collect fingerprinted lane3 runtime evidence before acceptance.
The Q2 report below is historical evidence, not a post-change capture result.

## Current parity gap list (supersedes the historical Q2 list)

1. **Session RNG ownership incomplete.** `Stuff::Random::Stream` is an instance value with
   stream ID `0x56465852`, algorithm version 1, explicit seed/reset, state/restore and u64 draw count.
   The design's Numerical Recipes LCG produces exactly representable 15-bit fractions
   with u32 initial seed/state (`rootSeed XOR streamId`) without calling `gos_rand` or
   the legacy Stuff table. The unused Effect/ExecuteInfo borrowing API was removed in the
   crash repair above; no production birth uses this stream. The existing Stuff
   table and its startup `gos_rand` seeding remain the default; only its count/snapshot are added.
   **There is no presentation env-seed hook or mission/restart/save-load reset yet.** Adding a
   new process-global owner to hide that gap was explicitly rejected by the binding review.
   The transcript covers Effect starts, not a complete legacy particle/profile birth-random tape.
2. **Draw RNG partially removed, not globally proved.** CardCloud now always reads its live
   CPU particles under `MC2_GPU_PARTICLES=1`, including an empty-cloud no-emission case. It
   no longer calls `SpawnCardCloud` or rerolls placeholder offsets/velocities while drawing.
   Birth random values already reside in the live particle state. This changes the opt-in
   master arm's CardCloud appearance; it does not flip the shipped master default.
   `[VFX_DRAW_RNG v1]`, under existing `MC2_GPU_PARTICLES_LOG=1`, compares the full legacy
   table/index/count before/after CardCloud plus its children. The game-free regression
   above exercises frozen GPU-harvest draws and producer Flush; runtime/GL proof,
   culling and other leaf classes remain outside its scope. PointCloud/ShardCloud still call
   their random-consuming placeholder helpers. PointCloud's harvested branch also falls
   through to placeholder Spawn when its active count is zero, even with oracle-render ON.
3. **Fixture remains the existing force-fire fixture.** It fires at `nullptr` and +150 world X
   (`code/warrior.cpp:2104`, `:2155`), not at quonsets. The prior 30 m versus approximately
   48 m minimum-range analysis still applies. No legal-range three-player-mech fixture or
   weapon-family demonstration is claimed. Laser/autocannon are additional owner-scene
   requirements; they do not replace any of the oracle's `{missile, ppc, explosion, smoke}` set.
4. **No post-change runtime set or controls.** Both-arm/two-fresh-process repeats, an effect-free
   control and an independently family-suppressed control per arm remain pending. The tool
   requires eight distinct image paths, engine-log paths and process identities per graded
   family. It now also rejects missing/mismatched owner/stream/version/seed/state/count,
   effect age, actual GL/FBO backend, resolved asset hashes, birth transcript, log hash,
   algorithm identity, exact gate vector and all-draw/flush/cull frozen-state proof. The diagnostic extractor
   deliberately emits owner=null and scope=CardCloud; it cannot certify itself as complete.
5. **Strict simulation proof pending.** The standalone stream probe proves reset/restore,
   known sequence, count, range and legacy-table isolation. It does not prove engine sim
   invariance. Four baseline checks and pre/post camera-park pairs must still pass strict
   equality. No baseline was written or regenerated. The new read-only strict checker
   rejects strict-bit drift even when graded chains agree.

## Files needing owners (not edited)

- **Effects-session owner:** `RenderWorld/RenderWorld.h`, `RenderWorld/RenderWorld.cpp`,
  `GameAdapters/StaticPropRenderAdapter.cpp` (begin/end/restore at lines 66/84/92),
  `code/mission.h`, `code/mission_init.cpp:78`, `code/mission.cpp:598`,
  `code/saveload.cpp:67,531` and `code/saveload_load.cpp`. The current RenderWorld API is
  namespace/process state, not a session object. Introduce a session-owned effects stream
  and pass it through root effect construction; destroy effects before releasing the owner.
  Register the separate harness seed in `Gates/gate_table.cpp` and its generated surfaces.
  Reset on mission start/restart; restore seed/state/count/version with presentation state
  on load (or document an intentional presentation reset). Do not reseed the sim stream.
- **Whole-gosFX birth routing:** `mclib/gosfx/particlecloud.cpp`, `spinningcloud.cpp`,
  `tube.cpp`, `debriscloud.cpp` still draw from `Stuff::Random`; `effectcloud.cpp:205`
  must pass the borrowed stream into its particle-owned child Start. Root sites include
  `code/weaponbolt_update.cpp`, `code/carnage.cpp`, `mclib/bdactor.cpp`,
  `mclib/bdactor_update.cpp`, `mclib/mech3d.cpp`, `mclib/mech3d_update.cpp`,
  `mclib/gvactor_geometry.cpp` and `mclib/gvactor_update.cpp`; inventory all remaining
  ExecuteInfo callers before declaring ownership complete.
- **Remaining draw RNG:** needs lane 1 for `mclib/gosfx/pointcloud.cpp:483,583` and
  `mclib/gosfx/shardcloud.cpp:326,454`. Reuse the live harvest and make empty clouds emit
  nothing. Their claimed `mclib/particles/spawn_point.cpp` / `spawn_shard.cpp` files cannot
  receive per-instance birth state through the current spec/transform/seed/age interface.
  They and `spawn_cardcloud.cpp` are left intact, not replaced by a seeded RNG in Draw.
- **Real fixture, minimum engine patch:** needs lane 1 for `code/warrior.cpp`. Reuse an
  explicit value of existing `MC2_FX_FORCE_SPAWN` for a separate quonset-order recipe;
  select only the three home-team players and stable quonset object IDs, schedule normal
  movement to an on-map/unobstructed approximately 500-world-unit firing location, then
  inject attack TacticalOrders with a real target WID. Existing tactical orders can hold
  progress; no new global WID latch is needed. Harness/bookmark changes remain in claim.
  An ABL alternative needs the actual resolved mc2_01 mission/brain asset, hence its owner.
  Family suppression needs an owned birth/draw seam covering both GPU and MLR routes.
  `MC2_DISABLE_GOSFX` only disables MLR and is not a valid GPU effect-free control.
- **Defect ledger:** needs orchestrator for `.claude/cleanup/BUGS-FOUND-FIXED.md` when the
  render-time RNG fix is committed and runtime-proved; that file is outside this claim.

## Verified headless attack seam and destruction path

`Mover::handleTacticalOrder` (`code/mover_combat.cpp:8,148`) sends player orders to
`setPlayerTacOrder` / the existing queue; `code/warrior_alarms.cpp:134` records the pending
order. `TacticalOrder::execute` calls `orderAttackObject` at `code/tacordr.cpp:953`.
`code/warrior_orders.cpp:395,444` constructs a real target-WID attack and performs the
normal range/pursuit setup. An existing headless commander-order example is
`code/warrior.cpp:2205`; it attacks nearest enemy mechs, not buildings. The ABL seam is
`code/ablmc2_orders.cpp:299,302` (findByPartId then commander-origin orderAttackObject).
Terrain-object enumeration is already exposed at `code/objmgr.h:381,483`; stable part IDs
resolve at `code/objmgr_pick.cpp:48`. None of these files was edited.

Current damage handling invokes building destruction directly:
`code/bldng.cpp:1513` calls `playDestruction`, with generic-explosion fallback;
`mclib/bdactor.cpp:786,815` creates and starts the effect. The same hit path immediately
updates the appearance at `code/bldng.cpp:1528`. `mclib/bdactor_static.cpp:64` rejects
static eligibility while destructFX exists; `:483` includes that eligibility in IsStaticNow.
The object-manager skip at `code/objmgr_update.cpp:903,1033` depends on IsStaticNow.
Effect advancement/draw are `mclib/bdactor_update.cpp:721` and `bdactor_render.cpp:339`.
Thus the old idle-update hook failure does not prove a real attack cannot play destruction
FX today. This is source evidence; actual quonset destruction is still unverified.

## Exact experiments and evidence format

Vector order: `(GPU_PARTICLES, VFX_ORACLE_RENDER, VFX_ORACLE_TUBE, VFX_ORACLE_SHAPE,
VFX_GPU_SIM_CARDCLOUD, VFX_GPU_SIM_COMPARE)`; all names have `MC2_` prefixes.

| Experiment | MLR | GPU | Meaning |
|---|---|---|---|
| `--experiment master` (default) | `(0,0,0,0,0,0)` | `(1,0,0,0,0,0)` | Historical master toggle; this diff changes CardCloud alone to live harvesting. Other placeholder/hybrid routes remain. |
| `--experiment harvested` | `(0,0,0,0,0,0)` | `(1,1,1,1,0,0)` | Retirement design's candidate; still hybrid/unproven and PointCloud empty-state fallback remains. |

The wrapper explicitly pins every gate, records it in fingerprint checks and emits one
`<arm>-r<N>-runtime.json` per fresh process. Those diagnostic files bind raw birth records
and their transcript SHA256 to the engine-log SHA256. They do not fabricate asset,
backend, session-reset, coverage or full-pipeline RNG attestations.

For PARITY, each primary/control record additionally needs `runtime`: `process`,
`effect_age`, `rng={owner:"RenderWorld",stream:"56465852",algorithm:"LCG32-NR-next15",version:1,seed,state,draw_count}`,
`backend={requested:"gl",drawn:"gl",readback:"gl_fbo"}`, nonempty resolved `assets`
(path-to-SHA256), `birth_transcript`, `log`, `log_sha256`, and the exact six-entry `gates`
dictionary (string "0"/"1" values). Positives also need
`draw_probe={scope:"all_draw_flush_cull",unchanged:true,frozen_repeats:2}` or greater.
Declare `evidence.experiment` as `master` or `harvested`. The tool verifies the bound log
bytes and birth transcript; positives must contain injected-stream birth records. Reviewed
coverage and full-pipeline scope remain explicit attestations, not image recognition.
Controls share seed/assets/backend/gates; positive repeats and arms must also share sampled
state/count/age/birth transcript. Missing or inconsistent fields remain DIAGNOSTIC.

## Offline proof and retained pre-change images

- `py -3 -B tools/vfx_oracle/probe_rng.py --out E:/mc2-archive-2026-10/vfx-oracle-g2/rng-probe-lcg`:
  compile/run rc=0, 30 s compiler / 10 s runtime bounds; optimized debug-info probe against
  the real random.cpp. Known root-seed-42 sequence starts 9340/32768, 21221/32768. Existing
  GameOS operator-delete exception-spec warnings only. This is not the VS engine build.
- `py -3 tools/build_contract_harnesses.py --run --unit --unit-only`: SUCCESS in 392 s;
  3,112 cases passed, 3 skipped; expected intentional-failure assertions remain expected.
- `py -3 -B -m pytest tools/test_capture_vfx_oracle.py tools/test_vfx_parity_oracle.py tools/test_vfx_rng_evidence.py -q --capture=sys -p no:tmpdir -p no:cacheprovider`:
  71 passed. No collection blocker or compiler timeout.
- `git -c core.whitespace=cr-at-eol diff --check`: passed; CRLF C++ and LF Python/report
  preserved. Plain `git diff --check` flags the preserved CR bytes as trailing whitespace.
- `py -3 -B tools/vfx_parity_oracle.py tools/vfx_oracle/regions.json --out E:/mc2-archive-2026-10/vfx-oracle-g2/prechange-diagnostic`:
  expected exit 1, all four DIAGNOSTIC; historical digest pairs unchanged. All four new
  montages were opened and viewed: PPC shows mechs/status bars without a demonstrated
  beam; missile shows enormous opaque GPU strips with differing repeats; explosion is
  obscured by an opaque GPU polygon; smoke loses the dense CPU puff and has drifting GPU
  specks/strips. None demonstrates laser/autocannon or the requested all-three scene.

Three preliminary standalone clang invocations failed on missing PLATFORM_WINDOWS,
missing GameOS/gameos include path, then missing test gos_rand implementation. These
were fast compile/link setup failures, not timeouts. The final bounded probe fixes those
inputs and passes; no production stubs were added. CIM process introspection was denied;
no process was killed. No Git Bash/contracts, VS engine build, build_fingerprint,
deployment, new captures, DEPLOY_FINGERPRINT runtime proof or sim pairs were run here:
the binding checkpoint reserves commit/build/deploy/runtime acceptance to the orchestrator.

## Orchestrator proof commands (not executed here)

Before and after the commit, retain runs with identical seed, fixture, asset bytes,
render gates and camera inputs. Use fresh `pre`/`post` output roots; never write baselines.
Each stage runs these four baseline checks (check prints a strict-drift warning but may
exit zero; inspect it and run the strict checker afterward):

```powershell
py -3 tools/sim_determinism.py check mc2_03 --install lane3 --duration 60 --out E:/mc2-archive-2026-10/vfx-oracle-g2/pre/mc2_03
py -3 tools/sim_determinism.py check mc2_10 --install lane3 --duration 60 --out E:/mc2-archive-2026-10/vfx-oracle-g2/pre/mc2_10
py -3 tools/sim_determinism.py check mc2_24 --install lane3 --duration 60 --out E:/mc2-archive-2026-10/vfx-oracle-g2/pre/mc2_24
py -3 tools/sim_determinism.py check mc2_16 --install lane3 --duration 60 --out E:/mc2-archive-2026-10/vfx-oracle-g2/pre/mc2_16
py -3 tools/sim_determinism.py run mc2_24 --install lane3 --duration 60 --env-a MC2_CAMERA_PARK=-1500,2500,0,60,800@300 --env-b MC2_CAMERA_PARK=-2000,2900,90,50,600@300 --out E:/mc2-archive-2026-10/vfx-oracle-g2/pre/camera
```

After orchestrator commit, the VS build/deploy and post-change capture commands are:

```powershell
& 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' --build build64 --config RelWithDebInfo --target build_fingerprint mc2 mc2_launcher
py -3 scripts/deploy_payload.py A:/Games/mc2-opengl/mc2-win64-lane3 --allow-new-target
py -3 scripts/capture_vfx_oracle.py --plan tools/vfx_oracle/capture-plan.json --experiment master --out E:/mc2-archive-2026-10/vfx-oracle-g2/post/master
py -3 scripts/capture_vfx_oracle.py --plan tools/vfx_oracle/capture-plan.json --experiment harvested --out E:/mc2-archive-2026-10/vfx-oracle-g2/post/harvested
```

Repeat the five sim commands with `/pre/` replaced by `/post/`. For each baseline mission,
substitute its name for mc2_03 in this exact strict proof (four retained independent logs):

```powershell
py -3 -B tools/vfx_oracle/check_strict_sim.py tests/smoke/sim_determinism/mc2_03.json E:/mc2-archive-2026-10/vfx-oracle-g2/pre/mc2_03/a/mc2_03.log E:/mc2-archive-2026-10/vfx-oracle-g2/pre/mc2_03/b/mc2_03.log E:/mc2-archive-2026-10/vfx-oracle-g2/post/mc2_03/a/mc2_03.log E:/mc2-archive-2026-10/vfx-oracle-g2/post/mc2_03/b/mc2_03.log --install lane3
py -3 tools/sim_determinism.py compare E:/mc2-archive-2026-10/vfx-oracle-g2/pre/camera/a/mc2_24.log E:/mc2-archive-2026-10/vfx-oracle-g2/post/camera/a/mc2_24.log
py -3 tools/sim_determinism.py compare E:/mc2-archive-2026-10/vfx-oracle-g2/pre/camera/b/mc2_24.log E:/mc2-archive-2026-10/vfx-oracle-g2/post/camera/b/mc2_24.log
```

Also repeat the camera pair with `--env MC2_DISABLE_GOSFX=1` in both stages as the
design's MLR-off diagnostic; that switch does not suppress the GPU routes. Inspect each
fingerprint verdict and strict comparison, not command exit alone. Same-process duplicate
bookmark poses supply the frozen CardCloud probe samples, not the fresh-process repeats.
Collect effect-free/family-suppressed controls only after valid cross-route hooks exist.
Create a new regions manifest with the post paths, reviewed demonstrations and bound
runtime records; run `vfx_parity_oracle.py <manifest> --out <fresh-montage-dir>`, then view
every montage. Missing ownership, coverage or controls must keep its result DIAGNOSTIC.

SIMID limits: current v1 includes movers, sampled building/turret fields, ABL instance
records/statics and selected RNG accounting (`code/sim_identity.cpp:164,202,332` and
`mclib/sim_identity.h:307,357`). It is not the complete authoritative kernel digest in the
sim design: pending execution/ingress/path state, all script/media state and presentation
particles are not all represented. Installed data fingerprints hash archives by content
but loose trees by path/size (`tools/sim_determinism.py:68`); pin resolved asset SHA256s
and options separately. Strict equality is necessary, not a proof of every unrecorded field.

## History not repeated; scratch retained

No billboard-per-profile/PPC-head substitutes or gpu_trail ring restamping (wrong primitive),
age-zero/midlife placeholder rebaking (discarded live curves), CPU curve cache or GPU sim
offload (measured poor payoff), idle-golden/default-ON promotion (missing combat evidence),
or quonset idle-update-hook spawning (historical static-skip obstacle). No Tube geometry
change is designed here; the CPU profile/UV and RenderDoc interpretation investigation is
still required before any Tube replacement. The CardCloud harvest itself reuses the
already-existing CPU-state route; the difference is removing its RNG-consuming fallback.

Retained scratch: `E:/mc2-archive-2026-10/g2-rng-probe.exe`; under
`E:/mc2-archive-2026-10/vfx-oracle-g2/`, `prechange-diagnostic/{digests.json,ppc.png,missile.png,explosion.png,smoke.png}`
and `rng-probe-lcg/{compile-command.json,compile.log,run.log,rng_probe.exe,rng_probe.pdb}`.
The same five files under `rng-probe/`, and the standalone root `g2-rng-probe.exe`, are
superseded pre-LCG scratch, not the final proof. The design's LCG requirement was caught
in the final design re-read and replaced the preliminary algorithm before delivery.
Existing `build64-tests` configure/build outputs were updated by doctest. No files deleted.
Faster next batch: authorize the exact owner surfaces up front; provide captured family
suppression hooks and stable scene target IDs; retain pre-change strict logs before the
new build; provide a standalone native RNG test seam instead of engine-header setup probes.

# Q2: GL VFX parity oracle (historical)

Status: **DIAGNOSTIC / PARITY not accepted** for each of missile/LRM trail, PPC,
explosion and smoke, and for the complete evidence set. All twelve final captures were
collected, fingerprint-checked, cropped and viewed. GPU missile/smoke repeats fail;
PPC beam/LRM trail coverage is unproven; no effect-free or suppressed-control set exists.
No particle promotion or MLR deletion is authorized by these results.
Worktree `A:/Games/mc2-s3`, branch `claude/vfx-oracle`, executable commit
`5636f22bc357414fa75ca15c8fa52209995cae21`, clean engine fingerprint. Install:
`A:/Games/mc2-opengl/mc2-win64-lane3` only. No engine files changed; no index writes or commits.

The binding MLR design is absent from this checkout. I read the complete local Git version with
`git show d4895e56d:.claude/cleanup/mlr-retirement-design.md`, plus the current
`sim-kernel-design.md` presentation/VFX rows. Q2 calls the oracle G batch 1;
the design's migration table calls the oracle **batch 0** and the gameplay/RNG boundary **batch 1**.
Batch numbers below refer to that table, not a renumbered plan.

## Capture recipe and limits

`scripts/capture_vfx_oracle.py` calls the unchanged `scripts/run_visual_capture.py` harness.
It acquires the ordinary host lease in the same process **before** the harness starts its deadline.
The direct runner otherwise starts its clock before `nofocus.spawn` waits for the lease:
one diagnostic run spent 128 seconds queued and produced no PNG. Neither lane-owned runner was edited.

Each arm uses two separate fresh engines (`r1`, `r2`), 30-second duration, fixed 30 Hz,
1920x1080, eight settle frames, `MC2_SMOKE_FREEZE_ANIM=1`, GL FBO readback,
`MC2_DETERMINISTIC_RNG=1`, `MC2_RNG_SEED=0xC0FFEE`, and the existing force-fire fixture.
MLR pins `MC2_GPU_PARTICLES=0` (equal to the shipped default false); GPU changes **only** that master to `1`.
Earlier diagnostics left the master unset, also false on this executable.
The harness scrubs ambient MC2 settings. Oracle render/tube/shape and GPU simulation gates remain unset/default OFF.
This measures the requested master toggle, not the fully harvested candidate described for future promotion.

Final recipes park the camera at the bookmark from frame zero with `MC2_CAMERA_PARK`.
Earlier diagnostic captures moved it only after freezing the sim, confounding pre-birth visibility.
Bookmark duplicates are the same pose within an engine, required by the capture iterator;
they are **not** the same-arm repeat controls.

There is a deterministic **indirect** seed path: startup `gos_srand` at `code/mechcmd2.cpp:378`
precedes `Stuff::InitializeClasses` at `:1007`; `mclib/stuff/random.cpp:54` fills its separate
250-word table from `gos_rand`. gosFX births use that table (`mclib/gosfx/effect.cpp:552`,
`:684`; `mclib/gosfx/particlecloud.cpp:570`; `mclib/gosfx/tube.cpp:1039`).
Mission reseeding (`code/mission_init.cpp:78`) does not reset the Stuff table.
The sidecar's `seed` is the smoke seed, **not** a gosFX stream state/draw-count record.
The existing gate also affects sim RNG; it was pinned equally in both arms, never changed in engine code.
No isolated presentation-only seed/reset hook exists. Adding it would require the expressly forbidden
`mclib/` and likely `Gates/` surfaces. The design's RenderWorld VFX stream (`0x56465852`),
initial/state/u64 draw count and render-consumes-zero-RNG proof remain unimplemented.

## Bookmarks and content

New fixture-authored data: `tools/vfx_oracle/bookmarks/mc2_24.json` frames the force-fire lance;
`mc2_01.json` widens the existing `tests/visual/bookmarks/mc2_01_combat.json` Werewolf pose;
`mc2_10.json` frames stock Zeus/Highlander spawn positions and their +150 X targets.
These are not user-authored saved poses. `covers` stays empty until a visible class is confirmed.
`mc2_10` coordinates were read from lane3's `mission.fst`; its loose mission copy matches the extracted bytes.
The loose FX library and effect CSV also match their FST payloads.

Resolved lane3 input SHA256:

| Input | SHA256 |
|---|---|
| `data/effects/mc2.fx` | `0fc3f12eec5245a7544429c93fc891f0f770a4131cd4a8ca95616bf4b5361b3d` |
| `data/objects/effects.csv` | `d07337e581382a60aab6aa97668b351f0e769b3cb5489dbbc1f640dfdbdf4aef` |
| `data/missions/mc2_10.fit` | `885244171387ccdf64f1414de2d2009ac67bf35002fb7fe689342f1d0f9c656b` |
| `data/missions/mc2_24.fit` | `d554a3948e9df993c19baec80868fbb03bff835f471096b467c58d121ed10341` |
| `data/missions/mc2_01.fit` | `95c715176dd664c68f1d04f386e954ef296724a4e0919bf33fa4e43dbef2d958` |
| `options.cfg` before final samples | `438d8223767e5ece50e6d02242605080049057d74d02c1d30dfe160be0b37b80` |
| `data/missions/gamesys.fit` | `b52918ce5b2a3342e5d06b9c3942b0b50b784f46f89ac9a0d4c367bfdf17dcf7` |
| `data/objects/compbas.csv` | `f95a229dc777aef4bf6f261713c1eb56ee5da5e9ba0a86f312d36656096e93de` |
| `data/objects/object2.pak` | `dda3fd6c8e03e21fb88dd409fba3afd355f2f58fd34c9801ea9ab17a1f0d2488` |

Each capture has an engine sidecar and harness manifest recording effective environment, options,
camera, frames, actual GL backend/FBO readback and binary provenance. This is stock-scene evidence,
not a stock/mod leaf inventory or a content-wide migration acceptance.

## Digest method and reviewed diagnostic

`tools/vfx_parity_oracle.py` hashes decoded RGBA8 crop pixels:
SHA256 of little-endian u32 width, little-endian u32 height, then row-major RGBA bytes.
Rectangles are half-open `[x0,y0,x1,y1]` at 1920x1080. It rejects out-of-bounds/empty crops,
different source resolutions, and duplicate resolved source paths used as repeat controls.
Its exit code is zero only when all four families receive **PARITY**. Repeatability alone
receives **DIAGNOSTIC**, even when all same-arm hashes match. Current evidence exits 1.
Every montage contains MLR1, MLR2, GPU1, GPU2 and absolute RGB difference MLR1/GPU1.

### PARITY evidence contract (Q2-fix)

Each family must have two MLR and two GPU captures, exact same-arm repeatability and
exact MLR/GPU decoded-pixel equality. Each primary capture needs a recorded visible-family
demonstration and capture provenance (fingerprint, bookmark, frame). Coverage is a reviewed
manifest attestation: the tool measures pixels but does not recognize beams, trails or smoke.
An effect-free control and an independent effect-suppressed run are required for each arm,
with matching fingerprint/bookmark/frame and full source resolution. All eight paths must
be distinct. A positive crop must differ from its effect-free control in **more than 16 pixels**,
each with maximum absolute RGB-channel difference **greater than 8** (8-bit channels).
The suppressed run must **FAIL** this same presence test in both arms. A negative run
that still passes presence rejects PARITY; a suppressed run submitted as a positive also fails.
These fixed thresholds reject empty/noise-only regions; they do not identify an effect.

The existing `images` and `rect` fields remain unchanged. Optional `evidence` has:

- `captures.mlr` and `captures.gpu`: two records each, ordered like `images`; each record
  has `fingerprint` (12-64 hex digits), `bookmark` (nonempty pose identifier), `frame`
  (nonnegative integer), and `demonstration` (nonempty reviewed visible-family observation).
- `effect_free.mlr` and `effect_free.gpu`: records with `image` and the same provenance.
- `suppressed.mlr` and `suppressed.gpu`: records with `image`, the same provenance and
  `effect_suppressed: true`, documenting the separately captured suppression run.

Every output row includes `status`, `parity_accepted` and `parity_failures`. Complete
evidence also records measured changed-pixel counts and the negative-control verdict.
The summary requires all four families; a partial set cannot exit successfully.
No coverage or control attestation has been added to today's fixture.

Diagnostic PPC scene: `E:/mc2-archive-2026-10/vfx-oracle-q2/ppc`, frame 164,
loop fixture, camera moved at capture trigger. Region `[650,230,1300,600]`.
I viewed `E:/mc2-archive-2026-10/vfx-oracle-q2/montages-ppc-diagnostic/ppc.png`:
five mechs are static; GPU muzzle glows differ between repeats, and broad translucent patches
appear around them. There is no distinct blue-white PPC beam. **Coverage unproven; repeatability fails.**

| Arm | Repeat 1 digest | Repeat 2 digest |
|---|---|---|
| GL MLR | `61214acf6097c8a14a77796a195a18061f75af38028d531796eef14f634d0ec3` | `61214acf6097c8a14a77796a195a18061f75af38028d531796eef14f634d0ec3` |
| GL GPU | `64e79eb78dcfc4bbbfa13b30e16565eb936099d15643918a1ec80741c938cc07` | `d6d1d631e373d47fae522acba02cf202f34fb8293918a144420c1818ba5acc66` |

All four sidecars have sim frame 163, scenario time 5.433, strict mover digest `d6b1ffa7e4a3ec6d`
and visual mover digest `a07eb70ad5727122`. These are existing mover digests, not a complete authoritative sim proof.
Even inside one frozen GPU run, the identical second pose changes the region digest:
GPU1 `0d125dc25d410e0f...`, GPU2 `fea981fee995d745...`; all four MLR same-pose samples match.
Thus the sampled GPU effects still change with render frames while the sampled mover state is constant.

## PARITY GAP LIST

1. **Repeatability:** all four MLR region repeat pairs match; GPU missile and smoke drift.
   GPU PPC crop repeats match without a demonstrated beam. GPU explosion equality is
   occlusion, not a parity or complete explosion-coverage proof.
2. **Coverage and controls:** PPC crop has no demonstrated beam; missile crop has no
   demonstrated LRM trail. No effect-free pixel-difference proof or independent suppressed
   negative-control set exists. Every family therefore remains **DIAGNOSTIC**.
3. **Observed geometry/opacity:** giant opaque polygons and reduced smoke silhouettes
   are observed. The cause is **unproven**; no packet/resource diff identifies the exact
   size/material failure. Do not accept an occluded crop as equality evidence.
4. **Draw-time placeholder randomness:** Draw calls placeholder Spawn at
   `mclib/gosfx/cardcloud.cpp:747`, which consumes draws at
   `mclib/particles/spawn_cardcloud.cpp:194`, `:208`, `:233`.
   The exact first divergent draw is not instrumented/proven. The next batch needs a
   presentation-owned seed/reset/state/draw-count at `mclib/stuff/random.cpp:44`, `:84`,
   wired to births at `mclib/gosfx/effect.cpp:552`, `:684`. Reseeding alone cannot fix
   render-time draws: Draw/flush must consume zero RNG and use frozen live effect/packet state.
5. **Fixture range:** the force-fire point is +150 world X (`code/warrior.cpp:2170`; loop `:2119`).
   `GameObject::distanceFrom` converts this to 30 metres (`code/gameobj.cpp:1928`,
   `mclib/terrain.cpp:21`). Lane3 gamesys `[WeaponRanges]` sets Long min=60;
   cell-diagonal buffer is about 12.07 m (`mclib/move.cpp:714`, `mclib/terrain.cpp:33`,
   `mclib/terrain.h:50`). Thus 30 < 47.93 m: `Mover::weaponInRange` rejects it
   (`code/mover_combat.cpp:658`), and `BattleMech::fireWeapon` returns at `code/mech_weapons.cpp:117`.
   Clan ER PPC is Long in the resolved compbas CSV (component ID 154, physical line 156), as are LRM racks.
   This explains why a nonzero aggregate weapon count is not PPC/LRM coverage.
   Replace this 30 m fixture with an **on-map unobstructed ~100 m (~500 world units)**
   target and verify actual PPC and LRM shots in-frame in the next capture batch.
   No range/gameplay rule was changed to manufacture a beam in this task.
6. **Hybrid routes:** tube and shape routes remain partly MLR. Tube has a separate
   default-OFF gate; ShapeCloud/DebrisCloud/PertCloud remain MLR. Master-toggle results
   cannot authorize GPU promotion or MLR deletion, even after the oracle accepts a crop set.

## Final per-family regions, digests and viewed results

Source paths are sealed in `tools/vfx_oracle/regions.json`; full results are
`E:/mc2-archive-2026-10/vfx-oracle-q2/montages/digests.json`.
`H ×2` below is the explicit pair `(H,H)` from separate fresh engines.
These are intended-family regions; all four statuses are **DIAGNOSTIC**, with
`parity_accepted=false`. They are **not** a passing four-family coverage set.

| Family | Scene / trigger / region | GL MLR vs GL GPU result | Montage viewed |
|---|---|---|---|
| PPC | mc2_24 / 154 / `[650,230,1300,600]` | Both arms repeat in this crop; no distinct PPC beam in either. Tiny actor/lighting differences remain. Coverage unproven; no parity verdict. | [ppc.png](E:/mc2-archive-2026-10/vfx-oracle-q2/montages/ppc.png) |
| Missile | mc2_10 / 156 / `[700,180,1250,590]` | MLR shows actors and a small black blob, without a clear swept missile trail. GPU adds huge opaque triangles/strips crossing actors; their arrangement changes between repeats. Coverage unproven and repeat control fails. | [missile.png](E:/mc2-archive-2026-10/vfx-oracle-q2/montages/missile.png) |
| Explosion / impact | mc2_10 / 184 / `[680,470,850,620]` | MLR shows a layered black cloud/square cluster; GPU covers it with an opaque polygon. No resolved flame/debris sequence. Crop repeat equality is occlusion, not parity or complete explosion coverage. | [explosion.png](E:/mc2-archive-2026-10/vfx-oracle-q2/montages/explosion.png) |
| Smoke / impact | mc2_10 / 184 / `[850,240,1000,400]` | MLR's dense dark layered puff is reduced to sparse specks under GPU, with unrelated giant dark strips over it. GPU specks/strips change between repeats. Shape mismatch; repeat control fails. | [smoke.png](E:/mc2-archive-2026-10/vfx-oracle-q2/montages/smoke.png) |

I opened and inspected **all four** linked final montages, plus the earlier PPC diagnostic montage.
The impact GPU log reports `Smoke` and `Fireball` Spawn probes **before** the capture trigger
(`final/impact/gpu/r1/mc2_10_capture.log:303`, `:309`, trigger `:315`). This establishes live leaves,
not their exact per-region parent class or a valid flame/debris silhouette.

| Family / arm | Digest pair |
|---|---|
| PPC / MLR | `5f2ec57541622357c30424201069f233c38e48ecc7ced2d1867aaaa316662af0` ×2 |
| PPC / GPU | `220538057c3184d74d4c61565903447f77fdcf6c4d99ffdad621cacfb482232b` ×2 |
| Missile / MLR | `9154c43a5912427bb0133f5c1901004263726c3af531d4cbc31550da9f6ffcbd` ×2 |
| Missile / GPU | (`f5321e447d7782fe9b04ffc6f28d13717863a2fcba075e3b47a0d455ed248d34`, `6bc4016755620fbb3d89fa120afec044d596a83817ba4587f0b53a6efb3ddfa0`) |
| Explosion / MLR | `278449240335719447a85dc6ea80e96b488e2137a4547f7aee022fe2c75fa963` ×2 |
| Explosion / GPU | `fb0ceaf5648f4bf50266c8b147a45d025b189bb8c3ce06a5a44a22eb89089b27` ×2 (occluded) |
| Smoke / MLR | `43054a40a48308e5249b76c64ee9938ca8b9b2d681f8fb36c67f73ae6194c80e` ×2 |
| Smoke / GPU | (`435c12c48bc36a5bb177d21eea5ab29808fd44ab0c1609422d952b58b1092ed2`, `361e6b234bebb61bf02e6d12f15f193595484a1963bf85df14c1571207a1b7ba`) |

All four primary sidecars within each scene agree on camera/options/GL FBO readback and the following mover state:

| Scene | Sim frame / scenario seconds | Strict mover digest |
|---|---|---|
| PPC | 153 / 5.100 | `7aa10ae95c424426` |
| Flight | 155 / 5.167 | `1a9e2147cdde31ef` |
| Impact | 183 / 6.100 | `159b5a034da6cb2d` |

The primary full-frame controls also fail for GPU flight and impact; cropping has not concealed that failure.
The seeded startup path and animation freeze are **insufficient** for this GPU master-toggle oracle.
What still changes is the rendered placeholder geometry/specks at constant sampled sim state.
An isolated presentation seed hook and a frozen live effect-state/packet source are still needed;
the forbidden lane-owned engine surfaces prevent completing that part here.

## Rerun commands

Use a fresh output directory so earlier evidence remains intact:

```powershell
py -3 scripts/capture_vfx_oracle.py --plan tools/vfx_oracle/capture-plan.json --out E:/mc2-archive-2026-10/vfx-oracle-q2/rerun
```

Copy `tools/vfx_oracle/regions.json` to `E:/mc2-archive-2026-10/vfx-oracle-q2/rerun/regions.json`.
In that copy replace `/final/` in the image paths with `/rerun/`, then:

```powershell
py -3 tools/vfx_parity_oracle.py E:/mc2-archive-2026-10/vfx-oracle-q2/rerun/regions.json --out E:/mc2-archive-2026-10/vfx-oracle-q2/rerun/montages
py -3 -B -m pytest tools/test_capture_vfx_oracle.py tools/test_vfx_parity_oracle.py -q -p no:tmpdir -p no:cacheprovider
```

The capture plan intentionally retains the failing stock fixture for reproduction; a successful
PPC/missile oracle needs the legal-distance fixture correction above before blessing coverage.
Each scene saves exact harness argv and `fingerprint-checks.json`; all twelve final entries are
`DEPLOY_FINGERPRINT OK: exe sha=5636f22bc357 matches worktree HEAD` with `failed=false`.

## Verification and remaining work

VS RelWithDebInfo build of `build_fingerprint mc2 mc2_launcher`: passed (existing compiler/link warnings).
`deploy_payload.py A:/Games/mc2-opengl/mc2-win64-lane3 --allow-new-target`: passed,
865 payload files verified, manifest source `5636f22bc357`. It reported absent optional terrain layers
and existing diagnostic cook byproducts; no other install was changed.
Every completed wrapper capture prints `DEPLOY_FINGERPRINT OK` for each repeat.
Focused pytest: 26 passed after review fixes. Read-only cleanup review: ACCEPT;
duplicate-source proof hole fixed and fingerprint negative cases added.
These are the original Q2 verification results; Q2-fix results are recorded below.

`scripts/check-background-launch.py`: failed in unchanged `scripts/tests/test_board_selftests.py`
(40 raw child spawns and missing direct-spawn registration); no finding names a new oracle file.
`git diff --check`: passed. No Git Bash/check-contracts, index writes or commits are possible under this task's sandbox.
`build_all_targets.py` was not used (brief identifies its Ninja probe stall); no C++ changed, so no new doctest/build-contract harness run.

Final plan command above (output `.../final`): completed, exit 1 for GPU repeat drift.
Actual region command (`py -3 tools/vfx_parity_oracle.py tools/vfx_oracle/regions.json --out E:/mc2-archive-2026-10/vfx-oracle-q2/montages`):
completed, exit 1 for GPU missile/smoke crop drift.
The four MLR region repeat pairs match; GPU PPC matches; GPU explosion is an occluded equal crop.
Therefore the required same-arm-equality proof is **not green**.
No RenderDoc pass proof, negative-control set, VK proof, mod coverage, or promotion verdict is claimed.
All scratch/capture artifacts are retained under `E:/mc2-archive-2026-10/vfx-oracle-q2/`;
the preliminary `24-mlr`, failed `24-f164-mlr`, `ppc`, `ppc-birth`, `volley`,
`montages-ppc-diagnostic`, and final `final/{ppc,flight,impact}` plus `montages` remain for audit.
Automatic Python scratch retained: `tools/__pycache__/vfx_parity_oracle.cpython-314.pyc`
and imported helper bytecode under `tools/__pycache__/`, `tools/mc2x_import/__pycache__/`,
and `scripts/__pycache__/`. Existing `build64` outputs were rebuilt; no scratch was deleted.
[inventory.json](E:/mc2-archive-2026-10/vfx-oracle-q2/inventory.json) lists all 186 retained capture,
log, manifest, command, fingerprint, digest and montage files with byte sizes and SHA256s;
the inventory file itself is retained alongside them.

## Q2-fix verification (status correction only)

No engine changes or new captures. Focused oracle pytest:

```powershell
py -3 -B -m pytest tools/test_capture_vfx_oracle.py tools/test_vfx_parity_oracle.py -q -p no:tmpdir -p no:cacheprovider
```

Result: **42 passed**, including empty-region rejection, stable-but-wrong GPU rejection,
suppressed-run failure, rejection when suppression still passes presence, provenance/coverage
requirements, complete four-family acceptance, and today's fixture remaining DIAGNOSTIC
even with fully matching pixels and repeats (including CLI exit 1).

Existing evidence rerun, preserving the original montage/digest outputs:

```powershell
py -3 -B tools/vfx_parity_oracle.py tools/vfx_oracle/regions.json --out E:/mc2-archive-2026-10/vfx-oracle-q2/q2-fix-montages
```

Result: **exit 1**, `DIAGNOSTIC: parity_accepted=False; missing_families=[]`.
PPC, missile/LRM trail, explosion and smoke each report `status=DIAGNOSTIC`,
`parity_accepted=false`; original decoded-pixel digest pairs are unchanged.
Retained new artifacts (no files deleted):
`E:/mc2-archive-2026-10/vfx-oracle-q2/q2-fix-montages/{digests.json,ppc.png,missile.png,explosion.png,smoke.png}`.
LF line endings preserved. `git diff --check` passed; the Q2 files remain untracked,
so a separate whitespace scan covers the edited files. Git Bash/check-contracts and
index writes/commits remain prohibited; build/deploy/smoke/doctest are unnecessary
for this Python/report-only correction. The orchestrator commits the existing Q2 work
and this fix; no new read-only agent review was requested for Q2-fix.
