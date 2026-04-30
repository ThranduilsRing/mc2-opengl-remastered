/***************************************************************
* FILENAME: Editor.cpp
* DESCRIPTION: Implements core Editor startup, update, and subsystem coordination.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Removed Editor dependency on the legacy version header and routed runtime version through EditorVersion.
****************************************************************/

#include "stdafx.h"

#ifndef EDITORINTERFACE_H
#include "EditorInterface.h"
#endif

#include <GameOS.hpp>
#include <ToolOS.hpp>
#include <mlr\mlr.hpp>
#include <Stuff\stuff.hpp>
#include "EditorData.h"
#include "..\resource.h"


// ---------------------------------------------------------------------------
// Editor startup tracing. Writes to CWD\editor-startup.log.
// Keep this deliberately C stdio-only so it works before GameOS logging exists.
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <stdarg.h>

class gosRenderer;
gosRenderer* getGosRenderer();

static void EditorTrace(const char* fmt, ...)
{
	FILE* f = fopen("editor-startup.log", "a");
	if (!f)
		return;

	va_list args;
	va_start(args, fmt);
	vfprintf(f, fmt, args);
	va_end(args);

	fprintf(f, "\n");
	fclose(f);
}

static void EditorTraceReset()
{
	// Append a separator rather than wiping the file.  InitInstance and OnIdle write
	// traces before GetGameOSEnvironment is called; opening with "w" here destroys them
	// and makes it impossible to diagnose MFC startup failures.
	FILE* f = fopen("editor-startup.log", "a");
	if (f)
	{
		fprintf(f, "--- GetGameOSEnvironment ---\n");
		fclose(f);
	}
}

/* begin jubilee stuff */
//#include "wlib.h"
#include "resource.h"
#include "EditorResourceFallback.h"
#include "EditorVersion.h"
//#include "d3dfont.h"
extern void InitWLib(void);
/* end jubilee stuff */


// globals used for memory
UserHeapPtr systemHeap = NULL;
UserHeapPtr guiHeap = NULL;

float MaxMinUV = 8.0f;

Stuff::MemoryStream *effectStream = NULL;
static MemoryPtr effectsData = NULL;  // kept alive until effectStream is deleted at shutdown

unsigned long systemHeapSize = 8192000;
unsigned long guiHeapSize = 1023999;
unsigned long tglHeapSize = 65536000;

DWORD BaseVertexColor = 0x00000000;		//This color is applied to all vertices in game as Brightness correction.

long gammaLevel = 0;
bool hasGuardBand = false;
extern long terrainLineChanged; // the terrain uses this
//extern float frameNum;	// tiny geometry needs this
extern float frameRate; // tiny geometry needs this

extern bool InEditor;
extern char FileMissingString[];
extern char CDMissingString[];
extern char MissingTitleString[];

extern char CDInstallPath[];

HSTRRES gosResourceHandle = 0;
HGOSFONT3D gosFontHandle = 0;
float gosFontScale = 1.0;
FloatHelpPtr globalFloatHelp = NULL;
unsigned long currentFloatHelp = 0;
extern float CliffTerrainAngle;

extern bool gNoDialogs;

char* ExceptionGameMsg = NULL; // some debugging thing I think

bool quitGame = FALSE;

bool gamePaused = FALSE;

bool reloadBounds = false;
bool justResaveAllMaps = false;

extern bool forceShadowBurnIn;
// these globals are necessary for fast files for some reason
FastFile 	**fastFiles = NULL;
long 		numFastFiles = 0;
long		maxFastFiles = 0;

#define MAX_SHAPES	0

										//Heidi, turn this FALSE to turn Fog of War ON!
extern unsigned char godMode;			//Can I simply see everything, enemy and friendly?

void InitDW (void);

TimerManagerPtr timerManager = NULL;

long FilterState = gos_FilterNone;

extern int TERRAIN_TXM_SIZE;
int ObjectTextureSize = 128;

Editor* editor = NULL;

char missionName[1024] = "\0";

enum { CPU_UNKNOWN, CPU_PENTIUM, CPU_MMX, CPU_KATMAI } Processor = CPU_PENTIUM;		//Needs to be set when GameOS supports ProcessorID -- MECHCMDR2

MidLevelRenderer::MLRClipper *  theClipper = NULL;

// called by gos
//---------------------------------------------------------------------------
char* GetGameInformation() 
{
	return(ExceptionGameMsg);
}

// called by GOS when you need to draw
//---------------------------------------------------------------------------
//define RUNNING_REMOTELY
#ifdef RUNNING_REMOTELY
#include <windows.h> /* for declaration of Sleep() */
#endif /*RUNNING_REMOTELY*/
void UpdateRenderers()
{
#ifdef RUNNING_REMOTELY
	Sleep(0.25/*seconds*/ * 1000.0); /* limit the frame rate when displaying on remote console */
#endif /*RUNNING_REMOTELY*/
	hasGuardBand = true;

	DWORD bColor = 0x0;
	if (eye)
		bColor = eye->fogColor;

 	gos_SetupViewport(1,1.0,1,bColor, 0.0, 0.0, 1.0, 1.0 );		//ALWAYS FULL SCREEN for now

	gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear );
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );

	gos_SetRenderState( gos_State_AlphaTest, TRUE );

	gos_SetRenderState( gos_State_Clipping, TRUE);

	gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );

	gos_SetRenderState( gos_State_Dither, 1);
	
	float viewMulX, viewMulY, viewAddX, viewAddY;

	gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);
			
	//------------------------------------------------------------
	gos_SetRenderState( gos_State_Filter, gos_FilterNone );

	// Guard against partial init (e.g. mc2.fx missing caused InitializeGameEngine
	// to return early, leaving editor and globalFloatHelp as NULL).
	if (!editor || !globalFloatHelp)
	{
		EditorTrace("UpdateRenderers: skipping render -- editor=%p globalFloatHelp=%p not initialized", editor, globalFloatHelp);
		return;
	}

	editor->render();

	globalFloatHelp->renderAll();

	turn++;

	reloadBounds = false;

   	//reset the TGL RAM pools.
   	colorPool->reset();
   	vertexPool->reset();
   	facePool->reset();
   	shadowPool->reset();
   	trianglePool->reset();
}

bool statisticsInitialized = false;
//------------------------------------------------------------
void DoGameLogic()
{
	EditorTrace("DoGameLogic: enter editor=%p globalFloatHelp=%p timerManager=%p globalHeapList=%p land=%p userInput=%p turn=%ld",
		editor, globalFloatHelp, timerManager, globalHeapList, land, userInput, turn);

	EditorTrace("DoGameLogic: before globalFloatHelp->resetAll");
	if (globalFloatHelp)
		globalFloatHelp->resetAll();
	EditorTrace("DoGameLogic: after globalFloatHelp->resetAll");

	//-------------------------------------
	// Get me the current frameRate.
	// Convert to frameLength and any other timing stuff.
	if (frameRate < 4.0)
		frameRate = 4.0;

	frameLength = 1.0f / frameRate;

	bool doTransformMath = true;


	if (doTransformMath)
	{
		//-------------------------------------
		// Poll devices for this frame.

		EditorTrace("DoGameLogic: before editor->update");
		if (editor)
			editor->update();
		EditorTrace("DoGameLogic: after editor->update");

		//----------------------------------------
		// Update all of the timers
		EditorTrace("DoGameLogic: before timerManager->update");
		if (timerManager)
			timerManager->update();
		EditorTrace("DoGameLogic: after timerManager->update");
	}


	//---------------------------------------------------
	// Update heap instrumentation.
	EditorTrace("DoGameLogic: before globalHeapList->update");
	if (globalHeapList)
		globalHeapList->update();
	EditorTrace("DoGameLogic: after globalHeapList->update");

	if (justResaveAllMaps)
	{
		EditorTrace("DoGameLogic: justResaveAllMaps quit");
		PostQuitMessage(0);
		quitGame = true;		//We just needed to resave the maps.  Exit immediately
	}
		
	//---------------------------------------------------------------
	// Somewhere in all of the updates, we have asked to be excused!
	if (quitGame)
	{
		EditorTrace("DoGameLogic: quitGame terminating");
		//EnterWindowMode();				//Game crashes if _TerminateApp called from fullScreen
		gos_TerminateApplication();
	}

	EditorTrace("DoGameLogic: exit");
}

//---------------------------------------------------------------------------
void InitializeGameEngine()
{
	EditorTrace("InitializeGameEngine: enter");
	InEditor = true;

	EditorTrace("InitializeGameEngine: opening MC2 resource DLL");
	if (fileExists("mc2resUS.dll"))
		gosResourceHandle = gos_OpenResourceDLL("mc2resUS.dll", NULL, 0);
	else
		gosResourceHandle = gos_OpenResourceDLL("mc2res.dll", NULL, 0);
	EditorTrace("InitializeGameEngine: gosResourceHandle=%p", gosResourceHandle);

	EditorTrace("InitializeGameEngine: opening editores.dll");
	gameResourceHandle = gos_OpenResourceDLL("editores.dll", NULL, 0);
	EditorTrace("InitializeGameEngine: gameResourceHandle=%p", gameResourceHandle);

	EditorTrace("InitializeGameEngine: loading editor fallback strings");

	/*
		Editor migration patch:

		The original editor uses EditorSafeLoadString(), which routes into the legacy
		GameOS resource-string layer. In Remastered, that resource layer is not
		initialized the same way during editor startup, and EditorSafeLoadString() reaches
		legacy resource lookup() with a null resource table.

		Do not patch Remastered's GameOS resource system for this. The editor
		only needs these strings as user-facing fallbacks during init, so keep the
		compatibility fix local to Editor.cpp.
	*/
	strncpy(FileMissingString, "Required file is missing.", 511);
	FileMissingString[511] = 0;

	strncpy(CDMissingString, "Required MechCommander 2 data is missing.", 1023);
	CDMissingString[1023] = 0;

	strncpy(MissingTitleString, "MC2 Editor", 255);
	MissingTitleString[255] = 0;

	char temp[256];
	strcpy(temp, "Arial, 8");
	char* pStr = strstr( temp, "," );
	if ( pStr )
	{
		gosFontScale = atoi( pStr + 2 );
		*pStr = 0;
	}
	char path[256];
	strcpy(path, "assets\\graphics\\arial8.bmp");
	EditorTrace("InitializeGameEngine: font extension from IDS_FLOAT_HELP_FONT ignored for GameOS bitmap/glyph loader: %s", temp);

	EditorTrace("InitializeGameEngine: loading font %s", path);
	gosRenderer* renderer = getGosRenderer();
	EditorTrace("InitializeGameEngine: renderer before font load=%p", renderer);
	if (renderer)
	{
		gosFontHandle = gos_LoadFont(path);
		EditorTrace("InitializeGameEngine: gosFontHandle=%lu", gosFontHandle);
	}
	else
	{
		gosFontHandle = 0;
		EditorTrace("InitializeGameEngine: skipping font load because renderer is NULL");
	}
	EditorTrace("InitializeGameEngine: after font load/skip gosFontHandle=%lu", gosFontHandle);

	// Legacy Voodoo3 hardware restriction removed for modern builds.
	EditorTrace("InitializeGameEngine: skipping legacy Voodoo3 machine information check");

   	//-------------------------------------------------------------
   	// Find the CDPath in the registry and save it off so I can
   	// look in CD Install Path for files.
	//Changed for the shared source release, just set to current directory
	//DWORD maxPathLength = 1023;
	//gos_LoadDataFromRegistry("CDPath", CDInstallPath, &maxPathLength);
	//if (!maxPathLength)
	//	strcpy(CDInstallPath,"..\\");
	strcpy(CDInstallPath,".\\");
	EditorTrace("InitializeGameEngine: CDInstallPath initialized to %s", CDInstallPath);

	long lastCharacter = (long)strlen(CDInstallPath)-1;
	if (CDInstallPath[lastCharacter] != '\\')
	{
		CDInstallPath[lastCharacter+1] = '\\';
		CDInstallPath[lastCharacter+2] = 0;
	}

	//--------------------------------------------------------------
	// Start the SystemHeap and globalHeapList
	EditorTrace("InitializeGameEngine: before globalHeapList new");
	globalHeapList = new HeapList;
	EditorTrace("InitializeGameEngine: after globalHeapList new ptr=%p", globalHeapList);
	gosASSERT(globalHeapList != NULL);

	EditorTrace("InitializeGameEngine: before globalHeapList init");
	globalHeapList->init();
	EditorTrace("InitializeGameEngine: after globalHeapList init");
	EditorTrace("InitializeGameEngine: before globalHeapList update");
	globalHeapList->update();		//Run Instrumentation into GOS Debugger Screen
	EditorTrace("InitializeGameEngine: after globalHeapList update");

	EditorTrace("InitializeGameEngine: before systemHeap new");
	systemHeap = new UserHeap;
	EditorTrace("InitializeGameEngine: after systemHeap new ptr=%p", systemHeap);
	gosASSERT(systemHeap != NULL);

	EditorTrace("InitializeGameEngine: before systemHeap init size=%lu", systemHeapSize);
	systemHeap->init(systemHeapSize,"SYSTEM");
	EditorTrace("InitializeGameEngine: after systemHeap init");

	EditorTrace("InitializeGameEngine: before Stuff::InitializeClasses");
	Stuff::InitializeClasses();
	EditorTrace("InitializeGameEngine: after Stuff::InitializeClasses");
	EditorTrace("InitializeGameEngine: before MidLevelRenderer::InitializeClasses");
	MidLevelRenderer::InitializeClasses(1024);
	EditorTrace("InitializeGameEngine: after MidLevelRenderer::InitializeClasses");
	EditorTrace("InitializeGameEngine: before gosFX::InitializeClasses");
	gosFX::InitializeClasses();
	EditorTrace("InitializeGameEngine: after gosFX::InitializeClasses");
	
	EditorTrace("InitializeGameEngine: before push MLR heap");
	gos_PushCurrentHeap(MidLevelRenderer::Heap);
	EditorTrace("InitializeGameEngine: after push MLR heap");

	EditorTrace("InitializeGameEngine: before TGAFilePool data\\Effects\\");
	MidLevelRenderer::TGAFilePool *pool = new MidLevelRenderer::TGAFilePool("data\\Effects\\");
	EditorTrace("InitializeGameEngine: after TGAFilePool ptr=%p", pool);
	EditorTrace("InitializeGameEngine: before MLRTexturePool");
	MidLevelRenderer::MLRTexturePool::Instance = new MidLevelRenderer::MLRTexturePool(pool);
	EditorTrace("InitializeGameEngine: after MLRTexturePool ptr=%p", MidLevelRenderer::MLRTexturePool::Instance);

	EditorTrace("InitializeGameEngine: before MLRSortByOrder");
	MidLevelRenderer::MLRSortByOrder *cameraSorter = new MidLevelRenderer::MLRSortByOrder(MidLevelRenderer::MLRTexturePool::Instance);
	EditorTrace("InitializeGameEngine: after MLRSortByOrder ptr=%p", cameraSorter);
	EditorTrace("InitializeGameEngine: before MLRClipper");
	theClipper = new MidLevelRenderer::MLRClipper(0, cameraSorter);
	EditorTrace("InitializeGameEngine: after MLRClipper ptr=%p", theClipper);
	
	EditorTrace("InitializeGameEngine: before pop MLR heap");
	gos_PopCurrentHeap();
	EditorTrace("InitializeGameEngine: after pop MLR heap");
	
	//------------------------------------------------------
	// Start the GOS FX.
	EditorTrace("InitializeGameEngine: before push gosFX heap");
	gos_PushCurrentHeap(gosFX::Heap);
	EditorTrace("InitializeGameEngine: after push gosFX heap");
	
	EditorTrace("InitializeGameEngine: before EffectLibrary new");
	gosFX::EffectLibrary::Instance = new gosFX::EffectLibrary();
	EditorTrace("InitializeGameEngine: after EffectLibrary ptr=%p", gosFX::EffectLibrary::Instance);
	Check_Object(gosFX::EffectLibrary::Instance);

	FullPathFileName effectsName;
	effectsName.init(effectsPath,"mc2.fx","");

	File effectFile;
	EditorTrace("InitializeGameEngine: before effectFile.open mc2.fx path=%s", (const char*)effectsName);
	long result = effectFile.open(effectsName);
	EditorTrace("InitializeGameEngine: after effectFile.open result=%ld", result);
	if (result != NO_ERR)
	{
		// STOP() may be a no-op in some build configs -- use a fatal message box so
		// the user sees the error regardless, then return to avoid the crash.
		STOP(("Could not find MC2.fx at path: %s (open result=%ld)", (const char*)effectsName, result));
		EditorTrace("InitializeGameEngine: FATAL mc2.fx open failed result=%ld path=%s -- aborting load", result, (const char*)effectsName);
		return;
	}

	long effectsSize = effectFile.fileSize();
	EditorTrace("InitializeGameEngine: mc2.fx effectsSize=%ld", effectsSize);
	if (effectsSize <= 0)
	{
		STOP(("MC2.fx is empty (size=%ld) at path: %s", effectsSize, (const char*)effectsName));
		EditorTrace("InitializeGameEngine: FATAL mc2.fx size=%ld -- aborting load", effectsSize);
		effectFile.close();
		return;
	}

	// effectsData is a global so it outlives effectStream (freed at shutdown alongside it).
	effectsData = (MemoryPtr)systemHeap->Malloc(effectsSize);
	effectFile.read(effectsData,effectsSize);
	effectFile.close();

	effectStream = new Stuff::MemoryStream(effectsData,effectsSize);
	EditorTrace("InitializeGameEngine: before EffectLibrary Load");
	gosFX::EffectLibrary::Instance->Load(effectStream);
	EditorTrace("InitializeGameEngine: after EffectLibrary Load");

	EditorTrace("InitializeGameEngine: before LightManager new");
	gosFX::LightManager::Instance = new gosFX::LightManager();
	EditorTrace("InitializeGameEngine: after LightManager ptr=%p", gosFX::LightManager::Instance);

	gos_PopCurrentHeap();

	// effectsData is intentionally NOT freed here -- effectStream holds a raw pointer
	// into it and lives until TerminateGameEngine. Freed there after effectStream is deleted.

	EditorTrace("InitializeGameEngine: before FloatHelp new");
	globalFloatHelp = new FloatHelp(MAX_FLOAT_HELPS);
	EditorTrace("InitializeGameEngine: after FloatHelp ptr=%p", globalFloatHelp);

	if (!gosFontHandle)
	{
		EditorTrace("InitializeGameEngine: FloatHelp created with gosFontHandle=0; skipping font-dependent setup");
	}
	else
	{
		EditorTrace("InitializeGameEngine: FloatHelp font handle available=%lu", gosFontHandle);
	}

	//---------------------------------------------------------------------
	float doubleClickThreshold = 0.2f;
	long dragThreshold = 10;
	EditorTrace("InitializeGameEngine: default thresholds doubleClick=%f drag=%ld", doubleClickThreshold, dragThreshold);

	// Editor frame update path calls Camera::update(), which uses userInput->setViewport().
	// The editor did not allocate userInput on this path, so the first DoGameLogic frame
	// can dereference NULL after the frame loop is restored.
	EditorTrace("InitializeGameEngine: before userInput init ptr=%p", userInput);
	if (!userInput)
	{
		userInput = new UserInput;
		userInput->init();
		userInput->setMouseDoubleClickThreshold(doubleClickThreshold);
		userInput->setMouseDragThreshold((float)dragThreshold);
	}
	EditorTrace("InitializeGameEngine: after userInput init ptr=%p", userInput);

	//--------------------------------------------------------------
	// Read in System.CFG
	EditorTrace("InitializeGameEngine: before System.CFG FitIniFile new");
	FitIniFilePtr systemFile = new FitIniFile;
	EditorTrace("InitializeGameEngine: after System.CFG FitIniFile new ptr=%p", systemFile);

#ifdef _DEBUG
	long systemOpenResult = 
#endif
	{
		EditorTrace("InitializeGameEngine: before system.cfg open");
		long tracedSystemOpenResult = systemFile->open("system.cfg");
		EditorTrace("InitializeGameEngine: after system.cfg open result=%ld", tracedSystemOpenResult);
	EditorTrace("InitializeGameEngine: skipping system.cfg reads for debug");
#ifdef _DEBUG
		systemOpenResult = tracedSystemOpenResult;
#endif
	}
		   
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
		systemFile->seekBlock("systemHeap");
		gosASSERT(systemBlockResult == NO_ERR);
		{
			long result = systemFile->readIdULong("systemHeapSize",systemHeapSize);
			gosASSERT(result == NO_ERR);
		}

#ifdef _DEBUG
		long systemPathResult = 
#endif
		systemFile->seekBlock("systemPaths");
		gosASSERT(systemPathResult == NO_ERR);
		{
			long result = systemFile->readIdString("terrainPath",terrainPath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("artPath",artPath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("fontPath",fontPath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("savePath",savePath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("spritePath",spritePath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("shapesPath",shapesPath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("soundPath",soundPath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("objectPath",objectPath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("cameraPath",cameraPath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("tilePath",tilePath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("missionPath",missionPath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("warriorPath",warriorPath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("profilePath",profilePath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("interfacepath",interfacePath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("moviepath",moviePath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("CDsoundPath",CDsoundPath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("CDmoviepath",CDmoviePath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("tglPath",tglPath,79);
			gosASSERT(result == NO_ERR);

			result = systemFile->readIdString("texturePath",texturePath,79);
			gosASSERT(result == NO_ERR);
		}

#ifdef _DEBUG
		long fastFileResult = 
#endif
		systemFile->seekBlock("FastFiles");
		gosASSERT(fastFileResult == NO_ERR);
		{
			long result = systemFile->readIdLong("NumFastFiles",maxFastFiles);
			if (result != NO_ERR)
				maxFastFiles = 0;

			if (maxFastFiles)
			{
				fastFiles = (FastFile **)malloc(maxFastFiles*sizeof(FastFile *));
				memset(fastFiles,0,maxFastFiles*sizeof(FastFile *));

				long fileNum = 0;
				char fastFileId[10];
				char fileName[100];
				sprintf(fastFileId,"File%d",fileNum);
	
				while (systemFile->readIdString(fastFileId,fileName,99) == NO_ERR)
				{
					bool result = FastFileInit(fileName);
					if (!result)
						STOP(("Unable to startup fastfiles.  Probably an old one in the directory!!"));

					fileNum++;
					sprintf(fastFileId,"File%d",fileNum);
				}
			}
		}

		result = systemFile->seekBlock("CameraSettings");
		if (result == NO_ERR)
		{
			result = systemFile->readIdFloat("MaxPerspective",Camera::MAX_PERSPECTIVE);
			if (result != NO_ERR)
				Camera::MAX_PERSPECTIVE = 88.0f;

			if (Camera::MAX_PERSPECTIVE > 90.0f)
				Camera::MAX_PERSPECTIVE = 90.0f;

			result = systemFile->readIdFloat("MinPerspective",Camera::MIN_PERSPECTIVE);
			if (result != NO_ERR)
				Camera::MIN_PERSPECTIVE = 18.0f;

			if (Camera::MIN_PERSPECTIVE < 0.0f)
				Camera::MIN_PERSPECTIVE = 0.0f;

			result = systemFile->readIdFloat("MaxOrtho",Camera::MAX_ORTHO);
			if (result != NO_ERR)
				Camera::MAX_ORTHO = 88.0f;

			if (Camera::MAX_ORTHO > 90.0f)
				Camera::MAX_ORTHO = 90.0f;

			result = systemFile->readIdFloat("MinOrtho",Camera::MIN_ORTHO);
			if (result != NO_ERR)
				Camera::MIN_ORTHO = 18.0f;

			if (Camera::MIN_ORTHO < 0.0f)
				Camera::MIN_ORTHO = 0.0f;

			result = systemFile->readIdFloat("AltitudeMinimum",Camera::AltitudeMinimum);
			if (result != NO_ERR)
				Camera::AltitudeMinimum = 560.0f;

			if (Camera::AltitudeMinimum < 110.0f)
				Camera::AltitudeMinimum = 110.0f;

			result = systemFile->readIdFloat("AltitudeMaximumHi",Camera::AltitudeMaximumHi);
			if (result != NO_ERR)
				Camera::AltitudeMaximumHi = 1600.0f;

			result = systemFile->readIdFloat("AltitudeMaximumLo",Camera::AltitudeMaximumLo);
			if (result != NO_ERR)
				Camera::AltitudeMaximumHi = 1500.0f;
		}
	}

	systemFile->close();
	delete systemFile;
	systemFile = NULL;

	//--------------------------------------------------------------
	// Read in Prefs.cfg
	FitIniFilePtr prefs = new FitIniFile;

	Environment.Key_Exit= (unsigned long)-1;


#ifdef _DEBUG
	long prefsOpenResult = 
#endif
		prefs->open("prefs.cfg");

	gosASSERT (prefsOpenResult == NO_ERR);
	{
#ifdef _DEBUG
		long prefsBlockResult = 
#endif
		
		prefs->seekBlock("MechCommander2Editor");
		gosASSERT(prefsBlockResult == NO_ERR);
		{
			long result = prefs->readIdFloat("CliffTerrainAngle",CliffTerrainAngle);
			if (result != NO_ERR)
				CliffTerrainAngle = 45.0f;
		}

		prefs->seekBlock("MechCommander2");
		gosASSERT(prefsBlockResult == NO_ERR);
		{
			Environment.Key_SwitchMonitors = 0;
			Environment.Key_FullScreen = 0;

			long bitD = 0;
			long result = prefs->readIdLong("BitDepth",bitD);
			if (result != NO_ERR)
				Environment.bitDepth				= 16;
			else if (bitD == 32)
				Environment.bitDepth				= 32;
			else
				Environment.bitDepth				= 16;

			//Editor can't draw fullscreen!
//			bool fullScreen;
//			result = prefs->readIdBoolean("FullScreen",	fullScreen);
//			if (result != NO_ERR)
//				Environment.fullScreen = 1;
//			else
//				Environment.fullScreen = (fullScreen == TRUE);

			long fullScreenCard;
			result = prefs->readIdLong("FullScreenCard",fullScreenCard);
			if (result != NO_ERR)
				Environment.FullScreenDevice		= 0;
			else
				Environment.FullScreenDevice		= fullScreenCard;

			long rasterizer;
			result = prefs->readIdLong("Rasterizer", rasterizer);
			if (result != NO_ERR)
				Environment.Renderer				= 0;
			else
				Environment.Renderer				= rasterizer;

			long filterSetting;
			result = prefs->readIdLong("FilterState",filterSetting);
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

			long terrainTextureRes = TERRAIN_TXM_SIZE;
			result = prefs->readIdLong("TerrainTextureRes", terrainTextureRes);
			if (result != NO_ERR)
				TERRAIN_TXM_SIZE = 64;
			else
				TERRAIN_TXM_SIZE = terrainTextureRes;

			long objectTextureRes = ObjectTextureSize;
			result = prefs->readIdLong("ObjectTextureRes", objectTextureRes);
			if (result != NO_ERR)
				ObjectTextureSize = 128;
			else
				ObjectTextureSize = objectTextureRes;

			result = prefs->readIdLong("Brightness",gammaLevel);
			if (result != NO_ERR)
				gammaLevel = 0;

			result = prefs->readIdFloat("DoubleClickThreshold",doubleClickThreshold);
			if (result != NO_ERR)
				doubleClickThreshold = 0.2f;

			result = prefs->readIdLong("DragThreshold",dragThreshold);
			if (result != NO_ERR)
				dragThreshold = 10;

			result = prefs->readIdULong("BaseVertexColor",BaseVertexColor);
			if (result != NO_ERR)
				BaseVertexColor = 0x00000000;
		}
	}
	
	prefs->close();
	
	delete prefs;
	prefs = NULL;
	//---------------------------------------------------------------------

	//--------------------------------------------------
	// Setup Mouse Parameters from Prefs.CFG
	//--------------------------------------------------

 	//---------------------------------------------------------
	// Start the Tiny Geometry Layer Heap.
	EditorTrace("InitializeGameEngine: before TGL heap new");
	TG_Shape::tglHeap = new UserHeap;
	EditorTrace("InitializeGameEngine: after TGL heap new ptr=%p", TG_Shape::tglHeap);
	EditorTrace("InitializeGameEngine: before TGL heap init size=%lu", tglHeapSize);
	TG_Shape::tglHeap->init(tglHeapSize,"TinyGeom");
	EditorTrace("InitializeGameEngine: after TGL heap init");

		//Start up the TGL RAM pools.
		EditorTrace("InitializeGameEngine: before colorPool new/init");
		colorPool 		= new TG_VertexPool;
		EditorTrace("InitializeGameEngine: colorPool ptr=%p", colorPool);
		colorPool->init(20000);
		EditorTrace("InitializeGameEngine: after colorPool init");
		
		EditorTrace("InitializeGameEngine: before vertexPool new/init");
		vertexPool 		= new TG_GOSVertexPool;
		EditorTrace("InitializeGameEngine: vertexPool ptr=%p", vertexPool);
		vertexPool->init(20000);
		EditorTrace("InitializeGameEngine: after vertexPool init");
		
		EditorTrace("InitializeGameEngine: before facePool new/init");
		facePool 		= new TG_DWORDPool;
		EditorTrace("InitializeGameEngine: facePool ptr=%p", facePool);
		facePool->init(40000);
		EditorTrace("InitializeGameEngine: after facePool init");
		
		EditorTrace("InitializeGameEngine: before shadowPool new/init");
		shadowPool 		= new TG_ShadowPool;
		EditorTrace("InitializeGameEngine: shadowPool ptr=%p", shadowPool);
		shadowPool->init(20000);
		EditorTrace("InitializeGameEngine: after shadowPool init");
		
		EditorTrace("InitializeGameEngine: before trianglePool new/init");
		trianglePool 	= new TG_TrianglePool;
		EditorTrace("InitializeGameEngine: trianglePool ptr=%p", trianglePool);
		trianglePool->init(20000);
		EditorTrace("InitializeGameEngine: after trianglePool init");

	//--------------------------------------------------------------
	
	//------------------------------------------------
	// Fire up the MC Texture Manager.
	EditorTrace("InitializeGameEngine: before MC_TextureManager new");
	mcTextureManager = new MC_TextureManager;
	EditorTrace("InitializeGameEngine: after MC_TextureManager new ptr=%p", mcTextureManager);

	EditorTrace("InitializeGameEngine: before mcTextureManager start");
	if (getGosRenderer())
	{
		mcTextureManager->start();
		EditorTrace("InitializeGameEngine: after mcTextureManager start");
	}
	else
	{
		EditorTrace("InitializeGameEngine: skipping mcTextureManager start because renderer is NULL");
	}

	//Startup the vertex array pool
	EditorTrace("InitializeGameEngine: before mcTextureManager startVertices");
	mcTextureManager->startVertices(500000);
	EditorTrace("InitializeGameEngine: after mcTextureManager startVertices");

	// clearArrays() resets both the vertex and render-shape managers.
	// The game initializes rsManager through startShapes(); the editor must do the same
	// before the restored DoGameLogic/update path can safely run.
	EditorTrace("InitializeGameEngine: before mcTextureManager startShapes");
	mcTextureManager->startShapes(10000);
	EditorTrace("InitializeGameEngine: after mcTextureManager startShapes");

	//--------------------------------------------------------------
	// Load up the mouse cursors
	godMode = true;
	EditorTrace("InitializeGameEngine: before MOVE_init");
	MOVE_init(30);
	EditorTrace("InitializeGameEngine: after MOVE_init");

	//--------------------------
	// Create and Load the master Effects File
	EditorTrace("InitializeGameEngine: before WeaponEffects new");
	weaponEffects = new WeaponEffects;
	EditorTrace("InitializeGameEngine: after WeaponEffects new ptr=%p", weaponEffects);
	EditorTrace("InitializeGameEngine: before WeaponEffects init");
	weaponEffects->init("Effects");
	EditorTrace("InitializeGameEngine: after WeaponEffects init");

	EditorTrace("InitializeGameEngine: creating Editor");
editor = new Editor();
EditorTrace("InitializeGameEngine: after Editor new ptr=%p", editor);

if (justResaveAllMaps)
{
    EditorTrace("InitializeGameEngine: before editor->resaveAll");
    editor->resaveAll();
    EditorTrace("InitializeGameEngine: after editor->resaveAll");
}

{
    EditorTrace("InitializeGameEngine: before editorGui path init");

    FullPathFileName guiloader;
    guiloader.init(artPath, "editorGui", ".fit");

    EditorTrace("InitializeGameEngine: editorGui path=%s", (const char*)guiloader);

    FitIniFile loader;
    EditorTrace("InitializeGameEngine: before editorGui open");
    long result = loader.open(guiloader);
    EditorTrace("InitializeGameEngine: after editorGui open result=%ld", result);

    if (result == NO_ERR)
    {
        EditorTrace("InitializeGameEngine: before editor->init");

if (getGosRenderer())
{
    editor->init(const_cast<char*>((const char*)guiloader));
    EditorTrace("InitializeGameEngine: after editor->init");
}
else
{
    EditorTrace("InitializeGameEngine: skipping editor->init because renderer is NULL");
}
    }
    else
    {
        EditorTrace("InitializeGameEngine: skipping editor->init because editorGui open failed");
    }
}
	//---------------------------------------------------------
	// Start the Timers
	EditorTrace("InitializeGameEngine: TimerManager init");
	timerManager = new TimerManager;
	timerManager->init();

	//---------------------------------------------------------
	// Start the Color table code
	EditorTrace("InitializeGameEngine: initColorTables");
	initColorTables();

	if (!statisticsInitialized)
	{
		statisticsInitialized = true;
		HeapList::initializeStatistics();
	}

	//Startup the Office Watson Handler.
	EditorTrace("InitializeGameEngine: calling InitDW");
	InitDW();
	EditorTrace("InitializeGameEngine: complete");
}

bool alreadyTermed = false;
void TerminateGameEngine()
{
	EditorTrace("TerminateGameEngine: enter");
	if (alreadyTermed)
		return;
	else
		alreadyTermed = true;

	//---------------------------------------------------------
	// End the Color table code
	destroyColorTables();

	//---------------------------------------------------------
	// End the Timers
	delete timerManager;
	timerManager = NULL;

	if ( editor )
		delete editor;
	editor = NULL;

	//--------------------------
	// master Effects File
	if (weaponEffects)
		delete weaponEffects;
	weaponEffects = NULL;

	if (userInput)
	{
		EditorTrace("TerminateGameEngine: deleting userInput ptr=%p", userInput);
		delete userInput;
		userInput = NULL;
	}

	MOVE_cleanup();

	//------------------------------------------------
	// shutdown the MC Texture Manager.
	if (mcTextureManager)
	{
		mcTextureManager->freeShapes();
		mcTextureManager->freeVertices();

		mcTextureManager->destroy();
		delete mcTextureManager;
		mcTextureManager = NULL;
	}

	//---------------------------------------------------------
	// End the Tiny Geometry Layer Heap.
	if (TG_Shape::tglHeap)
	{
		//Shut down the TGL RAM pools.
		if (colorPool)
		{
			colorPool->destroy();
			delete colorPool;
			colorPool = NULL;
		}
		
		if (vertexPool)
		{
			vertexPool->destroy();
			delete vertexPool;
			vertexPool = NULL;
		}

		if (facePool)
		{
			facePool->destroy();
			delete facePool;
			facePool = NULL;
		}

		if (shadowPool)
		{
			shadowPool->destroy();
			delete shadowPool;
			shadowPool = NULL;
		}

		if (trianglePool)
		{
			trianglePool->destroy();
			delete trianglePool;
			trianglePool = NULL;
		}
 
		TG_Shape::tglHeap->destroy();

		delete TG_Shape::tglHeap;
		TG_Shape::tglHeap = NULL;
	}

	//
	//--------------------------
	// Turn off the fast Files
	//--------------------------
	//
	FastFileFini();

	delete globalFloatHelp;
	globalFloatHelp = NULL;

	//----------------------------------------------------
	// Shutdown the MLR and associated stuff libraries
	//----------------------------------------------------
	gos_PushCurrentHeap(gosFX::Heap);

	delete effectStream;
	effectStream = NULL;
	// Now safe to free the backing buffer -- effectStream no longer holds a pointer into it.
	if (effectsData)
	{
		systemHeap->Free(effectsData);
		effectsData = NULL;
	}
	delete gosFX::LightManager::Instance;
	gosFX::LightManager::Instance = NULL;

	gos_PopCurrentHeap();

	delete theClipper;
	theClipper = NULL;

	delete MidLevelRenderer::MLRTexturePool::Instance;
	MidLevelRenderer::MLRTexturePool::Instance = NULL;

	gosFX::TerminateClasses();
	MidLevelRenderer::TerminateClasses();
	Stuff::TerminateClasses();

	//--------------------------------------------------------------
	// End the SystemHeap and globalHeapList
	if (systemHeap)
	{
		systemHeap->destroy();

		delete systemHeap;
		systemHeap = NULL;
	}

	if (globalHeapList)
	{
		globalHeapList->destroy();

		delete globalHeapList;
		globalHeapList = NULL;
	}

	//---------------------------------------------------------

	gos_DeleteFont(gosFontHandle);

	if (gameResourceHandle)
	{
		gos_CloseResourceDLL( gameResourceHandle );
		gameResourceHandle = nullptr;
	}
	if (gosResourceHandle)
	{
		gos_CloseResourceDLL(gosResourceHandle);
		gosResourceHandle = nullptr;
	}

	/*terminating stuff not initialized explicitly by InitializeGameEngine()*/
	//---------------------------------------------------------
	// TEST of PORT
	// Create VFX PANE and WINDOW to test draw of old terrain!
	if (globalPane)
	{
		delete globalPane;
		globalPane = NULL;
	}

	if (globalWindow)
	{
		delete globalWindow;
		globalWindow = NULL;
	}

	// Tear down the SDL/GL context and GameOS renderer that InitGameOS created.
	extern void EditorGameOS_Shutdown();
	EditorTrace("TerminateGameEngine: calling EditorGameOS_Shutdown");
	EditorGameOS_Shutdown();
	EditorTrace("TerminateGameEngine: complete");
}

//----------------------------------------------------------------------------
// Same command line Parser as MechCommander
void ParseCommandLine(char *command_line)
{
	EditorTrace("ParseCommandLine: %s", command_line ? command_line : "<null>");
	int i;
	int n_args = 0;
	int index = 0;
	char *argv[30];
	
	char tempCommandLine[4096];
	memset(tempCommandLine,0,4096);
	strncpy(tempCommandLine,command_line,4095);

	while (tempCommandLine[index] != '\0')  // until we null out
	{
		argv[n_args] = tempCommandLine + index;
		n_args++;
		while (tempCommandLine[index] != ' ' && tempCommandLine[index] != '\0')
		{
			index++;
		}
		while (tempCommandLine[index] == ' ')
		{
			tempCommandLine[index] = '\0';
			index++;
		}
	}

	i=0;
	while (i<n_args)
	{
		if (strcmpi(argv[i],"-burnInShadows") == 0)
		{
			forceShadowBurnIn = true;
		}
		else if (strcmpi(argv[i],"-resaveall") == 0)
		{
			justResaveAllMaps = true;
		}
		else if (strcmpi(argv[i],"-nodialog") == 0)
		{
			gNoDialogs = true;
		}
		else if (strcmpi(argv[i],"-mission") == 0)
		{
			i++;
			if (i < n_args)
			{
				if (argv[i][0] == '"')
				{
					// They typed in a quote, keep reading argvs
					// until you find the close quote
					strcpy(missionName,&(argv[i][1]));
					bool scanName = true;
					while (scanName && (i < n_args))
					{
						i++;
						if (i < n_args)
						{
							strcat(missionName," ");
							strcat(missionName,argv[i]);

							if (strstr(argv[i],"\"") != NULL)
							{
								scanName = false;
								missionName[strlen(missionName)-1] = 0;
							}
						}
						else
						{
							//They put a quote on the line with no space.
							//
							scanName = false;
							missionName[strlen(missionName)-1] = 0;
						}
					}
				}
				else
					strcpy(missionName,argv[i]);
			}
		}
		
		i++;
	}
}

//---------------------------------------------------------------------
void GetGameOSEnvironment( char* CommandLine )
{
	EditorTraceReset();
	EditorTrace("GetGameOSEnvironment: enter");
	EditorTrace("%s", EditorVersion_GetStartupLine());
	ParseCommandLine(CommandLine);
	EditorTrace("GetGameOSEnvironment: missionName=%s", missionName);

	// by Methuselas: keep the hosted Editor runtime labeled with the
	// Editor-owned SemVer rather than the global engine version stamp.
	Environment.applicationName			= (char*)EditorVersion_GetApplicationName();

	Environment.debugLog				= "";			//"DebugLog.txt";
	Environment.memoryTraceLevel		= 5;
	Environment.spew					= ""; //"GameOS_Texture GameOS_DirectDraw GameOS_Direct3D ";
	Environment.TimeStampSpew			= 0;

	Environment.GetGameInformation		= GetGameInformation;
	Environment.UpdateRenderers			= UpdateRenderers;
	Environment.InitializeGameEngine	= InitializeGameEngine;
	Environment.DoGameLogic				= DoGameLogic;
	Environment.TerminateGameEngine		= TerminateGameEngine;
	
	Environment.soundDisable			= TRUE;
	Environment.soundHiFi				= FALSE;
	Environment.soundChannels			= 24;

	Environment.dontClearRegistry		= true;
	Environment.allowMultipleApps		= false;
	// by Methuselas: the Editor owns its own SemVer now. Do not pull the
	// legacy MC2 engine stamp back into the Editor runtime identity.
	Environment.version					= (char*)EditorVersion_GetSemVer();

	// Remastered editor must run as a normal window. If this is left unset,
	// GameOS/SDL can create a fullscreen/topmost render surface that visually
	// sits above the desktop while mouse input still goes to windows underneath.
	Environment.fullScreen				= 0;
	Environment.Key_FullScreen			= 0;
	Environment.Key_SwitchMonitors		= 0;

	Environment.AntiAlias				= 0;
//
// Texture infomation
//
	Environment.Texture_S_256			= 6;
	Environment.Texture_S_128			= 1;
	Environment.Texture_S_64			= 0;
	Environment.Texture_S_32			= 1;
	Environment.Texture_S_16			= 5;

	Environment.Texture_K_256			= 2;
	Environment.Texture_K_128			= 5;
	Environment.Texture_K_64			= 5;
	Environment.Texture_K_32			= 5;
	Environment.Texture_K_16			= 5;

	Environment.Texture_A_256			= 0;
	Environment.Texture_A_128			= 1;
	Environment.Texture_A_64			= 5;
	Environment.Texture_A_32			= 1;
	Environment.Texture_A_16			= 0;

	Environment.bitDepth				= 16;

	Environment.RaidDataSource			= "MechCommander 2:Raid4"; 
	Environment.RaidFilePath			= "\\\\aas1\\MC2\\Test\\GOSRaid";
	Environment.RaidCustomFields		= "Area=GOSRaid"; 	

	// The legacy editor UI is laid out in a fixed 1024x768 coordinate space.
	// Do not inherit the desktop resolution here; on modern SDL/GameOS that can
	// produce a topmost/fullscreen-sized render surface with mismatched input,
	// hiding the menu/status areas and letting the mouse interact underneath.
	Environment.screenWidth = 1024;
	Environment.screenHeight = 768;
	Environment.fullScreen = 0;
	Environment.Key_FullScreen = 0;
	Environment.Key_SwitchMonitors = 0;

	Environment.Suppress3DFullScreenWarning = 1;

	EditorTrace("GetGameOSEnvironment: forced editor window %ldx%ld fullscreen=%ld",
		Environment.screenWidth,
		Environment.screenHeight,
		Environment.fullScreen);

	EditorData::setMapName( NULL );
}



