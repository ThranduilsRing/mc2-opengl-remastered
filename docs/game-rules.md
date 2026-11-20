# Game rules

Read through `mc2::rules::current()` (`code/game_rules.h`); networked games use the defaults. Helpers: `code/economy_rules.h`,
`code/called_shot_floor.h`.

## Harness

Every mission start or load prints one line with all rules that change the sim:
`[GAME_RULES] snapshot networked= calledShots= pilotSkillWeight= salvage= cbillMultiplier=`.
`scripts/run_smoke.py` (and `tools/sim_determinism.py`, which runs it) fail a run with `[RULES] HARD FAIL` when that line shows a
non-default value, because every baseline pins the default rules. Opt in with `--allow-nondefault-rules` on either tool.
The per-objective payout line `[GAME_RULES] payout base= pct= credited=` prints only with `MC2_DIAG_TAGS=GAME_RULES`.

## Economy

- **C-Bill multiplier (50-200 %, default 100).** Scales only the mission-objective payout at the end of a mission
  (`LogisticsData::setMissionCompleted`, per successful objective), truncating toward zero. It does not scale the per-mission
  `additionalCBills` stipend, salvage refunds or cheats (they do not pass through the scaled path). The objectives HUD still shows
  the base reward (follow-up). Salvage Generous plus a multiplier can make selling salvage a net gain.
- **Salvage (Generous / MC2 / Scarce, default MC2).** Sets the fraction of a mech's cost charged to salvage it
  (`salvagemecharea.cpp`): MC2 = 0.8 (stock, exact), Generous = 0.6, Scarce = 1.0. Which mechs are offered is unchanged.
  `MechSalvageChance` is dead code and is not wired.

## Combat

- **Called shots (Off / Floors / Classic, default Floors).** Off removes aimed shots for both sides: the aim is dropped at fire time
  (`mover_attack.cpp`, `mech_weapons.cpp`, `gvehicl_weapons.cpp`), the aim-leg/arm/head commands show the refused cursor and issue
  no order (`missiongui_orders.cpp`), and no called-shot radio line plays (`mover_combat.cpp`). `MC2_CALLED_SHOT_FIX=0` forces Classic.
- **Pilot skill weight (0-200 %, default 100).** Scales the gunnery skill used for to-hit (`mover_attack.cpp`, `turret.cpp`).

Wiring is pinned by `scripts/tests/test_rules_wiring.py`.
