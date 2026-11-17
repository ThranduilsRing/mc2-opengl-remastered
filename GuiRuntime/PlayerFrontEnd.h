#pragma once
#include "Gates/render_options.h"
#include <string>

namespace PlayerFrontEnd {
// The game owns CPrefs; the shared UI also links into tools without CPrefs.
extern bool (*saveRenderOptions)(const mc2::options::RenderOptions&);
void loadSettings();
void drawMainMenu(bool showCredits);
void drawOptions();
void requestCampaignStart(std::string fit);
std::string takeCampaignStart();
std::string takeMissionStart();
int restartIfRequested(int argc, char** argv);
}
