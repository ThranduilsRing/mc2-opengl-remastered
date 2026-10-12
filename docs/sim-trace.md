<!-- doc-status: current | reviewed:2026-10-11 -->
# The `[DT]` determinism trace (`MC2_SIM_TRACE`)

WHAT: a compile-time facility (`cmake -DMC2_SIM_TRACE=ON`, default OFF, no runtime gate; `mclib/sim_trace.h`) that prints one
`[DT] f=<render frame> t=<scenario time> <TAG> ...` line per event at the places a determinism hunt reads first:

| tag | where | what it carries |
|---|---|---|
| `RNGF` | every sim frame (`code/sim_identity.cpp`, with the `[SIMID v1]` line) | draws since the mission seed on the simulation stream (`sim=`, the always-on counter behind `SimRandomDraws()`), the appearance stream (`app=`) and the weather stream (`wx=`); the same three counts ride on every `[SIMID v1]` line as `rng=` |
| `RNGCALL` | every `RandomNumber` / `RollDice` (`mclib/routines.cpp`; `SignedRandomNumber` goes through `RandomNumber`) | the caller's return addresses (`TRACEBASE` once, to rebase against the PDB) |
| `HIT`, `INJURE`, `DESTROYLOC`, `CTBRANCH` | `code/mech_damage.cpp` | weapon hit, body damage, location destruction, the centre-torso blow branch and its inputs |
| `BOLT` | `code/weaponbolt_update.cpp` | a bolt's distance to a mech target per frame |
| `REQ`, `CALC`, `CALCDONE` | `code/warrior.cpp`, `code/movemgr.cpp` | path request, calculation start, resulting path cursors |
| `SETTGT` | `code/warrior_target.cpp` | a pilot's target change |
| `SADD`, `SREM` | `code/contact.cpp` | sensor contact added / removed |
| `WX` | `code/weather.cpp` | the weather update's inputs (camera position, elevation, frame length) |
| `ABL` | `mclib/ablxstd.cpp` (only with `MC2_SIM_TRACE_ABL=ON` as well: thousands of lines per frame) | every standard-routine call: module, line, routine |
| `ABLRAND` | `mclib/ablxstd_std.cpp` | every script `random()` call: brain module, script line, range |
| `SETPOS`, `SCROLL` | `mclib/camera_input.cpp` | camera moves with the caller's stack |

WHY: when two runs of one mission diverge (`tools/sim_determinism.py` names the first differing sim frame), the `[DT]` streams of the two
logs explain it: the first frame whose `RNGF` counts differ names the consumer that drew out of order, its `RNGCALL` stack names the call
site, and the item lines show which state moved first. The mc2_10 causes (an ABL builtin reading the wall clock; weather drawing from the
shared stream a camera-dependent number of times) and the mc2_24 cause (an uncleared component table) were found this way.

How a hunt uses it:
1. `cmake -S . -B build64-ninja -DMC2_SIM_TRACE=ON` (the dev tree; turn it OFF again afterwards), build, deploy.
2. `py -3 tools/sim_determinism.py pairs-until-fail --mission <m> --max 30 --duration 30 --out <dir>`; a failing pair keeps both logs.
3. `grep -a "^\[DT\]" a/<m>.log > a.dt; grep -a "^\[DT\]" b/<m>.log > b.dt; diff a.dt b.dt | head`: read the first differing `RNGF`
   line; if the counts differ, the draws between the previous frame's `RNGCALL` stacks name the consumer; if they agree, the first differing
   item line (`HIT`, `SETTGT`, `CALC`, ...) is the state that moved, and the code that writes it is where to look.
4. Rebase a stack address: `addr - TRACEBASE.line + <address of mc2_sim_trace::line in the PDB>`; or `llvm-symbolizer --obj=mc2.exe`.

Proof that it compiles: a second Ninja tree, `MC2_NINJA_TREE=build-simtrace py -3 scripts/ninja_build.py -n mc2` then
`cmake -DMC2_SIM_TRACE=ON build-simtrace` and the build; the lane that adds a trace point builds once with it ON. A default build has
no `[DT]` code at all (the macros expand to nothing), and the LG19/LG20 gate ratchets see no new runtime gate.
