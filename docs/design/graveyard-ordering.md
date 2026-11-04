# Graveyard ordering (sim overlap, R1)

Code: `code/tick_commit.cpp` (`deferRetirement`, `retainRenderPacket`, `retireRenderPacket`), `GameOS/gameos/gameosmain.cpp` (frame loop).

## Rule

`retireRenderPacket` drains the graveyard. Its commands destroy appearances and effects (`AppearanceTypeList` refcounts are not atomic), and the sim worker reads them (it never pushes: a worker push would trip `assertMainThread("defer render packet release")`). So the drain must run after `mc2::overlap::waitWorker()`. `retireRenderPacket` starts with `assertSimQuiescent("graveyard drain")`; with `MC2_SIM_OVERLAP_AUDIT=2` a drain while the tick job is in flight reports `render-read-while-tick-in-flight`.

## Which retirements can reach the graveyard

`deferRetirement(Queue::Graveyard, cmd)` only stores into `graveyard[]` when a render packet is retained at that moment. Otherwise it returns false and the caller frees at once.

| Caller context | Reaches graveyard? |
|---|---|
| Sim worker, `collecting()` true (`releaseAppearance`, `retireObject`, `releaseEffect`) | No directly. It queues in `queues[Graveyard]`. |
| `commitTickQueues` re-issues those commands | Only if a packet is retained then. It runs at the start of the next frame, before `retainRenderPacket`, and the inline consumer has already retired every packet, so no. They free immediately. |
| Main thread between `retainRenderPacket` and `retireRenderPacket` (extract, `draw_screen`) | Yes, in principle. The VTOL `retireObject` sites are tick-side (`updateVTol`'s presentation branch, `missiongui_style.cpp:24-91`, returns before any retire), so they are not draw-time and do not land here. No known stock-play caller reaches the graveyard on this route. |
| Worker with `collecting()` false | The only such gaps are the `updatePass` prologue and epilogue. A push there would trip the Main assert, so it is a bug, not a route. |

The graveyard is empty in stock play, not proven empty in all play (the campaign CTD is not closed by this work). Measured on mc2_01 (30 s): 4080 `retireRenderPacket` calls, every one with `left=0` and zero non-empty drains, even with 1100+ airstrikes spawned (pool of 130, strikes never retire their appearance mid-mission). A test knob that spawns work on the worker therefore cannot make the graveyard non-empty, so the ordering proof is the assert plus the call count, not drained-object count.

## Proof row

`scripts/video_decode_smoke.py`, row "mc2_01 graveyard ordering": `MC2_SIM_OVERLAP_AUDIT=2`, `MC2_DIAG_TAGS=GRAVEYARD`. The game logs `[GRAVEYARD v1] retire_calls=N nonempty=M` every 100 calls. PASS needs `retire_calls >= 200` and no VIOLATION. A build that drains before `waitWorker` aborts with the VIOLATION on the first frame the worker is in flight. Fixtures: `scripts/tests/fixtures/video_smoke_graveyard_*.txt`.

If a future consumer retains packets across the tick commit (async render), the graveyard becomes non-empty on the worker path and the same row then covers real work; `nonempty` in the counter shows it.
