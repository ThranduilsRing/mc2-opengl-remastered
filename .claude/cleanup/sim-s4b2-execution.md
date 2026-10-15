# S4-b2a installation boundary and S4-b2b execution design

Baseline: exactly `606b584b3c192e07f7207e9a6f48940bfc798eab`. Owner ruling:
editor Part keys are content; the preserved original pilot's Enhanced Brain OPORD
wins regardless of `MC2_BRAIN_MISSIONFIT_OPORD`. No runtime proof was run for b2a.

## Installed means a synchronous handoff

The reader maps editor 0/1/2 explicitly to None/TraversePath/PatrolPath, accepts
stance 0..5 and 1..8 finite in-map points (None requires zero), and rejects the
whole order for malformed, duplicate, missing or stray order/waypoint fields.
There is no prefix admission or truncation. The selected alternate supplies content;
the original Part supplies the pilot. Admission follows commander initialization.
Mission::diagnoseAuthoredRoute owns the single rejection/suppression diagnostic;
receipt consumption does not log a second authored rejection. Suppression is info.
Absent, None, malformed and suppressed content cause zero adapter handoffs.

MissionContent ingress converts into the unchanged legacy TacticalOrder and calls
Mover::handleTacticalOrder, which calls MechWarrior::setGeneralTacOrder. The adapter
checks every installed point/mode synchronously, then records original/selected
Part IDs, preserved pilot, route kind, retained stance and the entire route before
returning. `authored_order_evidence` reads this mission-owned snapshot, never a
later pilot slot or a retired receipt. ABL can overwrite the slot before polling.
ApplicationStatus::Installed retains S4-b1's wire value 1 (Applied is its compatibility
alias). Neither spelling asserts movement execution/completion.

Installation has side effects today: setGeneralTacOrder sets newTacOrderReceived;
warrior update can clear movement/attack state and call execute. The disabled
route dispatch in tacordr.cpp and early route-status success in warrior_orders.cpp
do not make installation inert. This slice changes neither file nor warrior_move.cpp.

The separate `mc2_authored_order_seam` console target compiles the engine sources
and links its libraries, excluding the standalone corpus's actor/initializer stubs.
It uses real TacticalOrder initialization, real mover/pilot construction, the real
handler and real general setter. Only graphics-resource readiness is supplied by
the fixture. Both route kinds, one/eight points, distinct coordinates/modes and
the setter notification are asserted. `tools/build_contract_harnesses.py` is the
self-claimed runner change: normal `--run --unit` builds/runs this target after
mc2_tests and reruns its positive oracle with the adapter disconnected, requiring
an ordinary assertion failure (exit 1), never a crash. `--only`/`--asan` retain
their focused standalone scope; they are not the acceptance invocation.
The negative control still runs real admission, drain and tick completion, but
acknowledges application without calling the installation bridge. Slot/notification
assertions must fail even though the queue's receipt says Installed.

The authored witness adds a unique scratch mod under lane3/mods, copies stock
mc2_01.fit there, and writes fixtures only in that new directory. It never changes
the installed stock fit or archives. A forced alternate has a different content
pilot; evidence must retain the original pilot. Valid requires exactly one complete
installation; malformed requires zero installations and exactly one rejection;
suppressed requires zero installations and one info diagnostic, with the OPORD
runtime gate both off and on. An empty/disconnected loader cannot pass any case.
Scratch mods and per-case fixture copies remain for review. Movement is not graded.

## B2b route state and arbitration

Use a mission-owned RouteExecution record keyed by durable Part incarnation and
execution ID, separate from ingress receipts. Store original/selected Part IDs,
preserved pilot identity, route kind, stance, points/modes, next cursor, active
order identity/revision, state, suspension reason, retry budget/deadline and last
transition tick. Do not infer ownership from matching coordinates or legacy id
(legacy assignment does not copy id). Emit Started, Suspended, Resumed, Completed,
Interrupted and Failed events exactly once per transition with tick and reason.
Receipts may already have retired when these occur.

Define stance as an execution policy before enabling dispatch. There is no legacy
stance field or established pilot setter. Keep the validated value mission-owned;
map it deliberately to movement/combat choices in b2b with tests for all six values.
Do not invent a TacticalOrder member or silently discard the retained stance.

Replace warrior_move.cpp's endpoint reversal in getNextWayPoint with forward wrap
for patrol: last -> zero, including one point -> zero. Traverse visits each point
once and completes after its last arrival. Never read mode[numPoints] at traverse
completion; never compute -1 for a one-point patrol. A cursor advances only when
the consumer accepts that transition; query-only calls preserve it. Keep current
legacy cursor and mission execution cursor consistent across path requests.

BattleMech::setNextMovePath (mech_move.cpp) consumes getNextWayPoint and requests
the path. Mover::calcMovePath (mover_path.cpp) fatals without both PathFindMap
entries. Start execution only after mission path maps and goal map are ready;
do not create an early mission_update.cpp escape/goal-map order phase in b2a.
Ground-vehicle advancement must follow the same cursor contract. An unreachable
segment keeps its cursor while a bounded deterministic retry is pending, then
emits Failed(Unreachable); do not skip points or retry forever. Death, removal or
pilot/incarnation replacement emits Interrupted and releases ownership permanently.

Player orders cancel the authored route permanently, matching warrior.cpp's
general-order clearing. An explicit replacement general order from ABL cancels it
as Superseded; do not reinstall every tick and fight ABL. Temporary combat/alarm
orders suspend/resume only while the same authored general revision still owns
the slot. Preserve next cursor, mode and stance through suspension; resume from
the interrupted segment after revalidation. A replacement while suspended cancels
the record; clearing an alarm alone must not resurrect it. A completed traverse
is terminal; patrol remains active until cancelled, failed or dead. Prove each
case through real setters and update arbitration before enabling route dispatch.

## Save/restore and additional claims

warrior_state.cpp copies tactical orders and queues into raw saved data in
copyToData/init (including the regions originally identified at 596/624/821).
Changing tacordr.h changes that layout and is forbidden in b2a. B2b should also
keep the layout unchanged and use a versioned mission execution archive section.
Restore route identity, stance, points/modes, next cursor, state, owning revision,
suspension reason, retry budget/deadline and transition/event sequence alongside
the ingress source/sequence/tick state. Serialize durable identities, never pointers,
watch IDs or live-session tokens. Save only at a completed simulation tick. Read
into a candidate; bind after Part/pilot/commander restoration and path-map setup,
before the first active update. Reconcile restored slot/cursor and suspended owner
once; do not re-admit and replay a previously installed route. Reject inconsistent
restore data atomically with one diagnostic. Prove uninterrupted vs save-at-K/N
for pending, active, suspended and unreachable routes.

B2b must explicitly claim these currently excluded implementation files:
`code/warrior_move.cpp`, `code/mech_move.cpp`, `code/gvehicl_move.cpp`,
`code/mover_path.cpp`, `code/warrior.cpp`, `code/warrior_alarms.cpp`,
`code/warrior.h`, `code/warrior_state.cpp`, `code/warrior_orders.cpp`,
`code/tacordr.cpp`, `code/tacordr.h`, `code/mission_update.cpp`,
`code/saveload.cpp`, `code/saveload_load.cpp`, `code/saveload_internal.h`,
`code/save_archive.h`, `code/save_archive.cpp`.
Also renew `code/mission.h`, `code/mission.cpp`, `code/mission_init.cpp`,
`code/tactical_order_queue.h`, `code/tactical_order_queue.cpp`, their unit tests,
the production-seam test and registration, and the witness/test. Any other
consumer or new file requires a named claim before implementation. Save claims
remain contingent on lane 1's quickload ownership being released.

## Offline proof commands (this lane)

```powershell
Set-Location A:/Games/mc2-s1
$env:_MSPDBSRV_ENDPOINT_ = 'sol-s4b2-proof-' + $PID
py -3 tools/build_contract_harnesses.py --run --unit
if ($LASTEXITCODE) { throw 'contract/seam proof failed' }
py -3 tools/build_quiet.py --build-dir build64-order-seam --target mc2
if ($LASTEXITCODE) { throw 'targeted build failed' }
py -3 -B -m pytest scripts/tests/test_mission_order_ingress_witness.py --capture=sys -p no:tmpdir -p no:cacheprovider -q
if ($LASTEXITCODE) { throw 'witness offline tests failed' }
```

No Git Bash, deploy, smoke, witness or game/editor launch is permitted for this lane.
The orchestrator must run `bash scripts/check-contracts.sh`; generated caller/log/
launch baselines remain unchanged here. Compiler probes are bounded and a timeout
is reported separately from a compiler error.

Read-only census checks in this worktree report expected merge work: log_tag_census
adds two AUTHORED_ORDER v1 emitters (stderr level; stderr status/original/selected/
pilot/stance/points), alongside inherited S4-b1 ORDER_INGRESS/FIXTURE baseline drift.
callers.json needs the bootstrap_worktree_build import in the contract runner,
mc2x_import.fst import in the witness, and shifted caller lines; the existing
test_mission_order_ingress_witness.py also lacks a registered caller/allowlist entry.
background-launch remains red on the pre-existing tools/vfx_oracle/probe_rng.py
dynamic spawn. Do not regenerate those baselines in this slice or edit that other
lane's script. The orchestrator owns reconciliation and the complete board run.

## Orchestrator runtime proof (commands only; after commit)

Run on the orchestrator after the reviewed pathspec commit, with lane3 exclusively
leased by the harness. These commands use a detached baseline checkout, never
rewrite a committed SIMID baseline, and deploy only lane3. The two source trees
use the same VS RelWithDebInfo configuration/dependencies. Confirm both clean.
The pristine source/dependency copies and all proof artifacts remain on E:.

```powershell
Set-Location A:/Games/mc2-s1
$ErrorActionPreference = 'Stop'
function Require-Native { if ($LASTEXITCODE) { throw "native command failed: $LASTEXITCODE" } }
$post = (git rev-parse HEAD).Trim(); Require-Native
if (git status --porcelain --untracked-files=no) { throw 'commit reviewed changes first' }
$proof = 'E:/mc2-archive-2026-10/s4b2-' + $post.Substring(0,12)
New-Item -ItemType Directory -Path $proof -ErrorAction Stop | Out-Null
$pre = "$proof/pre-source"
git worktree add --detach $pre 606b584b3c192e07f7207e9a6f48940bfc798eab; Require-Native
$env:_MSPDBSRV_ENDPOINT_ = 'orchestrator-s4b2-pre-' + $PID
py -3 scripts/bootstrap_worktree_build.py --worktree $pre --donor A:/Games/mc2-s1 --build; Require-Native
$env:_MSPDBSRV_ENDPOINT_ = 'orchestrator-s4b2-post-' + $PID
py -3 scripts/bootstrap_worktree_build.py --worktree A:/Games/mc2-s1 --donor A:/Games/mc2-s1 --build; Require-Native
bash scripts/check-contracts.sh; Require-Native
py -3 tools/build_contract_harnesses.py --run --unit; Require-Native
Get-FileHash "$pre/build64/RelWithDebInfo/mc2.exe", 'A:/Games/mc2-s1/build64/RelWithDebInfo/mc2.exe' -Algorithm SHA256 |
    Format-List | Out-File "$proof/binary-sha256.txt"
$env:MC2_ACTIVE_MOD = ''; $env:MC2_MOD_DEPS = ''
$env:MC2_RNG_SEED = '0x4b2a'; $env:MC2_SMOKE_REQUIRE_FINGERPRINT = '1'
$env:MC2_BRAIN_MISSIONFIT_OPORD = '0'
# Pin any other inherited MC2_* settings identically across both stages.
Get-ChildItem Env:MC2_* | Sort-Object Name | Export-Clixml "$proof/environment.xml"
$strictPair = @'
import hashlib
import json
import os
from pathlib import Path
import sys
source, mission, output, fixture, camera = sys.argv[1:]
sys.path.insert(0, str(Path(source) / 'tools'))
import sim_determinism as s
out = Path(output)
out.mkdir(parents=True, exist_ok=False)
original_run = s.subprocess.run
calls = []
def strict_run(*args, **kwargs):
    result = original_run(*args, **kwargs)
    index = len(calls)
    calls.append(result.returncode)
    (out / ('harness-' + str(index) + '.txt')).write_text(result.stdout, encoding='utf-8')
    (out / ('environment-' + str(index) + '.json')).write_text(json.dumps(
        {k:v for k,v in kwargs['env'].items() if k.startswith('MC2_')}, sort_keys=True), encoding='utf-8')
    if result.returncode or '[DEPLOY_FINGERPRINT] OK' not in result.stdout:
        raise SystemExit('smoke/fingerprint failure; see harness transcript')
    return result
s.subprocess.run = strict_run
extra = {'both': {'MC2_FIXTURE_ORDER_LIVENESS': fixture}}
if camera == '1':
    extra.update(a={'MC2_CAMERA_PARK': '-1500,2500,0,60,800@300'},
                 b={'MC2_CAMERA_PARK': '-2000,2900,90,50,600@300'})
logs = s.run_mission(mission, '60', str(out), extra, install='lane3')
digest = hashlib.sha256()
install = Path('A:/Games/mc2-opengl/mc2-win64-lane3')
inputs = [install / name for name in s.FINGERPRINT_ARCHIVES if (install / name).is_file()]
for tree in s.FINGERPRINT_TREES:
    inputs.extend(p for p in (install / tree).rglob('*') if p.is_file())
for path in sorted(set(inputs)):
    digest.update(path.relative_to(install).as_posix().encode())
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            digest.update(block)
(out / 'content-sha256.txt').write_text(digest.hexdigest(), encoding='utf-8')
ok, message = s.compare(*(s.load_simid(log) for log in logs), min_frames=1200)
print(message)
if not ok or len(calls) != 2:
    raise SystemExit(1)
'@
Set-Content -LiteralPath "$proof/strict-pair.py" -Value $strictPair -Encoding UTF8
foreach ($stage in @('pre','post')) {
    $source = if ($stage -eq 'pre') { $pre } else { 'A:/Games/mc2-s1' }
    py -3 "$source/scripts/deploy_payload.py" A:/Games/mc2-opengl/mc2-win64-lane3 --source-root $source --build-dir "$source/build64/RelWithDebInfo" --allow-new-target; Require-Native
    $stageRoot = "$proof/$stage"; New-Item -ItemType Directory $stageRoot | Out-Null
    Get-FileHash A:/Games/mc2-opengl/mc2-win64-lane3/mc2.exe -Algorithm SHA256 | Format-List | Out-File "$stageRoot/deployed-sha256.txt"
    foreach ($m in @('mc2_03','mc2_10','mc2_24','mc2_16')) {
        $fixture = if ($m -eq 'mc2_16') { '1' } else { '0' }
        py -3 "$proof/strict-pair.py" $source $m "$stageRoot/$m" $fixture 0; Require-Native
    }
    foreach ($m in @('mc2_10','mc2_24')) {
        py -3 "$proof/strict-pair.py" $source $m "$stageRoot/camera-$m" 0 1; Require-Native
    }
}
# Compare the complete SIMID lines, including strict raw float digests.
# `check --baseline` is unsuitable: its strict-only mismatch is advisory.
foreach ($set in @('mc2_03','mc2_10','mc2_24','mc2_16','camera-mc2_10','camera-mc2_24')) {
    $m = $set.Replace('camera-','')
    if (Compare-Object (Get-Content "$proof/pre/$set/content-sha256.txt") (Get-Content "$proof/post/$set/content-sha256.txt")) { throw 'resolved content changed' }
    foreach ($arm in @('a','b')) {
        $before = "$proof/pre/$set/$arm/$m.log"; $after = "$proof/post/$set/$arm/$m.log"
        $seedBefore = Select-String -LiteralPath $before -Pattern '^\[DET_RNG\].*reseed=' | ForEach-Object { $_.Line }
        $seedAfter = Select-String -LiteralPath $after -Pattern '^\[DET_RNG\].*reseed=' | ForEach-Object { $_.Line }
        if (!$seedBefore -or (Compare-Object $seedBefore $seedAfter)) { throw 'seed mismatch or missing seed' }
        $index = if ($arm -eq 'a') { 0 } else { 1 }
        if (Compare-Object (Get-Content "$proof/pre/$set/environment-$index.json") (Get-Content "$proof/post/$set/environment-$index.json")) { throw 'resolved environment changed' }
        py -3 tools/sim_determinism.py compare "$proof/pre/$set/$arm/$m.log" "$proof/post/$set/$arm/$m.log" --min-frames 1200; Require-Native
    }
}
py -3 tools/mission_order_ingress_witness.py --authored --install A:/Games/mc2-opengl/mc2-win64-lane3; Require-Native
py -3 tools/mission_order_ingress_witness.py; Require-Native
```

Before accepting comparisons, require each artifact's seed and resolved content
fingerprint to agree pre/post, inspect fingerprint OK lines and smoke gate verdicts
(sim_determinism may preserve logs despite a failed smoke verdict), and preserve
the manifests/logs beside the two binary hashes. Fail on any strict mismatch,
missing frame budget, missing fingerprint, smoke failure or content/environment
difference. The fixture run is deliberately after all stock comparisons.

Runtime loader-disconnected negative control: the orchestrator creates a separate
reviewed local control commit that removes only `admitAuthoredRoutes();` in
mission_init.cpp, builds/deploys that fingerprinted commit to lane3 and runs the
same `--authored` command. It must exit 1 on the valid case with no installation.
Restore/deploy the reviewed positive commit and require exit 0 for all four cases.
Do not disable fingerprints or grade a copied executable against another HEAD.
The offline runner's adapter-disconnected control is separate from this live
loader-disconnected proof. B2b owns all route movement/arbitration runtime proofs.

Continue in the same orchestrator PowerShell session for the negative control:

```powershell
$control = "$proof/loader-disconnected-source"
git worktree add --detach $control $post; Require-Native
$file = "$control/code/mission_init.cpp"
$text = [IO.File]::ReadAllText($file)
$needle = 'admitAuthoredRoutes();'
if (($text.Split(@($needle),[StringSplitOptions]::None).Length - 1) -ne 1) { throw 'control hook count changed' }
[IO.File]::WriteAllText($file, $text.Replace($needle, '/* Authored loader disconnected for negative control. */'), [Text.UTF8Encoding]::new($false))
git -C $control diff -- code/mission_init.cpp; Require-Native
git -C $control add -- code/mission_init.cpp; Require-Native
git -C $control commit -m 'test(sim): disconnect authored loader control' -m 'Co-Authored-By: GPT-6 (Codex) <noreply@openai.com>' -- code/mission_init.cpp; Require-Native
$env:_MSPDBSRV_ENDPOINT_ = 'orchestrator-s4b2-control-' + $PID
py -3 scripts/bootstrap_worktree_build.py --worktree $control --donor A:/Games/mc2-s1 --build; Require-Native
py -3 "$control/scripts/deploy_payload.py" A:/Games/mc2-opengl/mc2-win64-lane3 --source-root $control --build-dir "$control/build64/RelWithDebInfo" --allow-new-target; Require-Native
Get-FileHash "$control/build64/RelWithDebInfo/mc2.exe" -Algorithm SHA256 | Format-List | Out-File "$proof/loader-control-sha256.txt"
py -3 "$control/tools/mission_order_ingress_witness.py" --authored --install A:/Games/mc2-opengl/mc2-win64-lane3
if ($LASTEXITCODE -ne 1) { throw 'loader-disconnected witness must fail with exit 1' }
py -3 scripts/deploy_payload.py A:/Games/mc2-opengl/mc2-win64-lane3 --source-root A:/Games/mc2-s1 --build-dir A:/Games/mc2-s1/build64/RelWithDebInfo --allow-new-target; Require-Native
py -3 tools/mission_order_ingress_witness.py --authored --install A:/Games/mc2-opengl/mc2-win64-lane3; Require-Native
```
