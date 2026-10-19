#include "tacticaloverview_state.h"

static inline float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

void TacticalOverviewState::setT(float v) { t_ = clamp01(v); }
void TacticalOverviewState::setSetpoint(float v) { setpoint_ = clamp01(v); }

// Wheel and hotkey both move the PERSISTENT setpoint_ (the desired overview
// level). t_ then eases toward setpoint_ in update(). This keeps the two input
// drivers from fighting — previously the wheel moved t_ directly while update()
// dragged t_ back toward setpoint_, so wheel-driven overview snapped back.
void TacticalOverviewState::applyWheel(long delta, bool atCeiling, float /*dt*/,
                                       bool worldOwnsWheel) {
    if (!worldOwnsWheel) return;
    float notches = static_cast<float>(delta) / 120.0f;        // GameOS wheel granularity
    if (delta < 0) {                              // zoom out
        if (atCeiling) setSetpoint(setpoint_ + (-notches) * kWheelGain);
    } else if (delta > 0) {                       // zoom in
        setSetpoint(setpoint_ - notches * kWheelGain);
    }
}

void TacticalOverviewState::toggleHotkey() {
    // Toggle between full overview and gameplay based on the current setpoint.
    setpoint_ = (setpoint_ > 0.5f) ? 0.0f : 1.0f;
}

void TacticalOverviewState::update(float dt) {
    float step = kHotkeyRate * dt;
    if (t_ < setpoint_)      setT(t_ + step > setpoint_ ? setpoint_ : t_ + step);
    else if (t_ > setpoint_) setT(t_ - step < setpoint_ ? setpoint_ : t_ - step);
}

float TacticalOverviewState::iconAlpha(float blend) const {
    if (blend <= kIconFadeLo) return 0.0f;
    if (blend >= kIconFadeHi) return 1.0f;
    return (blend - kIconFadeLo) / (kIconFadeHi - kIconFadeLo);
}
