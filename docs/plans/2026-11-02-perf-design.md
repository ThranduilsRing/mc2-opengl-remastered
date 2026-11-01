# CPU / Vulkan performance push: design (2026-11-02)

Author: Opus design pass, read-only on engine code. Base: claude/nifty-mendeleev e5cf6ba9c.
Tags: **V** = verified in code or raw data (file:line or proof path given); **I** = inferred; **I-est** = an estimate with an
explicit range. Line numbers are nifty HEAD unless a lane worktree is named.
Briefs for the first wave: `docs/plans/2026-11-02-perf-briefs/`.

Running note (kept so work is not lost): inputs read = Opus review, Astra review, W96 plan, lane10 census
(0b75c8648, partial) + raw `E:/mc2-archive-2026-10/proofs/census/ab3/`, Fable speed / static-zero-cpu / rest-of-frame,
lane9 `claude/static-p1r` (tip 7bff14962, plan `docs/plans/2026-11-01-static-p1r.md`; the named
`2026-10-31-static-p1-continuation.md` does not exist on that branch), lane8 `claude/record-thread` (6de50923e, base
a06e91928, 195 commits behind nifty), lane4 save-capacity (8373afa33), MEMORY.md render and process rows.

---

## A. Reconciled facts and the corrected baseline

### A1. Current HEAD baseline (new: the census raw data)

Source: `E:/mc2-archive-2026-10/proofs/census/ab3/profile.json`, arm A = e5cf6ba9c (fingerprint `sha=e5cf6ba9c043
dirty=0` in `pair1-a/mc2_24.game.log`), mc2_24 `low_horizon_0` (bench_view -1216 -5737 alt 150 pitch 10), 1920x1080,
20 s, 5 runs, **Tracy connected**, host NOISY (reserved-core external CPU p95 9-41 %, machine about 80 %). Means are the
median over 5 runs of total/Frame count. My recomputation from the raw file, not the census table:

| Block (thread) | ms/frame | Notes |
|---|---:|---|
| Frame (main), mean / median / p95 | 4.574 / 4.321 / 6.206 | bench `cpu` rows agree: median 3.98-4.62 per run (V, frames.jsonl) |
| GameLogic (main), **one event** | 1.216 | zone count is 2.00/frame: it is nested twice (V: `GameOS/gameos/gameosmain_frame.cpp:41` wraps `code/mechcmd2_frame.cpp:325`); the tool merges same-name events (V: `tools/perf/mission_cpu_profile.py:141`), so "GameLogic 2.43" is double counted |
| . Presentation.Terrain (main) | 0.786 | the static walk, `code/objmgr_misc.cpp:569-618` (V) |
| . Presentation Movers/Other/Interface (main) | 0.075 / 0.077 / 0.142 | |
| . framesSinceActive sweep (main) | 0.044 | |
| ExtractRenderSnapshot (main) | 0.248 | ExtractRenderList 0.182 |
| DrawScreen (main) | 2.145 | median 1.99 |
| . RenderObjects.Terrain (main) | 0.706 | the render walk over `rl.terrain`, `code/objmgr.cpp:1075-1092` (V) |
| . GpuStaticProps (main) | 0.589 | props island 0.542: record-submit 0.216, instance upload 0.055 |
| . TerrainFlush / DynamicShadowPass / RegistryFlush | 0.109 / 0.101 / 0.071 | |
| . shadows / particles / water / vegetation / post / interface | 0.106 / 0.079 / 0.068 / 0.049 / 0.089 / 0.060 | |
| Residual = Frame - GameLogic - Extract - DrawScreen | 0.965 | = main blocked in `present_thread::waitSealed()` (I, strong; no zone exists) |
| VKIsl.Present.Frame (present WORKER) | 0.928 | TailRecord 0.869 (scope to function return, V `engine_present_vk.cpp:2504`) |
| . tracy::VkCtx::Collect (worker, Tracy only) | 0.274 | runs in `finishFrame` -> `gpuprof::frameEnd` (V `vk_gpu_profile.cpp:269-308`), only when Tracy is connected |
| . QueuePresent / Acquire / Submit / CompositeWait | 0.098 / 0.056 / 0.018 / 0.001 | QueuePresent runs after `seal()` (V `vulkan_context.cpp:588-592`), so it overlaps main |
| SimOverlap.Tick (sim WORKER) | 0.395 | 2.76 ms per tick, p95 4.2 (bench `tick` rows, V) at about 7 frames per tick |
| GPU frame (bench `gpu` rows) | about 2.2 | GPU is not binding at 1080p |
| Island submits (main) | 0.006-0.024 each | merged submit stays dead (census table) |

Shipping-relevant correction: without Tracy connected, Collect (0.27) disappears from the worker and therefore from
main's waitSealed, so a non-Tracy frame is about **4.0-4.3 ms median** on this pose (I-est). This matches the W86
3.974 figure only by coincidence of offsets; current HEAD has NOT improved since W86 (census conclusion, agreed).

### A2. Rulings on the coordinator's census message

| Claim | Ruling | Evidence |
|---|---|---|
| main-thread busy median 4.412 on HEAD | ACCEPT as a NOISY Tracy-on number; the definition subtracts Acquire/QueuePresent, which are WORKER zones (cross-thread subtraction, about 0.15 ms error) | `engine_present_vk.cpp:2482,2661` run on the worker (`present_thread.cpp:58`) |
| "GameLogic is about 2.4-2.6 ms of 4.6" | REJECT: double count; real 1.22 mean / 1.13 median | nested zones (A1) and count 7442 = 2 x 3721 frames |
| "Appearances + GameLogic about 3 ms combined" | REJECT: Presentation.Appearances (0.94) is inside GameLogic | zone tree |
| "TailRecord 0.86 ms main-thread" | PARTLY: it runs on the worker but main waits for it at the frame top, so it is on main's critical path; 0.27 of it is Tracy Collect and 0.10 is QueuePresent | A1 |
| lane8 +1.1 ms not attributable to S3 | AGREE, and add a stronger finding: in arm B, RenderObjects.Terrain fell 0.706 -> 0.148 while ExtractRenderSnapshot rose 0.248 -> 1.507 (ExtractRenderList 0.182 -> 1.031). S3 moved the terrain-object render walk into the extract (copied value work), not off main | census ab3 arm B |
| merged submit dead | AGREE | submit zones 0.006-0.024 |
| re-rank toward GameLogic/walk and the present tail | AGREE (ranking in D0) | A1 |

### A3. Where the two reviews disagree, and the ruling

| # | Opus | Astra | Ruling (evidence) |
|---|---|---|---|
| 1 | Props consume only the sun from the light row, so make instances light-independent (P0-2) | The "sun" is `getLight*(terrainLight)` = sun x I + ambient, clamped, with a night floor; trees disable the sun | BOTH RIGHT; Astra's formula is the contract. V: `mclib/bdactor_update.cpp:575-584` (I = `land->getTerrainLight(position)`, colour = `eye->getLightRed(I)`), `mclib/camera.cpp:752-768` (`float2long(lightRed*I + ambientRed)`, floor `MC2_NIGHT_AMBIENT_FLOOR`, clamp 255), trees put that colour in slot 1 and set `getWorldLight(0)->active=false` (`mclib/bdactor_tree.cpp:459-473`), the row excludes inactive lights (`mclib/object_light_row.h:16-35,221-233`), so tree rows carry no sun and the VS finds none (`shaders/vulkan/color_prop_tier1.vert:258-276`). Also new: the actor colour is computed ONCE (`fogLightSet`, reset only at init, `bdactor.cpp:279`, `bdactor_tree.cpp:37`), so stock buildings freeze the first-frame sun. |
| 2 | Light-generation churn will re-arm all statics in combat (P0-1) | Historical; the current generation returns 0 | Astra: on HEAD and lane9 the generation is constant 0 (census item 1; `bdactor_diag.cpp:39` in lane9). P0-1 is moot unless someone reintroduces a light hash. Design consequence: the contract (D1) removes the need for any light generation for props. |
| 3 | 1.1-1.3 ms reachable; GameLogic 2.06 | Not defensible | Astra. Plus the GameLogic double count makes Opus's GameLogic arithmetic 2x too high, and Fable's "Presentation.Terrain 1.234" is not reproduced on HEAD (0.786). |
| 4 | S4-lite = move waitSealed (0.3-0.5 ms) | Another wait follows at `retirePresentHandles`; worker waits include GPU slot waits | Astra on mechanism: `retirePresentHandles` calls `getOrCreateEngineVulkanContext()` (V `gos_host_vk.cpp:186-187`), which calls `waitSealed()` (V `vulkan_context.cpp:857-860`), as does `engineVulkanContextValid()` (`:862-865`). Opus on value: today CompositeWait is 0.001 ms, so on this pose the tail is CPU, not GPU. Both kept: the metric attributes waits causally (B), and the restructure creates a VK-free main prefix (D3). |
| 5 | Prop draws "one per type x packet x 2 passes" | The second pass covers only fringe packets | Astra (V `vulkan_color_prop_island.cpp:1573-1576,1610`); alphaTest==2 packets go to a late list (`:1611-1619`). |
| 6 | HZB: rotate cbuf | Unsafe: instance/visibility/projected buffers are shared | Astra (V `vulkan_post_hzb_native.cpp:146,708-713,748,758`): one `instances` buffer is host-written every frame after the wait. Per-slot cbuf AND per-slot buffers or nothing. |
| 7 | W96 camera removeWorldLight fix | False defect | Astra (V `mclib/camera.h:1427-1454`: mismatch path scans and calls `updateLights()` at :1453). |
| 8 | Opus "quiet-measure sums across threads" | (not addressed) | Partly: the tool merges same names across threads (V), but the GameLogic inflation is nesting on main, not threads. |

### A4. Other verified facts used below

- Frame structure on main (V `GameOS/gameos/gameosmain.cpp:1035-1131`): `waitSealed` -> `retirePresentHandles` + `joinTail`
  -> GameLogic (`gameosmain_frame.cpp:41`) -> ExtractRenderSnapshot -> DrawScreen (sim kick in `beginFrameDraw`,
  `sim_overlap.cpp:153`) -> `overlap::waitWorker()` -> counters -> frame cap -> `frameSwapWindow` (enqueue to the
  present worker, mailbox depth 1, `present_thread.cpp:113-122`). The worker overlaps main only during QueuePresent.
- Sim overlap (V `code/mission_update.cpp:1318-1367`): ticks run on the sim worker from the kick in `beginFrameDraw`
  until `waitWorker()` right after DrawScreen in the SAME frame. A 2.76 ms tick therefore hides only behind DrawScreen;
  any excess is a main stall on tick frames. When DrawScreen shrinks, every tick frame (about 1 in 7 at 230 fps) pays
  the remainder: **p95 < 1.5 ms is impossible while a tick is 2.7 ms and must join in-frame** (I, arithmetic).
- `freezeDrawFrame` copies the whole RenderList (three `std::vector<RenderView>`, about 250-300 B each with a 120+ B
  `BoltDraw` inline) and HudSnapshot (vectors with `std::string` per mover) into a `make_shared` every frame
  (V `code/render_view_store.cpp:34-38`, `code/render_view.h:46-76,80-117`). Size of terrain list = visible terrain
  objects (thousands on mc2_24), so this copy is O(objects) (I: cost not isolated; sits inside ExtractRenderSnapshot 0.248).
- Registry staleness coupling: a range whose multishape `cachedFrame != currentFrame` is dropped because its light
  slot "this frame was filled by a different actor" (V `gos_static_prop_registry_flush_range.cpp:41-50`). With props
  light-independent (D1) that reason disappears for colour props; mechs still use rows (`mech_tier1.vert:80,172,207`).
- Indirect draws: none indexed anywhere (V rg: only `vkCmdDrawIndirect` in `vk_vegetation_state.cpp:256-257`,
  `vulkan_water_island_run.cpp:369`). Device profile has Required BDA, descriptor indexing, dynamic rendering, sync2
  (V `vk_device_profile.h:150-200`) but no `multiDrawIndirect`, `drawIndirectCount`, `shaderDrawParameters` items.
- Host GPUs: the host has BOTH an RX 7900 XTX and an RTX 2070 SUPER; smoke pass/fail defaults to the 2070, perf runs
  to the XTX (V `scripts/run_smoke.py:647-649`). Memory's "GPU is now 2070" is resolved: both are present.
- Lane9 `static-p1r` already implemented frame sun + 4 B tint at offset 68 (7edf00228), with tint = terrainLight only
  (lane9 `mclib/bdactor_static.cpp` registerStatic diff) and frame sun = `lightRed/255` (lane9 `code/gamecam.cpp:401-404`).
  That drops the ambient term and the clamp, and gives trees a sun they never had: it VIOLATES the contract (D1) and
  will fail pixel identity for a reason the lane labels "intentional". It also changes the specular direction to the
  terrain sun (memory landmine `building-diffuse-stuff-mc2-frame-mix`: worldLights[0] direction != terrain sun).
- Lane9 P1 adds per-object work inside the walk (`preparePresentState`, LOD probe, bookkeeping; lane9
  `code/objmgr_misc.cpp` diff) and saves nothing yet: it must not land as a perf item on its own.
- Save-capacity (lane4) is a correctness lane, not part of this design; it is a hard-gate neighbour only. No perf slice
  below touches save code or section versions.

### A5. What is missing (census items not measured)

INERT reason histogram, draws per island per frame, light re-arms (moot at 0), night and battle scenarios, a waitSealed
zone, tail sub-zones, the non-Tracy frame, S3 overhead on a rebased lane8. Section C supplies the counters.

---

## B. The metric

### B1. Primary number: MAIN_BUSY (the "<1 ms")

Per frame, measured by the bench sink (`GameOS/gameos/battle_bench.h`, armed by the `bench_output` command, no env
gate), **with Tracy not connected**:

    MAIN_BUSY = frame_ms - gpu_wsi_wait_ms(main)

- `frame_ms` = bench `cpu` row (beginFrame at the top of `Frame` to endFrame after `frameSwapWindow`), as today.
- `gpu_wsi_wait_ms(main)` = time main spends in waits released by the GPU or WSI on the MAIN thread only: VKContext
  RingWait, every island FenceWait/SlotWait/timeline wait, HZB `post_prev_submit`, plus the GPU-caused share of
  waitSealed (B2). Worker zones are never subtracted from main.
- Waits on CPU workers stay IN (waitSealed CPU share, `overlap::waitWorker`, `joinTail`, job joins): they are critical path.
- DrawScreen < 0.1 ms uses the same rule on the main `DrawScreen` scope.

Pass (stock): median < 1.0 ms AND p95 < 1.5 ms AND tick-frame p95 < 2.0 ms (frames with `ticks > 0`, reported
separately so 30 Hz stalls cannot hide in the median) on mc2_01/03/10/24, fixed poses, 1080p, XTX.

### B2. Exclusive-cost accounting (one table per run; columns add to frame_ms on main)

New bench fields (C1 adds them; all steady_clock, no Tracy):

| Column | Thread | Definition |
|---|---|---|
| main_exec | main | frame_ms minus every wait below |
| wait_sealed_cpu | main | waitSealed time not intersecting a worker GPU/WSI wait interval |
| wait_sealed_gpu | main | waitSealed time intersecting worker Acquire / CompositeWait / fence intervals (interval intersection from per-frame timestamps) |
| wait_sim | main | `overlap::waitWorker` + `joinTail` |
| wait_gpu_main | main | RingWait + island Fence/Slot + HZB prev-submit waits |
| worker_exec | present worker | presentFrame minus its own GPU/WSI waits (and minus Collect when Tracy is live) |
| copies_allocs | main | ExtractRenderSnapshot copy bytes and heap-allocation count per frame (count via a per-frame counter in `freezeDrawFrame` and the arena) |
| island[i].record / submit / draws | main | per island: record ms, submit ms, draws recorded |
| tick_ms | sim worker | per tick, as today (`tick` rows) |

Attribution rule: a main wait is charged to the GPU only for the overlap with a recorded GPU/WSI wait on the thread it
waits for; otherwise it is charged to that thread's CPU. This fixes Astra's misclassification point without hiding
real worker CPU.

### B3. Sim-tick latency for 200 active movers

Fixture `battle200` = mc2_24 + 128 mech and 2 vehicle clones (existing UI-command spawn, quiet-measure method) + an
acknowledged attack order. Active = alive AND (moving OR firing) in the world row; the fixture fails unless
`units_alive >= 200` and `active >= 150` in every world sample after 3 s (new `active` field in the `world` row). Report
tick_ms p50/p95/max and tick-frame MAIN_BUSY p95. Interim target (Fable 10-29): tick p50 <= 2.5 ms, p95 <= 4 ms. Today:
70 stock movers already cost 2.76 ms p50 per tick on mc2_24 (Tracy on, NOISY), so 200 is far off (I).

### B4. Fixtures

- Fixed poses: mc2_01 and mc2_10 `sun_facing_low`, mc2_24 `low_horizon_0`, mc2_03 mission view (quiet-measure set).
- Moving camera `camera_path`: 20 s scripted path sent through `bench_view` at 10 Hz by the existing command driver
  (`tools/perf/mission_cpu_profile.py:51 drive_bookmarks`): orbit 360 deg at alt 150, low-angle pitch 10 pass, zoom
  in/out, pan across the base. The camera is not a fixed RTS camera: this fixture is the honest one for residency and
  LOD-band events.
- `battle200` (B3), day and `set_light night`.
- Statistics: 5 interleaved A/B pairs, paired bootstrap (`tools/perf/paired_cpu.py`), noise annotated not rejected
  (owner ruling 10-30); laptop for absolute ms.

---

## C. Fast-test scaffolding FIRST (lands on nifty immediately: tests, harnesses, counter-only)

Rule: counters first. No item below changes rendering or simulation; each fails loudly; each has a runtime.

### C1. Per-frame counters + smoke-lite assertions (`[PERF_COUNTERS v1]`)

- Files: `GameOS/gameos/battle_bench.h`, `mclib/battle_bench_sink.h` (new fields), `GameOS/gameos/gameosmain.cpp`
  (zones `Frame.WaitSealed`, `Frame.WaitWorker` around the existing calls; timestamps into the sink),
  `GameOS/gameos/vulkan_context.cpp` (record caller label of the first BLOCKING waitSealed per frame),
  island files: one `draws` increment next to each `vkCmdDraw*` (`vulkan_color_prop_island.cpp`, terrain, water, mech,
  legacy shape, sky, vfx, shadow), `code/objmgr_misc.cpp` (visited / inert-eligible / activations counts; reuse lane9's
  counters only if lane9 merges first, else plain integers), `code/render_view_store.cpp` (copy bytes, element counts).
- Interface: one log line every 300 frames and at run end:
  `[PERF_COUNTERS v1] frames=N draws.props=.. draws.terrain=.. ... walk.visited=.. walk.inert_eligible=..
  rearm=.. lightgen_bumps=.. extract.bytes=.. first_vk_wait_site=<label> first_vk_wait_ms_into_frame=..`
  plus the same fields in bench `cpu` rows.
- Smoke-lite: `scripts/smoke_lib/logparse.py` parses it; `scripts/smoke_lib/gates.py` gets a `perf_counter` bucket with
  per-mission ceilings in a new `tests/smoke/perf_counter_ceilings.json` (start = observed x 1.25; ratchet down as
  slices land): draws per island, rearm per frame = 0 on mc2_10/mc2_01, lightgen_bumps = 0, `persistent_vanish` = 0,
  `stale_frame_drop` steady-state = 0. Missing line = red (presence rule).
- Fails: a slice that raises draws, re-arms or copies past the ceiling turns smoke-lite red with the counter named.
- Runtime: zero extra game time (rides smoke-lite, 2 x 10 s); pytest for the parser < 2 s.

### C2. Lighting-contract parity counter (`[LIGHT_CONTRACT v1]`)

- Files: new header `mclib/prop_light_contract.h` (pure function `contractSun(S, A, F, I, policy) -> rgb8`, the camera
  formula), its doctest `tests/unit/test_prop_light_contract.cpp` (table: I in {0, 0.001, 0.5, 1}, ambient 0/64/255,
  floor 0/32/64, trees SUN_OFF), and a runtime check at the existing bake (`mclib/bdactor_lightbake.cpp`, where the row
  is built): compare the row's first infinite light colour with `contractSun` for that actor -> checks / mismatches.
- Fails: any building or tree whose old-row sun differs from the contract by > 1/255 per channel; smoke-lite asserts
  mismatches = 0 on mc2_01 day and mc2_10 night.
- Runtime: doctest < 1 s; runtime free (bake is per actor, rare).

### C3. GPU-cull parity vs CPU reference (`[CULL_PARITY v1]`)

- Files: `RenderCore/hzb_prop_bounds.h` is the CPU reference math; new doctest `tests/unit/test_hzb_cull_reference.cpp`
  (frustum AABB + HZB mip/footprint selection on synthetic depth pyramids, incl. the MIN-footprint and per-object LOD
  over-cull bugs from memory `hzb-static-prop-cull-low-roi`); runtime check in `vulkan_post_hzb_native.cpp`: every
  64th frame read the keep mask through the existing lagged readback ring and compare with the CPU reference computed
  on the same bounds and the same (lagged) HZB copy; count false-culls (GPU culled, CPU visible) and false-keeps.
- Fails: false-culls > 0 is red (a vanish bug); false-keeps are advisory (cost only).
- Runtime: doctest < 2 s; runtime one readback per 64 frames.

### C4. Frozen-world fixture (the replay harness that is possible on nifty today)

The sealed-packet replay ("record path with no sim or window") needs S3's sealed packets, which exist only on lane8.
Today's landable form: a dev-command fixture `bench_freeze on|off` (command channel, not an env gate) that freezes
the sim tick (existing `mc2SimTickFrozen` path used by `overlapKick`, `mission_update.cpp:1324`) and pins the camera,
so N frames re-run presentation + extract + record on an identical world. Output: the bench rows. Uses: variance-free
record/walk timing, residency oracle runs, and later the S3 replay. Stage 2 (after lane8 rebases): `MC2_FRAME_RECORD`
writes K sealed FrameDraws + native feeds to disk and `mc2_frame_replay.exe` (EXCLUDE_FROM_ALL target, like
`mc2_mod_load`, CMakeLists.txt:942) records them on a headless VK device and asserts byte-identical command streams
per island between two builds. Runtime stage 1: 20 s per mission; stage 2: < 30 s per replay set.

### C5. Static-walk micro-bench (counts x ns)

- Dev command `bench_walk N`: run the terrain-object presentation loop N times on the frozen world (C4) with per-step
  counters and timers: objects visited, recalcBounds calls, groundElevation calls, setObjectParameters, touch vs
  update, setVisibility, plus the render walk (`renderView` calls) and extract terrain entries.
- Output: `[WALK_BENCH v1] step=<name> calls=.. ns_per_call=.. total_ms=..` for mc2_24 and mc2_10.
- Purpose: the grounding skip and every walk cut is proven by calls x ns (the Opus/Fable rule), not by frame CI.
- Runtime: about 30 s per mission.

### C6. S4 race stress test

- Files: extend `tests/unit/test_present_handoff.cpp` with a jittered mailbox stress: 100k enqueue/seal cycles with
  random sleeps on both sides, asserting the ownership invariants of `mc2::overlap` (Main never owns Render while
  `!sealed`; seal happens before WSI; `waitSealed` returns only when sealed) and that a main-side VK access between
  enqueue and seal is caught; plus a TSAN-free logic checker (sequence log compared against the allowed state machine).
- Game-side: smoke-lite run with `MC2_SIM_OVERLAP_AUDIT` and the existing jitter (`jitterSleep`, `sim_overlap.cpp:157`)
  on mc2_10, grep VIOLATION = 0 (runner fails on it).
- Runtime: doctest < 10 s; game part rides smoke-lite.

---

## D. Perf slices

### D0. Ranking (CPU ms toward <1 ms) / (effort x risk), mc2_24 low_horizon, HEAD, non-additive

| Rank | Slice | Measured block it attacks | Expected MAIN_BUSY gain | Effort | Risk | Score |
|---:|---|---|---|---|---|---|
| 1 | D2 static walk -> ACTIVE-only (presentation + render walk + extract entries), needs D1 | 0.786 + 0.706 + about 0.15 + 0.044 = about 1.69 | -0.9 .. -1.5 (I-est; active share unmeasured) | L | M | highest |
| 2 | D1a lighting contract (prerequisite; no ms of its own) | light-row coupling | 0 .. -0.05 | S-M | M (look) | gate for 1 and 4 |
| 3 | D3 present tail: decompose, cut, then overlap (S4-lite) | waitSealed about 0.83 (0.55 without Tracy) | -0.15 .. -0.45 | S then M | M | high |
| 4 | D1b GPU-driven props (MDI + count) | props 0.589 + registry flush 0.071 | -0.15 .. -0.45 (step 1 at most 0.2) | M-L | M | medium |
| 5 | D3c record thread S4 (lane8) | the DrawScreen remainder after 1+4 (about 0.8-1.1) | -0.5 .. -0.9 | L | H | needed for DrawScreen < 0.1 |
| 6 | D4 backlog (sim tick pipelining, extract copies, interface, shadows, terrain, audio, allocs) | see D4 | each 0.02-0.2, tick stalls p95-critical | S-M each | L-M | measured order |
| 7 | D5 update-rate budgeting (UE row 6) | movers 0.15 main, SimPose 0.29 worker | -0.05 .. -0.1 stock; battle larger | S-M | L-M | battle |

Overlap (do NOT add): D2 shrinks GameLogic, which shrinks the VK-free main prefix D3's overlap can hide behind; D1b's
step 2 and D2 both remove the per-frame feed build; D3c's gain is whatever record work D1b/D2 did not already delete.
Honest compound forecast for mc2_24 after D1+D2+D3+D1b (2 weeks): MAIN_BUSY median **2.0-2.8 ms** (I-est), mc2_01
**0.7-1.0 ms** (W86 Frame 1.29 with GameLogic really about 0.22). The stock <1 ms on mc2_24 needs D3c plus D4 and
the tick fix; it is not a 2-week deliverable.

### D1. Light-independent props, then GPU-driven prop draws

#### D1a. LIGHTING CONTRACT v1 (colour-prop consumers: `color_prop_tier1.vert`)

Inputs per frame (frame UBO, `RenderCore/gpu_abi/GpuColorFrameUBO.h`): S = camera `lightRed/Green/Blue` (0-255, after
`camera_daylight.cpp` interpolation), A = `ambientRed/Green/Blue`, F = night floor (`MC2_NIGHT_AMBIENT_FLOOR`, boot
constant, default 0), Dspec = the direction of `worldLights[0]` (the one the row carried; NOT the terrain sun, see A4).
Inputs per instance (light-independent, written once at recipe build, 4 B at offset 68 of the 112 B record, lane9 ABI):
I8 = quantised `land->getTerrainLight(position)` (map data, static per mission, I), policy bits:
SUN_OFF (trees, `bdactor_tree.cpp:473`), NEUTRAL (non-actor submissions: keep today's behaviour, audit list in brief).

Formula (shared C++/GLSL, C2): per channel, if I > 0: c = trunc_or_round_as(float2long)(S*I + A); c = max(c, F);
c = min(c, 255); else c = max(A, F). sun = SUN_OFF ? 0 : c/255. Direction for specular = Dspec. Ambient: unchanged
(SH only, R2). Emission: windows and hot colours (`kFlagIsWindow`, seed path, `aRGBHighlight`), lights-out flag and
night lamps (pool lights in the FS, `color_prop_tier1.frag:136-138`) untouched. Fog: per-instance fogRGB stays as is.
Weather: MC2 has no weather lighting term (rg `weather` in camera.cpp: none, V); nothing to preserve. Mods: same shader,
override bit 6; mod scripts that change daylight get live behaviour (below).

Frozen vs live: stock bakes the colour once per actor (A3 row 1). The contract evaluates S and A each frame. That is
identical whenever S and A have not changed since the actor's first update, i.e. all stock missions without a
`time_of_day` change (I; C1 adds a "camera light changed since mission start" counter to prove it per run). Intentional
change, the only one: after a daylight change (dev `time_of_day`, mod ABL), props follow the sun instead of keeping the
first-frame value. Everything else must be pixel-identical (<= 1 LSB): this makes the proof falsifiable, unlike the
W96 "identical except where the sun source changes" allowance, which this design withdraws.

Consumers audited (lane9 reader audit, accepted): only `color_prop_tier1.vert:259-278` reads rows for props;
`shadow_prop.vert` and `legacy_shape_tier1.vert` do not; mechs keep rows. Lane9's CPU replay/diagnostic readers stay.

Steps (one Sonnet lane, one big run, D1a brief): (1) contract header + doctest + C2 runtime counter; (2) fix lane9's
tint to the contract (I8 + policy bits, UBO carries S, A, F, Dspec); (3) remove the VS row read (lane9 has this);
(4) proofs. What breaks: building brightness (missing ambient), trees gaining sun, specular direction; detection = C2
mismatches, pixel identity on day+night. Rollback: revert the commit (one shader + recipe field); no data, no save.
Gain: about 0 ms directly; it unlocks D2 (no light-slot reason for per-frame `touch()`) and D1b (instance record no
longer refers to a per-frame dedup slot).

#### D1b. GPU-driven prop draws

Design:
- Feature admission: add Wanted items `multi_draw_indirect` (VkPhysicalDeviceFeatures), `draw_indirect_count`
  (Vulkan12Features), `shader_draw_parameters` (Vulkan11Features) to `vk_device_profile.h` kItemIds + `evaluate()` with
  doctest rows in the existing profile test. Admitted -> MDI route; refused -> today's CPU loop. This is a capability
  route logged in `[VK_PROFILE v1]`, not an env gate (no new default-OFF gate).
- Data: packet table SSBO (one row per packet x pass: indexCount, firstIndex, baseVertex, layerOff, layerOn, uvScale,
  alphaTest, materialId, typeId), replaces the per-draw push constants; the VS reads it by `gl_DrawID`.
- Compute (extends `shaders/hzb_cull.comp`): frustum (always, also below 1024 instances) + HZB per instance -> compacted
  instance index list per packet group + counts -> `VkDrawIndexedIndirectCommand[]` + count buffer, using subgroup
  ballot for compaction (subgroup ops are core 1.1).
- Draws: three `vkCmdDrawIndexedIndirectCount` calls in today's order: core pass (all packets), fringe pass (alpha
  packets), late list (alphaTest==2). Order inside each call = packet order = today's CPU loop order.
- HZB resources: per-frame-slot cbuf AND per-slot `instances`, `visibility`, `projected` buffers with a ticket per slot
  (fixes Astra row 6 properly); bounds stay generation-cached.
- Threading/lifetime: all buffers are per-slot ring entries retired by the frame ticket; nothing host-written while a
  slot is in flight.
Steps: (1) admission + packet table + MDI for the colour pass (feed still CPU-built; saves at most the 0.216 record and
the push constants); (2) after D2: resident instance store with dirty-range upload, no per-frame feed copy (removes most
of BatcherFlush 0.556 and the 112 B double copy, `vulkan_post_hzb_native.cpp:748`, colour island copy); (3) shadow props.
What breaks: fringe/late order, packet-param mismatch (black fence class from memory), over-cull. Detection: C3
false-culls = 0, C1 draws per island (MDI reports draw count from the count buffer readback, lagged), packet dump CSV
(`MC2_VK_PROP_PACKET_DUMP`) equality CPU vs GPU path, day+night montage. Rollback: device-profile refusal path is the
old loop, kept for one window. Gain: step 1 -0.05..-0.2, step 2 -0.15..-0.35 (overlaps D2), GPU VS time drops at 4K.

### D2. Static walk removal / resident inert ranges (Fable P2-P7, corrected)

Core: `GameObjectManager` keeps `activeTerrain` (handles) and per-appearance `presentState`; only ACTIVE objects are
walked by presentation (`objmgr_misc.cpp:569-618`), extracted (`objmgr_update.cpp` ExtractRenderList terrain part), and
rendered (`objmgr.cpp:1075-1092`). INERT = Fable's classification (static eligible, registered, LOD settled, no
spotlight/searchlight child, not falling/selected/targeted/destroyed-in-transition) MINUS the light-env term (D1a makes
it unnecessary for props). Activation events: Fable's setter list (bldng/terrobj/turret/gate setters, invalidate,
tombstone, registry reset, mission load / save-load = all ACTIVE for one settle frame) plus: tree LOD band crossing
evaluated against the camera only when the camera moved more than a band epsilon (moving fixture covers it), terrain
height generation bump (grounding), nightFactor crossing 0 (lamps).
Residency: INERT ranges skip `flushRangeCurrentness` (`gos_static_prop_registry_flush_range.cpp:41`); the persistent
store is the feed; GPU frustum+HZB (D1b compute) provides same-frame visibility; CPU consumers of static visibility:
picking (lazy OBB at click time, memory `building_pick_world_obb`), turret/gate/artillery sim gates read `windowsVisible`
of ACTIVE classes only (unchanged), determinism unaffected because presentation is not sim (SIMID proves).
Interfaces: `markPresentActive(handle, reason)` (idempotent), `settle()` after one clean ACTIVE frame; packet carries
registry generation + height generation so S3/S4 can assert the store it draws matches the extract.
Steps (Sonnet-sized big runs): (S1) residency for colour props + same-frame GPU visibility (D1b compute frustum for all
counts); (S2) ACTIVE-only presentation walk + grounding by height generation (prove by C5 calls x ns); (S3) ACTIVE-only
extract + render walk (shrinks RenderList, `freezeDrawFrame` copy and lane8's moved extract cost at once); (S4) pick +
consumer audit, delete framesSinceActive sweep. Coordination: S3 touches render_view/objmgr extract, lane8's area:
D2-S3 lands on nifty first and lane8 rebases on it (lane8 must NOT keep copying inert terrain into packets).
What breaks: black/stale resident (wrong LOD, recipe or flags forever), missing activation (destroyed building drawn
intact), pick misses, tree LOD pops on a moving camera. Detection: an EAGER SHADOW ORACLE (not a self-compare): on
`bench_freeze` and on 1/16 sampled objects per frame in normal runs under `MC2_OBJECT_PARITY_TRACE`, run the old eager
presentation for the object into a scratch snapshot and compare visible-set membership (CPU bounds vs GPU keep mask),
model matrix, recipe index, LOD, flags, pointLight, lightsOut, highlight; mismatches = 0 asserted; plus C1 counters,
`persistent_vanish` = 0, moving-camera montage. Hard gates: SIMID 03/10/24, saves untouched (no serialized state; all
ACTIVE on load), mods first missions (CVE-G, Exodus, Carver5, MagicExpansion, Omnitech, REAP3). Rollback: the
classifier returns ACTIVE for everything (one-line change) = today's walk. Gain: of about 1.69 ms, keep the ACTIVE share
(5-40 %, unmeasured; C1 gives it) plus the loop skeleton: -0.9..-1.5 ms on mc2_24, -0.2..-0.35 on mc2_10, small on
mc2_01 (I-est).

### D3. Present / frame-top wait restructuring, then record thread S4

Correct synchronisation model:
- Ownership: Main owns Render outside [enqueue, seal]; the worker owns Render inside it and Present after `seal()`
  (V `present_thread.cpp:113-130`, `vulkan_context.cpp:588-592`). Any `getOrCreateEngineVulkanContext()` or
  `engineVulkanContextValid()` on main waits for seal (V `:857-865`).
- The submission mutex is held across `vkQueuePresentKHR` (V `vulkan_context.cpp:588-592`): once main overlaps the
  worker, a main submit issued during QueuePresent blocks on WSI; with FIFO vsync that can be milliseconds. So the
  overlap window must end before main's first submit, and `seal()` stays before the WSI call.
- Worker waits: Acquire (WSI) and CompositeWait (GPU) are charged by B2 attribution, not as CPU.
D3a (counter-only, wave 1): sub-zones inside TailRecord (HUD colour proof, ImGui, blit, Collect, Submit, CompositeWait),
`Frame.WaitSealed` zone, first-blocking-wait site label and its time into the frame (C1).
D3b (behaviour, window): cut the tail: skip ImGui record when no window is open and nothing is drawn (I, needs D3a),
record the HUD colour proof only when HUD content changed, move Collect decimation to every 4th frame (profiler builds).
D3c-lite "VK-free prefix": make `engineVulkanContextValid()` a lock-free read of a published atomic (no wait), move
`retirePresentHandles` + texture retirements to the first point that needs the context, assert (audit) that GameLogic
and ExtractRenderSnapshot do not touch the VK context, and place the single `waitSealed` immediately before DrawScreen.
Overlap ceiling = min(worker tail, main prefix): today prefix about 1.4 ms (GameLogic + extract) vs tail 0.55-0.83,
so up to the full tail hides; after D2 the prefix shrinks to about 0.3-0.6 and the gain falls with it (non-additive).
D3c full S4 (lane8): only after (i) lane8 is rebased on a nifty that has D2-S3, (ii) C6 stress green, (iii) rebased
S3 overhead <= 0.10 ms MAIN_BUSY on mc2_24 (W96 condition kept). Then the existing present worker records the sealed
packet; main observes CPU completion before arena/resource retirement (lane8 plan).
What breaks: ownership races (texture retirement, handle clock `RenderWorld::advanceFrame`), WSI blocking main,
validation errors. Detection: C6, overlap audit VIOLATION = 0, Khronos sync validation run (lane8 battery), SIMID.
Rollback: D3c-lite = restore the frame-top wait (one call); S4 = inline consumer (lane8 keeps it). Gain: D3b -0.1..-0.3,
D3c-lite -0.1..-0.4 (after D2: -0.1..-0.25), S4 -0.5..-0.9 of DrawScreen (I-est).

### D4. Remaining costs: measured backlog (order by C1/B2 numbers, re-sorted at each decision point)

| Item | Today (V where measured) | Candidate | Gain (I-est) |
|---|---|---|---|
| Sim tick in-frame join | 2.76 ms/tick p50, p95 4.2 on 70 movers; joins after DrawScreen in the same frame | pipeline: kick at frame N, join at the frame that needs tick N's results (the next kick), render reads only extracted snapshots; requires S3-style snapshot completeness | tick-frame p95 -1..-2.5 ms; the p95 gate is impossible without it |
| freezeDrawFrame copies | inside Extract 0.248; O(visible terrain objects) | shrinks with D2-S3; then swap-in arenas instead of make_shared copy | -0.05..-0.15 |
| Presentation.Movers/Other + Interface | 0.075 + 0.077 + 0.142 | D5 rate budgeting; interface: cache GUI layout when unchanged | -0.05..-0.15 |
| Shadows (DynamicShadowPass 0.101 + render shadows 0.106) | measured | skip when no caster moved (Fable round 2) | -0.05..-0.15 |
| Terrain CPU (TerrainFlush 0.109 + geometry 0.073) | measured | camera-dirty only | -0.05..-0.12 |
| VFX + vegetation + water record | 0.079 + 0.049 + 0.068 | record cost after S4 | moves with S4 |
| Audio | not measured | add zone | unknown |
| Allocations | not measured | per-frame heap counter (C1) | unknown |
| Dynamic prop leaves 929/frame | bench `dynamic_prop_leaves` (V) | classify why they miss residency (C1 reasons), fold into D2 | -0.1..-0.3 (I) |

### D5. Update-rate budgeting for mechs/effects (UE parity row 6), candidate

Presentation-only rates: off-view movers run `update(false)`, footprints and `setObjectParameters` every 4th frame,
keep `recalcBounds` + `windowsVisible` (sim gates read it). Worker: lazy snapshot SimPose (Fable rest-of-frame 1),
exact under SIMID only if inputs are snapshotted where eager evaluation happens today. Stock gain small (-0.05..-0.1
main), battle larger; schedule after D2.

### D6. Parallel-safety map (file ownership per lane)

| Lane | Owns | Must not touch |
|---|---|---|
| perf-counters (C1, C4, C5 commands) | battle_bench*, gameosmain.cpp zones, smoke_lib parse/gates, tools/perf, `draws` increments | shaders, registry logic |
| light-contract (D1a, lane9) | color_prop_tier1.vert, frame UBO ABI, prop batcher tint, bdactor_static/tree recipe tint, prop_light_contract.h, C2 | objmgr walk, present |
| cull-parity (C3) | hzb_prop_bounds.h tests, vulkan_post_hzb_native.cpp check | prop island draw loop |
| present-sync (C6, D3a, then D3b) | present_thread.*, engine_present_vk.cpp zones, test_present_handoff.cpp | GameLogic, islands |
| lane8 rebase (coordinator) | record-thread files | D2 files until D2-S3 lands |
Known small conflict: C1 adds one counter line in `vulkan_color_prop_island.cpp`; light-contract touches one line
there. Merge order: counters first.

---

## E. Two-week schedule (from 2026-11-02)

| Days | Wave | Lanes (parallel) | Lands how |
|---|---|---|---|
| 1-2 | W1 | perf-counters (C1, C4, C5); light-contract (D1a, lane9 continuation); cull-parity (C3); present-sync step 1 (C6 + D3a); lane8 merge-main (coordinator, running) | C-items: nifty immediately. D1a: window W97. |
| 3 | DP1 | Census v2 (Sonnet, 1 run): C1 counters on HEAD, mc2_24/10/01 fixed + camera_path + battle200 day/night, non-Tracy MAIN_BUSY table B2, INERT active share, walk C5 table, tail sub-zones, rebased S3 overhead | META REVIEW gates wave 2 (go/no-go per slice, re-rank D0) |
| 3-7 | W2 | D2-S1+S2 (residency + ACTIVE-only presentation; needs D1a merged); D3b tail cuts + D3c-lite; D1b step 1 (admission + MDI colour pass); lane8 S3 closure only if DP1 overhead <= 0.10 ms, else rescope onto D2 | windows W98 (D3b/D3c-lite, D1b-1), W99 (D2) |
| 7 | DP2 | paired A/B for W98/W99 tips (5 pairs, 3 fixtures), montages viewed | META REVIEW |
| 8-12 | W3 | D2-S3 (ACTIVE-only extract/render walk) then lane8 rebases on it; D1b step 2 (resident store, dirty ranges); sim-tick pipelining design + counters (D4 row 1); D4 items by measured order | W100, W101 |
| 12 | DP3 | full census; decide S4 dispatch ship; decide D5 | META REVIEW |
| 13-14 | consolidate | S4 dispatch (if DP3 green) or D4 items; publish | W102 |

STOP now: lane9 pixel-identity chase against a frozen-light reference and its "intentional sun change" waiver; P1
classification as a standalone landing (no CPU payoff, adds walk work); merged submit; VFX-once for stock; grounding
skip as a standalone (fold into D2-S2 with calls x ns); any number taken from the double-counted GameLogic zone; lane8
growth of extract-side copies of inert terrain objects; frame-time CI as proof for < 0.1 ms items; gate-tool features;
remaining C++23 work except save-format omissions (owner: deprioritised).

Decision points need a meta review (Opus or Astra) because each changes the order: DP1 (is the active share small
enough for D2's forecast; does the tail cut beat overlap; is S3 overhead acceptable), DP2 (did D2 regress night/moving
camera; was D3c-lite's gain eaten by D2), DP3 (S4 ship).

Hard gates for every engine-behaviour tip: SIMID 03/10/24 unchanged chains, saves untouched (no section version
change in any perf tip), the six mods' first missions within known error budgets, Vulkan validation 0, day+night
montages VIEWED (mc2_01_saved_205644, mc2_10_saved_204523) plus the camera_path montage for D2.

---

## F. First-wave briefs (Sonnet-ready, <= 25 lines each)

In `docs/plans/2026-11-02-perf-briefs/`:
1. `w1-perf-counters.md` - C1 + C4 + C5 (counter-only; lands on nifty without a window).
2. `w1-light-contract.md` - D1a on lane9's static-p1r (behaviour; window W97).
3. `w1-cull-parity.md` - C3 (test + counter-only check; lands immediately).
4. `w1-present-sync.md` - C6 + D3a (tests + zones; lands immediately), D3b design note only.
Census v2 (DP1) is briefed after W1 lands, because it consumes C1's counters.
