# mc2_sim: the sim static library and its remaining link edges

Slice 1 of the sim kernel (design: .claude/cleanup/sim-kernel-design.md). `mc2_sim` (CMakeLists.txt, after `add_executable(mc2 ...)`) is a static library of
the sim translation units from `scripts/sim_include_scope.json`: code/ablmc2_orders, mission_update, sim_identity, sim_rng, tactical_order_queue,
tacordr, warrior, warrior_orders. `mclib/cmponent.cpp` stays in the mclib library. `mc2` (so the `--headless` runner) links `mc2_sim`; compile
definitions, include directories and PCH are those of mc2. It names only `mc2_gate` (header usage). No behaviour change.

## What "links without GameOS/render libs" means today

A static archive links nothing, so the measurable boundary is its undefined symbols. Measured on the box (Linux clang, `nm -A --undefined-only
libmc2_sim.a`, symbols defined inside the archive removed, each remainder assigned to the library or object set that defines it; raw output in
E:/mc2-archive-2026-10/proofs/sim-kernel-lib-s1/edges.txt). 372 external symbols:

| Owner of the symbol | Count | Meaning |
|---|---|---|
| mc2 exe objects (the rest of code/) | 151 | game globals and managers (`ObjectManager`, `MPlayer`, `PathManager`, `CurObject`), `GameObject::*`, mission/ABL glue; not GameOS or render, but not yet in a library |
| system / libstdc++ / libc | 94 | expected |
| libmclib | 90 | not listed; mclib is itself mixed sim and render |
| libgameos | 33 | the GameOS/render edges below |
| libmc2_gate, libwindows | 2 + 2 | gate registry `g_slot/resolve`; `CharUpperA` (warrior), `S_snprintf` (mission_update) |

A library-only link of `mc2_sim` without gameos/renderworld is therefore not possible yet: 31 of the 33 gameos symbols come from ONE TU,
mission_update.cpp (per-frame presentation hooks living in the sim update).

## GameOS / render edges (symbol <- call-site TU)

Render / UI presentation (all mission_update; cheapest to cut by moving the calls to the caller or a hook table):
- `gos_SetWorldToClipGL`, `gos_TextStringLength`, `gos_TextSetAttributes` (debug text overlay)
- `gos_GetTerrainShadowSoftness`, `gos_SetTerrainShadowSoftness`, `gos_TerrainLodChunk_VisualDampWanted`, `gos_TerrainLodChunk_UpdateVisualDampMovers`
- `gpu_cull::substrate_frameBegin`
- `gos_GetKeyStatus` (input read in the update path)

Platform runtime, not render (belongs in a platform library the sim may link):
- `mc2::overlap::{waitWorker, runOnWorker, installHooks, simulationFrame, assertSimQuiescent, setSimulationFrame}` (mission_update)
- `mc2::log::vevent`, `mc2_diag::{tagEnabled, writeEvent}`, `crashbundle_append`, `tracy::*` (profiling and diagnostics)
- `SmokeMode::{fixedTimestepEnabled, fixedTimestepOnSimFrame, onSimTick}` (mission_update): the tick driver; belongs with the headless runner
- `InternalFunctionStop` (ablmc2_orders, tacordr: ABL error report), `mc2GetSimRngState` (sim_identity, defined in gameos for the RNG trace), `operator new/delete` (mimalloc in gameos)

## Edges broken in this slice

None (the brief allows at most 3). The cheapest are the `gos_Get/SetTerrainShadowSoftness` pair, `gpu_cull::substrate_frameBegin` and the
`gos_Text*` overlay: each is one call site in mission_update.cpp, but each needs a presentation-hook interface owned by the render side, which is
slice 2. Leaving the code untouched makes the determinism proof a pure link-structure proof.

## Next slice, in order

1. Move the presentation calls of mission_update.cpp behind one hook table installed by the frame loop (null in headless).
2. Move `mc2::overlap`, the `SmokeMode` tick callbacks, logging/Tracy and `operator new` into a platform library `mc2_sim` may link.
3. Move the TUs behind the 151 exe-object symbols (managers, globals) into the library; then mc2_sim + mclib + the platform library links the headless runner with no gameos/render library.
