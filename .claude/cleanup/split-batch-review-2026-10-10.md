# Split batch — Opus adversarial review (2026-10-10; ablmc2, mover, mech, move, missiongui, warrior; ACCEPT-WITH-FIX)

Condensed by the orchestrator. Reviewed at parent vs split commits, line-multiset plus macro/include/extern diffs; `check-order-gate-coverage.py` and `check-mover-registry-coherence.sh` run read-only (pass).

## Finding (fix landed by lane 2)
- **mech gate timing.** `d0fe71c74` replaced the eager file-scope `static const bool s_gpuCullLifecycle = (getenv("MC2_GPU_CULL_LIFECYCLE") != nullptr);` (old `mech.cpp:142`) with a lazy local-static `inline bool mech_gpuCullLifecycle()` in `mech_internal.h:235`, first read at `mech_update.cpp:726/748/783`. Values applied inside `main` (`launcher_env.json` via `_putenv_s`, dev-shell `set_gate`) are now seen by mechs but not by `objmgr.cpp:145` / `gvehicl.cpp:124` (still pre-main) → divergent gate state; `readback_isActorVisibleLagged` fails open. Fix: restore the file-scope static in `mech_update.cpp`, delete the helper (a typed shared gate later).
- Log-text change: `ablmc2_internal.h:75` `[ABL_ARG_GUARD]` em dash → `-` (nothing parses it; restored).
- Nit: `StepAdjDir`, `adjTile`, `typeString` in `move_internal.h` → `inline constexpr`.

## Confirmed safe
- All other moved globals/statics are constant-initialized (`logisticsTable`, `DistanceToWaypoint`, `InitWayPath`, `maxNumDoors`, ablmc2 roster/camera statics, `s_freezeMovers*`, `cellShift`); startup lambda/getenv statics stayed in their original files; no registrars in these files.
- Only header `static` is the stateless `static inline abl_arg_guard_log`; no duplicated definitions; 160 new `extern`s all resolve to definitions in files that include the same header.
- Macros: no `#undef` in originals; no macro with two values; conditionals defined before use within each piece; hoisted macros never used before their original definition; `#if` line counts identical.
- Include guards/order: each piece includes only its `*_internal.h`, whose include sequence equals the original's (quirks inherited); missiongui's mid-file includes moved earlier — no name collisions with `CTRL/SHIFT/ALT/WAYPT/TACMAP_ID`.
- Purges re-derived with unifdef: match; none of the 11 macros defined anywhere (CMake, headers, editor adds only `MC2_IMGUI`, `MC2_IS_EDITOR`); `#if 1` kept the `#if 1` branch; `USE_SEPARATE_WATER_MAPS` (FALSE) kept `#else`.
- ABL: 281 ordered `ABLi_addFunction` calls in one list in `ablmc2.cpp`; md5 unchanged; the 3 removed were inside `#if 0`.
- missiongui chain: 30 branches, same order, split 5/8/8/5/4, groups end `else return false; return true;`, chained with `||`; epilogue sees the same `UiCmdCtx`.
- warrior: all gate statics stayed in `warrior.cpp`; order-gate check passes.
- Contract scripts: none now check nothing; only stale line numbers in comments (`cursor_guard.py`, `gate-defaults-allowlist.txt`, `run_smoke.py`, `checklib.py:103`).

## Checks added to `tools/split` (before bdactor, mech3d, gos_postprocess, gameos_graphics)
1. Linkage/init-form diff: `static`→`extern`/`inline` or eager→lazy fails unless allowlisted; gate caches stay file-scope in the using piece.
2. Moved lines byte-identical (string literals, encoding).
3. Hoisted mid-file includes listed with prior macros; headers grepped for those names.
4. `#if/#ifdef` use follows its `#define` in the same piece or the macro moves to the header.
5. No non-inline `static` variables in `*_internal.h`; mutable `inline` → `inline constexpr`.
6. Flag `inline` functions defined in one piece and called from another.
7. Record static-registrar order before/after (feature registry, `ABLi_addFunction`).
8. Grep scripts/ tools/ for the source path; widen "forbid in file X" to `stem*.cpp`; run `--self-test`.
