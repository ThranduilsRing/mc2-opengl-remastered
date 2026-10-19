//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef PREFS_H
#define PREFS_H

#ifndef MCLIB_H
#include"mclib.h"
#endif

#include "Gates/preferences_types.h"


class CPrefs : public mc2::options::Preferences {
public:
	CPrefs() = default;
	int load( const char* pFileName = "options");
	int save();
	int applyPrefs(bool bApplyResolution = 1);

	void setNewName( const char* pNewName );
	void setNewUnit( const char* pNewUnit );
	void setNewIP( const char* pNewIP );

	// Player render options (the options.cfg keys listed in Gates/render_options.h). load() validates them, applyPrefs() publishes them.
	mc2::options::RenderOptions render;

private:
    bool loadedDefault_ = false;
    std::string sourceText_;
    std::vector<mc2::options::PreferenceValue> loadedValues_;
    unsigned long long loadedKeyboard_ = 0;
};

extern CPrefs prefs;
#endif /*PREFS_H*/
