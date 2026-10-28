# G7 overlap audit (2026-10-28)

Design: `2026-10-27-g7-sim-render-overlap-design.md`. Mechanism: at the tail of frame N (after snapshot extract, `draw_screen`,
frame counters and pass stats, before `frameSwapWindow`) `Mission::overlapKick` polls the tick clock and runs the sim part of the
banked ticks (`updatePass(true,false,true,true)`) on the persistent worker in `GameOS/gameos/sim_overlap.cpp`. The main loop joins
right after `frameFrameCap`. Frame N+1's `Mission::update` then runs only the frame part (`updatePass(true,true,false,false)`) and
`updatePresentationOnly`. The worker is therefore running only while the main thread presents, waits on vsync or the frame cap, and
never while anything reads live sim objects. Kill-switch: `MC2_SIM_OVERLAP=0` (default ON) runs the old serial loop.

## What overlaps what

The only main-thread work concurrent with a tick is `frameSwapWindow` (present) and `frameFrameCap` (sleep/wait). Neither reads
mission, mover, terrain or appearance state. Everything that does read it (snapshot extract, GUI render, `draw_screen`, the frame
part, presentation update) is between join and kick, so it is serialised with the tick by construction. The debug assertions below
verify that rather than assume it.

## Shared structures crossing the boundary

| Structure | Who touches it | Protection |
|---|---|---|
| Mission, movers, objects, terrain, weather, turn | worker during the tick; main between join and kick | join barrier: main never runs between kick and join except present/cap. `assertSimQuiescent` at snapshot extract and `updatePresentationOnly` |
| `s_pre` (OverlapPre) and `s_turnAfterLastPass`, `s_tickClock` | kick (main) writes before starting worker; worker writes results; next `update` reads after `waitWorker` | worker start and join are mutex/cv happens-before edges |
| `frameLength` | worker sets it to the render length captured at kick | set at the top of the worker job; main does not read it while in flight |
| `g_mc2FrameCounter` | `advanceFrameCounter` (main, before kick); tick>0 increments on the worker | ordered by kick; same increments as the serial loop |
| `s_extraTickInFrame` | worker only | set and cleared inside the job |
| Battle bench sink (`ticks`, `tickMs`) | worker adds tick time; main prints at frame end | now `std::atomic` |
| Mover appearance / `alphaValue` | presentation (main) | presentation no longer writes sim fields: `forceVisibleOverride` takes a local copy (commit d18071984); presentation runs only after join |
| GL/Vulkan/texture/sound entry points | must be main thread | `assertMainThread` in `gos_NewEmptyTexture`, `gos_NewTextureFromMemory`, `gos_NewTextureFromFile`, `gos_DestroyTexture` |
| Legacy job pool | tick code that dispatches jobs | the pool is owned by the worker while a tick runs; main does not dispatch during that window |

## Ineligible frames (serial path at the top of the next frame)

First frame, multiplayer, paused or frozen sim, pending in-mission save or load, and a turn mismatch (new mission or loaded save).
UI orders issued during frame N+1's frame part take effect one tick later than in the serial loop.

## Debug assertion mode

`[SIM_OVERLAP v1] VIOLATION kind site=... count=N` is printed once per site; `MC2_SIM_OVERLAP_AUDIT=1` aborts on the first one.
Two kinds: render or presentation code reached while a tick is in flight (`assertSimQuiescent`), and a thread-affine GPU resource
call made from the worker (`assertMainThread`). Presentation writes to sim state are caught the first way, because presentation
may only run when the worker is idle, and by the alphaValue fix, which removed the one known write.
