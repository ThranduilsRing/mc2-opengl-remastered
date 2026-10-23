#pragma once
#include "../../RenderCore/MissionEnvironment.h"
#include <string_view>

// MISSION-VISUAL-TUNING-1: optional per-mission renderer tuning profiles.
//
// Reads data/visual_tuning.json at mission start and applies global defaults
// then mission-specific overrides to existing renderer globals/tunables.
//
// Precedence: engine defaults < profile defaults < mission overrides < env vars < ImGui.
// Every load resets all supported values; missing entries cannot inherit a mission.
// Invalid/unknown key = one-time warning, no crash.
//
// Override profile path with env var MC2_VISUAL_TUNING_FILE.

void visualTuning_applyProfile(const char* missionName);
bool visualTuning_saveCurrentToMission();  // write current slider state to JSON for active mission

const char* visualTuning_getProfilePath();
const char* visualTuning_getActiveMission();
bool        visualTuning_hasProfileFile();
int         visualTuning_getAppliedKeyCount();

struct VisualTuningSource {
    std::string path;
    std::string provider;
    std::string text;
    bool editable = false;
    std::string error;
};

// Version 1 is the unversioned shipped flat-key vocabulary. Version 2 records
// that vocabulary in an explicit compatibility environment without changing it.
RenderCore::MissionEnvironment visualTuning_resolve(
    std::string_view mission, const std::vector<VisualTuningSource>& sources,
    const std::map<std::string, float>& sessionOverrides = {});
const RenderCore::MissionEnvironment& visualTuning_environment();
void visualTuning_setMissionLighting(const RenderCore::LegacyMissionLighting& lighting);
std::string visualTuning_environmentJson();
