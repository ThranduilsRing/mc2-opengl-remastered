#include "stdafx.h"
#include <memory>
#include "vk_scene_renderer.h"
#include "render_submissions.h"
#include "Gates/preferences_legacy.h"
/***************************************************************
* FILENAME: Editor.cpp
* DESCRIPTION: Implements core Editor startup, update, and subsystem coordination.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 05/16/2026
* MODIFICATION: by Methuselas
* CHANGES: Remove temporary Editor startup/update trace calls after port validation.
****************************************************************/


#ifndef EDITORINTERFACE_H
#include "EditorInterface.h"
#endif

#include <GameOS.hpp>
#include <ToolOS.hpp>
#include <mlr\mlr.hpp>
#include <Stuff\stuff.hpp>
#include "EditorData.h"
#include "..\resource.h"
#include "EditorResourceCatalog.h"
#include "MC2Strings.h"


class gosRenderer;
gosRenderer* getGosRenderer();

#include "resource.h"
#include "EditorResourceFallback.h"
#include "EditorVersion.h"
extern void InitWLib(void);




extern Stuff::MemoryStream *effectStream;	// NS3: def in mclib/txmmgr.cpp
extern DWORD BaseVertexColor;	// NS3: def in mclib/quad.cpp (used at ParseCmdLine/prefs)
static MemoryPtr effectsData = nullptr;  // kept alive until effectStream is deleted at shutdown

// 128 MB for editor (original 8 MB was a 1999-era constraint).
// Editor-only: game binary unchanged.
unsigned long systemHeapSize = 128 * 1024 * 1024;
unsigned long guiHeapSize = 1023999;


bool hasGuardBand = false;
extern long terrainLineChanged; // the terrain uses this
extern float frameRate; // tiny geometry needs this

extern bool InEditor;
extern char FileMissingString[];
extern char CDMissingString[];
extern char MissingTitleString[];

extern char CDInstallPath[];

FloatHelpPtr globalFloatHelp = nullptr;
unsigned long currentFloatHelp = 0;
extern float CliffTerrainAngle;

extern bool gNoDialogs;

char* ExceptionGameMsg = nullptr; // some debugging thing I think

bool quitGame = FALSE;

bool gamePaused = FALSE;

extern bool reloadBounds;	// NS3: def in mclib/bdactor.cpp
extern bool justResaveAllMaps;	// NS3: def in mclib/terrain.cpp

extern bool forceShadowBurnIn;
// these globals are necessary for fast files for some reason

#define MAX_SHAPES	0

										//Heidi, turn this FALSE to turn Fog of War ON!
extern unsigned char godMode;			//Can I simply see everything, enemy and friendly?

void InitDW (void);

TimerManagerPtr timerManager = nullptr;

long FilterState = gos_FilterNone;

extern int TERRAIN_TXM_SIZE;
extern int ObjectTextureSize;	// NS3: def in mclib/bdactor.cpp

Editor* editor = nullptr;

extern char missionName[1024];	// NS3: def in GameOS/gameos/gos_crashbundle.cpp

enum { CPU_UNKNOWN, CPU_PENTIUM, CPU_MMX, CPU_KATMAI } Processor = CPU_PENTIUM;		//Needs to be set when GameOS supports ProcessorID -- MECHCMDR2

extern MidLevelRenderer::MLRClipper * theClipper;	// NS3: def in mclib/bdactor.cpp

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
			
	gos_SetRenderState( gos_State_Filter, gos_FilterNone );

	// Guard against partial init (e.g. mc2.fx missing caused InitializeGameEngine
	// to return early, leaving editor and globalFloatHelp as NULL).
	if (!editor || !globalFloatHelp)
	{
		return;
	}

	editor->render();

	globalFloatHelp->renderAll();

	turn++;

	reloadBounds = false;

   	colorPool->reset();
   	vertexPool->reset();
   	facePool->reset();
   	shadowPool->reset();
   	trianglePool->reset();
}

bool statisticsInitialized = false;
void DoGameLogic()
{

	if (globalFloatHelp)
		globalFloatHelp->resetAll();

	if (frameRate < 4.0)
		frameRate = 4.0;

	frameLength = 1.0f / frameRate;

	bool doTransformMath = true;


	if (doTransformMath)
	{

		if (editor)
			editor->update();

		if (timerManager)
			timerManager->update();
	}




	if (justResaveAllMaps)
	{
		PostQuitMessage(0);
		quitGame = true;		//We just needed to resave the maps.  Exit immediately
	}
		
	if (quitGame)
	{
		//EnterWindowMode();				//Game crashes if _TerminateApp called from fullScreen
		gos_TerminateApplication();
	}

}

void InitializeGameEngine()
{
	InEditor = true;

	if (fileExists("mc2resUS.dll"))
		gosResourceHandle = gos_OpenResourceDLL("mc2resUS.dll", nullptr, 0);
	else
		gosResourceHandle = gos_OpenResourceDLL("mc2res.dll", nullptr, 0);

	gameResourceHandle = gos_OpenResourceDLL("editores.dll", nullptr, 0);


	EditorResourceCatalog::Load();

	MC2Strings::Load();


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

	gosRenderer* renderer = getGosRenderer();
	if (renderer)
	{
		gosFontHandle = gos_LoadFont(path);
	}
	else
	{
		gosFontHandle = nullptr;
	}


	strcpy(CDInstallPath,".\\");

	long lastCharacter = (long)strlen(CDInstallPath)-1;
	if (CDInstallPath[lastCharacter] != '\\')
	{
		CDInstallPath[lastCharacter+1] = '\\';
		CDInstallPath[lastCharacter+2] = 0;
	}





	Stuff::InitializeClasses();
		MidLevelRenderer::InitializeClasses(1024);
		gosFX::InitializeClasses();
	
	gos_PushCurrentHeap(MidLevelRenderer::Heap);

	MidLevelRenderer::TGAFilePool *pool = new MidLevelRenderer::TGAFilePool("data\\Effects\\");
		MidLevelRenderer::MLRTexturePool::Instance = new MidLevelRenderer::MLRTexturePool(pool);

	MidLevelRenderer::MLRSortByOrder *cameraSorter = new MidLevelRenderer::MLRSortByOrder(MidLevelRenderer::MLRTexturePool::Instance);
		theClipper = new MidLevelRenderer::MLRClipper(nullptr, cameraSorter);
	
	gos_PopCurrentHeap();
	
	gos_PushCurrentHeap(gosFX::Heap);
	
	gosFX::EffectLibrary::Instance = new gosFX::EffectLibrary();
	Check_Object(gosFX::EffectLibrary::Instance);

	FullPathFileName effectsName;
	effectsName.init(effectsPath,"mc2.fx","");

	File effectFile;
	long result = effectFile.open(effectsName);
	if (result != NO_ERR)
	{
		// STOP() may be a no-op in some build configs -- use a fatal message box so
		// the user sees the error regardless, then return to avoid the crash.
		STOP(("Could not find MC2.fx at path: %s (open result=%ld)", (const char*)effectsName, result));
		return;
	}

	long effectsSize = effectFile.fileSize();
	if (effectsSize <= 0)
	{
		STOP(("MC2.fx is empty (size=%ld) at path: %s", effectsSize, (const char*)effectsName));
		effectFile.close();
		return;
	}

	// effectsData is a global so it outlives effectStream (freed at shutdown alongside it).
	effectsData = (MemoryPtr)std::malloc(effectsSize);
	effectFile.read(effectsData,effectsSize);
	effectFile.close();

	effectStream = new Stuff::MemoryStream(effectsData,effectsSize);
	gosFX::EffectLibrary::Instance->Load(effectStream);

	gosFX::LightManager::Instance = new gosFX::LightManager();

	gos_PopCurrentHeap();

	// effectsData is intentionally NOT freed here -- effectStream holds a raw pointer
	// into it and lives until TerminateGameEngine. Freed there after effectStream is deleted.

	globalFloatHelp = new FloatHelp(MAX_FLOAT_HELPS);

	if (!gosFontHandle)
	{
	}
	else
	{
	}

	float doubleClickThreshold = 0.2f;
	long dragThreshold = 10;

	// Editor frame update path calls Camera::update(), which uses userInput->setViewport().
	// The editor did not allocate userInput on this path, so the first DoGameLogic frame
	// can dereference NULL after the frame loop is restored.
	if (!userInput)
	{
		userInput = new UserInput;
		userInput->init();
		userInput->setMouseDoubleClickThreshold(doubleClickThreshold);
		userInput->setMouseDragThreshold((float)dragThreshold);
	}

	auto systemFile = std::make_unique<FitIniFile>();

#ifdef _DEBUG
	long systemOpenResult = 
#endif
	{
		long tracedSystemOpenResult = systemFile->open("system.cfg");
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
	systemFile.reset();

	auto prefs = std::make_unique<FitIniFile>();

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
	
	prefs.reset();


		colorPool 		= new TG_VertexPool;
		colorPool->init(20000);
		
		vertexPool 		= new TG_GOSVertexPool;
		vertexPool->init(20000);
		
		facePool 		= new TG_DWORDPool;
		facePool->init(40000);
		
		shadowPool 		= new TG_ShadowPool;
		shadowPool->init(20000);
		
		trianglePool 	= new TG_TrianglePool;
		trianglePool->init(20000);

	


	if (getGosRenderer())
	{
		mc2::VkSceneRenderer::start();
	}
	else
	{
	}

	mc2::renderSubmissions().startVertices(500000);

	// clearArrays() resets both the vertex and render-shape managers.
	// The game initializes rsManager through startShapes(); the editor must do the same
	// before the restored DoGameLogic/update path can safely run.
	mc2::renderSubmissions().startShapes(10000);

	godMode = true;
	MOVE_init(30);

	weaponEffects = new WeaponEffects;
		weaponEffects->init("Effects");

editor = new Editor();

if (justResaveAllMaps)
{
    editor->resaveAll();
}

{

    FullPathFileName guiloader;
    guiloader.init(artPath, "editorGui", ".fit");


    FitIniFile loader;
    long result = loader.open(guiloader);

    // Log missing editorGui.fit or renderer initialization: either prevents
    // editor->init() from creating the pilot catalog and object manager.
    {
        FILE* f = fopen("editor-startup.log", "a");
        if (f)
        {
            fprintf(f, "InitializeGameEngine: editorGui.fit open result=%ld renderer=%d -> editor->init %s\n",
                result, getGosRenderer() ? 1 : 0,
                (result == NO_ERR && getGosRenderer()) ? "RUNS" : "SKIPPED");
            fclose(f);
        }
    }

    if (result == NO_ERR)
    {

if (getGosRenderer())
{
    editor->init(const_cast<char*>((const char*)guiloader));
}
else
{
}
    }
    else
    {
    }
}
	timerManager = new TimerManager;
	timerManager->init();

	initColorTables();

	if (!statisticsInitialized)
	{
		statisticsInitialized = true;
	}

	InitDW();
}

bool alreadyTermed = false;
void TerminateGameEngine()
{
	if (alreadyTermed)
		return;
	else
		alreadyTermed = true;

	destroyColorTables();

	delete timerManager;
	timerManager = nullptr;

	if ( editor )
		delete editor;
	editor = nullptr;

	if (weaponEffects)
		delete weaponEffects;
	weaponEffects = nullptr;

	if (userInput)
	{
		delete userInput;
		userInput = nullptr;
	}

	MOVE_cleanup();

    if (mc2::VkSceneRenderer::active())
        mc2::VkSceneRenderer::shutdown();

	if (colorPool || vertexPool || facePool || shadowPool || trianglePool)
	{
		if (colorPool)
		{
			colorPool->destroy();
			delete colorPool;
			colorPool = nullptr;
		}
		
		if (vertexPool)
		{
			vertexPool->destroy();
			delete vertexPool;
			vertexPool = nullptr;
		}

		if (facePool)
		{
			facePool->destroy();
			delete facePool;
			facePool = nullptr;
		}

		if (shadowPool)
		{
			shadowPool->destroy();
			delete shadowPool;
			shadowPool = nullptr;
		}

		if (trianglePool)
		{
			trianglePool->destroy();
			delete trianglePool;
			trianglePool = nullptr;
		}
	}

	//
	//
	FastFileFini();

	delete globalFloatHelp;
	globalFloatHelp = nullptr;

	gos_PushCurrentHeap(gosFX::Heap);

	delete effectStream;
	effectStream = nullptr;
	// Now safe to free the backing buffer -- effectStream no longer holds a pointer into it.
	if (effectsData)
	{
		std::free(effectsData);
		effectsData = nullptr;
	}
	delete gosFX::LightManager::Instance;
	gosFX::LightManager::Instance = nullptr;

	gos_PopCurrentHeap();

	delete theClipper;
	theClipper = nullptr;

	delete MidLevelRenderer::MLRTexturePool::Instance;
	MidLevelRenderer::MLRTexturePool::Instance = nullptr;

	gosFX::TerminateClasses();
	MidLevelRenderer::TerminateClasses();
	Stuff::TerminateClasses();






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

	if (globalPane)
	{
		delete globalPane;
		globalPane = nullptr;
	}

	if (globalWindow)
	{
		delete globalWindow;
		globalWindow = nullptr;
	}

	extern void EditorGameOS_Shutdown();
	EditorGameOS_Shutdown();
}

//----------------------------------------------------------------------------
// Same command line Parser as MechCommander
void ParseCommandLine(char *command_line)
{
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
					strcpy(missionName,&(argv[i][1]));
					bool scanName = true;
					while (scanName && (i < n_args))
					{
						i++;
						if (i < n_args)
						{
							strcat(missionName," ");
							strcat(missionName,argv[i]);

							if (strstr(argv[i],"\"") != nullptr)
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

void GetGameOSEnvironment( char* CommandLine )
{
		ParseCommandLine(CommandLine);

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


	EditorData::setMapName( nullptr );
}



