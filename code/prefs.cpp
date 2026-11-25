//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "mc2_czstring.h"
#include"platform_io.h" // for _chmod()

#include <cstring>
#include"prefs.h"
#include"gamesound.h"
#include"missiongui.h"
#include"gos_profiler.h"

#include"soundsys.h"
#include"../resource.h"
#include "mc2fmt.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "Gates/render_options_publish.h"
#include "../GuiRuntime/PlayerFrontEnd.h"

extern void gos_SetPostFxOptions(int casEnabled, float casSharpness, int tonemapMode);   // GameOS/gameos/gos_postprocess.cpp

#include "Gates/preferences_legacy.h"
extern long GameVisibleVertices;

int CPrefs::applyPrefs(bool applyRes) {
	ZoneScopedN("CPrefs::applyPrefs");
	if (soundSystem) {
		ZoneScopedN("CPrefs::applyPrefs soundSystem");

		soundSystem->setDigitalMasterVolume(this->DigitalMasterVolume);
		soundSystem->setSFXVolume(this->sfxVolume);
		soundSystem->setRadioVolume(this->RadioVolume);
		soundSystem->setMusicVolume(this->MusicVolume);
		soundSystem->setBettyVolume(this->BettyVolume);
	}

	mc2::options::runtimePreferences() = static_cast<const mc2::options::Preferences&>(*this);
	mc2UseAsyncMouse = asyncMouse;

	// The effective options: the file's values, then the MC2_OPTIONS_OVERRIDE test channel on top (never written back by save()).
	{
		mc2::options::RenderOptions effective = this->render;
		// The log lines print once per process: applyPrefs() runs again when the player changes a setting.
		static bool s_logged = false;
		const bool report = !s_logged;
		s_logged = true;
		const std::string overrideSpec = mc2::gate::str<"MC2_OPTIONS_OVERRIDE">();
		mc2::options::g_activeOverrides = overrideSpec;
		if (!overrideSpec.empty()) {
			std::vector<mc2::options::Reject> rejects;
			const int accepted = mc2::options::applyOverrides(effective, overrideSpec, rejects);
			if (report) {
				mc2::fmt::print_err("[OPTIONS v1] event=override accepted={} rejected={} spec={}\n", accepted, rejects.size(), mc2::fmt::safe(overrideSpec.c_str()));
				for (const auto& r : rejects)
					mc2::options::logRejectedOption(r.key, r.text, r.why, mc2::options::find(r.key));
			}
		}
		mc2::options::publish(effective);
        PlayerFrontEnd::saveRenderOptions = [](const mc2::options::RenderOptions& options) {
            const auto previous = prefs.render;
            prefs.render = options;
            if (!prefs.save()) { prefs.render = previous; return false; }
            return true;
        };
		mc2::options::saveSpatialAA = [](int mode) {
			const int previous = prefs.render.spatialAA;
			prefs.render.spatialAA = mode;
			if (!prefs.save()) { prefs.render.spatialAA = previous; return false; }
			auto options = mc2::options::current();
			options.spatialAA = mode;
			mc2::options::publish(options);
			return true;
		};
		// Quiet when every reader saw the published values; a reader that cached its value before the publish names itself.
		if (report && (mc2::options::earlyReads() > 0 || mc2::options::publishChangedValues()))
			mc2::fmt::print_err("[OPTIONS v1] event=stale_reader early_reads={} first={}:{} changed_since_start={}\n", mc2::options::earlyReads(), mc2::fmt::safe(mc2::options::firstEarlyFile()), mc2::options::firstEarlyLine(), mc2::options::publishChangedValues() ? 1 : 0);
		gos_SetPostFxOptions(effective.postCas ? 1 : 0, effective.postCasSharpness, effective.tonemapMode);
	}

	int bitDepth = this->bitDepth;

	//Play with the fog distance.
	float fogPercent = static_cast<float>(fogPos) / 100.0f;
	// Override: full visibility, no distance culling
	Camera::MaxClipDistance = 50000.0f;
	Camera::MinHazeDistance = 50000.0f;

	::GameVisibleVertices = 500;

	if (land)
	{
		ZoneScopedN("CPrefs::applyPrefs terrainVisibility");
		land->resetVisibleVertices(::GameVisibleVertices);
		land->update();
	}

	if ( applyRes )
	{
		ZoneScopedN("CPrefs::applyPrefs resolution");
		long localRenderer = renderer;
		if ((renderer != 0) && (renderer != 3))
			localRenderer = 0;

		bool localFullScreen = fullScreen;
		bool localWindow = !fullScreen;
		if (Environment.fullScreen && fullScreen)
			localFullScreen = false;

        {
            ZoneScopedN("CPrefs::applyPrefs gos_SetScreenMode");
            if (renderer == 3)
                gos_SetScreenMode(this->resolutionX, this->resolutionY, bitDepth,0,0,0,true,localFullScreen,0,localWindow,0,localRenderer);
            else
                gos_SetScreenMode(this->resolutionX, this->resolutionY, bitDepth,renderer,0,0,0,localFullScreen,0,localWindow,0,localRenderer);
        }


	}

	return 0;
}

namespace {
template<std::size_t N> void remember(char (&entries)[10][N], mc2::czstring name) {
    if (!name) return;
    char saved[N]{};
    std::strncpy(saved, name, N - 1);
    int index = 0;
    while (index < 9 && entries[index][0] && S_stricmp(saved, entries[index]) != 0) ++index;
    std::memmove(entries[1], entries[0], static_cast<std::size_t>(index) * N);
    std::memcpy(entries[0], saved, N);
}
}
void CPrefs::setNewName(mc2::czstring name) { remember(playerName, name); }
void CPrefs::setNewUnit(mc2::czstring name) { remember(unitName, name); }
void CPrefs::setNewIP(mc2::czstring name) { remember(ipAddresses, name); }
