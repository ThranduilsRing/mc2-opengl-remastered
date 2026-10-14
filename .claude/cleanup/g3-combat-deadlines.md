<!-- doc-status: current | reviewed:2026-10-14 -->
# G3-a: combat retirement semantics and the observed-divergence proof

Parent: `124bc9b70` on `claude/vfx-g2b` (runtime-accepted G2b-a
`e49b52dff`, follow-up `ce87679ad`). This branch is `claude/vfx-g3`.
G3-a adds observations only. The deadline, damage-consumed and collision-complete
fields below are a **proposed contract for G3-b/G3-c**, not implemented state.
There is no deadline assignment, retirement change, save-format change or baseline
regeneration in G3-a. Fable's explicit task claims supersede the older one-lane rules.

## Clock and restore contract proposed for the next slices

`turn` is an observation coordinate, not a combat clock. It increments at the
start of every active `Mission::update`, including paused updates
(`code/mission_update.cpp:1129`), and resets at load (`code/saveload.cpp:579`).
Single-player pause/freeze skips object updates (`code/mission_update.cpp:446-449`);
multiplayer overrides that pause. An absolute `turn` deadline would expire while
paused and would restore in the wrong epoch. The G3-a trace calls its current
`turn` coordinate `tick`; it means update ordinal, **not elapsed combat time**.

Proposed clock: a mission-owned `uint64_t combatTicks`, 1,000 ticks per simulation
second, initially zero. Immediately before a non-frozen ObjectManager update,
advance it by `floor(frameLength * 1000 + fractionalTicks)` and retain the
fractional remainder as a mission member (`double`, in [0,1)). Zero-length steps
advance zero; zero-duration policies expire on that step, subject to collision
completion. Pause/freeze advances neither member; multiplayer advances when its
object update runs. `frameLength` is the same duration used by flight and collision
timers, including forced-frame-rate and sim-speed changes. Fixed smoke time stays
at its existing 1x convention (`code/mission_update.cpp:303-329`). Speed therefore
changes the number of wall-clock frames, not the combat seconds until expiry.
Duration conversion is `ceil(double(seconds) * 1000)` with no nearest rounding;
e.g. 0.0001 -> 1, 0 -> 0, 0.5 -> 500. Fractional accumulation prevents 30 Hz from
being treated as 33 ms forever. Test 30/60 Hz, variable steps, pause and 0.25/4x.

Unarmed state is a distinct boolean, never a magic zero deadline. Flight is
unarmed. Impact/hit, ground miss and water miss arm once from the resolved weapon
type; explosion creation arms once from the resolved explosion policy. No-effect
paths use the same rules. Damage and collision scheduling remain separate state.
Use checked addition and conversion, reserving `UINT64_MAX` as invalid. Reject
nonfinite/negative steps and out-of-range clock/save/policy values with one
`[COMBAT_RETIRE v1] reject reason=<reason> type=<type>` line; never wrap, crash,
silently clamp or disarm a bad restored deadline. A clock-step rejection leaves
the clock unchanged and blocks runtime acceptance rather than altering flight dt.

Save **remaining ticks**, `max(deadline - combatTicks, 0)`, plus `armed`,
`damageConsumed` and the collision-pass state. Save the fractional clock remainder
with the mission. On load establish a new zero epoch, restore the fractional
remainder, and reconstruct `deadline = combatTicks + remainingTicks` after the
clock exists. An armed zero remainder means expiry pending, not unarmed. A saved
explosion still awaiting its collision pass must retain that obligation. Saves
may break before release, but malformed saves reject before mission teardown.

The real loader dispatches by packet size. Required **unclaimed hunks** are
`code/objmgr_io.cpp:8-21` (all sizes must stay distinct), `:1034-1059`
(CarnageData allocation -> init -> Load), `:1109-1134` (WeaponBoltData equivalent),
and `code/saveload.cpp:569-579,871` (reset/restore ordering). Merely testing
CopyTo -> Load cannot prove them. Later tests must save via PacketFile and invoke
GameObjectManager::Load through a production mission load, including both new
packet types, an armed expired record, in-flight/unarmed records, inactive slots,
truncation, invalid flags and a size collision rejected by the existing assertion.
G3-a does not edit these files or claim a save/deadline round-trip proof.

## Combat policy, pool obligations and presentation ownership

| Type/path | Proposed policy source | Proposed value / handling |
|---|---|---|
| BoltProjectileData hit/miss/water-miss, with/without effect | `AfterHitTime`, parsed at parent `code/weaponbolt.cpp:231-233` | `ceil(AfterHitTime * 1000)`; absent = existing 0 seconds; present nonfinite/negative/overflow = reject type with one line |
| BeamProjectileData after impact | Type's existing initialized `afterHitTime=0`, parent `code/weaponbolt.h:122`; this branch does not currently read AfterHitTime | 0 ticks by default; propose the same optional AfterHitTime combat reader for beams, explicitly a new table capability rather than existing provenance; BeamDuration still owns pre-impact timing |
| Every explosion, with/without effect | New `[ExplosionData] CombatRetireSeconds` adjacent to `DelayUntilCollidable`, parent `code/carnage.cpp:294-296` | Proposed absent default = **0 seconds**: retirement at the next update after the scheduled collision pass; explicit valid >=0 override = `ceil(value * 1000)` from creation, still constrained by collision completion |
| Fire Carnage | Existing fire contract | Outside the explosion deadline slice; no new policy |

The explosion default is an explicit new combat policy, **not existing table
provenance**: preserve only its gameplay obligation by default. Hold longer only
when the combat table explicitly requests it. `LightData.Duration`
(`code/carnage.cpp:298,329`) belongs to light interpolation, never combat.
`DelayUntilCollidable` defaults to `EXPLOSION_TIME=0.5` seconds
(`code/carnage.cpp:59,294-296`) and controls collision, not retirement.
The initial default thus has a roughly 500-tick minimum on absent-delay types,
plus the next object-update boundary; it is not a fixed 500-tick retirement rule.

Today tangibility is armed for one update when the float timer crosses zero
(`code/carnage.cpp:755-763`); the manager can destroy that explosion before
`Mission::updateSimSystems` runs the scheduled collision pass
(`code/objmgr_update.cpp:1327`, `code/mission_update.cpp:1148`,
`code/mission_update.cpp:506-507`, `code/objmgr_misc.cpp:291-298`). G3-c must keep
it alive through that pass regardless of policy expiry. Record collision-complete
after the whole scheduled pass, even when there are no colliders; checking
"damage was delivered" is insufficient. If collisions are globally disabled,
mark the scheduled pass skipped/completed at that same phase. Static collision
handling is separate; do not replace or repeat it. On the following object update,
retire if the deadline has expired and the collision obligation is complete.
Tests must pin damage recipient/chunk order and the exact tangibility frame.

Three endings must stay distinct:

- Deadline expiry: retire on the first eligible object update at/after the deadline
  (large variable steps can skip exact tick equality); never deliver damage again.
- Forced allocation reuse: parent `code/objmgr.cpp:667,683` unconditionally calls
  finishNow. An unresolved bolt delivers its pending direct or area damage exactly
  once before reuse; a resolved one does not replay it. Saturated explosion pools
  must preserve the old scheduled collision obligation. Choose a completed/free
  slot; when all slots are pending, grow the manager-owned pool and handle table
  in deterministic append order instead of evicting a pending explosion. Stable
  slot handles never move or renumber. This needs objmgr.h storage changes and
  loader capacity changes as well as the allocator claim; no static owner.
  No accepted combat request may be silently dropped or applied early to obtain
  a slot. Memory/handle exhaustion must surface an explicit allocation failure;
  consuming shots must retain their unresolved obligation until creation succeeds.
- Teardown/release: discard objects and effects with no late damage; it is not a
  deadline or a pool-overflow resolution. Test teardown while armed and in flight.

Reset deadline/armed/damage-consumed/collision-complete **before** publishing the
new lifetime. Saturation tests must exercise real getWeapon/getCarnage, including
reuse before a bolt's first update, after impact, after expiry, pending explosion
  collision, nested explosion creation, all slots pending (pool growth) and restored pools.
Do not infer damage exactly once from the G3-a trace: current finishNow can submit
another hit after arrival (`code/weaponbolt.cpp:747-834`); G3-a observes it unchanged.

Choose **effect truncation** at retirement. Existing destruction kills owned
effects (`code/weaponbolt.cpp:352`, `code/carnage.cpp:856-878`); preserve that
ownership with no detached references into a reusable slot. Audit the manager's
setExists(false) path as well as destructors: Carnage currently retains its effect
until finishNow/destroy. G3-c must clean it at actual retirement. No presentation
owner or static-duration owner is introduced. The rule is **no gameplay decision
depends on effect state**; presentation execution/cleanup may inspect it. Bolt's
actual dependency includes `return(inView)` at parent `code/weaponbolt_update.cpp:1296`,
the final pointer-state check `:1251-1252`, and the Execute outcomes `:1057,1131,1175`.

G3-b's per-type report must resolve the **lane3 mounted** weapon/explosion files
(loose overrides before archives), list each type and AfterHitTime line, and measure
birth/impact-to-retire deltas on the instrumented old engine. Those are observed
object/effect-tail lengths, not a guessed specification duration. No runtime or
resolved-table census has been run in G3-a, so today's typical effect lengths and
each mounted type's numerical value are **unmeasured**, not accepted. Default 0
holds will often shorten existing tails; the rendering acceptance must inspect it.

## Instrumentation and comparison

`MC2_DIAG_TAGS=CONFIG,VFX_COMBAT,COMBAT` enables the existing diagnostic JSONL
facility. No new feature gate. VFX_COMBAT v1 identifies `kind`, GameObject handle
`slot`, a member `generation` incremented at init(type), per-generation `seq`, and
the `turn` observation `tick`. It records allocation birth, every post-update
position (launcher **and flight** for bolts), impact classified by the existing
target/water branch, submitted damage including target position and shot fields,
collision arming/static pass, restore, and terminal retirement. Birth omits
positions because connect/setPosition has not yet finished. A generation reused
before any update still has birth, finishNow damage and retirement observations.
Retire reasons distinguish update_false, forced_finish, teardown, reinit and
set_exists_false. Diagnostic retirement is deduplicated, never used by gameplay.
Type/effect IDs are cached at birth so teardown does not dereference a dead type
table. Load refreshes the effect cache from the restored effect ID before the
restore observation, including inactive records. Watch IDs use
`getWatchID(false)` so tracing cannot allocate an identity.

`updateOtherFx` reads VFX_COMBAT enablement once outside both combat-object loops;
disabled post-update tracing costs one boolean branch per live object and no
trace-method calls. Emitters retain their own checks for lifecycle/damage callers.
An allocation-free tracing-ON sink is not required: diagnostic-mode allocations
belong to the shared diagnostic facility and do not touch simulation state.

The existing registered COMBAT tag has weapon-fire, hit, death and fire-decision
consumers; per-object lifetime observations would pollute that stream. Retain
VFX_COMBAT. Lane 2 must add exactly this line to `knownTags()` in
`GameOS/gameos/diagnostic_trace.cpp`, outside the default whitelist:

```cpp
        "VFX_COMBAT", // Opt-in combat-object lifetime observations.
```

GameOS is read-only in this lane. Until lane 2 registers the tag, explicit
MC2_DIAG_TAGS still enables writes but warns about the unknown tag; registered-tag
query consumers cannot use it. Runtime acceptance waits for that integration.

Save packet structs do not contain trace members; identities are session-local.
Start a separate segment after a load/reset. The comparator rejects mixed sessions,
backward ticks, reused generations, sequence gaps and events after retirement;
an exhausted capture is not accepted as a shifted retirement.
It compares **ordered** events and exact float bits, stripping diagnostic envelope,
seq and deadline metadata/events. Default nonzero coverage requires bolt hits,
bolt/explosion damage, positions and retirements in both captures. Additional
`--require bolt:impact:miss` / `--require bolt:impact:water_miss` enforce those paths.
Negative controls cover earlier damage, earlier position and shifted retirement,
including an earlier damage change followed by a later retirement change.

The verdict is **first observed divergence**. The matching-prefix report counts
positions and damage submissions. It does not prove unobserved mover/global state,
damage receiver internals, intra-update positions, or gaps due to a stopped engine.
Use equal-length captures and verify fixture completion, fingerprints and coverage.
SIMID excludes bolts/Carnage (`code/sim_identity.cpp:168-218`). An accepted G3-a
same-arm pair must be equal; G3-b/c first divergence must be retirement, with earlier
damage/positions equal. New pools can naturally change subsequent allocation and
combat order, so equality after retirement is not the requirement.

## Deterministic combat fixture and orchestrator commands

Use **mc2_24**, the existing `MC2_AIMED_FIRE_FIXTURE=1` commander-origin nearest
enemy attack (`code/warrior.cpp:2183-2245`), fixed 30 Hz and deterministic RNG.
It enters real target-hit weapon logic, unlike the point-only FX_FORCE_SPAWN
fixture or passive smokes. Candidate view is
`tools/vfx_oracle/bookmarks/mc2_24.json:ppc` ([-1131,-6251,0], rot=0,
proj=85, alt=1600); the camera is an observation, not the attack driver.
Coverage remains **pending lane3 measurement**: if nearest-enemy pursuit produces
no hit/explosion damage within 60 s, the capture is blocked evidence. Do not weaken
the comparator or use FX_FORCE_SPAWN to satisfy it. The next separately claimed
fixture must deploy a fixed shooter, stationary enemy and unobstructed 100 m
separation, script one direct hit, area hit, ground miss and water miss, then pool
saturation. Manifest includes mounted data hashes, weapon IDs, target handles,
scripted update ordinals, water cell, bookmark and nonzero expected path counts.
It needs mission/driver ownership outside this claim; no ad hoc gameplay change here.

After Fable commits **G3-a instrumentation on this G2b parent**, build that commit
and use it as PRE for all later deadline implementations. An uninstrumented G2b
log cannot supply this baseline. In A:/Games/mc2-s3, PowerShell:

```powershell
$env:_MSPDBSRV_ENDPOINT_ = "sol-g3a-$PID"
py -3 tools/build_contract_harnesses.py --run --unit
py -3 tools/build_quiet.py --target mc2
py -3 scripts/deploy_payload.py A:/Games/mc2-opengl/mc2-win64-lane3 --allow-new-target
$g3Out = "E:/mc2-archive-2026-10/g3-a-pre"
New-Item -ItemType Directory -Path $g3Out
$env:MC2_LOG = "1"
$env:MC2_DIAG_TAGS = "CONFIG,VFX_COMBAT,COMBAT"
foreach ($mission in @("mc2_03", "mc2_10", "mc2_24", "mc2_16")) {
  py -3 tools/sim_determinism.py run $mission --install lane3 --duration 60 --out "$g3Out/$mission" --env MC2_AIMED_FIRE_FIXTURE=1 --env MC2_FIXTURE_ORDER_LIVENESS=1 --env-a "MC2_DIAGNOSTIC_TRACE_FILE=$g3Out/$mission-a.jsonl" --env-b "MC2_DIAGNOSTIC_TRACE_FILE=$g3Out/$mission-b.jsonl"
  py -3 tools/vfx_oracle/compare_combat_traces.py "$g3Out/$mission-a.jsonl" "$g3Out/$mission-b.jsonl"
}
py -3 tools/sim_determinism.py run mc2_24 --install lane3 --duration 60 --out "$g3Out/camera" --env MC2_AIMED_FIRE_FIXTURE=1 --env MC2_FIXTURE_ORDER_LIVENESS=1 --env-a "MC2_DIAGNOSTIC_TRACE_FILE=$g3Out/camera-a.jsonl" --env-b "MC2_DIAGNOSTIC_TRACE_FILE=$g3Out/camera-b.jsonl" --env-a MC2_CAMERA_PARK=-1500,2500,0,60,800@300 --env-b MC2_CAMERA_PARK=-2000,2900,90,50,600@300
py -3 tools/vfx_oracle/compare_combat_traces.py "$g3Out/camera-a.jsonl" "$g3Out/camera-b.jsonl"
```

Run contracts with `bash scripts/check-contracts.sh` in the orchestrator's existing
Git Bash, never launched here. Each run requires DEPLOY_FINGERPRINT OK for the
committed exe and no ignored fixture/env. Fresh output only: diagnostic JSONL
appends and rotates on startup; never append another engine to a baseline file.
Retain all artifacts and data/exe hashes; no `record`, `write-baseline`, `check`
against stale lane3 goldens, or `--baseline-update` command.
For POST repeat the commands using a fresh `g3-post` root and its accepted commit.
Compare PRE mission-a.jsonl to POST mission-a.jsonl with `--expect retire`; compare
POST a/b and the camera pair with the default equal expectation. Retain both
camera cross-version comparisons. Miss/water/no-effect and saturation requirements
must come from the production fixture before the relevant implementation lands.

## Exact additional claims for later slices

- `code/mission.h`, `code/mission_update.cpp` (owned combat clock, pause/speed,
  collision phase completion); new `code/combat_retirement.h` for the typed policy.
- `code/objmgr.cpp:654-686` (allocator forced-reuse decisions plus its pool/handle
  initialization and destroy hunks),
  `code/objmgr_misc.cpp:291-305,376-474` (collision completion/spawn publication)
  and `code/objmgr.h` (manager-owned pool/handle growth and collision completion).
- `code/objmgr_io.cpp:8-21,1034-1059,1109-1134` and validation helpers
  `code/saved_objects_check.h` (record sizes, real loader and preflight).
- `code/saveload.cpp:569-579,871`, `code/saveload.h`,
  `code/saveload_internal.h` if its continuation frame holds the clock remainder.
- `tools/vfx_oracle/g3-combat-fixtures.json`,
  `tools/vfx_oracle/bookmarks/g3-combat.json`,
  `tools/vfx_oracle/capture_combat_fixture.py`,
  `tools/vfx_oracle/test_capture_combat_fixture.py`,
  `code/warrior.cpp` or an approved gameplay-driver fixture implementation
  (fixed hit/miss/water/pool cases; one existing trace/fixture gate, no new feature gate).
- `tools/vfx_oracle/compare_combat_traces.py`,
  `tools/vfx_oracle/test_compare_combat_traces.py` (created and exercised in G3-a);
  later `tests/unit/test_combat_retirement.cpp`,
  `tests/unit/test_combat_retirement_loader.cpp`, one CMake source line each,
  and any production-path fixture linkage explicitly approved by Fable.

No loader, allocator, clock or fixture implementation beyond today's explicit
claims is edited. Deadline tests and runtime acceptance remain G3-b/c obligations.

## G3-a offline proof checkpoint

Review fixes rechecked with
`py -3 -B -c "import tempfile,pytest; tempfile.tempdir='C:/Users/Joe/AppData/Local/Temp'; raise SystemExit(pytest.main(['tools/vfx_oracle/test_compare_combat_traces.py','--capture=sys','-p','no:tmpdir','-p','no:cacheprovider','-q']))"`:
**17 passed**. The explicit tempfile root permits conftest initialization in this
sandbox; no pytest cache or temporary-directory fixtures are enabled.

`py -3 -B C:/Users/Joe/AppData/Local/Temp/g3_fix_probe.py`: MSVC compile/run
passed, with 60 s compile and 10 s run bounds and a unique PDB endpoint. Extracted
production `updateOtherFx`, `Load`, trace and retirement method bodies run against
stubbed surrounding services. Disabled tracing across 32 bolts and 32 Carnage
objects performs exactly one tag lookup, with no emissions; enabled tracing
emits 96 events. Both arms preserve update returns and destruction outcomes.
Restores deliberately differ from the birth cache (88 -> bolt 777 / Carnage 999),
cover active, inactive and tracing-OFF records, and pin inactive terminal
deduplication. The real comparator parses all ten restore-fixture events and
checks restored effect IDs on both restore and retirement. This is a method-body
regression probe, not a production GameObjectManager save-loader proof; base Load
and destruction services are stubs. No timeout or compiler failure occurred.
The syntax and original emitter probes below were rerun successfully after these
fixes. The review also retained VFX_COMBAT for the lane-2 registration above.

`py -3 -B -m pytest tools/vfx_oracle/test_compare_combat_traces.py --capture=sys -p no:tmpdir -p no:cacheprovider -q`:
**17 passed**. `git -c core.whitespace=cr-at-eol diff --check`: passed;
CRLF/mixed header endings were retained. The raw diff check treats CR as trailing
whitespace in the existing CRLF files, so the explicit CR-at-EOL setting is needed.
The helper formatting pass's accidental Windows-default decoding was corrected;
untouched UTF-8 comment bytes were restored before final verification.

`py -3 -B C:/Users/Joe/AppData/Local/Temp/g3_syntax.py`: four MSVC `/Zs`
translation units passed (weaponbolt.cpp, weaponbolt_update.cpp, carnage.cpp,
objmgr_update.cpp), 60 s timeout per compiler invocation, RelWithDebInfo defines,
unique PDB endpoint, no timeout or compiler error. This is syntax proof, not a build.

`py -3 -B C:/Users/Joe/AppData/Local/Temp/g3_emitter_probe.py`: passed. It extracts
the actual traceCombat/traceCombatRetire definitions into a standalone class/sink
fixture, compiles and runs them, then feeds all eight emitted events to the real
comparator parser. It pins exact position/damage bits, birth omissions, disabled
tag behavior, nonallocating watch-ID reads and terminal deduplication. The first
probe link failed because its SDK LIB path was absent; adding the toolchain library
paths to the scratch probe fixed it. This is a production **trace-method** test,
not an engine damage, allocator or loader test.

Full engine build, doctest/harness tier, Git Bash contracts, deployment and lane3
combat/camera runs were not executed: the binding proof checkpoint assigns them
to the orchestrator after its commit. Generated callers/log-tag/launch inventories
were not rewritten. The two new Python files need orchestrator caller discovery;
the comparator's manual entry point is documented above and pytest discovers its
test module. `py -3 -B tools/log_tag_census.py --check` passed (1,422 emitters,
1,089 distinct; no tag-baseline diff). `py -3 -B tools/callers.py --check` reported
a stale inventory: its current tracked-only delta is 47 `reference` path changes
in 31 script entries, replacing A:/Games/mc2-main with A:/Games/mc2-s3, with no caller
count/line/text changes. The in-memory delta is retained in
`C:/Users/Joe/AppData/Local/Temp/g3-caller-delta.json`; the tracked inventory was not
rewritten. After commit, add the two new Python entries and their import/discovery
evidence at the orchestrator regeneration checkpoint.
Scratch helpers/logs/probe binaries remain in the permitted Temp root;
no files were deleted. No baseline was regenerated and no engine was launched.
