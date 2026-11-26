#pragma once
#include "Gates/render_options.h"
#include "../code/game_rules.h"
#include <string>
#include <string_view>
#include <vector>

namespace PlayerFrontEnd {
// The game owns CPrefs; the shared UI also links into tools without CPrefs.
extern bool (*saveRenderOptions)(const mc2::options::RenderOptions&);
void loadSettings();
bool prepareCampaignRules(const mc2::rules::GameRules& rules, std::string_view fit);
void drawMainMenu(bool showCredits);
void drawOptions();
void requestCampaignStart(std::string fit);
std::string takeCampaignStart();
std::string takeMissionStart();
std::string takeSaveLoad();
int restartIfRequested(int argc, char** argv);

// New Campaign menu: every installed campaign (stock + each mod), not only the mounted layer's.
struct CampaignMenuEntry { std::string key, text; };
std::vector<CampaignMenuEntry> otherModCampaigns();   // campaigns that need a different mod layer; key = "<mod>::<fit>"
inline constexpr std::string_view kStockNoModKey = "::campaign";   // stock campaign with no mod layer mounted
inline bool isCrossModKey(std::string_view key) { return key.find("::") != std::string_view::npos; }
// A campaign save's General block records the mounted layers as ActiveMod / ModDeps ("-" = none, an empty quoted string reads back as absent).
inline std::string saveModField(std::string_view value) { return value.empty() ? std::string("-") : std::string(value); }
inline std::string loadModField(std::string_view value) { return value == "-" ? std::string() : std::string(value); }
std::string activeModId();                           // the mounted campaign mod ("" = stock)
std::string activeModDeps();                           // the mounted add-on layers (MC2_MOD_DEPS)
// Picking a key from otherModCampaigns() (or "::campaign" = stock with no mod layer): save the choice, relaunch the game into it.
bool relaunchForCampaign(std::string_view key);
// A campaign save records the mod layers it was made under; when they differ from the mounted ones, relaunch into them and load it there.
bool relaunchForSave(bool recorded, std::string_view savedMod, std::string_view savedDeps, std::string_view saveFile);
}
