#include "mc2_czstring.h"
#include "utils/mc2log.h"
//---------------------------------------------------------------------------
//
// MechCommander 2
//
// UserInput Class -- Polls the state of keyboard, mouse, joystick
//						for this frame and stores values.
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef USERINPUT_H
#include"userinput.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef INIFILE_H
#include"inifile.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef CLIP_H
#include"clip.h"
#endif

#ifndef MC2_RENDER_SUBMISSIONS_H
#include"render_submissions.h"
#endif

#include"../GameOS/gameos/gos_profiler.h"

#include"platform_windows.h"

#include<stuff/stuff.hpp>
#include<math.h>
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "../GameOS/gameos/gos_render.h"
#include "../GameOS/gameos/diagnostic_trace.h"
#include "../RenderCore/KtxLoader.h"
#include <algorithm>

//---------------------------------------------------------------------------
UserInput *userInput = nullptr;
extern bool 			hasGuardBand;
volatile bool			UserInput::drawMouse = false;
extern volatile bool 	mc2IsInDisplayBackBuffer;
extern volatile bool 	mc2IsInMouseTimer;

void MouseTimerInit();
void MouseTimerKill();
//---------------------------------------------------------------------------
void MouseCursorData::initCursors (mc2::czstring cursorFileName)
{
	ZoneScopedN("MouseCursorData::initCursors");
	//New
	// add an "a" to the end of the cursorFileName IF we are running in 800x600 or less.
	// Loads different sized cursors.
	char realHackName[1024];
	strcpy(realHackName,cursorFileName);
	if (Environment.screenWidth <= 800)
		mc2::fmt::to_buf(realHackName, "{}a", mc2::fmt::safe(cursorFileName));

	FullPathFileName cursorName;
	cursorName.init(artPath,realHackName,".fit");
	
	FitIniFile cursorFile;
	long result;
	long openResult;
	{
		ZoneScopedN("MouseCursorData::initCursors openFit");
		result = cursorFile.open(cursorName);
	}
	openResult = result;
	gosASSERT(result == NO_ERR);
	
	{
		ZoneScopedN("MouseCursorData::initCursors mainBlock");
		result = cursorFile.seekBlock("Main");
	}
	gosASSERT(result == NO_ERR);

	{
		ZoneScopedN("MouseCursorData::initCursors numCursors");
		result = cursorFile.readIdLong("NumCursors",numCursors);
	}
	gosASSERT(result == NO_ERR);

	gosASSERT( numCursors < MAX_MOUSE_STATES );

	cursorInfos = std::make_unique<StaticInfo[]>(numCursors);
		
	//----------------------------------------------
	// Each cursor is defined as a number of frames
	// and a TGA File Name which we use to create
	// the texture handle.
	char blockName[32];
	for (long i=0;i<numCursors;i++)
	{
		ZoneScopedN("MouseCursorData::initCursors cursor");
		mc2::fmt::to_buf(blockName, "Cursor{:d}", mc2::fmt::as_i32(i));
		{
			ZoneScopedN("MouseCursorData::initCursors staticInfo");
			cursorInfos[i].init( cursorFile, blockName,0,0,0x1,gos_Texture_Keyed);
		}
		{
			ZoneScopedN("MouseCursorData::initCursors metadata");
			cursorFile.readIdChar( "HotSpotX", mouseHS[i][0] );
			cursorFile.readIdChar( "HotSpotY", mouseHS[i][1] );
			cursorFile.readIdULong( "NumFrames", numFrames[i] );
			cursorFile.readIdFloat( "FrameLength", frameLengths[i] );
		}

	}

	// ---- MC2_LOG_CURSOR diagnostics (instrumentation only) ----
	// Logs the resolved FIT path, the open result, NumCursors, and the per-cursor
	// texture handle for the states the GUI actually uses (NORMAL=21, LOGISTICS=43).
	// A texHandle of 0 here means StaticInfo::init's loadTexture failed silently
	// (the smoking gun for an invisible-but-positioned cursor quad).
	if (false)
	{
		bool f = mc2::log::enabled(mc2::log::Cat::Ui, mc2::log::Level::Info);
		if (f)
		{
			MC2_LOG(Ui, Info, "[CURSOR] initCursors name='{}' realHack='{}' fitPath='{}' fitOpen={:d} numCursors={:d} screenW={:d}", mc2::fmt::safe(cursorFileName), mc2::fmt::safe(realHackName), mc2::fmt::safe(static_cast<const char*>(cursorName)), mc2::fmt::as_i32(openResult), mc2::fmt::as_i32(numCursors), mc2::fmt::as_i32(static_cast<long>(Environment.screenWidth)));
			for (long s = 0; s < numCursors; s++)
				MC2_LOG(Ui, Info, "[CURSOR]   cursor[{:d}] texHandle={:d} texWidth={:d}", mc2::fmt::as_i32(s), mc2::fmt::as_u32(cursorInfos[s].textureHandle), mc2::fmt::as_u32(cursorInfos[s].textureWidth));


		}
	}

	cursorFile.close();
	nativeCursors.create(std::span<const unsigned long>(numFrames, numCursors), [this](size_t shape, size_t frame) -> SDL_Cursor* {
		const auto& info = cursorInfos[shape];
		const auto image = mc2::textures::image(info.textureHandle);
		if (!image || info.uWidth <= 0 || info.vHeight <= 0 || !info.textureWidth) return nullptr;
		std::vector<uint8_t> decoded;
		const std::vector<uint8_t>* pixels = &image->pixels;
		if (image->isCompressed) {
			if (!RenderCore::ktxDecodeBlocksToRgba8(*image, 0, decoded)) return nullptr;
			pixels = &decoded;
		} else if (image->vkFormat != 37 && image->vkFormat != 43) return nullptr;
		// FIT coordinates use the logical atlas size; cooked replacements may be larger.
		const int x0 = static_cast<int>(info.u * image->width / info.textureWidth);
		const int y0 = static_cast<int>(info.v * image->height / info.textureWidth);
		const int w = static_cast<int>(info.uWidth * image->width / info.textureWidth);
		const int h = static_cast<int>(info.vHeight * image->height / info.textureWidth);
		const int columns = w > 0 ? image->width / w : 0;
		if (w <= 0 || h <= 0 || columns <= 0) return nullptr;
		const int x = x0 + static_cast<int>(frame % columns) * w;
		const int y = y0 + static_cast<int>(frame / columns) * h;
		if (x < 0 || y < 0 || x + w > image->width || y + h > image->height ||
			pixels->size() < static_cast<size_t>(image->width) * image->height * 4) return nullptr;
		std::vector<uint8_t> rgba(static_cast<size_t>(info.uWidth) * info.vHeight * 4);
		for (long py = 0; py < info.vHeight; ++py)
			for (long px = 0; px < info.uWidth; ++px) {
				const size_t src = (x + px * w / info.uWidth + (y + py * h / info.vHeight) * image->width) * 4;
				const size_t dst = (px + py * info.uWidth) * 4;
				std::copy_n(pixels->data() + src, 4, rgba.data() + dst);
			}
		mc2::keyCursorPixels(rgba);
		SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(rgba.data(), static_cast<int>(info.uWidth),
			static_cast<int>(info.vHeight), 32, static_cast<int>(info.uWidth * 4), SDL_PIXELFORMAT_RGBA32);
		if (!surface) return nullptr;
		SDL_Cursor* cursor = SDL_CreateColorCursor(surface, static_cast<unsigned char>(mouseHS[shape][0]),
			static_cast<unsigned char>(mouseHS[shape][1]));
		SDL_FreeSurface(surface);
		if (mc2_diag::tagRequested("CURSOR")) {
			if (!cursor) MC2_LOG(Ui, Warn, "[CURSOR] native creation failed shape={} frame={}: {}", shape, frame, SDL_GetError());
		}
		return cursor;
	});
	size_t created = 0, failed = 0;
	for (const auto& shape : nativeCursors.shapes)
		for (const auto cursor : shape) cursor ? ++created : ++failed;
	if (mc2_diag::tagRequested("CURSOR"))
		MC2_LOG(Ui, Info, "[CURSOR] native shapes={} created={} failed={}", nativeCursors.shapes.size(), created, failed);
}

//---------------------------------------------------------------------------
void MouseCursorData::destroy (void)
{
	graphics::set_native_cursor(nullptr, false);
	for (const auto& shape : nativeCursors.shapes)
		for (SDL_Cursor* cursor : shape) if (cursor) SDL_FreeCursor(cursor);
	nativeCursors.shapes.clear();
	if (mc2UseAsyncMouse && mc2MouseThreadStarted)
		MouseTimerKill();

	if (numCursors)
	{
		userInput->mouseOff();
		
		cursorInfos.reset();

		numCursors = 0;
	}
}	

//---------------------------------------------------------------------------
bool UserInput::getKeyDown (gosEnum_KeyIndex index)
{
	if ((gos_GetKeyStatus(index) == KEY_HELD) || (gos_GetKeyStatus(index) == KEY_PRESSED))
		return true;

	return(false);
}

//---------------------------------------------------------------------------
void UserInput::mouseOn (void)				//Draw Mouse Cursor
{
	drawMouse = true;
	syncNativeCursor();
}

void UserInput::mouseOff (void)				//Don't Draw Mouse Cursor
{
	drawMouse = false;
	syncNativeCursor();
}

void UserInput::setMouseCursor (long state)
{
	if ((state < 0) || (state >= mState_NUMMOUSESTATES))
		return;

    // sebi nodebug
    /*
	//DEBUG!!!!!!!!!!!!!!!!!!!!!!
	if (state == mState_NORMAL)
		printf("Go Flash");
    */

	mouseState = state;
	syncNativeCursor();
}

void UserInput::syncNativeCursor()
{
	graphics::set_native_cursor(cursors ? cursors->nativeCursors.get(mouseState, mouseFrame) : nullptr, drawMouse);
}

//---------------------------------------------------------------------------
void UserInput::update (void)
{
	if (mc2UseAsyncMouse && !mc2MouseThreadStarted)
		MouseTimerInit();
		
	if (!mc2UseAsyncMouse && mc2MouseThreadStarted)
		MouseTimerKill();

	//-----------------------------
	// Save the last Mouse States
	lastLeftMouseButtonState = leftMouseButtonState;
	lastRightMouseButtonState = rightMouseButtonState;

	lastMouseXPosition = mouseXPosition;
	lastMouseYPosition = mouseYPosition;

	leftMouseJustUp = 0;
	rightMouseJustUp = 0;

	bool bWasDouble = leftDoubleClick;

	//------------------------------------------------------
	// Reset Frame dependant variables
	leftClick = rightClick = middleClick = false;
	leftDoubleClick = rightDoubleClick = middleDoubleClick = false;

	
	DWORD LEFT_MOUSE_CODE = VK_LBUTTON;
	DWORD RIGHT_MOUSE_CODE = VK_RBUTTON;
	if ( GetSystemMetrics(SM_SWAPBUTTON) )
	{
		RIGHT_MOUSE_CODE = VK_LBUTTON;
		LEFT_MOUSE_CODE = VK_RBUTTON;
	}

	//-----------------
	// Poll the mouse.
	DWORD buttonStates;
	gos_GetMouseInfo(&mouseXPosition,&mouseYPosition,(&mouseXDelta),(&mouseYDelta),(&mouseWheelDelta),&buttonStates);

	// MC2_MOUSE_RECON downstream probe: shows the FINAL cursor/pick pixel the
	// mission code consumes (norm * viewMul) plus the viewMul itself. If a dead
	// zone persists, this tells us whether the final pixel actually reaches the
	// bottom/right edge or saturates early -- isolating cursor-draw vs pick.
	if (false)
	{
		static float s_lx = -1.0f, s_ly = -1.0f;
		const float ddx = mouseXPosition - s_lx, ddy = mouseYPosition - s_ly;
		if (ddx*ddx + ddy*ddy > 0.0001f)
		{
			s_lx = mouseXPosition; s_ly = mouseYPosition;
			mc2::fmt::print_out("[MOUSE_RECON_DS v1] norm=({:.3f},{:.3f}) viewMul=({:.1f},{:.1f}) finalPixel=({:d},{:d})\n", mouseXPosition, mouseYPosition, viewMulX, viewMulY, mc2::fmt::as_i32(float2long(mouseXPosition * viewMulX)), mc2::fmt::as_i32(float2long(mouseYPosition * viewMulY)));
		}
	}

    // sebi: actually use gos
	leftMouseButtonState = buttonStates & 1 ?           MC2_MOUSE_DOWN : MC2_MOUSE_UP;
	middleMouseButtonState = (buttonStates & 2) >> 1 ?  MC2_MOUSE_DOWN : MC2_MOUSE_UP;
	rightMouseButtonState = (buttonStates & 4) >> 2 ?   MC2_MOUSE_DOWN : MC2_MOUSE_UP;;

    /*
	SHORT code = GetAsyncKeyState(LEFT_MOUSE_CODE);
	SHORT rCode = GetAsyncKeyState( RIGHT_MOUSE_CODE );
	SHORT mCode = GetAsyncKeyState( VK_MBUTTON );
	leftMouseButtonState = code ? MC2_MOUSE_DOWN : MC2_MOUSE_UP;
	rightMouseButtonState = rCode ? MC2_MOUSE_DOWN : MC2_MOUSE_UP;
	middleMouseButtonState = mCode ? MC2_MOUSE_DOWN : MC2_MOUSE_UP;
    */

	//---------------------------------------------------------
	// Adjust MouseWheelDelta to get old Broken Win2K values.
	mouseWheelDelta *= -100;

	//-------------------------------------
	// Determine drag, double click states
	if ((leftMouseButtonState == MC2_MOUSE_UP) && (lastLeftMouseButtonState == MC2_MOUSE_DOWN))
	{
		//--------------------------------------------------------------------
		// Just lifted the button.  Drags are OFF!  Double Click clock starts!
		wasLeftMouseDrag = leftMouseDrag;
		wasRightMouseDrag = rightMouseDrag;
		
		leftMouseDrag = false;
		mouseLeftUpTime += frameLength;
//		if ( !bWasDouble )
			leftMouseJustUp = 1;

		mouseLeftHeldTime = 0.f;
	}

	if ((leftMouseButtonState == MC2_MOUSE_UP) && (lastLeftMouseButtonState == MC2_MOUSE_UP))
	{
		//--------------------------------------------
		// We are still up.  Increment mouse up time.
		mouseLeftUpTime += frameLength;
		mouseLeftHeldTime = 0.f;
	}

	if ( gos_GetKeyStatus( KEY_LMOUSE ) == KEY_PRESSED /*code & 0x0001*/ ) // clicked
	{
		//-------------------------------------------------------------------------------
		// We just clicked down.  If mouseUpTime is < threshold, this was a double click
		if ( (mouseLeftUpTime > 0.0 ) && (mouseLeftUpTime < mouseDblClickThreshold) && !bWasDouble)
		{
			leftDoubleClick = true;
			mouseLeftUpTime = 0.f; // make sure if we hold it, we don't keep clicking
		}
		else  
		{
			leftClick = true;
			mouseLeftUpTime = 0.001f;			
		}

		// Anchor the drag at the CURRENT (this-frame) click position, not
		// lastMouseXPosition (the PREVIOUS frame's position). The press is
		// detected after this frame's poll, so mouseXPosition IS the click
		// point; lastMouseXPosition lags one frame. Using the stale value made
		// a click issued mid-motion (the normal case: move to target, click)
		// measure ~a frame of approach travel on the next held frame and trip a
		// phantom drag/band-select over the threshold. (Was lastMouseX/YPosition.)
		mouseDragX = mouseXPosition;
		mouseDragY = mouseYPosition;

		leftMouseDrag = 0;
		

	}

	else if (gos_GetKeyStatus( KEY_LMOUSE ) == KEY_HELD /*code & 0x8000*/) // held
	{
		mouseLeftUpTime = 5.0;
		//-----------------------------------------------------------------------
		// Down and still down should indicate dragging.  Check threshold of 
		// Deltas to see if this is TRUE IF AND ONLY IF we are NOT yet DRAGGING!
		if (!leftMouseDrag)
		{
			if ((fabs(mouseDragX - mouseXPosition) > mouseDragThreshold) ||
				(fabs(mouseDragY - mouseYPosition) > mouseDragThreshold))
			{
				//------------------
				// We are dragging.
				leftMouseDrag = true;
			}
		}

		mouseLeftUpTime = 0;
		mouseLeftHeldTime += frameLength;

	}

	if ((rightMouseButtonState == MC2_MOUSE_UP) && (lastRightMouseButtonState == MC2_MOUSE_DOWN))
	{
		//--------------------------------------------------------------------
		// Just lifted the button.  Drags are OFF!  Double Click clock starts!
		wasRightMouseDrag = rightMouseDrag;
		rightMouseDrag = false;
		mouseRightUpTime = 0.001f;
		rightMouseJustUp = true;

		mouseRightHeldTime = 0.f;
	}

	if ((rightMouseButtonState == MC2_MOUSE_UP) && (lastRightMouseButtonState == MC2_MOUSE_UP))
	{
		//--------------------------------------------
		// We are still up.  Increment mouse up time.
		mouseRightUpTime += frameLength;
		rightMouseDrag = 0;
		mouseRightHeldTime = 0.f;
	}

	if (gos_GetKeyStatus( KEY_RMOUSE ) == KEY_PRESSED /*rCode & 0x0001*/)
	{
		//-------------------------------------------------------------------------------
		// We just clicked down.  If mouseUpTime is < threshold, this was a double click
		if (mouseRightUpTime && mouseRightUpTime < mouseDblClickThreshold)
			rightDoubleClick = true;
		else
			rightClick = true;

		// Anchor the drag at the CURRENT (this-frame) click position, not
		// lastMouseXPosition (the PREVIOUS frame's position). The press is
		// detected after this frame's poll, so mouseXPosition IS the click
		// point; lastMouseXPosition lags one frame. Using the stale value made
		// a click issued mid-motion (the normal case: move to target, click)
		// measure ~a frame of approach travel on the next held frame and trip a
		// phantom drag/band-select over the threshold. (Was lastMouseX/YPosition.)
		mouseDragX = mouseXPosition;
		mouseDragY = mouseYPosition;

		rightMouseDrag = 0;
	}
	else if (gos_GetKeyStatus( KEY_RMOUSE ) == KEY_HELD /*rCode & 0x8000*/)
	{
		//-----------------------------------------------------------------------
		// Down and still down should indicate dragging.  Check threshold of 
		// Deltas to see if this is TRUE IF AND ONLY IF we are NOT yet DRAGGING!
		if (!rightMouseDrag)
		{
			if ((fabs(mouseDragX - mouseXPosition) > mouseDragThreshold) ||
				(fabs(mouseDragY - mouseYPosition) > mouseDragThreshold))
			{		 
				//------------------
				// We are dragging.
				rightMouseDrag = true;
			}
		}

		rightClick = true;

		mouseRightUpTime = 0;
		mouseRightHeldTime += frameLength;
	}

	if ((middleMouseButtonState == MC2_MOUSE_UP) && (lastMiddleMouseButtonState == MC2_MOUSE_DOWN))
	{
		//--------------------------------------------------------------------
		// Just lifted the button.  Drags are OFF!  Double Click clock starts!
		mouseMiddleUpTime = 0.0;
	}

	if ((middleMouseButtonState == MC2_MOUSE_UP) && (lastMiddleMouseButtonState == MC2_MOUSE_UP))
	{
		//--------------------------------------------
		// We are still up.  Increment mouse up time.
		mouseMiddleUpTime += frameLength;
	}

	if (gos_GetKeyStatus( KEY_MMOUSE ) == KEY_PRESSED /*mCode & 0x0001*/)
	{
		//-------------------------------------------------------------------------------
		// We just clicked down.  If mouseUpTime is < threshold, this was a double click
		if (mouseMiddleUpTime < mouseDblClickThreshold)
			middleDoubleClick = true;
		else
			middleClick = true;
	}

	if (cursors->getNumFrames( mouseState ) > 1 )
	{
		mouseFrameLength += frameLength;
		if (mouseFrameLength > cursors->frameLengths[mouseState] )
		{
			mouseFrame++;
			if (mouseFrame >= cursors->getNumFrames(mouseState))
			{
				mouseFrame = 0;
			}

			mouseFrameLength = 0.0;
		}
	}
	
	syncNativeCursor();
	//sebi: no ASYNC mouse data
	//if (mc2UseAsyncMouse)
	if (false)
	{
		//Wait for thread to finish.  Otherwise, we may move its data buffer halfway through!!
		while (mc2IsInMouseTimer)
			;

		//ONLY set the mouse BLT data at the end of each update.  NO MORE FLICKERING THEN!!!
		// BLOCK THREAD WHILE THIS IS HAPPENING
		mc2IsInDisplayBackBuffer = true;

		if (!mc2MouseData)
		{
			mc2MouseData = static_cast<MemoryPtr>(malloc(sizeof(DWORD) * MOUSE_WIDTH * MOUSE_WIDTH));
			memset(mc2MouseData,0,sizeof(DWORD) * MOUSE_WIDTH * MOUSE_WIDTH);
		}

		//Need to update the mouse in the mouse thread to inform it that the cursor
		// possibly changed size and shape.
		mc2MouseHotSpotX = cursors->getMouseHSX( mouseState );
		mc2MouseHotSpotY = cursors->getMouseHSY( mouseState );

		mc2MouseWidth = cursors->cursorInfos[mouseState].width();
		mc2MouseHeight = cursors->cursorInfos[mouseState].height();

		DWORD totalMouseFrames = cursors->getNumFrames(mouseState);
 		if ( totalMouseFrames > 1 )
		{
			long framesPerRow = cursors->cursorInfos[mouseState].textureWidth/cursors->cursorInfos[mouseState].width();
			int iIndex = mouseFrame % framesPerRow;
			int jIndex = mouseFrame / framesPerRow;

			float oldU = cursors->cursorInfos[mouseState].u;
			float oldV = cursors->cursorInfos[mouseState].v;

			float newU = (.1f + oldU)/cursors->cursorInfos[mouseState].textureWidth + (static_cast<float>(iIndex) * cursors->cursorInfos[mouseState].width()/cursors->cursorInfos[mouseState].textureWidth);
			float newV = (.1f + oldV)/cursors->cursorInfos[mouseState].textureWidth + static_cast<float>(jIndex) * cursors->cursorInfos[mouseState].height()/cursors->cursorInfos[mouseState].textureWidth;

			float newU2 = newU + (cursors->cursorInfos[mouseState].width() + .1)/cursors->cursorInfos[mouseState].textureWidth;
			float newV2 = newV + (cursors->cursorInfos[mouseState].height() + .1)/cursors->cursorInfos[mouseState].textureWidth;

			cursors->cursorInfos[mouseState].setNewUVs( newU, newV, newU2, newV2 );
			cursors->cursorInfos[mouseState].getData(mc2MouseData);
			cursors->cursorInfos[mouseState].u = oldU;
			cursors->cursorInfos[mouseState].v = oldV;
		}
		else if (totalMouseFrames)
		{
			cursors->cursorInfos[mouseState].getData(mc2MouseData);
		}

		//Unblock Thread
		mc2IsInDisplayBackBuffer = false;
	}
}

//---------------------------------------------------------------------------
void UserInput::initMouseCursors (mc2::czstring mouseFile)
{
	ZoneScopedN("UserInput::initMouseCursors");
	if (cursors)
	{
		ZoneScopedN("UserInput::initMouseCursors destroyOld");
		cursors->destroy();
		cursors.reset();
	}

	{
		ZoneScopedN("UserInput::initMouseCursors alloc");
		cursors = std::make_unique<MouseCursorData>();
	}
	gosASSERT(cursors != nullptr);
	
	{
		ZoneScopedN("UserInput::initMouseCursors initCursors");
		cursors->initCursors(mouseFile);
	}

	mouseFrame = 0;
	syncNativeCursor();
}
	
//---------------------------------------------------------------------------
void UserInput::setMouseScale (float scaleFactor)
{
	if (scaleFactor > 0.0f)
		mouseScale = scaleFactor;
}

//---------------------------------------------------------------------------
// CURSOR-TOPMOST-1: see header note on captureCursorQuad().
static gos_VERTEX* s_cursorCaptureQuad = nullptr;
static unsigned long s_cursorCaptureTex = 0;
static bool s_cursorCaptured = false;
static void emitCursorSprite (StaticInfo& ci)
{
	if (s_cursorCaptureQuad)
	{
		memcpy(s_cursorCaptureQuad, ci.location, 4 * sizeof(gos_VERTEX));
		s_cursorCaptureTex = mc2::textures::sourceId( ci.textureHandle );
		s_cursorCaptured = true;
	}
	else
		ci.render();
}
bool UserInput::captureCursorQuad (gos_VERTEX quad[4], unsigned long& gosTex)
{
	s_cursorCaptureQuad = quad;
	s_cursorCaptured = false;
	render();
	s_cursorCaptureQuad = nullptr;
	gosTex = s_cursorCaptureTex;
	return s_cursorCaptured;
}

//---------------------------------------------------------------------------
void UserInput::render (void)						//Last thing rendered.  Draws Mouse.
{
	// ---- MC2_LOG_CURSOR diagnostics (instrumentation only, no behavior change) ----
	// Writes to cursor_debug.log (stdout goes to NUL when MC2_LOG unset). Throttled
	// so we get a steady heartbeat without spamming. Shows the exact render-gate
	// state every frame the cursor would-or-wouldn't draw: drawMouse, mouseState,
	// numCursors, and the resolved texture handle for the current state.
	if (false)
	{

		static int s_frame = 0;
		if ((s_frame++ % 60) == 0)
		{
			long nc = cursors ? cursors->numCursors : -1;
			long th = (cursors && mouseState >= 0 && mouseState < nc)
			          ? cursors->cursorInfos[mouseState].textureHandle : -999;
			MC2_LOG(Ui, Info, "[CURSOR] f={:d} drawMouse={:d} mouseState={:d} numCursors={:d} texHandle={:d} screenW={:d}", mc2::fmt::as_i32(s_frame), mc2::fmt::as_i32(static_cast<int>(drawMouse)), mc2::fmt::as_i32(mouseState), mc2::fmt::as_i32(nc), mc2::fmt::as_i32(th), mc2::fmt::as_i32(static_cast<long>(Environment.screenWidth)));

		}
	}

	// MC2_OS_CURSOR: the native OS cursor is shown (see gos_render.cpp) -> skip the
	// software cursor draw entirely so there is exactly one cursor on screen. Use for
	// campaigns where the software cursor sprite doesn't render (old-era MC2X/MCO at
	// the 800-logical GUI tier). Cached: read once.
	static int s_osCursor = -1;
	if (s_osCursor < 0) s_osCursor = mc2::gate::get<"MC2_OS_CURSOR">() ? 1 : 0;
	if (s_osCursor) return;
	if (usesNativeCursor()) return;

    //sebi, always NOT async mouse
	//if (!mc2UseAsyncMouse)
	{
		if (drawMouse && cursors && mouseState >= 0 && mouseState < cursors->numCursors)
		{
			if (!s_cursorCaptureQuad)
				gos_SetHudScaleExempt(true);   // cursor sprite never shrunk by the HUD-fit scale
				// Cursor renders at raw physical pixel position, not HUD-logical coords.
			// HUD-inverse transform belongs only on the click-reception side.
			long mouseX = getRawMouseX();
			long mouseY = getRawMouseY();
			mouseX -= cursors->getMouseHSX( mouseState );
			mouseY -= cursors->getMouseHSY( mouseState );
	
			cursors->cursorInfos[mouseState].setLocation( mouseX, mouseY );
	
			long totalMouseFrames = cursors->getNumFrames(mouseState);
	
			if ( totalMouseFrames > 1 )
			{
				long framesPerRow = cursors->cursorInfos[mouseState].textureWidth/cursors->cursorInfos[mouseState].width();
				int iIndex = mouseFrame % framesPerRow;
				int jIndex = mouseFrame / framesPerRow;

				float oldU = cursors->cursorInfos[mouseState].u;
				float oldV = cursors->cursorInfos[mouseState].v;
	
				float newU = (.1f + oldU)/cursors->cursorInfos[mouseState].textureWidth + (static_cast<float>(iIndex) * cursors->cursorInfos[mouseState].width()/cursors->cursorInfos[mouseState].textureWidth);
				float newV = (.1f + oldV)/cursors->cursorInfos[mouseState].textureWidth + static_cast<float>(jIndex) * cursors->cursorInfos[mouseState].height()/cursors->cursorInfos[mouseState].textureWidth;
	
				float newU2 = newU + (cursors->cursorInfos[mouseState].width() + .1)/cursors->cursorInfos[mouseState].textureWidth;
				float newV2 = newV + (cursors->cursorInfos[mouseState].height() + .1)/cursors->cursorInfos[mouseState].textureWidth;
	
				cursors->cursorInfos[mouseState].setNewUVs( newU, newV, newU2, newV2 );
				emitCursorSprite( cursors->cursorInfos[mouseState] );
				cursors->cursorInfos[mouseState].u = oldU;
				cursors->cursorInfos[mouseState].v = oldV;



			}
			else
				emitCursorSprite( cursors->cursorInfos[mouseState] );
		}
	}
}	

//---------------------------------------------------------------------------
