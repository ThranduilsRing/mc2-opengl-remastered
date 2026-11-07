#include "vk_scene_renderer.h"
#include "render_submissions.h"
#include "Gates/preferences_legacy.h"
//--------------------------------------------------------------------------
//
// And away we go.  MechCommander 2 -- Electric Boogaloo
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "mechcmd2_internal.h"
#include "startup_preload.h"
#include "render_resource_owners.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "mc2fmt.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "../GameOS/gameos/utils/mc2log.h"

#ifndef VERSION_H
#include"version.h"
#endif


//------------------------------------------------------------------------------------------------------------
// MechCmdr2 Global Instances of Things
// NS3: systemHeap defined in mclib/heap.cpp (engine boundary).

std::unique_ptr<UserInput> ownedUserInput;
std::unique_ptr<FloatHelp> ownedFloatHelp;
std::unique_ptr<PANE> ownedPane;
std::unique_ptr<WINDOW> ownedWindow;
std::unique_ptr<TimerManager> ownedTimerManager;
std::unique_ptr<GameSoundSystem> ownedSoundSystem;


// NS3: fastFiles/numFastFiles/maxFastFiles defined in mclib/fastfile.cpp (engine boundary).

long FilterState = gos_FilterNone;
bool quitGame = FALSE;
bool justStartMission = FALSE;
bool gamePaused = FALSE;
bool hasGuardBand = false;
bool canMultiTextureDetail = false;
bool gotBombData = false;

TimerManagerPtr timerManager = nullptr;

bool initGameLogs = false;
bool initNetLog = false;
GameLog* NetLog = nullptr;
bool initCombatLog = false;
GameLog* CombatLog = nullptr;
bool initBugLog = false;
GameLog* BugLog = nullptr;
bool initLRMoveLog = false;

bool KillAmbientLight = false;

void InitDW (void);

// sebi:? num graphics cards?
//extern DWORD					NumDevices;
DWORD					NumDevices = 1;

unsigned long elementHeapSize = 1024000;
unsigned long maxElements = 2048;
unsigned long maxGroups = 1024;

unsigned long systemHeapSize = 8192000;
unsigned long guiHeapSize = 1023999;
unsigned long logisticsHeapSize = 4095999;
unsigned long missionHeapSize = 3072000;
unsigned long spriteDataHeapSize = 2048000;
unsigned long spriteHeapSize = 8192000;
unsigned long polyHeapSize = 1024000;
void __stdcall ExitGameOS();

// NS3: gosResourceHandle defined in mclib/utilities.cpp (engine boundary).
// NS3: gosFontHandle + gosFontScale defined in mclib/floathelp.cpp (engine boundary).
// sebi: whe the f**k should this be defined? It is not even used
//extern HGOSFONT3D	FontHandle;
HGOSFONT3D	FontHandle;

FloatHelpPtr globalFloatHelp = nullptr;
unsigned long currentFloatHelp = 0;
float MaxMinUV = 8.0f;
ProcType Processor = CPU_PENTIUM;		//Needs to be set when GameOS supports ProcessorID -- MECHCMDR2
void EnterWindowMode();
long MaxResourcePoints = -1;
bool ShowMovers = false;
bool EnemiesGoalPlan = false;
bool inViewMode = false;
unsigned long viewObject = 0x0;

char* ExceptionGameMsg = nullptr;

char buildNumber[80];

bool EULAShown = false;

// sebi: donot know where it used, probably in some closed source linked lib
//extern bool gNoDialogs;
bool gNoDialogs = false;

//DEBUG
constexpr auto MAX_SHAPES = 0;
TG_MultiShape 	testShape[36];
TG_AnimateShape	testAnim[36];

glm::vec3 pos[36] = 
{
	glm::vec3(-3400.0f,3000.0f,-1.0f),
	glm::vec3(-3300.0f,2700.0f,-1.0f),
	glm::vec3(-3300.0f,2800.0f,-1.0f),
	glm::vec3(-3300.0f,2900.0f,-1.0f),
	glm::vec3(-3300.0f,3000.0f,-1.0f),
	glm::vec3(-3200.0f,2700.0f,-1.0f),
	glm::vec3(-3200.0f,2800.0f,-1.0f),
	glm::vec3(-3200.0f,2900.0f,-1.0f),
	glm::vec3(-3200.0f,3000.0f,-1.0f),
	glm::vec3(-3100.0f,2700.0f,-1.0f),
	glm::vec3(-3100.0f,2800.0f,-1.0f),
	glm::vec3(-3100.0f,2900.0f,-1.0f),
	glm::vec3(-3100.0f,3000.0f,-1.0f),
	glm::vec3(-3000.0f,2700.0f,-1.0f),
	glm::vec3(-3000.0f,2800.0f,-1.0f),
	glm::vec3(-3000.0f,2900.0f,-1.0f),
	glm::vec3(-3000.0f,3000.0f,-1.0f),
	glm::vec3(-2900.0f,2700.0f,-1.0f),
	glm::vec3(-2900.0f,2800.0f,-1.0f),
	glm::vec3(-2900.0f,2900.0f,-1.0f),
	glm::vec3(-2900.0f,3000.0f,-1.0f),
	glm::vec3(-2800.0f,2700.0f,-1.0f),
	glm::vec3(-2800.0f,2800.0f,-1.0f),
	glm::vec3(-2800.0f,2900.0f,-1.0f),
	glm::vec3(-2800.0f,3000.0f,-1.0f),
	glm::vec3(-2700.0f,2700.0f,-1.0f),
	glm::vec3(-2700.0f,2800.0f,-1.0f),
	glm::vec3(-2700.0f,2900.0f,-1.0f),
	glm::vec3(-2700.0f,3000.0f,-1.0f),
	glm::vec3(-2600.0f,2700.0f,-1.0f),
	glm::vec3(-2600.0f,2800.0f,-1.0f),
	glm::vec3(-2600.0f,2900.0f,-1.0f),
	glm::vec3(-2600.0f,3000.0f,-1.0f),
	glm::vec3(-2500.0f,2700.0f,-1.0f),
	glm::vec3(-2500.0f,2800.0f,-1.0f),
	glm::vec3(-2500.0f,2900.0f,-1.0f)
};

long NumDisableAtStart = 0;
long DisableAtStart[MAX_KILL_AT_START];

std::unique_ptr<OptionsScreenWrapper> optionsScreenWrapper;
std::unique_ptr<Mission> ownedMission;
std::unique_ptr<Logistics> ownedLogistics;
bool bInvokeOptionsScreenFlag = false;

bool SnifferMode = false;
gos_VERTEX *testVertex = nullptr;
WORD *indexArray = nullptr;
DWORD testTextureHandle = 0xffffffff;
float totalTime = 0;
DWORD numIterations = 4;
DWORD curIteration = 0;
DWORD curDevice = 0;
bool isUsingSoftware = false;
float trisPerSecond[MAX_HARDWARE_CARDS] = 
{
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f
};

bool checkedBomb = false;
char DebugStatusBarString[256];
GameDebugWindow* DebugWindow[NUM_DEBUG_WINDOWS] = {nullptr, nullptr, nullptr, nullptr};
GameObjectPtr DebugGameObject[3] = {nullptr, nullptr, nullptr};
long GameObjectWindowList[3] = {0, 0, 0};
long NumGameObjectsToDisplay = 0;
bool DebugWindowOpen[NUM_DEBUG_WINDOWS] = {false, false, false, false};
bool DebugStatusBarOpen = false;
bool DebugScoreBoardOpen = false;
bool DebugHelpOpen = false;

//***************************************************************************

char* __stdcall GetGameInformation() 
{
	return(ExceptionGameMsg);
}

//long cLoadString (HINSTANCE hInstance,  UINT uID, LPTSTR lpBuffer, int nBufferMax );

#define SnifferTime(x,y)	
DWORD startTime;
DWORD endTime;

//---------------------------------------------------------------------------
constexpr char GAME_REG_KEY[] = "Software\\Microsoft\\Microsoft Games\\MechCommander2\\1.0";
using EBUPROC = DWORD (*) (LPCTSTR lpRegKeyLocation, LPCTSTR lpEULAFileName, LPCSTR lpWarrantyFileName, BOOL fCheckForFirstRun);

bool FirstRunEula(void)
{
	return (true);
}



extern Stuff::MemoryStream *effectStream;	// NS3: def in mclib/txmmgr.cpp
extern MidLevelRenderer::MLRClipper * theClipper;

bool gameStarted = false;
void __stdcall InitializeGameEngine()
{
	ZoneScopedN("InitializeGameEngine");

	mc2PrintBuildFingerprint();
    // sebi WTF? do we ned to do it for InitDW?
	//__asm push esi;

	// gotta have this no matter what
	{ ZoneScopedN("InitializeGameEngine resourceDLL");
#ifdef PLATFORM_WINDOWS
	#ifdef _WIN64
		gosResourceHandle = gos_OpenResourceDLL("mc2res_64.dll", nullptr, 0);
	#else
		gosResourceHandle = gos_OpenResourceDLL("mc2res_32.dll", nullptr, 0);
	#endif
#else
	gosResourceHandle = gos_OpenResourceDLL("./libmc2res.so", nullptr, 0);
#endif
	}

	if(!gosResourceHandle) {
        mc2::fmt::print_err("Error loading ResourseDLL (mc2res)\n");
	}

	//Check for enough SwapFile Space FIRST!!!
	// In order to do that, we must force Win2K/XP to enlarge
	// its swapfile at the get go to insure goodness and that
	// the message does not come up during game run.
	DWORD testMemLength = 123000000;
	void *testMem = VirtualAlloc(nullptr,testMemLength,MEM_COMMIT,PAGE_READWRITE);

#ifndef LINUX_BUILD
   	MEMORYSTATUS ms;
   	GlobalMemoryStatus( &ms );
   	
   	if (ms.dwTotalPageFile < 250000000)
   	{
   		char txt[4096];
   		char msg[4096];
   		cLoadString(IDS_SWAPFILE_TOO_SMALL,txt,4095);
   		sprintf(msg,txt,(ms.dwAvailPageFile / (1024 * 1024)));

   		char caption[1024];
   		cLoadString(IDS_SWAPFILE_CAPTION,caption,1023);

   		mc2MessageBox(msg,caption,MB_OK | MB_ICONWARNING,IDOK);

   		gos_TerminateApplication();
   		return;
   	}
#endif

	if (testMem)
		VirtualFree(testMem,testMemLength,MEM_RELEASE);

	cLoadString(IDS_MC2_FILEMISSING,FileMissingString,511);
	cLoadString(IDS_MC2_CDMISSING,CDMissingString,1023);
	cLoadString(IDS_MC2_MISSING_TITLE,MissingTitleString,255);

	if ( !EULAShown )
	{
		if ( !FirstRunEula() )
		{
			gos_TerminateApplication();
			return;
		}
	}

	//Check for sufficient hard Drive space on drive game is running from
	char currentPath[1024];
	gos_GetCurrentPath( currentPath, 1023 );
	__int64 driveSpace = gos_GetDriveFreeSpace(currentPath);
	if (driveSpace < (20 * 1024 * 1024))
	{
		char title[256];
		char msg[2048];
		cLoadString(IDS_GAME_HDSPACE_ERROR,title,255);
		cLoadString(IDS_GAME_HDSPACE_MSG,msg,2047);
		DWORD result = mc2MessageBox(msg,title,MB_OKCANCEL | MB_ICONWARNING,IDOK);
		if (result == IDCANCEL)
			ExitGameOS();
	}

    //sebi: we are probably NOT Voodoo 3 
#if LINUX_BUILD && 0
	//Check if we are a Voodoo 3.  If so, ONLY allow editor to run IF
	// Starting resolution is 1024x768x16 or LOWER.  NO 32-BIT ALLOWED!
	if ((gos_GetMachineInformation(gos_Info_GetDeviceVendorID,0) == 0x121a) &&
		(gos_GetMachineInformation(gos_Info_GetDeviceDeviceID,0) == 0x0005))
	{
		DEVMODE dev;
		memset( &dev, 0, sizeof ( DEVMODE ) );
		dev.dmSize = sizeof( DEVMODE );
		dev.dmSpecVersion = DM_SPECVERSION;
		EnumDisplaySettings( nullptr, ENUM_CURRENT_SETTINGS, &dev );

		if ((dev.dmPelsWidth > 1024) || (dev.dmPelsHeight > 768) || (dev.dmBitsPerPel > 16))
		{
			char title[256];
			char msg[2048];
			cLoadString(IDS_GAME_ERROR,title,255);
			cLoadString(IDS_GAME_VOODOO3,msg,2047);
			mc2MessageBox(msg,title,MB_OK | MB_ICONWARNING,IDOK);
			ExitGameOS();
		}
	}
#endif

	//Then, we should check to see if the options.cfg exists.  if NOT, 
	// Bring up a sniffer warning dialog, sniff, bring up sniffer end dialog and end.
	if (!fileExists("options.cfg"))
	{
		// Smoke-mode guard: the sniffer branch would pop a blocking MessageBox
		// ("will now check your computer's hardware") that deadlocks any
		// unattended subprocess. If MC2_SMOKE_MODE is set, the caller almost
		// certainly spawned us from the wrong CWD — fail fast with a useful
		// summary line so the runner can bucket it rather than hang.
		if (SmokeMode::state().enabled) {
			SmokeMode::emitFailSummary("sniffer_would_run",
				"init_pre_sniffer");
			mc2::fmt::print_err("[SMOKE v1] sniffer branch reached; options.cfg not found at CWD. Is the runner spawning from the install directory?\n");
			fflush(stderr);
			std::exit(5);
		}

		//Must also check for a RIVA TNT card with driver 4.10.1.0131
		// If this card is in the machine with this driver, do NOT allow sniffer to run.
		// Just copy minprefs.cfg to options.cfg and move on.
		if ((gos_GetMachineInformation(gos_Info_GetDeviceVendorID,0) == 0x10de) &&
			(gos_GetMachineInformation(gos_Info_GetDeviceDeviceID,0) == 0x0020) &&
			(gos_GetMachineInformation(gos_Info_GetDeviceDriverVersionHi,0) == 0x0004000a) &&
			(gos_GetMachineInformation(gos_Info_GetDeviceDriverVersionLo,0) == 0x00010083))
		{
			CopyFile("minprefs.cfg","orgprefs.cfg",true);
			CopyFile("minprefs.cfg","options.cfg",true);
		}
		else
		{
			//NEW Plan.  Regardless, copy the minprefs to options.  That way, sniffer can only screw up once!!
			// After the first run, it will no longer run and the game will continue with minprefs.
			SnifferMode = true;
			CopyFile("minprefs.cfg","orgprefs.cfg",true);
			CopyFile("minprefs.cfg","options.cfg",true);
		}
	}

	//Seed the random Number generator.
	// DETERMINISTIC-RNG-1: under MC2_DETERMINISTIC_RNG the startup seed must be
	// pinned too, otherwise pre-mission (menu/startup) gos_rand calls vary per
	// run and break replay before Mission::init's per-mission reseed lands.
	// Default ON; MC2_DETERMINISTIC_RNG=0 (kill-switch) restores the stock wall-clock seed.
	{
		const bool detRng = mc2::gate::get<"MC2_DETERMINISTIC_RNG">();
		if (detRng) {
			uint32_t seed = 0xCAFEBABEu;  // fixed startup seed
			const std::string seedText = mc2::gate::str<"MC2_RNG_SEED">();   // "" when unset or empty
			if (!seedText.empty())
				seed = static_cast<uint32_t>(strtoul(seedText.c_str(), nullptr, 0));
			gos_srand(seed);
			SetSimMissionSeed(seed);   // pre-mission ABL seedRandom(-1) reseeds from the startup seed
		} else {
			gos_srand(timeGetTime());
		}
	}

	if (!SnifferMode)
	{
		ZoneScopedN("InitializeGameEngine normalStartup");
		//---------------------------------------------------------------------

		float doubleClickThreshold = 0.2f;
        //sebi :????
		//long dragThreshold = .016667;
		float dragThreshold = .016667f;
	
		Environment.Key_Exit=-1; // so escape doesn't kill your app
	
		//-------------------------------------------------------------
		// Find the CDPath in the registry and save it off so I can
		// look in CD Install Path for files.
		
		//Changed for the shared source release, just set to current directory
		//DWORD maxPathLength = 1023;
		//gos_LoadDataFromRegistry("CDPath", CDInstallPath, &maxPathLength);
		//if (!maxPathLength)
		//	strcpy(CDInstallPath,"..\\");
		// to find necessary files
#ifdef PLATFORM_WINDOWS
		// for my windows layout
		strcpy(CDInstallPath,"./");
#else
		strcpy(CDInstallPath,"../FinalBuild/");
#endif        

		//--------------------------------------------------------------
		// Start the SystemHeap and globalHeapList
		{
			ZoneScopedN("InitializeGameEngine heaps");
		
		
		
		}
	
		//Start finding the Leaks
//		systemHeap->startHeapMallocLog();

		//Check for OrgPrefs.cfg.  If this file is not present,
		// We need to create it because sniffer didn't run.
		// Create a new prefs.cfg, too.  This way, we can easily restore
		// them to a running state if they screw up their prefs.
		// All they have to do is delete the orgprefs.cfg file.
		// OR re-install!!
		//
		// ALWAYS use minPrefs.cfg here!
		if (!fileExists("orgprefs.cfg"))
		{
			CopyFile("minprefs.cfg","orgprefs.cfg",true);
			CopyFile("minprefs.cfg","options.cfg",true);
		}

		//--------------------------------------------------------------
		// Read in System.CFG
		{ ZoneScopedN("InitializeGameEngine system.cfg");
		FitIniFile systemFile;

	#ifdef _DEBUG
		long systemOpenResult =
	#endif
			systemFile.open("system.cfg");
			   
	#ifdef _DEBUG
		if( systemOpenResult != NO_ERR)
		{
			char Buffer[256];
			gos_GetCurrentPath( Buffer, 256 );
			STOP(( "Cannot find \"system.cfg\" file in %s",Buffer ));
		}
	#endif
	
		{
	#ifdef _DEBUG
			long systemBlockResult = 
	#endif
				systemFile.seekBlock("systemHeap");
			gosASSERT(systemBlockResult == NO_ERR);
			{
				long result = systemFile.readIdULong("systemHeapSize",systemHeapSize);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdULong("guiHeapSize",guiHeapSize);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdULong("logisticsHeapSize",logisticsHeapSize);
				gosASSERT(result == NO_ERR);
			}
	
	#ifdef _DEBUG
			long systemPathResult = 
	#endif
				systemFile.seekBlock("systemPaths");
			gosASSERT(systemPathResult == NO_ERR);
			{
				long result = systemFile.readIdString("terrainPath",terrainPath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("artPath",artPath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("fontPath",fontPath,79);
				gosASSERT(result == NO_ERR);
	
				//result = systemFile->readIdString("savePath",savePath,79);
				//gosASSERT(result == NO_ERR);

                // sebi: get user dependent savegame directory
                char userDataDir[1024] = {0};
                if(!gos_GetUserDataDirectory(userDataDir)) {
				    MC2_LOG(Io, Debug, "Failed to get user data directory");
                    gos_TerminateApplication();
                }
    
                S_snprintf(savePath, sizeof(savePath), "%s" PATH_SEPARATOR "%s" PATH_SEPARATOR, userDataDir, "savegame" );
                // MC2_SAVE_DIR: player-path harness redirect so a scripted run never touches the player's saves.
                const std::string saveDirOverride = mc2::gate::str<"MC2_SAVE_DIR">();
                if (!saveDirOverride.empty())
                    S_snprintf(savePath, sizeof(savePath), "%s" PATH_SEPARATOR, saveDirOverride.c_str());

                MC2_LOG(Io, Debug, "{}", mc2::fmt::safe(savePath));

	
				result = systemFile.readIdString("spritePath",spritePath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("shapesPath",shapesPath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("soundPath",soundPath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("objectPath",objectPath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("cameraPath",cameraPath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("tilePath",tilePath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("missionPath",missionPath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("warriorPath",warriorPath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("profilePath",profilePath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("interfacepath",interfacePath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("moviepath",moviePath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("CDsoundPath",CDsoundPath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("CDmoviepath",CDmoviePath,79);
				gosASSERT(result == NO_ERR);
	
				result = systemFile.readIdString("CDspritePath",CDspritePath,79);
				gosASSERT(result == NO_ERR);
			}
	
	#ifdef _DEBUG
			long fastFileResult = 
	#endif
				systemFile.seekBlock("FastFiles");
			gosASSERT(fastFileResult == NO_ERR);
			{
				long result = systemFile.readIdLong("NumFastFiles",maxFastFiles);
				if (result != NO_ERR)
					maxFastFiles = 0;
	
				if (maxFastFiles)
				{
					fastFiles = static_cast<FastFile **>(malloc(maxFastFiles*sizeof(FastFile *)));
					memset(fastFiles,0,maxFastFiles*sizeof(FastFile *));
	
					int fileNum = 0;
					char fastFileId[10];
					char fileName[100];
					mc2::fmt::to_buf(fastFileId, "File{:d}", mc2::fmt::as_i32(fileNum));
		
					while (systemFile.readIdString(fastFileId,fileName,99) == NO_ERR)
					{
						bool result = FastFileInit(fileName);
						if (!result)
							STOP(("Unable to startup fastfiles.  Probably an old one in the directory!!"));
	
						fileNum++;
						mc2::fmt::to_buf(fastFileId, "File{:d}", mc2::fmt::as_i32(fileNum));
					}
				}
			}

			// Mod overlay: all subdirs of ./mods/ are active.
			// Mods shadow base data/ files for relative data/* reads.
			InitModSearchPaths("./mods/");
			// Inject mod-overlay text FITs now that mod index is populated.
			// MC2Strings may have already fired Load() before this point (with
			// an empty mod index); LoadModFits() bypasses the one-shot guard.
			MC2Strings::LoadModFits();
			// Warm the OS file cache for the archives and mod files while init, the intro and the menu run.
			if (!justStartMission)
				startStartupPreload();

			long result = systemFile.seekBlock("UseMusic");
			if (result == NO_ERR)
				useMusic = TRUE;
			else
				useMusic = FALSE;
				
			result = systemFile.seekBlock("UseSound");
			if (result == NO_ERR)
			{
				useSound = TRUE;
			}
			else
			{
				useSound = FALSE;
				useMusic = FALSE;
			}

			result = systemFile.seekBlock("CameraSettings");
			if (result == NO_ERR)
			{
				result = systemFile.readIdFloat("MaxPerspective",Camera::MAX_PERSPECTIVE);
				if (result != NO_ERR)
					Camera::MAX_PERSPECTIVE = 88.0f;

				Camera::MAX_PERSPECTIVE = std::min(Camera::MAX_PERSPECTIVE, 90.0f);

				result = systemFile.readIdFloat("MinPerspective",Camera::MIN_PERSPECTIVE);
				if (result != NO_ERR)
					Camera::MIN_PERSPECTIVE = 18.0f;

				Camera::MIN_PERSPECTIVE = std::max(Camera::MIN_PERSPECTIVE, -89.0f);

				result = systemFile.readIdFloat("MaxOrtho",Camera::MAX_ORTHO);
				if (result != NO_ERR)
					Camera::MAX_ORTHO = 88.0f;

				Camera::MAX_ORTHO = std::min(Camera::MAX_ORTHO, 90.0f);

				result = systemFile.readIdFloat("MinOrtho",Camera::MIN_ORTHO);
				if (result != NO_ERR)
					Camera::MIN_ORTHO = 18.0f;

				Camera::MIN_ORTHO = std::max(Camera::MIN_ORTHO, 0.0f);

				result = systemFile.readIdFloat("AltitudeMinimum",Camera::AltitudeMinimum);
				if (result != NO_ERR)
					Camera::AltitudeMinimum = 10.0f;

				Camera::AltitudeMinimum = std::max(Camera::AltitudeMinimum, 1.0f);

				result = systemFile.readIdFloat("AltitudeMaximumHi",Camera::AltitudeMaximumHi);
				if (result != NO_ERR)
					Camera::AltitudeMaximumHi = 1600.0f;

				result = systemFile.readIdFloat("AltitudeMaximumLo",Camera::AltitudeMaximumLo);
				if (result != NO_ERR)
					Camera::AltitudeMaximumHi = 1500.0f;
			}
		}
	
		systemFile.close();
		}
	
		if (initGameLogs) {
			GameLog::setup();
			if (initNetLog && !NetLog) {
				NetLog = GameLog::getNewFile();
				if (!NetLog)
					Fatal(0, " Couldn't create Net Log ");
				long err = NetLog->open("net.log");
				if (err)
					Fatal(0, " Couldn't open Net Log ");
			}
			if (initCombatLog && !CombatLog) {
				CombatLog = GameLog::getNewFile();
				if (!CombatLog)
					Fatal(0, " Couldn't create Combat Log ");
				long err = CombatLog->open("combat.log");
				if (err)
					Fatal(0, " Couldn't open Combat Log ");
			}
			if (initBugLog && !BugLog) {
				BugLog = GameLog::getNewFile();
				if (!BugLog)
					Fatal(0, " Couldn't create Bug Log ");
				long err = BugLog->open("bug.log");
				if (err)
					Fatal(0, " Couldn't open Bug Log ");
			}
			if (initLRMoveLog && !GlobalMap::logEnabled) {
				GlobalMap::toggleLog();
			}
		}
	
		//--------------------------------------------------------------
		// Read in Prefs.cfg
		{ ZoneScopedN("InitializeGameEngine prefs.cfg");
		bool fullScreen = false;
		FitIniFile prefsFile;
		FitIniFile optsFile;
	
		long prefsOpenResult = prefsFile.open("prefs.cfg");
	
		gosASSERT (prefsOpenResult == NO_ERR);
		
		prefsOpenResult = optsFile.open("options.cfg");
		
		gosASSERT (prefsOpenResult == NO_ERR);
		
		{
	#ifdef _DEBUG
			long prefsBlockResult = 
	#endif
				prefsFile.seekBlock("MechCommander2");
				optsFile.seekBlock("MechCommander2");
			gosASSERT(prefsBlockResult == NO_ERR);
			{
				/*The following commented out options have been moved to "options.cfg" and are
				handled by the CPrefs class (in "prefs.h"). They have been moved to CPrefs
				because these options are potentially affected by the options screen and may
				need to support "apply, cancel and save" functionality.
				
				Understood.  Game must also know about them know or it cannot start up correctly!!
				-fs
				*/
				
				// store volume settings in global variable since soundsystem 
				// does not exist yet.  These will be set in SoundSystem::init()
				long result = optsFile.readIdLong("DigitalMasterVolume",DigitalMasterVolume);
				if (result != NO_ERR)
					DigitalMasterVolume = 255;
	
				result = optsFile.readIdLong("MusicVolume",MusicVolume);
				if (result != NO_ERR)
					MusicVolume = 64;
	
				result = optsFile.readIdLong("RadioVolume",RadioVolume);
				if (result != NO_ERR)
					RadioVolume = 64;
	
				result = optsFile.readIdLong("SFXVolume",sfxVolume);
				if (result != NO_ERR)
					sfxVolume = 64;
	
				result = optsFile.readIdLong("BettyVolume",BettyVolume);
				if (result != NO_ERR)
					BettyVolume = 64;
	
				result = optsFile.readIdBoolean( "Shadows", useShadows);
				if (result != NO_ERR)
					useShadows = true;
	
				result = optsFile.readIdBoolean( "DetailTexture", useWaterInterestTexture);
				if (result != NO_ERR)
					useWaterInterestTexture = true;
	
				result = optsFile.readIdLong("Difficulty",GameDifficulty);
				if (result != NO_ERR)
					GameDifficulty = 1;
					
				result = optsFile.readIdBoolean("UnlimitedAmmo",useUnlimitedAmmo);
				if (result != NO_ERR)
					useUnlimitedAmmo = true;
	
				result = optsFile.readIdLong("Rasterizer",renderer);
				if (result != NO_ERR)
					renderer = 0;
	
				if ((renderer < 0) || (renderer > 3))
					renderer = 0;
	
				//result = optsFile->readIdLong("Resolution",resolution);
				//if (result != NO_ERR)
				//	resolution = 0;
                
				result = optsFile.readIdLong("ResolutionX",resolutionX);
				if (result != NO_ERR)
					resolutionX = 800;
                
				result = optsFile.readIdLong("ResolutionY",resolutionY);
				if (result != NO_ERR)
					resolutionY = 600;
	
				result = optsFile.readIdBoolean("FullScreen",fullScreen);
				if (result != NO_ERR)
					fullScreen = true;
	
				result = optsFile.readIdLong("Brightness",gammaLevel);
				if (result != NO_ERR)
					gammaLevel = 0;
	
				result = optsFile.readIdBoolean( "useLeftRightMouseProfile", useLeftRightMouseProfile );
				if ( result != NO_ERR )
					useLeftRightMouseProfile = true;
	
				bool asyncMouse = false;
				result = optsFile.readIdBoolean( "useAsyncMouse", asyncMouse );
				if ( result != NO_ERR )
					asyncMouse = false;
	
				mc2UseAsyncMouse = asyncMouse;
	
				long filterSetting;
				result = prefsFile.readIdLong("FilterState",filterSetting);
				if (result == NO_ERR)
				{
					switch (filterSetting)
					{
					default:
						case 0:
							FilterState = gos_FilterNone;
						break;
	
						case 1:
							FilterState = gos_FilterBiLinear;
						break;
	
						case 2:
							FilterState = gos_FilterTriLinear;
						break;
					}
				}
	
                long tmp;
				result = prefsFile.readIdLong("TerrainTextureRes", tmp);
				if (result != NO_ERR)
					TERRAIN_TXM_SIZE = 64;
                else {
                    TERRAIN_TXM_SIZE = static_cast<int>(tmp);
                }
	
				result = prefsFile.readIdLong("ObjectTextureRes", tmp);
				if (result != NO_ERR)
					ObjectTextureSize = 128;
                else {
                    ObjectTextureSize = tmp;
                }
	
				result = prefsFile.readIdFloat("DoubleClickThreshold",doubleClickThreshold);
				if (result != NO_ERR)
					doubleClickThreshold = 0.2f;
	
                // sebi??
				//result = prefsFile->readIdLong("DragThreshold",dragThreshold);
				result = prefsFile.readIdFloat("DragThreshold",dragThreshold);
				if (result != NO_ERR)
					dragThreshold = .01667f;
					
				result = prefsFile.readIdULong("BaseVertexColor",BaseVertexColor);
				if (result != NO_ERR)
					BaseVertexColor = 0x00000000;
					
				result = prefsFile.readIdBoolean("RealLOS",useRealLOS);
				if (result != NO_ERR)
					useRealLOS = true;
					
				result = prefsFile.readIdLong("GameVisibleVertices",GameVisibleVertices);
				if (result != NO_ERR)
					GameVisibleVertices = 500;
				GameVisibleVertices = 500;  // Override config — full visibility

				result = prefsFile.readIdFloat("MaxClipDistance",Camera::MaxClipDistance);
				if (result != NO_ERR)
					Camera::MaxClipDistance	= 50000.0f;
				Camera::MaxClipDistance = 50000.0f;  // Override config
					
				result = prefsFile.readIdFloat("MinHazeDistance",Camera::MinHazeDistance);
				if (result != NO_ERR)
					Camera::MinHazeDistance	= 2000.0f;
					
				result = prefsFile.readIdFloat("View0Zoom",Camera::cameraZoom[0]);
				if (result != NO_ERR)
					Camera::cameraZoom[0] = 1200.0f;
					
				result = prefsFile.readIdFloat("View0Tilt",Camera::cameraTilt[0]);
				if (result != NO_ERR)
					Camera::cameraTilt[0] = 35.0f;
					
				result = prefsFile.readIdFloat("View1Zoom",Camera::cameraZoom[1]);
				if (result != NO_ERR)
					Camera::cameraZoom[1] = 1200.0f;
					
				result = prefsFile.readIdFloat("View1Tilt",Camera::cameraTilt[1]);
				if (result != NO_ERR)
					Camera::cameraTilt[1] = 35.0f;
					
				result = prefsFile.readIdFloat("View2Zoom",Camera::cameraZoom[2]);
				if (result != NO_ERR)
					Camera::cameraZoom[2] = 1200.0f;
					
				result = prefsFile.readIdFloat("View2Tilt",Camera::cameraTilt[2]);
				if (result != NO_ERR)
					Camera::cameraTilt[2] = 35.0f;
					
				result = prefsFile.readIdFloat("View3Zoom",Camera::cameraZoom[3]);
				if (result != NO_ERR)
					Camera::cameraZoom[3] = 1200.0f;
					
				result = prefsFile.readIdFloat("View3Tilt",Camera::cameraTilt[3]);
				if (result != NO_ERR)
					Camera::cameraTilt[3] = 35.0f;
			}
		}
		
		prefsFile.close();
		optsFile.close();
		
		prefsFile.close();
		
		optsFile.close();
		
		//---------------------------------------------------------------------
		//void __stdcall gos_SetScreenMode( DWORD Width, DWORD Height, DWORD bitDepth=16, DWORD Device=0, bool disableZBuffer=0, bool AntiAlias=0, bool RenderToVram=0, bool GotoFullScreen=0, int DirtyRectangle=0, bool GotoWindowMode=0, bool EnableStencil=0, DWORD Renderer=0 );
	
		/*The following commented out code is now handled by CPrefs::applyPrefs()
		(in "prefs.cpp").*/
		/*
		switch (resolution)
		{
			case 0:			//640by480
				if (renderer == 3)
					gos_SetScreenMode(640,480,16,0,0,0,true,fullScreen,0,!fullScreen,0,renderer);
				else
					gos_SetScreenMode(640,480,16,0,0,0,0,fullScreen,0,!fullScreen,0,renderer);
				break;
	
			case 1:			//800by600
				if (renderer == 3)
					gos_SetScreenMode(800,600,16,0,0,0,true,fullScreen,0,!fullScreen,0,renderer);
				else
					gos_SetScreenMode(800,600,16,0,0,0,0,fullScreen,0,!fullScreen,0,renderer);
				break;
	
			case 2:			//1024by768
				if (renderer == 3)
					gos_SetScreenMode(1024,768,16,0,0,0,true,fullScreen,0,!fullScreen,0,renderer);
				else
					gos_SetScreenMode(1024,768,16,0,0,0,0,fullScreen,0,!fullScreen,0,renderer);
				break;
	
			case 3:			//1280by1024
				if (renderer == 3)
					gos_SetScreenMode(1280,1024,16,0,0,0,true,fullScreen,0,!fullScreen,0,renderer);
				else
					gos_SetScreenMode(1280,1024,16,0,0,0,0,fullScreen,0,!fullScreen,0,renderer);
				break;
	
			case 4:			//1600by1200
				if (renderer == 3)
					gos_SetScreenMode(1600,1200,16,0,0,0,true,fullScreen,0,!fullScreen,0,renderer);
				else
					gos_SetScreenMode(1600,1200,16,0,0,0,0,fullScreen,0,!fullScreen,0,renderer);
				break;
		}
		*/
	
		/* load and apply options from "options.cfg" */
		{ ZoneScopedN("InitializeGameEngine prefs.applyPrefs"); prefs.load(); prefs.applyPrefs(); ffmpegProbeAvailability(); }
		}
	
	
		//--------------------------------------------------
		// Setup Mouse Parameters from Prefs.CFG
		{ ZoneScopedN("InitializeGameEngine input");
		ownedUserInput = std::make_unique_for_overwrite<UserInput>();
		userInput = ownedUserInput.get();
		userInput->init();
		userInput->setMouseDoubleClickThreshold(doubleClickThreshold);
		userInput->setMouseDragThreshold(dragThreshold);
		}
		//--------------------------------------------------
	
		{ ZoneScopedN("InitializeGameEngine font");
		char temp[256];
		cLoadString( IDS_FLOAT_HELP_FONT, temp, 255 );
		char* pStr = strstr( temp, "," );
		if ( pStr )
		{
			gosFontScale = -atoi( pStr + 1 );
			*pStr = 0;
		}
		char path [256];
		strcpy( path, "assets" PATH_SEPARATOR "graphics" PATH_SEPARATOR );
		strcat( path, temp );	
	
		gosFontHandle = gos_LoadFont(path);
		FontHandle = gosFontHandle;
	
		ownedFloatHelp = std::make_unique<FloatHelp>(MAX_FLOAT_HELPS);
		globalFloatHelp = ownedFloatHelp.get();
		}
	
		//
		//----------------------------------
		// Start associated stuff.
		//----------------------------------
		
		//-------------------------------
		// Used to output debug stuff!
		// Mondo COOL!
		// simply do this in the code and stuff goes to the file called mc2.output
		//		DEBUG_STREAM << thing_you_want_to_output
		//
		// IMPORTANT NOTE:
		{ ZoneScopedN("InitializeGameEngine renderClasses");
		Stuff::InitializeClasses();
		MidLevelRenderer::InitializeClasses(8192*4,8192,0,0,true);
		gosFX::InitializeClasses();
		}
		
		{ ZoneScopedN("InitializeGameEngine MLR");
		gos_PushCurrentHeap(MidLevelRenderer::Heap);
	
		MidLevelRenderer::TGAFilePool *pool = new MidLevelRenderer::TGAFilePool("data" PATH_SEPARATOR "tgl" PATH_SEPARATOR "128" PATH_SEPARATOR);
		MidLevelRenderer::MLRTexturePool::Instance = new MidLevelRenderer::MLRTexturePool(pool);
	
		MidLevelRenderer::MLRSortByOrder *cameraSorter = new MidLevelRenderer::MLRSortByOrder(MidLevelRenderer::MLRTexturePool::Instance);
		theClipper = new MidLevelRenderer::MLRClipper(nullptr, cameraSorter);
		
		gos_PopCurrentHeap();
		}
	
		//------------------------------------------------------
		// Start the GOS FX.
		{ ZoneScopedN("InitializeGameEngine gosFX");
		gos_PushCurrentHeap(gosFX::Heap);
		
		gosFX::EffectLibrary::Instance = new gosFX::EffectLibrary();
		Check_Object(gosFX::EffectLibrary::Instance);
	
		FullPathFileName effectsName;
		effectsName.init(effectsPath,"mc2.fx","");
	
		File effectFile;
		long result = effectFile.open(effectsName);
		if (result != NO_ERR)
			STOP(("Could not find MC2.fx"));
			
		long effectsSize = effectFile.fileSize();
		MemoryPtr effectsData = static_cast<MemoryPtr>(std::malloc(effectsSize));
		effectFile.read(effectsData,effectsSize);
		effectFile.close();
		
		InstallEffectStream(effectsData, static_cast<size_t>(effectsSize));
		gosFX::EffectLibrary::Instance->Load(effectStream);

		// Authored effect overlays are optional; absent definitions leave the stock catalog intact.
		mc2fxdefs::EffectDefRegistry::instance().applyAll(mc2::particles::SpecLibrary::Instance());

		gosFX::LightManager::Instance = new gosFX::LightManager();
	
		gos_PopCurrentHeap();
	
		std::free(effectsData);
		}
		
	
		//--------------------------------------------------------------
		// Start the GUI Heap.
		{ ZoneScopedN("InitializeGameEngine guiHeap");
		
		}
		
		//------------------------------------------------
		// Initialize scene resources.
		{ ZoneScopedN("InitializeGameEngine textureManager");
		mc2::VkSceneRenderer::start();
		}
	
		//--------------------------------------------------------------
		// Load up the mouse cursors
		{ ZoneScopedN("InitializeGameEngine cursors");
		userInput->initMouseCursors("cursors");
		userInput->mouseOff();
		userInput->setMouseCursor(mState_NORMAL);
		}
	
		//------------------------------------------------
		// Give the Sound System a Whirl!
		{ ZoneScopedN("InitializeGameEngine sound");
		ownedSoundSystem = std::make_unique_for_overwrite<GameSoundSystem>();
		soundSystem = ownedSoundSystem.get();
		soundSystem->init();
		(static_cast<SoundSystem *>(soundSystem))->init("sound");
		}
		
		//-----------------------------------------------
		// Only used by camera to retrieve screen coords.
		{ ZoneScopedN("InitializeGameEngine paneWindow");
		ownedPane = std::make_unique_for_overwrite<PANE>();
		globalPane = ownedPane.get();
		ownedWindow = std::make_unique_for_overwrite<WINDOW>();
		globalWindow = ownedWindow.get();
	
		globalPane->window = globalWindow;
		globalPane->x0 = 0;
		globalPane->y0 = 0;
		globalPane->x1 = Environment.screenWidth;
		globalPane->y1 = Environment.screenHeight;
	
		globalWindow->buffer = nullptr;			//This is set at the start of Renders.  For now we HOLD LOCK during entire old 2D render test.  This will go away once we hit 3D
		globalWindow->shadow = nullptr;
		globalWindow->stencil = nullptr;
		globalWindow->x_max = globalPane->x1 - globalPane->x0 - 1;
		globalWindow->y_max = globalPane->y1 - globalPane->y0 - 1;
		}
	
		//---------------------------------------------------------
		// Start the Timers
		{ ZoneScopedN("InitializeGameEngine timers");
		ownedTimerManager = std::make_unique_for_overwrite<TimerManager>();
		timerManager = ownedTimerManager.get();
		timerManager->init();
		}
	
		//---------------------------------------------------------
		// Start the Color table code
		{ ZoneScopedN("InitializeGameEngine colorTables");
		initColorTables();
		}
				
		//---------------------------------------------------------
		// Start the Mission, Scenario and Logistics classes here
		{ ZoneScopedN("InitializeGameEngine missionLogistics");
		ownedMission = std::make_unique_for_overwrite<Mission>();
		mission = ownedMission.get();
		
		ownedLogistics = std::make_unique_for_overwrite<Logistics>();
		logistics = ownedLogistics.get();
		}
	
		{ ZoneScopedN("InitializeGameEngine debugWindow");
        const char* dbgfont = "assets" PATH_SEPARATOR "graphics" PATH_SEPARATOR "arial8.tga";
		GameDebugWindow::setFont(dbgfont);
		DEBUGWINS_init();
		}
	
		{ ZoneScopedN("InitializeGameEngine networking");
		StartupNetworking();
		}

#ifdef USE_movie
		movieSoundUseDirectSound(0);
#endif

		if (justStartMission)
		{
#ifdef _WIN32
			for (int arg = 1; arg < __argc; ++arg) {
				if (std::strcmp(__argv[arg], "--objective-logistics") == 0 &&
				    !logistics->initObjectiveFixture(missionName)) {
					gos_TerminateApplication();
					return;
				}
			}
#endif
			logistics->setLogisticsState(log_STARTMISSIONFROMCMDLINE);
			char commandersToLoad[MAX_MC_PLAYERS][3] = {{0, 0, 0}, {1, 1, 1}, {2, 0, 2}, {3, 3, 3}, {4, 4, 4}, {5, 5, 5}, {6, 6, 6}, {7, 7, 7}};
#ifdef _WIN32
			// Test hook (mc10 load crash repro): use the logistics identity table
			// {i,i,i} with entry 0 = -1, exactly as logistics_begin.cpp builds it.
			for (int arg = 1; arg < __argc; ++arg) {
				if (std::strcmp(__argv[arg], "--identity-commanders") == 0) {
					for (int i = 0; i < MAX_MC_PLAYERS; ++i)
						commandersToLoad[i][0] = commandersToLoad[i][1] = commandersToLoad[i][2] = static_cast<char>(i);
					commandersToLoad[0][0] = commandersToLoad[0][1] = commandersToLoad[0][2] = -1;
				}
			}
#endif
			mission->init(missionName, MISSION_LOAD_SP_QUICKSTART, 0, nullptr, commandersToLoad, 2);
			eye->activate();
			eye->update();
			mission->start();
		}
		else
		{
			int param = log_SPLASH;
			if ( MPlayer && MPlayer->launchedFromLobby )
				param = log_ZONE;
			{ ZoneScopedN("InitializeGameEngine logistics.start");
			logistics->start( param);				//Always start with logistics in Splash Screen Mode
			}
			{ ZoneScopedN("InitializeGameEngine Mission::initBareMinimum");
			Mission::initBareMinimum();
			}
		}
	
		{ ZoneScopedN("InitializeGameEngine initDialogs");
		initDialogs();
		}
	
		{ ZoneScopedN("InitializeGameEngine loseFocusBehavior");
		gos_EnableSetting(gos_Set_LoseFocusBehavior, 2 );
		}
	
		DWORD numJoysticks;
		{ ZoneScopedN("InitializeGameEngine joystickCount");
		numJoysticks = gosJoystick_CountJoysticks();
		}
	
		{ ZoneScopedN("InitializeGameEngine joystickInit");
		for (long i=0;i<numJoysticks;i++)
		{
			ZoneScopedN("InitializeGameEngine joystickProbe");
			gosJoystick_Info joyInfo;
			gosJoystick_GetInfo(i, &joyInfo);
			
			//Search for the Attila Strategic Commander here.
			// Look for the Attila VidPid 0x045e0033 (Unique!)
			// Andy G did not do what I asked for here.  Will do in our version when we branch.
			// For NOW, ANY 3 Axis joystick with 15 buttons is a Strategic Commander.
			if ((joyInfo.nAxes == 3) && (joyInfo.nButtons == 15))
			{
				userInput->addAttila(i);
				gosJoystick_SetPolling(i, true, 0.1f);
			}
		}
		}
	
		//Time BOMB goes here.
		// Set Date and write Binary data to registry under key
		// GraphicsData!!
		SYSTEMTIME bombDate;
		DWORD dataSize = sizeof(SYSTEMTIME);
		gos_LoadDataFromRegistry("GraphicsDataInit2", &bombDate, &dataSize);
		if (dataSize == 0)
		{
			bombDate.wYear = 2001;
			bombDate.wMonth = 6;
			bombDate.wDayOfWeek = 5;
			bombDate.wDay = 15;
			bombDate.wHour = 0;
			bombDate.wMinute = 0;
			bombDate.wSecond = 0;
			bombDate.wMilliseconds = 0;
		
			dataSize = sizeof(SYSTEMTIME);
			gos_SaveDataToRegistry("GraphicsDataInit2", &bombDate, dataSize);
		}

		gameStarted = true;
	}
	else
	{
		char msgBuffer[4096];
		char msgTitle[1024];

		cLoadString(IDS_SNIFFER_INIT_MSG,msgBuffer,4095);
		cLoadString(IDS_SNIFFER_INIT_TITLE,msgTitle,1023);

		mc2MessageBox(msgBuffer,msgTitle,MB_OK,IDOK);

		//-------------------------------------------------------------
		// Find the CDPath in the registry and save it off so I can
		// look in CD Install Path for files.
		
		//Changed for the shared source release, just set to current directory

		//DWORD maxPathLength = 1023;
		//gos_LoadDataFromRegistry("CDPath", CDInstallPath, &maxPathLength);
		//if (!maxPathLength)
		//	memset(CDInstallPath,0,1024);
		memset(CDInstallPath,0,1024);

		//Set ScreenMode to 800x600x16 FULLSCREEN!!!!
		// A window will return BAD DATA!!
		//
		// If this fails, GAME CANNOT RUN on their machine!!!!!
		//
		// Check which rasterizer comes up.  If software, tell prefs we cannot run in hardware!!
		//

		while (((gos_GetMachineInformation(gos_Info_GetDeviceLocalMemory, curDevice) +
				gos_GetMachineInformation(gos_Info_GetDeviceAGPMemory, curDevice)) < 6291456) && 
				(curDevice < gos_GetMachineInformation( gos_Info_NumberDevices )))
			curDevice++;

		if (curDevice < gos_GetMachineInformation( gos_Info_NumberDevices ))
			gos_SetScreenMode(Environment.screenWidth, Environment.screenHeight, 16, curDevice, 0, 0, 0,
			                  Environment.fullScreen ? 1 : 0, 0, Environment.fullScreen ? 0 : 1, 0, 0);

		//Create about a thousand textured random triangles.
		testVertex = static_cast<gos_VERTEX *>(malloc(sizeof(gos_VERTEX) * 3000));
		indexArray = static_cast<WORD *>(malloc(sizeof(WORD) * 3000));
		for (long i=0;i<3000;i++)
		{
			testVertex[i].x = RandomNumber(1000) - 100;
			testVertex[i].y = RandomNumber(800) - 100;
			testVertex[i].z = (static_cast<float>(RandomNumber(150)) / 100.0f) - 0.25f;
			testVertex[i].rhw = 0.5f;
			testVertex[i].u = static_cast<float>(RandomNumber(100)) / 100.0f;
			testVertex[i].v = static_cast<float>(RandomNumber(100)) / 100.0f;
			testVertex[i].argb = 0x3fffffff;
			testVertex[i].frgb = 0x3f1f2f3f;
			indexArray[i] = i;
		}

		testTextureHandle = gos_NewTextureFromFile(gos_Texture_Solid,"testtxm.tga");
	}

	//Make any directories we need which should be empty.
	CreateDirectory(savePath,nullptr);
	CreateDirectory(transcriptsPath,nullptr);

	//Startup the Office Watson Handler.
	InitDW();

    // sebi WTF? do we ned to do it for InitDW?
	//__asm pop esi;
}
