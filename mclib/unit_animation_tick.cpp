#include "unit_animation_tick.h"
#include "unit_anim_state.h"

void mc2::sim::advanceLinearAnimation(UnitAnimState& state, float rate, long frames, float seconds)
{
    const float increment = rate * seconds;
    if (increment == 0.0f) return;
    if (frames <= 0) { state.currentFrame = 0; state.canTransition = true; return; }
    if (state.setFirstFrame) state.setFirstFrame = false;
    else state.currentFrame += state.isReversed ? -increment : increment;
    if (state.currentFrame >= frames) {
        state.currentFrame = state.isLooping ? state.currentFrame - frames : static_cast<float>(frames - 1);
        state.canTransition = true;
    }
    if (state.currentFrame < 0.0f) {
        state.currentFrame = state.isLooping ? state.currentFrame + frames : 0.0f;
        state.canTransition = true;
    }
}
