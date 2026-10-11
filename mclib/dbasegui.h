//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//--------------------------------------------------------------------------------------
//
// MechCommander 2
//
// This header contains the base classes for the GUI
//
// GUI is now single update driven.  An event comes in, the manager decides who its for
// and passes the event down.  Eveything still know how to draw etc.
//
// All drawing is done through gos_drawQuad and drawTriangle
//
// Basic cycle is 
//		call gObject->update with this frame's events.
//			This will check the events to see if any pertain to me.
//			Draw anything to the texture plane that needs drawing.
//			Call any code which the events might have trigged.
//		call gObject->render with this frame's events.
//			This draws the object to the screen.
//			called in order of depth.
// 
//--------------------------------------------------------------------------------------

#ifndef DBASEGUI_H
#define DBASEGUI_H
//--------------------------------------------------------------------------------------
// Include Files
//--------------------------------------------------------------------------------------
inline constexpr int gtINVALID = -1;
inline constexpr int gtBACKGROUND = 0;
inline constexpr int gtBUTTON = 1;
inline constexpr int gtTEXT = 2;
inline constexpr int gtHOTTEXT = 3;
inline constexpr int gtEDIT = 4;
inline constexpr int gtCHECKBOX = 5;
inline constexpr int gtSPINNER = 6;
inline constexpr int gtLISTBOX = 7;
inline constexpr int gtTABCTRL = 8;
inline constexpr int gtDRAGOBJECT = 9;
inline constexpr int gtMENU = 10;
inline constexpr int gtDIALOG = 11;

inline constexpr int aMSG_NOTHING = -1;
inline constexpr int aMSG_MOUSE_OVER = 0;
inline constexpr int aMSG_BUTTON_DOWN = 1;
inline constexpr int aMSG_BUTTON_UP = 2;
inline constexpr int aMSG_BUTTON_GRAY = 3;
inline constexpr int aMSG_MOUSE_MOVE = 4;
inline constexpr int aMSG_SELCHANGE = 5;
inline constexpr int aMSG_CANCEL = 6;
inline constexpr int aMSG_OK = 7;

inline constexpr int aBTN_NEWCAMPAIGN = 0;
inline constexpr int aBTN_LOADMISSION = 1;
inline constexpr int aBTN_SAVEMISSION = 2;
inline constexpr int aBTN_MULTIPLAYER = 3;
inline constexpr int aBTN_PREFERENCES = 4;
inline constexpr int aBTN_OPENINGCIN = 5;
inline constexpr int aBTN_QUIT = 6;
inline constexpr int aBTN_RETURNGAME = 7;

// Status bar interface colors
#define SD_YELLOW	((0x00) + (0xff<<8) + (0xff<<16) + (0xff<<24))
#define SD_RED		((0x00) + (0x00<<8) + (0xff<<16) + (0xff<<24))
#define SD_GREEN	((0x00) + (0xff<<8) + (0x00<<16) + (0xff<<24))
#define SD_BLUE		((0xff) + (0x00<<8) + (0x00<<16) + (0xff<<24))
#define SD_ORANGE	((0x00) + (0x7f<<8) + (0xff<<16) + (0xff<<24))
#define SD_WHITE	((0xff) + (0xff<<8) + (0xff<<16) + (0xff<<24))
#define SD_GRAY		((0x3f) + (0x3f<<8) + (0x3f<<16) + (0xff<<24))
#define SD_BLACK	((0x00) + (0x00<<8) + (0x00<<16) + (0xff<<24))

#define SX_WHITE	((0xff) + (0xff<<8) + (0xff<<16) + (0xbf<<24))
#define SX_CYAN		((0xff) + (0xff<<8) + (0x00<<16) + (0xbf<<24))
#define SX_YELLOW	((0x00) + (0xff<<8) + (0xff<<16) + (0xbf<<24))
#define SX_RED		((0x00) + (0x00<<8) + (0xff<<16) + (0xbf<<24))
#define SX_GREEN	((0x00) + (0xff<<8) + (0x00<<16) + (0xbf<<24))
#define SX_BLUE		((0xff) + (0x00<<8) + (0x00<<16) + (0xbf<<24))
#define SX_ORANGE	((0x00) + (0x7f<<8) + (0xff<<16) + (0xbf<<24))
#define SX_GRAY		((0x3f) + (0x3f<<8) + (0x3f<<16) + (0xbf<<24))
#define SX_BLACK	((0x00) + (0x00<<8) + (0x00<<16) + (0xbf<<24))

#define SB_YELLOW	((0x00) + (0xff<<8) + (0xff<<16) + (0x6f<<24))
#define SB_RED		((0x00) + (0x00<<8) + (0xff<<16) + (0x6f<<24))
#define SB_GREEN	((0x00) + (0xff<<8) + (0x00<<16) + (0x6f<<24))
#define SB_BLUE		((0xff) + (0x00<<8) + (0x00<<16) + (0x6f<<24))
#define SB_ORANGE	((0x00) + (0x7f<<8) + (0xff<<16) + (0x6f<<24))
#define SB_WHITE	((0xff) + (0xff<<8) + (0xff<<16) + (0x6f<<24))
#define SB_GRAY		((0x3f) + (0x3f<<8) + (0x3f<<16) + (0x6f<<24))
#define SB_BLACK	((0x00) + (0x00<<8) + (0x00<<16) + (0x6f<<24))

#define XP_YELLOW	((0x00) + (0xff<<8) + (0xff<<16) + (0x3f<<24))
#define XP_RED		((0x00) + (0x00<<8) + (0xff<<16) + (0x3f<<24))
#define XP_GREEN	((0x00) + (0xff<<8) + (0x00<<16) + (0x3f<<24))
#define XP_BLUE		((0xff) + (0x00<<8) + (0x00<<16) + (0x3f<<24))
#define XP_ORANGE	((0x00) + (0x7f<<8) + (0xff<<16) + (0x3f<<24))
#define XP_WHITE	((0xff) + (0xff<<8) + (0xff<<16) + (0x3f<<24))
#define XP_GRAY		((0x3f) + (0x3f<<8) + (0x3f<<16) + (0x3f<<24))
#define XP_BLACK	((0x00) + (0x00<<8) + (0x00<<16) + (0x3f<<24))


//--------------------------------------------------------------------------------------
#endif