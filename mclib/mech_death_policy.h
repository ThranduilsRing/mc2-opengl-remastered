#pragma once

// Values are persisted in MoverData; never renumber existing causes.
enum DeathCause
{
    DEBUGGER_DEATH = 0,
    ENGINE_DEATH = 1,
    PILOT_DEATH = 2,
    EJECTION_DEATH = 3,
    UNDETERMINED_DEATH = 4,
    POWER_USED_UP = 5,
    CORE_EXPLOSION_DEATH = 6,
    SELF_DESTRUCT_DEATH = 7
};

namespace mc2wreck {
constexpr bool salvageable(bool disabled, bool destroyed, int cause, bool salvageAll = false)
{
    return cause != CORE_EXPLOSION_DEATH && cause != SELF_DESTRUCT_DEATH &&
        disabled && (!destroyed || salvageAll);
}

// A replacement hulk is already posed. Without one, keep evaluating the
// original fall/death animation (including imported per-actor skin palettes).
constexpr bool animate(bool destroyed, bool replacementHulk)
{
    return !destroyed || !replacementHulk;
}
}
