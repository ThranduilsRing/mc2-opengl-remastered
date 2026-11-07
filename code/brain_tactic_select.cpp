// brain_tactic_select.cpp — TACTIC-WEIGHTS-A deterministic weighted tactic selection.
// Gate: MC2_TACTIC_WEIGHTS (default OFF).
// Pure functions — no global state, no warrior mutation.

#include <algorithm>
#include "brain_tactic_select.h"
#include "tacordr.h"
#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------------
// Wang hash (32-bit finaliser)
// Input mix: wid XOR tick*2654435761u XOR missionSeed*1013904223u then Wang-mix.
uint32_t tacticWang(int wid, uint32_t tick, uint32_t missionSeed)
{
    uint32_t h = static_cast<uint32_t>(wid)
               ^ (tick        * 2654435761u)
               ^ (missionSeed * 1013904223u);
    // Wang finaliser (avalanche)
    h ^= (h >> 16);
    h *= 0x45d9f3bu;
    h ^= (h >> 16);
    h *= 0x45d9f3bu;
    h ^= (h >> 16);
    return h;
}

// ---------------------------------------------------------------------------
// tacticName — short name for trace output
const char* tacticName(int idx)
{
    return tactic_table::name(idx);
}

// ---------------------------------------------------------------------------
// Pilot modulation
// lerp helper (clamped [0,1])
static float lerpClamped(float a, float b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return a + (b - a) * t;
}

void applyPilotModulation(const float wBase[], float wEff[], int numTactics,
                          int gunnery, int aggressiveness, int courage)
{
    // Copy base weights
    for (int i = 0; i < numTactics; i++)
        wEff[i] = (wBase[i] < 0.0f) ? 0.0f : wBase[i];

    // Normalised stat values [0..1]
    float aggNorm  = static_cast<float>(aggressiveness) / 10.0f;
    float gunNorm  = static_cast<float>(gunnery)        / 10.0f;

    // High aggressiveness (>0.6) lifts aggressive tactics, depresses defensive.
    // lerp(lo=0.7, hi=1.5, aggNorm) for aggressive; lerp(1.3, 0.5, aggNorm) for defensive.
    float aggBoost  = lerpClamped(0.7f, 1.5f, aggNorm);  // Pursue/HitAndRun/Joust
    float aggDepress = lerpClamped(1.3f, 0.5f, aggNorm);  // FightingWithdraw/HullDown

    wEff[TACTIC_JOUST]             *= aggBoost;
    wEff[TACTIC_PURSUE]            *= aggBoost;
    wEff[TACTIC_HIT_AND_RUN]       *= aggBoost;
    wEff[TACTIC_FIGHTING_WITHDRAW] *= aggDepress;
    wEff[TACTIC_HULL_DOWN]         *= aggDepress;

    // High gunnery (>0.6) lifts precision tactics.
    float gunBoost = lerpClamped(0.8f, 1.4f, gunNorm);
    wEff[TACTIC_INDIRECT_FIRE]  *= gunBoost;
    wEff[TACTIC_STOP_AND_FIRE]  *= gunBoost;

    // Courage<3 zeroes aggressive tactics (pilot won't commit)
    if (courage < 3) {
        wEff[TACTIC_PURSUE]      = 0.0f;
        wEff[TACTIC_HIT_AND_RUN] = 0.0f;
        wEff[TACTIC_JOUST]       = 0.0f;
    }

    // Normalize to sum=1; all-zero fallback → TACTIC_STOP_AND_FIRE
    float sum = 0.0f;
    for (int i = 0; i < numTactics; i++)
        sum += wEff[i];

    if (sum <= 0.0f) {
        // All-zero: fallback to STOP_AND_FIRE
        for (int i = 0; i < numTactics; i++) wEff[i] = 0.0f;
        if (TACTIC_STOP_AND_FIRE < numTactics)
            wEff[TACTIC_STOP_AND_FIRE] = 1.0f;
    } else {
        float invSum = 1.0f / sum;
        for (int i = 0; i < numTactics; i++)
            wEff[i] *= invSum;
    }
}

// ---------------------------------------------------------------------------
// Weighted pick
int selectTactic(const float wEff[], int numTactics, uint32_t hash)
{
    // Map hash to [0, 1)
    float r = static_cast<float>((hash & 0xFFFFFFu)) / 16777216.0f;

    float cumulative = 0.0f;
    for (int i = 0; i < numTactics; i++) {
        cumulative += wEff[i];
        if (r < cumulative)
            return i;
    }
    // Numerical fallback (rounding overshoot): return last non-zero weight tactic
    for (int i = numTactics - 1; i >= 0; i--) {
        if (wEff[i] > 0.0f)
            return i;
    }
    return TACTIC_STOP_AND_FIRE;
}
