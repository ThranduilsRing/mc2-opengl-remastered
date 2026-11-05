#include "mc2_czstring.h"
#include "Gates/preferences_legacy.h"
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
#define USERINPUT_H

#ifndef DSTD_H
#include"dstd.h"
#endif

#ifndef MATHFUNC_H
#include"mathfunc.h"
#endif

#ifndef UTILITIES_H
#include"utilities.h"
#endif

#include<string.h>
#include<gameos.hpp>
#include<memory>

//---------------------------------------------------------------------------
inline constexpr int MC2_MOUSE_UP = 0;
inline constexpr int MC2_MOUSE_DOWN = 1;

inline constexpr int MOUSE_WIDTH = 32;

// sebi 2026-04-21: was 256. Wolfman MC2X `cursors.fit` declares 262.
// Fixed-size arrays `mouseHS`/`numFrames`/`frameLengths` in MouseCursorData
// silently overran index 256+ when loading his cursor file → heap corruption →
// delayed crash in the readIdChar/readIdULong metadata loop. gosASSERT at
// userinput.cpp:91 is print-and-continue in release, so it never halted.
// `mState_NUMMOUSESTATES` (243) is the code-addressable state ceiling;
// this just gives the load-time arrays headroom for modded content.
inline constexpr int MAX_MOUSE_STATES = 512;

inline constexpr int mState_MOVE = 0;
inline constexpr int mState_MOVE_LOS = 1;
inline constexpr int mState_RUN = 2;
inline constexpr int mState_RUN_LOS = 3;
inline constexpr int mState_JUMP = 4;
inline constexpr int mState_JUMP_LOS = 5;
inline constexpr int mState_WALKWAYPT = 6;
inline constexpr int mState_WALKWAYPT_LOS = 7;
inline constexpr int mState_RUNWAYPT = 8;
inline constexpr int mState_RUNWAYPT_LOS = 9;
inline constexpr int mState_JUMPWAYPT = 10;
inline constexpr int mState_JUMPWAYPT_LOS = 11;
inline constexpr int mState_LINK = 12;
inline constexpr int mState_GENERIC_ATTACK = 13;
inline constexpr int mState_ATTACK_LOS = 14;
inline constexpr int mState_CURPOS_ATTACK = 15;
inline constexpr int mState_CURPOS_ATTACK_LOS = 16;
inline constexpr int mState_ENERGY_WEAPONS = 17;
inline constexpr int mState_ENERGY_WEAPONS_LOS = 18;
inline constexpr int mState_AIMED_ATTACK = 19;
inline constexpr int mState_AIMED_ATTACK_LOS = 20;
inline constexpr int mState_NORMAL = 21;
inline constexpr int mState_INFO = 22;
inline constexpr int mState_LAYMINES = 23;
inline constexpr int mState_ROTATE_CAMERA = 24;
inline constexpr int mState_UNCERTAIN_AIRSTRIKE = 25;
inline constexpr int mState_AIRSTRIKE = 26;
inline constexpr int mState_SENSORSTRIKE = 27;
inline constexpr int mState_VEHICLE = 28;
inline constexpr int mState_CANTVEHICLE = 29;
inline constexpr int mState_DONT = 30;
inline constexpr int mState_REPAIR = 31;
inline constexpr int mState_XREPAIR = 32;
inline constexpr int mState_EJECT = 33;
inline constexpr int mState_XEJECT = 34;
inline constexpr int mState_CAPTURE = 35;
inline constexpr int mState_XCAPTURE = 36;
inline constexpr int mState_SALVAGE = 37;
inline constexpr int mState_XSALVAGE = 38;
inline constexpr int mState_GUARD = 39;
inline constexpr int mState_GUARD_LOS = 39;
inline constexpr int mState_GUARDTOWER = 40;
inline constexpr int mState_GUARDTOWER_LOS = 41;
inline constexpr int mState_XMINES = 42;
inline constexpr int mState_LOGISTICS = 43;
inline constexpr int mState_TUTORIALS = 44;
inline constexpr int mState_LONGRNG_ATTACK = 45;
inline constexpr int mState_SHRTRNG_ATTACK = 46;
inline constexpr int mState_MEDRNG_ATTACK = 47;
inline constexpr int mState_WALK1 = 48;
inline constexpr int mState_WALKLOS1 = 80;
inline constexpr int mState_RUN1 = 112;
inline constexpr int mState_RUNLOS1 = 144;
inline constexpr int mState_JUMP1 = 176;
inline constexpr int mState_JUMPLOS1 = 208;
inline constexpr int mState_LONGRNG_LOS = 240;
inline constexpr int mState_MEDRNG_LOS = 241;
inline constexpr int mState_SHRTRNG_LOS = 242;

inline constexpr int mState_NUMMOUSESTATES = 243;		//Add states BETWEEN this one and above!!!
											//Used to keep from setting cursor to STUPID!!

inline constexpr double mouseFrameRate = 0.10;

extern volatile char mc2MouseHotSpotX;
extern volatile char mc2MouseHotSpotY;
extern volatile char mc2MouseWidth;
extern volatile char mc2MouseHeight;

extern volatile bool mc2MouseThreadStarted;	//Has the thread starting running yet?

extern volatile MemoryPtr mc2MouseData;

//---------------------------------------------------------------------------
class MouseCursorData
{
	protected:
		//---------------------------------------------------------------
		// Mouse textures are assumed 32x32.  Animating mice are stored
		// on the smallest texture that will hold their shape.  Thus if
		// have a mouse with four frames of animation it will be stored
		// on a 64x64.
		
		long numCursors;
		
		std::unique_ptr<StaticInfo[]> cursorInfos;	// gotta be pointer, so destructor is called before texmgr quits

		char mouseHS[MAX_MOUSE_STATES][2];
		unsigned long numFrames[MAX_MOUSE_STATES];
		float frameLengths[MAX_MOUSE_STATES];

		friend class UserInput;
		
	public:
		MouseCursorData (void)
		{
			init();
		}
		
		~MouseCursorData (void)
		{
			destroy();
		}
		
		void destroy (void);
		
		void init (void)
		{
			numCursors = 0;
			cursorInfos.reset();
			memset( mouseHS, 0, sizeof( mouseHS ) );
			memset( numFrames, 0, sizeof( numFrames ) );
			memset( frameLengths, 0, sizeof( frameLengths ) );
		}
		
		
		long getNumFrames (long state)
		{
			if ((state >= 0) && (state < numCursors))
				return numFrames[state]; // no animating for now

			return 0;
		}

		char getMouseHSX (long state)
		{
			if ((state >= 0) && (state < numCursors))
				return mouseHS[state][0];
				
			return 0;
		}
				
		char getMouseHSY (long state)
		{
			if ((state >= 0) && (state < numCursors))
				return mouseHS[state][1];
				
			return 0;
		}
		
				
		void initCursors (mc2::czstring cursorFile);
};

//---------------------------------------------------------------------------
class UserInput
{
	protected:

		//--------------------
		// Mouse Information
		float mouseXPosition;					//Current Mouse X Position.
		float mouseYPosition;					//Current Mouse Y position.

		int mouseXDelta;						//Amount mouse has moved in x since last poll
		int mouseYDelta;						//Amount mouse has moved in y since last poll

		int mouseWheelDelta;					//Amount Mouse wheel has moved since last poll

		long leftMouseButtonState;				//Mouse Current Left Button State
		long rightMouseButtonState;				//Mouse Current Right Button State
		long middleMouseButtonState;			//Mouse Current Middle Button State

		bool leftClick;							//Mouse has been left clicked
		bool rightClick;						//Mouse has been right clicked
		bool leftDoubleClick;					//Mouse has been left double clicked
		bool rightDoubleClick;					//Mouse has been right double clicked
		bool middleClick;						//Mouse has been middle clicked
		bool middleDoubleClick;					//Mouse has been middle Double Clicked

		bool leftMouseDrag;						//Mouse is left Dragging
		bool rightMouseDrag;					//Mouse is right Dragging

		bool wasLeftMouseDrag;					//Mouse is left Dragging
		bool wasRightMouseDrag;					//Mouse is right Dragging

		float mouseDragX;						//Where we started Dragging
		float mouseDragY;						//Where we started Dragging;

		float mouseDragThreshold;				//Distance mouse MUST move before a drag is assumed.
		float mouseDblClickThreshold;			//Time between clicks maximum to assume double click.

		float mouseLeftUpTime;					//Time since last left mouse up.
		float mouseRightUpTime;					//Time since last right mouse up.
		float mouseMiddleUpTime;				//Time since last right mouse up.

		float mouseLeftHeldTime;					//How long the mouse has been down
		float mouseRightHeldTime;					//How long the mouse has been down




		long lastLeftMouseButtonState;			//Last Left Mouse Button State
		long lastRightMouseButtonState;			//Last Right Mouse Button State
		long lastMiddleMouseButtonState;		//Last Middle Mouse Button State

		float lastMouseXPosition;				//Last Mouse X Position.
		float lastMouseYPosition;				//Last Mouse Y Position.
		
		std::unique_ptr<MouseCursorData> cursors;				//Stores the actual cursor data and Info
		
		bool drawTerrainPointer;				//Draw an inverse project cross
		long mouseState;						//Which cursor should I draw?
		long mouseFrame;						//current Mouse Frame
		DWORD mouseTextureHandle;				//current Mouse Texture handle.
		float mouseFrameLength;					//Time in Current Mouse Frame
		float mouseScale;						//Scale factor for cursor for depth cueing.

		float viewMulX;
		float viewAddX;
		float viewMulY;
		float viewAddY;

		bool leftMouseJustUp;					// leftButtonUp message equivalent
		bool rightMouseJustUp;					// right up equivalent
		
		DWORD	attilaIndex;					// Set to 0xffffffff if no Attila Present.
		
	public:

		//Need to have the mouse draw here
		static volatile bool drawMouse;			//Should I draw the Mouse Cursor?

		UserInput (void)
		{
			init();
		}

		~UserInput (void)
		{
			destroy();
		}

		void init (void)
		{
			mouseScale = 1.0f;
			mouseXPosition = mouseYPosition = 0.0;

			mouseXDelta = mouseYDelta = 
			mouseWheelDelta = 0;

			leftClick = rightClick = middleClick = false;
			leftMouseButtonState = rightMouseButtonState = middleMouseButtonState = MC2_MOUSE_UP;
			leftMouseDrag = rightMouseDrag = false;
			leftDoubleClick = rightDoubleClick = middleDoubleClick = false;

			mouseDragThreshold = 0.0166667f;
			mouseDblClickThreshold = 0.2f;

			mouseLeftUpTime = mouseRightUpTime = mouseMiddleUpTime = 0.0;
			mouseDragX = mouseDragY = 0.0;

			lastLeftMouseButtonState = lastRightMouseButtonState = lastMiddleMouseButtonState = MC2_MOUSE_UP;

			lastMouseYPosition = lastMouseXPosition = 0.0;
			
			drawTerrainPointer = false;
			mouseState = -1;
			mouseFrameLength = 0.0;

			viewMulX = viewMulY = viewAddX = viewAddY = 0.0f;
			leftMouseJustUp = 0;
			rightMouseJustUp = 0;
			
			attilaIndex = 0xffffffff;

			mouseLeftHeldTime = 0.f;
			mouseRightHeldTime = 0.f;

			cursors.reset();
		}

		void initMouseCursors (mc2::czstring mouseFile);
		
		void destroy (void)
		{
			cursors.reset();

			init();
		}

		void setViewport(float mulX, float mulY, float addX, float addY)
		{
			viewMulX = mulX;
			viewMulY = mulY;
			viewAddX = addX;
			viewAddY = addY;
		}

		//------------------------
		// Keyboard Functionality
		bool getKeyDown (gosEnum_KeyIndex index)
		{
			if ((gos_GetKeyStatus(index) == KEY_HELD) || (gos_GetKeyStatus(index) == KEY_PRESSED))
				return true;

			return(false);
		}

		bool leftShift (void)
		{
			return (getKeyDown(KEY_LSHIFT));
		}

		bool rightShift (void)
		{
			return (getKeyDown(KEY_RSHIFT));
		}

		bool shift (void)
		{
			return (leftShift() || rightShift());
		}

		bool leftAlt (void)
		{
			return (getKeyDown(KEY_LALT));
		}

		bool rightAlt (void)
		{
			return (getKeyDown(KEY_RALT));
		}

		bool alt (void)
		{
			return (leftAlt() || rightAlt());
		}

		bool leftCtrl (void)
		{
			return (getKeyDown(KEY_LCONTROL));
		}

		bool rightCtrl (void)
		{
			return (getKeyDown(KEY_RCONTROL));
		}

		bool ctrl (void)
		{
			return (leftCtrl() || rightCtrl());
		}

		//---------------------------------
		// Mouse Functionality
		void setMouseDoubleClickThreshold (float time)
		{
			mouseDblClickThreshold = time;
		}

		void setMouseDragThreshold (float distance)
		{
			mouseDragThreshold = distance;
		}

		// getMouseX/Y return the RAW screen pixel. These feed BOTH world-space
		// interactions (terrain pick, drag-select box, move commands) AND HUD
		// widget hit-tests, so they must stay raw -- applying the HUD-shrink
		// inverse here corrupts the world-space drag anchor (it is discontinuous
		// at the band edge and divides about the bottom-center anchor, throwing
		// the drag start toward a corner). HUD widgets that are DRAWN shrunk by
		// s_hud_scale instead call the dedicated getMouseHudX/Y below.
		long getMouseX (void)    { return float2long(mouseXPosition * viewMulX); }
		long getMouseY (void)    { return float2long(mouseYPosition * viewMulY); }
		long getRawMouseX (void) { return float2long(mouseXPosition * viewMulX); }
		long getRawMouseY (void) { return float2long(mouseYPosition * viewMulY); }
		// HUD-widget hit-test accessors: raw screen point run through
		// gos_HudInverseMousePoint (inverse of the bottom-band s_hud_scale shrink
		// in flushHUDBatch). Use ONLY for hit-testing widgets that are drawn
		// shrunk (the in-mission command bar + force-group icons) so a click on
		// the drawn-inward button maps back to its authored hit-rect. No-op
		// outside the rendered bottom band / when the HUD scale is inactive.
		long getMouseHudX (void) { float x = mouseXPosition * viewMulX, y = mouseYPosition * viewMulY; gos_HudInverseMousePoint(x, y); return float2long(x); }
		long getMouseHudY (void) { float x = mouseXPosition * viewMulX, y = mouseYPosition * viewMulY; gos_HudInverseMousePoint(x, y); return float2long(y); }
		float logicalW (void)    { return viewMulX; }   // logical 2D space size (cursor mapping)
		float logicalH (void)    { return viewMulY; }
		float normMouseX (void)  { return mouseXPosition; }
		float normMouseY (void)  { return mouseYPosition; }
		float realMouseX (void)  { return mouseXPosition * viewMulX; }
		float realMouseY (void)  { return mouseYPosition * viewMulY; }

		// MC2_VK_PICK_PROBE test hook: set the cached normalized 0..1 mouse
		// directly (as if a fresh gos_GetMouseInfo poll returned it), so the
		// probe can read getMouseX()/getMouseY() through the real viewMul
		// transform without a full userInput->update() side-effecting poll.
		void pickProbeSetNormalized (float nx, float ny)
		{
			mouseXPosition = nx;
			mouseYPosition = ny;
		}
		
		void setMousePos (float x, float y)
		{
			float xRes = 0.0f;
			float yRes = 0.0f;

			if ((fabs(viewMulX) > Stuff::SMALL) && 
				(fabs(viewMulY) > Stuff::SMALL))
			{
				xRes = x/viewMulX;
				yRes = y/viewMulY;
			}
			
			gos_SetMousePosition(xRes,yRes);
		}
		
		long getMouseXDelta (void)
		{
			return mouseXDelta;
		}

		long getMouseYDelta (void)
		{
			return mouseYDelta;
		}

		long getMouseWheelDelta (void)
		{
			return mouseWheelDelta;
		}

		// Consume the wheel for this frame so later handlers in the same frame
		// skip it (e.g. the formation-line spacing wheel must stop the overview
		// zoom from also firing). Mirrors clearLeftClick().
		void clearMouseWheelDelta (void)
		{
			mouseWheelDelta = 0;
		}

		long getMouseLeftButtonState (void)
		{
			return leftMouseButtonState;
		}

		long getMouseRightButtonState (void)
		{
			return rightMouseButtonState;
		}

		bool isLeftDrag (void)
		{
			return leftMouseDrag;
		}

		bool isRightDrag (void)
		{
			return rightMouseDrag;
		}

		bool wasLeftDrag (void)
		{
			return wasLeftMouseDrag;
		}

		bool wasRightDrag (void)
		{
			return wasRightMouseDrag;
		}

		float getMouseDragX (void)
		{
			return mouseDragX * viewMulX;
		}

		float getMouseDragY (void)
		{
			return mouseDragY * viewMulY;
		}

		// HUD-corrected drag-anchor (mirror of getMouseHudX/Y): for hit-testing the
		// shrunk in-mission HUD widgets (e.g. the unit-info scroll bar) whose rects
		// are authored 100% coords. No-op outside the rendered bottom band.
		float getMouseDragHudX (void)
		{
			float x = mouseDragX * viewMulX, y = mouseDragY * viewMulY;
			gos_HudInverseMousePoint(x, y);
			return x;
		}

		float getMouseDragHudY (void)
		{
			float x = mouseDragX * viewMulX, y = mouseDragY * viewMulY;
			gos_HudInverseMousePoint(x, y);
			return y;
		}

		bool isLeftClick (void) {
			return(leftClick);
		}

		// Consume the current left-click so later handlers in the same frame skip
		// it (e.g. the Tactical Overview squad-card click selects a group and must
		// stop the world-pick from also firing under the card).
		void clearLeftClick (void) {
			leftClick = false;
		}

		bool isRightClick (void) {
			return(rightClick);
		}

//		bool isLeftHeld() { 
//			return isLeftClick && lastLeftMouseButtonState == MC2_MOUSE_DOWN;
//		}

		bool isLeftDoubleClick (void)
		{
			return leftDoubleClick;
		}

		bool isRightDoubleClick (void)
		{
			return rightDoubleClick;
		}

		bool isMiddleClick (void)
		{
			return middleClick;
		}
		
		bool isMiddleDoubleClick (void)
		{
			return middleDoubleClick;
		}
		
		void setMouseCursor (long state);

		long getMouseCursor (void)
		{
			return (mouseState);
		}

		void setMouseFrame (long frameNum)
		{
			mouseFrame = frameNum;
		}

		long getMouseFrame (void)
		{
			return (mouseFrame);
		}
				
		void mouseOn (void);				//Draw Mouse Cursor
		void mouseOff (void);				//Don't Draw Mouse Cursor
		
		void pointerOn (void)
		{
			drawTerrainPointer = true;
		}
		
		void pointerOff (void)
		{
			drawTerrainPointer = false;
		}

		bool leftMouseReleased()
		{
			return leftMouseJustUp;
		}

		bool rightMouseReleased()
		{
			return rightMouseJustUp;
		}
		
		//------------------------------------------------------------------------------------
		// Used to make mouse move off into distance in perspective to help depth perception
		void setMouseScale (float scaleFactor);
		
		//----------------------------------------------------------
		// Attila Functionality.
		void addAttila (DWORD joyIndex)
		{
			attilaIndex = joyIndex;
		}
		
		float getAttilaXAxis (void)		//Left/Right Scroll
		{
			float result = 0.0f;
			if (attilaIndex != 0xffffffff)
				result = gosJoystick_GetAxis(attilaIndex, JOY_XAXIS);
				
			return result;			
		}
		
		float getAttilaYAxis (void)		//Up/Down Scroll
		{
			float result = 0.0f;
			if (attilaIndex != 0xffffffff)
				result = gosJoystick_GetAxis(attilaIndex, JOY_YAXIS);
				
			return result;			
		}
		
		float getAttilaRAxis (void)	//Camera Rotation
		{
			float result = 0.0f;
			if (attilaIndex != 0xffffffff)
				result = gosJoystick_GetAxis(attilaIndex, JOY_RZAXIS);
				
			return result;			
		}

		float getMouseRightHeld() 
		{
			return mouseRightHeldTime;
		}

		float getMouseLeftHeld() 
		{
			return mouseLeftHeldTime;
		}
		
		//------------------------
		// Poller
		void update (void);
		
		void render (void);						//Last thing rendered.  Draws Mouse.
		// CURSOR-TOPMOST-1: fills the current software-cursor sprite quad (raw
		// physical pixels, 4 gos_VERTEX with UVs) and its gos texture id WITHOUT
		// submitting a draw, so the ImGui foreground list (last 2D draw) can paint
		// the cursor above every ImGui HUD panel. false: no software cursor wanted.
		bool captureCursorQuad (gos_VERTEX quad[4], unsigned long& gosTex);
};

extern UserInput *userInput;
//---------------------------------------------------------------------------
#endif
