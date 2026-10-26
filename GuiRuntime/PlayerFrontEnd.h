#pragma once
#include "Gates/render_options.h"
#include <string>

namespace PlayerFrontEnd {
// The game owns CPrefs; the shared UI also links into tools without CPrefs.
extern bool (*saveRenderOptions)(const mc2::options::RenderOptions&);
void loadSettings();
void drawMainMenu();
void drawOptions();
std::string takeCampaignStart();
int restartIfRequested(int argc, char** argv);
}
