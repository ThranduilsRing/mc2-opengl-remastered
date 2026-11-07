# code/ legacy-idiom sweep (branch claude/cxx23-code-sweep, base 2d042f852)

Result: ratchet --check --worktree: 117 fewer legacy sites than baseline (code/, excluding pilotreviewarea* and mechlopedia*).
44 files, 3 commits. Full ninja build exit 0 at the final code state.

## What changed
- LG09: functional casts `T(x)` -> `static_cast<T>(x)` (arithmetic only: int, long, float, double, int64_t, unsigned). Same semantics as the C-style cast; compile-checked. One `(long)getState()` C cast too.
- LG10: one-sided if-clamps -> `x = std::min/max(x, b)` with the operands ordered so ties and NaN behave as the original `if (x > b) x = b`. Two-sided pairs where the bound may cross are two statements, not std::clamp (only mechlabscreen sw/sh/cw/ch and gameobj altLevel, whose constant bounds are ordered, use std::clamp).
  Mixed-type sites use `std::max<long>(...)` (value-identical to the original promotion) or float literals matching the variable's type; sites where a literal's type would change the comparison were left alone.
- LG08: objmgr_update.cpp commit-order loop -> structured binding.

## Skipped
- LG18 (all 11): pointer reinterpret_casts and unions (carnage.h, warrior.h), no value copy, so not std::bit_cast candidates. ablmc2_repro.cpp is a sentinel pointer.
- LG09 false positives: missiongui.h / missiongui_orders.cpp member-function-pointer types.
- LG10 skipped: argmin/argmax loops with extra assignments (battle_bench_fixture, brain_special_dispatch, brain_missionfit_oporbd, forcegroupbar, mission_update*, missionbegin, warrior_combat, warrior_target, fixtures, contact.cpp:1105); throttle else-if clamps (gvehicl_move, mech_move: bounds may cross, std::clamp would be UB); turret/torso yaw clamps (also zero a rate); `>=` bound forms (collsn, mechlabscreen 712/718, mover_path) which assign bound-1; sim_tick_clock.h, missiongui_support, missiongui_update (extra assignments); optionsarea SetScroll (long vs float compare).
- LG08 remaining: objmgr_update.cpp sort lambda (pair params), ui_profile_registry.cpp iterator `hit->second`.
- Not touched: code/pilotreviewarea*, code/mechlopedia*.
