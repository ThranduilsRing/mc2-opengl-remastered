#pragma once
#include "../mclib/math/glm_config.h"

namespace mc2::player {
// Commander output for one unit. L1b supplies these from the brain runtime;
// L1a translates them through Mission::submitPlayerOrder, just like the UI.
enum class Goal { Move, Attack };
enum class Posture { Advance, Engage };
struct UnitIntent {
    long unitWatchId = 0;
    Goal goal = Goal::Move;
    glm::vec3 location{};
    long targetWatchId = 0;
    Posture posture = Posture::Advance;
};
}
