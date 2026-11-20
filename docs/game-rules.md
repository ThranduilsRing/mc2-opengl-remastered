# Game rules: economy

Read through `mc2::rules::current()` (`code/game_rules.h`); networked games use the defaults. Helpers: `code/economy_rules.h`.

- **C-Bill multiplier (50-200 %, default 100).** Scales only the mission-objective payout at the end of a mission
  (`LogisticsData::endMission`, per successful objective), truncating toward zero. It does not scale the per-mission
  `additionalCBills` stipend, salvage refunds or cheats (they do not pass through the scaled path). Each payout logs
  `[GAME_RULES] payout base= pct= credited=`.
- **Salvage (Generous / MC2 / Scarce, default MC2).** Sets the fraction of a mech's cost charged to salvage it
  (`salvagemecharea.cpp`): MC2 = 0.8 (stock, exact), Generous = 0.6, Scarce = 1.0. Which mechs are offered is unchanged.
  `MechSalvageChance` is dead code and is not wired.
