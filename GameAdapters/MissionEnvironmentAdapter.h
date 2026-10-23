#pragma once
#include "../GameOS/gameos/visual_tuning_profile.h"

namespace GameAdapters::Environment {
VisualTuningSource readProfileSource(std::string_view path);
const RenderCore::LegacyMissionLighting& publishLighting(const RenderCore::LegacyMissionLighting& lighting);
}
