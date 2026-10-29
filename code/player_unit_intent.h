#pragma once
#include "../mclib/math/glm_config.h"

namespace mc2::player {
// Commander assigns objectives; squads share focus/rally; pilots own execution.
struct SquadIntent {
    unsigned id = 0;
    long leaderWatchId = 0, targetWatchId = 0;
    glm::vec3 rally{};
    bool extracting = false;
};
enum class Goal { Move, Attack, Repair, Recover };
enum class Posture { Advance, Engage, Retreat };
struct UnitIntent {
    long unitWatchId = 0;
    Goal goal = Goal::Move;
    glm::vec3 location{};
    long targetWatchId = 0;
    Posture posture = Posture::Advance;
    bool longRange = false;
};
}
