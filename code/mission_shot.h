#pragma once
#include <string_view>
// Harness-only mission-start screenshot (scripts/sweep_campaign.py --shots). Defined in missionresults.cpp, outside the
// sim include-firewall scope, so mission_update.cpp does not reach GameOS directly.
void mc2MissionStartShot(std::string_view missionName);
