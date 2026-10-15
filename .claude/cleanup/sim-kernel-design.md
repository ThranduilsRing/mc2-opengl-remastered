# S1: one 30 Hz simulation kernel

Tick count owns time; TacticalOrder owns ingress. Windowed, editor and headless sessions share loaders, state and digests. Use C++23 values with explicit lifetime owners, no static-duration owners or permanent feature gates. Mods remain supported; the single final release follows migration, Vulkan and editor completion.

## Tick loop and snapshots

GameSession owns SimKernel, pacing and optional presentation; SimKernel owns systems and completed:u64. Tick zero is fully loaded state. A step opens executing=completed+1, drains once, runs systems in their existing order, commits completed, clears executing, publishes an immutable snapshot and emits one digest. Render-only pumps never advance ticks.
Gameplay dt is exactly the representable float 1.0f/30.0f. Seconds and ABL milliseconds derive from tick/30.0 and tick*(1000.0/30.0); deadlines round positive durations upward. Replace accidental render timing with explicit tick phases; brain cadences share one saved integer schedule registry.
Pacing uses integer/rational credit: clamp wall delta to 250 ms, apply rate 1/2, 1, 3/2 or 2, and execute at most eight ticks per pump. Discard excess whole-tick credit, retain the fraction and log overload. Never enlarge dt, skip tick numbers or accumulate unbounded debt; overload slows game time.
Rates mean 15/30/45/60 ticks per wall second. Account elapsed time at the old rate before applying a pump-boundary change. Pause/resume/load clear credit and rebase wall time; pause retains commands for the next tick. Wall credit, pause and speed are presentation state.
Audio, camera smoothing, menu fades and input timers use presentation time. Gameplay Timer deadlines use ticks; updateWhilePaused UI timers remain outside Sim.
Initially present the latest snapshot without interpolation, avoiding blends across destruction, teleport or load. Capture pins a completed tick and freezes cosmetics. Later interpolation remains presentation-only.
Snapshots carry stable handles, transforms, HP/status, HUD paths and events, never mutable-sim pointers. Preserve ordered single-threaded path/object/collision/sensor/brain work during extraction. Animation-driven damage and motion unlocks require deterministic sim events before appearances can be omitted.

### Editor session lifecycle

An immutable document/content/seed snapshot creates an isolated GameSession at tick zero. Play shares pacing; paused Step advances exactly once; Reset rebuilds the same snapshot and seed. These operations never mutate document or undo history; edits require a new explicit snapshot. Runtime save/load uses the same sim schema, excluding editor controls.

## TacticalOrder queue: one ingress

The mission ingress queue is distinct from per-warrior execution queues. All UI, ABL, replay, support and future peer inputs use owned typed orders, never packed legacy words or execution pointers.
TacticalOrder carries orderId/origin/kind/unitOrder/subOrder, optional move/attack/target, formationOrdinal and groupFlags. OrderExecution adds issued/notBefore/lastUpdate ticks, stage/status, lastMoveGoal and optional point-unit handle.
OrderEnvelope is {applyTick,sourceId,sequence,issuer,recipients[],queueMode,payload}. submit validates shape, finite coordinates, bounds, permissions, identity and capacity; application revalidates live generations, authority and targets. Rejections are deterministic and logged, never crashes or silent drops.
Canonical order is (applyTick,sourceId,sequence,recipient ordinal). Stable recorded roster order preserves formations/selectionIndex. During tick k, submitNext targets k+1; between ticks it targets completed+1. Explicit later ticks are allowed; earlier ones reject. Future lockstep waits for a complete batch rather than inserting retroactively; transport remains absent.
Submission captures handles, canonical world points, range and modifiers outside Sim. Formation goals use sim data at application, never live selection, screen coordinates or waypoint keys. Speed/pause/view actions stay outside ingress.
ABL order builtins submit with saved instance source/sequence; synchronous success means admission only. Application results are script-visible receipts; queries remain queries. Internal suborders/retries stay in the executing system, avoiding recursive enqueue; non-order mutations remain deterministic VM host operations.
Replays use the same envelopes and pin ruleset, digest schema, resolved content, seed, initial-save digest and target tick. Receipt IDs follow admission order; replay preserves it and future peers must establish it before submission.

## Presentation dependencies and the headless runner

The shared headless application mode accepts mission, exact tick budget, optional seed/replay/save and uses the windowed validating loader, ABL host, tick and digest. Terminal results are explicit; short runs cannot satisfy requested endpoints.
Headless creates no window/input/GPU/textures/shapes/appearance/HUD/audio device/video decoder/presentation RNG owners. Bounds, collision, weapon timings and media durations load independently; never replace gameplay data formerly obtained from appearance with dummy values. Damage/fall recovery remain sim; path projection belongs to HUD.
Separate terrain collision/navigation from visibility, textures, baking and visual displacement. Keep gameplay craters only where they affect navigation/damage; cosmetic rotation is presentation data.
Environment deterministically owns authored weather progression and rain/trend/lightning deadlines, independently of effects settings. RenderWeather owns camera-local drops. Any gameplay weather consumer reads Environment, not lighting/camera.
Logical music/voice/video requests complete from ticks and validated asset durations, never device callbacks. ABL camera getters return the last scripted value; setters emit cues, never expose the physical camera. mc2_01's changed mid-mission scripted rotation restore is accepted. forceMovieEnd/callout/voice host semantics still require audit.
Windowed cinematic and in-mission videos remain required, including campaign-start/win/in-mission playback controls; headless logical completion cannot prove playback.
The harness headless branch creates a background process with per-run scratch/log/save isolation and immutable install inputs. It requires a boot capability proving presentation never initialized before omitting the desktop/GPU lease; foreign windowed-engine protection and install/deploy coordination remain.
Initially missions run sequentially. Tier2 retains its stock manifest/fixtures with tick budgets and tick-scheduled injections; grade load, progress, endpoint, events, save/resume, fingerprints and malformed-mod rejection. GPU/FPS/visual gates remain windowed.

## RNG streams and save ownership

Preserve defined u32 LCG arithmetic: state=state*1664525+1013904223, output high 15 bits, with existing shaping; standard distributions are unsuitable. Invalid ranges reject without drawing.
Choose a random u32 root seed once outside Sim for fresh games; harness seeds are explicit and saves restore verbatim. Each system owns its stream; SimKernel owns the inventory. New initial state is rootSeed XOR fixed streamId, never enum position or std::hash.
Each stream record R is {streamId,algorithmId,algorithmVersion,initialSeed:u32,state:u32,drawCount:u64}, restored before draws. Seed alone cannot resume; initialSeed stays immutable and lifetime drawCount survives reseeds.
Only ABL seedrandom reseeds ABL. Explicit seeds accept 0..INT32_MAX; below -1 rejects. For -1 derive from root seed, instance, executing tick and saved ordinal, never wall time. History records ordinal/instance/tick/requested/resolved seeds, starting at ordinal 1 with nondecreasing ticks.

## Shared sim/save contract

This is the shared ownership contract with [S3/B7](cxx23-step4-save-schema-design.md). All authoritative rows participate in digests and validated bit-exact save roundtrips. Required RngState contains eight sim streams; optional Presentation streams are written windowed and discarded headless without sim draws.

| State / fixed ID | Owner and saved contract |
|---|---|
| Clocks | SimKernel: completedTick:u64, scheduling phases/next-due ticks and SimRules; derive other clock views. No executing tick, wall credit, pause or speed. |
| MissionInit / 0x494E4954 | Loader/kernel: required R even after initialization. |
| Combat / 0x434F4D42 | CombatSystem: required R for hit/critical rolls. |
| PilotChecks / 0x50494C54 | WarriorSystem: required R for piloting/skills. |
| AI / 0x41494445 | DecisionSystem: required R; tactical hash derives solely from root seed/tick/handle, not a separate seed. |
| Path / 0x50415448 | PathSystem: required R and ordered requests/results/retries/cursors. |
| ABL / 0x41424C52 | Mission VM host: required R plus reseed ordinal/history. |
| Physical / 0x50485953 | Impact system: required R for gameplay falls/hits. |
| Environment / 0x454E5652 | Environment: required R plus authored progression, rain/trend/lightning values, check/thunder deadlines, scheduling phase and next cue sequence. No camera/drop state. |
| Appearance / 0x41505052 | RenderWorld appearance: optional R. |
| VFX / 0x56465852 | RenderWorld effects: optional R. |
| Decals / 0x4445434C | RenderWorld decals: optional R. |
| RainDrops / 0x5241494E | RenderWeather: optional R. |
| TerrainVisual / 0x54564953 | Render terrain: optional R and rebuild recipe; rebuilding consumes no sim draws. |
| AudioVariation / 0x41554452 | Presentation audio: optional R; device/ack variation cannot affect logical media or Sim. |
| Orders | Kernel ingress/warrior execution: pending envelopes, source next/last sequences, receipts/results/acknowledgements and active/queued OrderExecution. |
| ABL state | Mission host: module/instance order, typed eternals/statics/FSM/return/order flags, integer schedules, source/receipt bindings and reseed state; VM returns before save boundary. |
| Media/cues | Logical host: ordered requests, stable IDs/next sequence, kind/asset/source, start/end/duration ticks, loop/status, typed trajectories and completion/consumption latches. |
| Editor session | Snapshot fingerprint and normal sim sections; never document/undo or editor/pacing controls. |

Readers validate required IDs, duplicates, algorithm versions, lengths and bounded queues before publishing owners. Unknown required RNG records reject once; optional presentation is excluded from authoritative equality. Every executable random consumer, including initialization and view-only calls, must belong to an explicit stream.

## Include and link firewall

Sim depends only on audited Core math, handles, readers, logging and deterministic VM-host interfaces. Presentation reads snapshots/submits orders; Sim/VM cannot reach Camera/eye/input/audio, GameOS/RenderCore/RenderWorld/SDL/GPU/Windows, platform clocks, wall-time functions, CRT rand/srand or global gos RNG, even through aliases, macros, PCH, transitive includes, externs or function pointers.
File IO belongs to loaders. Enforce explicit source manifests, resolved dependency/symbol walks and target link graphs; missing inputs fail closed. Integrate into the existing firewall row and subsume the narrower audio-read check only with equivalent coverage. Temporary exceptions name TU/dependency/reason/removal batch; the final kernel has none.

## SIMID v2 and proofs

Keep v1 unchanged during extraction; its presentation counters and visible-chain float masking are unsuitable for headless equality. V2 emits schema/ruleset/tick/authoritative/subsystem digests after every completed tick, including empty-mover missions.
Hash explicit fixed-endian fields in stable handle/instance/stream order, never padding/pointers/unordered traversal/wall stamps. Cover existence/generations, transforms/physics, health/ammo/cooldowns, teams/objectives/resources, buildings/turrets, sensors, paths, execution/ingress/receipts, ABL state, schedules/reseeds, logical media, Environment, root seed and all sim R fields.
Exclude cosmetics/animation, camera, visibility caches, presentation counters and consumed presentation events. Same-build strict canonical float bits are authoritative; quantized/visual/presentation digests diagnose only. Save and digest share field definitions, with distinct encodings and explicit content/schema identity.
Resolved fingerprints include content bytes and overlay precedence. Comparison requires matching ruleset/seed/manifest/initial state/order log, contiguous ticks and exact endpoint; missing/duplicate fields, early exit and short common prefixes fail. Preserve first-difference subsystem/entity diagnostics and trace stacks.

## Migration order and acceptance

Preserve dt, call order, shared draws and initialization draws during extraction; collectors never draw or mutate. Owned RNG/v2 collectors precede deliberate clock, next-tick and stream-splitting ruleset changes. Such changes may alter trajectories and need an explicit reference with explained first difference; never silently rebaseline an extraction regression.
The reference kernel must replay identically at all supported speeds, presentation rates, pauses, irregular pumps and capped stalls, with live deadlines/path fairness. Windowed/headless equality applies at every completed tick; completed-boundary save/resume must match uninterrupted execution, including pending paths/orders and mid-track media.
Retain stock mc2_03/10/24/16 coverage, camera/effects independence and exact-budget headless tier2, plus windowed/render/video/editor, offline contracts, RelWithDebInfo and relevant ASan checks. Starvation is inconclusive, never success. Only reviewed ruleset changes replace authoritative references.
Delete retail/fixed/paused wall-clock branches, LastTimeGetTime, dynamicFrameTiming, forcedFrameRate sim adjustment and speed-dt multiplication after migration. Retire fixed-timestep/deterministic-RNG gates once unconditional; seed overrides and compile-time trace remain harness diagnostics. No variable-dt or CRT fallback ships.

## Retail wall-clock deletion, risks and owner decision

Open risks: animation timing coupled to damage/motion; mod ABL immediate-order/media semantics; initialization draws, float drift, ordering, pool reuse and path-budget phases; GPU/heap/timing defects invisible headless; scheduler overload responsiveness. Keep windowed/ASan coverage and deterministic repro inputs.
Cross-platform strict floats remain unproven: pin compiler/FP policy now; Linux/macOS replay equality is a later gate. No raw struct/vtable dumps or physical camera state enter authoritative saves. Invalid readers log once and reject without crashing.

## Wire schema

The standalone value APIs in mc2::sim are sim_rng and tactical_order_queue; ObjectRef={slot:u32,generation:u32}. Both TUs exclude the PCH unconditionally, enforced by scope metadata/firewall.
RNG records sort by numeric stream ID. AblReseedState separately owns ordinal/history. For -1 reseeds, u32 v=rootSeed XOR instanceId XOR low32(tick) XOR high32(tick) XOR low32(ordinal) XOR high32(ordinal) XOR 0x41424C52; then v^=v>>16; v*=0x7FEB352D; v^=v>>15; v*=0x846CA68B; v^=v>>16, modulo 2^32.

| Wire contract | Frozen values/invariants |
|---|---|
| Origins/kinds | Origins 0 player/1 commander/2 self; kinds 0..26 retain retail tags, 27 airstrike/28 reinforcement/29 mission control. Kind 0 cannot enter ingress. Other tactical enums retain explicit retail values. |
| Scalars/variants | Support/issuer tags are one-based u8 in schema order; other enums u32 except FireRange:i32 (-4..3). WayPath encodes length, points, curPoint, exactly length modes. Support replaces move/attack optionals. |
| Sources/issuers | Sources 1 UI/2 ABL/3 replay/4 peer/5 MissionContent. Only ABL carries instanceId and matching issuer; UI/peer use commander, replay either. Issuer tags 1 commander/2 instance. Required admission/application interfaces enforce permissions and live validity; peer is a protocol tag, not transport. |
| Sequences/IDs | Sequences start 0 and admit exactly nextSequence, rejecting gaps/duplicates; receipts start 1. No counter/draw wraps. Capacity failure consumes neither. Restore nextReceiptId=1+overflow-checked sum of source nextSequence, even after retirement. |
| Queue/status | Modes 1 replace/2 append/3 prepend; admission 1 accepted; application 0 pending/1 installed (Applied alias)/2 rejected/3 partial; recipients 1 applied/2 rejected. Reason 0 means success, nonzero rejection. Results retain canonical batch/roster order. |
| Receipt payload | After recipient results: optional commandResult then acknowledged. Completed support has result {1 applied/2 rejected,reasonCode:u32} and no recipient results; completed unit orders only recipient results; pending neither. Command result/status agree. Invalid application outcomes use rejection 0xFFFFFFFF. Reasons survive restore. |
| Bounds/lifecycle | 256 sources, 4096 pending, 8192 retained receipts, 128 recipients/waypoints/roster entries, 65536 reseeds. Acknowledge completed receipts only; retire acknowledged through a chosen ID while retaining source history. Save/restore only between ticks; endTick requires final results for its batch. Restore Clock's completed tick before cross-validation. |
| Movement/targets | Kinds 2..10/19/20 require move; point/path movement needs points; Wait optionally carries move. Attack 16/17 requires attack. Targets required for 3/5/8/9/16/21/22/23/24/26; Guard 10 accepts target or nonempty point/path; others have none. |
| Support | No recipients; reinforcement IDs are unique and preserve authored order. Strike tags 1 small/2 large/3 sensor. Mission-control argument/action tags 1 objective {objectiveId:u32,completed:bool}, 2 resource {commanderId:u32,resourcePoints:i32>=0}, 3 result {result:u32:0 running/1 won/2 lost}. Other actions require a schema extension. |

## Mission ingress

Mission owns ingress, receipts and bindings by value. Destroy/reset clears them before actor teardown; session IDs come from a non-owning, never-wrapping process counter. Tokens {session:u64,receiptId:u64} require the expected session on submit/ack/query, so replacement even at the same address invalidates old tokens.
Bind after actors, pilots and commanders exist. A generation-1 roster slot records original Part, watch mapping, actor/pilot addresses and runtime Part ID, independently of gated watch generations. Resolve verifies existence/destruction and both watch directions; mismatch permanently retires it. No dynamic/replacement rebinding; future watch-ID reuse requires unconditional incarnation notification.
The simulation predicate is active && (!mc2SimTickFrozen(missionInterface) || MPlayer); freeze includes pause/freeze_movers, multiplayer retains its override. Drain before actor updates; contiguous completedTick is separate from retail turn. Every opened tick completes, including brain return 9999; inactive/frozen pumps neither drain nor increment.
MissionContent tag 5 has no instanceId, requires Commander origin/issuer and forbids support. Production accepts MissionContent or UI for the recipient commander; UI requires Player origin. Other producer integration remains pending.
The bounded adapter accepts Replace only, Stop=11, MoveToPoint=2, Patrol=7 and Traverse=6; origins map explicitly to retail Player=0/Commander=1. MoveTo is one finite point/cursor zero/Slow-or-Fast ordinary movement; reject jump/special/modifiers/formations/group/sub/unit payloads and orderId beyond signed32. Routes use the authored cap below.
Admission/application require existence, pilot, commander and mobility. A temporary distinct slot-code marker proves a fresh synchronous handoff even if old content matches; suppression restores the old code and rejects. Installation must match origin/time/flags and every supported move parameter/mode. Legacy assignment omits id and selection can change delayedTime, so neither is an installation serial.
MissionContent logs each rejection once, acknowledges completed receipts and retires after endTick, retaining source sequences. Authored diagnostics have one loader/receipt owner. Other consumers own acknowledgement; pending receipts cannot be acknowledged. Installed means slot handoff, not execution; evidence is captured synchronously before ABL replacement.

### Authored Part contract

Part keys remain the editor format, preserving untouched bytes and existing Brain/ABL assignments. An eligible Enhanced OPORD on the preserved original pilot wins, using the same predicate as OPORD population (gate, valid/live warrior, non-player archetype, supported primary type); suppression emits one info line naming the Part. Never dual-dispatch, position-match, auto-convert or mutate shared ABL.

| Field/identity | Contract |
|---|---|
| OrderType | Editor 0 None/no envelope, 1 Traverse once, 2 Patrol loop; these differ from queue kind tags. |
| OrderStance | 0 Cautious/1 Conservative/2 Normal/3 Aggressive/4 Berserker/5 Suicidal. Retain mission-owned and in evidence; execution policy remains to be defined, with no legacy layout change. |
| WaypointCount | None=0; routes=1..8. Queue cap 128 and legacy cap 15 do not widen authored content; reader/export changes must agree. |
| Waypoint{i}X/Y/Z | Required typed finite floats; X/Y strictly inside map bounds, Z finite. Missing type/value, duplicate, invalid or stray/excess keys reject atomically once with selected Part and reason; no defaults/partial route/truncation. |
| Presence | No order keys means absent; any without OrderType is malformed. |
| Alternate | Parse selected block, retain original/selected IDs separately and preserve original pilot; bind/admit after commanders, never by position. |

### Save/resume contract

A versioned Archive OrderIngress section must retain completed tick; sorted sources with tag/instance/next/last sequences; pending full envelopes/rosters; retained receipts with per-recipient reasons/status/acknowledgement; nextReceiptId; durable recipient Part/entity incarnation, generation and retirement state.
Exclude pointers/raw watch IDs/live session tokens/atomic allocator/executing batch/applicationActive/socket/fixture state. Save after tick completion and receipt consumption; restore tick before receipt cross-validation. Sources survive receipt retirement.
Validate candidates then publish atomically, one diagnostic on failure leaving live state intact. Rebind after Part/pilot reconstruction before updates, using fresh sessions and reacquired retained-receipt tokens; never restore by pointer or gated watch fallback. Save hooks require lane 1's claim release and remain unimplemented.

### Acceptance

Stock absent/None/rejected/suppressed content must cause zero handoffs: installation already flags new orders and can clear paths/attack state or call execute. The real initializer/handler/setter seam and disconnected-adapter oracle establish installation independently of receipts. The fingerprinted runtime witness must exceed retained-receipt capacity and fail when drain, completion or retirement is disconnected.
Pin exactly 606b584b3 and post-commit SHA256 binaries; require identical resolved content, seed and environment for strict mc2_03/10/24/16 chains and both camera arms. Missing budgets/fingerprints/success or any strict mismatch fails; never rewrite baselines. See [execution decisions and claims](sim-s4b2-execution.md) for route arbitration, restore and the default-off OPORD gate question.
