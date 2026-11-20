#define MAINMENU_CPP
/*************************************************************************************************\
MainMenu.cpp			: Implementation of the MainMenu component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifdef LINUX_BUILD
#include "platform_windows.h"
struct DDSURFACEDESC2 {
};
#else
#include<windows.h>
#include<ddraw.h>
#endif
#include "mc2_czstring.h"
#include"mainmenu.h"
#include"mclib.h"
#include"inifile.h"
#include"logisticsdata.h"
#include "prewarm.h"
#include"logisticsdialog.h"
#include"gamerulesdialog.h"
#include"abutton.h"
#include"optionsscreenwrapper.h"
#include "../resource.h"
#include"mechlopedia.h"
#include"gamesound.h"
#include"aanimobject.h"
#include"multplyr.h"

#include"prefs.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "mc2fmt.h"
#include "../GuiRuntime/PlayerFrontEnd.h"
#include "../GuiRuntime/GuiRuntime.h"
#include "imgui.h"
#include "net/runtime.h"
#include "diagnostic_trace.h"
#include "utils/mc2log.h"
#include <set>
extern CPrefs prefs;
#include "screen_settle_capture.h"

namespace {
bool& coopOpen = mc2::net::lobbyVisible();
// Report real hit regions to the tailed mouse script; never activate controls here.
void coopProofWidget(std::string_view name, float left, float top, float right, float bottom, float width, float height) {
    if (mc2::gate::str<"MC2_INPUT_SCRIPT">().empty()) return;
    static std::set<std::string> reported;
    const auto [position, inserted] = reported.emplace(name);
    if (inserted && mc2_diag::tagRequested("COOP_UI"))
        std::fprintf(stderr, "[COOP_UI] %.*s %.3f %.3f\n", static_cast<int>(name.size()), name.data(), (left + right) * 400 / width, (top + bottom) * 300 / height);
}
void coopProofItem(std::string_view name) {
    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const auto size = ImGui::GetIO().DisplaySize;
    coopProofWidget(name, a.x, a.y, b.x, b.y, size.x, size.y);
}
void drawCoopLobby() {
    if (!ImGui::GetCurrentContext()) return;
    const auto* viewport = ImGui::GetMainViewport();
    const float scale = viewport->WorkSize.y / 1080.0f;
    if (!coopOpen) return;
    mc2::net::pumpLobby();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
        viewport->WorkPos.y + viewport->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(740 * scale, 520 * scale));
    ImGui::Begin("Co-op lobby", &coopOpen, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
    ImGui::SetWindowFontScale(scale);
    static char endpoint[64] = "127.0.0.1:23460";
    static int selected = 0;
    const auto& state = mc2::net::lobbyState();
    ImGui::TextUnformatted("MECHCOMMANDER 2 / CO-OP");
    ImGui::Separator();
    if (!state.offered && !mc2::net::lobbyConnected()) {
        ImGui::InputText("IP:port", endpoint, sizeof(endpoint));
        ImGui::TextWrapped("Host: enter this PC's LAN IPv4 address and port. Join: enter the host's address.");
        const auto missionName = [](int index) {
            char name[16]; std::snprintf(name, sizeof(name), "mc2_%02d", index + 1);
            return std::string(name);
        };
        if (ImGui::BeginCombo("Stock mission", missionName(selected).c_str())) {
            for (int i = 0; i < 24; ++i)
                if (ImGui::Selectable(missionName(i).c_str(), selected == i)) selected = i;
            ImGui::EndCombo();
        }
        if (ImGui::Button("Host game")) {
            mc2::net::openLobby(true, endpoint, missionName(selected));
            if (state.error.empty() && mc2_diag::tagRequested("COOP_UI")) std::fprintf(stderr, "[COOP_UI] Listening 0 0\n");
        }
        coopProofItem("Host");
        ImGui::SameLine();
        if (ImGui::Button("Join game")) mc2::net::openLobby(false, endpoint, "mc2_01");
        coopProofItem("Join");
        ImGui::TextWrapped("Waiting for a peer after Host or Join. Both players must use identical content.");
    }
    if (state.offered && state.error.empty()) {
        ImGui::Text("Mission: %s", state.mission.c_str());
        ImGui::Text("Host: %s", (state.hosting ? state.localReady : state.peerReady) ? "READY" : "not ready");
        ImGui::Text("Player 2: %s", (state.hosting ? state.peerReady : state.localReady) ? "READY" : "not ready");
        bool ready = state.localReady;
        if (ImGui::Checkbox("I'm ready", &ready)) mc2::net::lobbyReady(ready);
        if (!state.localReady) coopProofItem("Ready");
        if (state.hosting) {
            ImGui::BeginDisabled(!state.canStart());
            if (ImGui::Button("Start mission")) mc2::net::lobbyStart();
            if (state.canStart()) coopProofItem("Start");
            ImGui::EndDisabled();
        } else ImGui::TextUnformatted("The host starts when both players are ready.");
    }
    ImGui::PushTextWrapPos();
    if (!state.error.empty()) ImGui::TextUnformatted(state.error.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Back (Esc)") || ImGui::IsKeyPressed(ImGuiKey_Escape) || userInput->getKeyDown(KEY_ESCAPE)) coopOpen = false;
    ImGui::End();
    if (!coopOpen) mc2::net::closeLobby();
}
}


constexpr int MM_MSG_NEW_CAMPAIGN = 90;
constexpr int MM_MSG_SAVE = 92;
constexpr int MM_MSG_LOAD = 91;
constexpr int MM_MSG_MULTIPLAYER = 93;
constexpr int MM_MSG_RETURN_TO_GAME = 94;
constexpr int MM_MSG_OPTIONS = 95;
constexpr int MM_MSG_ENCYCLOPEDIA = 96;
constexpr int MM_MSG_EXIT = 97;
constexpr int MM_MSG_SINGLE_MISSION = 98;
constexpr int MM_MSG_LEGAL = 99;

extern volatile bool mc2IsInMouseTimer;
extern volatile bool mc2IsInDisplayBackBuffer;

void MouseTimerKill();

extern void (*AsynFunc)(RECT& WinRect,DDSURFACEDESC2& mouseSurfaceDesc );




extern bool bInvokeOptionsScreenFlag;
bool	MainMenu::bDrawMechlopedia = false;;
bool	MainMenu::bLoadShown = false;

void SplashIntro::init()
{
	FullPathFileName path;
	path.init( artPath, "mcl_splashscreenintro", ".fit" );

	FitIniFile file;
	if ( NO_ERR != file.open( path ) )
	{
		char errorStr[256];
		(*std::format_to_n( errorStr, sizeof(errorStr) - 1, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(path)) ).out = '\0');
		Assert(0,0,errorStr );
	}

	LogisticsScreen::init( file, "Static", "Text", "Rect", "Button", nullptr, "AnimObject" );
}

MainMenu::MainMenu(  )
{
	optionsScreenWrapper = nullptr;
	bOptions = 0;
	bSave = bLoad = 0;
	helpTextArrayID = 0;
	mechlopedia = nullptr;
	bDrawMechlopedia = 0;
	tuneId = -1;
	bLoadSingle = 0;
	bLoadCampaign = 0;
	bRulesDlg = false;
	rulesForSolo = false;
	introOver = 0;
	bHostLeftDlg = 0;

	introMovie = nullptr;
}

//-------------------------------------------------------------------------------------------------

MainMenu::~MainMenu()
{
	if ( optionsScreenWrapper )
		optionsScreenWrapper.reset();
}

int MainMenu::init( FitIniFile& file )
{
	file.seekBlock("Tunes");
	file.readIdLong("TuneId",tuneId);

	setLayoutNeeds( { "mcl_mm.fit", 14, 3, 10, 0, 2, false } ); // stock element counts; see LayoutNeeds
	LogisticsScreen::init( file, "Static", "Text", "Rect", "Button", nullptr, "AnimObject" );

	FullPathFileName name;
	name.init( artPath, "mcl_sp", ".fit" );
	FitIniFile file2;
	if ( NO_ERR != file2.open( name ) )
	{
		char errorStr[256];
		(*std::format_to_n( errorStr, sizeof(errorStr) - 1, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(name)) ).out = '\0');
		Assert(0,0,errorStr );
	}

	background.init( file2, "Static", "Text", "Rect", "Button", nullptr, "AnimObject" );

	for ( int i = 0; i < buttonCount; i++ )
	{
		buttons[i].setMessageOnRelease();
		buttons[i].setPressFX(LOG_VIDEOBUTTONS);
		buttons[i].setHighlightFX(LOG_DIGITALHIGHLIGHT);
	}

	beginAnim.initWithBlockName( &file, "InAnim" );
	endAnim.initWithBlockName( &file, "OutAnim" );


	intro.init();

	FullPathFileName path;
	path.init( artPath, "mcl_mp_loadmap", ".fit" );

	FitIniFile mpFile;

	if ( NO_ERR != mpFile.open( path ) )
	{
		char error[256];
		(*std::format_to_n( error, sizeof(error) - 1, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(path)) ).out = '\0');
		Assert( 0, 0, error );
		return -1;		
	}

	singleLoadDlg.init( &mpFile );

	introMovie = nullptr;

	// Open on the first displayed update, after startup work and renderer setup.
	introMovie = new MC2Movie;

	return 0;
}

void MainMenu::begin()
{
	status = RUNNING;
	promptToQuit = 0;
	beginAnim.begin();
	beginFadeIn( 0 );
	endAnim.end();
	background.beginFadeIn( 0 );
	endResult = RUNNING;
	musicStarted = false;
	bLoadSingle = 0;
	bLoadCampaign = 0;
	bRulesDlg = false;
	promptToDisconnect = 0;
	bLegal = 0;

	// no host left dlg, sometimes we get this begin call 2x's due to netlib weirdness
	if ( !introMovie )
	{
			while (mc2IsInMouseTimer)
				;

			//ONLY set the mouse BLT data at the end of each update.  NO MORE FLICKERING THEN!!!
			// BLOCK THREAD WHILE THIS IS HAPPENING
			mc2IsInDisplayBackBuffer = true;

			mc2UseAsyncMouse = prefs.asyncMouse;
			if ( !mc2UseAsyncMouse)
				MouseTimerKill();

			mc2IsInDisplayBackBuffer = false;

			AsynFunc = nullptr;

			//Force mouse Cursors to smaller or larger depending on new video mode.
			userInput->initMouseCursors("cursors");
			userInput->mouseOn();
			userInput->setMouseCursor( mState_LOGISTICS );

			DWORD localRenderer = prefs.renderer;
			if (prefs.renderer != 0 && prefs.renderer != 3)
				localRenderer = 0;

   			bool localFullScreen = prefs.fullScreen;
   			bool localWindow = !prefs.fullScreen;
   			if (Environment.fullScreen && prefs.fullScreen)
   				localFullScreen = false;


			// make sure we get into 800 x 600 mode
			if ( Environment.screenWidth != 800 )
			{
			
				if (prefs.renderer == 3)
					gos_SetScreenMode(800,600,16,0,0,0,true,localFullScreen,0,localWindow,0,localRenderer);
				else
					gos_SetScreenMode(800,600,16,prefs.renderer,0,0,0,localFullScreen ,0,localWindow,0,localRenderer);
			}


	}
}

void MainMenu::end()
{
	endAnim.end();
	bHostLeftDlg = 0;

}

void MainMenu::setDrawBackground( bool bNewDrawBackground )
{
	bDrawBackground = bNewDrawBackground;
	if ( bDrawBackground && !introOver)
	{
		intro.begin();
	}
}

// A campaign that cannot start or load: tell the player with the existing one-button dialog (the host-left dialog slot) instead of silently doing nothing.
static void showCampaignRefusal( bool& dialogFlag, mc2::czstring text )
{
	// The czstring setText leaves the button label unset; the 3-arg form sets it to OK, then the text is replaced.
	LogisticsOneButtonDialog::instance()->setText( IDS_DIALOG_OK, IDS_DIALOG_OK, IDS_DIALOG_OK );
	LogisticsOneButtonDialog::instance()->setText( text );
	LogisticsOneButtonDialog::instance()->begin();
	dialogFlag = true;
}

// New Campaign: after the picker, the Game Rules dialog (hidden in multiplayer: rules are forced to the defaults there).
bool MainMenu::beginCampaignRules( std::string_view campaignFile )
{
	if ( MPlayer || mc2::net::active() || !GameRulesDialog::init() )
		return false;
	rulesCampaign = campaignFile;
	bLoadCampaign = false;
	bRulesDlg = true;
	GameRulesDialog::instance()->begin( prefs.gameRules );
	return true;
}

int	MainMenu::handleMessage( unsigned long what, unsigned long who )
{
	switch ( who )
	{
		case MM_MSG_NEW_CAMPAIGN:
			if ( MPlayer )
			{
				LogisticsOKDialog::instance()->setText( IDS_PROMPT_TO_DISCONNECT, IDS_DIALOG_NO, IDS_DIALOG_YES );
				LogisticsOKDialog::instance()->begin();
				endResult = MM_MSG_NEW_CAMPAIGN;
				promptToDisconnect = true;
			}
			else
			{
				endAnim.begin();
				beginAnim.end();
				bLoadCampaign = true;
				LogisticsSaveDialog::instance()->beginCampaign();
				if ( LogisticsSaveDialog::instance()->isDone() )
				{
					// a missing campaign file is logged by startNewCampaign; stay on the menu instead of RESTART
					if ( beginCampaignRules( LogisticsSaveDialog::instance()->getFileName() ) )
						;	// the Game Rules dialog takes over; ACCEPT starts the campaign
					else if ( LogisticsData::instance->startNewCampaign( LogisticsSaveDialog::instance()->getFileName().c_str()) )
						status = RESTART;
					else
					{
						endAnim.end();
						beginAnim.begin();
						bLoadCampaign = false;
						showCampaignRefusal( bHostLeftDlg, "This campaign could not be started: its campaign file is missing or damaged." );
					}
				}
			}
			break;
		case MM_MSG_SAVE:
			if ( MPlayer )
			{
				LogisticsOKDialog::instance()->setText( IDS_PROMPT_TO_DISCONNECT, IDS_DIALOG_NO, IDS_DIALOG_YES );
				LogisticsOKDialog::instance()->begin();
				endResult = who;
				promptToDisconnect = true;
			}
			else
			{
		
				// need to pop dialog here
				LogisticsSaveDialog::instance()->begin();
				endAnim.begin();
				beginAnim.end();
				bSave = true;
			}
			break;

		case MM_MSG_LOAD:

			if ( MPlayer )
			{
				LogisticsOKDialog::instance()->setText( IDS_PROMPT_TO_DISCONNECT, IDS_DIALOG_NO, IDS_DIALOG_YES );
				LogisticsOKDialog::instance()->begin();
				endResult = who;
				promptToDisconnect = true;
			}
			else
			{
			
				// need to pop dialog here
				LogisticsSaveDialog::instance()->beginLoad();
				endAnim.begin();
				beginAnim.end();
				bLoad = true;
			}
			break;
		case MM_MSG_MULTIPLAYER:
			coopOpen = mc2::net::lobbyFrontEndEligible(bDrawBackground);
			break;

		case MM_MSG_RETURN_TO_GAME:
			{
			if ( !bDrawBackground )
			{
				endAnim.begin();
				beginAnim.end();
				endResult = NEXT;
				soundSystem->playDigitalSample( LOG_MAINMENUBUTTON );
				soundSystem->playDigitalMusic(LogisticsData::instance->getCurrentMissionTune().value_or(-1));
			}
			}
			break;
		case MM_MSG_OPTIONS:
			// need to throw up the options screen here...
			if (!optionsScreenWrapper)
			{
				optionsScreenWrapper = std::make_unique_for_overwrite<OptionsScreenWrapper>();
				optionsScreenWrapper->init();
			}
			optionsScreenWrapper->begin();

			bOptions = true;
			break;
		case MM_MSG_ENCYCLOPEDIA:
			bDrawMechlopedia = true;
			beginFadeOut(1.0);
			if ( !mechlopedia )
			{
				mechlopedia = new Mechlopedia;
				mechlopedia->init();
			}

			mechlopedia->begin();
			break;
		case MM_MSG_EXIT:
			promptToQuit = 1;
			// may need to set the text here
			LogisticsOKDialog::instance()->setText( IDS_DIALOG_QUIT_PROMPT,
					IDS_DIALOG_NO, IDS_DIALOG_YES );
				
			LogisticsOKDialog::instance()->begin();
			getButton( who )->press( 0 );
			break;

		case MM_MSG_SINGLE_MISSION:

			if ( MPlayer )
			{
				LogisticsOKDialog::instance()->setText( IDS_PROMPT_TO_DISCONNECT, IDS_DIALOG_NO, IDS_DIALOG_YES );
				LogisticsOKDialog::instance()->begin();
				endResult = who;
				promptToDisconnect = true;
			}
			else
			{
				bLoadSingle = true;
				endAnim.begin();
				beginAnim.end();
				singleLoadDlg.beginSingleMission();
				getButton( who )->press( 0 );
			}
			break;

		case MM_MSG_LEGAL:
			{
				bLegal = 1;
				// may need to set the text here
				if ( !LogisticsLegalDialog::instance() )
				{
					FullPathFileName path;
					path.init( artPath, "mcl_dialoglegal", ".fit" );
					FitIniFile file;
					file.open( path );
					LogisticsLegalDialog::init( file );
				}
				LogisticsLegalDialog::instance()->setText( IDS_DIALOG_OK,
						IDS_DIALOG_OK, IDS_DIALOG_OK );
				//Needs to be this long for LOC!
				// -fs
				char realText[2048];
				cLoadString(IDS_LAWYER_BABBLE, realText, 2047 );
				char lawyerBabble[2048];
				DWORD pIDLen = 64;
				char pID[64];
				(*std::format_to_n( pID, sizeof(pID) - 1, "INVALID ID" ).out = '\0');
				gos_LoadDataFromRegistry("PID", pID, &pIDLen);
				sprintf( lawyerBabble, realText, pID );
				LogisticsLegalDialog::instance()->setText( lawyerBabble );
				LogisticsLegalDialog::instance()->begin();
				LogisticsLegalDialog::instance()->setFont( IDS_LAWYER_BABBLE_FONT );				
				getButton( who )->press( 0 );
			}
			break;
		default:
			break;
	}

	return 0;
}

void MainMenu::skipIntro()
{
	if ( introMovie )
	{
		introMovie->stop();
		delete introMovie;
		introMovie = nullptr;

	}
}

void MainMenu::update()
{
    if (coopOpen) return;
	{	// via-menu timing marker (scripts/run_smoke.py --via-menu reads its wallclock stamp): the first time the main menu runs
		static bool s_menuMarked = false;
		if ( !s_menuMarked ) { s_menuMarked = true; mc2::fmt::print_out("[VIA_MENU v1] event=main_menu\n"); }
	}
    const auto pickedMission = PlayerFrontEnd::takeMissionStart();
    if (!pickedMission.empty() && LogisticsData::instance) {
        skipIntro();
        LogisticsData::instance->setSingleMission(pickedMission.c_str());
        LogisticsData::instance->setRules(prefs.gameRules);
        status = SKIPONENEXT;
        return;
    }
    if (!introMovie) mc2::prewarm::pumpMain(4.0, mc2::prewarm::Screen::MainMenu, !bDrawBackground);
    const auto pickedCampaign = PlayerFrontEnd::takeCampaignStart();
    if (!pickedCampaign.empty() && LogisticsData::instance) {
        skipIntro();
        if (LogisticsData::instance->startNewCampaign(pickedCampaign.c_str()))
            status = RESTART;
        else
            showCampaignRefusal( bHostLeftDlg, "This campaign could not be started: its campaign file is missing or damaged." );
        return;
    }
	// MC2_BOOT_TO_BAY=<campaign .fit basename> (e.g. "pictures of a rebellion"):
	// headless boot for screen capture. One-shot: auto-start the campaign (mirrors
	// the New-Campaign menu action) and request RESTART, which enters MissionBegin;
	// MissionBegin then jumps straight to the mech bay (see its RESTART handler) so
	// the LOGISTICS capture can dump the bay/purchase/loadout/launch screens with no
	// clicks. Requires MC2_ACTIVE_MOD + MC2_MOD_DEPS set (as the launcher does).
	{
		static bool s_bootToBayFired = false;
		const std::string bootCampText = mc2::gate::str<"MC2_BOOT_TO_BAY">();   // "" when unset or empty
		const char* bootCamp = bootCampText.c_str();
		// The intro movie plays first unless it was skipped (MC2_MENU_CANARY_SKIP_INTRO, which MissionBegin honours by dropping it):
		// a --via-menu run must pay it like a player; the headless harnesses all set the skip.
		if ( !s_bootToBayFired && bootCamp[0] && LogisticsData::instance && !introMovie )
		{
			s_bootToBayFired = true;
			if ( !LogisticsData::instance->startNewCampaign( bootCamp ) )
			{
				// missing campaign: error already logged, stay on the main menu
				showCampaignRefusal( bHostLeftDlg, "This campaign could not be started: its campaign file is missing or damaged." );
				return;
			}
			// MC2_BOOT_TO_MISSION=<mission FileName>: jump to a specific mission's
			// logistics instead of the campaign's first mission. Used to boot the
			// LAST purchasing-enabled mission (fullest roster -> highest icon indices)
			// for icon-atlas verification. startNewCampaign defaults to mission 0.
			const std::string bootMissionText = mc2::gate::str<"MC2_BOOT_TO_MISSION">();   // "" when unset or empty
			const char* bootMission = bootMissionText.c_str();
			if ( bootMission[0] )
				LogisticsData::instance->setCurrentMissionAnyStage( bootMission );
			status = RESTART;
			return;
		}
	}

	// MC2_BOOT_TO_SCREEN=encyclopedia: headless boot straight into the
	// Mechlopedia from the main menu (mirrors MM_MSG_ENCYCLOPEDIA, no fade so
	// captures start immediately). One-shot; harness-capturable without clicks.
	{
		static bool s_bootToEncycloFired = false;
		const std::string bootScrText = mc2::gate::str<"MC2_BOOT_TO_SCREEN">();   // "" when unset or empty
		const char* bootScr = bootScrText.c_str();
		// prefix match: "encyclopedia" or "encyclopedia_<tab>" (tab handled in
		// Mechlopedia::begin)
		if ( !s_bootToEncycloFired && bootScr[0] && !S_strnicmp(bootScr, "encyclopedia", 12)
			&& !mc2::gate::isNonEmpty<"MC2_BOOT_TO_BAY">() && !introMovie )
		{
			s_bootToEncycloFired = true;
			bDrawMechlopedia = true;
			if ( !mechlopedia )
			{
				mechlopedia = new Mechlopedia;
				mechlopedia->init();
			}
			mechlopedia->begin();
		}
	}

	if ( bDrawBackground || MPlayer || LogisticsData::instance->isSingleMission() )
	{
		getButton( MM_MSG_SAVE )->disable( true );
	}
	else
		getButton( MM_MSG_SAVE )->disable( false );

	getButton( MM_MSG_MULTIPLAYER )->disable( !mc2::net::lobbyFrontEndEligible(bDrawBackground) );

	if ( introMovie )
	{
		mc2::prewarm::pumpMain(4.0, mc2::prewarm::Screen::FrontEndMovie, false);
		userInput->mouseOff();
		if (!introMovie->getMovieName()) {
			FullPathFileName path;
			path.init(moviePath, "msft", ".bik");
			RECT movieRect = {0, 0, Environment.screenWidth, Environment.screenHeight};
			introMovie->init(path, movieRect, true);
		}

		{
			static int s_introDbgFrame = 0;
			gosEnum_KeyStatus escSt  = gos_GetKeyStatus(KEY_ESCAPE);
			gosEnum_KeyStatus spcSt  = gos_GetKeyStatus(KEY_SPACE);
			gosEnum_KeyStatus lmsSt  = gos_GetKeyStatus(KEY_LMOUSE);
			if (escSt != KEY_FREE || spcSt != KEY_FREE || lmsSt != KEY_FREE || s_introDbgFrame % 60 == 0)
				printf("[INTRO_MOVIE] frame=%d esc=%d spc=%d lmouse=%d\n", s_introDbgFrame, static_cast<int>(escSt), static_cast<int>(spcSt), static_cast<int>(lmsSt));
			++s_introDbgFrame;
		}

		if (userInput->getKeyDown(KEY_SPACE) || userInput->getKeyDown(KEY_ESCAPE) || userInput->getKeyDown(KEY_LMOUSE))
		{
			introMovie->stop();
		}

		bool result = introMovie->update();
		if (result)
		{
			
			//Movie's Over.
			//Whack it.
			delete introMovie;
			introMovie = nullptr;
		}

		return;
	}

	if (!musicStarted)
	{
		musicStarted = true;
		soundSystem->setMusicVolume( prefs.MusicVolume );
		soundSystem->playDigitalMusic(tuneId);
	}

	bLoadShown = bLoad && endAnim.isDone();
	if ( !introMovie && !bLoad && mc2::gate::str<"MC2_BOOT_TO_SCREEN">() == "loadgame" )
		handleMessage( 0, MM_MSG_LOAD );
	if ( endAnim.isDone() )
	{
		status = endResult;
	}

	if ( bDrawMechlopedia )
	{
		mechlopedia->update();
		if ( mechlopedia->getStatus() == NEXT )
		{
			beginFadeIn( 0 );
			bDrawMechlopedia = 0;
			if ( !bDrawBackground )
				status = NEXT;
		}
		return;
	}

	if ( bOptions )
	{
		OptionsScreenWrapper::status_type result = optionsScreenWrapper->update();
		if (result == OptionsScreenWrapper::opt_DONE)
		{
			optionsScreenWrapper->end();
			bOptions = 0;
		}

		return;
	}

	if ( bRulesDlg )
	{
		GameRulesDialog* dlg = GameRulesDialog::instance();
		dlg->update();
		if ( dlg->isDone() )
		{
			bRulesDlg = false;
			if ( rulesForSolo )
			{
				// Solo: the map list stays up; ACCEPT remembers the rules, they are applied when the mission is picked.
				rulesForSolo = false;
				if ( dlg->getStatus() == LogisticsScreen::YES )
				{
					prefs.gameRules = dlg->rules();
					(void)prefs.save();
				}
			}
			else if ( dlg->getStatus() == LogisticsScreen::YES )
			{
				const mc2::rules::GameRules chosen = dlg->rules();
				prefs.gameRules = chosen;	// remembered as the next new campaign's starting point
				(void)prefs.save();
				if ( LogisticsData::instance->startNewCampaign( rulesCampaign.c_str() ) )
				{
					LogisticsData::instance->setRules( chosen );
					status = endResult = RESTART;
				}
				else
				{
					endAnim.end();
					beginAnim.begin();
					showCampaignRefusal( bHostLeftDlg, "This campaign could not be started: its campaign file is missing or damaged." );
				}
			}
			else
			{
				endAnim.end();
				beginAnim.begin();
			}
		}
		return;
	}

	if ( (bSave || bLoad || bLoadCampaign) && endAnim.isDone() )
	{
		LogisticsSaveDialog::instance()->update();

		if ( LogisticsSaveDialog::instance()->getStatus() == LogisticsScreen::YES 
			&& LogisticsSaveDialog::instance()->isDone() )
		{
			
			char name[1024];
			strcpy( name, savePath );
			strcat( name, LogisticsSaveDialog::instance()->getFileName().c_str() );
			int index = strlen( name ) - 4;
			if ( S_stricmp( &name[index], ".fit" ) !=0 ) 
				strcat( name, ".fit" );

			
			FitIniFile file;
			if ( bSave )
			{
				// make sure the save game directory exists, if not create it
				CreateDirectory( savePath, nullptr );

				if ( NO_ERR != file.createWithCase( name ) )
				{
					char errorStr[1024];
					(*std::format_to_n( errorStr, sizeof(errorStr) - 1, "couldn't open the file {}", mc2::fmt::safe(name) ).out = '\0');
					Assert( 0, 0, errorStr );
				}
				else
				{
					LogisticsData::instance->save( file );
					LogisticsSaveDialog::instance()->end();
					file.close();
				}
				bSave = bLoad = 0;
				status = NEXT;
				

			}
			else if ( bLoadCampaign )
			{
				if ( beginCampaignRules( LogisticsSaveDialog::instance()->getFileName() ) )
					;	// the Game Rules dialog takes over; ACCEPT starts the campaign
				else if ( LogisticsData::instance->startNewCampaign( LogisticsSaveDialog::instance()->getFileName().c_str()) )
					status = endResult = RESTART;
				else
				{
					endAnim.end();
					beginAnim.begin();
					showCampaignRefusal( bHostLeftDlg, "This campaign could not be started: its campaign file is missing or damaged." );
				}
//				background.beginFadeOut( 1.0f );
//				beginFadeOut( 1.0f );
				bLoadCampaign = 0;
			}
			else
			{
				const bool do_not_make_lower = true;
				if ( NO_ERR != file.open( name, READ, 50, do_not_make_lower ) )
				{
					char errorStr[1024];
					(*std::format_to_n( errorStr, sizeof(errorStr) - 1, "couldn't open the file {}", mc2::fmt::safe(name) ).out = '\0');
					Assert( 0, 0, errorStr );
					MC2_LOG(Io, Error, "campaign save: could not open the save file {}, staying on the menu", mc2::fmt::safe(name));
					endAnim.end();
					beginAnim.begin();
					LogisticsSaveDialog::instance()->end();
					bSave = bLoad = 0;
				}
				else if ( !LogisticsData::instance->load( file ) )
				{
					// the save could not be loaded (logged by load; a campaign-file failure changes no state): stay on the menu
					endAnim.end();
					beginAnim.begin();
					LogisticsSaveDialog::instance()->end();
					bSave = bLoad = 0;
					file.close();
					showCampaignRefusal( bHostLeftDlg, "This saved game could not be loaded: its campaign file is missing or damaged." );
				}
				else
				{
					LogisticsSaveDialog::instance()->end();
					bSave = bLoad = 0;
					status = RESTART;
					file.close();
				}

			}
		}
		else if ( LogisticsSaveDialog::instance()->getStatus() == LogisticsScreen::NO &&
			LogisticsSaveDialog::instance()->isDone())
		{
			LogisticsSaveDialog::instance()->end();
			bSave = bLoad = bLoadCampaign = 0 ;
			if ( !bDrawBackground )
				status = NEXT;
			else
			{
				beginAnim.begin();
				endAnim.end();
			}
		}
		return;
	}
	else if ( bLoadSingle && endAnim.isDone())
	{
		singleLoadDlg.update();
		if ( singleLoadDlg.takeRulesRequest() && !MPlayer && !mc2::net::active() && GameRulesDialog::init() )
		{
			rulesForSolo = true;
			bRulesDlg = true;
			GameRulesDialog::instance()->begin( prefs.gameRules );
		}
		else if ( singleLoadDlg.isDone() )
		{
			if ( singleLoadDlg.getStatus() == YES )
			{
				const char* pName = singleLoadDlg.getMapFileName();
				if (pName)
				{
					LogisticsData::instance->setSingleMission( pName );
					LogisticsData::instance->setRules( prefs.gameRules );	// solo remembers the last chosen rules
					status = SKIPONENEXT;
				}
			}

			bLoadSingle = 0;
			beginAnim.begin();
			endAnim.end();
		}
	}

	else if ( promptToQuit )
	{
		LogisticsOKDialog::instance()->update();
		{
			if ( LogisticsOKDialog::instance()->getStatus() == LogisticsScreen::YES )
			{
				soundSystem->playDigitalSample( LOG_EXITGAME );
				gos_TerminateApplication();
				promptToQuit = 0;

			}
			else if ( LogisticsOKDialog::instance()->getStatus() == LogisticsScreen::NO)
			{
				if ( LogisticsOKDialog::instance()->isDone() )
					promptToQuit = 0;
			}

			

		}
	}
	else if ( bLegal )
	{
		LogisticsLegalDialog::instance()->update();
		if ( LogisticsLegalDialog::instance()->isDone() )
		{
			LogisticsLegalDialog::instance()->end();
			bLegal = 0;
		}
	}
	else if ( bHostLeftDlg )
	{
		LogisticsOneButtonDialog::instance()->update();
		if ( LogisticsOneButtonDialog::instance()->isDone() )
		{
			LogisticsOneButtonDialog::instance()->end();
			bHostLeftDlg = 0;
		}

		if ( MPlayer ) // has to be done, but can't be done when initialized
		{
			MPlayer->closeSession();
			delete MPlayer;
			MPlayer = nullptr;
		}
	}
	else if ( promptToDisconnect )
	{
		LogisticsOKDialog::instance()->update();
		if ( LogisticsOKDialog::instance()->isDone() )
		{
			if ( YES == LogisticsOKDialog::instance()->getStatus() )
			{
				if ( MPlayer )
				{
					MPlayer->closeSession();
					delete MPlayer;
					MPlayer = nullptr;
				}
				long oldRes = endResult;
				endResult = 0;

				handleMessage( oldRes, oldRes );

				setDrawBackground( true );
			}
			else
				handleMessage( NEXT, NEXT );
			
			promptToDisconnect = 0;
		}
	}
	else
	{
		if ( bDrawBackground  )
		{
			if ( !intro.allAnimObjectsDone() )
			{
				intro.update();
				background.update();
				if (userInput->getKeyDown(KEY_ESCAPE) || (Environment.Renderer == 3))
				{
					introOver = true;
					userInput->mouseOn();
					soundSystem->playDigitalSample( LOG_MAINMENUBUTTON );

				}
				else if ( !introOver )
					return;
			}
			else
			{
				background.update();
				if ( !introOver )
					soundSystem->playDigitalSample( LOG_MAINMENUBUTTON );

				introOver = true;
				userInput->mouseOn();

			}
		}

		beginAnim.update();
		endAnim.update();

		LogisticsScreen::update();
		if ( (!bLoadSingle) && userInput->isLeftClick() && !inside( userInput->getMouseX(), userInput->getMouseY() ) )
		{
			handleMessage( 0, MM_MSG_RETURN_TO_GAME );
		}
	}
}

bool MainMenu::occludesLogisticsScreens() const
{
	// Bink intro plays as a fullscreen GameOS quad; anything routed through
	// the ImGui HUD would composite on top of it.
	if ( introMovie )
		return true;

	// Splash-mode menu (bDrawBackground): render() covers the screen with an
	// opaque fullscreen rect plus the splash background, so the logistics
	// screen behind is never meant to be visible.  NOTE: aAnimation's
	// isAnimating() means "begin() was called and end() was not" -- it stays
	// true for the menu's whole lifetime after the slide-in starts -- so it
	// cannot be used to detect the transition window.  bDrawBackground alone
	// is the correct gate: MissionBegin sets it true for the entire splash
	// and false for the in-logistics ESC menu, where the screen behind the
	// semi-transparent dim is supposed to show.
	return bDrawBackground;
}

void MainMenu::render()
{
    if (ImGui::GetCurrentContext()) ImGui::GetIO().MouseDrawCursor = true;
    if (!introMovie && !bOptions && !bDrawMechlopedia && !coopOpen)
        // Owner 11-15: the community/credits panel appears only once the real menu
        // does, i.e. after the splash logo animation (render() returns early until then).
        PlayerFrontEnd::drawMainMenu(bDrawBackground && !bLegal && !bRulesDlg &&
                                     (intro.allAnimObjectsDone() || introOver || bHostLeftDlg));
    if (!introMovie && bDrawBackground && !bOptions && !bDrawMechlopedia)
        if (coopOpen) drawCoopLobby();
	// UI-ASPECT-ANCHOR-1: front-end frame — draw the UI on the 16:9 canvas
	// (re-asserted every frame; auto-cleared at flushHUDBatch).
	gos_SetUiCanvasActive( true );

	if (introMovie)
	{
		introMovie->render();
		return;
	}
	if ( bDrawMechlopedia && (fadeTime > fadeOutTime || !fadeOutTime) )
	{
		mechlopedia->render();
		return;
	}

	
	if ( bDrawBackground && rendersDefsPageOverlay() )
	{
		float dw = 0.f, dh = 0.f;
		if ( GuiRuntime::GetDisplaySize( dw, dh ) && dw > 0.f )
			GuiRuntime::DrawUiRect( 0.f, 0.f, dw, dh, 0xff000000, true );
	}
	if ( bOptions )
	{
		optionsScreenWrapper->render();
		return;
	}

	//DO NOT play the splash screen animation in software.
	// WOW does it beat up the framerate!
	float xDelta = 0.f;
	float yDelta = 0.f;  
	long color = 0xff000000;

	if (Environment.Renderer != 3)
	{

		if ( beginAnim.isAnimating() && !beginAnim.isDone() )
		{
			xDelta = beginAnim.getXDelta();
			yDelta = beginAnim.getYDelta();

			float time = beginAnim.getCurrentTime();
			float endTime = beginAnim.getMaxTime();
			if ( endTime )
			{
				color = interpolateColor( 0x00000000, 0x7f000000, time/endTime );

			}
		}

		else if (endAnim.isAnimating() /*&& !endAnim.isDone()*/)
		{
			xDelta = endAnim.getXDelta();
			yDelta = endAnim.getYDelta();

			float time = endAnim.getCurrentTime();
			float endTime = endAnim.getMaxTime();
			if ( endTime && (time <= endTime))
			{
				color = interpolateColor( 0x7f000000, 0x00000000, time/endTime );
			}
		}

		GUI_RECT rect = { 0, 0, Environment.screenWidth, Environment.screenHeight };
		drawRect( rect, color );

		if ( bDrawBackground )
		{
			background.render();
			intro.render();
			if ( !intro.allAnimObjectsDone() && !introOver && !bHostLeftDlg )
				return;


		}
	}
	else
	{
		GUI_RECT rect = { 0, 0, Environment.screenWidth, Environment.screenHeight };
		drawRect( rect, color );
	}

	if ( !xDelta && !yDelta && !hasDefsUiPage() )
	{
		// Legacy GameOS copyright line.  When the data/defs UI page is
		// active it renders this text itself through ImGui; drawing it here
		// too produces a second, GameOS-scaled copy across the screen.
		drawShadowText( 0xffc66600, 0xff000000, textObjects[1].font.getTempHandle(),
			textObjects[1].globalX(), textObjects[1].globalTop(),
			textObjects[1].globalRight(), textObjects[1].globalBottom(),
			true, textObjects[1].textString().c_str(), false, textObjects[1].font.getSize(), 1, 1 );
	}

	textObjects[1].showGUIWindow( false );

	
	if ( (!bSave && !bLoad && !bLoadSingle && !bLoadCampaign) || (!endAnim.isDone() && endResult != RESTART ) )	
		LogisticsScreen::render( xDelta, yDelta );
	else if ( bLoadSingle )
	{
		if ( !bRulesDlg )	// the rules dialog replaces the map list while it is up (the defs pages would draw over it)
			singleLoadDlg.render();
	}
	else
		LogisticsSaveDialog::instance()->render();
	if ( !xDelta && !yDelta && !bSave && !bLoad && !bLoadSingle && !bLoadCampaign && !bLegal )
	{
		static bool markedReady = false;
		if ( !markedReady )
		{
			markedReady = true;
			mc2::fmt::print_out("[MENU_READY v1] animation_complete=1 buttons_drawn=1\n");
		}
		mc2::screen_settle::tickFrontEnd(-1, -1);
	}
 
	if (!coopOpen && !bOptions && !bDrawMechlopedia && !bLoad && !bSave && !bLoadSingle && !bLoadCampaign &&
		!xDelta && !yDelta && !promptToQuit && !promptToDisconnect) {
		const auto* button = getButton(MM_MSG_MULTIPLAYER);
		float x = button->globalLeft(), y = button->globalTop(), w = button->width(), h = button->height();
		float width = Environment.screenWidth, height = Environment.screenHeight;
		// Defs rectangles use the actual display, which can differ from the legacy screen mode.
		if (getDefsElementScreenRect("game.mcl_mm.button.multiplayer", x, y, w, h))
			GuiRuntime::GetDisplaySize(width, height);
		coopProofWidget("Multiplayer", x, y, x + w, y + h, width, height);
	}
	if ( promptToQuit || promptToDisconnect )
	{
		LogisticsOKDialog::instance()->render();
	}
	if ( bLegal )
	{
		LogisticsLegalDialog::instance()->render();
	}
	if ( bRulesDlg )
	{
		GameRulesDialog::instance()->render();
	}
	if ( bHostLeftDlg )
	{
		LogisticsOneButtonDialog::instance()->render();
	}



	
}

void MainMenu::setHostLeftDlg( mc2::czstring playerName )
{
	LogisticsOneButtonDialog::instance()->setPlayerLeftText( playerName );

	if ( MPlayer && MPlayer->playerInfo[MPlayer->commanderID].booted )
	{
		LogisticsOneButtonDialog::instance()->setText( IDS_MP_PLAYER_KICKED,
					IDS_DIALOG_OK, IDS_DIALOG_OK );

	}
	LogisticsOneButtonDialog::instance()->begin();
	bHostLeftDlg = true;

}




//*************************************************************************************************
// end of file ( MainMenu.cpp )
