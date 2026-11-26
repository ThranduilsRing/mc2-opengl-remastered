//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef PREFS_H
#define PREFS_H

#ifndef MCLIB_H
#include"mclib.h"
#endif

#include "mc2_czstring.h"
#include "Gates/preferences_types.h"
#include "game_rules.h"
#include <expected>

// The @ toggles and add-ons the launcher gates select (MC2_MOD_DEPS, the cheat and Faster Weapons gates). legacyCampaignMods() adds
// every add-on the builds before campaign add-on selection auto-mounted: what a save without a Mods key ran with.
namespace mc2::rules { std::string launcherModDefaults(); std::string legacyCampaignMods(); }


class CPrefs : public mc2::options::Preferences {
public:
	CPrefs() = default;
	int load( mc2::czstring pFileName = "options");
	std::expected<void, int> save();
	int applyPrefs(bool bApplyResolution = 1);

	void setNewName( mc2::czstring pNewName );
	void setNewUnit( mc2::czstring pNewUnit );
	void setNewIP( mc2::czstring pNewIP );

	// Player render options (the options.cfg keys listed in Gates/render_options.h). load() validates them, applyPrefs() publishes them.
	mc2::options::RenderOptions render;

	// Rules for solo skirmishes and new campaigns: the last ones the player used (options.cfg "GameRules<Key>", code/game_rules.h).
	// Kept here, not in Preferences, so Gates/ and mclib never see game_rules.h.
	mc2::rules::GameRules gameRules;

private:
    std::vector<mc2::options::PreferenceValue> snapshotValues() const;
    bool loadedDefault_ = false;
    std::string sourceText_;
    std::vector<mc2::options::PreferenceValue> loadedValues_;
    unsigned long long loadedKeyboard_ = 0;
};

extern CPrefs prefs;
#endif /*PREFS_H*/
