#include "utils/mc2log.h"
#define LOADSCREEN_CPP
/*************************************************************************************************\
LoadScreen.cpp			: Implementation of the LoadScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifdef LINUX_BUILD
#include"platform_windows.h"
struct DDSURFACEDESC2 {
};
#else
#include<windows.h>
#include<ddraw.h>
#endif
#include"loadscreen.h"
#include"aanimobject.h"
#include"tgainfo.h"
#include"mclib.h"
#include"prefs.h"
#include"multplyr.h"
#include"mission.h"
#include"gamesound.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "mc2fmt.h"
#include "gos_render.h"
#include "gos_screenshot.h"
#include "../GameOS/gameos/utils/mc2log.h"
float loadProgress = 0.0f;

long LoadScreen::xProgressLoc = 0;
long LoadScreen::yProgressLoc = 0;
long LoadScreen::xWaitLoc = 0;
long LoadScreen::yWaitLoc = 0;
bool LoadScreen::turnOffAsyncMouse = false;

TGAFileHeader* LoadScreen::progressTextureMemory = nullptr;
TGAFileHeader* LoadScreen::progressBackground = nullptr;
TGAFileHeader* LoadScreen::mergedTexture = nullptr;
TGAFileHeader* LoadScreen::waitingForPlayersMemory = nullptr;
// Owners of the raw TGA buffers the pointers above alias (the header + pixels live in one char block).
static std::unique_ptr<char[]> s_progressBackgroundBuf, s_progressTextureBuf, s_mergedTextureBuf, s_waitingForPlayersBuf;
static LoadScreen* s_loadingScreen = nullptr;

std::unique_ptr<LoadScreen> LoadScreenWrapper::enterScreen = nullptr;
std::unique_ptr<LoadScreen> LoadScreenWrapper::exitScreen = nullptr;


extern volatile bool mc2IsInMouseTimer;
extern volatile bool mc2IsInDisplayBackBuffer;

extern void (*AsynFunc)(RECT& WinRect,DDSURFACEDESC2& mouseSurfaceDesc );
extern CPrefs prefs;

void ProgressTimer(	RECT& WinRect,DDSURFACEDESC2& mouseSurfaceDesc );
//
// Returns the number of bits in a given mask.  Used to determine if we are in 555 mode vs 565 mode.
WORD GetNumberOfBits( DWORD dwMask );

void MouseTimerInit();
void MouseTimerKill();

LoadScreenWrapper::LoadScreenWrapper()
{
	//How about, for shits and giggles we toss the static members we created for single player when this inits for multiplayer
	// or any other player!!!!
	if ( enterScreen )
		enterScreen.reset();
	if ( exitScreen )
		exitScreen.reset();

	enterScreen = nullptr;
	exitScreen = nullptr;
	enterScreen = std::make_unique_for_overwrite<LoadScreen>();
	exitScreen = std::make_unique_for_overwrite<LoadScreen>();
	bFirstTime = 0;
}

LoadScreenWrapper::~LoadScreenWrapper()
{
	if ( enterScreen )
		enterScreen.reset();
	if ( exitScreen )
		exitScreen.reset();

	enterScreen = nullptr;
	exitScreen = nullptr;
}

void LoadScreenWrapper::init( FitIniFile& file )
{
	enterScreen->init( file );

//	changeRes();
	bFirstTime = 0;
}

void LoadScreenWrapper::changeRes()
{
	const char* Appendix = nullptr;

    /*
	switch( prefs.resolution )
	{
	case 0:
		Appendix = "_640";
		break;

	case 1:
		break;

	case 2:
		Appendix = "_1024";

		break;

	case 3:
		Appendix = "_1280";

		break;

	case 4:
		Appendix = "_1600";

		break;

	default:
		Assert( 0, 0, "Unexpected resolution found in prefs" );
		break;
	}
    */

    //sebi:
    const int resolutions[] = {     640,    1024,       1280,       1600,       1920};
    const char* const str_resolutions[] = { "_640", "_1024",    "_1280",    "_1600",    "_1920"};
    bool b_search_for_best_resolution = true;
    int suitable_res = 0;

    for(int i=0;i<sizeof(resolutions)/sizeof(resolutions[0]);++i) {
        if(prefs.resolutionX == resolutions[i]) {
            suitable_res = i;
            b_search_for_best_resolution = false;
            break;
        }
    }

    if(b_search_for_best_resolution) {
        int min_pos_dist = 10000;
        MC2_LOG(Render, Debug, "Not native resolution, selecting closest matching resolution for loading screens");
        for(int i=0;i<sizeof(resolutions)/sizeof(resolutions[0]);++i) {
            int dst = resolutions[i] - prefs.resolutionX;
            if(dst >=0 && dst < min_pos_dist) {
                min_pos_dist = dst;
                suitable_res = i;
            }
        }
    }
    Appendix = str_resolutions[suitable_res];
    //

	char fileName[256];
	mc2::fmt::to_buf(fileName, "mcl_loadingscreen");

	if ( Appendix )
		strcat( fileName, Appendix );

	FullPathFileName path;

	path.init( artPath, fileName, ".fit" );

	FitIniFile outFile;

	// Tracks the resolved loadscreen name for debug-state asset dump.
	const char* resolvedLoadScreen = fileName;

	if ( NO_ERR != outFile.open( path ) )
	{
		// Old MC2X campaigns (pre-1920 era) may lack high-res loading screens.
		// Walk DOWN the resolution table and use the best fit that opens.
		bool opened = false;
		for ( int fi = suitable_res - 1; fi >= 0; fi-- )
		{
			char fbName[256];
			mc2::fmt::to_buf(fbName, "mcl_loadingscreen{}", mc2::fmt::safe(str_resolutions[fi]));
			FullPathFileName fbPath;
			fbPath.init( artPath, fbName, ".fit" );
			if ( NO_ERR == outFile.open( static_cast<const char*>(fbPath) ) )
			{
				MC2_LOG(Render, Debug, "loadscreen: {} missing, fell back to {}.fit",
				       mc2::fmt::safe(fileName), mc2::fmt::safe(fbName));
				// fbName is stack-local; copy into fileName so resolvedLoadScreen stays valid.
				strncpy( fileName, fbName, sizeof(fileName) - 1 );
				fileName[sizeof(fileName) - 1] = '\0';
				opened = true;
				break;
			}
		}
		if ( !opened )
		{
			char error[256];
			mc2::fmt::to_buf(error, "couldn't open file {} (no fallback found)", mc2::fmt::safe(static_cast<const char*>(path)));
			Assert( 0, 0, error );
			return;
		}
	}

	// DEBUG-STATE-ASSETS: record resolved loadscreen filename for per-mission JSON dump.
	{
		extern char g_dbgLoadScreen[64];
		strncpy( g_dbgLoadScreen, resolvedLoadScreen, sizeof(g_dbgLoadScreen) - 1 );
		g_dbgLoadScreen[sizeof(g_dbgLoadScreen) - 1] = '\0';
	}

	//The 0x2 means that we do NOT want to flush this texture when we toss
	// the texture cache before a mission.  BUT we DO want the texture to cache out
	// to make more room for stuff.
	exitScreen->init( outFile, 0x2 );

	exitScreen->setupOutAnims();

	LoadScreen::changeRes( outFile );
}

void LoadScreen::changeRes( FitIniFile& outFile )
{

	s_progressBackgroundBuf.reset();
	progressBackground = nullptr;

	s_waitingForPlayersBuf.reset();
	waitingForPlayersMemory = nullptr;

	if ( !progressBackground )
	{
		char progressPath[256];
		char progressBackgroundPath[256];



		long result = outFile.seekBlock( "LoadingBar" );
		gosASSERT( result == NO_ERR );

		outFile.readIdLong( "XLocation", xProgressLoc );
		outFile.readIdLong( "YLocation", yProgressLoc );
		outFile.readIdString( "FileName", progressPath, 255 );
		outFile.readIdString( "BackgroundFileName", progressBackgroundPath, 255);


		File tgaFile;
		FullPathFileName path;
		path.init( artPath, progressBackgroundPath, ".tga" );
		if ( NO_ERR != tgaFile.open( path ) )
		{
			char error[256];
			mc2::fmt::to_buf(error, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(path)));
			Assert( 0, 0, error );
			return;
		}

		long size = tgaFile.fileSize();
	
		s_progressBackgroundBuf = std::make_unique<char[]>(size);
		progressBackground = reinterpret_cast<TGAFileHeader*>(s_progressBackgroundBuf.get());
		tgaFile.read( reinterpret_cast<unsigned char*>(progressBackground), tgaFile.fileSize() );

		tgaFile.close();

		path.init( artPath, progressPath, ".tga" );
		if ( NO_ERR != tgaFile.open( path ) )
		{
			char error[256];
			mc2::fmt::to_buf(error, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(path)));
			Assert( 0, 0, error );
			return;
		}

		size = tgaFile.fileSize();

		s_progressTextureBuf = std::make_unique<char[]>(size);
		progressTextureMemory = reinterpret_cast<TGAFileHeader*>(s_progressTextureBuf.get());
		tgaFile.read( reinterpret_cast<unsigned char*>(progressTextureMemory), tgaFile.fileSize() );

		s_mergedTextureBuf = std::make_unique<char[]>(size);
		mergedTexture = reinterpret_cast<TGAFileHeader*>(s_mergedTextureBuf.get());
		memcpy( mergedTexture, progressTextureMemory, size );

		tgaFile.close();

		result = outFile.seekBlock( "WaitImage" );
		gosASSERT( result == NO_ERR );
		outFile.readIdString( "FileName", progressPath, 255 );
		outFile.readIdLong( "XLocation", xWaitLoc );
		outFile.readIdLong( "YLocation", yWaitLoc );

		path.init( artPath, progressPath, ".tga" );
		if ( NO_ERR != tgaFile.open( path ) )
		{
			char error[256];
			mc2::fmt::to_buf(error, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(path)));
			Assert( 0, 0, error );
			return;
		}

		size = tgaFile.fileSize();

		s_waitingForPlayersBuf = std::make_unique<char[]>(size);
		waitingForPlayersMemory = reinterpret_cast<TGAFileHeader*>(s_waitingForPlayersBuf.get());
		tgaFile.read( reinterpret_cast<unsigned char*>(waitingForPlayersMemory), tgaFile.fileSize() );

		flipTopToBottom( reinterpret_cast<BYTE*>((waitingForPlayersMemory + 1)), waitingForPlayersMemory->pixel_depth,
			waitingForPlayersMemory->width, waitingForPlayersMemory->height );
	}
}

void LoadScreenWrapper::begin()
{
	waitForResChange = 0;
	bFirstTime = true;

	enterScreen->begin();
}

void LoadScreenWrapper::update()
{
	if ( loadProgress > 99 )
		soundSystem->playDigitalSample( LOAD_DOORS_OPENING );
	else if ( bFirstTime )
		soundSystem->playDigitalSample( LOAD_DOORS_CLOSING );

	 bFirstTime = 0;

	if ( waitForResChange ) // waiting one more render to force 
	{
		status = READYTOLOAD;
		waitForResChange = 0;
	}
	else
	{

	if ( Environment.screenWidth == 800 )
		{
			enterScreen->update();
			status = enterScreen->getStatus();
			if ( status == READYTOLOAD )
			{
				waitForResChange = 1;
				changeRes();
				status = RUNNING;
			}
		}
		else
		{
			exitScreen->update();
			status = exitScreen->getStatus();
		}
	}
}

void LoadScreenWrapper::render( int xOffset, int yOffset )
{
	if ( Environment.screenWidth == 800 )
	{
		enterScreen->render( xOffset, yOffset );
	}
	else
		exitScreen->render( xOffset, yOffset );
}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
LoadScreen::LoadScreen(  )
{
	progressBackground = nullptr;
	progressTextureMemory = nullptr;
	animIndices = nullptr;
}

//-------------------------------------------------------------------------------------------------

LoadScreen::~LoadScreen()
{
	if (s_loadingScreen == this) {
		s_loadingScreen = nullptr;
		graphics::set_loading_draw(nullptr);
	}
	s_progressBackgroundBuf.reset();
	s_progressTextureBuf.reset();
	s_mergedTextureBuf.reset();
	s_waitingForPlayersBuf.reset();
	animIndices.reset();

	waitingForPlayersMemory = progressBackground = progressTextureMemory = mergedTexture = nullptr;
}

void LoadScreen::begin()
{
	for ( int i = 0; i < animObjectsCount; i++ )
	{
		//The assign below will overwrite the assign in defaultConstructor, leaking the memory
		animObjects[i].animInfo.destroy();

		animObjects[i].animInfo = inAnims[animIndices[i]];
		animObjects[i].animInfo.begin();
		animObjects[i].update();
	}

	loadProgress = 0;

	//-----------------------------------------------
	// The native cursor is independent of the loading framebuffer.
	userInput->mouseOn();
}
void LoadScreen::init(FitIniFile& file, DWORD neverFlush)
{
	LogisticsScreen::init( file, "Static", "Text", "Rect", "Button", "Edit", "AnimObject", neverFlush );

	file.seekBlock( "AnimationTopOut" );
	outAnims[0].init( &file, "" );

	file.seekBlock( "AnimationBottomOut" );
	outAnims[1].init( &file, "" );

	file.seekBlock( "AnimationLeftOut" );
	outAnims[2].init( &file, "" );

	file.seekBlock( "AnimationRightOut" );
	outAnims[3].init( &file, "" );

	file.seekBlock( "AnimationTopIn" );
	inAnims[0].init( &file, "" );

	file.seekBlock( "AnimationBottomIn" );
	inAnims[1].init( &file, "" );

	file.seekBlock( "AnimationLeftIn" );
	inAnims[2].init( &file, "" );

	file.seekBlock( "AnimationRightIn" );
	inAnims[3].init( &file, "" );

	file.seekBlock( "AnimationTop2In" );
	inAnims[4].init( &file, "" );

	file.seekBlock( "AnimationTop2Out" );
	outAnims[4].init( &file, "" );


	// Stock keeps the "LOADING" text in AnimObject18; MCO ships it as Text0 and has no AnimObjects.
	text.init( &file, NO_ERR == file.seekBlock( "AnimObject18" ) ? "AnimObject18" : "Text0" );
	s_loadingScreen = this;
	graphics::set_loading_draw([] {
		gos_SetUiCanvasActive(true);
		if (s_loadingScreen) s_loadingScreen->render(0, 0);
	});
	

	if ( animObjectsCount )
	{
		char blockName[256];
		animIndices = std::make_unique<int[]>(animObjectsCount);
		for ( int i= 0; i < animObjectsCount; i++ )
		{
			mc2::fmt::to_buf(blockName, "AnimObject{:d}", mc2::fmt::as_i32(i));
			file.seekBlock( blockName );
			file.readIdString( "AnimationOut", blockName, 255 );
			if ( strstr( blockName, "2" ) )
				animIndices[i] = 4;
			else if ( strstr( blockName, "Top" ) )
				animIndices[i] = 0;
			else if ( strstr( blockName, "Bottom" ) )
				animIndices[i] = 1;
			else if ( strstr( blockName, "Left" ) )
				animIndices[i] = 2;
			else
				animIndices[i] = 3;
		}
	}

}

void LoadScreen::update()
{
	status = RUNNING;
	LogisticsScreen::update();

	bool bDone = true;
	for ( int i = 0; i < animObjectsCount; i++ )
	{
		bDone &= animObjects[i].isDone();
	}

	if ( bDone )
	{
		if ( loadProgress < 99.f )
		{

			for ( int i = 0; i < animObjectsCount; i++ )
			{
				//The assign below will overwrite the assign in begin, leaking the memory
				animObjects[i].animInfo.destroy();

				animObjects[i].animInfo = outAnims[animIndices[i]];
				animObjects[i].begin();
				animObjects[i].update();
			}

			status = READYTOLOAD;
			prefs.applyPrefs( true );

			turnOffAsyncMouse = mc2UseAsyncMouse;
			if ( !mc2UseAsyncMouse )
				MouseTimerInit();
			mc2UseAsyncMouse = true;			
			AsynFunc = ProgressTimer;
		}
		else
		{
			loadProgress = 0;

			status = NEXT;

			//YIKES!!  We could be checking the if before the null and executing after!!  Block the thread!
			//Wait for thread to finish.
			while (mc2IsInMouseTimer)
				;

			//ONLY set the mouse BLT data at the end of each update.  NO MORE FLICKERING THEN!!!
			// BLOCK THREAD WHILE THIS IS HAPPENING
			mc2IsInDisplayBackBuffer = true;

			AsynFunc = nullptr;
			mc2UseAsyncMouse = turnOffAsyncMouse;
			if ( !mc2UseAsyncMouse)
				MouseTimerKill();

			mc2IsInDisplayBackBuffer = false;

			//Force mouse Cursors to smaller or larger depending on new video mode.
			userInput->initMouseCursors("cursors");
			userInput->mouseOn();
		}
	}
	else
	{
		userInput->mouseOn();
	}
}

void LoadScreen::render( int x, int y )
{
	//ignoring animation information...
	if (graphics::loading_phase().empty()) LogisticsScreen::render();
	else LogisticsScreen::renderLegacy();

	// MCO's mcl_loadingscreen.fit has no AnimObjects: animObjects is null, so no slide offset.
	int curX = animObjectsCount ? static_cast<int>(animObjects[0].animInfo.getXDelta()) : 0;
	int curY = animObjectsCount ? static_cast<int>(animObjects[0].animInfo.getYDelta()) : 0;
	const auto phase = graphics::loading_phase();
	if (!phase.empty()) curX = curY = 0;
	text.move( x + curX, y + curY );
	// Keep the stock localized label, font and colour; only the trailing dots vary with wall time.
	std::string label = text.text;
	while (!label.empty() && (label.back() == '.' || label.back() == ' ')) label.pop_back();
	label += " " + std::string(graphics::loading_dots(SDL_GetTicks64()), '.');
	const int boxWidth = std::max(static_cast<int>(text.width()), static_cast<int>(text.font.width("Terrain textures")));
	const int boxX = std::clamp(static_cast<int>(text.x() + (text.width() - boxWidth) / 2), 0,
	                          std::max(0, Environment.screenWidth - boxWidth));
	const int lineHeight = static_cast<int>(text.font.height()) + 2;
	const int boxY = std::min(static_cast<int>(text.y()), Environment.screenHeight - 2 * lineHeight);
	text.font.render(label.c_str(), boxX, boxY, boxWidth, lineHeight, text.getColor(), false, text.alignment);
	if (!phase.empty()) {
		const auto stage = graphics::loading_label(phase);
		text.font.render(stage.data(), boxX, boxY + lineHeight, boxWidth, lineHeight, text.getColor(), false, text.alignment);
		// Reuse the existing multi-frame loading capture option; captures are actual presented frames.
		if (mc2::gate::get<"MC2_SCREENSHOT_AT_FRAME">() == -5) {
			static Uint64 lastShot = 0;
			static unsigned shot = 0;
			const auto now = SDL_GetTicks64();
			if (shot < 5 && now - lastShot >= 350) {
				const auto path = mc2::gate::str<"MC2_SCREENSHOT_PATH">() + ".loading" + std::to_string(++shot) + ".tga";
				if (gos::screenshot::request(path.c_str())) lastShot = now;
			}
		}
	}
	text.move( -x - curX, -y - curY);
}

void ProgressTimer(	RECT& WinRect,DDSURFACEDESC2& mouseSurfaceDesc )
{

	// LOAD-BANNER-RESIDUE-1: this async mouse-thread blit is a DDraw-era
	// remnant. On the GL port the load screen renders its own progress bar
	// every pump, while this overlay composited the progress art over
	// WHATEVER was on screen whenever loadProgress sat in (0,100) — the
	// world-floating blue banner at mission start, and the same art flashing
	// behind the pilot-ready screen during campaign save/load ops
	// (Mission::save/load arm it around near-instant file IO). Retired by
	// default; MC2_LEGACY_LOAD_BANNER=1 restores the old overlay if some
	// genuinely blocking path turns out to need it.
	{
		static const bool s_legacyBanner = false;
		if ( !s_legacyBanner )
			return;
	}

	if ( !LoadScreen::progressBackground )
		return;

	// LOAD-BANNER-RESIDUE-1 probe (MC2_LOG_PREVIEW): whenever this async blit
	// is about to composite the progress art, log the progress value — pins
	// down WHICH lingering state draws the world-floating banner at mission
	// start. Throttled; file-based per probe discipline.
	if ( mc2::gate::get<"MC2_LOG_PREVIEW">() && ( (loadProgress > 0 && loadProgress < 100) || loadProgress == 1000 ) )
	{
		static int s_bannerTick = 0;
		if ( (s_bannerTick++ % 30) == 0 )
		{
			if ( bool f = mc2::log::enabled(mc2::log::Cat::Ui, mc2::log::Level::Info) )
			{
				MC2_LOG(Ui, Info, "[LOAD-BANNER] ProgressTimer draw loadProgress={:.1f}", loadProgress);

			}
		}
	}

	long destX = 0;
	long destY = 0;

	BYTE* pMem = reinterpret_cast<BYTE*>((LoadScreen::mergedTexture + 1));
	long destRight = 0;
	long destBottom = 0;

	long srcWidth = 0;
	long srcDepth = 0;


	if ( loadProgress > 0 && loadProgress < 100 )
	{
		destX = 0;
		destY = 0;

		long destWidth = LoadScreen::progressBackground->width;
		long destHeight = LoadScreen::progressBackground->height;

		float widthIncPerProgress = static_cast<float>(destWidth) * 0.01f;

		long* pLSrc =  reinterpret_cast<long*>((LoadScreen::progressBackground+1));
		long* pLDest = reinterpret_cast<long*>((LoadScreen::mergedTexture+1));

		// merge background and current progress together...
		for ( int i = 0; i < 2; i++ )
		{
			for ( int y = 0; y < destHeight; y++ )
			{

				for ( long x = 0; x < LoadScreen::progressBackground->width; x++ )
				{
					if ( x < destWidth )
						*pLDest++ = *pLSrc++;
					else
					{
						pLDest++;
						pLSrc++;
					}
				}
			}
			pLSrc = reinterpret_cast<long*>(( LoadScreen::progressTextureMemory + 1 ));
			pLDest = reinterpret_cast<long*>((LoadScreen::mergedTexture+1));

			destWidth = loadProgress * widthIncPerProgress;
		}

		destX = WinRect.left + (LoadScreen::xProgressLoc);
		destY = WinRect.top + (LoadScreen::yProgressLoc);

		pMem = reinterpret_cast<BYTE*>((LoadScreen::mergedTexture + 1));
		destRight = destX + LoadScreen::progressBackground->width;
		destBottom = (destY + LoadScreen::progressBackground->height);

		srcWidth = LoadScreen::progressBackground->width;
		srcDepth = LoadScreen::progressBackground->pixel_depth/8;
	}
	else if ( loadProgress == 1000 )
	{
		destX = WinRect.left + LoadScreen::xWaitLoc;
		destY = WinRect.top + LoadScreen::yWaitLoc;

		pMem = reinterpret_cast<BYTE*>((LoadScreen::waitingForPlayersMemory + 1));
		destRight = destX + LoadScreen::waitingForPlayersMemory->width;
		destBottom = (destY + LoadScreen::waitingForPlayersMemory->height);
		destRight = destRight > WinRect.right ? WinRect.right : destRight;
		destBottom = destBottom > WinRect.bottom ? WinRect.top : destBottom;


		srcWidth = LoadScreen::waitingForPlayersMemory->width;
		srcDepth = LoadScreen::waitingForPlayersMemory->pixel_depth/8;

	}
	else
    {
		return;
    }

#ifndef LINUX_BUILD
    // now put it on the screen...

    long destWidth = destRight - destX;
    long destHeight = destBottom - destY;


    for ( int y = 0; y < destHeight; y++ )
    {
        BYTE* pSrc = pMem + y * srcWidth * srcDepth;
        BYTE* pDest = reinterpret_cast<MemoryPtr>(mouseSurfaceDesc.lpSurface) + destX * mouseSurfaceDesc.ddpfPixelFormat.dwRGBBitCount/8 +
            ((destY + y) * mouseSurfaceDesc.lPitch);

        for ( long x = 0; x < destWidth; x++ )
        {

            DWORD mColor = *reinterpret_cast<long*>(pSrc);
            BYTE baseAlpha 		= 0;
            BYTE baseColorRed	= (mColor & 0x00ff0000)>>16;
            BYTE baseColorGreen	= (mColor & 0x0000ff00)>>8;
            BYTE baseColorBlue 	= (mColor & 0x000000ff);
            pSrc += 4;

            if ( mouseSurfaceDesc.ddpfPixelFormat.dwRGBBitCount == 32 )
            {
                (*reinterpret_cast<long*>(pDest)) = mColor;
                pDest += 4;
            }
            else if ( mouseSurfaceDesc.ddpfPixelFormat.dwRGBBitCount == 24 )
            {

                if ( !baseAlpha )
                {
                    pDest++;
                    pDest++;
                    pDest++; 
                }

                *pDest++ = baseColorRed;
                *pDest++ = baseColorGreen;
                *pDest++ = baseColorBlue;

            }
            else if ( mouseSurfaceDesc.ddpfPixelFormat.dwRGBBitCount == 16 )
            {
                bool in555Mode = false;
                if (GetNumberOfBits(mouseSurfaceDesc.ddpfPixelFormat.dwGBitMask) == 5)
                    in555Mode = true;

                if ( !baseAlpha )
                {
                    long clr;
                    if (in555Mode)
                    {
                        clr = (baseColorRed >> 3) << 10;
                        clr += (baseColorGreen >> 3) << 5;
                        clr += (baseColorBlue >> 3);
                    }
                    else
                    {
                        clr = (baseColorRed >> 3) << 11;
                        clr += (baseColorGreen >> 2) << 5;
                        clr += (baseColorBlue >> 3);
                    }

                    *pDest++ = clr & 0xff;
                    *pDest++ = clr >> 8;
                }
                else
                {
                    pDest++;
                    pDest++;
                }
            }


        }
    }
#endif
}


void LoadScreen::setupOutAnims()
{
	for ( int i = 0; i < animObjectsCount; i++ )
	{
		//The assign below will overwrite the assign in begin, leaking the memory
		animObjects[i].animInfo.destroy();

		animObjects[i].animInfo = outAnims[animIndices[i]];
		animObjects[i].begin();
		animObjects[i].update();
	}
}

//*************************************************************************************************
// end of file ( LoadScreen.cpp ) 
