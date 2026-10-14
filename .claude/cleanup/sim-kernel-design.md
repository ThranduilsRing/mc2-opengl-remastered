# S1: one 30 Hz simulation kernel
Tick count owns time; TacticalOrder owns command ingress; windowed and headless runs share the same state and digest.
Design only, 2026-10-13; implementation belongs to the orchestrator's scheduled lane.

## Contract and current evidence

Owner rulings supersede the older lane/path instructions: this document was researched only in `A:/Games/mc2-codex`.
There is one final release after migration, Vulkan and editor work; saves may break before it. Mod content remains supported.
Use C++23 value types and explicit lifetime owners; no static-duration owners, legacy-shape wrappers or permanent feature gates.
This implements target-architecture section 2.3 and program-review T3/S1b (week 2+, after ownership/load-path prerequisites).
T3's buildings/turrets/ABL/count extension already exists here; verify it rather than implementing it twice.

| Evidence in this checkout | Design consequence |
|---|---|
| `code/mechcmd2_frame.cpp:369`, `:391`, `:402`, `:427`, `:514` derive dt, override it for smoke, scale retail dt, poll input and call one mission update | Separate presentation pumping from zero-or-many fixed ticks. |
| `code/mission_update.cpp:300`, `:320`, `:341`, `:1129` have two clocks, paused wall sampling and render-driven turn increments | Delete both clock branches after extracting their deterministic reference path. |
| `code/ablmc2_camera.cpp:745` now returns mission milliseconds for GetLogisticsTime | Preserve a script clock derived exclusively from ticks. |
| `code/weather.cpp:161`, `:282`, `:428` isolate weather RNG but use the camera for drops | Stream isolation alone does not make Weather a sim system. |
| `mclib/cmponent.cpp:640` zeroes the table including unauthored disableLevel | Keep this initialization; never restore heap-dependent defaults to match an older baseline. |
| `mclib/soundsys.cpp:1335`, `:1341`, `mclib/script_music.h:16` separate script music from device playback | Move the script state and duration metadata into mission-owned deterministic state. |
| `mclib/appearance_rng.cpp:8`, `:13`, `mclib/mech3d_update.cpp:168` isolate appearance draws | Move that state out of static storage and finish removing presentation work from sim. |
| `code/sim_identity.cpp:170`, `:202`, `:342` already collect objects, ABL statics and three RNG counts | Extend coverage and split authoritative/presentation digests. |

The refreshed lane1-state records mc2_03/10/24/16 baseline passes, not the earlier hunt narrative.
HANDOFF-LOG entries 2026-10-12 11:36 and 13:46 record 12 mc2_24 pairs, camera-park parity and deterministic script music.
They are historical evidence, not fresh proofs of the design below. `docs/sim-trace.md` defines the compile-time DT hunt facility.

## Tick loop and snapshots

`GameSession` owns `SimKernel`, the pacing scheduler and optional presentation; `SimKernel` owns mission systems and `Tick completed` (u64).
Tick zero is the fully loaded initial state. `step()` sets `executing = completed + 1`, drains that tick's commands once and runs systems;
then commits `completed = executing`, clears executing, publishes the immutable state and emits one authoritative digest.
`executing` is kernel-owned transient context, absent between ticks and never saved. Never increment a tick on a render-only frame.
All gameplay integrates with the same representable float `1.0f / 30.0f`; authoritative time is the integer tick, not accumulated float seconds.
Seconds and ABL milliseconds are derived as `tick / 30.0` and `tick * (1000.0 / 30.0)` without a truncating 33 ms clock.
Sim deadlines use ticks, rounding positive durations upward; an explicit phase within a tick replaces accidental render-order timing.
Legacy float clock views exist only during extraction and disappear once users move to the tick context.

The outer loop polls UI once, resolves commands against the latest immutable snapshot, then runs this scheduler:

```text
wallDelta = clamp(monotonicNow - previousWall, 0, 250 ms)
previousWall = monotonicNow
if paused: discard accumulated credit; pump presentation; return
credit += wallDelta * rate                     // rational rate: 1/2, 1, 3/2, 2
while credit >= 1/30 second and steps < 8:
    kernel.step(); credit -= 1/30 second; steps += 1
if credit >= 1/30 second: discard whole overdue credit; retain fraction; log overload
present(latestSnapshot, presentationDelta)
```

Use integer/rational pacing arithmetic (e.g. nanoseconds multiplied by 30 and the rate numerator), carrying the remainder.
The 8-tick cap bounds work; excess wall credit is lost, never simulated with a larger dt or skipped tick numbers.
At sustained overload game time slows; it cannot recover an unbounded debt. The 250 ms clamp also prevents suspend/resume bursts.
Speed buttons request 15, 30, 45 or 60 ticks per wall second; they never multiply physics dt or ABL time.
Apply a rate change at a pump boundary after accounting for elapsed time at the old rate. Speed and wall credit are presentation settings.
Pause/resume/load rebase the wall sample and clear credit; commands queued while paused remain due for the next executed tick.
Audio, input double-click timers, camera smoothing and menu fades receive presentation dt, not sim `frameLength`.
`mclib/timing.h:18` currently mixes sim floats, DWORD wall state and timers; replace it with separate sim time and platform timing headers.
Gameplay Timer semantics move to tick deadlines; updateWhilePaused UI timers leave the kernel entirely.

Initially render the latest snapshot without interpolation: the architecture chooses an RTS-tolerable 33 ms update interval,
and this avoids blending across destruction, teleport, target changes or save/load while proving separation.
Camera/UI remain responsive every presentation frame. Capture mode pins a completed tick and freezes cosmetic animations.
Later interpolation may blend previous/current transforms inside presentation only; it must never feed targeting, LOS, collision or saves.
Snapshots contain stable handles, world transforms, HP/status, HUD path polylines and event records, with no pointers to mutable sim objects.
Use single-threaded ordered simulation first. Retain current system order from `code/mission_update.cpp:1144` through `:1174`;
extract path, objects, collisions, sensors and mission brain without reordering their gameplay work in the extraction batches.
Brain cadence (`code/warrior.cpp:80`, `:115`) uses owned integer schedules, saved once in Clock by stable system ID; ABL/Environment reference that registry, never a second clock.

### Editor session lifecycle

An immutable editor document snapshot (including resolved content and chosen root seed) builds an isolated `GameSession` at tick zero.
Play runs the same pacing scheduler; pause stops ticks; step is available only while paused and executes exactly one `kernel.step()`.
Reset destroys that runtime and rebuilds it from the same snapshot and seed, restoring initial queues/receipts/schedules.
Play, pause, step, runtime save/load and reset never mutate the editor document or its undo history. Edits remain document commands;
an explicit new snapshot starts a new session to include them. Closing the session releases its runtime owners without document changes.
Prove snapshot/document/undo hashes unchanged across play/step/reset, exactly one digest per step, and reset replay identical to tick zero.

## TacticalOrder queue: one ingress

The existing per-warrior waypoint queue (`code/warrior.h:759`) is execution state, not the new mission ingress queue.
`code/missiongui_orders.cpp:705` presently expands selected units and either queues waypoints or calls handleTacticalOrder immediately (`:759`).
ABL calls the imperative warrior order helpers (`code/ablmc2_orders.cpp:165`); those mutate goals and request paths (`code/warrior_orders.cpp:67`).
Replace this ingress with owned, fully initialized TacticalOrder values; do not serialize the legacy packed words or copying quirks
(`code/tacordr.h:249`). Keep tactical meanings, replace execution-stage and pointer fields with typed state.
Target `TacticalOrder` is `{orderId, origin, kind, unitOrder, subOrder, move?, attack?, target:ObjectRef?, formationOrdinal, groupFlags}`;
move/attack/support arms contain named values, never packed words. `OrderExecution` carries payload, issued/notBefore/lastUpdate ticks,
typed stage/status, lastMoveGoal and optional point-unit ObjectRef. Ingress, execution queues and saves use these same types.

```text
OrderEnvelope { applyTick, sourceId, sequence, issuer, recipients[], queueMode, TacticalOrder payload }
submit(OrderEnvelope) -> expected<OrderReceipt, OrderError>
submitNext(source, sequence, issuer, recipients, mode, payload) -> receipt for (executing ? executing + 1 : completed + 1)
beginTick(Tick) -> immutable ordered batch       // kernel-only drain, exactly once
snapshotPending() / restorePending(validatedRecords)  // save/replay boundary
```

`submit` validates shape, finite coordinates, bounded lengths, source permissions, duplicate identity and capacity.
At application revalidate live handle generations, authority and legal targets; record deterministic rejections without crashing.
No silent drop on capacity exhaustion. Canonical ordering is `(applyTick, sourceId, sequence, recipient ordinal)`;
recipient order is recorded explicitly in stable roster order, preserving formation/selectionIndex behavior.
Track executing tick k explicitly: submissions during k target k+1; between ticks they target completed+1. Explicit submit may choose later.
Commands due before that boundary reject. Future lockstep waits for a complete tick batch,
it does not insert commands retroactively. Network transport remains compiled out; this protocol defines a future peer's input contract.

UI polling (`mclib/userinput.cpp:228`) and picking (`code/missiongui_update.cpp:795`) stay outside sim.
Capture recipient handles, target handle or canonical world point, attack range and waypoint modifiers at submission.
Compute formation goals using sim data at application; never query live selection, screen coordinates or WAYPOINT_KEY during a tick.
Single-player calls submitNext through precisely the ingress a peer/harness uses. Speed/pause/view actions belong to outer scheduling.
Audit bypasses such as `code/controlgui_update.cpp:645`, airstrikes (`code/missiongui_orders.cpp:569`) and reinforcement/driver mutations.
Support actions become typed command variants of the same ingress, including deterministic costs, spawn and mission-control actions.

ABL order builtins submit with an instance source ID and saved sequence counter during tick k for k+1.
Their synchronous result means validated/accepted, not already executed; apply results become deterministic script-visible receipt state.
Preserve status-query builtins as queries. Internal execution suborders/path retries remain within the applying system, avoiding recursive enqueue.
Audit scripts that issue then immediately inspect an order; test their behavior under the authorized next-tick semantics.
Non-order script mutations remain deterministic VM host operations inside the tick, not external UI entry points.
Harnesses feed the same validated envelopes from a versioned replay, never synthesize clicks or call warrior helpers directly.
Replay header pins ruleset, digest schema, content manifest, mission seed, initial save digest and target tick; records use explicit fields.
Save the pending queue, deduplication/sequence state and each warrior's execution queue at a completed-tick boundary.
Source sequences are monotonic u64; save each stable source ID, next sequence and last admitted sequence for duplicate rejection.
Receipts retain accepted/application status and per-recipient results until deterministic acknowledgement; ABL bindings save receipt IDs.

## Presentation dependencies and the headless runner

`mc2 --headless --mission <name> --ticks N [--seed S] [--orders replay] [--load save]` is a shared application mode,
with the same validating loaders, mission initialization, ABL host, tick function, command validation and digest encoder as windowed play.
Ticks run to the exact requested endpoint without wall pacing; terminal mission results are recorded and early termination is explicit.
No SDL window/input, GPU/backend startup, textures, shape baking, appearance updates, camera, HUD, audio device, video decode or presentation RNG owners.
Load gameplay bounds, collision nodes, weapon timings and immutable audio/video duration metadata independently of visual assets.
Never substitute dummy values for gameplay data formerly obtained from Appearance; extract such data and prove equality first.
Bolt hit/damage (`code/weaponbolt_update.cpp:428`) and fall recovery (`code/mech_move.cpp:783`) stay in sim even if their visuals disappear.
Move the four path projection calls (`code/mech_update.cpp:787`, `:789`, `code/gvehicl_update.cpp:791`, `:793`) to HUD rendering.
Split terrain's collision/navigation data from camera visibility, texture updates, geometry, light baking and visual displacement damp.
Retain gameplay craters only if they alter navigation/damage; cosmetic crater rotation is a presentation event parameter.

Weather today changes lights, sound and camera-local drops; the consumer scan finds presentation reads, not gameplay weather effects.
Give authored weather progression a deterministic Environment timeline; emit lighting/rain/thunder cues, with drops in RenderWeather.
Environment runs in both modes independently of the effects option. Any discovered gameplay weather consumer reads Environment, never camera lighting.
Music/voice/video logical requests and completion use tick deadlines plus validated content durations, never device end callbacks.
Owner ruling: ABL camera getters return the last scripted camera value; setters emit presentation cues. Player camera motion never flows back.
Stock mc2_01, mc2_10 and tut_01 save and restore the camera around cutscenes. mc2_01's mid-mission read now restores the scripted rotation, an accepted difference.
The scripted cue host exposes core values rather than Camera types; physical camera state is unavailable to the kernel. forceMovieEnd and callout/voice queries (`code/ablmc2_camera.cpp:706`, `:734`) still require a host audit.
Headless evaluates those logical completions and records cues. Windowed retains cinematic and in-mission video playback.
Changing these bindings also requires windowed proof of campaign-start, won-mission and in-mission videos with `[VIDEO v1] play` controls.

Extend `scripts/run_smoke.py` with an explicit headless branch before its host-lease acquisition (`:761` today).
That branch invokes the harness-managed background process with console_child, per-run scratch, isolated save/log paths and an immutable install snapshot.
It obtains no desktop/GPU host lease because it creates neither; this is a new runner policy, never `--no-lease` or an owner bypass.
Require an executable capability/boot record proving the headless branch never initialized presentation; missing or contradictory records fail.
Keep foreign-windowed-engine protection unchanged; classify harness-managed headless processes separately only after this capability proof.
Retain install/deploy coordination so builds/deploys cannot modify live inputs; initially run missions sequentially to avoid shared-file assumptions.
Tier2 reuses its 24-mission manifest and fixture inputs, with tick budgets replacing duration and frame-number injections.
Grade load success, liveness/progress, exact tick completion, sim events, save/resume and digests. GPU/FPS/visual gates stay in windowed tier1/capture.
Fingerprint, crash detection and malformed-mod rejection remain mandatory; headless success cannot prove rendered/video playback parity.

## RNG streams and save ownership

Preserve the current defined LCG arithmetic and shaping: `mclib/det_rng.h:21` advances a u32 with 1664525/1013904223, returns high 15 bits.
Do not use implementation-dependent standard distributions. Validate ranges; invalid content logs a rejection without advancing a stream.
Choose a random u32 root seed once outside the kernel for a normal fresh game; proofs/harnesses supply explicit seeds. Saves restore it verbatim.
Current mission-name hashing (`code/mission_init.cpp:74`) is extraction evidence, not the target default; fresh-game randomness is the ownership-plan ruling at `cxx23-step4-ownership-plan.md:51`.
Each system owns a value stream; SimKernel owns the stream inventory, not shared global mutable state. All draw counters become u64.
For new streams derive initial u32 state as `rootSeed XOR streamId`; IDs below are fixed schema constants, never enum position or std::hash.
Each stream record carries `{streamId, algorithmId, algorithmVersion, initialSeed:u32, state:u32, drawCount:u64}`; restore before any draw.
The save also carries tick, scheduling phases, pending commands and script logical state. Seed alone cannot resume a stream.

## Shared sim/save contract

This is the single contract table for S1 and [S3/B7](cxx23-step4-save-schema-design.md); owners are targets, citations are current-code evidence.
`R` means the stream record above. Mission saves require all sim streams in RngState; optional streams live in Presentation, absent headless.
Every authoritative row participates in the complete digest; every carried record has bit-exact roundtrip and malformed-input rejection proofs.

| Row | Owner | Kernel rule | What the save carries | Proof |
|---|---|---|---|---|
| Clocks/tick | SimKernel; GameSession pacing | Fixed 30 Hz; executing k admits k+1; speed is ticks/wall second | completedTick:u64, integer scheduling phases/next-due ticks, SimRules; derive seconds/ABL ms/turn; no executing tick, wall credit, pause or speed | Same replay at all rates/pumps; boundary restore matches uninterrupted ticks |
| MissionInit RNG / 0x494E4954 | Mission loader/kernel | Authored generation (`code/mission_init_load.cpp:683`) | Required R, even after init | Init choices and next draws match |
| Combat RNG / 0x434F4D42 | CombatSystem | Hit/critical rolls (`code/mech_weapons.cpp:170`, `code/mech_damage.cpp:227`) | Required R | Hit/critical continuation |
| PilotChecks RNG / 0x50494C54 | WarriorSystem | Piloting/skills (`code/mech.cpp:321`, `code/warrior.cpp:727`) | Required R | Skill/check continuation |
| AI RNG / 0x41494445 | DecisionSystem | Decision draws only; current tactical hash (`code/warrior.cpp:1390`) migrates to root seed/tick/handle | Required R; rootSeed is RngState's sole tactic seed | Choice and hash continuation |
| Path RNG / 0x50415448 | PathSystem | Goal/reroute rolls (`code/mover_path.cpp:456`, `code/warrior_move.cpp:1079`) | Required R plus ordered path requests/results | Pending/retry continuation |
| ABL RNG / 0x41424C52 | Mission ABL host | Random callbacks (`code/ablmc2.cpp:332`); only ABL reseeds | Required R plus ABL reseed ordinal/history | Explicit and -1 reseeds resume exactly |
| Physical RNG / 0x50485953 | Physical/impact system | Gameplay fall/hit effects (`code/mech_move.cpp:783`, `code/weaponbolt_update.cpp:428`) | Required R | Fall/impact continuation |
| Environment RNG / 0x454E5652 | Environment timeline | Trend/lightning (`code/weather.cpp:243`, `:317`), no drop draws | Required R; Environment deadlines separate | Camera/effects-option invariant draws |
| Appearance RNG / 0x41505052 | RenderWorld appearance | Cosmetic only (`mclib/mech3d_update.cpp:168`) | Optional Presentation R | Visual restore; sim digest unchanged |
| VFX RNG / 0x56465852 | RenderWorld effects | Cosmetic beam/tumble (`code/weaponbolt_update.cpp:974`, `mclib/mech3d_update.cpp:767`) | Optional Presentation R | Visual restore; sim digest unchanged |
| Decals RNG / 0x4445434C | RenderWorld decals | Cosmetic rotation (`code/mech_update.cpp:459`, `code/weaponbolt_update.cpp:490`) | Optional Presentation R | Decal restore; sim digest unchanged |
| RainDrops RNG / 0x5241494E | RenderWeather | Camera-local drops (`code/weather.cpp:428`) | Optional Presentation R | Camera changes never advance sim streams |
| TerrainVisual RNG / 0x54564953 | Render terrain | Visual water bits (`mclib/mapdata.cpp:603`, `:644`) | Optional Presentation R plus visual rebuild recipe | Rebuild consumes no sim draws |
| AudioVariation RNG / 0x41554452 | Presentation audio | Cosmetic acknowledgements (`code/tacordr.cpp:959`, `:989`, `:1021`) | Optional Presentation R; logical media separate | Device/ack variation cannot change sim |
| Order queue + ingress | Kernel OrderIngress; warrior execution | Same typed TacticalOrder; canonical batch; executing k submits k+1, between ticks completed+1 | Pending envelopes, per-source next/last-admitted sequences, retained receipts/results/acknowledgements, active/queued OrderExecution | During/between-tick admission, duplicate/capacity rejection, queued save replay |
| ABL state | Mission ABL environment/host | VM returns before boundary; deterministic queries/host operations | Module/instance order, typed eternals/statics/FSM/return/order flags, integer schedules, source/receipt bindings, reseedOrdinal:u64 and typed reseed history | Corpus roundtrip; issue/query/result and -1 reseed continuation |
| Media/cue timelines | Kernel logical media/cue host | Tick completions from content durations; no device callback or physical Camera | Ordered requests, stable IDs/next sequence, kind/asset/source, start/end/duration ticks, loop/status, typed cue trajectories, completion/consumption latches | Mid-track/cue save matches headless/windowed; windowed campaign/won/in-mission video play |
| Environment | Kernel Environment timeline | Required in both modes regardless of effects option; emits presentation cues | Authored progression, rain/trend/lightning values, check/thunder deadlines, scheduling phase, next cue sequence; no drops/camera fog | Mid-weather continuation, camera/effects-option invariant digest |
| Editor session lifecycle | Editor document; isolated GameSession | Immutable snapshot; play/pause; paused step = one tick; reset rebuilds same snapshot/seed | Runtime uses same sim sections and snapshot fingerprint; no editor document/undo, wall pacing or editor control state | Document/undo unchanged; one-tick step; reset replay equality |

Today RandomNumber/RollDice share GameOS state (`mclib/routines.cpp:74`, `:93`;
`GameOS/gameos/gameos.cpp:290`); even view-only water/radio draws still consume it. Commented rand blocks are not live consumers.
Audit every executable random call, including `code/weaponbolt.h:354` and `code/carnage.cpp:213`, before assigning its effects to a stream.
Script `seedrandom(n)` reseeds only ABL; `-1` derives from root seed, instance ID, executing tick and saved reseed ordinal, replacing `time(nullptr)`.
Record ordinal, instance, tick, requested seed and resolved seed; increment ordinal per accepted reseed. initialSeed stays immutable; drawCount counts lifetime draws across reseeds.
It never reseeds combat or a global CRT (`code/ablmc2.cpp:187`, `code/warrior.cpp:328`). Invalid reseeds reject with a log line.
Save readers validate required IDs, duplicates, supported algorithm versions, lengths and bounded queues before constructing owners.
Unknown required RNG records fail with one reason log; presentation state is optional for headless and excluded from authoritative equality.
Windowed writes its presentation streams too; headless loads/discards that section without consuming a sim draw.

## Include and link firewall

Create a `Sim/` target depending only on audited Core math, handles, readers, log sink and deterministic VM host interfaces.
Game/presentation adapters may read snapshots and submit commands; Sim cannot include or reference Camera/eye/UserInput/SoundSystem,
GameOS, RenderCore, RenderWorld, SDL, GPU, windows.h, platform Clock or chrono/system clocks, even through a PCH or transitively.
Ban timeGetTime/GetTickCount/QueryPerformanceCounter/time/clock/gos_GetElapsedTime, CRT rand/srand and global gos_rand/gos_srand in Sim/VM.
No function-pointer or extern shim granting sim camera/device/clock access. File IO occurs in loaders, not through a tick's environment.

Extend `scripts/check-include-firewall.sh` (currently render-only scope at `:24`) with the sim scope and forbidden symbols,
and a dependency walk over resolved includes for the exact Sim target (including generated headers and PCH).
Use CMake's explicit sim source manifest during extraction so unrelocated sim TUs cannot escape a directory-only scan.
Compiler include/dependency output plus AST symbol references must reject aliases/macros/indirect includes; missing manifest inputs fail closed.
Check the target link graph for prohibited libraries and build the shared kernel with no presentation include directories or libraries.
Negative fixtures cover direct/transitive Camera, a timing alias, VM wall seed, PCH leakage and an extern clock shim, with clean twins.
Integrate this into the existing firewall contract row; subsume the narrower `scripts/check-sim-no-audio-reads.sh` once equivalent tests pass.
No broad allowlist; temporary migration exceptions name a TU, dependency, reason and removal batch. Final kernel has zero exceptions.

## SIMID v2 and proofs

Keep the existing v1 encoder unchanged while adding v2 in parallel. Its whole-line comparator (`tools/sim_determinism.py:110`)
currently includes app/weather counters; `visible_chain` (`:101`) strips only strict float bits, so it is not a headless oracle.
V2 emits `{schema, ruleset, tick, authoritative, subsystem digests}` after each completed tick, including empty-mover missions.
Hash explicit fixed-endian fields in stable handle/instance/stream-ID order, with no pointers, padding, unordered traversal or wall timestamps.
Authoritative coverage includes live object existence/generation, transforms/physics, health/ammo/cooldowns, teams/objectives/resources,
turret/building state, sensors/contacts, path requests/results/cursors, execution and ingress queues, script receipts,
ABL FSM/statics/globals/arrays, schedules/reseed ordinal/history, logical media/cues, Environment, rootSeed and every sim RNG initialSeed/state/count/version.
Do not hash cosmetic gestures/animation frames, camera, visibility caches, app/drop counters or presentation events already consumed.
Separate presentation digests aid capture diagnosis; strict canonical sim float bits are required for same-build parity,
while quantized/visual records remain useful diagnostics, never substitutes for authoritative lockstep equality.
Save and digest share field definitions but distinct encodings; content identity and schema versions accompany the digest.
Upgrade loose-data fingerprinting from path/size (`tools/sim_determinism.py:79`) to resolved content bytes and override precedence.
Comparator requires matching ruleset, seed, manifest, initial state, order log, contiguous ticks and exact requested endpoint;
missing fields, duplicate ticks, early exit or a short common prefix cannot pass. Keep first-difference subsystem/entity diagnostics and DT stacks.

## Migration order and acceptance

1. Freeze current content, seeds and mc2_03/10/24/16 baselines; verify zero-init, appearance split and music liveness.
   Run existing check commands and 12 counted mc2_24 pairs beyond tick 4000; retain artifacts on first failure.
2. Extract one-step kernel and separate UI/camera/HUD/visual work in small batches, preserving dt, call order and shared RNG sequence.
   Dual-run/read-only shadow collectors prove v1 graded/objects/ABL unchanged at every batch; classify changed visual counters separately.
   Loader extraction still performs current shared-stream initialization draws until streams are deliberately separated.
3. Introduce v2 collectors and explicit owner-held RNG state without changing draw order; v1 remains byte-identical on the reference arm.
   Move digest scratch vectors (`code/sim_identity.cpp:223`, `:228`) into the session's collector owner; no static-duration scratch owners remain.
   Test forbidden-dependency fixtures and save/restore all newly owned state. No collector draws or mutates simulation.
4. Activate tick-derived clocks, next-tick commands and independent streams as explicit ruleset changes, one change per reviewed batch.
   Reconstructing time from ticks and splitting a shared RNG necessarily can change old trajectories; an unchanged old hash is not a valid claim.
   For each change, freeze its new reference arm and prove the extracted kernel equals it tick-for-tick; show old/new first difference,
   expected clock/order/draw cause and affected scenario tests. Never silently rebaseline to excuse an extraction regression.
5. Add accumulator/speed scheduling around that reference kernel; same tick/order replay must have identical v2 at 0.5x/1x/1.5x/2x,
   15/30/60/144 presentation fps, pauses, irregular pumps and capped stalls. No tick is skipped; deadlines/path fairness remain live.
6. Switch to the shared loader/headless runner; same initial state and envelopes produce identical v2 at EVERY completed tick.
   Save at K, resume to N in both modes and compare against uninterrupted N, including a pending order/path and mid-track script state.
7. Run tier2 headless, keep windowed/video/editor floors, remove temporary exceptions and delete the obsolete retail path/gates.
   Every subsequent ownership or math batch must keep the frozen authoritative digest unchanged; only reviewed ruleset changes replace baselines.

Existing harness recipes (future implementation proofs; not run for this document):

```text
py -3 tools/sim_determinism.py check mc2_03
py -3 tools/sim_determinism.py check mc2_10
py -3 tools/sim_determinism.py check mc2_24
py -3 tools/sim_determinism.py check mc2_16
py -3 tools/sim_determinism.py pairs-until-fail --mission mc2_24 --max 12 --duration 60 --out scratch/s1-pairs
py -3 tools/sim_determinism.py run mc2_10 --duration 60 --env-a MC2_CAMERA_PARK=-1500,2500,0,60,800@300 --env-b MC2_CAMERA_PARK=-2000,2900,90,50,600@300
```

Repeat the camera-park pair on mc2_24 and with effects disabled; require authoritative equality after parking at tick 300,
while allowing presentation counters to differ. Existing fixtures are render-stamped; convert @300 to tick scheduling for the new harness.
Use pairs-until-fail until 12 passes (30 at final acceptance); STARVED pairs do not count (`tools/sim_determinism.py:245`).
Retain host-load ledger, manifests and both logs on FAIL; three consecutive STARVED runs are inconclusive, not success.
Windowed proofs continue using normal host leases. New headless runs have exact tick budgets and process timeouts, not frame starvation excuses.
Required implementation gates: offline contracts/doctest first, RelWithDebInfo build/all-targets, ASan, tier1, headless tier2 and save/resume parity.
Render/video/editor checks remain necessary when extraction touches their paths; digest equality alone cannot detect missing presentation.

## Retail wall-clock deletion, risks and owner decision

Delete mission_update's fixed/retail/paused wall-clock branches, LastTimeGetTime, dynamicFrameTiming, forcedFrameRate sim adjustment
and sim-speed dt multiplication in mechcmd2_frame. Move wall/sim ratio diagnostics to the scheduler.
Retire MC2_SMOKE_FIXED_TIMESTEP and MC2_DETERMINISTIC_RNG after callers/baselines use the unconditional kernel;
seed overrides and compile-time MC2_SIM_TRACE remain harness inputs/diagnostics. No hidden retail CRT or variable-dt fallback ships.
Audit time calls and seed callbacks in the VM and startup; platform/UI clocks survive only outside the kernel.

Risks: animation events may trigger damage or unlock motion; separate event timing from display animation before skipping appearances.
ABL camera/callout/audio reads and immediate order results can affect mod scripts; preserve documented logical semantics and test stock/mod corpora.
Float-time drift, initialization draws, system ordering, pool reuse and path budget phases can move outcomes despite equal mover hashes.
Headless can conceal GPU/heap/Tracy timing defects; keep windowed/ASan proofs and deterministic repro artifacts, not obsolete known-flake assumptions.
The cap sacrifices wall-rate under load; stress-test responsiveness and tick fairness, and expose overload in diagnostics.
Strict cross-platform floating identity is unproven; pin compiler/FP policy now and make Linux/macOS replay parity an explicit later acceptance gate.

S3/B7's wire catalogue implements the shared table above. No raw struct/vtable dumps, pointers, wall accumulator or physical camera state enters authoritative saves.
Readers reject malformed records/content with a log line and never crash; optional presentation cannot change authoritative equality.

Owner ruling: camera-reading scripts receive the last scripted camera value. Live player-camera reads remain banned; mc2_01 restoring scripted rotation at its mid-mission read is an accepted compatibility difference.

## Q4 / CODEX-Q4-S1-STEP2 claimed amendment (2026-10-13)

The isolated API files are `code/sim_rng.{h,cpp}` and `code/tactical_order_queue.{h,cpp}`.
Values live in `mc2::sim`; `ObjectRef` is the shared `{slot:u32,generation:u32}` representation.
No live sim call sites change. Root CMake disables the PCH for these two TUs; scope metadata
names those exclusions and the firewall verifies the unconditional source property declaration.

The following previously unspecified values are frozen for these skeleton codecs:

- RNG records use ascending numeric stream IDs. `RngState` contains exactly eight records.
  The ABL host separately owns `AblReseedState {reseedOrdinal:u64,reseedHistory:AblReseed[]}`.
  Accepted reseeds start at ordinal 1, have nondecreasing executing ticks and preserve lifetime draw counts.
  For `-1`, let u32 `v = rootSeed XOR instanceId XOR low32(tick) XOR high32(tick)
  XOR low32(ordinal) XOR high32(ordinal) XOR 0x41424C52`; then `v ^= v >> 16`,
  `v *= 0x7FEB352D`, `v ^= v >> 15`, `v *= 0x846CA68B`, `v ^= v >> 16`.
  Products wrap modulo 2^32. Explicit seeds accept 0..INT32_MAX; values below -1 reject.
- Origin tags retain 0 player/1 commander/2 self; tactical kinds retain 0..26 from `tacordr.h`,
  with 27 airstrike/28 reinforcement/29 mission control; kind 0 cannot be admitted.
  Other tactical enum values preserve their named retail meanings, declared explicitly in the header.
  Support kinds carry one tagged SupportCommand in place of the move/attack optionals.
  Support and issuer variant tags are u8, one-based in their declared schema order; other enums are u32,
  except FireRange:i32 (-4..3). WayPath carries points length, points, curPoint, then exactly that many mode tags.
- Source tags are 1 UI/2 ABL/3 replay/4 future peer. Only ABL sources carry instanceId;
  their issuer must match it. UI/peer issuers are commander IDs; replay can carry either.
  Issuer tags are 1 commander/2 instance. A required admission interface supplies mission permissions;
  a required application interface checks live authority, handle generations and legal targets before effects.
  Peer registration is a protocol value only; it starts no transport.
- Sequences start at 0, admit exactly nextSequence, reject previously admitted identities and gaps,
  and never wrap. Receipt IDs start at 1. A draw or increment requiring an unrepresentable next value rejects.
  Receipt IDs follow ingress call order; replay preserves that order, and a future peer must establish
  a stable admission order before calling submit. Canonical drain order does not renumber issued receipts.
  Queue-mode tags are 1 replace/2 append/3 prepend. Admission status is 1 accepted;
  application status is 0 pending/1 applied/2 rejected/3 partial, recipient result 1 applied/2 rejected.
  Reason 0 means applied; nonzero means rejected. Application and recipients follow canonical batch/roster order.
- Caps are 256 sources, 4096 pending envelopes, 8192 retained receipts, 128 recipients/waypoints/roster entries,
  and 65536 ABL reseeds. Acknowledgement marks a completed receipt; explicit retirement releases acknowledged
  records through a chosen ID, retaining source sequence state. On restore, nextReceiptId must equal
  one plus the overflow-checked sum of all sources' nextSequence values, even after every receipt retires.
  Capacity errors consume neither sequences nor IDs.
  Completed tick comes from Clock when restoring OrderIngress; executing tick/drained batch are transient.
  Queue save/restore is forbidden during a tick, and endTick requires final results for the drained batch.
- Movement kinds 2..10/19/20 require move parameters; point/path movements require at least one point.
  Wait may carry move parameters. Attack kinds 16/17 require attack parameters. Object-directed kinds
  3/5/8/9/16/21/22/23/24/26 require a target. Guard (10) accepts either an object target or a valid
  nonempty point/path in its move parameters, preserving retail Guard-at-point; other kinds carry no target.
  Support commands have no recipient roster. Reinforcement IDs retain authored roster order and must be unique.
  Strike tags are 1 small/2 large/3 sensor. Mission-control actions are 1 set objective `{objectiveId:u32,completed:bool}`,
  2 set resource `{commanderId:u32,resourcePoints:i32 >= 0}`, 3 set mission result `{result:u32:0 running/1 won/2 lost}`;
  argument tags match those action IDs. Additional mission-control actions need an explicit schema extension.
