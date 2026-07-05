/*************************************************************************************************\
MissionBegin.cpp			: Implementation of the MissionBegin component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"missionbegin.h"
#include"mclib.h"
#include"objmgr.h"
#include"mech.h"
#include"logisticsvariant.h"
#include"mechicon.h"
#include"logisticsdata.h"
#include"logisticsmech.h"
#include"logisticspilot.h"
#include"missionselectionscreen.h"
#include"mechbayscreen.h"
#include"pilotreadyscreen.h"
#include"mechpurchasescreen.h"
#include"mechlabscreen.h"
#include"missionbriefingscreen.h"
#include"mpconnectiontype.h"
#include"mpparameterscreen.h"
#include"mpgamebrowser.h"
#include"mploadmap.h"
#include"mainmenu.h"
#include"mission.h"
#include"gamesound.h"
#include"loadscreen.h"
#include"mpprefs.h"
#include"chatwindow.h"
#include"logisticsmechicon.h"
#include"../GameOS/gameos/gos_profiler.h"
#include"../GuiRuntime/GuiRuntime.h"   // MENU-STALE-CLEAR-2: ImGui-layer dim rect
#include"../GameOS/gameos/gos_dev_shell.h"   // DEV-SHELL-1: ui_reload command registration
#include <cstdlib>
#include <string>
#include"platform_str.h"   // S_stricmp (MC2_BOOT_TO_SCREEN parse, LINUX_BUILD-safe)

#include"prefs.h"
extern CPrefs prefs;

class MechLabScreen;

extern long renderer;

void initABL (void);
void closeABL (void);

//Tutorial
// Please save these two flags with the saveGames!!
bool	MissionBegin::FirstTimePurchase = true;
bool	MissionBegin::FirstTimeMechLab = true;

// DEV-SHELL-1 ui_reload: the front-end instance the shell command targets.
// Set on construction, cleared on destruction — a plain lifetime mirror.
static MissionBegin* s_devShellMissionBegin = nullptr;

MissionBegin::MissionBegin()
{
	s_devShellMissionBegin = this;
	memset( screens, 0, sizeof( screens));
	memset( singlePlayerScreens, 0, sizeof(singlePlayerScreens));
	memset( multiplayerScreens, 0, sizeof( multiplayerScreens));

	curScreenX = -1;
	curScreenY = 1;

	mainMenu = NULL;

	bSplash = 0;
	bMultiplayer = 0;
	animJustBegun = 0;
	placeHolderScreen = NULL;

}

MissionBegin::~MissionBegin()
{
	if ( s_devShellMissionBegin == this )
		s_devShellMissionBegin = nullptr;
	for ( int i = 0; i < 5; i++ )
	{
		for ( int j = 0; j < 3; j++ )
		{
			if ( singlePlayerScreens[i][j] )
			{
				delete singlePlayerScreens[i][j];
				singlePlayerScreens[i][j] = NULL;
			}
			if ( multiplayerScreens[i][j] )
			{
				delete multiplayerScreens[i][j];
				multiplayerScreens[i][j] = NULL;
			}
		}
	}

	delete LogisticsMechIcon::s_pTemplateIcon;
	LogisticsMechIcon::s_pTemplateIcon = NULL;

	delete placeHolderScreen;
	placeHolderScreen = NULL;

	if (mainMenu)
	{
		delete mainMenu;
		mainMenu = NULL;
	}
}

// DEV-SHELL-1 ui_reload implementation. Re-reads the active screen's loose
// .fit from data/art/ and re-inits the screen object in place. Safe because
// LogisticsScreen::init() calls clear() first (delete[] + recount). Textures
// come back from the txmmgr filename cache — .fit layout edits (positions,
// rects, text, button defs) reload; changed .tga pixels need a restart
// (texture-cache refresh is a separate slice).
bool MissionBegin::reloadCurrentScreenLayout( std::string& infoOut, std::string& errOut )
{
	char path[256];
	FitIniFile file;

	if ( bSplash )
	{
		if ( !mainMenu ) { errOut = "main menu not constructed"; return false; }
		strcpy( path, artPath );
		strcat( path, "mcl_mm.fit" );
		if ( NO_ERR != file.open( path ) ) { errOut = std::string("open failed: ") + path; return false; }
		mainMenu->init( file );
		file.close();
		mainMenu->setDrawBackground( true );
		infoOut = "{\"screen\":\"main_menu\",\"fit\":\"mcl_mm.fit\"}";
		return true;
	}

	if ( curScreenX < 0 || curScreenY < 0 )
	{
		errOut = "no active front-end screen (in mission?)";
		return false;
	}
	if ( bMultiplayer )
	{
		errOut = "multiplayer screens not supported by ui_reload v0";
		return false;
	}
	// skipLogistics reroutes substitute grid cells (e.g. screens[3][1] becomes
	// MissionSelection) — the static_cast per cell below would then be wrong.
	if ( screens[curScreenX][curScreenY] != singlePlayerScreens[curScreenX][curScreenY] )
	{
		errOut = "active cell is substituted (skipLogistics reroute); reload unsupported";
		return false;
	}

	const long id = 10 * curScreenX + curScreenY;
	const char* fitName = nullptr;
	const char* screenName = nullptr;
	switch ( id )
	{
		case 1:  fitName = "mcl_cm_layout.fit";    screenName = "campaign_select";    break;
		case 11: fitName = "mcl_mn.fit";           screenName = "mission_briefing";   break;
		case 21: fitName = "mcl_mb_layout.fit";    screenName = "mech_bay";           break;
		case 31: fitName = "mcl_pr_layout.fit";    screenName = "pilot_ready_launch"; break;
		case 20: fitName = "mcl_m$.fit";           screenName = "mech_purchase";      break;
		case 22: fitName = "mcl_mc.fit";           screenName = "mech_lab_loadout";   break;
		case 41: fitName = "mcl_loadingscreen.fit"; screenName = "load";              break;
		default: errOut = "unknown screen id " + std::to_string( id ); return false;
	}

	strcpy( path, artPath );
	strcat( path, fitName );
	if ( NO_ERR != file.open( path ) ) { errOut = std::string("open failed: ") + path; return false; }

	// init overloads are per-derived-class and NOT virtual — dispatch by grid
	// cell (cell types are fixed at construction, guarded above).
	LogisticsScreen* scr = screens[curScreenX][curScreenY];
	switch ( id )
	{
		case 1:  static_cast<MissionSelectionScreen*>(scr)->init( &file ); break;
		case 11: static_cast<MissionBriefingScreen*>(scr)->init( &file );  break;
		case 21: static_cast<MechBayScreen*>(scr)->init( &file );          break;
		case 31: static_cast<PilotReadyScreen*>(scr)->init( &file );       break;
		case 20: static_cast<MechPurchaseScreen*>(scr)->init( file );      break;
		case 22: static_cast<MechLabScreen*>(scr)->init( file );           break;
		case 41: static_cast<LoadScreenWrapper*>(scr)->init( file );       break;
	}
	file.close();

	// Re-enter the screen so per-visit population (icons, lists) rebuilds
	// against the fresh widget layout.
	scr->begin();

	infoOut = std::string("{\"screen\":\"") + screenName + "\",\"fit\":\"" + fitName + "\"}";
	return true;
}

// gos_dev_shell extension handler (plain fn ptr, main-thread at poll point).
static bool devShellUiReload( const char* /*paramsJson*/,
                              std::string& dataJsonOut, std::string& errorOut )
{
	if ( !s_devShellMissionBegin )
	{
		errorOut = "front-end not active";
		return false;
	}
	return s_devShellMissionBegin->reloadCurrentScreenLayout( dataJsonOut, errorOut );
}

bool MissionBegin::startAnimation (long bId, bool isButton, float scrollTime, long nFlashes)
{
	if (animationRunning)
		return false;
	else
	{
		animationRunning = true;
		timeLeftToScroll = scrollTime;
		targetButtonId = bId;
		targetIsButton = isButton;
		buttonNumFlashes = nFlashes;
		buttonFlashTime = 0.0f;
	}

	return true;
}

// Headless harness: map MC2_BOOT_TO_SCREEN to a logistics screen grid cell.
// Returns true if MC2_BOOT_TO_BAY is active (boot mode), filling x/y with the
// target cell. purchase=[2][0] bay=[2][1] loadout=[2][2] launch=[3][1].
// Unset/unknown MC2_BOOT_TO_SCREEN defaults to bay (preserves prior behavior).
static bool mc2BootScreenXY( int& x, int& y )
{
	const char* bootBay = std::getenv("MC2_BOOT_TO_BAY");
	if ( !bootBay || !bootBay[0] )
		return false;
	x = 2; y = 1; // default: mech bay
	const char* scr = std::getenv("MC2_BOOT_TO_SCREEN");
	if ( scr && scr[0] )
	{
		if      ( !S_stricmp(scr, "purchase") ) { x = 2; y = 0; }
		else if ( !S_stricmp(scr, "bay") )      { x = 2; y = 1; }
		else if ( !S_stricmp(scr, "loadout") )  { x = 2; y = 2; }
		else if ( !S_stricmp(scr, "launch") )   { x = 3; y = 1; }
	}
	return true;
}

// CRASH-SOAK harness (MC2_SOAK_AUTOWIN). When set, the booted campaign drives
// itself from logistics to mission start with no clicks: each settled (RUNNING)
// logistics screen at curScreenX>=2 is forced to advance (NEXT) on a throttle so
// it walks bay -> pilotready -> load -> mission start. Gated on the env flag;
// default OFF = byte-identical. Single getenv cached at startup.
static const bool s_soakAutoWin =
	( std::getenv("MC2_SOAK_AUTOWIN") != nullptr );

// SOAK-SCREEN-CHECK-1 (MC2_SOAK_CHECK_SCREENS). When set (and MC2_SOAK_AUTOWIN
// is also active), before each mission launch the soak walks through
// singlePlayerScreens[2][0] (mech_purchase) and singlePlayerScreens[2][2]
// (mech_lab_loadout), dwells briefly on each to let dumpFrontEndState fire,
// emits [SOAK] screen-check markers, then returns to [2][1] and resumes the
// normal autowin advance. Default OFF = byte-identical to prior behaviour.
static const bool s_soakCheckScreens =
	( std::getenv("MC2_SOAK_CHECK_SCREENS") != nullptr );

// SOAK-AUTO-PURCHASE-1 (MC2_SOAK_AUTO_PURCHASE). Campaign missions require the
// player to buy + deploy a mech in the mech bay before launch; the auto-advance
// soak never does this, so purchase-required missions stall at the bay and never
// launch. When set (with MC2_SOAK_AUTOWIN), on each fresh settle at mech_bay
// [2][1] with an empty force group, the soak grants infinite money, buys one
// drop-weight-legal mech (mid/bottom of the purchase list), deploys it to lance
// slot 1, assigns the first available pilot, and selects it (which also makes the
// loadout/mechlab screen reachable for MC2_SOAK_CHECK_SCREENS). Default OFF =
// byte-identical to prior behaviour.
static const bool s_soakAutoPurchase =
	( std::getenv("MC2_SOAK_AUTO_PURCHASE") != nullptr );

// SOAK-STOP-AT-BAY-1 (MC2_SOAK_STOP_AT_BAY). For validation harnesses that only
// need to auto-win ONE mission to reach the bay and then sit there (e.g. to
// screenshot a logistics-screen fix) without running a full campaign soak.
// When set (with MC2_SOAK_AUTOWIN), suppresses the normal autowin advance
// (soakForceNext) the first time curScreenX==2 && curScreenY==1 (mech bay) is
// reached, so the soak parks there instead of clicking NEXT into
// purchase/loadout/launch/mission2. Default OFF = byte-identical.
static const bool s_soakStopAtBay =
	( std::getenv("MC2_SOAK_STOP_AT_BAY") != nullptr );

// SOAK-LANCE-RANDOM-1 (MC2_SOAK_LANCE_RANDOM). Requires MC2_SOAK_AUTO_PURCHASE.
// When set, REPLACES the single-mech buy with a full random-sized lance purchase:
// picks a random target count N in [1..12] per mission (seeded from a per-bay
// counter via a cheap integer hash), then buys+deploys up to N random mech
// variants (weight >= 20t) from the purchase pool. Stops early on pilot
// exhaustion, addMechToForceGroup rejection, or empty pool. Drop-weight cap is
// intentionally IGNORED (cheat-mode stress). Default OFF = byte-identical.
static const bool s_soakLanceRandom =
	( std::getenv("MC2_SOAK_LANCE_RANDOM") != nullptr );

// State machine for the screen-check detour. IDLE when not active.
enum class SoakCheckState : int
{
	IDLE            = 0,
	ENTER_PURCHASE  = 1,  // about to begin [2][0]
	DWELL_PURCHASE  = 2,  // waiting for dwell timer on [2][0]
	ENTER_LOADOUT   = 3,  // about to begin [2][2]
	DWELL_LOADOUT   = 4,  // waiting for dwell timer on [2][2]
	RETURN_TO_BAY   = 5,  // returning to [2][1] so normal advance fires
};

void MissionBegin::begin()
{
	ZoneScopedN("MissionBegin::begin");
	bReadyToLoad = 0;
	{ ZoneScopedN("MissionBegin::begin initABL");
	initABL();
	}

	//-----------------------------------------------
	// Tutorial Data
	animationRunning = false;
	timeLeftToScroll = 0.0f;
	targetButtonId = 0;
	buttonNumFlashes = 0;
	buttonFlashTime = 0.0f;

	//---------------------------------------------
	//Load up the Logistics Brain for Tutorials.
	// OK if brain file is NOT there!!
	FullPathFileName brainFileName;
	const char * brainfile = LogisticsData::instance->getCurrentABLScript();
	if ( brainfile )
		 brainFileName.init(missionPath, brainfile, ".abl");
	
	if (brainfile && fileExists(brainFileName))
	{
		ZoneScopedN("MissionBegin::begin tutorialBrain");
		long numErrors, numLinesProcessed;
		logisticsScriptHandle = ABLi_preProcess(brainFileName, &numErrors, &numLinesProcessed);
		gosASSERT(logisticsScriptHandle >= 0);
		
		logisticsBrain = new ABLModule;
		gosASSERT(logisticsBrain != NULL);
			
	#ifdef _DEBUG
		long brainErr = 
	#endif
			logisticsBrain->init(logisticsScriptHandle);
		gosASSERT(brainErr == NO_ERR);
		
		logisticsBrain->setName("Logistics");
	}
	else
	{
		logisticsScriptHandle = 0;
		logisticsBrain = NULL;
	}

	// CRASH-SOAK harness marker: report whether a logistics ABL brain was
	// found+loaded for the current mission. Cheap (one printf at screen
	// entry); always emitted so it shows up regardless of soak gate.
	printf("[SOAK] abl logistics brain=%s loaded=%d\n",
		brainfile ? brainfile : "(none)",
		(logisticsBrain != NULL) ? 1 : 0);
	fflush(stdout);

	//---------------------------------------------
	DWORD localRenderer = prefs.renderer;
	if (prefs.renderer != 0 && prefs.renderer != 3)
		localRenderer = 0;

   	bool localFullScreen = prefs.fullScreen;
   	bool localWindow = !prefs.fullScreen;
   	if (Environment.fullScreen && prefs.fullScreen)
   		localFullScreen = false;

	if (prefs.renderer == 3)
		gos_SetScreenMode(800,600,16,0,0,0,true,localFullScreen,0,localWindow,0,localRenderer);
	else if (prefs.bitDepth)
		gos_SetScreenMode(800,600,32,prefs.renderer,0,0,0,localFullScreen,0,localWindow,0,localRenderer);
	else
		gos_SetScreenMode(800,600,16,prefs.renderer,0,0,0,localFullScreen,0,localWindow,0,localRenderer);

	if ( mainMenu ) // already initialized
	{
		ZoneScopedN("MissionBegin::begin reuseExistingScreens");

		// Headless harness: on re-entry (e.g. after setCurrentMissionAnyStage
		// cross-stage jump) the default below stomps to [0][1] mission-select,
		// which then auto-routes to the briefing screen. Under MC2_BOOT_TO_BAY,
		// lock the boot-forced screen ([2][0] purchase, etc.) and hold it.
		{
			int bx, by;
			if ( mc2BootScreenXY( bx, by ) )
			{
				curScreenX = bx;
				curScreenY = by;
				if ( screens[curScreenX][curScreenY] )
				{
					screens[curScreenX][curScreenY]->beginFadeIn( 1.0 );
					screens[curScreenX][curScreenY]->begin();
				}
				Mission::initTGLForLogistics();
				bDone = 0;
				return;
			}
		}

		curScreenX = 0;
		curScreenY = 1;

		// LOGISTICS-PILOTSCREEN-FIX: the RESTART/boot path clobbers a grid cell when
		// mission 1 has PlayLogistics=FALSE + PlaySelection=TRUE (MCO campaigns):
		// screens[3][1] (PilotReadyScreen) is overwritten with MissionSelectionScreen
		// at :644. This reuse-path begin() never reset screens[][] (only the RESTART
		// handler does, at the loop near :630-637), so the clobber persisted into every
		// later mission -> the PilotReady screen stayed replaced by the briefing/launch
		// screen and pilots could never be assigned. Restore the grid to the canonical
		// singlePlayerScreens before routing. Idempotent no-op for stock (PlayLogistics
		// =TRUE / PlaySelection=FALSE -> never clobbered), so byte-identical there.
		for (int ri = 0; ri < 5; ri += 1)
			for (int rj = 0; rj < 3; rj += 1)
				screens[ri][rj] = singlePlayerScreens[ri][rj];

		if ( LogisticsData::instance->skipLogistics() )
		{
					
			if ( LogisticsData::instance->showChooseMission() )
			{
				curScreenX = 3;
				curScreenY = 1;
				screens[3][1] = singlePlayerScreens[0][1];
			}
		}

		if ( screens[curScreenX][curScreenY] )
		{
			screens[curScreenX][curScreenY]->beginFadeIn( 1.0 );
			screens[curScreenX][curScreenY]->begin();
		}

		Mission::initTGLForLogistics();
		bDone = 0;
		return;
	}
	
	MissionSelectionScreen*		pMissionSelectionScreen;
	MechBayScreen*				pMechBayScreen;
	PilotReadyScreen*			pPilotSelectionScreen;
	MechLabScreen*				pMechLabScreen;
	MechPurchaseScreen*			pPurchaseMechScreen;
	MissionBriefingScreen*		pBriefingScreen;
	LoadScreenWrapper*			pLoadScreen;

	pMissionSelectionScreen = NULL;
	pMechBayScreen = NULL;
	pPilotSelectionScreen = NULL;
	pMechLabScreen = NULL;
	pPurchaseMechScreen = NULL;
	pBriefingScreen = NULL;
	pLoadScreen = NULL;

	bDone = 0;

	char path[256];
	FitIniFile file;

	// initialize the main menu
	{ ZoneScopedN("MissionBegin::begin mainMenu");
	mainMenu = new MainMenu;

	strcpy( path, artPath );
	strcat( path, "mcl_mm.fit" );

	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", path );
		Assert( 0, 0, error );
		return;
	}

	mainMenu->init( file );
	mainMenu->setDrawBackground( true );
	mainMenu->begin();
	const char* menuCanarySkipIntro = std::getenv("MC2_MENU_CANARY_SKIP_INTRO");
	if ( menuCanarySkipIntro && menuCanarySkipIntro[0] && menuCanarySkipIntro[0] != '0' )
	{
		mainMenu->skipIntro();
		printf("[MENU_CANARY] skip_intro=1\n");
	}
	file.close();
	}

	
	// initialize mission selection
	{ ZoneScopedN("MissionBegin::begin missionSelection");
	pMissionSelectionScreen = new MissionSelectionScreen();
	strcpy( path, artPath );
	strcat( path, "mcl_cm_layout.fit" );
	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", path );
		Assert( 0, 0, error );
		return;
	}

	
	pMissionSelectionScreen->init( &file );	
	file.close();
	}

	// initialize mission briefing
	{ ZoneScopedN("MissionBegin::begin missionBriefing");
	pBriefingScreen = new MissionBriefingScreen();
	strcpy( path, artPath );
	strcat( path, "mcl_mn.fit" );
	
	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", path );
		Assert( 0, 0, error );
		return;
	}
	pBriefingScreen->init( &file );	
	file.close();
	}

	// initialize mech bay
	{ ZoneScopedN("MissionBegin::begin mechBay");
	strcpy( path, artPath );
	strcat( path, "mcl_mb_layout.fit" );
	
	pMechBayScreen = new MechBayScreen();
	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", path );
		Assert( 0, 0, error );
		return;
	}

	// initialize animations, these are held in the mech bay file
	pMechBayScreen->init( &file );	
	file.seekBlock( "DownAnim" );
	downAnim.init(&file, "");
	file.seekBlock("UpAnim"); 
	upAnim.init( &file, "" );
	file.seekBlock( "NextAnim" );
	leftAnim.init( &file, "" );
	file.seekBlock( "BackAnim" );
	rightAnim.init( &file, "" );
	file.close();
	}


	// initialize pilot ready
	{ ZoneScopedN("MissionBegin::begin pilotReady");
	strcpy( path, artPath );
	strcat( path, "mcl_pr_layout.fit" );
	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", path );
		Assert( 0, 0, error );
		return;	
	}



	// initialize pilot ready
	pPilotSelectionScreen = new PilotReadyScreen;
	pPilotSelectionScreen->init( &file );

	file.close();
	}


	// initalize purchase pilot
	{ ZoneScopedN("MissionBegin::begin mechPurchase");
	pPurchaseMechScreen = new MechPurchaseScreen;

	strcpy( path, artPath );
	strcat( path, "mcl_m$.fit" );

	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", path );
		Assert( 0, 0, error );
		return;		
	}

	pPurchaseMechScreen->init( file );
	file.close();
	}

	// initialize mech lab
	{ ZoneScopedN("MissionBegin::begin mechLab");
	pMechLabScreen = new MechLabScreen;

	strcpy( path, artPath );
	strcat( path, "mcl_mc.fit" );
	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", path );
		Assert( 0, 0, error );
		return;		
	}

	pMechLabScreen->init( file );
	file.close();
	}

	// initialize mech lab
	{ ZoneScopedN("MissionBegin::begin loadScreen");
	pLoadScreen = new LoadScreenWrapper;

	strcpy( path, artPath );
	strcat( path, "mcl_loadingscreen.fit" );
	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", path );
		Assert( 0, 0, error );
		return;		
	}

	pLoadScreen->init( file );
	file.close();
	}

	singlePlayerScreens[0][1] = pMissionSelectionScreen;
	singlePlayerScreens[1][1] = pBriefingScreen;
	singlePlayerScreens[2][1] = pMechBayScreen;
	singlePlayerScreens[3][1] = pPilotSelectionScreen;
	singlePlayerScreens[2][0] = pPurchaseMechScreen;
	singlePlayerScreens[2][2] = pMechLabScreen;
	singlePlayerScreens[4][1] = pLoadScreen;

	for ( int i = 0; i < DIM_SCREEN_X; i++ )
	{
		for ( int j = 0; j < DIM_SCREEN_Y; j++ )
		{
			if ( singlePlayerScreens[i][j] )
			{
				/*if (  singlePlayerScreens[i][j]->getButton(MB_MSG_NEXT) )
					singlePlayerScreens[i][j]->getButton(MB_MSG_NEXT)->setPressFX( -1 );

				if ( singlePlayerScreens[i][j]->getButton(MB_MSG_PREV) )
					singlePlayerScreens[i][j]->getButton(MB_MSG_PREV)->setPressFX( -1 );*/
				if ( singlePlayerScreens[i][j]->getButton(MB_MSG_MAINMENU) )
					singlePlayerScreens[i][j]->getButton(MB_MSG_MAINMENU)->setPressFX( LOG_MAINMENUBUTTON );
			}
			
		}
	}




	for (int i = 0; i < 5/*dim screen X*/; i+=1)
	{
		int j;
		for (j = 0; j < 3/*dim screen Y*/; j += 1)
		{
			screens[i][j] = singlePlayerScreens[i][j];
		}
	}

	pMissionSelectionScreen->begin();

}

void MissionBegin::init()
{
	// DEV-SHELL-1: register the ui_reload shell command (inert when
	// MC2_DEV_SHELL is unset; re-registration on re-init just replaces).
	gos_dev_shell::registerCommand( "ui_reload", devShellUiReload );

	begin();
}

void MissionBegin::end()
{
	logisticsBrain = NULL;
	closeABL();
}

bool inPurchase = false;
bool inMechLab = false;
//Returns screen ID as a function of curScreenX and curScreenY 
long MissionBegin::getCurrentScreenId()
{
	//singlePlayerScreens[0][1] = pMissionSelectionScreen;		ID 1
	//singlePlayerScreens[1][1] = pBriefingScreen;				ID 11
	//singlePlayerScreens[2][1] = pMechBayScreen;				ID 21
	//singlePlayerScreens[3][1] = pPilotSelectionScreen;		ID 31
	//singlePlayerScreens[2][0] = pPurchaseMechScreen;			ID 20
	//singlePlayerScreens[2][2] = pMechLabScreen;				ID 22
	//singlePlayerScreens[4][1] = pLoadScreen;					ID 41
	long screenId = 10 * curScreenX + curScreenY;

	if ((screenId == 20) && FirstTimePurchase && !MPlayer )
		inPurchase = true;
	else if ((screenId == 22) && FirstTimeMechLab && !MPlayer )
		inMechLab = true;

	if ((screenId != 20) && inPurchase)
		FirstTimePurchase = false;
	else if ((screenId != 22) && inMechLab)
		FirstTimeMechLab = false;

	if ((screenId == 20) && !FirstTimePurchase)
		screenId = 0;
	else if ((screenId == 22) && !FirstTimeMechLab)
		screenId = 0;

	return (screenId);
}

const char* MissionBegin::update()
{
	// LOGISTICS capture: on each front-end screen change, dump the screen + bay/
	// purchase/loadout/launch state to an MCP-readable LOGISTICS diagnostic event.
	// Self-gated on the LOGISTICS diag tag (no cost unless enabled). curScreenX==-1
	// during the splash/main menu. One hook covers all screens via curScreenX/Y.
	if ( !bSplash && curScreenX >= 0 && LogisticsData::instance )
	{
		static int s_lastDumpX = -2, s_lastDumpY = -2;
		if ( curScreenX != s_lastDumpX || curScreenY != s_lastDumpY )
		{
			s_lastDumpX = curScreenX;
			s_lastDumpY = curScreenY;
			const char* nm = "unknown";
			switch ( 10 * curScreenX + curScreenY )
			{
				case 1:  nm = "campaign_select";    break; // [0][1] MissionSelectionScreen
				case 11: nm = "mission_briefing";   break; // [1][1] BriefingScreen
				case 21: nm = "mech_bay";           break; // [2][1] MechBayScreen
				case 20: nm = "mech_purchase";      break; // [2][0] PurchaseMechScreen
				case 22: nm = "mech_lab_loadout";   break; // [2][2] MechLabScreen
				case 31: nm = "pilot_ready_launch"; break; // [3][1] PilotSelectionScreen
				case 41: nm = "load";               break; // [4][1] LoadScreen
				default: break;
			}
			LogisticsData::instance->dumpFrontEndState( nm );
		}
	}

	if ( bSplash )
	{
		mainMenu->update();
		if ( LogisticsScreen::RUNNING != mainMenu->getStatus() )
		{
			bSplash = 0;
			if ( LogisticsScreen::RESTART == mainMenu->getStatus() )
			{
				LogisticsScreen*		pCurScreen = screens[curScreenX][curScreenY];
				if ( pCurScreen )
					pCurScreen->end();
				int i;
				for (i = 0; i < 5/*dim screen X*/; i+=1)
				{
					int j;
					for (j = 0; j < 3/*dim screen Y*/; j += 1)
					{
						screens[i][j] = singlePlayerScreens[i][j];
					}
				}
				if ( LogisticsData::instance->skipLogistics() )
				{
					if ( LogisticsData::instance->showChooseMission() )
					{
						curScreenX = 3;
						curScreenY = 1;
						screens[3][1] = singlePlayerScreens[0][1];
						screens[curScreenX][curScreenY]->begin();
					}
					else
					{
						curScreenX = 4;
						curScreenY = 1;
						screens[curScreenX][curScreenY]->begin();
					}
				}
				else
				{
					// MC2_BOOT_TO_BAY headless capture: jump straight to the chosen
					// logistics screen (campaign + first mission already set by
					// startNewCampaign) instead of the campaign-select screen, so the
					// LOGISTICS capture dumps it. MC2_BOOT_TO_SCREEN picks the cell
					// (purchase=[2][0] bay=[2][1] loadout=[2][2] launch=[3][1]).
					int bootX, bootY;
					if ( mc2BootScreenXY( bootX, bootY ) )
					{
						curScreenX = bootX;
						curScreenY = bootY;
					}
					else
					{
						curScreenX = 0;
						curScreenY = 1;
					}
					screens[curScreenX][curScreenY]->beginFadeIn( 1.0 );
					screens[curScreenX][curScreenY]->begin();
				}
				return LogisticsData::instance->getCurrentBigVideo();

			}
			else if ( LogisticsScreen::MULTIPLAYERRESTART == mainMenu->getStatus() )
			{
				LogisticsScreen*		pCurScreen = screens[curScreenX][curScreenY];
				if ( pCurScreen )
					pCurScreen->end();

				beginMPlayer();
				int i;
				for (i = 0; i < 5/*dim screen X*/; i+=1)
				{
					int j;
					for (j = 0; j < 3/*dim screen Y*/; j += 1)
					{
						screens[i][j] = multiplayerScreens[i][j];
					}
				}
				curScreenX = 0;
				curScreenY = 1;
				screens[curScreenX][curScreenY]->beginFadeIn( 1.0 );
			}
			else if ( LogisticsScreen::SKIPONENEXT == mainMenu->getStatus() )
			{
				LogisticsScreen*		pCurScreen = screens[curScreenX][curScreenY];
				if ( pCurScreen )
					pCurScreen->end();

				int i;
				for (i = 0; i < 5/*dim screen X*/; i+=1)
				{
					int j;
					for (j = 0; j < 3/*dim screen Y*/; j += 1)
					{
						screens[i][j] = singlePlayerScreens[i][j];
					}
				}
				curScreenX = 1;
				curScreenY = 1;
				screens[curScreenX][curScreenY]->beginFadeIn( 1.0 );
				screens[curScreenX][curScreenY]->begin();
			
			}
			if ( screens[curScreenX][curScreenY] && curScreenX != -1  )
			{
				screens[curScreenX][curScreenY]->begin();
			}
			else // no screen? stay on main menu
			{
				bSplash = true;
				mainMenu->begin();
			}
				
		}
		return NULL;
	}

	leftAnim.update();
	rightAnim.update();
	downAnim.update();
	upAnim.update();

	
	LogisticsScreen*		pCurScreen = screens[curScreenX][curScreenY];
	if ( pCurScreen )
	{
		if (logisticsBrain && !MPlayer)
			logisticsBrain->execute();

		//--------------------
		//For Tutorial
		if (animationRunning)
		{
			//Move mouse to correct position.
			if (targetIsButton)
			{
				aButton *targetButton = pCurScreen->getButton(targetButtonId);
				if (!targetButton)
				{
					animationRunning = false;
				}
				else
				{
					userInput->setMouseCursor(mState_TUTORIALS);

					//Get button position.
					float buttonPosX = (targetButton->left() + targetButton->right()) * 0.5f;

					float buttonPosY = (targetButton->top() + targetButton->bottom()) * 0.5f;

					//-------------------
					// Mouse Checks Next
					float realMouseX = userInput->realMouseX();
					float realMouseY = userInput->realMouseY();

					if (timeLeftToScroll > 0.0f)
					{
						float xDistLeft = buttonPosX - realMouseX;
						float yDistLeft = buttonPosY - realMouseY;

						float xDistThisFrame = xDistLeft / timeLeftToScroll * frameLength;
						float yDistThisFrame = yDistLeft / timeLeftToScroll * frameLength;

						userInput->setMousePos(realMouseX + xDistThisFrame, realMouseY+yDistThisFrame);

						timeLeftToScroll -= frameLength;
					}
					else
					{
						userInput->setMousePos(buttonPosX,buttonPosY);

						//We are there.  Start flashing.
						if (buttonNumFlashes)
						{
							buttonFlashTime += frameLength;
							if ( buttonFlashTime > .5f )
							{
								pCurScreen->getButton( targetButtonId )->setColor( 0xffffffff );
								buttonFlashTime = 0.0f;
								buttonNumFlashes--;
							}
							else if ( buttonFlashTime > .25f )
							{
								pCurScreen->getButton( targetButtonId )->setColor( 0xff7f7f7f );
							}
						}
						else
						{
							//Flashing is done.  We now return you to your regularly scheduled program.
							animationRunning = false;
							pCurScreen->getButton( targetButtonId )->setColor( 0xffffffff );
						}
					}
				}
			}
			else
			{
				aRect *targetButton = pCurScreen->getRect(targetButtonId);
				if (!targetButton)
				{
					animationRunning = false;
				}
				else
				{
					userInput->setMouseCursor(mState_TUTORIALS);
		
					//Get button position.
					float buttonPosX = (targetButton->left() + targetButton->right()) * 0.5f;
		
					float buttonPosY = (targetButton->top() + targetButton->bottom()) * 0.5f;
		
					//-------------------
					// Mouse Checks Next
					float realMouseX = userInput->realMouseX();
					float realMouseY = userInput->realMouseY();
		
					if (timeLeftToScroll > 0.0f)
					{
						float xDistLeft = buttonPosX - realMouseX;
						float yDistLeft = buttonPosY - realMouseY;
		
						float xDistThisFrame = xDistLeft / timeLeftToScroll * frameLength;
						float yDistThisFrame = yDistLeft / timeLeftToScroll * frameLength;
		
						userInput->setMousePos(realMouseX + xDistThisFrame, realMouseY+yDistThisFrame);
		
						timeLeftToScroll -= frameLength;
					}
					else
					{
						userInput->setMousePos(buttonPosX,buttonPosY);
		
						//We are there.  Start flashing.
						if (buttonNumFlashes)
						{
							buttonFlashTime += frameLength;
							if ( buttonFlashTime > .5f )
							{
								pCurScreen->getRect( targetButtonId )->setColor( 0xff000000 );
								buttonFlashTime = 0.0f;
								buttonNumFlashes--;
							}
							else if ( buttonFlashTime > .25f )
							{
								pCurScreen->getRect( targetButtonId )->setColor( 0xffffffff );
							}
						}
						else
						{
							//Flashing is done.  We now return you to your regularly scheduled program.
							animationRunning = false;
							pCurScreen->getRect( targetButtonId )->setColor( 0xff000000 );
						}
					}
				}
			}
		}

		pCurScreen->update();

		// CRASH-SOAK: drive the settled logistics screen toward mission start by
		// synthesizing a NEXT (same lever the launch button pulls). Only when the
		// screen has settled to RUNNING (real transition pending takes priority),
		// on a logistics screen (curScreenX>=2, past splash/select), throttled and
		// fired once per screen-settle so we don't spam. Single-player only.
		// Walk forward from ANY settled grid column toward launch: the
		// inter-mission flow re-enters at mission-selection [0][1] / briefing
		// [1][1], so cover curScreenX 0..3 (was 2..3 for the pre-mission-only
		// walk). Briefing video is already gated by MC2_BOOT_TO_BAY
		// (controlgui playMovie), so widening here adds no video block.
		bool soakForceNext = false;
		if ( s_soakAutoWin && !MPlayer && curScreenX < 4 &&
			 pCurScreen->getStatus() == LogisticsScreen::RUNNING && !animJustBegun )
		{
			// SOAK-AUTO-PURCHASE-1: buy + deploy a mech so purchase-required campaign
			// missions can launch under auto-advance. Runs once per fresh bay visit at
			// [2][1]; the latch resets whenever we leave the bay (next mission re-buys).
			static bool  s_autoPurchasedThisBay = false;
			static float s_purchaseSettle       = 0.0f;
			if ( s_soakAutoPurchase && curScreenX == 2 && curScreenY == 1 )
			{
				s_purchaseSettle += frameLength;
				if ( !s_autoPurchasedThisBay && s_purchaseSettle >= 0.3f &&
				     LogisticsData::instance )
				{
					LogisticsData* ld = LogisticsData::instance;
					EList<LogisticsMech*, LogisticsMech*> fg;
					ld->getForceGroup( fg );
					if ( fg.Count() > 0 )
					{
						// Already have a lance (e.g. campaigns that pre-stage mechs).
						s_autoPurchasedThisBay = true;
					}
					else
					{
						ld->addCBills( 100000000 );          // effectively infinite money
						const int CAP = 512;
						LogisticsVariant* pool[CAP];
						int count = CAP;
						ld->getPurchasableMechs( pool, count );
						int n = ( count < CAP ) ? count : CAP;
						int maxDrop = ld->getMaxDropWeight();

						// Build a compact mech-only index into pool (weight >= 20t).
						// Both code paths (single-buy and lance-random) use this filter.
						const int MECH_MIN_TONS = 20;
						int mechIdx[CAP];
						int mechCount = 0;
						for ( int i = 0; i < n; ++i )
						{
							if ( pool[i] && (int)pool[i]->getMaxWeight() >= MECH_MIN_TONS )
								mechIdx[mechCount++] = i;
						}

						if ( s_soakLanceRandom )
						{
							// SOAK-LANCE-RANDOM-2: deploy MAX-SIZE lance with MAX MECH VARIETY.
							// Goal: stress-test as many distinct mech loadouts as possible each
							// mission.  Lance size = min(12, pilots, distinctCandidates).
							// Sources (in priority order, deduplicated by variant pointer):
							//   (a) Salvaged inventory mechs already owned (getForceGroup()==0),
							//       accumulated by MC2_SOAK_KILL_ENEMY. No weight filter needed.
							//   (b) Purchasable mechs (weight >= 20t) covering additional variants.
							// Combined list is Fisher-Yates shuffled by per-mission LCG so each
							// mission gets a different ordering (deterministic, no srand).

							static unsigned int s_soakLanceMissionNum = 0;
							++s_soakLanceMissionNum;
							// Thomas Wang 32-bit mix -> per-mission shuffle seed
							unsigned int seed = s_soakLanceMissionNum;
							seed = (seed ^ 61u) ^ (seed >> 16);
							seed = seed + (seed << 3);
							seed ^= seed >> 4;
							seed = seed * 0x27d4eb2du;
							seed ^= seed >> 15;

							const int LANCE_MAX = 12;

							// Candidate: salvage (invMech!=null, no buy) or purchasable (invMech==null).
							struct Candidate {
								LogisticsMech*    invMech;
								LogisticsVariant* variant;
							};
							const int CAND_CAP = 256;
							Candidate cands[CAND_CAP];
							int       nCands = 0;

							// Variant dedup table (pointer equality).
							LogisticsVariant* seenVariants[CAND_CAP];
							int               nSeen = 0;
							auto variantSeen = [&]( LogisticsVariant* v ) -> bool {
								for ( int i = 0; i < nSeen; ++i )
									if ( seenVariants[i] == v ) return true;
								return false;
							};
							auto markVariant = [&]( LogisticsVariant* v ) {
								if ( nSeen < CAND_CAP ) seenVariants[nSeen++] = v;
							};

							// -- (a) salvaged inventory mechs (unassigned) ----------------------
							{
								EList<LogisticsMech*, LogisticsMech*> inv;
								ld->getInventory( inv );
								for ( EList<LogisticsMech*, LogisticsMech*>::EIterator it = inv.Begin();
								      !it.IsDone() && nCands < CAND_CAP; it++ )
								{
									LogisticsMech* m = (*it);
									if ( !m ) continue;
									if ( m->getForceGroup() ) continue;
									LogisticsVariant* v = m->getVariant();
									if ( !v ) continue;
									if ( variantSeen(v) ) continue;
									cands[nCands++] = { m, v };
									markVariant(v);
								}
							}
							int nSalvageCands = nCands;

							// -- (b) purchasable mechs (weight>=20t) not covered by salvage -----
							for ( int i = 0; i < mechCount && nCands < CAND_CAP; ++i )
							{
								LogisticsVariant* v = pool[ mechIdx[i] ];
								if ( !v ) continue;
								if ( variantSeen(v) ) continue;
								cands[nCands++] = { nullptr, v };
								markVariant(v);
							}

							// -- Fisher-Yates shuffle (Knuth LCG) -------------------------------
							for ( int i = nCands - 1; i > 0; --i )
							{
								seed = seed * 1664525u + 1013904223u;
								int j = (int)( seed % (unsigned int)( i + 1 ) );
								Candidate tmp = cands[i]; cands[i] = cands[j]; cands[j] = tmp;
							}

							// -- deploy down shuffled list --------------------------------------
							int deployed    = 0;
							int fromSalvage = 0;
							int fromBuy     = 0;
							const char* firstName = "none";

							for ( int slot = 1; slot <= LANCE_MAX && (slot - 1) < nCands; ++slot )
							{
								Candidate&        cand    = cands[slot - 1];
								LogisticsMech*    mech    = cand.invMech;
								LogisticsVariant* variant = cand.variant;
								const char*       src     = mech ? "salvage" : "buy";

								if ( !mech )
								{
									// (b) buy path: purchase then locate fresh instance
									if ( ld->purchaseMech( variant ) != 0 )
									{
										printf("[SOAK] lance-add slot=%d FAILED purchase variant=%s\n",
										    slot, (const char*)variant->getName());
										continue;
									}
									EList<LogisticsMech*, LogisticsMech*> inv;
									ld->getInventory( inv );
									for ( EList<LogisticsMech*, LogisticsMech*>::EIterator it = inv.Begin();
									      !it.IsDone(); it++ )
									{
										LogisticsMech* m = (*it);
										if ( m && m->getVariant() == variant && !m->getForceGroup() )
										{ mech = m; break; }
									}
									if ( !mech )
									{
										printf("[SOAK] lance-add slot=%d WARN bought-not-found variant=%s\n",
										    slot, (const char*)variant->getName());
										continue;
									}
								}

								// Pilot check BEFORE force-group assignment
								LogisticsPilot* pilot = ld->getFirstAvailablePilot();
								if ( !pilot )
								{
									printf("[SOAK] lance-add slot=%d STOPPED no-pilot mech=%s\n",
									    slot, (const char*)variant->getName());
									break;
								}

								if ( ld->addMechToForceGroup( mech, slot ) != 0 )
								{
									printf("[SOAK] lance-add slot=%d REJECTED addMechToForceGroup mech=%s\n",
									    slot, (const char*)variant->getName());
									continue;
								}
								mech->setPilot( pilot );

								if ( deployed == 0 )
								{
									ld->setMechToModify( mech );
									firstName = (const char*)variant->getName();
								}

								printf("[SOAK] lance-add slot=%d mech=%s tons=%d src=%s\n",
								    slot, (const char*)variant->getName(),
								    (int)variant->getMaxWeight(), src);
								++deployed;
								if ( src[0] == 's' ) ++fromSalvage; else ++fromBuy;
							}

							// Count remaining available pilots for summary
							int pilotsAvail = 0;
							{
								LogisticsPilot* pp[256];
								long pc = 256;
								ld->getPilots( pp, pc );
								for ( long pi = 0; pi < pc; ++pi )
									if ( pp[pi] && pp[pi]->isAvailable() )
										++pilotsAvail;
							}

							printf("[SOAK] lance-v2 deployed=%d distinct=%d pilots=%d fromSalvage=%d fromBuy=%d salvageCands=%d buyCands=%d missionNum=%u\n",
							    deployed, deployed, pilotsAvail,
							    fromSalvage, fromBuy,
							    nSalvageCands, nCands - nSalvageCands,
							    s_soakLanceMissionNum);
							fflush( stdout );
						}
						else
						{
							// Original single-mech path (MC2_SOAK_LANCE_RANDOM not set).
							// Prefer HEAVIEST mech within drop-weight cap; fall back to lightest.
							int cap = ( maxDrop > 0 ) ? maxDrop : 0x7fffffff;
							LogisticsVariant* pick      = nullptr;
							int               pickW     = -1;
							LogisticsVariant* lightest  = nullptr;
							int               lightestW = 0x7fffffff;
							for ( int i = 0; i < mechCount; ++i )
							{
								LogisticsVariant* v = pool[ mechIdx[i] ];
								int w = (int)v->getMaxWeight();
								if ( w < lightestW ) { lightestW = w; lightest = v; }
								if ( w <= cap && w > pickW ) { pickW = w; pick = v; }
							}
							if ( !pick )
								pick = lightest;
							if ( !pick && n > 0 )
								pick = pool[n - 1];

							if ( pick && ld->purchaseMech( pick ) == 0 )
							{
								LogisticsMech* bought = nullptr;
								EList<LogisticsMech*, LogisticsMech*> inv;
								ld->getInventory( inv );
								for ( EList<LogisticsMech*, LogisticsMech*>::EIterator it = inv.Begin();
								      !it.IsDone(); it++ )
									if ( (*it) && (*it)->getVariant() == pick && !(*it)->getForceGroup() )
										bought = (*it);
								if ( bought )
								{
									ld->addMechToForceGroup( bought, 1 );
									LogisticsPilot* pilot = ld->getFirstAvailablePilot();
									if ( pilot )
										bought->setPilot( pilot );
									ld->setMechToModify( bought );
									printf("[SOAK] auto-purchase mech=%s tons=%d pilot=%d deployed=slot1 maxDrop=%d pool=%d\n",
										(const char*)pick->getName(), (int)pick->getMaxWeight(),
										pilot ? 1 : 0, maxDrop, n);
								}
								else
								{
									printf("[SOAK] auto-purchase WARN bought-not-found variant=%s\n",
										(const char*)pick->getName());
								}
							}
							else
							{
								printf("[SOAK] auto-purchase FAILED reason=%s pool=%d maxDrop=%d\n",
									pick ? "purchase-rejected" : "empty-pool", n, maxDrop);
							}
							fflush( stdout );
						}
						s_autoPurchasedThisBay = true;
					}
				}
			}
			else if ( s_soakAutoPurchase )
			{
				// Left the bay (or feature off): reset so the next mission re-purchases.
				s_autoPurchasedThisBay = false;
				s_purchaseSettle       = 0.0f;
			}

			static long  s_soakLastAdvX = -99;
			static long  s_soakLastAdvY = -99;
			static float s_soakSettleTimer = 0.0f;

			// SOAK-SCREEN-CHECK-1: state machine that detours through purchase [2][0]
			// and loadout [2][2] before the normal autowin advance from mech_bay [2][1].
			// Statics here so they persist across frames (dwell states run every frame).
			static SoakCheckState s_checkState = SoakCheckState::IDLE;
			static float          s_checkDwell = 0.0f;
			static bool           s_checkDone  = false;
			// Mission serial: incremented each time we depart [2][1] toward a launch.
			// s_checkDone is valid only for the current value of this counter.
			static int            s_checkMissionSerial = 0;
			static int            s_checkDoneSerial    = -1;

			// s_checkDone is true iff we already ran the walk for this mission arrival.
			// Keyed by serial so it auto-resets across missions without touching
			// s_soakLastAdvX (which would re-trigger the mission-boundary detector).
			s_checkDone = ( s_checkDoneSerial == s_checkMissionSerial );

			// Detect mission-boundary: a fresh arrival at [2][1] (last-adv was not
			// [2][1]) while the machine is idle AND we haven't done a walk yet for
			// this mission.  The !s_checkDone guard prevents re-triggering when the
			// walk completion sets lastAdv=-99 to unblock the normal advance path.
			if ( s_soakCheckScreens &&
			     curScreenX == 2 && curScreenY == 1 &&
			     !( s_soakLastAdvX == 2 && s_soakLastAdvY == 1 ) &&
			     s_checkState == SoakCheckState::IDLE &&
			     !s_checkDone )
			{
				++s_checkMissionSerial;
				s_checkDone  = false;   // new serial != doneSerial
				s_checkDwell = 0.0f;
			}

			// Run the screen-check state machine every frame while it is active.
			// When the machine is active we suppress the normal settle-timer advance.
			bool checkMachineActive = ( s_soakCheckScreens && !s_checkDone &&
			                            s_checkState != SoakCheckState::IDLE );
			// True while we are at [2][1] waiting for the kick-off settle timer:
			// this suppresses the normal advance path (else branch) so we don't
			// double-accumulate s_soakSettleTimer.
			bool checkKickoffPending = false;

			if ( s_soakCheckScreens && !s_checkDone &&
			     s_checkState == SoakCheckState::IDLE &&
			     curScreenX == 2 && curScreenY == 1 )
			{
				checkKickoffPending = true;
				// Kick off once the settle timer fires for the first time at [2][1].
				// We only start the machine when the settle period has elapsed so
				// the bay screen has had a chance to fully initialise.
				if ( curScreenX != s_soakLastAdvX || curScreenY != s_soakLastAdvY )
					s_soakSettleTimer += frameLength;
				else
					s_soakSettleTimer = 0.0f;

				if ( s_soakSettleTimer >= 0.5f )
				{
					s_soakSettleTimer  = 0.0f;
					s_checkState       = SoakCheckState::ENTER_PURCHASE;
					checkMachineActive = true;
					checkKickoffPending = false;
				}
			}

			if ( checkMachineActive )
			{
				// Keep the settle-tracker pointed at the current cell so the normal
				// advance path is suppressed while the machine runs.
				s_soakLastAdvX = curScreenX;
				s_soakLastAdvY = curScreenY;

				switch ( s_checkState )
				{
				case SoakCheckState::ENTER_PURCHASE:
				{
					// Purchase screen [2][0] does NOT need getMechToModify — it lists
					// buyable inventory regardless of force-group selection.
					// Transition [2][1] -> [2][0].
					pCurScreen->end();
					curScreenY = 0;
					if ( screens[curScreenX][curScreenY] )
						screens[curScreenX][curScreenY]->begin();
					// dumpFrontEndState fires next frame when the top-of-update
					// curScreenX/Y change detector sees the new cell.
					s_checkDwell = 0.0f;
					s_checkState = SoakCheckState::DWELL_PURCHASE;
					break;
				}
				case SoakCheckState::DWELL_PURCHASE:
				{
					s_checkDwell += frameLength;
					if ( s_checkDwell >= 0.5f )
					{
						// Emit purchase screen-check marker.
						// items = LogisticsData inventory count (mechs available to buy).
						// ok=1 if any exist; ok=0 if inventory empty.
						// Limitation: reflects LogisticsData list, NOT rendered widget asset
						// validity (no per-icon load signal available cheaply here).
						int items = 0;
						if ( LogisticsData::instance )
						{
							EList<LogisticsMech*, LogisticsMech*> inv;
							LogisticsData::instance->getInventory( inv );
							items = (int)inv.Count();
						}
						printf("[SOAK] screen-check screen=purchase ok=%d items=%d\n",
							(items > 0) ? 1 : 0, items);
						fflush(stdout);
						// Return to [2][1] briefly before going to loadout.
						if ( screens[curScreenX][curScreenY] )
							screens[curScreenX][curScreenY]->end();
						curScreenY = 1;
						if ( screens[curScreenX][curScreenY] )
							screens[curScreenX][curScreenY]->begin();
						s_checkDwell = 0.0f;
						s_checkState = SoakCheckState::ENTER_LOADOUT;
					}
					break;
				}
				case SoakCheckState::ENTER_LOADOUT:
				{
					// MechLabScreen::begin() dereferences getMechToModify() at
					// mechlabscreen.cpp:237 — crash if null.  Guard here so we never call
					// begin() when no mech is selected (e.g. PoaR / campaign missions where
					// logistics force-group is not staged before the screen walk).
					bool mechReady = ( LogisticsData::instance &&
					                   LogisticsData::instance->getMechToModify() != nullptr );
					if ( !mechReady )
					{
						printf("[SOAK] screen-check screen=loadout ok=0 reason=no-mech-selected\n");
						fflush(stdout);
						// Return to [2][1] and mark done; normal advance takes over.
						if ( screens[curScreenX][curScreenY] )
							screens[curScreenX][curScreenY]->end();
						curScreenY = 1;
						if ( screens[curScreenX][curScreenY] )
							screens[curScreenX][curScreenY]->begin();
						s_checkDoneSerial = s_checkMissionSerial;  // latch: done for THIS mission
						s_checkDone  = true;
						s_checkState = SoakCheckState::IDLE;
						// Reset lastAdv so the normal advance path sees [2][1] as a new
						// cell and fires.  The boundary detector is guarded by !s_checkDone
						// so it will NOT re-trigger the walk even with lastAdv=-99.
						s_soakLastAdvX = -99;
						s_soakLastAdvY = -99;
						s_soakSettleTimer = 0.0f;
						break;
					}
					// Transition [2][1] -> [2][2] (loadout/mechlab).
					if ( screens[curScreenX][curScreenY] )
						screens[curScreenX][curScreenY]->end();
					curScreenY = 2;
					if ( screens[curScreenX][curScreenY] )
						screens[curScreenX][curScreenY]->begin();
					s_checkDwell = 0.0f;
					s_checkState = SoakCheckState::DWELL_LOADOUT;
					break;
				}
				case SoakCheckState::DWELL_LOADOUT:
				{
					s_checkDwell += frameLength;
					if ( s_checkDwell >= 0.5f )
					{
						// Emit loadout screen-check marker.
						// items = force-group mech count (mechs assigned to this lance).
						// ok=1 if at least one mech is in the force group.
						// Same limitation as purchase: list count, not rendered icon validity.
						int items = 0;
						if ( LogisticsData::instance )
						{
							EList<LogisticsMech*, LogisticsMech*> fg;
							LogisticsData::instance->getForceGroup( fg );
							items = (int)fg.Count();
						}
						printf("[SOAK] screen-check screen=loadout ok=%d items=%d\n",
							(items > 0) ? 1 : 0, items);
						fflush(stdout);
						// Return to [2][1] and mark done so normal advance fires.
						if ( screens[curScreenX][curScreenY] )
							screens[curScreenX][curScreenY]->end();
						curScreenY = 1;
						if ( screens[curScreenX][curScreenY] )
							screens[curScreenX][curScreenY]->begin();
						s_checkDwell = 0.0f;
						s_checkDoneSerial = s_checkMissionSerial;  // latch: done for THIS mission
						s_checkDone  = true;
						s_checkState = SoakCheckState::IDLE;
						// Reset lastAdv so the normal advance path sees [2][1] as a fresh
						// cell and fires soakForceNext (else it idles on "same cell" and the
						// soak HANGS at the bay). The mission-boundary detector is guarded by
						// !s_checkDone (now latched true), so this will NOT restart the walk.
						s_soakLastAdvX = -99;
						s_soakLastAdvY = -99;
						s_soakSettleTimer = 0.0f;
					}
					break;
				}
				default:
					break;
				}
				// Machine is consuming this frame — do not fall through to normal advance.
			}
			else if ( !checkMachineActive && !checkKickoffPending )
			{
				// Normal autowin advance path (no check-screens, or check already done,
				// or not at [2][1]).
				if ( curScreenX != s_soakLastAdvX || curScreenY != s_soakLastAdvY )
				{
					s_soakSettleTimer += frameLength;
					// SOAK-STOP-AT-BAY-1: park here instead of advancing. Still latch
					// s_soakLastAdvX/Y so this doesn't re-fire/spam every frame, and
					// skip the soakForceNext assignment (leave it false).
					if ( s_soakStopAtBay && curScreenX == 2 && curScreenY == 1 )
					{
						s_soakLastAdvX = curScreenX;
						s_soakLastAdvY = curScreenY;
						s_soakSettleTimer = 0.0f;
						printf("[SOAK] parked screen=bay (MC2_SOAK_STOP_AT_BAY)\n");
						fflush(stdout);
					}
					else
					if ( s_soakSettleTimer >= 0.5f )
					{
						soakForceNext = true;
						s_soakLastAdvX = curScreenX;
						s_soakLastAdvY = curScreenY;
						s_soakSettleTimer = 0.0f;
						printf("[SOAK] advance screen=%d,%d\n", (int)curScreenX, (int)curScreenY);
						if ( LogisticsData::instance )
							printf("[SOAK] launch mission=%s stage=%d\n",
								(const char*)LogisticsData::instance->getCurrentMission(),
								(int)LogisticsData::instance->getCurrentMissionNum());
						fflush(stdout);
					}
				}
				else
				{
					s_soakSettleTimer = 0.0f; // same cell already advanced; idle
				}
			}
		}

		if ( pCurScreen->getStatus() == LogisticsScreen::GOTOSPLASH || ( MPlayer && MPlayer->hostLeft ) )
		{
			pCurScreen->end();
			beginSplash();
			if ( MPlayer &&  MPlayer->hostLeft )
			{
				MPlayer->closeSession();
				delete MPlayer;
				MPlayer = NULL;
			}
				
			return NULL;
		}

		if ( pCurScreen->getStatus() == LogisticsScreen::READYTOLOAD && 
			(curScreenX == 4 ||( MPlayer && curScreenX == 3 )) )
			bReadyToLoad = true;


		if ( pCurScreen->getStatus() != LogisticsScreen::RUNNING || soakForceNext )
		{
			soundSystem->stopBettySample(); // don't want to carry droning on to next screen
			soundSystem->stopSupportSample();
			if ( pCurScreen->getStatus() == LogisticsScreen::NEXT || soakForceNext )
			{
				pCurScreen->end();
				if ( curScreenX < 4 )
				{
					if ( MPlayer )// different rules for multiplayer
					{
						if ( dynamic_cast<MPParameterScreen*>( screens[curScreenX][curScreenY] ) )
						{
							if ( curScreenX == 2 )
							{
								
								if ( !MPlayer->missionSettings.quickStart)
								{
									setUpMultiplayerLogisticsScreens();	
								}						
							}
							else if ( curScreenX == 3 )
							{
								bDone = true;
							}
						}
					}
					if ( screens[curScreenX+1][curScreenY] )
					{
						if ( screens[curScreenX+1][curScreenY] == placeHolderScreen )
							curScreenX++;
						screens[curScreenX+1][curScreenY]->begin();
					}


					leftAnim.begin();
					curScreenX++;
					soundSystem->playDigitalSample( LOG_NEXTBACKBUTTONS );
					animJustBegun = true;
				}
				else
				{
					bDone = true;
				}

				
			}
			else if ( pCurScreen->getStatus() == LogisticsScreen::PREVIOUS )
			{
				pCurScreen->end();
				if (screens[curScreenX-1][curScreenY] ) 
				{
					if ( screens[curScreenX-1][curScreenY] == placeHolderScreen )
						curScreenX--;
				
					screens[curScreenX-1][curScreenY]->begin();
				}
				rightAnim.begin();
				curScreenX--;
				soundSystem->playDigitalSample( LOG_NEXTBACKBUTTONS );
				animJustBegun = true;
			}
			else if ( pCurScreen->getStatus() == LogisticsScreen::DOWN )
			{
				pCurScreen->end();
				if ( screens[curScreenX][curScreenY+1] )
					screens[curScreenX][curScreenY+1]->begin();
				upAnim.begin();
				curScreenY++;
				soundSystem->playDigitalSample( LOG_NEXTBACKBUTTONS );
				animJustBegun = true;
			}
			else if ( pCurScreen->getStatus() == LogisticsScreen::UP )
			{
				pCurScreen->end();
				if (screens[curScreenX][curScreenY-1]) 
					screens[curScreenX][curScreenY-1]->begin();
				downAnim.begin();
				curScreenY--;
				soundSystem->playDigitalSample( LOG_NEXTBACKBUTTONS );
				animJustBegun = true;
			}
			else if ( pCurScreen->getStatus() == LogisticsScreen::MAINMENU )
			{
				bSplash = true;
				mainMenu->setDrawBackground( false );
				mainMenu->begin();
				animJustBegun = true;
			}
			else if ( pCurScreen->getStatus() == LogisticsScreen::SKIPONENEXT )
			{
				pCurScreen->end();
				if ( curScreenX < 3 - 1 )
				{
					if ( screens[curScreenX+1+1][curScreenY] )
						screens[curScreenX+1+1][curScreenY]->begin();
					leftAnim.begin();
					curScreenX++;
					curScreenX++;
					soundSystem->playDigitalSample( LOG_NEXTBACKBUTTONS );
					animJustBegun = true;
				}
				else
				{
					bDone = true;
				}
			}
			else if ( pCurScreen->getStatus() == LogisticsScreen::SKIPONEPREVIOUS )
			{
				pCurScreen->end();
				if (screens[curScreenX-1-1][curScreenY] ) 
					screens[curScreenX-1-1][curScreenY]->begin();
				rightAnim.begin();
				curScreenX--;
				curScreenX--;
				soundSystem->playDigitalSample( LOG_NEXTBACKBUTTONS );
				animJustBegun = true;
			}
		} 
	}
	else
		bDone = true;

	return NULL;

}

void MissionBegin::render()
{
	// UI-ASPECT-ANCHOR-1: front-end frame — draw the UI on the 16:9 canvas
	// (re-asserted every frame; auto-cleared at flushHUDBatch).
	gos_SetUiCanvasActive( true );

	long xOffset = 0;
	long yOffset = 0;
	
	LogisticsScreen* pOtherScreen = 0;
	LogisticsScreen* pCurScreen = 0;
	if ( curScreenX > -1 && curScreenY > -1 )
	{
		pCurScreen = screens[curScreenX][curScreenY];

	}

	long xOtherOffset = 0;
	long yOtherOffset = 0;

	if ( bSplash )
	{
		if ( pCurScreen )
		{
			if ( MainMenu::bDrawMechlopedia )
				pCurScreen->beginFadeIn(1.0);
			else if ( !mainMenu->occludesLogisticsScreens() )
			{
				// Only render the screen behind the menu when it can actually
				// be seen (menu slide animations).  The defs UI path draws
				// through ImGui, which composites AFTER the menu's GameOS
				// movie/background draws -- rendering here while the menu is
				// opaque would put this screen ON TOP of the movie and menu
				// background instead of behind them.
				pCurScreen->render();
				// MENU-STALE-CLEAR-2: OG dimmed the logistics screen behind the
				// in-logistics menu with a fullscreen rect; the defs menu page
				// has no backdrop, so the screen's ImGui page bled through
				// wherever the menu art didn't cover (bottom third — any panel).
				// Submit the dim IN THE IMGUI LAYER between the screen page
				// (submitted above) and the menu page (submitted below).
				{
					float dw = 0.f, dh = 0.f;
					if ( GuiRuntime::GetDisplaySize( dw, dh ) && dw > 0.f )
						GuiRuntime::DrawUiRect( 0.f, 0.f, dw, dh, 0xE6000000, true );
				}
			}
			else
			{
				// MENU-STALE-CLEAR-1: with the defs menu page active the legacy
				// menu background no longer repaints the backbuffer, so backing
				// out of a logistics screen left its last frame visible beneath
				// the ImGui menu. Paint the GameOS layer black first (same
				// pattern as Mechlopedia::render).
				GUI_RECT rect = { 0, 0, Environment.screenWidth, Environment.screenHeight };
				drawRect( rect, 0xff000000 );
				// MENU-STALE-CLEAR-2: the GameOS clear killed the top-2/3 ghost
				// but ImGui-layer remnants (any panel's defs page submitted
				// earlier this frame) survived in the bottom third. Submit an
				// opaque ImGui-layer backdrop BEFORE the menu page so the menu
				// composites over black, not over the previous screen.
				{
					float dw = 0.f, dh = 0.f;
					if ( GuiRuntime::GetDisplaySize( dw, dh ) && dw > 0.f )
						GuiRuntime::DrawUiRect( 0.f, 0.f, dw, dh, 0xff000000, true );
				}
			}
		}
		mainMenu->render();
		return;
	}


	if ( pCurScreen /*&& pCurScreen->getStatus() == LogisticsScreen::RUNNING*/ )
	{
		if ( leftAnim.isAnimating() && !leftAnim.isDone() )
		{
			if ( animJustBegun )
				leftAnim.begin(); // restart to compensate for LONG begins

			xOffset = leftAnim.getXDelta() + 800;			
			yOffset = leftAnim.getYDelta();
			xOtherOffset = xOffset - 800;
			yOtherOffset = yOffset;
			pOtherScreen = screens[curScreenX-1][curScreenY];
			if ( pOtherScreen == placeHolderScreen )
				pOtherScreen = screens[curScreenX-2][curScreenY];
		}
		else if ( downAnim.isAnimating() && !downAnim.isDone() )
		{
			if ( animJustBegun )
				downAnim.begin(); // restart to compensate for LONG begins

			xOffset = downAnim.getXDelta();
			yOffset = downAnim.getYDelta();
			xOtherOffset = xOffset;
			yOtherOffset = yOffset + 600;
			pOtherScreen = screens[curScreenX][curScreenY + 1];
		}
		else if ( upAnim.isAnimating() && !upAnim.isDone() )
		{
			if ( animJustBegun )
				upAnim.begin(); // restart to compensate for LONG begins

			xOffset = upAnim.getXDelta();
			yOffset = upAnim.getYDelta();
			xOtherOffset = xOffset;
			yOtherOffset = yOffset - 600;
			pOtherScreen = screens[curScreenX][curScreenY - 1];
		}
		else if ( rightAnim.isAnimating() && !rightAnim.isDone() )
		{
			if ( animJustBegun )
				rightAnim.begin(); // restart to compensate for LONG begins

			xOffset = rightAnim.getXDelta() - 800;
			yOffset = rightAnim.getYDelta();
			xOtherOffset = xOffset + 800;
			yOtherOffset = yOffset;
			pOtherScreen = screens[curScreenX+1][curScreenY];
			if ( pOtherScreen == placeHolderScreen )
				pOtherScreen = screens[curScreenX+2][curScreenY];

		}

		if ( curScreenX == 4 ) // don't scroll last screen
		{
			if ( pOtherScreen )
				pOtherScreen->render( 0, 0 );
			pCurScreen->render( 0, 0 );
		}

		else
		{
			if ( pOtherScreen )
				pOtherScreen->render( xOtherOffset, yOtherOffset );
			pCurScreen->render( xOffset, yOffset );

		}

	

	}

	animJustBegun = false;
}

void MissionBegin::beginSplash( const char* playerName)
{
	// check for old screen and end that
	if ( curScreenX > -1 && curScreenY > -1 )
	{
		LogisticsScreen*		pCurScreen = screens[curScreenX][curScreenY];
		if ( pCurScreen )
			pCurScreen->end();
	}


	bSplash = true;
	curScreenX = 0;
	curScreenY = 1;
	bReadyToLoad = 0;
	bDone = 0;
	if ( mainMenu )
	{
		if ( MPlayer && MPlayer->launchedFromLobby )
			mainMenu->skipIntro();

		mainMenu->setDrawBackground( true );
		mainMenu->begin();
		soundSystem->playDigitalSample( LOG_MAINMENUBUTTON );
		if (playerName)
			mainMenu->setHostLeftDlg(playerName);
	}
}

void MissionBegin::beginMPlayer()
{
	// already set up
	if ( multiplayerScreens[0][1] )
		return;


		//multiplayer setup screens
	MPConnectionType*		pMPConnectionType = NULL;
	placeHolderScreen = NULL;
	MPGameBrowser*		pMPGameBrowser = NULL;
	MPParameterScreen*		pMPParameterScreen = NULL;

	char path[512];
	FitIniFile file;
	// initalize MPConnectionType
	pMPConnectionType = new MPConnectionType;

	strcpy( path, artPath );
	strcat( path, "mcl_mp_connectiontype.fit" );

	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", path );
		Assert( 0, 0, error );
		return;		
	}

	pMPConnectionType->init( &file );
	file.close();

	// initalize MPPlaceHolderScreen
	placeHolderScreen = new MPPlaceHolderScreen;

	// initalize MPParameterScreen
	pMPParameterScreen = new MPParameterScreen;

	strcpy( path, artPath );
	strcat( path, "mcl_mp_param.fit" );

	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", path );
		Assert( 0, 0, error );
		return;		
	}

	pMPParameterScreen->init( &file );
	file.close();

	// initalize MPGameBrowser
	pMPGameBrowser = new MPGameBrowser;

	strcpy( path, artPath );
	strcat( path, "mcl_mp_lanbrowser.fit" );

	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", path );
		Assert( 0, 0, error );
		return;		
	}

	pMPGameBrowser->init( &file );
	file.close();


	// initalize MP prefs
	MPPrefs* pMPPrefs = new MPPrefs;

	strcpy( path, artPath );
	strcat( path, "mcl_mp_playerprefs.fit" );

	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", path );
		Assert( 0, 0, error );
		return;		
	}

	pMPPrefs->init( file );
	file.close();

	LoadScreenWrapper* pMLoadScreen = new LoadScreenWrapper;

	strcpy( path, artPath );
	strcat( path, "mcl_loadingscreen.fit" );
	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", path );
		Assert( 0, 0, error );
		return;		
	}

	pMLoadScreen->init( file );
	file.close();


	
	pMPConnectionType->ppConnectionScreen = (void **)(&(screens[1][1]));
	pMPConnectionType->pLocalBrowserScreen = pMPGameBrowser;
	pMPConnectionType->pDirectTcpipScreen = pMPGameBrowser;
	pMPConnectionType->pMPPlaceHolderScreen = placeHolderScreen;

	
	multiplayerScreens[0][1] = pMPConnectionType;
	multiplayerScreens[1][1] = pMPGameBrowser;
	multiplayerScreens[2][1] = pMPParameterScreen;
	multiplayerScreens[3][1] = pMLoadScreen;
	multiplayerScreens[2][0] = pMPPrefs;

	pMPPrefs->initColors();
	

	{
		for ( int i = 0; i < 4; i++ )
		{
			for ( int j = 0; j < 4; j++ )
			{
				if ( multiplayerScreens[i][j] )
				{
					if (  multiplayerScreens[i][j]->getButton(MB_MSG_NEXT) )
						multiplayerScreens[i][j]->getButton(MB_MSG_NEXT)->setPressFX( -1 );

					if ( multiplayerScreens[i][j]->getButton(MB_MSG_PREV) )
						multiplayerScreens[i][j]->getButton(MB_MSG_PREV)->setPressFX( -1 );
					if ( multiplayerScreens[i][j]->getButton(MB_MSG_MAINMENU) )
						multiplayerScreens[i][j]->getButton(MB_MSG_MAINMENU)->setPressFX( LOG_MAINMENUBUTTON );
				}
				
			}
		}
	}
}

void MissionBegin::setUpMultiplayerLogisticsScreens()
{


	for (int i = 0; i < 5/*dim screen X*/; i+=1)
	{
		int j;
		for (j = 0; j < 3/*dim screen Y*/; j += 1)
		{
			screens[i][j] = singlePlayerScreens[i][j];
		}
	}
	curScreenX = 0;
	curScreenY = 1;

}

void MissionBegin::setToMissionBriefing()
{
	bReadyToLoad = 0;

	if ( screens[curScreenX][curScreenY] )
		screens[curScreenX][curScreenY]->end();
	{
		curScreenX = 1;
		curScreenY = 1;
		if ( screens[curScreenX][curScreenY] )
			screens[curScreenX][curScreenY]->begin();
	}

}

void MissionBegin::restartMPlayer( const char* playerName )
{
	bReadyToLoad = 0;
	bDone = 0;

	if ( screens[curScreenX][curScreenY] )
		screens[curScreenX][curScreenY]->end();

	for (int i = 0; i < 5/*dim screen X*/; i+=1)
	{
		int j;
		for (j = 0; j < 3/*dim screen Y*/; j += 1)
		{
			screens[i][j] = this->multiplayerScreens[i][j];
		}
	}

		curScreenX = 2;
		curScreenY = 1;
		if ( screens[curScreenX][curScreenY] )
		{
			screens[curScreenX][curScreenY]->begin();

			if ( playerName )
			{
				((MPParameterScreen*)screens[curScreenX][curScreenY])->setHostLeftDlg(playerName);
			}
		}



}

void MissionBegin::beginZone()
{
	beginMPlayer();
	restartMPlayer(NULL);
	bReadyToLoad = 0;
	bDone = 0;

	LogisticsData::instance->startMultiPlayer();

	mainMenu->skipIntro();

	ChatWindow::init();


}
void MissionBegin::beginAtConnectionScreen()
{

	beginMPlayer();
	int i;
	for (i = 0; i < 5/*dim screen X*/; i+=1)
	{
		int j;
		for (j = 0; j < 3/*dim screen Y*/; j += 1)
		{
			screens[i][j] = multiplayerScreens[i][j];
		}
	}
	curScreenX = 0;
	curScreenY = 1;
	screens[curScreenX][curScreenY]->beginFadeIn( 1.0 );
	bReadyToLoad = 0;
	bDone = 0;

	LogisticsData::instance->startMultiPlayer();

	ChatWindow::init();


}
