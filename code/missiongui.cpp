//--------------------------------------------------------------------------------------
//
// MechCommander 2
//
// This header contains the mission classes for the GUI
//
// GUI is now single update driven.  An event comes in, the manager decides who its for
// and passes the event down.  Eveything still know how to draw etc.
//
// All drawing is done through gos_drawQuad and drawTriangle
//
// Basic cycle is 
//		call GObject->update with this frame's events.
//			This will check the events to see if any pertain to me.
//			Draw anything to the texture plane that needs drawing.
//			Call any code which the events might have trigged.
//		call GObject->render with this frame's events.
//			This draws the object to the screen.
//			called in order of depth.
// 
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//--------------------------------------------------------------------------------------
// Include Files
#ifndef MISSIONGUI_H
#include"missiongui.h"
#endif
#include "tacticaloverview.h"
#include"terrain_runtime.h"   // TERRAIN-RUNTIME-CONSUMER-WATER-1

// M1.6 + M2.6: IsStaticPropPickEnabled, lookupAtPixel, setLastGameplayPick,
// clearLastGameplayPick, getLastGameplayPick, IsObjectIdBufferEnabled,
// IsMechPickEnabled, IsMechPickDebugEnabled, IsMechPickPierceFogEnabled,
// IsGpuPickHoverEnabled, IsGpuPickHoverTraceEnabled.
#include "../RenderWorld/RenderWorld.h"
#include "../RenderWorld/ScreenPick.h"  // GPU_PICK_HOVER_DYNAMIC-1: screenPickCompute
#include "gameplay_pick.h"  // M2-pre: tryGameplayPick spine + GameplayPickRequest
#include "../GameAdapters/MechRenderAdapter.h"  // M2.6: findMechByHandle (forward-decls BattleMech)
#ifdef MC2_IS_EDITOR
#include "../editor/EditorObjectMgr.h"
#endif
#ifdef MC2_IMGUI
#include "../GuiRuntime/EditorInspector.h"  // Task 7: Ctrl+Shift+LMB inspector bridge
#include "../GameAdapters/StaticPropRenderAdapter.h"  // inspector: getRecipeShapeName
#include "imgui.h"
#endif
#include "../GameOS/gameos/gos_input.h"  // IMGUI-PAUSE-INPUT-FIX-1: input::setGameModalActive

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef SOUNDS_H
#include"sounds.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef GAMECAM_H
#include"gamecam.h"
#endif

#ifndef VERTEX_H
#include"vertex.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef ARTLRY_H
#include"artlry.h"
#endif

#ifndef LOGISTICSPILOT_H
#include"logisticspilot.h"
#endif

#ifndef LOGISTICSDATA_H
#include"logisticsdata.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef MOVEMGR_H
#include"movemgr.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef PREFS_H
#include"prefs.h"
#endif

#ifndef KEYBOARDREF_H
#include"keyboardref.h"
#endif

#include "../resource.h"

#include"platform_windows.h"

#include"gvehicl.h" // remove
#include"projectz_trace.h"
#include "gos_visual_diff.h"  // Stage 2.E: gate edge-scroll when capture enabled
#include "gos_profiler.h"     // diag: MIF.* Tracy zones for 20ms spike bisection

	static const char* terrainStr[NUM_TERRAIN_TYPES] = {
			"Blue Water",	//MC_BLUEWATER_TYPE
			"Green Water",	//MC_GREEN_WATER_TYPE
			"Mud",	//MC_MUD_TYPE
			"Moss",	//MC_MOSS_TYPE
			"Dirt",	//MC_DIRT_TYPE
			"Ash",	//MC_ASH_TYPE
			"Mountain",	//MC_MOUNTAIN_TYPE
			"Tundra",	//MC_TUNDRA_TYPE
			"Forest",	//MC_FORESTFLOOR_TYPE
			"Grass",	//MC_GRASS_TYPE
			"Concrete",	//MC_CONCRETE_TYPE
			"Cliff",	//MC_CLIFF_TYPE
			"Slimy",	//MC_SLIMY_TYPE
			"None"	//MC_NONE_TYPE
		};


extern void testKeyStuff();

extern long helpTextHeaderID;
extern long helpTextID;
extern CPrefs prefs;

extern bool invulnerableON;
extern bool MLRVertexLimitReached;


//--------------------------------------------------------------------------------------
// Globals
extern float frameLength;

extern void DEBUGWINS_print (const char* s, long window = 0);
extern void DEBUGWINS_setGameObject (long debugObj, GameObjectPtr obj);
extern void DEBUGWINS_toggle (bool* windowsOpen);
extern void DEBUGWINS_display (bool* windowsOpen);
extern void DEBUGWINS_viewGameObject (long debugObj);
extern MidLevelRenderer::MLRClipper * theClipper;

extern GameObjectPtr DebugGameObject[3];
#ifndef FINAL
extern float CheatHitDamage;
#endif

float MissionInterfaceManager::pauseWndVelocity = -50.f;

//--------------------------------------------------------------------------------------
//class MissionInterfaceManager : public InterfaceManager
//--------------------------------------------------------------------------------------

#define CTRL	0x10000000
#define SHIFT	0x01000000
#define ALT		0x00100000
#define WAYPT	0x20000000

#define ATTILA_FACTOR			1.0f
#define ATTILA_ROTATION_FACTOR	4.1f
#define ATTILA_THRESHOLD		0.01f

int MissionInterfaceManager::mouseX = 0;
int MissionInterfaceManager::mouseY = 0;
GameObject* MissionInterfaceManager::target = NULL;
MissionInterfaceManager* MissionInterfaceManager::s_instance = NULL;
gosEnum_KeyIndex MissionInterfaceManager::WAYPOINT_KEY = (gosEnum_KeyIndex)-1;

//--------------------------------------------------------------------------------------
// MISSION-START-HOVER-TARGET-LIFETIME-1
// Hover/pick cache state for updateTarget(). Hoisted to file scope (was function-local
// statics) so it can be invalidated at mission lifecycle boundaries. At mission start the
// first Mission::update -> updateTarget ran with a STALE target / hover cache from the
// PREVIOUS mission, then issued a virtual call through a freed object's vtable -> crash.
// invalidateHoverTarget() clears all of it on mission begin AND end.
static int           s_hoverLastMouseX = -1;
static int           s_hoverLastMouseY = -1;
static GameObject*   s_hoverCachedTarget = nullptr;
static unsigned long s_hoverCachedTargetWID = 0;   // watch-ID of the cached hover target — validate by handle before any deref of the raw pointer
static bool          s_hoverCacheValid  = false;
// Generation stamp: bumped on every mission begin/end invalidate. A cached target is only
// trusted while its stamp matches; any boundary crossing discards it cheaply.
static unsigned      s_hoverMissionSerial = 0;
// Suppress hover picking until the mission is fully live (objects loaded). Set true by
// invalidateHoverTarget() at mission END (and at init), cleared at mission BEGIN once the
// world is ready. While true updateTarget() is a no-op for picking.
static bool          s_hoverSuppressed = true;
// After arming, keep picking OFF for a few live frames: the first updates of a mission can
// still pick a not-yet-settled world (findObjectByMouse returns a stale/garbage pointer).
static long          s_hoverSettleFrames = 0;
// Watch-IDs of the persistent target + oldTargets, captured when each was set to a LIVE
// object. updateTarget re-validates via ObjectManager->getByWatchID() (registry lookup, no
// deref) before touching them, so a target whose object DIED since last frame is dropped,
// not crashed-on. MAX_ICONS == oldTargets[] size (forcegroupbar.h).
static unsigned long  s_targetWatchID = 0;
static unsigned long  s_oldTargetWatchID[MAX_ICONS] = {0};


extern bool drawTerrainTiles;
extern bool drawTerrainOverlays;
extern bool renderObjects;
extern bool drawTerrainGrid;
extern bool drawLOSGrid;
extern bool useFog;
extern bool useWaterInterestTexture;
extern bool useShadows;
extern bool useFaceLighting;
extern bool useVertexLighting;
extern bool renderTGLShapes;
extern bool useLeftRightMouseProfile;
bool drawGUIOn = true;				//Used to shut off GUI for Screen Shots and Movie Mode
bool toggledGUI = true;				//Used to tell if I shut the GUI off or if its in movieMode
long lightState = 0;
extern bool ShowMovers;
extern bool CullPathAreas;
extern bool ZeroHPrime;
extern bool CalcValidAreaTable;
extern bool EnemiesGoalPlan;
bool paintingMyVtol = false;

long			MissionInterfaceManager::OldKeys[MAX_COMMAND] = {-1};


MissionInterfaceManager::Command		MissionInterfaceManager::commands[MAX_COMMAND] = { 
		KEY_S,				mState_SHRTRNG_ATTACK, mState_SHRTRNG_LOS,		false,		&MissionInterfaceManager::attackShort, &MissionInterfaceManager::defaultAttack, 43200, 
		KEY_M,				mState_MEDRNG_ATTACK,  mState_MEDRNG_LOS,		false,		&MissionInterfaceManager::attackMedium, &MissionInterfaceManager::defaultAttack, 43201,
		KEY_L,				mState_LONGRNG_ATTACK, mState_LONGRNG_LOS,		false,		&MissionInterfaceManager::attackLong, &MissionInterfaceManager::defaultAttack, 43202,
		KEY_A,				mState_ENERGY_WEAPONS,	mState_ENERGY_WEAPONS_LOS,		false,		&MissionInterfaceManager::energyWeapons, &MissionInterfaceManager::energyWeapons, 43204,
		KEY_D,				mState_GENERIC_ATTACK, mState_ATTACK_LOS,		false,		&MissionInterfaceManager::defaultAttack,0, -1,
		KEY_J,				mState_JUMP1, mState_JUMP_LOS,		false,		&MissionInterfaceManager::jump, &MissionInterfaceManager::stopJump, 43205,
		KEY_J | WAYPT, 		mState_JUMPWAYPT,  mState_JUMPWAYPT_LOS,		false,		&MissionInterfaceManager::jump, &MissionInterfaceManager::stopJump, -1,
		KEY_C,				mState_CURPOS_ATTACK, mState_CURPOS_ATTACK_LOS,		false,		&MissionInterfaceManager::fireFromCurrentPos, &MissionInterfaceManager::stopFireFromCurrentPos, 43206,
		KEY_G,				mState_GUARD, mState_GUARD,				false,		&MissionInterfaceManager::guard, &MissionInterfaceManager::stopGuard, 43207,
		KEY_W,				mState_ENERGY_WEAPONS,	mState_ENERGY_WEAPONS_LOS,	false,		&MissionInterfaceManager::conserveAmmo, 0, -1,
		KEY_E,				-1,	-1,						false,		&MissionInterfaceManager::selectVisible, 0, 43209,
		KEY_F,				-1,	-1,						false,		&MissionInterfaceManager::forceShot, 0, 43210,
		KEY_HOME,				-1,	-1,						true,		&MissionInterfaceManager::cameraNormal,0, -1,
		KEY_F2,				-1,		-1,					true,		&MissionInterfaceManager::cameraDefault,0, -1,
		KEY_F3,				-1,		-1,					true,		&MissionInterfaceManager::cameraMaxIn,0, -1,
		KEY_F4,				-1,		-1,					true,		&MissionInterfaceManager::cameraMaxOut,0, -1,
		KEY_F5,				-1,		-1,					true,		&MissionInterfaceManager::cameraFour,0, -1,
		KEY_F2 | CTRL,		-1,		-1,					true,		&MissionInterfaceManager::cameraAssign0,0, -1,
		KEY_F3 | CTRL, 		-1,		-1,					true,		&MissionInterfaceManager::cameraAssign1,0, -1,
		KEY_F4 | CTRL, 		-1,		-1,					true,		&MissionInterfaceManager::cameraAssign2,0, -1,
		KEY_F5 | CTRL, 		-1,		-1,					true,		&MissionInterfaceManager::cameraAssign3,0, -1,
		KEY_NUMPAD2,		mState_DONT, mState_AIMED_ATTACK_LOS,		false,		&MissionInterfaceManager::aimLeg, 0, 43211,
		KEY_NUMPAD5,		mState_DONT,  mState_AIMED_ATTACK_LOS,		false,		&MissionInterfaceManager::aimArm, 0, 43212,
		KEY_NUMPAD8,		mState_DONT,  mState_AIMED_ATTACK_LOS,		false,		&MissionInterfaceManager::aimHead, 0, 43213,
		KEY_BACK,	-1, -1,							false,		&MissionInterfaceManager::removeCommand, 0, 43214,
		KEY_NEXT | 0x100,	-1,	-1,						false,		&MissionInterfaceManager::powerDown, 0, 43215,
		KEY_PRIOR| 0x100,	-1,	-1,						false,		&MissionInterfaceManager::powerUp, 0, 43216,
		KEY_END| 0x100,	mState_EJECT, mState_EJECT,		false,		&MissionInterfaceManager::eject, 0, 43217,
		KEY_SPACE, 			mState_RUN,	 mState_RUN_LOS,				false,		&MissionInterfaceManager::changeSpeed, &MissionInterfaceManager::stopChangeSpeed, 43218,
		KEY_SPACE | WAYPT, 	mState_RUNWAYPT, 	 mState_RUNWAYPT_LOS,					false,		&MissionInterfaceManager::changeSpeed, &MissionInterfaceManager::stopChangeSpeed, -1,
		KEY_UP | 0x100,				-1,	-1,						false,		&MissionInterfaceManager::scrollUp, 0,  IDS_HOTKEY_TRACKU,
		KEY_DOWN | 0x100,			-1,	-1,						false,		&MissionInterfaceManager::scrollDown, 0, IDS_HOTKEY_TRACKD,
		KEY_LEFT | 0x100,			-1,	-1,						false,		&MissionInterfaceManager::scrollLeft, 0, IDS_HOTKEY_TRACKL,
		KEY_RIGHT | 0x100,	 		-1,	-1,						false,		&MissionInterfaceManager::scrollRight, 0, IDS_HOTKEY_TRACKR,
		KEY_SUBTRACT,		-1,	-1,						false,		&MissionInterfaceManager::zoomOut,0, 43225,
		KEY_MINUS,			-1,	-1,						false,		&MissionInterfaceManager::zoomOut,0, -1,
		KEY_ADD,			-1,	-1,						false,		&MissionInterfaceManager::zoomIn,0, 43224,
		SHIFT | KEY_EQUALS,			-1,	-1,				false,		&MissionInterfaceManager::zoomIn,0, -1,
		KEY_LEFT | SHIFT | 0x100, 	-1,	-1,						false,		&MissionInterfaceManager::rotateLeft, 0, IDS_HOTKEY_ROTATEL,
		KEY_RIGHT | SHIFT | 0x100,	-1,	-1,						false,		&MissionInterfaceManager::rotateRight, 0, IDS_HOTKEY_ROTATER,
		KEY_UP | SHIFT | 0x100,	-1,	-1,						false,		&MissionInterfaceManager::tiltUp, 0, IDS_HOTKEY_TILTU,
		KEY_DOWN | SHIFT | 0x100,		-1,	-1,						false,		&MissionInterfaceManager::tiltDown, 0, IDS_HOTKEY_TILTD,
		KEY_HOME | 0x100,			-1,	-1,						false,		&MissionInterfaceManager::centerCamera, 0, -1,
		KEY_LEFT | CTRL,	-1,	-1,						false,		&MissionInterfaceManager::rotateLightLeft, 0, -1,
		KEY_RIGHT | CTRL,	-1,	-1,						false,		&MissionInterfaceManager::rotateLightRight, 0, -1,
		KEY_UP | CTRL,		-1,	-1,						false,		&MissionInterfaceManager::rotateLightUp, 0, -1,
		KEY_DOWN | CTRL,	-1,	-1,						false,		&MissionInterfaceManager::rotateLightDown, 0, -1,
		KEY_T | CTRL | ALT, -1,	-1,						true,		&MissionInterfaceManager::drawTerrain, 0, -1,
		KEY_O | CTRL | ALT, -1,	-1,						true,		&MissionInterfaceManager::drawOverlays, 0, -1,
		KEY_B | CTRL | ALT, -1,	-1,						true,		&MissionInterfaceManager::drawBuildings, 0, -1,
		CTRL | ALT | KEY_G, -1,	-1,						true,		&MissionInterfaceManager::showGrid, 0, -1,
		CTRL | ALT | KEY_Q, -1,	-1,						true,		&MissionInterfaceManager::recalcLights, 0, -1, 
		CTRL | ALT | KEY_F, -1,	-1,						true,		&MissionInterfaceManager::drawFog, 0, -1,
		CTRL | ALT | KEY_P, -1,	-1,						true,		&MissionInterfaceManager::usePerspective, 0, -1,
		CTRL | ALT | KEY_S, -1,	-1,						true,		&MissionInterfaceManager::drawTGLShapes, 0, -1,
		CTRL | ALT | KEY_V, -1,	-1,						true,		&MissionInterfaceManager::drawWaterEffects, 0, -1,
		CTRL | ALT | KEY_W, -1,	-1,						true,		&MissionInterfaceManager::recalcWater, 0, -1,
		CTRL | ALT | KEY_D, -1,	-1,						true,		&MissionInterfaceManager::drawShadows, 0,  -1,
		CTRL | ALT | KEY_L, -1,	-1,						true,		&MissionInterfaceManager::changeLighting, 0, -1,
		CTRL | ALT | KEY_Z, -1,	-1,						true,		&MissionInterfaceManager::toggleGUI, 0, -1,
		KEY_V,				-1,	-1,						true,		&MissionInterfaceManager::vehicleCommand, 0, 43222,
		KEY_F9,			-1,	-1,						true,		&MissionInterfaceManager::showObjectives, 0, 43226,
		KEY_ESCAPE,			-1,	-1,						true,		&MissionInterfaceManager::togglePause, 0, 43234,
		KEY_F1,				-1,	-1,					true,		&MissionInterfaceManager::toggleHotKeys, 0, -1,
		KEY_P,			-1,	-1,						true,		&MissionInterfaceManager::togglePause, 0, -1,
		KEY_TAB,			-1,	-1,						true,		&MissionInterfaceManager::switchTab, 0, -1,
		KEY_TAB | SHIFT,	-1,	-1,						true,		&MissionInterfaceManager::reverseSwitchTab, 0, -1,
		KEY_I,				-1, -1,						true,		&MissionInterfaceManager::infoCommand, &MissionInterfaceManager::infoButtonReleased, 43219,
		KEY_N,				-1, -1,						true,		&MissionInterfaceManager::gotoNextNavMarker, 0, -1,
		KEY_MULTIPLY,  mState_UNCERTAIN_AIRSTRIKE, mState_AIRSTRIKE, true, &MissionInterfaceManager::sendAirstrike, 0, 43220,
		KEY_DIVIDE, 		mState_SENSORSTRIKE, mState_SENSORSTRIKE, true, &MissionInterfaceManager::sendSensorStrike, 0, 43221,
		KEY_BACKSLASH,		-1, -1,				true,		&MissionInterfaceManager::toggleCompass, 0, 43223,
		ALT | KEY_SLASH,	-1, -1,						false,		&MissionInterfaceManager::quickDebugInfo, 0, -1,
		ALT | SHIFT | KEY_SLASH, -1, -1,				false,		&MissionInterfaceManager::setGameObjectWindow, 0, -1,
		ALT | CTRL | KEY_1, -1, -1,					false,		&MissionInterfaceManager::pageGameObjectWindow1, 0, -1,
		ALT | CTRL | KEY_2, -1, -1,					false,		&MissionInterfaceManager::pageGameObjectWindow2, 0, -1,
		ALT | CTRL | KEY_3, -1, -1,					false,		&MissionInterfaceManager::pageGameObjectWindow3, 0, -1,
		ALT | KEY_1,		-1,	-1,						false,		&MissionInterfaceManager::jumpToDebugGameObject1, 0, -1,
		ALT | KEY_2,		-1,	-1,						false,		&MissionInterfaceManager::jumpToDebugGameObject2, 0, -1,
		ALT | KEY_3,		-1,	-1,						false,		&MissionInterfaceManager::jumpToDebugGameObject3, 0, -1,
 		ALT | KEY_T,		-1, -1,						false,		&MissionInterfaceManager::teleport, 0, -1,
		ALT | KEY_W,		-1,	-1,						false,		&MissionInterfaceManager::toggleDebugWins, 0, -1,
		ALT | KEY_M,		-1,	-1,						false,		&MissionInterfaceManager::showMovers, 0, -1,
		ALT | KEY_C,		-1,	-1,						false,		&MissionInterfaceManager::cullPathAreas, 0, -1,
		ALT | KEY_Z,		-1,	-1,						false,		&MissionInterfaceManager::zeroHPrime, 0, -1,
		ALT | KEY_A,		-1,	-1,						false,		&MissionInterfaceManager::calcValidAreaTable, 0, -1,
		ALT | KEY_G,		-1,	-1,						false,		&MissionInterfaceManager::globalMapLog, 0, -1,
		ALT | KEY_B,		-1,	-1,						false,		&MissionInterfaceManager::brainDead, 0, -1,
		ALT | KEY_P,		-1,	-1,						false,		&MissionInterfaceManager::goalPlan, 0, -1,
		ALT | KEY_V,		-1,	-1,						false,		&MissionInterfaceManager::showVictim, 0, -1,
		ALT | CTRL | KEY_P,	-1, -1,						false,		&MissionInterfaceManager::enemyGoalPlan, 0, -1,
		ALT | KEY_4,	-1, -1,					false,		&MissionInterfaceManager::damageObject1, 0, -1,
		ALT | KEY_5,	-1, -1,					false,		&MissionInterfaceManager::damageObject2, 0, -1,
		ALT | KEY_6,	-1, -1,					false,		&MissionInterfaceManager::damageObject3, 0, -1,
		ALT | KEY_7,	-1, -1,					false,		&MissionInterfaceManager::damageObject4, 0, -1,
		ALT | KEY_8,	-1, -1,					false,		&MissionInterfaceManager::damageObject5, 0, -1,
		ALT | KEY_9,	-1, -1,					false,		&MissionInterfaceManager::damageObject6, 0, -1,
		ALT | KEY_0,	-1, -1,					false,		&MissionInterfaceManager::damageObject0, 0, -1,
		//ALT | KEY_8,	-1, -1,					false,		&MissionInterfaceManager::damageObject8, 0, -1,
		//ALT | KEY_9,	-1, -1,					false,		&MissionInterfaceManager::damageObject9, 0, -1,
		//ALT | KEY_0,	-1, -1,					false,		&MissionInterfaceManager::damageObject0, 0, -1,
		KEY_H,				-1, -1,						true,		&MissionInterfaceManager::toggleHoldPosition, 0, -1,
		KEY_RETURN,			-1, -1,						true,		&MissionInterfaceManager::handleChatKey, 0, IDS_HOTKEY_CHAT,
		SHIFT | KEY_RETURN,	-1, -1,				true,		&MissionInterfaceManager::handleTeamChatKey, 0, IDS_HOTKEY_CHAT_TEAM,
		KEY_E | SHIFT,				-1,	-1,						false,		&MissionInterfaceManager::addVisibleToSelection, 0, -1,
		KEY_EQUALS,			-1,	-1,				false,		&MissionInterfaceManager::zoomIn,0, -1,
		ALT | KEY_PERIOD, -1, -1,						false, &MissionInterfaceManager::rotateObjectLeft, 0, -1,
		ALT | KEY_COMMA, -1, -1,				false,	&MissionInterfaceManager::rotateObjectRight, 0, -1,
		KEY_PAUSE,		-1, -1,						true,		&MissionInterfaceManager::togglePause, 0, -1

};

extern bool drawTerrainGrid;
extern long 	turn;				//What frame of the scenario is it?

extern char DebugStatusBarString[256];

void MissionInterfaceManager::init (void)
{
	realRotation = 0.0;
	dragStart.Zero();
	dragEnd.Zero();
	isDragging = FALSE;
	terrainLineChanged = 0;

	// VPL-retirement Step 3 3b: invalidate the per-frame inverseProject cache.
	prevMouseX = -2147483647;
	prevMouseY = -2147483647;
	cachedWPos.Zero();
	inverseProjectCacheValid = false;
	
	for (long i=0;i<MAX_TEAMS;i++)
	{
		vTol[i] = 0;
		paintingVtol[i] = 0;
		vTolTime[i] = 0.0f;
		dustCloud[i] = NULL;
		recoveryBeam[i] = NULL;
		mechRecovered[i] = false;
		mechToRecover[i] = 0;
		vehicleID[i] = 0;
	}
	
	bPaused = 0;
	bPausedWithoutMenu = 0;
	resolution = Environment.screenWidth;
	s_instance = this;
	bEnergyWeapons = 0;
	zoomChoice = 2;
	memset( oldTargets, 0, sizeof( GameObject*) * MAX_ICONS );
	bDrawHotKeys = 0;
	
	hotKeyFont.init( IDS_HOT_KEY_FONT );
	hotKeyHeaderFont.init( IDS_HOT_KEY_HEADER_FONT );
	lastUpdateDoubleClick = 0;

	keyboardRef = new KeyboardRef;
	
	animationRunning = false;
	timeLeftToScroll = 0.0f;
	targetButtonId = -1;
	buttonNumFlashes = 0;
	guiFrozen = false;
	reinforcement = NULL;
	bForcedShot = false;
	bAimedShot = false;

	// MISSION-START-HOVER-TARGET-LIFETIME-1: ensure no stale hover/target carries into the
	// freshly-initialized GUI. Leave picking SUPPRESSED until armHoverTarget() runs at the
	// point the mission world is live.
	invalidateHoverTarget();
}

//--------------------------------------------------------------------------------------
// MISSION-START-HOVER-TARGET-LIFETIME-1
// Clear every piece of persistent hover/target/pick state and bump the generation serial.
// Called on mission BEGIN (before first updateTarget) and on mission END (teardown). After
// this, picking is suppressed until armHoverTarget() declares the mission live.
void MissionInterfaceManager::invalidateHoverTarget (void)
{
	target              = NULL;
	s_hoverCachedTarget = nullptr;
	s_hoverCachedTargetWID = 0;
	s_hoverCacheValid   = false;
	s_hoverLastMouseX   = -1;
	s_hoverLastMouseY   = -1;
	s_hoverSuppressed   = true;
	++s_hoverMissionSerial;
	if (s_instance)
		memset( s_instance->oldTargets, 0, sizeof( GameObject*) * MAX_ICONS );
}

//--------------------------------------------------------------------------------------
// MISSION-START-HOVER-TARGET-LIFETIME-1
// Declare the mission world fully live: re-enable hover picking. Safe to call repeatedly.
void MissionInterfaceManager::armHoverTarget (void)
{
	s_hoverSuppressed = false;
	s_hoverSettleFrames = 4; // give the live world a few frames before the first pick
}

#define TACMAP_ID		0
#define FLASH_JUMPERS	31798
bool MissionInterfaceManager::startAnimation(long buttonId,bool isButton,bool isPressed,float timeToScroll,long numFlashes)
{
	if (animationRunning)
		return false;
	else
	{
		animationRunning = true;
		timeLeftToScroll = timeToScroll;
		targetButtonId = buttonId;
		buttonNumFlashes = numFlashes;
		targetIsButton = isButton;
		targetIsPressed = isPressed;
		buttonFlashTime = 0.0f;

		if (buttonId < 0)		//It means flash TacMap Objective using the ABS of this objective number
		{
			controlGui.animateTacMap(buttonId,timeToScroll,numFlashes);

			animationRunning = true;
			timeLeftToScroll = timeToScroll;
			targetButtonId = TACMAP_ID;
			buttonNumFlashes = numFlashes;
			targetIsButton = false;
			targetIsPressed = false;
			buttonFlashTime = 0.0f;
		}

		if (buttonId == RPTOTAL_CALLOUT)		//RP total callout on Support Palette
		{
			controlGui.flashRPTotal(numFlashes);

			animationRunning = true;
			timeLeftToScroll = timeToScroll;
			targetButtonId = buttonId;
			buttonNumFlashes = numFlashes;
			targetIsButton = false;
			targetIsPressed = false;
			buttonFlashTime = 0.0f;
		}

		if (buttonId == FLASH_JUMPERS)		//Just flash the mech Icons.  Do not do anything else.
		{
			controlGui.forceGroupBar.flashJumpers(numFlashes);
			animationRunning = false;
		}
	}

	return true;
}

static char tutorialPlayerName[1024];
void MissionInterfaceManager::setTutorialText(const char *text)
{
	cLoadString(IDS_TUTORIAL,tutorialPlayerName,1023);
	controlGui.setChatText(tutorialPlayerName,text, 0x00ffffff, 0 );
}

// ---- MC2_MIF_SPLIT: split MissionInterfaceManager::update() (the 1K 43.9ms) --
// Default OFF. Prime suspect: Camera::inverseProject walking every quad on
// camera-motion cache misses -> walk-vs-cacheHit counter proves it.
//
// MISSION-INTERFACE-PERF-1 (MC2_IFACE_COST_SPLIT, default OFF, either env
// enables the whole thing): extends the split with
//   - a rollovers bucket (updateRollovers was untimed),
//   - ControlGui::update sub-phase fold-in (accumulators DEFINED in
//     controlgui.cpp, extern below -- TOBJSPLIT precedent, terrobj.cpp),
//   - one-line [IFACE_PERF v1] emission every 900 frames + at mission end
//     (MissionInterfaceManager::destroy) so short smoke runs produce numbers
//     without an interactive Tracy session.
// chrono (not RDTSC) is fine here: per-phase once-per-frame brackets, not
// per-element hot-loop probes. Zero chrono calls when both envs unset.
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cmath>     // GPU-GROUND-PICK-PARITY-1: sqrt/fabs for delta magnitude
#include <cstring>   // GPU-GROUND-PICK-PARITY-1: strcmp for lookupFailReason classify
#include "../GameOS/gameos/diagnostic_trace.h"  // CURSOR_TARGET trace (offscreen-pick recon)
// CGui sub-phase per-frame ns accumulators -- DEFINED in controlgui.cpp.
// Index order MUST match the MF_CG_* tail of MifPhase below (fold loop in
// mfFrameEnd is the single mapping point).
extern unsigned long long g_ifaceCgFrameNs[8];
namespace {
	enum MifPhase { MF_TOTAL=0, MF_INVPROJ, MF_LOS, MF_CTRLGUI, MF_UPDTGT, MF_POSTTGT, MF_DRAWBARS, MF_ROLLOVERS,
		// ControlGui::update sub-phases, folded from g_ifaceCgFrameNs[] --
		// order matches CgPhase in controlgui.cpp.
		MF_CG_PAUSE, MF_CG_BTNHOVER, MF_CG_ROSTER, MF_CG_MOVERSTATE, MF_CG_TACMAP, MF_CG_INFOWND, MF_CG_VEHTAB, MF_CG_FGBAR,
		MF_COUNT };
	const int MF_CG_FIRST = MF_CG_PAUSE;
	static const char* s_mfNames[MF_COUNT] = {"TOTAL","invProj","LOS","controlGui","updateTarget","postTarget","drawBars","rollovers",
		"cg.pauseWnd","cg.btnHover","cg.rosterScan","cg.moverState","cg.tacMap","cg.infoWnd","cg.vehicleTab","cg.fgBar"};
	static const bool s_mfOn = (getenv("MC2_MIF_SPLIT") != nullptr) || (getenv("MC2_IFACE_COST_SPLIT") != nullptr);
	static unsigned long long s_mfSum[MF_COUNT]={0}, s_mfMax[MF_COUNT]={0}, s_mfFrameNs[MF_COUNT]={0};
	static unsigned long long s_mfFrames=0, s_mfInvWalks=0, s_mfInvHits=0;
	// [IFACE_PERF v1] window accumulators (reset each emission).
	static unsigned long long s_mfWinSum[MF_COUNT]={0}, s_mfWinMax[MF_COUNT]={0};
	static unsigned long long s_mfWinFrames=0, s_mfWinWalks=0, s_mfWinHits=0;
	static const unsigned long long MF_WINDOW_FRAMES = 900ULL;
	static bool s_mfAtexit=false;
	static void mfEmit() {
		if (!s_mfOn) return;
		std::printf("[MIF_SPLIT v1] event=shutdown frames=%llu invProj_walks=%llu invProj_cacheHits=%llu",
			s_mfFrames, s_mfInvWalks, s_mfInvHits);
		for (int i=0;i<MF_COUNT;i++)
			std::printf(" %s={avg_us:%.1f,max_us:%.1f}", s_mfNames[i],
				s_mfFrames?(double)s_mfSum[i]/s_mfFrames/1000.0:0.0, (double)s_mfMax[i]/1000.0);
		std::printf("\n"); std::fflush(stdout);
	}
	// One [IFACE_PERF v1] line per window (or partial window at mission end).
	static void mfEmitWindow(const char* event) {
		if (!s_mfOn || !s_mfWinFrames) return;
		std::printf("[IFACE_PERF v1] event=%s frames=%llu invProj_walks=%llu invProj_cacheHits=%llu",
			event, s_mfWinFrames, s_mfWinWalks, s_mfWinHits);
		for (int i=0;i<MF_COUNT;i++)
			std::printf(" %s={avg_us:%.1f,max_us:%.1f}", s_mfNames[i],
				(double)s_mfWinSum[i]/s_mfWinFrames/1000.0, (double)s_mfWinMax[i]/1000.0);
		std::printf("\n"); std::fflush(stdout);
		for (int i=0;i<MF_COUNT;i++) { s_mfWinSum[i]=0; s_mfWinMax[i]=0; }
		s_mfWinFrames=0; s_mfWinWalks=0; s_mfWinHits=0;
	}
	struct MifScope {
		int idx; std::chrono::steady_clock::time_point t0;
		explicit MifScope(int i):idx(i){ if(s_mfOn) t0=std::chrono::steady_clock::now(); }
		~MifScope(){ if(!s_mfOn) return; s_mfFrameNs[idx]+=(unsigned long long)
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count(); }
	};
	static void mfFrameEnd(unsigned long long totalNs) {
		if (!s_mfOn) return;
		if (!s_mfAtexit) { s_mfAtexit=true; std::atexit(mfEmit); }
		++s_mfFrames; s_mfFrameNs[MF_TOTAL]=totalNs;
		for (int c=0;c<8;c++) { s_mfFrameNs[MF_CG_FIRST+c]=g_ifaceCgFrameNs[c]; g_ifaceCgFrameNs[c]=0; }
		for (int i=0;i<MF_COUNT;i++) {
			s_mfSum[i]+=s_mfFrameNs[i]; if(s_mfFrameNs[i]>s_mfMax[i]) s_mfMax[i]=s_mfFrameNs[i];
			s_mfWinSum[i]+=s_mfFrameNs[i]; if(s_mfFrameNs[i]>s_mfWinMax[i]) s_mfWinMax[i]=s_mfFrameNs[i];
			s_mfFrameNs[i]=0;
		}
		if (++s_mfWinFrames >= MF_WINDOW_FRAMES) mfEmitWindow("window");
	}
}

// ---- GPU-GROUND-PICK-PARITY-1 (S1): GPU-vs-CPU ground-pick oracle ----------
// Gate MC2_GPU_GROUND_PICK_PARITY (default OFF). Parity-ONLY: at each hover /
// click site the CPU Camera::inverseProject already produced the authoritative
// wPos; this oracle ALSO runs the GPU depth-unproject (lookupAtPixel) at the
// same cursor pixel, times both paths, and records the world-space position
// delta. The GPU result is NEVER consumed. Mirrors the MC2_MIF_SPLIT counter
// idiom above and the MC2_TERRAIN_PICK_PARITY discipline the recon cites
// (.claude/GPU-PICKING-RECON-1.md sec 6). Three measurements per the slice:
//   (a) delta distribution (world units) + >2wu flag lines with context,
//   (b) synchronous readback stall us vs the CPU quad-walk us (R1 verdict),
//   (c) buffer-unavailable frame count (pause/menu/ortho fallback -> R3 data).
namespace {
	static const bool s_gpParityOn = RenderWorld::IsGpuGroundPickParityEnabled();
	// Sample knob: run the GPU pick every Nth eligible site. Default 30 (NOT 1):
	// the synchronous single-pixel glReadPixels stalls the pipeline (measured
	// avg ~1.2ms, max tens of ms, worst-case >1s when the prior-frame FBO write
	// isn't yet visible), and firing it on EVERY camera-motion frame froze the
	// mc2_24 fly-through (heartbeat_freeze) in the S1 soak. A conservative
	// default sample rate keeps the oracle from freezing the game if someone
	// enables the gate without tuning; set MC2_GPU_GROUND_PICK_PARITY_SAMPLE=1
	// for a dense interactive parity capture (move the cursor by hand).
	static int s_gpSampleN = []() {
		const char* v = getenv("MC2_GPU_GROUND_PICK_PARITY_SAMPLE");
		int n = (v && *v) ? atoi(v) : 30;
		return n > 0 ? n : 30;
	}();
	static const char* s_gpMissionTag = []() {
		const char* v = getenv("MC2_GPU_GROUND_PICK_PARITY_TAG");
		return (v && *v) ? v : "?";
	}();
	// Center-probe knob: unattended smoke parks the cursor at (0,0), so the
	// primary cursor-driven oracle never samples real terrain (compared=0).
	// MC2_GPU_GROUND_PICK_PARITY_CENTER=1 adds a screen-CENTER parity probe on
	// camera-motion frames: CPU inverseProject at the center pixel vs GPU
	// depth-unproject at the same center pixel. This measures the actual
	// GPU-vs-CPU ground-pos delta over the terrain the camera is looking at
	// (incl. displaced terrain), which the parked-cursor path cannot. Same
	// throttle as the cursor path. Diagnostic-only; result never consumed.
	static const bool s_gpCenter = (getenv("MC2_GPU_GROUND_PICK_PARITY_CENTER") != nullptr);
	// Counters (process-lifetime; atexit summary).
	static unsigned long long s_gpSites=0;          // eligible parity sites seen
	static unsigned long long s_gpCompared=0;        // both CPU+GPU produced a point
	static unsigned long long s_gpGpuInvalid=0;      // GPU had no world pos (fallback)
	static unsigned long long s_gpStaleGen=0;        // GENERATION_MISMATCH (recycled-slot pixel)
	static unsigned long long s_gpSlotDead=0;        // SLOT_DEAD / INDEX_OUT_OF_RANGE / TERRAIN_RESERVED
	static unsigned long long s_gpNoDepth=0;         // isValid record but depth==0 (no worldPos)
	static unsigned long long s_gpFbUnavail=0;       // buffer unavailable (R3)
	static unsigned long long s_gpBg=0;              // background/sky pixel (no depth)
	static unsigned long long s_gpOver2wu=0;         // delta > 2 wu
	static double s_gpDeltaSum=0.0, s_gpDeltaMax=0.0; // horizontal (xy) delta wu
	static double s_gpDeltaZSum=0.0, s_gpDeltaZMax=0.0; // vertical (z) delta wu
	// Timing accumulators (ns).
	static unsigned long long s_gpCpuNs=0, s_gpCpuMaxNs=0, s_gpCpuSamples=0;
	static unsigned long long s_gpGpuNs=0, s_gpGpuMaxNs=0, s_gpGpuSamples=0;
	// Histogram buckets for the horizontal delta distribution (wu).
	// [0]<=0.5  [1]<=1  [2]<=2  [3]<=5  [4]<=20  [5]>20
	static unsigned long long s_gpHist[6]={0};
	// Center-probe counters (screen-center CPU-vs-GPU parity over real terrain).
	static unsigned long long s_gpcCompared=0, s_gpcInvalid=0, s_gpcBg=0;
	static double s_gpcDeltaSum=0.0, s_gpcDeltaMax=0.0;
	static double s_gpcDeltaZSum=0.0, s_gpcDeltaZMax=0.0;
	static unsigned long long s_gpcHist[6]={0};
	static bool s_gpAtexit=false;
	static void gpEmit() {
		if (!s_gpParityOn) return;
		const double cpuAvg = s_gpCpuSamples ? (double)s_gpCpuNs/s_gpCpuSamples/1000.0 : 0.0;
		const double gpuAvg = s_gpGpuSamples ? (double)s_gpGpuNs/s_gpGpuSamples/1000.0 : 0.0;
		const double dAvg   = s_gpCompared ? s_gpDeltaSum/(double)s_gpCompared : 0.0;
		const double dzAvg  = s_gpCompared ? s_gpDeltaZSum/(double)s_gpCompared : 0.0;
		std::fprintf(stderr,
			"[GPU_PICK_PARITY] event=shutdown mission=%s sites=%llu compared=%llu "
			"gpu_invalid=%llu buffer_unavailable=%llu background_px=%llu over_2wu=%llu\n",
			s_gpMissionTag, s_gpSites, s_gpCompared, s_gpGpuInvalid,
			s_gpFbUnavail, s_gpBg, s_gpOver2wu);
		std::fprintf(stderr,
			"[GPU_PICK_PARITY] invalid_reasons={stale_gen:%llu,slot_dead:%llu,no_depth:%llu}\n",
			s_gpStaleGen, s_gpSlotDead, s_gpNoDepth);
		std::fprintf(stderr,
			"[GPU_PICK_PARITY] delta_wu={xy_avg:%.3f,xy_max:%.3f,z_avg:%.3f,z_max:%.3f} "
			"hist_xy_wu=[<=0.5:%llu,<=1:%llu,<=2:%llu,<=5:%llu,<=20:%llu,>20:%llu]\n",
			dAvg, s_gpDeltaMax, dzAvg, s_gpDeltaZMax,
			s_gpHist[0], s_gpHist[1], s_gpHist[2], s_gpHist[3], s_gpHist[4], s_gpHist[5]);
		std::fprintf(stderr,
			"[GPU_PICK_PARITY] timing_us={cpu_invproj_avg:%.1f,cpu_invproj_max:%.1f,"
			"gpu_readback_avg:%.1f,gpu_readback_max:%.1f} cpu_samples=%llu gpu_samples=%llu\n",
			cpuAvg, (double)s_gpCpuMaxNs/1000.0, gpuAvg, (double)s_gpGpuMaxNs/1000.0,
			s_gpCpuSamples, s_gpGpuSamples);
		if (s_gpCenter) {
			const double cdAvg  = s_gpcCompared ? s_gpcDeltaSum/(double)s_gpcCompared : 0.0;
			const double cdzAvg = s_gpcCompared ? s_gpcDeltaZSum/(double)s_gpcCompared : 0.0;
			std::fprintf(stderr,
				"[GPU_PICK_PARITY] center_probe mission=%s compared=%llu invalid=%llu background_px=%llu "
				"delta_wu={xy_avg:%.3f,xy_max:%.3f,z_avg:%.3f,z_max:%.3f} "
				"hist_xy_wu=[<=0.5:%llu,<=1:%llu,<=2:%llu,<=5:%llu,<=20:%llu,>20:%llu]\n",
				s_gpMissionTag, s_gpcCompared, s_gpcInvalid, s_gpcBg,
				cdAvg, s_gpcDeltaMax, cdzAvg, s_gpcDeltaZMax,
				s_gpcHist[0], s_gpcHist[1], s_gpcHist[2], s_gpcHist[3], s_gpcHist[4], s_gpcHist[5]);
		}
		std::fflush(stderr);
	}
	// Center-screen parity probe. Caller (hover site) passes the CPU
	// inverseProject result at the CENTER pixel (ccx/y/z) + the center pixel
	// coords. Runs the GPU depth-unproject at the same center pixel and records
	// the delta. Throttled by the same sample counter. Result never consumed.
	static void gpCenterProbe(int cxPix, int cyPix, float ccx, float ccy, float ccz) {
		if (!s_gpParityOn || !s_gpCenter) return;
		if ((s_gpSites % (unsigned long long)s_gpSampleN) != 0) return;
		if (!RenderWorld::IsObjectIdBufferEnabled()) return;
		float vMulX, vMulY, vAddX, vAddY;
		gos_GetViewport(&vMulX, &vMulY, &vAddX, &vAddY);
		if (!(vMulX > 0.0f && vMulY > 0.0f)) return;
		RenderWorld::ScreenPickContext ctx;
		ctx.mouseX = cxPix; ctx.mouseY = cyPix;
		ctx.vMulX  = vMulX;  ctx.vMulY  = vMulY;
		ctx.vAddX  = vAddX;  ctx.vAddY  = vAddY;
		ctx.drawableWidth  = Environment.drawableWidth;
		ctx.drawableHeight = Environment.drawableHeight;
		RenderWorld::screenPickCompute(&ctx);
		RenderWorld::LookupResult res = RenderWorld::lookupAtPixel(ctx.glX, ctx.glY);
		if (!res.worldPosValid) {
			const char* r = res.lookupFailReason ? res.lookupFailReason : "";
			if (std::strcmp(r, "BACKGROUND_PIXEL") == 0) ++s_gpcBg; else ++s_gpcInvalid;
			return;
		}
		const float dx = res.worldX - ccx;
		const float dy = res.worldY - ccy;
		const float dz = res.worldZ - ccz;
		const double dHoriz = std::sqrt((double)dx*dx + (double)dy*dy);
		const double dVert  = std::fabs((double)dz);
		++s_gpcCompared;
		s_gpcDeltaSum += dHoriz; if (dHoriz > s_gpcDeltaMax) s_gpcDeltaMax = dHoriz;
		s_gpcDeltaZSum += dVert; if (dVert  > s_gpcDeltaZMax) s_gpcDeltaZMax = dVert;
		int b = dHoriz <= 0.5 ? 0 : dHoriz <= 1.0 ? 1 : dHoriz <= 2.0 ? 2
		        : dHoriz <= 5.0 ? 3 : dHoriz <= 20.0 ? 4 : 5;
		++s_gpcHist[b];
		if (dHoriz > 2.0) {
			std::fprintf(stderr,
				"[GPU_PICK_PARITY] center_over_2wu mission=%s center_px=(%d,%d) gl=(%d,%d) "
				"cpu=(%.1f,%.1f,%.1f) gpu=(%.1f,%.1f,%.1f) dxy=%.2f dz=%.2f\n",
				s_gpMissionTag, cxPix, cyPix, ctx.glX, ctx.glY,
				ccx, ccy, ccz, res.worldX, res.worldY, res.worldZ, dHoriz, dVert);
		}
	}
	// Called at each ground-pick site. cpuNs = measured CPU inverseProject cost
	// for this site; (cx,cy,cz) = the CPU wPos it produced. mouseX/Y = raw cursor.
	// site = short label ("hover"/"Lmove"/"Rmove") for >2wu flag context.
	static void gpParitySite(int mouseX, int mouseY, unsigned long long cpuNs,
	                         float cx, float cy, float cz, const char* site) {
		if (!s_gpParityOn) return;
		if (!s_gpAtexit) { s_gpAtexit = true; std::atexit(gpEmit); }
		++s_gpSites;
		// CPU timing (always recorded when we have a measurement).
		s_gpCpuNs += cpuNs; ++s_gpCpuSamples;
		if (cpuNs > s_gpCpuMaxNs) s_gpCpuMaxNs = cpuNs;
		// Sampling knob: skip the GPU readback on non-sampled sites.
		if ((s_gpSites % (unsigned long long)s_gpSampleN) != 0) return;
		if (!RenderWorld::IsObjectIdBufferEnabled()) { ++s_gpFbUnavail; return; }
		// Viewport transform (same proven path as the GPU hover block).
		float vMulX, vMulY, vAddX, vAddY;
		gos_GetViewport(&vMulX, &vMulY, &vAddX, &vAddY);
		if (!(vMulX > 0.0f && vMulY > 0.0f &&
		      mouseX >= 0 && mouseY >= 0 &&
		      mouseX < (int)vMulX && mouseY < (int)vMulY)) {
			++s_gpFbUnavail; return;   // offscreen / no viewport -> R3 fallback
		}
		RenderWorld::ScreenPickContext ctx;
		ctx.mouseX = mouseX; ctx.mouseY = mouseY;
		ctx.vMulX  = vMulX;  ctx.vMulY  = vMulY;
		ctx.vAddX  = vAddX;  ctx.vAddY  = vAddY;
		ctx.drawableWidth  = Environment.drawableWidth;
		ctx.drawableHeight = Environment.drawableHeight;
		RenderWorld::screenPickCompute(&ctx);
		const std::chrono::steady_clock::time_point g0 = std::chrono::steady_clock::now();
		RenderWorld::LookupResult res = RenderWorld::lookupAtPixel(ctx.glX, ctx.glY);
		const unsigned long long gpuNs = (unsigned long long)
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - g0).count();
		s_gpGpuNs += gpuNs; ++s_gpGpuSamples;
		if (gpuNs > s_gpGpuMaxNs) s_gpGpuMaxNs = gpuNs;
		// Classify the GPU result. lookupFailReason distinguishes buffer-absent
		// (NO_FBO_OR_TEX / NO_POSTPROCESS -> R3) from an in-scene sky/far pixel.
		if (!res.worldPosValid) {
			const char* r = res.lookupFailReason ? res.lookupFailReason : "NO_WORLD_POS";
			if (std::strcmp(r, "NO_FBO_OR_TEX") == 0 ||
			    std::strcmp(r, "NO_POSTPROCESS") == 0 ||
			    std::strcmp(r, "OID_BUFFER_DISABLED") == 0) {
				++s_gpFbUnavail;
			} else if (std::strcmp(r, "BACKGROUND_PIXEL") == 0) {
				++s_gpBg;           // sky / far plane under cursor (depth==0)
			} else if (std::strcmp(r, "GENERATION_MISMATCH") == 0) {
				++s_gpStaleGen;     // recycled-slot pixel: last-write-wins stale objectID
				++s_gpGpuInvalid;
			} else if (std::strcmp(r, "SLOT_DEAD") == 0 ||
			           std::strcmp(r, "INDEX_OUT_OF_RANGE") == 0 ||
			           std::strcmp(r, "TERRAIN_RESERVED") == 0) {
				++s_gpSlotDead;
				++s_gpGpuInvalid;
			} else {
				// isValid record parsed but depth==0 -> object pixel without a
				// resolvable world pos (rare); or an unclassified reason string.
				++s_gpNoDepth;
				++s_gpGpuInvalid;
			}
			return;
		}
		// Both paths produced a world point: record the delta. MC2 world convention
		// (matches EditorInspector consumer): x/y horizontal plane, z elevation.
		const float dx = res.worldX - cx;
		const float dy = res.worldY - cy;
		const float dz = res.worldZ - cz;
		const double dHoriz = std::sqrt((double)dx*dx + (double)dy*dy);
		const double dVert  = std::fabs((double)dz);
		++s_gpCompared;
		s_gpDeltaSum += dHoriz; if (dHoriz > s_gpDeltaMax) s_gpDeltaMax = dHoriz;
		s_gpDeltaZSum += dVert; if (dVert  > s_gpDeltaZMax) s_gpDeltaZMax = dVert;
		int b = dHoriz <= 0.5 ? 0 : dHoriz <= 1.0 ? 1 : dHoriz <= 2.0 ? 2
		        : dHoriz <= 5.0 ? 3 : dHoriz <= 20.0 ? 4 : 5;
		++s_gpHist[b];
		if (dHoriz > 2.0) {
			++s_gpOver2wu;
			std::fprintf(stderr,
				"[GPU_PICK_PARITY] over_2wu site=%s mission=%s screen=(%d,%d) gl=(%d,%d) "
				"cpu=(%.1f,%.1f,%.1f) gpu=(%.1f,%.1f,%.1f) dxy=%.2f dz=%.2f\n",
				site, s_gpMissionTag, mouseX, mouseY, ctx.glX, ctx.glY,
				cx, cy, cz, res.worldX, res.worldY, res.worldZ, dHoriz, dVert);
		}
	}
}

void MissionInterfaceManager::update (void)
{
	// IMGUI-PAUSE-INPUT-FIX-1: tell the input layer when the pause menu is up so its
	// clicks win over any open ImGui window (Graphics Options etc.). Set every frame,
	// before any early return below, so it clears the moment the menu closes.
	input::setGameModalActive(isPaused() && !isPausedWithoutMenu());

	const std::chrono::steady_clock::time_point _mifT0 =
		s_mfOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	if ( Environment.screenWidth != resolution )
	{
		swapResolutions();
	}

	bForcedShot = false;
	bAimedShot = false;

	if (eye && eye->inMovieMode)
	{
        SPEW(("DBG", "eye->inMovieMode: %d\n", eye->inMovieMode));
		drawGUIOn = false;
		userInput->mouseOff();
		
		//Need to check for ESC key so user can end IMI IMMEDIATELY.
		// After that check, just return.  Do NOT update any of the UI!!!!
		if (userInput->getKeyDown(KEY_ESCAPE))
			eye->forceMovieToEnd();

		controlGui.update(isPaused() && !isPausedWithoutMenu(), false );
			
		return;
	}
	else
	{
		drawGUIOn = toggledGUI;
		userInput->mouseOn();
	}
	
	if (animationRunning)
	{
		if (targetIsButton)
		{
			//Move mouse to correct position.
			ControlButton *targetButton = controlGui.getButton(targetButtonId);
			if (!targetButton)
			{
				animationRunning = false;
				return;
			}

			userInput->setMouseCursor(mState_TUTORIALS);

			//Get button position.
			float buttonPosX = (targetButton->location[0].x + targetButton->location[1].x +
								targetButton->location[2].x + targetButton->location[3].x) * 0.25f;

			float buttonPosY = (targetButton->location[0].y + targetButton->location[1].y +
								targetButton->location[2].y + targetButton->location[3].y) * 0.25f;

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
						controlGui.getButton( targetButtonId )->setColor( 0xffffffff );
						buttonFlashTime = 0.0f;
						buttonNumFlashes--;
					}
					else if ( buttonFlashTime > .25f )
					{
						controlGui.getButton( targetButtonId )->setColor( 0xff7f7f7f );
					}
				}
				else
				{
					//Flashing is done.  We now return you to your regularly scheduled program.
					animationRunning = false;
					controlGui.getButton( targetButtonId )->setColor( 0xffffffff );
					if (targetIsPressed)
						controlGui.pushButton(targetButtonId);
				}
			}
		}
		else
		{
			ControlGui::RectInfo *targetButton = controlGui.getRect(targetButtonId);
			if (!targetButton)
			{
				animationRunning = false;
			}

			userInput->setMouseCursor(mState_TUTORIALS);

			//Get button position.
			float buttonPosX = (targetButton->rect.left + targetButton->rect.right) * 0.5f;

			float buttonPosY = (targetButton->rect.top + targetButton->rect.bottom) * 0.5f;

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
						targetButton->color = 0xff000000;
						buttonFlashTime = 0.0f;
						buttonNumFlashes--;
					}
					else if ( buttonFlashTime > .25f )
					{
						targetButton->color = 0xffffffff;
					}
				}
				else
				{
					//Flashing is done.  We now return you to your regularly scheduled program.
					animationRunning = false;
					targetButton->color = 0xff000000;
				}
			}
		}
		
		return;		//Don't let anything else in the GUI run!!
	}
	
	//Tutorial has locked out the gui.  Move mouse to screen center and do nothing else!!
	if (guiFrozen)
	{
		userInput->setMouseCursor(mState_TUTORIALS);
		userInput->setMousePos(Environment.screenWidth * 0.5f,Environment.screenHeight * 0.5f);
		controlGui.update( isPaused() && !isPausedWithoutMenu(), false );
		return;
	}

	//---------------------------------------------------
	// Tactical overview: advance the blend-state and drive the camera from the
	// current blend. Hotkey detection lives in GameCamera::update (a proven
	// per-frame path with input already polled this frame).
	g_tacticalOverview.advance(frameLength);
	g_tacticalOverview.driveCamera(eye);

	// Overview squad-card click: select that force group and consume the click so
	// the world-pick under the card doesn't also fire. Runs before the mouse
	// handling below. Cards draw at raw screen coords (HUD-scale exempt).
	if ( g_tacticalOverview.active() && userInput->isLeftClick() && !userInput->isLeftDrag() )
	{
		const TacticalOverview::CardHit* hit = g_tacticalOverview.cardHitAt(
		             (float)userInput->getRawMouseX(), (float)userInput->getRawMouseY() );
		if ( hit )
		{
			if ( hit->forceGroup >= 0 )
			{
				selectForceGroup( hit->forceGroup, true );
			}
			else if ( hit->unit )
			{
				// Singleton card: select just that unit (deselect the rest).
				Team* pTeam = Team::home;
				if ( pTeam )
					for ( long i = 0; i < pTeam->getRosterSize(); i++ )
					{
						Mover* m = (Mover*)pTeam->getMover( i );
						if ( m && m->isOnGUI() && m->getCommander()->getId() == Commander::home->getId() )
							m->setSelected( 0 );
					}
				((Mover*)hit->unit)->setSelected( 1 );
			}
			userInput->clearLeftClick();
			g_tacticalOverview.armReleaseSuppression();	// stop the move on mouse-up
		}
	}

	// Formation line draw mode (MC2_TACMAP_FORMATION_LINE): owns LMB while
	// armed/dragging. Runs BEFORE the world mouse handlers below so the press
	// cannot select/deselect and the release cannot fire a normal move.
	// Rendering (ghost line + pips) stays in controlgui.
	if ( TacticalOverview::formationLineEnabled()
		&& g_tacticalOverview.flState() != TacticalOverview::FL_IDLE
		&& eye && Team::home )
	{
		long fmx = userInput->getMouseX();
		long fmy = userInput->getMouseY();

		if ( g_tacticalOverview.flState() == TacticalOverview::FL_ARMED )
		{
			if ( userInput->isLeftClick() )
			{
				// Legacy quadList picker: the worldToClipGL INVERSE is broken in
				// the game camera (far-plane unproject lands above the camera,
				// X response collapses - see inverseProject Phase 7B notes), so
				// the approx unprojectors cannot be used here. inverseProject is
				// the production picker; its O(quads) cost only runs during an
				// active formation drag.
				Stuff::Vector2DOf<long> sXY;
				sXY.x = fmx; sXY.y = fmy;
				Stuff::Vector3D w;
				eye->inverseProject( sXY, w );
				{
					// Snapshot selected friendly movers at drag start.
					Mover* flMovers[32];
					int nm = 0;
					Team* pTeam = Team::home;
					for ( long i = 0; i < pTeam->getRosterSize() && nm < 32; i++ )
					{
						Mover* pMover = (Mover*)pTeam->getMover( i );
						if ( pMover && pMover->isSelected()
							&& pMover->getCommander()->getId() == Commander::home->getId()
							&& pMover->getExists() && !pMover->isDestroyed() && !pMover->isDisabled() )
							flMovers[nm++] = pMover;
					}
					if ( nm == 0 )
					{
						g_tacticalOverview.flOnCancel();	// nothing selected
					}
					else
					{
						g_tacticalOverview.flSetMovers( flMovers, nm );
						g_tacticalOverview.flOnDragStart( w );
					}
					// Either way this press belongs to draw mode, not the
					// world: consume the click now and eat the matching
					// release later (suppression persists until consumed).
					userInput->clearLeftClick();
					g_tacticalOverview.armReleaseSuppression();
				}
			}
		}
		else // FL_DRAGGING
		{
			Stuff::Vector2DOf<long> sXY;
			sXY.x = fmx; sXY.y = fmy;
			Stuff::Vector3D w;
			eye->inverseProject( sXY, w );
			g_tacticalOverview.flOnDragMove( w );

				// Spacing wheel: stretches/tightens the squad spread while
				// drawing. Consume it so the overview zoom-on-wheel (later this
				// frame) does not also fire.
				long flWheel = userInput->getMouseWheelDelta();
				if ( flWheel != 0 )
				{
					g_tacticalOverview.flAdjustSpacing( flWheel );
					userInput->clearMouseWheelDelta();
				}

			// MC2_FL_TRACE=1: per-drag-frame coord audit. Round-trips the
			// unprojected world point back through the render projection; if
			// rx,ry != fmx,fmy the unproject/render matrix or pixel spaces
			// disagree (split-brain), if they match but the line looks wrong
			// the mouse coords themselves are off.
			{
				static const bool s_flTrace = ( getenv( "MC2_FL_TRACE" ) != NULL );
				if ( s_flTrace )
				{
					ModernClipResult rr = eye->projectModernClipGL( w );
					float vmx, vmy, vax, vay;
					gos_GetViewport( &vmx, &vmy, &vax, &vay );
					float rx = 0.0f, ry = 0.0f;
					if ( rr.clip.w > 0.0001f )
					{
						rx = vax + ( rr.clip.x / rr.clip.w * 0.5f + 0.5f ) * vmx;
						ry = vay + ( 1.0f - ( rr.clip.y / rr.clip.w * 0.5f + 0.5f ) ) * vmy;
					}
					fprintf( stderr, "[FLTRACE] mouse=(%ld,%ld) raw01=(%.4f,%.4f) world=(%.1f,%.1f,%.1f) reproj=(%.1f,%.1f) vp=(%.0f,%.0f,%.0f,%.0f) env=(%ld,%ld)\n",
						fmx, fmy,
						userInput->realMouseX() / ( vmx > 1.0f ? vmx : 1.0f ),
						userInput->realMouseY() / ( vmy > 1.0f ? vmy : 1.0f ),
						w.x, w.y, w.z, rx, ry, vmx, vmy, vax, vay,
						(long)Environment.screenWidth, (long)Environment.screenHeight );
					fflush( stderr );
				}
			}

			if ( userInput->leftMouseReleased() )
			{
				// Accidental click guard: tiny line = cancel, no orders.
				const float kMinLineLenWorld = 50.0f;	// tune later
				float ldx = g_tacticalOverview.flEnd().x - g_tacticalOverview.flStart().x;
				float ldy = g_tacticalOverview.flEnd().y - g_tacticalOverview.flStart().y;
				if ( ldx * ldx + ldy * ldy < kMinLineLenWorld * kMinLineLenWorld )
				{
					g_tacticalOverview.flOnCancel();
				}
				else
				{
					// v1 assignment: iterate snapshot order, each mover takes
					// nearest free slot. Orders FIRST, then flOnRelease
					// (release clears the snapshot).
					Stuff::Vector3D slots[32];
					int ns = g_tacticalOverview.flComputeSlots( slots, 32 );
					// Endpoints sit on the terrain; interpolated slots can
					// float over dips - drop each onto the surface.
					if ( land )
						for ( int s = 0; s < ns; s++ )
							slots[s].z = land->getTerrainElevation( slots[s] );
					bool slotUsed[32] = { false };
					int nm = g_tacticalOverview.flMoverCount();
					for ( int i = 0; i < nm; i++ )
					{
						Mover* pMover = g_tacticalOverview.flMover( i );
						// Revalidate: mover may have died mid-drag.
						if ( !pMover || !pMover->getExists()
							|| pMover->isDestroyed() || pMover->isDisabled() )
							continue;
						int   best = -1;
						float bestD2 = 0.0f;
						Stuff::Vector3D mp = pMover->getPosition();
						for ( int s = 0; s < ns; s++ )
						{
							if ( slotUsed[s] ) continue;
							float dx = slots[s].x - mp.x, dy = slots[s].y - mp.y;
							float d2 = dx * dx + dy * dy;
							if ( best < 0 || d2 < bestD2 ) { best = s; bestD2 = d2; }
						}
						if ( best < 0 ) break;
						slotUsed[best] = true;

						LocationNode path;
						path.location = slots[best];
						path.run = true;
						path.next = NULL;
						TacticalOrder tacOrder;
						tacOrder.init( ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_MOVETO_POINT, false );
						tacOrder.initWayPath( &path );
						tacOrder.moveParams.wait = false;
						tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
						tacOrder.pack( NULL, NULL );
						// Single-unit dispatch on purpose: handleOrders/
						// calcMoveGoals would re-cluster the slots.
						pMover->handleTacticalOrder( tacOrder );
					}
					g_tacticalOverview.flOnRelease();
					soundSystem->playDigitalSample( BUTTON5 );
				}
			}
		}
	}

	//---------------------------------------------------
	// Per Andy G.  One check per frame saves log file!
	bool shiftDn = userInput->shift();
	bool altDn = userInput->alt();
	bool ctrlDn = userInput->ctrl();

	//Cheats
	// Comment out here if FINAL
#ifndef FINAL
	#define MC2_DAMAGE_HEAD			0
	#define MC2_DAMAGE_LARM			1
	#define MC2_DAMAGE_RARM			2
	#define MC2_DAMAGE_LTORSO		3
	#define MC2_DAMAGE_RTORSO		4
	#define MC2_DAMAGE_CTORSO		5
	#define MC2_DAMAGE_LLEG			6
	#define MC2_DAMAGE_RLEG			7
	#define MC2_DAMAGE_RLTORSO		8
	#define MC2_DAMAGE_RRTORSO		9	
	#define MC2_DAMAGE_RCTORSO		10	
			
	DWORD CheatCommand = 0xffffffff;
	if (userInput->getKeyDown(KEY_1) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_HEAD;
	if (userInput->getKeyDown(KEY_2) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_LARM;
	if (userInput->getKeyDown(KEY_3) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_RARM;
	if (userInput->getKeyDown(KEY_4) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_LTORSO;
	if (userInput->getKeyDown(KEY_5) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_RTORSO;
	if (userInput->getKeyDown(KEY_6) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_CTORSO;
	if (userInput->getKeyDown(KEY_7) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_LLEG;
	if (userInput->getKeyDown(KEY_8) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_RLEG;
	if (userInput->getKeyDown(KEY_9) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_RLTORSO;
	if (userInput->getKeyDown(KEY_0) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_RRTORSO;
	if (userInput->getKeyDown(KEY_MINUS) && shiftDn && ctrlDn && altDn)
		CheatCommand = MC2_DAMAGE_RCTORSO;

	WeaponShotInfo cheatHit;

	switch (CheatCommand)
	{
		case 0xffffffff:
			break;
			
		case MC2_DAMAGE_HEAD:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_HEAD,0.0f);
		break;
		
		case MC2_DAMAGE_LARM:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_LARM,0.0f); 
		break;
		
 		case MC2_DAMAGE_RARM:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_RARM,0.0f); 
 		break;
		
    	case MC2_DAMAGE_LTORSO:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_LTORSO,0.0f); 
 		break;
		
    	case MC2_DAMAGE_RTORSO:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_RTORSO,0.0f); 
 		break;
		  
    	case MC2_DAMAGE_CTORSO:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_CTORSO,0.0f); 
 		break;
		
    	case MC2_DAMAGE_LLEG:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_LLEG,0.0f); 
 		break;
		
    	case MC2_DAMAGE_RLEG:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_RLEG,0.0f); 
 		break;
		
    	case MC2_DAMAGE_RLTORSO:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_RLTORSO,0.0f); 
 		break;
		
    	case MC2_DAMAGE_RRTORSO:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_RRTORSO,0.0f); 
 		break;
		
    	case MC2_DAMAGE_RCTORSO:
			cheatHit.init(0,-2,CheatHitDamage,MECH_ARMOR_LOCATION_RCTORSO,0.0f); 
 		break;
    }
	
	//Call handleWeaponHit for each selected Mech!
	if (CheatCommand != -1)
	{
		Team* pTeam = Team::home;
		for (long i = 0; i < pTeam->getRosterSize(); i++)
		{
			Mover* pMover = (Mover*)pTeam->getMover( i );
			if ( pMover->isSelected() && pMover->isMech())
			{
				pMover->handleWeaponHit(&cheatHit);
			}
		}
	}
#endif
	
	if ( bDrawHotKeys )
	{
		keyboardRef->update();
		//sebi: also check keys, otherwise no way to exit from keyboardref screen (when pressing F1)
		update(false, false, -1, -1, 0,  false);
		return;
	}

	//-------------------
	// Mouse Checks Next
	mouseX = userInput->getMouseX();
	mouseY = userInput->getMouseY();

	bool bGui = false;

	// Raw-space GUI test. ae7e1794 switched the bGui hit-test below to
	// HUD-inverse space so a click on a drawn-shrunk command-bar widget would
	// correctly register / suppress a spurious world order. But gos_HudInverseMousePoint
	// maps the WHOLE s_hud_scale shrink band (raw y >= ~0.66*sh) outward toward the
	// bottom-center anchor, so an OPEN world pixel sitting above the actual drawn
	// chrome (but inside the shrink band) gets pushed DOWN into an authored widget
	// rect -> inRegion(HUD-inverse) wrongly returns true over the lower-center world.
	// That over-claim made updateTarget zero a genuinely-hovered unit (target=0 at
	// the `if(bGui)` below), so the hovered mech/vehicle/building name stopped
	// rendering for units in the lower screen band (regression since 2026-06-10).
	// findObjectByMouse picks in RAW space (what the user sees), so the name/target
	// gate must also use a RAW-space GUI test; the drawn-shrunk widget rect is a
	// subset of its authored rect, so raw-in-authored-region still returns true when
	// the cursor is genuinely over a drawn widget. bGui (HUD-inverse) is kept for
	// order/drag suppression so the ae7e1794 widget-click fix is unchanged.
	bool bRawGui = controlGui.inRegion( mouseX, mouseY, isPaused() && !isPausedWithoutMenu() );

	// check and see if its in the control area
	// HUD command bar draws shrunk -> test the region in HUD-inverse space (the
	// raw mouseX/mouseY locals stay raw for terrain inverseProject below).
	if ( controlGui.inRegion( userInput->getMouseHudX(), userInput->getMouseHudY(), isPaused() && !isPausedWithoutMenu() ) )
	{
		bGui = true;
		if ( userInput->isLeftClick() && !userInput->isLeftDrag() )
		{
			dragStart.x = 0.f;
			dragStart.y = 0.f;
		}
	}
	// Clear stale help text only when the cursor is genuinely over a drawn HUD
	// widget (raw space). Using bGui (HUD-inverse) here would leave the previous
	// frame's text up over the over-claimed world band; using bRawGui keeps the
	// hovered-unit name path (set in updateTarget below) authoritative for the world.
	if ( !bRawGui )
	{
		helpTextHeaderID = helpTextID = 0;
	}

	if (bGui)
		userInput->setMouseCursor( mState_NORMAL );

	// find out where the mouse is
	Stuff::Vector2DOf<long>	mouseXY;
	mouseXY.x = mouseX;
	mouseXY.y = mouseY;

	// VPL-retirement Step 3 3b: mandatory per-frame delta-cache. This is the
	// SOLE production Camera::inverseProject caller and runs every frame; the
	// repointed inverseProject (Step 3 3a) walks every quad. Skip the walk
	// when neither the cursor pixel (exact, >=1px, no tolerance) nor the
	// camera world->clip matrix changed since the last real projection.
	const Stuff::Matrix4D& curW2C = eye->getWorldToClip();
	const bool mouseSame = (mouseX == prevMouseX) && (mouseY == prevMouseY);
	const bool camSame   = inverseProjectCacheValid &&
		(memcmp(&cachedWorldToClip, &curW2C, sizeof(Stuff::Matrix4D)) == 0);
	if (inverseProjectCacheValid && mouseSame && camSame)
	{
		// Cursor and camera both unchanged - reuse the cached world point.
		wPos = cachedWPos;
		if (s_mfOn) { ++s_mfInvHits; ++s_mfWinHits; }
	}
	else
	{
		// Coarse cache-miss-frames-only zone (not per-quad): the O(quads)
		// inverseProject walk was invisible in Tracy without it.
		ZoneScopedN("MIF.InverseProject");
		MifScope _mInv(MF_INVPROJ);
		if (s_mfOn) { ++s_mfInvWalks; ++s_mfWinWalks; }
		// Per-frame cursor world position. screenToGroundPlaneApprox (the O(1)
		// inverse-clip/z=0 unproject) shares the same broken worldToClipGL inverse
		// as the chunk raycast picker (collapsed X response / wrong eye), so it
		// returned a near-constant garbage world point -> the hover passability
		// check flagged walkable terrain as a red-X "dead area" across the screen,
		// and dragStart (= wPos) anchored the select box off-screen. Use the
		// legacy forward-projection inverseProject instead (correct, same picker
		// the discrete move click uses). The delta-cache above limits this to
		// cursor/camera-change frames; the O(quads) cost only matters on the
		// oversized 1K map (a separate, known perf concern), not normal maps.
		const std::chrono::steady_clock::time_point _cpuT0 =
			s_gpParityOn ? std::chrono::steady_clock::now()
			             : std::chrono::steady_clock::time_point{};
		eye->inverseProject(mouseXY, wPos);
		// GPU-GROUND-PICK-PARITY-1: oracle fires only on the quad-walk branch (the
		// exact O(quads) cost the GPU depth-unproject would replace); the cache-hit
		// branch above skips both paths, matching the recon's "per cursor-move
		// frame" comparison basis (R1). Parity-only; wPos is untouched.
		if (s_gpParityOn) {
			const unsigned long long _cpuNs = (unsigned long long)
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - _cpuT0).count();
			gpParitySite(mouseX, mouseY, _cpuNs, wPos.x, wPos.y, wPos.z, "hover");
			// GPU-GROUND-PICK-PARITY-1 center probe: unattended smoke parks the
			// cursor at (0,0) so the cursor path never samples real terrain
			// (compared=0). When MC2_GPU_GROUND_PICK_PARITY_CENTER=1, run a
			// screen-CENTER CPU-vs-GPU parity comparison over the terrain the
			// camera is actually looking at (incl. displaced maps). CPU pick at
			// center is a separate inverseProject; wPos above is NOT disturbed.
			if (s_gpCenter) {
				const int cxPix = Environment.screenWidth  / 2;
				const int cyPix = Environment.screenHeight / 2;
				Stuff::Vector2DOf<long> centerXY;
				centerXY.x = cxPix; centerXY.y = cyPix;
				Stuff::Vector3D centerW;
				eye->inverseProject(centerXY, centerW);
				gpCenterProbe(cxPix, cyPix, centerW.x, centerW.y, centerW.z);
			}
		}
		prevMouseX = mouseX;
		prevMouseY = mouseY;
		cachedWPos = wPos;
		cachedWorldToClip = curW2C;
		inverseProjectCacheValid = true;
	}

	// find out if this position is passable, has line of sight
	int cellR, cellC;
	bool passable = 1;
	bool lineOfSight = 0;
	{
		ZoneScopedN("MIF.LOS"); MifScope _mLOS(MF_LOS);
		if ( Terrain::IsGameSelectTerrainPosition( wPos ) )
		{
			land->worldToCell(wPos, cellR, cellC);
			if (Team::home)	   //May go NULL during multiplayer when a player first dies?
				lineOfSight = Team::home->teamLineOfSight(wPos,0.0f);
		}
	}


	// update buttons and stuff, even if not in region, it draws objectives and stuff
	{
		ZoneScopedN("MIF.ControlGui"); MifScope _mCG(MF_CTRLGUI);
		controlGui.update( isPaused() && !isPausedWithoutMenu(), lineOfSight );
	}

	bool leftClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isLeftClick());
	bool rightClicked = (!userInput->isLeftDrag() && !userInput->wasRightDrag() && userInput->rightMouseReleased());
	bool bLeftDouble = userInput->isLeftDoubleClick();


	{
		ZoneScopedN("MIF.UpdateTarget"); MifScope _mUT(MF_UPDTGT);
		// Pass the RAW-space GUI flag (not the HUD-inverse bGui): the world pick +
		// hovered-unit name must be suppressed only when the cursor is over a widget
		// in the space the pick (findObjectByMouse) and the user actually use.
		updateTarget(bRawGui);
	}

	{ ZoneScopedN("MIF.PostTarget");
	//------------------------------------
	// Attila (Strategic Commander) Next
	attilaXAxis 	= userInput->getAttilaXAxis();  
	attilaYAxis		= userInput->getAttilaYAxis();
	attilaRZAxis	= userInput->getAttilaRAxis();
	
	//-------------------------------
	// Update the Debug Status Bar...
	int row, col;
	land->worldToCell(wPos, row, col);
	sprintf(DebugStatusBarString, "TIME: %06d, MOUSE: [%d, %d] %d,%d,%d (%.2f, %.2f, %.2f), PATHMGR: %02d(%02d)",
		(long)scenarioTime,
		row, col,
		GlobalMoveMap[0] ? GlobalMoveMap[0]->calcArea(row, col) : -1,
		GlobalMoveMap[1] ? GlobalMoveMap[1]->calcArea(row, col) : -1,
		GlobalMoveMap[2] ? GlobalMoveMap[2]->calcArea(row, col) : -1,
		wPos.x, wPos.y, wPos.z,
		PathManager->numPaths, PathManager->peakPaths);
	if (MPlayer) {
		char mpStr[256];
		if (MPlayer->isServer())
			sprintf(mpStr, ", MULTIPLAY: %s-(%d)SERVER {%d,%d}", MPlayer->getPlayerName(), MPlayer->commanderID, MPlayer->maxReceiveLoad, MPlayer->maxReceiveSize);
		else
			sprintf(mpStr, ", MULTIPLAY: %s-(%d)CLIENT {%d,%d}", MPlayer->getPlayerName(), MPlayer->commanderID, MPlayer->maxReceiveLoad, MPlayer->maxReceiveSize);
		strcat(DebugStatusBarString, mpStr);
	}
	if (EnemiesGoalPlan)
		strcat(DebugStatusBarString, " [ENEMIES GOALPLAN]");
	if (ShowMovers)
		strcat(DebugStatusBarString, " [SHOW MOVERS]");
	if (CullPathAreas)
		strcat(DebugStatusBarString, " [CULL PATH AREAS]");
	if (ZeroHPrime)
		strcat(DebugStatusBarString, " [ZERO HPRIME]");
	if (CalcValidAreaTable)
		strcat(DebugStatusBarString, " [CALC VALID AREA]");
	if (GlobalMap::logEnabled)
		strcat(DebugStatusBarString, " [GLOBAL LOG]");
	if (!MechWarrior::brainsEnabled[1])
		strcat(DebugStatusBarString, " [BRAINDEAD: 1]");

	if ( bLeftDouble && target && target->isMover() && target->getTeam() == Team::home )
	{
		int forceGroup = -1;
		for ( int i = 0; i < 10; i++ )
		{
			if ( ((Mover*)target)->isInUnitGroup( i ) )
			{
				forceGroup = i;
				break;
			}
		}

		if ( forceGroup != -1 )
			selectForceGroup( forceGroup, true );

		lastUpdateDoubleClick = true;
		
	}

	Team* pTeam = Team::home;

	int moverCount = 0;
	int nonMoverCount = 0;
	for (long i=0;i<pTeam->getRosterSize();i++)
	{
		Mover* pMover = (Mover*)pTeam->getMover( i );
		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId()  )
		{
			if ( pMover->maxMoveSpeed )
			{
				passable &= pMover->canMoveHere( wPos );
				moverCount++;
			}
			else
				nonMoverCount++;
		}

	}
	
	{
		ZoneScopedN("MIF.InputStyle");
		if( useLeftRightMouseProfile ) // using AOE control style
		{
			if ( WAYPOINT_KEY == -1 )
				WAYPOINT_KEY = KEY_LCONTROL;

			commandClicked = rightClicked;
			selectClicked = !bLeftDouble && !lastUpdateDoubleClick && userInput->leftMouseReleased() && !userInput->getKeyDown( KEY_T) && !isDragging;
			cameraClicked = gos_GetKeyStatus( KEY_LMENU ) == KEY_HELD;
			if ( moveCameraAround( lineOfSight, passable, ctrlDn, bGui, moverCount, nonMoverCount ) )
			{
				bool leftClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isLeftClick());
				bool rightClicked = (!userInput->isLeftDrag() && !userInput->wasRightDrag() && userInput->rightMouseReleased());

				// deal with the hot keys
				update( leftClicked, rightClicked, mouseX, mouseY, target,  lineOfSight );

				return;
			}

			updateAOEStyle(shiftDn, altDn, ctrlDn, bGui, lineOfSight, passable, moverCount, nonMoverCount);


		}
		else // using mc1 style
		{
			commandClicked = leftClicked;
			selectClicked = leftClicked && !lastUpdateDoubleClick;
			cameraClicked = userInput->isRightDrag();

			if ( WAYPOINT_KEY == -1 )
				WAYPOINT_KEY = KEY_LCONTROL;
			if ( moveCameraAround( lineOfSight, passable, ctrlDn, bGui, moverCount, nonMoverCount ) )
			{
				bool leftClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isLeftClick());
				bool rightClicked = (!userInput->isLeftDrag() && !userInput->wasRightDrag() && userInput->rightMouseReleased());

				// deal with the hot keys
				update( leftClicked, rightClicked, mouseX, mouseY, target,  lineOfSight );
				return;
			}

			updateOldStyle(shiftDn, altDn, ctrlDn, bGui, lineOfSight, passable, moverCount, nonMoverCount);
		}
	}

	{
		ZoneScopedN("MIF.DrawBars"); MifScope _mDB(MF_DRAWBARS);
		for (int i = 0; i < Team::home->getRosterSize(); i++ )
		{
			Mover* pMover = (Mover*)Team::home->getMover( i );
			MechWarrior* pilot = pMover->getPilot();
			if ( pilot && pMover->getCommander()->getId() == Commander::home->getId())
			{
				GameObject* pTmpTarget = pilot->getCurrentTarget( );
				if ( pTmpTarget && (i < MAX_ICONS))	//Must check this because old test maps have more then 16 movers on them!!
				{
					pTmpTarget->setDrawBars(true);
					oldTargets[i] = pTmpTarget;
					s_oldTargetWatchID[i] = pTmpTarget->getWatchID(); // for next-frame liveness check
				}
			}
		}
	}

	} // MIF.PostTarget

	if ( !bLeftDouble && !( lastUpdateDoubleClick &&
		userInput->getMouseLeftButtonState() == MC2_MOUSE_DOWN ) )// check for the hold )
		lastUpdateDoubleClick = false;

	{
		ZoneScopedN("MIF.Rollovers"); MifScope _mRO(MF_ROLLOVERS);
		updateRollovers();
	}

	if (s_mfOn)
		mfFrameEnd((unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - _mifT0).count());
}

void MissionInterfaceManager::updateVTol()
{
	//We're probably going to use this alot!
	long commanderID = Commander::home->getId();
	paintingMyVtol = paintingVtol[commanderID];
	
	for (long vtolNum = 0;vtolNum < MAX_TEAMS; vtolNum++)
	{
		// update the vtol, if it needs it
		if ( paintingVtol[vtolNum] && (!bPaused || MPlayer))
		{
			if (vehicleID[vtolNum] != 147)		//We are the standard vtol.
			{
				if (!vehicleDropped[vtolNum] && (vTol[vtolNum]->currentFrame >= 145))
				{
					vehicleDropped[vtolNum] = true;
					soundSystem->playDigitalSample(VTOL_DROP,vPos[vtolNum]);
					
					//OK, if this is another persons vtol, I probably shouldn't do this
					// Glenn, what should it do?
					// Just check my commanderID against the vtolNum and if they match do it?
					// Otherwise do nothing?
					// Help Me, Spock!
					// -fs
					if (MPlayer) 
					{
						if (commanderID == vtolNum)
						{
							//--------------------------------------------------------
							// This will get the other machines to enable the mover...
							MPlayer->sendReinforcement(vehicleID[vtolNum], MPlayer->reinforcements[commanderID][0], "noname", commanderID, vPos[vtolNum], 2);
						}
					}
					else
						addVehicle( vPos[vtolNum] );
				}
				else if (vTol[vtolNum]->currentFrame >= 300)
				{
					paintingVtol[vtolNum] = 0;
					delete vTol[vtolNum]; 
					vTol[vtolNum] = NULL;
					
					if (dustCloud[vtolNum])
					{
						dustCloud[vtolNum]->Kill();
						delete dustCloud[vtolNum];
						dustCloud[vtolNum] = NULL;
					}
					
					if (recoveryBeam[vtolNum])
					{
						recoveryBeam[vtolNum]->Kill();
						delete recoveryBeam[vtolNum];
						recoveryBeam[vtolNum] = NULL;
					}

					//We should have set this when the helo was brought in.
					// Let it takeoff now that the vtol is out of the way.
					if (MPlayer) {
						if ((vtolNum < 0) || (vtolNum >= MAX_MC_PLAYERS))
							STOP(("updateVTOL: bad vtolNum"));
						if (MPlayer->reinforcements[vtolNum][0] < 0)
							STOP(("updateVTOL: bad reinforcement"));
						MoverPtr mover = MPlayer->moverRoster[MPlayer->reinforcements[vtolNum][0]];
						if (mover) {
							TacticalOrder tacOrder;
							tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_POWERUP, true);
							tacOrder.pack(NULL, NULL);					
							if (!MPlayer->isServer())
								MPlayer->sendPlayerOrder(&tacOrder, false, 1, &mover);
							else
								mover->handleTacticalOrder(tacOrder);
						}
						MPlayer->reinforcements[vtolNum][0] = -1;
						}
					else if (reinforcement)
					{
						reinforcement->getPilot()->orderPowerUp(true, ORDER_ORIGIN_SELF);
						reinforcement = NULL;
					}

					if ( !MPlayer || vtolNum == MPlayer->commanderID )
						controlGui.unPressAllVehicleButtons();
				}
			}
			else	//Update the KARNOV animation
			{
				if ((vTol[vtolNum]->getCurrentGestureId() == 1) && !vTol[vtolNum]->getInTransition())
				{
					vTol[vtolNum]->setGesture(0);

					//Needs to STAY false, probably in MPlayer too because
					// If the mech we are about to recover gets destroyed, we need to abort!!
					// Probably in Multiplayer now its possible to recover even if destroyed!
					if (!MPlayer)
						mechRecovered[vtolNum] = false;		//Need to know when mech is done so Karnov can fly away.

					vTolTime[vtolNum] = scenarioTime;
					
					//Start GOsFX - We are now hovering.
					if (recoveryBeam[vtolNum])
					{
						Stuff::LinearMatrix4D 	shapeOrigin;
						Stuff::LinearMatrix4D	localToWorld;
						Stuff::LinearMatrix4D	localResult;
									
						Stuff::Vector3D dustPos = vTol[vtolNum]->position;
						Stuff::Point3D wakePos;
						wakePos.x = -dustPos.x;
						wakePos.y = dustPos.z;
						wakePos.z = dustPos.y;
							
						shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
						shapeOrigin.BuildTranslation(wakePos);
									
						gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
					
						recoveryBeam[vtolNum]->SetLoopOff();
						recoveryBeam[vtolNum]->SetExecuteOn();
						recoveryBeam[vtolNum]->Start(&info);
					}

					//Actually the Karnov Deploy Sound Effect!
					soundSystem->playDigitalSample(RADAR_HUM,vTol[vtolNum]->position,false);
				}
				else if ((vTol[vtolNum]->getCurrentGestureId() == 0) && !mechRecovered[vtolNum] && mechToRecover[vtolNum] && !mechToRecover[vtolNum]->isDestroyed())
				{
					if (scenarioTime > vTolTime[vtolNum] + Mover::recoverTime)
					{
						bool doIt = true;
						if (MPlayer)
						{
							if (commanderID == vtolNum)
							{
								//--------------------------------------------------------
								// This will get the other machines to enable the mover...
								MPlayer->sendReinforcement(vehicleID[vtolNum], MPlayer->reinforcements[vtolNum][1], MPlayer->reinforcementPilot[vtolNum], vtolNum, vPos[vtolNum], 5);
							}
							else
								doIt = false;
						}
						
						if (doIt && !mechToRecover[vtolNum]->isDestroyed())
						{
							//Seems like we don't want to do this on every machine, do we?
							// -fs

							//STAY in recover until its done.  I know it shouldn't be possible but trust me, its happening.
							while (((MoverPtr)mechToRecover[vtolNum])->recover() == false)
								;

							if (mechToRecover[vtolNum]->isDisabled())
							{
								mechToRecover[vtolNum]->setStatus(OBJECT_STATUS_SHUTDOWN,true);
								mechToRecover[vtolNum]->getSensorSystem()->broken = false;
								((MoverPtr)mechToRecover[vtolNum])->timeLeft = 1.0f;
								((MoverPtr)mechToRecover[vtolNum])->exploding = false;
							}
							char* newPilotName = NULL;
							if (MPlayer)
								newPilotName = MPlayer->reinforcementPilot[vtolNum];
							else
								newPilotName = (char*)LogisticsData::instance->getBestPilot( mechToRecover[vtolNum]->tonnage );

							mission->tradeMover(mechToRecover[vtolNum], Commander::commanders[vtolNum]->getTeam()->getId(), vtolNum, newPilotName, "pbrain");
							mechRecovered[vtolNum] = true;
							mechToRecover[vtolNum]->getPilot()->orderPowerUp(true, ORDER_ORIGIN_SELF);
							if (MPlayer)
								MPlayer->reinforcements[vtolNum][1] = -1;
						}
					}
				}
				else if ((vTol[vtolNum]->getCurrentGestureId() == 0) && (mechRecovered[vtolNum] || mechToRecover[vtolNum]->isDestroyed()) && (!vTol[vtolNum]->getInTransition()))
				{
					vTol[vtolNum]->setGesture(2);	//Fly Away!!
				}
				else if ((vTol[vtolNum]->getCurrentGestureId() == 2) && (!vTol[vtolNum]->getInTransition()))
				{
					paintingVtol[vtolNum] = 0;
					delete vTol[vtolNum];
					vTol[vtolNum] = NULL;
					
					if (dustCloud[vtolNum])
					{
						dustCloud[vtolNum]->Kill();
						delete dustCloud[vtolNum];
						dustCloud[vtolNum] = NULL;
					}
					
					if (recoveryBeam[vtolNum])
					{
						recoveryBeam[vtolNum]->Kill();
						delete recoveryBeam[vtolNum];
						recoveryBeam[vtolNum] = NULL;
					}

					//Check if mech was recovered.  If not, restore the resource points!!!
					// This can happen if we call in a recovery vehicle and the mech is destroyed
					// BEFORE the recovery vehicle recovers it!!
					if (!mechRecovered[vtolNum]) {
						if (MPlayer) {
							if (MPlayer->isServer()) {
								//MPlayer->playerInfo[vtolNum].resourcePoints += 10000;
								Stuff::Vector3D pos;
								MPlayer->sendReinforcement(10000, 0, "noname", vtolNum, pos, 6);
							}
							}
						else
							LogisticsData::instance->setResourcePoints(LogisticsData::instance->getResourcePoints() + 10000);
					}

					if ( !MPlayer || vtolNum == MPlayer->commanderID )
						controlGui.unPressAllVehicleButtons();
				}
			}
	
			if (dustCloud[vtolNum] && dustCloud[vtolNum]->IsExecuted())
			{
				Stuff::LinearMatrix4D 	shapeOrigin;
				Stuff::LinearMatrix4D	localToWorld;
				Stuff::LinearMatrix4D	localResult;
						
				Stuff::Vector3D dustPos = vTol[vtolNum]->position;
				Stuff::Point3D wakePos;
				wakePos.x = -dustPos.x;
				wakePos.y = dustPos.z;
				wakePos.z = dustPos.y;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(wakePos);
						
				Stuff::OBB boundingBox;
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
				
				dustCloud[vtolNum]->Execute(&info);
			}
			
			if (recoveryBeam[vtolNum] && recoveryBeam[vtolNum]->IsExecuted())
			{
				Stuff::LinearMatrix4D 	shapeOrigin;
				Stuff::LinearMatrix4D	localToWorld;
				Stuff::LinearMatrix4D	localResult;
						
				Stuff::Vector3D dustPos = vTol[vtolNum]->position;
				Stuff::Point3D wakePos;
				wakePos.x = -dustPos.x;
				wakePos.y = dustPos.z;
				wakePos.z = dustPos.y;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(wakePos);
						
				Stuff::OBB boundingBox;
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
				
				recoveryBeam[vtolNum]->Execute(&info);
			}
				
			if (vTol[vtolNum])	//Might have been deleted above!!
			{
				vTol[vtolNum]->recalcBounds();
				vTol[vtolNum]->update();			//Must update even if not on screen!
			}
		}
		else if (paintingVtol[vtolNum] && bPaused)
		{
			if (vTol[vtolNum])	//Might have been deleted above!!
			{
				long cFrame = vTol[vtolNum]->currentFrame;
				
				vTol[vtolNum]->recalcBounds();
				vTol[vtolNum]->update();			//Must update even if not on screen!
				
				//If we are paused, do not animate.
				vTol[vtolNum]->currentFrame = cFrame;
			}
		}
	}
}

	
// bGui here is the RAW-space "cursor over a drawn HUD widget" flag (bRawGui at the
// caller), NOT the HUD-inverse order-suppression bGui. It only gates whether a
// genuinely-picked world object (and its name help-text) is kept. See bRawGui note
// in update().
void MissionInterfaceManager::updateTarget( bool bGui)
{
	
	// MISSION-START-HOVER-TARGET-LIFETIME-1 (completed): suppress hover BEFORE any deref.
	// During mission start / settle frames / teardown the persistent target + oldTargets[]
	// can point at freed objects, and findObjectByMouse can return a stale/garbage pointer.
	// Clear hover state WITHOUT dereferencing (no setDrawBars/setTargeted -> no virtual call
	// on freed memory) and bail. s_hoverSettleFrames keeps picking off for a few live frames
	// after arming, so the first beginMission update can't pick a not-yet-ready world.
	if ( s_hoverSuppressed || s_hoverSettleFrames > 0 )
	{
		if ( s_hoverSettleFrames > 0 )
			s_hoverSettleFrames--;
		memset( oldTargets, 0, sizeof(GameObject*) * MAX_ICONS );
		memset( s_oldTargetWatchID, 0, sizeof(s_oldTargetWatchID) );
		target = NULL;
		s_targetWatchID = 0;
		return;
	}

	// Validate the PERSISTENT target + oldTargets are still live before dereferencing them:
	// an object targeted last frame may have died this frame. getByWatchID(wid) is a registry
	// lookup (NO deref of the possibly-freed pointer); if it no longer maps to our pointer the
	// object is gone -> drop it silently (its draw-bars died with it).
	if ( target && ObjectManager->getByWatchID( s_targetWatchID ) != target )
	{
		target = NULL;
		s_targetWatchID = 0;
	}
	for ( int i = 0; i < MAX_ICONS; i++ )
	{
		if ( oldTargets[i] && ObjectManager->getByWatchID( s_oldTargetWatchID[i] ) != oldTargets[i] )
		{
			oldTargets[i] = 0;
			s_oldTargetWatchID[i] = 0;
		}
	}

	// unset anything that isn't targeted (now validated-live, safe to deref)
	for ( int i = 0; i < MAX_ICONS; i++ )
	{
		if ( oldTargets[i] )
		{
			oldTargets[i]->setDrawBars( 0 );
			oldTargets[i] = 0;
			s_oldTargetWatchID[i] = 0;
		}
	}

	// if there was a target, unset it
	if ( target )
		target->setTargeted( 0 );

	//----------------------------------------------------------------------------------------
	// Get Any object we are over.  Change cursor if appropriate.  Set selected if necessary.
	// Clear the target pointer if we have done all we need to do here.  Otherwise, leave it
	// set and issue an order below based on who is in the myForce pointers.
	//
	// GPU_PICK_HOVER_DYNAMIC-1: fast mech hover via ObjectId MRT.
	// Gate: MC2_GPU_PICK_HOVER=1 + MC2_OBJECT_ID_BUFFER=1.
	// Only Mech kind uses the GPU result; static props fall through to CPU.
	// Mouse-position cache: no glReadPixels when cursor hasn't moved.
	{
		static const bool s_hoverGate = []() {
			return RenderWorld::IsGpuPickHoverEnabled() &&
			       RenderWorld::IsObjectIdBufferEnabled();
		}();
		static const bool s_hoverTrace = []() {
			return RenderWorld::IsGpuPickHoverTraceEnabled();
		}();
		// MISSION-START-HOVER-TARGET-LIFETIME-1: these caches are now FILE-SCOPE statics
		// (s_hover*) so invalidateHoverTarget() can clear them at mission boundaries.
		// Per-frame counters for trace output (reset on first call each frame via atexit).
		static int s_attempts = 0, s_mechHits = 0, s_nonMech = 0, s_cpuFallback = 0;
		static bool s_atexitRegistered = false;
		if (s_hoverGate) {
			if (!s_atexitRegistered) {
				s_atexitRegistered = true;
				std::atexit([]() {
					if (s_attempts && RenderWorld::IsGpuPickHoverTraceEnabled()) {
						std::fprintf(stderr,
							"[GPU_PICK_HOVER v1] session: attempts=%d mech_hits=%d "
							"non_mech_fallback=%d cpu_fallback=%d\n",
							s_attempts, s_mechHits, s_nonMech, s_cpuFallback);
					}
				});
			}
			// Cache hit: mouse hasn't moved, reuse last result — but only if the cached
			// object is still live. Validate by watch-ID WITHOUT dereferencing the raw
			// cached pointer (getByWatchID takes the stored WID; '==' is a pointer
			// compare). A mech that died since the cache was set leaves a stale/poisoned
			// pointer; reusing it raw let target reach a virtual call (target->isDisabled)
			// with target = 0xFFFF.. -> READ-violation crash. If it no longer resolves to
			// the same live registered object, drop the cache and fall through to a fresh pick.
			if (s_hoverCacheValid && mouseX == s_hoverLastMouseX && mouseY == s_hoverLastMouseY) {
				if (s_hoverCachedTarget &&
				    ObjectManager->getByWatchID(s_hoverCachedTargetWID) == s_hoverCachedTarget) {
					target = s_hoverCachedTarget;
					goto gpu_hover_done;
				}
				s_hoverCacheValid = false;   // cached object died — re-pick fresh below
			}
			// Viewport-to-GL coord transform via proven ScreenPick utility.
			float vMulX, vMulY, vAddX, vAddY;
			gos_GetViewport(&vMulX, &vMulY, &vAddX, &vAddY);
			if (vMulX > 0.0f && vMulY > 0.0f &&
			    mouseX >= 0 && mouseY >= 0 &&
			    mouseX < (int)vMulX && mouseY < (int)vMulY) {
				RenderWorld::ScreenPickContext ctx;
				ctx.mouseX = mouseX; ctx.mouseY = mouseY;
				ctx.vMulX  = vMulX;  ctx.vMulY  = vMulY;
				ctx.vAddX  = vAddX;  ctx.vAddY  = vAddY;
				ctx.drawableWidth  = Environment.drawableWidth;
				ctx.drawableHeight = Environment.drawableHeight;
				RenderWorld::screenPickCompute(&ctx);
				++s_attempts;
				RenderWorld::LookupResult res = RenderWorld::lookupAtPixel(ctx.glX, ctx.glY);
				if (res.isValid && res.kind == RenderWorld::RenderObjectKind::Mech) {
					BattleMech* bm = GameAdapters::Mech::findMechByHandle(res.handle);
					if (bm != nullptr) {
						++s_mechHits;
						if (s_hoverTrace) {
							std::fprintf(stderr,
								"[GPU_PICK_HOVER v1] hit kind=Mech handle=%u idx=%u "
								"mech=%p screen=(%d,%d) gl=(%d,%d)\n",
								res.handle.bits,
								(unsigned)res.handle.index(),
								(void*)bm, mouseX, mouseY, ctx.glX, ctx.glY);
						}
						s_hoverLastMouseX   = mouseX;
						s_hoverLastMouseY   = mouseY;
						s_hoverCachedTarget = bm;
						s_hoverCachedTargetWID = bm->getWatchID();   // bm freshly picked = live; cache its WID for next-frame validation
						s_hoverCacheValid   = true;
						target = bm;
						goto gpu_hover_done;
					}
				} else if (res.isValid) {
					++s_nonMech;
					if (s_hoverTrace) {
						std::fprintf(stderr,
							"[GPU_PICK_HOVER v1] non_mech kind=%d screen=(%d,%d) "
							"falling through to CPU\n",
							(int)res.kind, mouseX, mouseY);
					}
				}
			}
			// GPU miss or non-Mech: invalidate cache, fall through to CPU.
			s_hoverCacheValid = false;
			++s_cpuFallback;
		}
	}
	target = ObjectManager->findObjectByMouse(mouseX, mouseY);
	gpu_hover_done:;
	if ( target )
	{
		// Remember the freshly-picked (live) target's watch-ID so next frame's top-of-function
		// validation can detect if its object dies. Picking only runs post-settle, so target
		// here is a live object -> getWatchID() is safe. target may be nulled by the checks
		// below (bGui/sensor/destroyed); that's fine -- the WID is only consulted when target
		// is non-null next frame, and target is never reassigned to a different object here.
		s_targetWatchID = target->getWatchID();
		// Drop a stale/freed pick before ANY virtual dispatch below. getWatchID()
		// is a non-virtual member read (safe even on freed memory); round-trip it
		// through the ObjectManager to confirm the object is still live+registered.
		// Without this, a mech that died the same frame (common under cheat-mode
		// mass-kill) leaves a dangling target whose first virtual call
		// (isDisabled / getDescription) jumps through a freed vtable -> EXEC at 0x0.
		if ( ObjectManager->getByWatchID( s_targetWatchID ) != target )
			target = 0;
		if ( target && bGui )
			target = 0;
		else if ( target && target->isMover() && !ShowMovers && !(MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]))
		{
			if ((target->getTeamId() != Team::home->getId()) && 
				!target->isDisabled() && 
				(((Mover *)target)->conStat < CONTACT_SENSOR_QUALITY_1))
				target = NULL;
		}
		
		// STABILITY: a disabled mech may be mid-teardown (null/partial appearance,
		// stale vtable) — the virtual derefs below (getDescription / getAppearance)
		// then jump through a freed slot and crash (EXEC at 0x0). This surfaces under
		// cheat-mode soak mass-disable but is a latent hazard whenever a hovered mech
		// dies the same frame. Don't compute hover info for a disabled target.
		if ( target && target->isDisabled() )
			target = 0;

		if ( target )
		{
			// MISSION-START-HOVER-TARGET-LIFETIME-1: secondary defensive guard (insurance,
			// not the fix). The lifecycle invalidation + suppression above is what prevents a
			// stale/freed target reaching this virtual call at mission start.
			if ( !target )
				return;
			int descID = target->getDescription();
			if ( descID != -1 )
			{
				int nameID = target->getAppearance()->getObjectNameId();

				helpTextHeaderID = nameID;
				helpTextID = descID;
			}
			if ( target->isDestroyed() )
			{
				target = 0;
			}
			
			else if ( target->isDisabled() && !target->isMover() )
				target = 0;
			else if ( !target->isSelectable())
			{
				if ( !target->isDisabled() && !target->isMover() )
					target = NULL;
			}
		}
	}

	// CURSOR_TARGET symptom trace (INPUT-CURSOR-OFFSCREEN-TARGET-RECON-1): emit when a
	// resolved hover/command target is anomalous — projects behind the camera (w<=1e-4)
	// or lands far from the cursor. This is the offscreen-pick symptom; pairs with the
	// pick_rect mechanism emit in objmgr.cpp. Gate MC2_CURSOR_TARGET_TRACE +
	// MC2_DIAG_TAGS=CURSOR_TARGET. Read via get_diagnostic_events("CURSOR_TARGET").
	// Recon only — no behavior change.
	{
		static const bool s_cursorTargetTrace = (std::getenv("MC2_CURSOR_TARGET_TRACE") != nullptr);
		if ( s_cursorTargetTrace && target && eye && mc2_diag::tagEnabled("CURSOR_TARGET") )
		{
			Stuff::Vector3D _ct_pos = target->getPosition();
			Stuff::Vector4D _ct_sp;
			eye->projectForScreenXY( _ct_pos, _ct_sp );
			bool  _ct_behind = (_ct_sp.w <= 1e-4f);
			float _ct_dx = _ct_sp.x - (float)mouseX;
			float _ct_dy = _ct_sp.y - (float)mouseY;
			float _ct_distSq = _ct_dx*_ct_dx + _ct_dy*_ct_dy;
			if ( _ct_behind || _ct_distSq > 90000.0f )   // behind camera, or >300px from cursor
			{
				char _ct_buf[320];
				snprintf(_ct_buf, sizeof(_ct_buf),
				         "{\"site\":\"update_target\",\"mouse\":[%ld,%ld],\"target_screen\":[%.1f,%.1f],\"w\":%.4f,\"behind\":%d,\"dist_sq\":%.0f,\"is_mover\":%d,\"watch_id\":%d}",
				         (long)mouseX, (long)mouseY, _ct_sp.x, _ct_sp.y, _ct_sp.w,
				         _ct_behind?1:0, _ct_distSq,
				         target->isMover()?1:0, (int)s_targetWatchID);
				mc2_diag::writeEvent("CURSOR_TARGET", 1, 0, _ct_buf);
			}
		}
	}
}

void MissionInterfaceManager::drawWayPointPaths()
{
	Team* pTeam = Team::home;
   	for (long i=0;i<pTeam->getRosterSize();i++)
   	{
   		Mover* pMover = (Mover*)pTeam->getMover( i );
   		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
   		{
   			pMover->updateDrawWaypointPath();
   		}
   	}
}
	
void MissionInterfaceManager::updateOldStyle( bool shiftDn, bool altDn, bool ctrlDn, 
											 bool bGui, bool lineOfSight, bool passable, 
											 long moverCount, long nonMoverCount )
{

	printDebugInfo();

	//We're probably going to use this alot!
	long commanderID = Commander::home->getId();
	
 	// Update the waypoint markers so that they are visible!
   	if ( userInput->getKeyDown( WAYPOINT_KEY ) || controlGui.getMines() )
   	{
		drawWayPointPaths(); 	
		//Can Never make a patrol path.  Causes movement wubbies!
//		if ( makePatrolPath() )
//			return;
   	}

	int mState = userInput->getMouseCursor();

	bool leftClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isLeftClick() && !lastUpdateDoubleClick);
	bool rightClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isRightClick());

	// M1.6 Q6 4-site observable: set to true immediately after each of
	// the setSelected(true) writer sites this style body owns (Shift+
	// additive site and plain-LMB-select site). Read at tail by
	// tryStaticPropPick to short-circuit when the legacy path already
	// claimed this click. Local scope; no leakage.
	bool moverSelectedThisFrame = false;

	// deal with the hot keys
	if ( update( leftClicked, rightClicked, mouseX, mouseY, target,  lineOfSight ) )
		return;

	// special case for bottom of screen
	if ( bGui ) // don't target or anything if we are int the control panel
	{
		// now check the selection versus the commands
		if ( !canJump() && controlGui.getJump() )
			controlGui.toggleJump();

		userInput->setMouseCursor( mState );
		if ( controlGui.mouseInVehicleStopButton )
			return;
		if ( !userInput->isLeftDrag() && !controlGui.isAddingAirstrike() && !controlGui.isAddingVehicle() )
			return;
	}
	
	
	Team* pTeam = Team::home;
	
	if (target) // if there is a target, make the appropritate cursor
	{
		userInput->setMouseCursor( makeTargetCursor( lineOfSight, moverCount, nonMoverCount ) );
	}
	else // make a move cursor
	{
		userInput->setMouseCursor( makeNoTargetCursor( passable, lineOfSight, ctrlDn, bGui, moverCount, nonMoverCount ) );
	}

	// Suppress the move-on-release when this click landed on an overview squad
	// card (it already selected the squad; don't also move under the card).
	if ( userInput->leftMouseReleased() && g_tacticalOverview.consumeReleaseSuppression() )
	{
		// consumed: skip the world move for this release
	}
	else if ( userInput->leftMouseReleased() && !userInput->wasLeftDrag() && !bGui && !lastUpdateDoubleClick) // move on the mouse ups
	{
		// PRECISION (companion to the per-frame O(1) approx at update():~778):
		// the per-frame wPos uses screenToGroundPlaneApprox (fast preview/status
		// bar, but a z=0 ground-plane point that drifts ~elevation in XY on hilly
		// terrain). For the DISCRETE move/place/recover action below we need the
		// real terrain-surface hit, so recompute wPos once here via inverseProject
		// (its actual job — terrain-surface/placement-Z). Runs only on click, so
		// its O(quads) cost is fine; move-orders land on the correct elevated cell.
		{
			Stuff::Vector2DOf<long> clickXY;
			clickXY.x = mouseX;
			clickXY.y = mouseY;
			const std::chrono::steady_clock::time_point _cpuT0 =
				s_gpParityOn ? std::chrono::steady_clock::now()
				             : std::chrono::steady_clock::time_point{};
			eye->inverseProject(clickXY, wPos);
			// GPU-GROUND-PICK-PARITY-1: precise per-click move-order pick (R2 case).
			// Depth-unproject reads the true rendered terrain surface -> the recon
			// expects GPU >= CPU accuracy on slopes; sample distant clicks for the
			// reverse-Z precision check. Parity-only; wPos is untouched.
			if (s_gpParityOn) {
				const unsigned long long _cpuNs = (unsigned long long)
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - _cpuT0).count();
				gpParitySite(mouseX, mouseY, _cpuNs, wPos.x, wPos.y, wPos.z, "Lmove");
			}
		}

		if ( (controlGui.isAddingVehicle() && !paintingVtol[commanderID] && canAddVehicle( wPos )) ||
			(controlGui.isAddingSalvage() && !paintingVtol[commanderID] && canRecover( wPos )))
		{
			// Target has already been confirmed  as a mover(BattleMech) by canRecover
			// OR it doesn't matter because the canAddVehicle part of the beginVtol function doesn't reference it!
			// Again, Its OK because we only use it for salvage craft.
			// Need to pass this in so that Multiplayer can pass it in.
			beginVtol(-1,commanderID,NULL,(MoverPtr)target);	//In Single player, this should always be zero?
			return;
		}
		else if ( (controlGui.isAddingVehicle() && !paintingVtol[commanderID] && !canAddVehicle( wPos )) || 
				(controlGui.isAddingSalvage() && !paintingVtol[commanderID] && !canRecover( wPos )))
		{
			soundSystem->playDigitalSample( INVALID_GUI );
			return;
		}
		else if ( controlGui.isAddingAirstrike() && !paintingVtol[commanderID] ) // if we're painting the vtol, carry on
		{
			addAirstrike();
			return;
		}
		
		else if (!target)
		{
			if ( controlGui.isSelectingInfoObject() )
			{
				soundSystem->playDigitalSample( INVALID_GUI );
				controlGui.cancelInfo();
			}
			else if (Terrain::IsGameSelectTerrainPosition(wPos))
			{
				
				if (controlGui.getGuardTower())
					doGuardTower();
				else if ( passable || selectionIsHelicopters())
				{
					if ( controlGui.getGuard() )
					{
						doGuard(target);
					}
					else if (!bForcedShot)
						doMove( wPos );
				}
				else
				{
					userInput->setMouseCursor(mState_DONT);
				}
			}
			else
			{
				userInput->setMouseCursor(mState_DONT);
			}
		}
		else 
			//We clicked on a target.  If mouse cursor is normal, we want to select a friendly.
				//if the mouse cursor is some form of attack, attack the target.
		{
			if ( target->isMover() && ( target->getTeamId() == Team::home->getId() || 
					CONTACT_VISUAL == ((Mover*)target)->getContactStatus(Team::home->getId(), true) 
					|| target->isDisabled() ) )
				{
					controlGui.setInfoWndMover( (Mover*)target );
				}
		
			if ( controlGui.getSalvage() )
				doSalvage();
			else if ( controlGui.getGuard() )
			{
				doGuard(target);
			}
			else if ( target->getTeamId() == Team::home->getId() && !target->isDisabled() )
			{
				
				//--------------------------------------------------------------------
				// User wants to select the guy we are on.  If he did
				// it with a shift, add him.  If not, whack all and just add target!
				if (shiftDn && target->getCommanderId() == Commander::home->getId() )
				{
					bool alreadyThere = false;
					//-------------------------------------------
					// First, check if he's already there!
					for (long i=0;i<pTeam->getRosterSize();i++)
					{
						Mover* pMover = (Mover*)pTeam->getMover( i );
						if (pMover == target && pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId())
							alreadyThere = true;
					}
					
					if (!alreadyThere && !target->isDisabled()) {
						target->setSelected( true );
						// M1.6 Q6 site 1: legacy Shift+additive-select on
						// friendly mover. Sets fallback gate; tail
						// tryStaticPropPick short-circuits so Section 11
						// invariant holds (no M1.6 log line in this case).
						moverSelectedThisFrame = true;
					} else {
						// M1.6 Q6 RESOLVED: setSelected(false) toggle-off
						// site MUST NOT set the gate per spec rationale.
						target->setSelected( false );
					}
				}
				else if ( controlGui.getRepair() && canRepair( target ) )
				{
					doRepair(target);
				}
			 	else if ( controlGui.getSalvage() && canSalvage( target ))
					doSalvage();
				else if ( controlGui.getGuardTower() )
					doGuardTower();
				else if ( target->getTeam() && Team::home->isFriendly(target->getTeam()) && target->getFlag(OBJECT_FLAG_CANREFIT) && target->getFlag(OBJECT_FLAG_MECHBAY) && canRepairBay(target) )
					doRepairBay(target);
				else if ( controlGui.isSelectingInfoObject() && target->isMover() )
					controlGui.setInfoWndMover( (Mover*)target );
				else if ( target->isMover() && !target->isDisabled() && target->getCommanderId() == Commander::home->getId() )
				{
					for (long i=0;i<pTeam->getRosterSize();i++)
					{
						Mover* pMover = (Mover*)pTeam->getMover( i );
						if (pMover->getCommander()->getId() == Commander::home->getId())
						{
							// M1.6 Q6 RESOLVED: clear-others setSelected(false)
							// loop MUST NOT set the gate.
							pMover->setSelected( false );
						}
					}

					target->setSelected( true );
					// M1.6 Q6 site 2: legacy plain-LMB-select on friendly
					// mover. Sets fallback gate.
					moverSelectedThisFrame = true;
				}
				else
					soundSystem->playDigitalSample( INVALID_GUI );


			}
			else
			{
				if ( controlGui.getGuard() ) // trying to guard invalid thing, cancel guard
					controlGui.toggleGuard();
				else if ( userInput->getMouseCursor() == mState_INFO && 
						target->isMover()  )
							controlGui.setInfoWndMover( (Mover*)target );
				else if ( !target->isDisabled() && !bForcedShot && !bAimedShot)
					doAttack();
			
			}
		}
	}

	// Task 7: Ctrl+Shift+LMB inspector pick. Runs before the legacy
	// gameplay picks so the inspector can claim the click first.
	// Priority: ImGui window capture > inspector > existing gameplay picks.
	//
	// Gate uses IsWindowHovered(AnyWindow) rather than WantCaptureMouse.
	// WantCaptureMouse is tainted by mouse_any_down (true whenever any button
	// is held, even over the viewport) and would suppress the pick during the
	// entire duration of every click.  IsWindowHovered is purely spatial --
	// true only when the cursor is physically over an ImGui window -- so it
	// correctly blocks the pick only when the user is interacting with ImGui.
	bool pickedByInspector = false;
#ifdef MC2_IMGUI
	if (ctrlDn && shiftDn && leftClicked && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
		GameplayPickRequest inspReq;
		inspReq.mouseX               = mouseX;
		inspReq.mouseY               = mouseY;
		inspReq.shiftDn              = true;    // shift IS held (ctrl+shift+LMB); required by tryGameplayPick gate
		inspReq.leftClicked          = true;
		inspReq.bGui                 = bGui;
		inspReq.bLeftDouble          = false;
		inspReq.moverSelectedThisFrame = false; // inspector always attempts the lookup
		GameplayPickResult inspResult = tryGameplayPick(inspReq);
		EditorInspector::setPickResult(mouseX, mouseY, inspResult.lookup);
		if (inspResult.lookup.isValid) {
			if (inspResult.lookup.kind == RenderWorld::RenderObjectKind::StaticProp) {
				EditorInspector::StaticPropInspectorData sd;
				sd.populated   = true;
				sd.recipeIndex = static_cast<int32_t>(inspResult.lookup.gameObjectId);
				GameAdapters::StaticProp::getRecipeShapeName(
				    sd.recipeIndex, sd.shapeName, sizeof(sd.shapeName));
				sd.materialGpuActive = GameAdapters::StaticProp::isMaterialGpuActive();
				{
					RenderCore::MaterialHandle mh;
					mh.bits = inspResult.lookup.materialHandleBits;
					sd.materialIdx = mh.index();
					sd.materialGen = mh.generation();
				}
				sd.materialGpuPopulated = GameAdapters::StaticProp::getMaterialGpuData(
				    inspResult.lookup.materialHandleBits, &sd.materialGpu);
				EditorInspector::setStaticPropData(sd);
			} else if (inspResult.lookup.kind == RenderWorld::RenderObjectKind::Mech) {
				BattleMech* bm = GameAdapters::Mech::findMechByHandle(inspResult.lookup.handle);
				if (bm) {
					EditorInspector::MechInspectorData md;
					md.populated   = true;
					strncpy_s(md.variantName, sizeof(md.variantName), bm->variantName, _TRUNCATE);
					strncpy_s(md.longName,    sizeof(md.longName),    bm->longName,    _TRUNCATE);
					md.chassisClass = static_cast<int>(bm->chassisClass);
					md.teamId       = bm->getTeamId();
					md.disabled     = bm->isDisabled();
					md.destroyed    = bm->isDestroyed();
					md.crippled     = bm->isCrippled();
					md.conStat      = bm->conStat;
					for (int i = 0; i < MAX_MOVER_ARMOR_LOCATIONS; ++i) {
						md.totalCurArmor += bm->armor[i].curArmor;
						md.totalMaxArmor += static_cast<float>(bm->armor[i].maxArmor);
					}
					for (int i = 0; i < MAX_MOVER_BODY_LOCATIONS; ++i) {
						md.totalCurStr += bm->body[i].curInternalStructure;
						md.totalMaxStr += static_cast<float>(bm->body[i].maxInternalStructure);
					}
					MechWarriorPtr pilot = bm->getPilot();
					if (pilot)
						strncpy_s(md.pilotName, sizeof(md.pilotName), pilot->getName(), _TRUNCATE);
					EditorInspector::setMechData(md);
				}
#ifdef MC2_IS_EDITOR
				else if (Unit* u = EditorObjectMgr::instance()->findUnitByMechHandle(inspResult.lookup.handle)) {
					EditorInspector::MechInspectorData md;
					md.populated = true;
					strncpy_s(md.variantName, sizeof(md.variantName), u->getDisplayName(), _TRUNCATE);
					md.teamId    = u->getAlignment();
					Pilot* ep    = u->getPilot();
					if (ep)
						strncpy_s(md.pilotName, sizeof(md.pilotName), ep->getName(), _TRUNCATE);
					EditorInspector::setMechData(md);
				}
#endif
			}
		}
		// IMG-INSPECT-2: terrain CPU fallback when object-ID misses.
		// Terrain::IsGameSelectTerrainPosition(wPos) is the canonical
		// "world point is on selectable map terrain" guard (also used
		// at line ~789 for LOS/cell lookup). If OID buffer is disabled
		// (MC2_OBJECT_ID_BUFFER=0), inspResult.lookup.isValid is always
		// false here so terrain wins on any in-map click - expected.
		bool inspectorConsumed = inspResult.lookup.isValid;
		if (!inspResult.lookup.isValid && land &&
				Terrain::IsGameSelectTerrainPosition(wPos)) {
			EditorInspector::TerrainInspectorData td;
			td.populated = true;
			td.worldX    = static_cast<float>(wPos.x);
			td.worldY    = static_cast<float>(wPos.y);
			td.worldZ    = static_cast<float>(wPos.z);
			land->worldToTile(wPos, td.tileRow, td.tileCol);
			land->worldToCell(wPos, td.cellRow, td.cellCol);
			EditorInspector::setTerrainData(td);
			inspectorConsumed = true;
		}
		if (inspectorConsumed)
			pickedByInspector = true;
	}
#endif

	// M1.6: env-gated static-prop pick attempt. Runs AFTER the legacy
	// mover-selection path above. Short-circuits internally when
	// moverSelectedThisFrame == true (Section 11 + Q6/Q8 invariant) or
	// when MC2_STATIC_PROP_PICK / MC2_OBJECT_ID_BUFFER are off.
	if (!pickedByInspector) {
		const bool bLeftDouble = userInput->isLeftDoubleClick();
		tryStaticPropPick(moverSelectedThisFrame,
		                  shiftDn,
		                  leftClicked,
		                  bGui,
		                  bLeftDouble,
		                  mouseX,
		                  mouseY);
		// M2.6: mech-pick consumer fires second. Both callers invoke the
		// shared tryGameplayPick spine; each kind-guards on r.lookup.kind.
		// At most one hit branch fires per click (handle ranges disjoint:
		// static-prop indices < kMechHandleBase, mech indices >= 65536).
		tryMechPick(moverSelectedThisFrame,
		            shiftDn,
		            leftClicked,
		            bGui,
		            bLeftDouble,
		            mouseX,
		            mouseY);
	}
}
void MissionInterfaceManager::updateAOEStyle(bool shiftDn, bool altDn, bool ctrlDn,
											  bool bGui, bool lineOfSight, bool passable, 
											  long moverCount, long nonMoverCount )
{
	printDebugInfo();

	//We're probably going to use this alot!
	long commanderID = Commander::home->getId();

 	// Update the waypoint markers so that they are visible!
   	if ( userInput->getKeyDown( WAYPOINT_KEY ) || controlGui.getMines() )
   	{
		drawWayPointPaths(); 	
		//Can Never make a patrol path.  Causes movement wubbies!
//		if ( makePatrolPath() )
//			return;
   	}

	int mState = userInput->getMouseCursor();

	bool leftClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isLeftClick());
	bool rightClicked = (!userInput->isLeftDrag() && !userInput->wasRightDrag() && userInput->rightMouseReleased());

	// M1.6 Q6 4-site observable: set to true immediately after each of
	// the setSelected(true) writer sites this style body owns (Shift+
	// additive site and plain-LMB-select site). Read at tail by
	// tryStaticPropPick to short-circuit when the legacy path already
	// claimed this click. Local scope; no leakage.
	bool moverSelectedThisFrame = false;

	// deal with the hot keys
	if ( update( leftClicked, rightClicked, mouseX, mouseY, target,  lineOfSight ) )
		return;

	// special case for bottom of screen
	if ( bGui ) // don't target or anything if we are int the control panel
	{
		// now check the selection versus the commands
		if ( !canJump() && controlGui.getJump() )
			controlGui.toggleJump();

		userInput->setMouseCursor( mState );
		if ( controlGui.mouseInVehicleStopButton )
			return;
		if ( !userInput->isLeftDrag() && !controlGui.isAddingAirstrike() && !controlGui.isAddingVehicle() && !controlGui.isAddingSalvage() )
			return;
	}
	
	Team* pTeam = Team::home;
	
	if (target) // if there is a target, make the appropritate cursor
	{
		userInput->setMouseCursor( makeTargetCursor( lineOfSight, moverCount, nonMoverCount ) );
	}
	else // make a move cursor
	{
		userInput->setMouseCursor( makeNoTargetCursor( passable, lineOfSight, ctrlDn, bGui, moverCount, nonMoverCount ) );
	}

	if ( userInput->rightMouseReleased() && !userInput->wasRightDrag() && !bGui) // move on the mouse ups
	{
		// Discrete-click precision fix, mirror of updateOldStyle (438e0117): the
		// per-frame wPos is screenToGroundPlaneApprox (z=0 ground-plane point that
		// drifts ~elevation in XY on hilly terrain / oblique camera pitch -> move
		// orders landed on the "other side of the map"). For this discrete right-
		// click move/place/recover action recompute wPos once via inverseProject
		// (the real terrain-surface hit). Click-only, so its O(quads) cost is fine.
		{
			Stuff::Vector2DOf<long> clickXY;
			clickXY.x = mouseX;
			clickXY.y = mouseY;
			const std::chrono::steady_clock::time_point _cpuT0 =
				s_gpParityOn ? std::chrono::steady_clock::now()
				             : std::chrono::steady_clock::time_point{};
			eye->inverseProject(clickXY, wPos);
			// GPU-GROUND-PICK-PARITY-1: precise per-click move-order pick (R2 case),
			// right-click variant. Parity-only; wPos is untouched.
			if (s_gpParityOn) {
				const unsigned long long _cpuNs = (unsigned long long)
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - _cpuT0).count();
				gpParitySite(mouseX, mouseY, _cpuNs, wPos.x, wPos.y, wPos.z, "Rmove");
			}
		}

		if ( (controlGui.isAddingVehicle() && !paintingVtol[commanderID] && canAddVehicle( wPos )) ||
			(controlGui.isAddingSalvage() && !paintingVtol[commanderID] && canRecover( wPos )))
		{
			// Target has already been confirmed  as a mover(BattleMech) by canRecover
			// OR it doesn't matter because the canAddVehicle part of the beginVtol function doesn't reference it!
			// Again, Its OK because we only use it for salvage craft.
			// Need to pass this in so that Multiplayer can pass it in.
			beginVtol(-1,commanderID,NULL,(MoverPtr)target);	//In Single player, this should always be zero?
			return;
		}
		else if ( (controlGui.isAddingVehicle() && !paintingVtol[commanderID] && !canAddVehicle( wPos )) || 
				(controlGui.isAddingSalvage() && !paintingVtol[commanderID] && !canRecover( wPos )))
		{
			soundSystem->playDigitalSample( INVALID_GUI );
			return;
		}
		else if ( controlGui.isAddingAirstrike() && !paintingVtol[commanderID] ) // if we're painting the vtol, carry on
		{
			addAirstrike();
			return;
		}
		
		else if (!target )
		{
			if ( controlGui.isSelectingInfoObject() )
			{
				soundSystem->playDigitalSample( INVALID_GUI );
				controlGui.cancelInfo();
			}
			else if (Terrain::IsGameSelectTerrainPosition(wPos))
			{
				if ( passable || selectionIsHelicopters() )
				{
					if (controlGui.getGuardTower())
						doGuardTower();
					else if ( controlGui.getGuard() )
					{
						doGuard(NULL);
					}
					else				
						doMove( wPos );
				}
				else
				{
					userInput->setMouseCursor(mState_DONT);
				}
			}
			else
			{
				userInput->setMouseCursor(mState_DONT);
			}
		} 
		else if ( controlGui.getSalvage() )
			doSalvage();
		else if ( controlGui.getGuard() )
			doGuard(target);
		else if ( controlGui.getRepair() && canRepair( target ) )
			doRepair(target);
		else if ( controlGui.getGuardTower() )
			doGuardTower();
		else if ( target->getTeam() && Team::home->isFriendly(target->getTeam()) && target->getFlag(OBJECT_FLAG_CANREFIT) && target->getFlag(OBJECT_FLAG_MECHBAY) && canRepairBay(target) )
			doRepairBay(target);
		else if ( controlGui.getSalvage() && canSalvage( target ))
			doSalvage();
		else if ( controlGui.isSelectingInfoObject() && target->isMover() )
			controlGui.setInfoWndMover( (Mover*)target );
		else if ( target->getTeam() != Team::home && !target->isDisabled() )
			doAttack();
		else if (!bForcedShot && !bAimedShot)
			soundSystem->playDigitalSample( INVALID_GUI );
	
	}
	else if ( selectClicked && !bGui )
	{
			//We clicked on a target.  If mouse cursor is normal, we want to select a friendly.
				//if the mouse cursor is some form of attack, attack the target.
		if ( (controlGui.isAddingVehicle() && !paintingVtol[commanderID] && canAddVehicle( wPos )) || 
			(controlGui.isAddingSalvage() && !paintingVtol[commanderID] && canRecover( wPos )))
		{
			// Target has already been confirmed  as a mover(BattleMech) by canRecover.
			// OR it doesn't matter because the canAddVehicle part of the beginVtol function doesn't reference it!
			// Again, Its OK because we only use it for salvage craft.
			// Need to pass this in so that Multiplayer can pass it in.
			beginVtol(-1,commanderID,NULL,(MoverPtr)target);	//In Single player, this should always be zero?
			return;
		}
		else if ( (controlGui.isAddingVehicle() && !paintingVtol[commanderID] && !canAddVehicle( wPos )) || 
				(controlGui.isAddingSalvage() && !paintingVtol[commanderID] && !canRecover( wPos )))
		{
			soundSystem->playDigitalSample( INVALID_GUI );
			return;
		}
		else if ( controlGui.isAddingAirstrike() && !paintingVtol[commanderID] ) // if we're painting the vtol, carry on
		{
			addAirstrike();
			return;
		}

		else if ( target )
		{
			if ( target->isMover() && ( target->getTeamId() == Team::home->getId() || 
					CONTACT_VISUAL == ((Mover*)target)->getContactStatus(Team::home->getId(), true) 
					|| target->isDisabled() ) )
				{
					controlGui.setInfoWndMover( (Mover*)target );
				}
		
			if (target->getCommanderId() == Commander::home->getId() && !target->isDisabled()  )
			{
				
				//--------------------------------------------------------------------
				// User wants to select the guy we are on.  If he did
				// it with a shift, add him.  If not, whack all and just add target!
				if (shiftDn)
				{
					bool alreadyThere = false;
					//-------------------------------------------
					// First, check if he's already there!
					for (long i=0;i<pTeam->getRosterSize();i++)
					{
						Mover* pMover = (Mover*)pTeam->getMover( i );
						if (pMover == target && pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
							alreadyThere = true;
					}
					
					if (!alreadyThere && !target->isDisabled()) {
						target->setSelected( true );
						// M1.6 Q6 site 3: legacy Shift+additive-select on
						// friendly mover (AOE style). Sets fallback gate;
						// tail tryStaticPropPick short-circuits so
						// Section 11 invariant holds.
						moverSelectedThisFrame = true;
					} else {
						// M1.6 Q6 RESOLVED: setSelected(false) toggle-off
						// site MUST NOT set the gate per spec rationale.
						target->setSelected( false );
					}
				}
				else if ( target->isMover() )
				{
					for (long i=0;i<pTeam->getRosterSize();i++)
					{
						Mover* pMover = (Mover*)pTeam->getMover( i );
						if (pMover->getCommander()->getId() == Commander::home->getId())
						{
							// M1.6 Q6 RESOLVED: clear-others setSelected(false)
							// loop MUST NOT set the gate.
							pMover->setSelected( false );
						}
					}

					target->setSelected( true );
					// M1.6 Q6 site 4: legacy plain-LMB-select on friendly
					// mover (AOE style). Sets fallback gate.
					moverSelectedThisFrame = true;
				}
			}
		}
		else
		{	
			// tried to select nothing
			controlGui.cancelInfo();
			soundSystem->playDigitalSample( INVALID_GUI );
		}
	}

	// Task 7: Ctrl+Shift+LMB inspector pick. Runs before the legacy
	// gameplay picks so the inspector can claim the click first.
	// Priority: ImGui window capture > inspector > existing gameplay picks.
	//
	// Gate uses IsWindowHovered(AnyWindow) rather than WantCaptureMouse.
	// WantCaptureMouse is tainted by mouse_any_down (true whenever any button
	// is held, even over the viewport) and would suppress the pick during the
	// entire duration of every click.  IsWindowHovered is purely spatial --
	// true only when the cursor is physically over an ImGui window -- so it
	// correctly blocks the pick only when the user is interacting with ImGui.
	bool pickedByInspector = false;
#ifdef MC2_IMGUI
	if (ctrlDn && shiftDn && leftClicked && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
		GameplayPickRequest inspReq;
		inspReq.mouseX               = mouseX;
		inspReq.mouseY               = mouseY;
		inspReq.shiftDn              = true;    // shift IS held (ctrl+shift+LMB); required by tryGameplayPick gate
		inspReq.leftClicked          = true;
		inspReq.bGui                 = bGui;
		inspReq.bLeftDouble          = false;
		inspReq.moverSelectedThisFrame = false; // inspector always attempts the lookup
		GameplayPickResult inspResult = tryGameplayPick(inspReq);
		EditorInspector::setPickResult(mouseX, mouseY, inspResult.lookup);
		if (inspResult.lookup.isValid) {
			if (inspResult.lookup.kind == RenderWorld::RenderObjectKind::StaticProp) {
				EditorInspector::StaticPropInspectorData sd;
				sd.populated   = true;
				sd.recipeIndex = static_cast<int32_t>(inspResult.lookup.gameObjectId);
				GameAdapters::StaticProp::getRecipeShapeName(
				    sd.recipeIndex, sd.shapeName, sizeof(sd.shapeName));
				sd.materialGpuActive = GameAdapters::StaticProp::isMaterialGpuActive();
				{
					RenderCore::MaterialHandle mh;
					mh.bits = inspResult.lookup.materialHandleBits;
					sd.materialIdx = mh.index();
					sd.materialGen = mh.generation();
				}
				sd.materialGpuPopulated = GameAdapters::StaticProp::getMaterialGpuData(
				    inspResult.lookup.materialHandleBits, &sd.materialGpu);
				EditorInspector::setStaticPropData(sd);
			} else if (inspResult.lookup.kind == RenderWorld::RenderObjectKind::Mech) {
				BattleMech* bm = GameAdapters::Mech::findMechByHandle(inspResult.lookup.handle);
				if (bm) {
					EditorInspector::MechInspectorData md;
					md.populated   = true;
					strncpy_s(md.variantName, sizeof(md.variantName), bm->variantName, _TRUNCATE);
					strncpy_s(md.longName,    sizeof(md.longName),    bm->longName,    _TRUNCATE);
					md.chassisClass = static_cast<int>(bm->chassisClass);
					md.teamId       = bm->getTeamId();
					md.disabled     = bm->isDisabled();
					md.destroyed    = bm->isDestroyed();
					md.crippled     = bm->isCrippled();
					md.conStat      = bm->conStat;
					for (int i = 0; i < MAX_MOVER_ARMOR_LOCATIONS; ++i) {
						md.totalCurArmor += bm->armor[i].curArmor;
						md.totalMaxArmor += static_cast<float>(bm->armor[i].maxArmor);
					}
					for (int i = 0; i < MAX_MOVER_BODY_LOCATIONS; ++i) {
						md.totalCurStr += bm->body[i].curInternalStructure;
						md.totalMaxStr += static_cast<float>(bm->body[i].maxInternalStructure);
					}
					MechWarriorPtr pilot = bm->getPilot();
					if (pilot)
						strncpy_s(md.pilotName, sizeof(md.pilotName), pilot->getName(), _TRUNCATE);
					EditorInspector::setMechData(md);
				}
#ifdef MC2_IS_EDITOR
				else if (Unit* u = EditorObjectMgr::instance()->findUnitByMechHandle(inspResult.lookup.handle)) {
					EditorInspector::MechInspectorData md;
					md.populated = true;
					strncpy_s(md.variantName, sizeof(md.variantName), u->getDisplayName(), _TRUNCATE);
					md.teamId    = u->getAlignment();
					Pilot* ep    = u->getPilot();
					if (ep)
						strncpy_s(md.pilotName, sizeof(md.pilotName), ep->getName(), _TRUNCATE);
					EditorInspector::setMechData(md);
				}
#endif
			}
		}
		// IMG-INSPECT-2: terrain CPU fallback when object-ID misses.
		// Terrain::IsGameSelectTerrainPosition(wPos) is the canonical
		// "world point is on selectable map terrain" guard (also used
		// at line ~789 for LOS/cell lookup). If OID buffer is disabled
		// (MC2_OBJECT_ID_BUFFER=0), inspResult.lookup.isValid is always
		// false here so terrain wins on any in-map click - expected.
		bool inspectorConsumed = inspResult.lookup.isValid;
		if (!inspResult.lookup.isValid && land &&
				Terrain::IsGameSelectTerrainPosition(wPos)) {
			EditorInspector::TerrainInspectorData td;
			td.populated = true;
			td.worldX    = static_cast<float>(wPos.x);
			td.worldY    = static_cast<float>(wPos.y);
			td.worldZ    = static_cast<float>(wPos.z);
			land->worldToTile(wPos, td.tileRow, td.tileCol);
			land->worldToCell(wPos, td.cellRow, td.cellCol);
			EditorInspector::setTerrainData(td);
			inspectorConsumed = true;
		}
		if (inspectorConsumed)
			pickedByInspector = true;
	}
#endif

	// M1.6: env-gated static-prop pick attempt. Runs AFTER the legacy
	// mover-selection path above. Short-circuits internally when
	// moverSelectedThisFrame == true (Section 11 + Q6/Q8 invariant) or
	// when MC2_STATIC_PROP_PICK / MC2_OBJECT_ID_BUFFER are off.
	if (!pickedByInspector) {
		const bool bLeftDouble = userInput->isLeftDoubleClick();
		tryStaticPropPick(moverSelectedThisFrame,
		                  shiftDn,
		                  leftClicked,
		                  bGui,
		                  bLeftDouble,
		                  mouseX,
		                  mouseY);
		// M2.6: mech-pick consumer (mirrors updateOldStyle tail).
		tryMechPick(moverSelectedThisFrame,
		            shiftDn,
		            leftClicked,
		            bGui,
		            bLeftDouble,
		            mouseX,
		            mouseY);
	}
}

void MissionInterfaceManager::updateWaypoints (void)
{
	// Update the waypoint markers so that they are visible and must happen AFTER camera update!!
	// Or they wiggle something fierce.
   	if ( userInput->getKeyDown( WAYPOINT_KEY ) || controlGui.getMines() )
   	{
   		Team* pTeam = Team::home;
   		for (long i=0;i<pTeam->getRosterSize();i++)
   		{
   			Mover* pMover = (Mover*)pTeam->getMover( i );
   			if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
   			{
   				pMover->updateDrawWaypointPath();
   			}
   		}
   		
   	}
}

int MissionInterfaceManager::update( bool leftClickedClick, bool rightClickedClick, int MouseX, int MouseY, GameObject* pTarget, bool bLOS )
{
	bool shiftDn = userInput->shift();
	bool ctrlDn = userInput->ctrl();
	bool altDn = userInput->alt();
	bool wayPtDn = userInput->getKeyDown( WAYPOINT_KEY );

	bool bRetVal = 0;

	int i = 0;
	int last = MAX_COMMAND;

	// if chatting, ignore keyboard input
	if ( controlGui.updateChat() )
	{
		i = 100;
		last = 102;
		bRetVal = 1;
	}


	if ( gos_GetKeyStatus( WAYPOINT_KEY ) == KEY_RELEASED )
	{
		Team* pTeam  = Team::home;
		for (long i = 0; i < pTeam->getRosterSize(); i++)
		{
			Mover* pMover = (Mover*)pTeam->getMover( i );
			if ( pMover->isSelected() && !pMover->getPilot()->getExecutingTacOrderQueue() && pMover->getCommander()->getId() == Commander::home->getId() )
			{
				if (MPlayer && !MPlayer->isServer()) {
					TacticalOrder tacOrder;
					tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_WAYPOINTS_DONE);
					tacOrder.pack(NULL, NULL);					
					MPlayer->sendPlayerOrder(&tacOrder, false, 1, &pMover);
					}
				else {
					pMover->getPilot()->setExecutingQueue(true);
					pMover->getPilot()->executeTacOrderQueue();
				}
			}
		}
		bRetVal = 1;
	}

	mouseX = MouseX;
	mouseY = mouseY;
	setTarget( pTarget );


	if ( !bRetVal )
	{
		for ( int j = KEY_0; j < KEY_9 + 1; j++ )
		{
			if ( userInput->getKeyDown( (gosEnum_KeyIndex)j )
				&& gos_GetKeyStatus( (gosEnum_KeyIndex)j ) != KEY_HELD )
			{
				if ( ctrlDn )
					makeForceGroup( j - KEY_0 );
				else if ( shiftDn )
					selectForceGroup( j - KEY_0, 0 );
				else
					selectForceGroup( j - KEY_0, 1 );
			}
		}
	}

	for ( ; i < last; i++ )
	{
		int key = commands[i].key;
		if ( userInput->getKeyDown( gosEnum_KeyIndex(key & 0x0000000ff) ) )
		{
			// check for shifts and stuff
			// must check way pt first, because it can be shift or ctrl
			if ( (key & WAYPT) )
			{	if ( !wayPtDn )
   					continue;
			}
			else if ( ((key & SHIFT) ? true : false) != shiftDn )
				continue;
			else if ( ((key & CTRL) ? true : false) != ctrlDn )
				continue;
			else if ( ((key & ALT) ? true : false) != altDn )
				continue;
			
			// got this far, call the command
			if ( commands[i].key != -1 )
				userInput->setMouseCursor(bLOS ? commands[i].cursorLOS : commands[i].cursor);

			if ( !commands[i].singleClick )
			{
				if ( (this->*commands[i].function)() )
					bRetVal = 1;
			}
			else if ( gos_GetKeyStatus( (gosEnum_KeyIndex)(key & 0x000fffff) ) != KEY_HELD )
			{
				if ( this->commands[i].function && (this->*commands[i].function)() )
					bRetVal = 1;
				terrainLineChanged = turn;
			}	
		}
		else if ( gos_GetKeyStatus( gosEnum_KeyIndex(key & 0x000fffff) ) == KEY_RELEASED 
			&& commands[i].releaseFunction )
		{
			if ( (this->*commands[i].releaseFunction)() )
				bRetVal = 1;
		}
	}

	return bRetVal;
}

void MissionInterfaceManager::doAttack()
{

	if ( userInput->getKeyDown( WAYPOINT_KEY ) || !target )
	{
		soundSystem->playDigitalSample( INVALID_GUI );
		return; // don't do if in waypoint mode
	}

	// TARGETING-CURRENTNESS-GUARD-1 (non-fatal, recon — FRAME-CURRENTNESS-GUARDS-1).
	// Consumer-boundary observer: at the instant an attack order is about to issue,
	// measure whether `target` is actually current — its watch-ID still resolves to
	// the same object, it projects in front of the camera, and it lands near the
	// cursor. This is the "eligible across the whole map" symptom (a stale/offscreen
	// target accepted because doAttack only checks `!target` + friendly). No behavior
	// change; this only counts/traces so we can prove whether bad orders actually
	// issue before adding any suppression. Gate MC2_TARGETING_GUARD +
	// MC2_DIAG_TAGS=TARGETING. Read via get_diagnostic_events("TARGETING").
	{
		static const bool s_tgtGuard = (std::getenv("MC2_TARGETING_GUARD") != nullptr);
		if ( s_tgtGuard && eye && mc2_diag::tagEnabled("TARGETING") )
		{
			static uint64_t s_tg_orders = 0, s_tg_staleWID = 0, s_tg_behind = 0, s_tg_far = 0;
			++s_tg_orders;
			const bool _tg_staleWID = (ObjectManager->getByWatchID( target->getWatchID() ) != target);
			Stuff::Vector3D _tg_pos = target->getPosition();
			Stuff::Vector4D _tg_sp;
			eye->projectForScreenXY( _tg_pos, _tg_sp );
			const bool  _tg_behind = (_tg_sp.w <= 1e-4f);
			const float _tg_dx = _tg_sp.x - (float)mouseX;
			const float _tg_dy = _tg_sp.y - (float)mouseY;
			const float _tg_distSq = _tg_dx*_tg_dx + _tg_dy*_tg_dy;
			const bool  _tg_far = (_tg_distSq > 90000.0f);   // >300px from cursor
			if (_tg_staleWID) ++s_tg_staleWID;
			if (_tg_behind)   ++s_tg_behind;
			if (_tg_far)      ++s_tg_far;
			if (_tg_staleWID || _tg_behind || _tg_far)
			{
				char _tg_buf[384];
				snprintf(_tg_buf, sizeof(_tg_buf),
				         "{\"site\":\"doAttack\",\"stale_wid\":%d,\"behind\":%d,\"far_cursor\":%d,"
				         "\"target_screen\":[%.1f,%.1f],\"w\":%.4f,\"mouse\":[%ld,%ld],\"dist_sq\":%.0f,"
				         "\"is_mover\":%d,\"watch_id\":%d,"
				         "\"totals\":{\"orders\":%llu,\"stale_wid\":%llu,\"behind\":%llu,\"far\":%llu}}",
				         _tg_staleWID?1:0, _tg_behind?1:0, _tg_far?1:0,
				         _tg_sp.x, _tg_sp.y, _tg_sp.w, (long)mouseX, (long)mouseY, _tg_distSq,
				         target->isMover()?1:0, (int)target->getWatchID(),
				         (unsigned long long)s_tg_orders, (unsigned long long)s_tg_staleWID,
				         (unsigned long long)s_tg_behind, (unsigned long long)s_tg_far);
				mc2_diag::writeEvent("TARGETING", 1, 0, _tg_buf);
			}
		}
	}

	TacticalOrder tacOrder;
	bool bCapture =  target->isCaptureable(Team::home->getId());
	bool bFireFromCurrentPos = controlGui.getCurrentRange() == FIRERANGE_CURRENT ? 1 : 0;
	if ( controlGui.getFireFromCurrentPos() )
		bFireFromCurrentPos = true;

	if (!bCapture)
	{
		if (!Team::home->isFriendly( target->getTeam() ))
		{
			tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_OBJECT );
			tacOrder.targetWID = target->getWatchID();
			tacOrder.attackParams.type = bEnergyWeapons ? ATTACK_CONSERVING_AMMO : ATTACK_TO_DESTROY;
			tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
			tacOrder.attackParams.range = FIRERANGE_OPTIMAL;
			tacOrder.attackParams.pursue = !bFireFromCurrentPos;
			tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;

			target->getAppearance()->flashBuilding(1.3f,0.2f,0xffff0000);
		}
		else
		{
			//Do NOT Attack friendlies unless force Fire.
			// That TacOrder is issued elsewhere!!
			return;
		}
	}
	else
	{
		for (long i = 0; i < Team::home->getRosterSize(); i++)
		{
			Mover* pMover = (Mover*)Team::home->getMover( i );
			if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
			{
				if ((pMover->getObjectClass() == BATTLEMECH) && 
					(pMover->getMoveType() == MOVETYPE_GROUND))
				{
					if ( target->getCaptureBlocker(pMover) )
					{
						soundSystem->playDigitalSample( INVALID_GUI );
						return;
					}
				}
				else
				{
					//Can't capture player has at least one support thing selected
					// At least until design tells me what they REALLY want!
					// -fs
					soundSystem->playDigitalSample( INVALID_GUI );
					return;
				}
			}
		}
		
		tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_CAPTURE );
		tacOrder.targetWID = target->getWatchID();
		tacOrder.attackParams.type = ATTACK_NONE;
		tacOrder.attackParams.method = ATTACKMETHOD_RAMMING;
		tacOrder.attackParams.pursue = true;
		tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
	}
	
	soundSystem->playDigitalSample(BUTTON5);

	Team* pTeam = Team::home;
	
	for (long i = 0; i < pTeam->getRosterSize(); i++)
	{
		Mover* pMover = (Mover*)pTeam->getMover( i );
		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId())
		{
			tacOrder.attackParams.range = (FireRangeType)pMover->attackRange;
			if (pMover->attackRange == FIRERANGE_CURRENT)
				tacOrder.attackParams.pursue = false;
			else if (controlGui.getFireFromCurrentPos())
				tacOrder.attackParams.pursue = false;
			else
				tacOrder.attackParams.pursue = true;
			tacOrder.pack(NULL, NULL);

			//---------------------------------------------------------------------
			// Helper function--perhaps this should just be a part of the mover and
			// group handleTacticalOrder() functions?
			if (MPlayer && !MPlayer->isServer())
				MPlayer->sendPlayerOrder(&tacOrder, false, 1, &pMover);
			else
				pMover->handleTacticalOrder(tacOrder);
		}
	}

	controlGui.setFireFromCurrentPos( 0 );
}

int MissionInterfaceManager::attackShort()
{
	controlGui.setRange( FIRERANGE_SHORT );
	if ( commandClicked && target )
	{
		doAttack();
		return 1;
	}
	return 0;
}

int MissionInterfaceManager::attackMedium()
{
	controlGui.setRange( FIRERANGE_MEDIUM );
	if ( commandClicked && target )
	{
		doAttack();
		return 1;
	}
	return 0;
}

int MissionInterfaceManager::attackLong()
{
	controlGui.setRange( FIRERANGE_LONG );
	if ( commandClicked && target )
	{
		doAttack();
		return 1;
	}
	return 0;
}

int MissionInterfaceManager::alphaStrike()
{
	return 0;
}
int MissionInterfaceManager::defaultAttack()
{
	
	controlGui.setRange( FIRERANGE_OPTIMAL );

	return 0;
}

int  MissionInterfaceManager::jump()
{
	if ( !canJump() )
	{
		soundSystem->playDigitalSample( INVALID_GUI );
		return 0;
	}
	
	if ( !controlGui.getJump() )
		controlGui.toggleJump();
	return 0;
}

int MissionInterfaceManager::stopJump()
{
	if ( controlGui.getJump() )
		controlGui.toggleJump();
	return 1;

}
void MissionInterfaceManager::doJump()
{
	bool passable = 0;

	if ( !target )
	{
		if (Terrain::IsGameSelectTerrainPosition(wPos))
		{
			int cellR, cellC;
			land->worldToCell(wPos, cellR, cellC);
			passable = GameMap->getPassable(cellR,cellC);
		}
			
		bool canJump = canJumpToWPos();	
		if (canJump)
		{
			TacticalOrder tacOrder;
			tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_JUMPTO_POINT, false);
		
			tacOrder.moveParams.wait = false;
			tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_JUMP;
			tacOrder.setWayPoint( 0, wPos );
			tacOrder.pack(NULL, NULL);

			handleOrders(tacOrder);

			controlGui.setDefaultSpeed();
			
			soundSystem->playDigitalSample(BUTTON5);
			controlGui.setDefaultSpeed();
		}
		else
		{
			userInput->setMouseCursor(mState_DONT);
			soundSystem->playDigitalSample( INVALID_GUI );
		}
		
			
	}
	else
	{
		userInput->setMouseCursor(mState_DONT);
		soundSystem->playDigitalSample( INVALID_GUI );
	}
	
}
		
int MissionInterfaceManager::fireFromCurrentPos()
{
	if ( !controlGui.getFireFromCurrentPos() )
		controlGui.toggleFireFromCurrentPos( );

	return forceShot();
}
int MissionInterfaceManager::stopFireFromCurrentPos()
{
	if ( controlGui.getFireFromCurrentPos() )
		controlGui.toggleFireFromCurrentPos();
	return 1;
}

void MissionInterfaceManager::doGuard(GameObject* who)
{
	if ( who )
	{
		TacticalOrder tacOrder;
		tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_GUARD);
		tacOrder.targetWID = who->getWatchID();
		tacOrder.pack(NULL, NULL);	
		handleOrders( tacOrder );

		controlGui.setDefaultSpeed();
	}
	else
	{
		LocationNode path;
		path.location = wPos;
		path.run = true;
		path.next = NULL;

		TacticalOrder tacOrder;
		tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_GUARD);
		tacOrder.initWayPath( &path );
		tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
		tacOrder.pack(NULL, NULL);	
		handleOrders( tacOrder );

		controlGui.setDefaultSpeed();
	}

	soundSystem->playDigitalSample(BUTTON5);
}
int MissionInterfaceManager::guard()
{
	if ( !controlGui.getGuard() )
		controlGui.toggleGuard();
	return 0;
}
int MissionInterfaceManager::stopGuard()
{
	if ( controlGui.getGuard() )
		controlGui.toggleGuard();
	return 0;
}
int MissionInterfaceManager::conserveAmmo()
{
	return 0;
}
int MissionInterfaceManager::selectVisible()
{
	Team* pTeam = Team::home;
	for (long i=0;i<pTeam->getRosterSize();i++)
	{
		Mover* pMover = (Mover*)pTeam->getMover( i );
		if (pMover->getCommander()->getId() == Commander::home->getId())
		{
			pMover->setSelected( false );
		}
	}
		
	return addVisibleToSelection();

}

int MissionInterfaceManager::addVisibleToSelection()
{
	//----------------------------------------------------------------
	// Grab each member of the homeTeam and see if they are onScreen
	dragStart.Zero();		
	dragEnd = eye->getScreenRes();
	
	for (int i=0;i<ObjectManager->getNumMovers();i++)
	{
		if (ObjectManager->moverInRect(i,dragStart,dragEnd))
		{
			GameObjectPtr	mover = ObjectManager->getMover(i);
			if (mover && (mover->getCommanderId() == Commander::home->getId()) && !mover->isDisabled())
			{
				mover->setSelected( true );
			}
		}
	}

	dragStart.Zero();
	dragEnd.Zero();

	return 1;
}
int MissionInterfaceManager::aimLeg()
{
	if ( target )
	{
		BattleMech* pMech = dynamic_cast<BattleMech*>(target );
		
		if ( pMech && pMech->getMoveType() == MOVETYPE_GROUND)
		{
			if (!anySelectedWithoutAreaEffect())
			{
				//Can't aim shot.  No weapon which is not area effect available!
				userInput->setMouseCursor( mState_DONT );
				return 1;		
			}
			else if ( pMech->body[MECH_BODY_LOCATION_LLEG].damageState == IS_DAMAGE_DESTROYED 
				&& pMech->body[MECH_BODY_LOCATION_RLEG].damageState == IS_DAMAGE_DESTROYED )
			{
				userInput->setMouseCursor( mState_DONT );
				return 1;		
			}
			else  if ( !commandClicked )// cursor already set above, however if in doubt 
				return 1;
			else
			{
				TacticalOrder tacOrder;
				tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_OBJECT);
				tacOrder.targetWID = target->getWatchID();
				tacOrder.attackParams.type = ATTACK_TO_DESTROY;
				tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
				tacOrder.attackParams.range = FIRERANGE_OPTIMAL;
				tacOrder.attackParams.pursue = controlGui.getFireFromCurrentPos() ? false : true;
				tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
				
				bool bRight = pMech->body[MECH_BODY_LOCATION_LLEG].damageState == IS_DAMAGE_DESTROYED;
				tacOrder.attackParams.aimLocation = bRight ? MECH_BODY_LOCATION_RLEG :  MECH_BODY_LOCATION_LLEG;
				tacOrder.pack( NULL, NULL);
				handleOrders( tacOrder );

				soundSystem->playDigitalSample(BUTTON5);

				bAimedShot = true;
				return 1;
			}
			
		}
	}
	else
	{
		userInput->setMouseCursor( mState_DONT );
		return 1;
	}

	return 0;
}

int MissionInterfaceManager::aimArm()
{
	BattleMech* pMech = dynamic_cast<BattleMech*>(target);
		
	if ( pMech && pMech->getMoveType() == MOVETYPE_GROUND)
	{
		if ( anySelectedWithoutAreaEffect() && commandClicked )
		{
			TacticalOrder tacOrder;
			tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_OBJECT);
			tacOrder.targetWID = target->getWatchID();
			tacOrder.attackParams.type = ATTACK_TO_DESTROY;
			tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
			tacOrder.attackParams.pursue = controlGui.getFireFromCurrentPos() ? false : true;
			tacOrder.attackParams.range = FIRERANGE_OPTIMAL;
			tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
			bool bRight = pMech->body[MECH_BODY_LOCATION_LARM].damageState == IS_DAMAGE_DESTROYED;
			tacOrder.attackParams.aimLocation = bRight ? MECH_BODY_LOCATION_RARM :  MECH_BODY_LOCATION_LARM;
			tacOrder.pack( NULL, NULL);
			handleOrders( tacOrder );

			soundSystem->playDigitalSample(BUTTON5);

			bAimedShot = true;
			return 1;
		}

		if (!anySelectedWithoutAreaEffect())
		{
			//Can't aim shot.  No weapon which is not area effect available!
			userInput->setMouseCursor( mState_DONT );
			return 1;		
		}
		else if ( pMech->body[MECH_BODY_LOCATION_LARM].damageState == IS_DAMAGE_DESTROYED 
			&& pMech->body[MECH_BODY_LOCATION_RARM].damageState == IS_DAMAGE_DESTROYED )
		{
			userInput->setMouseCursor( mState_DONT );
			return 1;		
		}
		else // cursor already set above, however if in doubt 
			return 1;
	}
	else
	{
		userInput->setMouseCursor( mState_DONT );
		return 1;
	}
}

int MissionInterfaceManager::aimHead()
{
	if ( target )
	{
		BattleMech* pMech = dynamic_cast<BattleMech*>(target);
		
		if ( pMech && pMech->getMoveType() == MOVETYPE_GROUND)
		{
			if (anySelectedWithoutAreaEffect() && commandClicked )
			{
				TacticalOrder tacOrder;

				tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_OBJECT);
				tacOrder.targetWID = target->getWatchID();
				tacOrder.attackParams.type = ATTACK_TO_DESTROY;
				tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
				tacOrder.attackParams.pursue = controlGui.getFireFromCurrentPos() ? false : true;
				tacOrder.attackParams.range = FIRERANGE_OPTIMAL;
				tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
				tacOrder.attackParams.aimLocation = MECH_BODY_LOCATION_HEAD;
				tacOrder.pack( NULL, NULL);
				handleOrders( tacOrder );

				bAimedShot = true;
				soundSystem->playDigitalSample(BUTTON5);
			}
			else if (!anySelectedWithoutAreaEffect())
			{
				//Can't aim shot.  No weapon which is not area effect available!
				userInput->setMouseCursor( mState_DONT );
				return 1;		
			}
			else if ( pMech->body[MECH_BODY_LOCATION_HEAD].damageState == IS_DAMAGE_DESTROYED )
			{
				userInput->setMouseCursor( mState_DONT );
				return 1;		
			}
			else // cursor already set above, however if in doubt 
				return 1;
		}
		else
		{
			userInput->setMouseCursor( mState_DONT );
			return 1;
		}
	}
	else
	{
		userInput->setMouseCursor( mState_DONT );
		return 1;
	}

	return 0;
}

int MissionInterfaceManager::removeCommand()
{
	controlGui.doStop();
	return 1;
}
int MissionInterfaceManager::powerUp()
{
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_POWERUP, true);
	tacOrder.pack(NULL, NULL);					
	handleOrders( tacOrder );
	return 1;
}
int MissionInterfaceManager::powerDown()
{
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_POWERDOWN, true);
	tacOrder.pack(NULL, NULL);
	handleOrders( tacOrder );
	return 1;
					
}
int MissionInterfaceManager::changeSpeed()
{
 	if ( controlGui.isDefaultSpeed() )
		controlGui.toggleDefaultSpeed();
	return 0;
}

int MissionInterfaceManager::stopChangeSpeed()
{
	if ( !controlGui.isDefaultSpeed() )
		controlGui.toggleDefaultSpeed();
	return 1;
}
int MissionInterfaceManager::eject()
{
	if ( target && target->isMover() && target->getCommanderId() == Commander::home->getId() && !invulnerableON)
	{
		if (  commandClicked )
		{
			doEject( target );
		}

		userInput->setMouseCursor( mState_EJECT );
	
	}
	else if ( !controlGui.forceGroupBar.inRegion( userInput->getMouseX(), userInput->getMouseY() ) )
		userInput->setMouseCursor( mState_XEJECT );
	return 1;
}

int MissionInterfaceManager::bigAirStrike()
{
	Stuff::Vector3D v = makeAirStrikeTarget( wPos );

	IfaceCallStrike (ARTILLERY_LARGE,&v,NULL);
//	if ( !isPaused() )
		soundSystem->playSupportSample(SUPPORT_AIRSTRIKE);
	return 1;
}
int MissionInterfaceManager::smlAirStrike()
{
	Stuff::Vector3D v = makeAirStrikeTarget( wPos );

	IfaceCallStrike (ARTILLERY_SMALL,&v,NULL);
//	if ( !isPaused() )
		soundSystem->playSupportSample(SUPPORT_AIRSTRIKE);
	return 1;
}
int MissionInterfaceManager::snsAirStrike()
{
	Stuff::Vector3D v = makeAirStrikeTarget( wPos );

	IfaceCallStrike (ARTILLERY_SENSOR,&v,NULL);
//	if ( !isPaused() )
		soundSystem->playSupportSample(SUPPORT_PROBE);
	return 1;
}

Stuff::Vector3D MissionInterfaceManager::makeAirStrikeTarget( const Stuff::Vector3D& pos )
{
	Stuff::Vector3D newPos = pos;
/*	bool 	lineOfSight = Team::home->teamLineOfSight(pos);
	if ( !lineOfSight )
	{
		// need to offset x, y, random amounts up to five tiles... 
		long randX = rand();
		long randY = rand();
		long offsetX = randX % (128*5);
		long offsetY = randY % (128*5);
		if ( randX % 2 )
			offsetX = -offsetX;
		if ( randY % 2 )
			offsetY = -offsetY;

		newPos.x += offsetX;
		newPos.y += offsetY;
	}*/

	return newPos;
}

int MissionInterfaceManager::cameraAssign0()
{
	Camera::cameraZoom[0] = eye->cameraAltitude;
	Camera::cameraTilt[0] = eye->getProjectionAngle();

	return 1;
}

int MissionInterfaceManager::cameraAssign1()
{
	Camera::cameraZoom[1] = eye->cameraAltitude;
	Camera::cameraTilt[1] = eye->getProjectionAngle();

	return 1;
}

int MissionInterfaceManager::cameraAssign2()
{
	Camera::cameraZoom[2] = eye->cameraAltitude;
	Camera::cameraTilt[2] = eye->getProjectionAngle();

	return 1;
}

int MissionInterfaceManager::cameraAssign3()
{
	Camera::cameraZoom[3] = eye->cameraAltitude;
	Camera::cameraTilt[3] = eye->getProjectionAngle();

	return 1;
}

int MissionInterfaceManager::cameraNormal()
{
	if (eye)
		eye->allNormal();

	return 1;
}


int MissionInterfaceManager::cameraDefault()
{
	if (eye)
		eye->setCameraView(0);
 		
	return 1;
}
int MissionInterfaceManager::cameraMaxIn()
{
	if (eye)
		eye->setCameraView(1);

	return 1;
}
int MissionInterfaceManager::cameraMaxOut()
{
	if (eye)
		eye->ZoomMin();

	return 1;
}
int MissionInterfaceManager::cameraTight()
{
	if (eye)
		eye->setCameraView(2);
		
 	return 1;
}
int MissionInterfaceManager::cameraFour()
{
	if (eye)
		eye->setCameraView(3);
		
	return 1;
}

bool MissionInterfaceManager::anySelectedWithoutAreaEffect (void)
{
	//Scan the list of selected movers and see if any have at least one non-area effect weapon
	// Used for Called shots which cannot be called if weapon is area effect!!
	Team* pTeam = Team::home;
	for (long i = 0; i < pTeam->getRosterSize(); i++)
	{
		Mover* pMover = (Mover*)pTeam->getMover( i );
		if ( pMover->isSelected() && pMover->hasNonAreaWeapon() && pMover->getCommander()->getId() == Commander::home->getId())
		   	return true;
	}

	return false;
}

int MissionInterfaceManager::handleOrders( TacticalOrder& order)
{
	Team* pTeam = Team::home;

	//--------------------------------------------------------
	// First, find out how many are jumping and calc a list of
	// jump locations...
	bool isMoveOrder = false;
	Stuff::Vector3D moveGoals[MAX_MOVERS];
	long numMovers = 0;
	if (order.code == TACTICAL_ORDER_JUMPTO_POINT) {
		for (long i = 0; i < pTeam->getRosterSize(); i++) {
			Mover* pMover = pTeam->getMover(i);
			if (pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId())
				numMovers++;
		}
		MoverGroup::calcJumpGoals(order.getWayPoint(0), numMovers, moveGoals, NULL);
		isMoveOrder = true;
		}
	else if (order.code == TACTICAL_ORDER_MOVETO_POINT) {
		for (long i = 0; i < pTeam->getRosterSize(); i++) {
			Mover* pMover = pTeam->getMover(i);
			if (pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId())
				numMovers++;
		}
		MoverGroup::calcMoveGoals(order.getWayPoint(0), numMovers, moveGoals);
		isMoveOrder = true;
	}

	numMovers = 0;
	for (long i = 0; i < pTeam->getRosterSize(); i++)
	{
		Mover* pMover = (Mover*)pTeam->getMover( i );
		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
		{
			//---------------------------------------------------------------------
			// Helper function--perhaps this should just be a part of the mover and
			// group handleTacticalOrder() functions?
			if (isMoveOrder ) {
				order.setWayPoint(0, moveGoals[numMovers++]);
				order.pack(NULL, NULL);
			}
			if (MPlayer && !MPlayer->isServer()) {
				MPlayer->sendPlayerOrder(&order, false, 1, &pMover, 0, NULL, userInput->getKeyDown(WAYPOINT_KEY));
				//pMover->getPilot()->setExecutingQueue(false);
				//pMover->getPilot()->addQueuedTacOrder(order);					
				}
			else
			{
				order.attackParams.range = (FireRangeType)pMover->attackRange;
				order.pack( NULL, NULL );
				if ( userInput->getKeyDown( WAYPOINT_KEY ) ) // way point one
				{
					pMover->getPilot()->setExecutingQueue(FALSE);
					pMover->getPilot()->addQueuedTacOrder(order);					
				}
				else if ( controlGui.getMines() )
				{
					pMover->getPilot()->addQueuedTacOrder(order);
					pMover->getPilot()->setExecutingQueue(TRUE);
				}
				else
					pMover->handleTacticalOrder(order);
			}

		}
	}

	return 1;
}

int MissionInterfaceManager::scrollUp()
{
	float frameFactor = frameLength / baseFrameLength;
	float scrollFactor = scrollInc / eye->getScaleFactor() * frameFactor;
	eye->moveUp(scrollFactor);
	return 1;
}

int MissionInterfaceManager::scrollDown()
{
	float frameFactor = frameLength / baseFrameLength;
	float scrollFactor = scrollInc / eye->getScaleFactor() * frameFactor;
	eye->moveDown(scrollFactor);
	return 1;
}
int MissionInterfaceManager::scrollLeft()
{
	float frameFactor = frameLength / baseFrameLength;
	float scrollFactor = scrollInc / eye->getScaleFactor() * frameFactor;
	eye->moveLeft(scrollFactor);
	return 1;
}
int MissionInterfaceManager::scrollRight()
{
	float frameFactor = frameLength / baseFrameLength;
	float scrollFactor = scrollInc / eye->getScaleFactor() * frameFactor;
	eye->moveRight(scrollFactor);
	return 1;
}
// [MOUSE-ANCHORED-ZOOM-1 / FIX-4] Mouse-anchored zoom helper. Gated by
// MC2_LOWCAM_ZOOM_ANCHOR (default OFF -> legacy fixed-pivot zoom unchanged).
// Capture cursor world point A, scroll-target T and pre-step altitude h0 BEFORE
// the zoom, run the supplied legacy zoom (calling the member fn), then shift the
// scroll target so A stays under the cursor: T_new = T + (A - T)*(1 - h1/h0).
namespace {
	bool lowcamZoomAnchorEnabled()
	{
		static int cached = -1;
		if (cached < 0)
		{
			// Default OFF: anchored zoom still reads as a pan in practice; use the
			// legacy fixed-pivot wheel zoom. Opt in with MC2_LOWCAM_ZOOM_ANCHOR=1.
			const char* v = getenv("MC2_LOWCAM_ZOOM_ANCHOR");
			cached = (v && v[0] && v[0] != '0') ? 1 : 0;
		}
		return cached != 0;
	}
}

void MissionInterfaceManager::anchoredZoom(long mx, long my, int (MissionInterfaceManager::*zoomFn)())
{
	// OFF path: byte-identical to legacy — just run the zoom.
	if (!lowcamZoomAnchorEnabled() || !eye || !eye->getUsePerspective())
	{
		(this->*zoomFn)();
		return;
	}

	// [MOUSE-ANCHORED-ZOOM-1 v2] The prior single-shot position shift PANNED:
	// it used h1=cameraAltitudeDesired (the instant setpoint) but the camera's
	// ACTUAL altitude eases toward it over ~5 frames (Camera::update), so the
	// pivot snapped the full distance on frame 0 while altitude barely moved.
	// Fix: capture cursor-world A, the ACTUAL altitude h0, and pivot T0 here,
	// then let Camera::update() shift the pivot each frame LOCKED to the eased
	// altitude (frac = 1 - cameraAltitude/h0) until it settles. ZOOMs, no pan.
	Stuff::Vector3D A;
	bool haveA = eye->screenToTerrainApprox(mx, my, A);

	(this->*zoomFn)();   // updates cameraAltitudeDesired (the zoom target)

	// Fallback: cursor->world failed -> leave legacy fixed-pivot zoom, no jump.
	if (!haveA)
		return;
	eye->beginZoomAnchor(A, eye->getCameraAltitude(), eye->getPosition());
}

int MissionInterfaceManager::zoomOut()
{
	float frameFactor = frameLength / baseFrameLength;
	eye->ZoomOut(zoomInc * frameFactor * eye->getScaleFactor());
	return 1;
}
int MissionInterfaceManager::zoomIn()
{
	float frameFactor = frameLength / baseFrameLength;
	eye->ZoomIn(zoomInc * frameFactor * eye->getScaleFactor());
	return 1;
}
int MissionInterfaceManager::zoomChoiceOut()
{
	float frameFactor = frameLength / baseFrameLength;
	eye->ZoomOut(zoomInc * frameFactor * 18.0f * eye->getScaleFactor());

	return 1;
}
int MissionInterfaceManager::zoomChoiceIn()
{
	float frameFactor = frameLength / baseFrameLength;
	eye->ZoomIn(zoomInc * frameFactor * 18.0f * eye->getScaleFactor());
 	return 1;
}
int MissionInterfaceManager::rotateLeft()
{
	float frameFactor = frameLength / baseFrameLength;
	realRotation += degPerSecRot * frameFactor;
	if (realRotation >= rotationInc)
	{
		eye->rotateLeft(rotationInc);
		realRotation = 0;
	}

	return 1;
}
int MissionInterfaceManager::rotateRight()
{
	float frameFactor = frameLength / baseFrameLength;
	realRotation -= degPerSecRot * frameFactor;
	if (realRotation <= -rotationInc)
	{
		eye->rotateRight(rotationInc);
		realRotation = 0;
	}

	return 1;
}
int MissionInterfaceManager::tiltUp()
{
	float frameFactor = frameLength / baseFrameLength;
	float scrollFactor = scrollInc / eye->getScaleFactor() * frameFactor;
	eye->tiltDown(scrollFactor * frameFactor * 30.0f);
	return 1;
}
int MissionInterfaceManager::tiltDown()
{
	float frameFactor = frameLength / baseFrameLength;
	float scrollFactor = scrollInc / eye->getScaleFactor() * frameFactor;
	eye->tiltUp(scrollFactor * frameFactor * 30.0f);
	return 1;
	
}
int MissionInterfaceManager::centerCamera()
{
	return 0;
}
int MissionInterfaceManager::rotateLightLeft()
{
	#ifndef FINAL
	eye->rotateLightLeft(rotationInc);
	#endif
	return 1;
}
int MissionInterfaceManager::rotateLightRight()
{
	#ifndef FINAL
	eye->rotateLightRight(rotationInc);
	#endif
	return 1;
}
int MissionInterfaceManager::rotateLightUp()
{
	#ifndef FINAL
	eye->rotateLightUp(rotationInc);
	#endif
	return 1;
}
int MissionInterfaceManager::rotateLightDown()
{
	#ifndef FINAL
	eye->rotateLightDown(rotationInc);
	#endif
	return 1;
}

int MissionInterfaceManager::drawTerrain()
{
	#ifndef FINAL
	drawTerrainTiles ^= TRUE;
	#endif
	return 1;
}
int MissionInterfaceManager::drawOverlays()
{
	#ifndef FINAL
	drawTerrainOverlays ^= TRUE;
	#endif
	return 1;
}
int MissionInterfaceManager::drawBuildings()
{
	#ifndef FINAL
	renderObjects ^= TRUE;
	#endif
	return 1;
}
int MissionInterfaceManager::showGrid()
{
	#ifndef FINAL
	drawTerrainGrid = !drawTerrainGrid;
	#endif
	return 1;
}
int MissionInterfaceManager::recalcLights()
{
	#ifndef FINAL
	Terrain::recalcLight ^= TRUE;
	Terrain::recalcShadows = false;
	#endif
	return 1;
}
int MissionInterfaceManager::drawFog()
{
	#ifndef FINAL
	useFog ^= TRUE;
	#endif
	return 1;
}
int MissionInterfaceManager::usePerspective()
{
	#ifndef FINAL
	eye->usePerspective ^= true;
	#endif
	return 1;
}
int MissionInterfaceManager::toggleGUI()
{
	#ifndef FINAL
	drawGUIOn ^= true;
	toggledGUI = drawGUIOn;
	#endif
	return 1;
}
int MissionInterfaceManager::drawTGLShapes()
{
	#ifndef FINAL
	renderTGLShapes ^= true;
	terrainLineChanged = turn;
	#endif
	return 1;
}
int MissionInterfaceManager::drawWaterEffects()
{
	#ifndef FINAL
	useWaterInterestTexture ^= true;
	terrainLineChanged = turn;
	#endif
	return 1;
}
int MissionInterfaceManager::recalcWater()
{
	#ifndef FINAL
	Terrain::mapData->recalcWater();
	#endif
	return 1;
}
int MissionInterfaceManager::drawShadows()
{
	#ifndef FINAL
	useShadows ^= TRUE;
	#endif
	return 1;
}
int MissionInterfaceManager::changeLighting()
{
	#ifndef FINAL
	drawLOSGrid ^= true;
	#endif
	return 1;
}

void MissionInterfaceManager::init (FitIniFilePtr loader)
{
	long result = loader->seekBlock("CameraData");
	gosASSERT(result == NO_ERR);
	
	result = loader->readIdFloat("ScrollIncrement",scrollInc);
	gosASSERT(result == NO_ERR);
	scrollInc *= prefs.scrollSpeedMult;

	result = loader->readIdFloat("RotationIncrement",rotationInc);
	gosASSERT(result == NO_ERR);
	
	result = loader->readIdFloat("ZoomIncrement",zoomInc);
	gosASSERT(result == NO_ERR);
	zoomInc *= prefs.zoomSpeedMult;

	result = loader->readIdFloat("ScrollLeft",screenScrollLeft);
	gosASSERT(result == NO_ERR);
		
	result = loader->readIdFloat("ScrollRight",screenScrollRight);
	gosASSERT(result == NO_ERR);
	
	result = loader->readIdFloat("ScrollUp",screenScrollUp);
	gosASSERT(result == NO_ERR);
	
	result = loader->readIdFloat("ScrollDown",screenScrollDown);
	gosASSERT(result == NO_ERR);

	result = loader->readIdFloat("BaseFrameLength",baseFrameLength);
	gosASSERT(result == NO_ERR);
	
	result = loader->readIdFloat("RotationDegPerSec",degPerSecRot);
	gosASSERT(result == NO_ERR);
	
	float missionDragThreshold;
	result = loader->readIdFloat("MouseDragThreshold",missionDragThreshold);
	gosASSERT(result == NO_ERR);

	float newThreshold = missionDragThreshold / Environment.screenHeight;
	userInput->setMouseDragThreshold(newThreshold);
	
	float missionDblClkThreshold;
	result = loader->readIdFloat("MouseDoubleClickThreshold",missionDblClkThreshold);
	gosASSERT(result == NO_ERR);
	
	userInput->setMouseDoubleClickThreshold(missionDblClkThreshold);

	swapResolutions();
}	
	
//--------------------------------------------------------------------------------------
void MissionInterfaceManager::destroy (void)
{
	// [IFACE_PERF v1] flush the partial cost-split window at mission teardown
	// so runs shorter than 900 frames still emit numbers. No-op when gate OFF.
	mfEmitWindow("mission_end");

	hotKeyFont.destroy();
	hotKeyHeaderFont.destroy();

	delete keyboardRef;
	keyboardRef = NULL;
}	

//--------------------
// TO TEST LIGHTING MODEL
bool rotateLightRt = false;
bool rotateLightLf = false;
bool rotateLightUp = false;
bool rotateLightDn = false;
		
//--------------------------------------------------------------------------------------

void HandlePlayerOrder (MoverPtr mover, TacticalOrderPtr tacOrder) {

	//---------------------------------------------------------------------
	// Helper function--perhaps this should just be a part of the mover and
	// group handleTacticalOrder() functions?
	if (MPlayer && !MPlayer->isServer())
		MPlayer->sendPlayerOrder(tacOrder, false, 1, &mover);
	else
		mover->handleTacticalOrder(*tacOrder);
}
						
//--------------------------------------------------------------------------------------
void MissionInterfaceManager::drawVTOL (void)
{
	for (long vtolNum = 0;vtolNum < MAX_TEAMS; vtolNum++)
	{
		if ( paintingVtol[vtolNum] && vTol[vtolNum] )
		{
			vTol[vtolNum]->render();
			vTol[vtolNum]->renderShadows();
			
			if (dustCloud[vtolNum] && dustCloud[vtolNum]->IsExecuted())
			{
				gosFX::Effect::DrawInfo drawInfo;
				drawInfo.m_clipper = theClipper;
	
				Stuff::LinearMatrix4D 	shapeOrigin;
				Stuff::LinearMatrix4D	localToWorld;
				Stuff::LinearMatrix4D	localResult;
						
				Stuff::Vector3D dustPos = vTol[vtolNum]->position;
				Stuff::Point3D wakePos;
				wakePos.x = -dustPos.x;
				wakePos.y = dustPos.z;
				wakePos.z = dustPos.y;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(wakePos);
						
				drawInfo.m_parentToWorld = &shapeOrigin;
				if (!MLRVertexLimitReached)
					dustCloud[vtolNum]->Draw(&drawInfo);
			}
			
			if (recoveryBeam[vtolNum] && recoveryBeam[vtolNum]->IsExecuted())
			{
				gosFX::Effect::DrawInfo drawInfo;
				drawInfo.m_clipper = theClipper;
	
				Stuff::LinearMatrix4D 	shapeOrigin;
				Stuff::LinearMatrix4D	localToWorld;
				Stuff::LinearMatrix4D	localResult;
						
				Stuff::Vector3D dustPos = vTol[vtolNum]->position;
				Stuff::Point3D wakePos;
				wakePos.x = -dustPos.x;
				wakePos.y = dustPos.z;
				wakePos.z = dustPos.y;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(wakePos);
						
				drawInfo.m_parentToWorld = &shapeOrigin;
				if (!MLRVertexLimitReached)
					recoveryBeam[vtolNum]->Draw(&drawInfo);
			}
		}
	}
}

//--------------------------------------------------------------------------------------
void MissionInterfaceManager::render (void)
{
	if (drawGUIOn)
	{
		//-------------------------
		// Draw the dragSelect Box
		if (isDragging)
		{
			gos_VERTEX vertices[5];
			DWORD color = SB_WHITE;
			
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );
		
			gos_SetRenderState( gos_State_Specular,FALSE );

			gos_SetRenderState( gos_State_AlphaTest, TRUE );
					
			gos_SetRenderState( gos_State_Texture, 0 );

			gos_SetRenderState( gos_State_Filter, gos_FilterNone );

			Stuff::Vector4D screenPos;
			// [PROJECTZ:ScreenXYOracle id=gui_drag_box_origin]
			eye->projectForScreenXY( dragStart, screenPos );

			vertices[0].x 		= screenPos.x;
			vertices[0].y 		= screenPos.y;
			vertices[0].z		= 0.0;
			vertices[0].rhw		= 0.5;
			vertices[0].u		= 0.0;
			vertices[0].v		= 0.0;
			vertices[0].argb	= color;
			
			vertices[1].x 		= dragEnd.x;
			vertices[1].y 		= screenPos.y;
			vertices[1].z		= 0.0;
			vertices[1].rhw		= 0.5;
			vertices[1].u		= 0.0;
			vertices[1].v		= 0.0;
			vertices[1].argb	= color;
			
			vertices[2].x 		= dragEnd.x;
			vertices[2].y 		= dragEnd.y;
			vertices[2].z		= 0.0;
			vertices[2].rhw		= 0.5;
			vertices[2].u		= 0.0;
			vertices[2].v		= 0.0;
			vertices[2].argb	= color;
			
			vertices[3].x 		= screenPos.x;
			vertices[3].y 		= dragEnd.y;
			vertices[3].z		= 0.0;
			vertices[3].rhw		= 0.5;
			vertices[3].u		= 0.0;
			vertices[3].v		= 0.0;
			vertices[3].argb	= color;
		
			vertices[4].x 		= screenPos.x;
			vertices[4].y 		= screenPos.y;
			vertices[4].z		= 0.0;
			vertices[4].rhw		= 0.5;
			vertices[4].u		= 0.0;
			vertices[4].v		= 0.0;
			vertices[4].argb	= color;
			
			gos_SetRenderState( gos_State_IsHUD, 1 );
			gos_DrawQuads(vertices,4);

			vertices[0].argb	= SB_BLACK;
			vertices[1].argb	= SB_BLACK;
			vertices[2].argb	= SB_BLACK;
			vertices[3].argb	= SB_BLACK;
			vertices[4].argb	= SB_BLACK;

			gos_DrawLines(&vertices[0],2);
			gos_DrawLines(&vertices[1],2);
			gos_DrawLines(&vertices[2],2);
			gos_DrawLines(&vertices[3],2);
			gos_SetRenderState( gos_State_IsHUD, 0 );
		}

		if ( userInput->getKeyDown( WAYPOINT_KEY ) || controlGui.getMines() )
		{
			Team* pTeam = Team::home;
			for (long i=0;i<pTeam->getRosterSize();i++)
			{
				Mover* pMover = (Mover*)pTeam->getMover( i );
				if ( pMover->isSelected()  && pMover->getCommander()->getId() == Commander::home->getId() )
				{
					pMover->drawWaypointPath();
				}
			}
			
		}

		if ( bDrawHotKeys )
		{
			drawHotKeys();
			return;
		}

		if (turn > 3 /*&& !controlGui.inRegion(userInput->getMouseX(), userInput->getMouseY())*/ )
		{
#ifdef DRAW_CURSOR_CROSSHAIRS
			Stuff::Vector4D cursorPos;
			// [PROJECTZ:DebugOnly id=debug_cursor_crosshair]
			eye->projectForDebugOverlay(wPos,cursorPos);

			DWORD color = SB_WHITE;
			
			gos_SetRenderState( gos_State_Texture, 0 );

			gos_SetRenderState( gos_State_Filter, gos_FilterNone );

			Stuff::Vector4D				ul, br, pos1, pos2;

			pos1.x 		= cursorPos.x-5;
			pos1.y 		= cursorPos.y;
			pos1.z		= 0.000001f;
			
			pos2.x 		= cursorPos.x+5;
			pos2.y 		= cursorPos.y;
			pos2.z		= 0.000001f;
			pos2.w = pos1.w = 1.0f;

			{
				LineElement newElement(pos1,pos2,color,NULL,-1);
				newElement.draw();
			}

 			pos1.x 		= cursorPos.x;
			pos1.y 		= cursorPos.y-5;
			pos1.z		= 0.000001f;
			
			pos2.x 		= cursorPos.x;
			pos2.y 		= cursorPos.y+5;
			pos2.z		= 0.000001f;
			pos2.w = pos1.w = 1.0f;

			{
				LineElement newElement(pos1,pos2,color,NULL,-1);
				newElement.draw();
			}
			
#endif
			if (eye->usePerspective)
			{
				//Depth cue the cursor.
				Stuff::Vector3D Distance;
				Stuff::Point3D objPosition;
				Stuff::Point3D eyePosition(eye->getCameraOrigin());
				objPosition.x = -wPos.x;
				objPosition.y = wPos.z;
				objPosition.z = wPos.y;
		
				Distance.Subtract(objPosition,eyePosition);
				float eyeDistance = Distance.GetApproximateLength();
				float scaleFactor = 0.0f;
				scaleFactor = eyeDistance / (Camera::MaxClipDistance * 2.0f);
				scaleFactor = 1.5f - scaleFactor;

				userInput->setMouseScale(scaleFactor);			
 			}
		}
	}

	gos_SetRenderState( gos_State_IsHUD, 1 );
	controlGui.render( isPaused() && !isPausedWithoutMenu() );
	gos_SetRenderState( gos_State_IsHUD, 0 );

  /* 	if ( scenarioTime  < 7.0 )
   	{
   		long color = 0xff000000;
   		if ( (prefs.resolution == 0 && Environment.screenWidth == 640)
   		  || (prefs.resolution == 1 && Environment.screenWidth == 800)
   		  || (prefs.resolution == 2 && Environment.screenWidth == 1024)
   		  || (prefs.resolution == 3 && Environment.screenWidth == 1280)
   		  ||(prefs.resolution == 4 && Environment.screenWidth == 1600))
   		{
   			color = interpolateColor( 0xff000000, 0x00000000, (scenarioTime-swapTime)/(7.0-swapTime) );
   		}
   		else
   			swapTime = scenarioTime;
   		
   		GUI_RECT tmpRect = { 0,0, Environment.screenWidth, Environment.screenHeight };
   		drawRect( tmpRect, color );

	}*/
}	

void MissionInterfaceManager::initTacMap( PacketFile* file, int packet )
{
	file->seekPacket(packet);
	int size = file->getPacketSize( );
	BYTE* mem = new BYTE[size];

	// readPacket can return short on a transient _read failure; consuming the
	// uninitialized buffer then walks a garbage buildingCount off the heap
	// (one-off crash 2026-06-11, GameTacMap::initBuildings READ AV).
	if ( file->readPacket( packet, mem ) != size )
	{
		printf("[TACMAP] readPacket(%d) short read (expected %d) -- zeroing buffer\n", packet, size);
		memset( mem, 0, size );
	}

	controlGui.initTacMapBuildings( mem, size );
	delete[] mem;

	file->seekPacket(packet + 1);
	size = file->getPacketSize( );
	mem = new BYTE[size];

	if ( file->readPacket( packet + 1, mem ) != size )
	{
		printf("[TACMAP] readPacket(%d) short read (expected %d) -- zeroing buffer\n", packet + 1, size);
		memset( mem, 0, size );
	}

	controlGui.initTacMap( mem, size );
	delete[] mem;

	swapTime = 0.f;

	for ( int i = 0; i < MAX_ICONS; i++ )
	{
		oldTargets[i] = NULL;
	}

	target = NULL;
}

void MissionInterfaceManager::printDebugInfo()
{
	if (userInput->isLeftClick() || userInput->isRightClick()) {
		int row, col;
		land->worldToCell(wPos, row, col);
		char debugString[256];
		// Same NULL guard as the prior session's fix at ~line 776 — GlobalMoveMap[1]
		// is NULL when GlobalMap::init bailed out with badLoad on mod content.
		int ga0 = GlobalMoveMap[0] ? GlobalMoveMap[0]->calcArea(row, col) : -1;
		int ga1 = GlobalMoveMap[1] ? GlobalMoveMap[1]->calcArea(row, col) : -1;
		if (target)
			sprintf(debugString, "POS = %s(%c) %d,%d [%d, %d] (%.4f, %.4f, %.4f) OBJ = %s\n", terrainStr[GameMap->getTerrain(row, col)], GameMap->getPassable(row, col) ? 'T' : 'F', ga0, ga1, row, col, wPos.x, wPos.y, wPos.z, target->getName());
		else
			sprintf(debugString, "POS = %s(%c) %d,%d [%d, %d] (%.4f, %.4f, %.4f)\n", terrainStr[GameMap->getTerrain(row, col)], GameMap->getPassable(row, col) ? 'T' : 'F', ga0, ga1, row, col, wPos.x, wPos.y, wPos.z);
		DEBUGWINS_print(debugString);
	}

}

void MissionInterfaceManager::doMove(const Stuff::Vector3D& pos)
{
	/*if ( controlGui.getGuardTower()  )
	{
		doGuardTower();
		return;
	}*/
	if ( controlGui.isAddingAirstrike() &&  controlGui.isButtonPressed( ControlGui::SENSOR_PROBE ))
	{
		// tmp hack
		Stuff::Vector3D tmp = wPos;
		wPos = pos;
		addAirstrike( );
		wPos = tmp;

		return;
	}
	
	LocationNode path;
	path.location = pos;
	path.run = true;
	path.next = NULL;

	TacticalOrder tacOrder;
	if ( controlGui.getJump() )
	{
		doJump();
		return;
	}
	else
		tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_MOVETO_POINT, false);
	tacOrder.initWayPath(&path);
	tacOrder.moveParams.wait = false;
	tacOrder.moveParams.wayPath.mode[0] =  controlGui.getWalk() ? TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
	if ( controlGui.getMines() )
		tacOrder.moveParams.mode = MOVE_MODE_MINELAYING;
	tacOrder.pack(NULL, NULL);


	handleOrders(tacOrder);
	
	soundSystem->playDigitalSample(BUTTON5);

	controlGui.setDefaultSpeed();
}

bool MissionInterfaceManager::makePatrolPath()
{
	Team* pTeam = Team::home;
	for (long i=0;i<pTeam->getRosterSize();i++)
	{
		Mover* pMover = (Mover*)pTeam->getMover( i );
		if ( pMover->isSelected() && pMover->getPilot()->getNumTacOrdersQueued() && pMover->getCommander()->getId() == Commander::home->getId() )
		{
			if ( pMover->isCloseToFirstTacOrder( wPos ) )
			{
				userInput->setMouseCursor( mState_LINK );
				if ( commandClicked )
				{
					pMover->getPilot()->setTacOrderQueueLooping( true );
				}

				return 1;
			}
			else 
			{
				Stuff::Vector3D tmp;
				tmp.Subtract( wPos, pMover->getPosition() );
				if ( tmp.GetLength() < 25 ) // random amount
				{
					userInput->setMouseCursor( mState_LINK );
					if ( commandClicked )
					{
						doMove(wPos);
						pMover->getPilot()->setTacOrderQueueLooping( true );
					}
					return 1;
				}
			}
			
		}
	}

	return 0;
}

int MissionInterfaceManager::forceShot()
{
	bForcedShot = true;

	userInput->setMouseCursor( mState_GENERIC_ATTACK );
	if ( commandClicked )
	{
			TacticalOrder tacOrder;
			tacOrder.init(ORDER_ORIGIN_PLAYER,  target ? TACTICAL_ORDER_ATTACK_OBJECT : TACTICAL_ORDER_ATTACK_POINT);
			tacOrder.targetWID = target ? target->getWatchID() : 0;
			if ( !target )
				tacOrder.attackParams.targetPoint = wPos;
			tacOrder.attackParams.type = bEnergyWeapons ? ATTACK_CONSERVING_AMMO : ATTACK_TO_DESTROY;
			tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
			tacOrder.attackParams.range = (FireRangeType)FIRERANGE_OPTIMAL;
			tacOrder.attackParams.pursue = controlGui.getFireFromCurrentPos() ? false : true;
			tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
			
			soundSystem->playDigitalSample(BUTTON5);

			if (target)
				target->getAppearance()->flashBuilding(1.3f,0.2f,0xffff0000);

			handleOrders( tacOrder );
			return 1;
	}

	return 0;

	

}

void MissionInterfaceManager::selectForceGroup( int forceGroup, bool deselect )
{
	Team* pTeam = Team::home;
	bool bAllSelected = deselect ? false : true;
	if ( deselect )
	{

		for (long i=0;i<pTeam->getRosterSize();i++)
		{
			Mover* pMover = (Mover*)pTeam->getMover( i );
			if (pMover && pMover->isOnGUI() && (pMover->getCommander()->getId() == Commander::home->getId()))
			{
				pMover->setSelected( 0 );
			}
		}					
	}
	else // see if the whole force group is already seleced
	{
		for (long i=0;i<pTeam->getRosterSize();i++)
		{
			Mover* pMover = (Mover*)pTeam->getMover( i );
			if (pMover && pMover->isInUnitGroup( forceGroup ) && !pMover->isDisabled() && !pMover->isSelected() && pMover->isOnGUI() && (pMover->getCommander()->getId() == Commander::home->getId()))
			{
				bAllSelected = false;
			}
		}
	}

	for (long i=0;i<pTeam->getRosterSize();i++)
	{
		Mover* pMover = (Mover*)pTeam->getMover( i );
		if (pMover && pMover->isInUnitGroup( forceGroup ) && !pMover->isDisabled() && pMover->isOnGUI() && (pMover->getCommander()->getId() == Commander::home->getId()))
			pMover->setSelected( !bAllSelected );
	}
}
void MissionInterfaceManager::makeForceGroup( int forceGroup )
{
	Team* pTeam = Team::home;

	for (int i=0;i<pTeam->getRosterSize();i++)
	{
		Mover* pMover = (Mover*)pTeam->getMover( i );
		if (pMover && (pMover->getCommander()->getId() == Commander::home->getId()))
		{
			if ( pMover->isInUnitGroup( forceGroup ) )
				pMover->removeFromUnitGroup( forceGroup );
		}
	}					

	for (int i=0;i<pTeam->getRosterSize();i++)
	{
		Mover* pMover = (Mover*)pTeam->getMover( i );
		if (pMover && (pMover->getCommander()->getId() == Commander::home->getId()))
		{
			if (pMover->isSelected() )
				pMover->addToUnitGroup( forceGroup );
		}
	}					
}

bool MissionInterfaceManager::moveCameraAround( bool lineOfSight, bool passable, bool ctrl, bool bGui, 
											   long moverCount, long nonMoverCount )
{
	bool bRetVal = 0;
	bool middleClicked = (!userInput->isLeftDrag() && !userInput->isRightDrag() && userInput->isMiddleClick());

	if ( useLeftRightMouseProfile && ((userInput->isLeftClick() && userInput->getKeyDown(KEY_T)) || userInput->isLeftDoubleClick()) && target )
	{
		if (eye)
			((GameCamera *)eye)->setTarget(target);
	}

	// Stage 2.E: when MC2_VISUAL_DIFF_CAPTURE=1, skip edge-scroll entirely.
	// The OS cursor sits wherever the user left it (often a screen corner),
	// which would drive the camera differently each run and torpedo
	// same-config visual-diff determinism.
	if (!cameraClicked && !isPaused() && !VisualDiff::isCaptureEnabled()) // sebi: only scoll if not paused
	{
		if ( mouseX <= (screenScrollLeft))
			scrollLeft();

		if ((mouseX >= (Environment.screenWidth - screenScrollRight)) )
			scrollRight();

		if ( (mouseY <= (screenScrollUp) && mouseY >= -screenScrollUp ) )
			scrollUp();

		if ( (mouseY >= (Environment.screenHeight - screenScrollDown) ) )
			scrollDown();
	}

	if (attilaXAxis < -ATTILA_THRESHOLD)
	{
		float oldScrollInc = scrollInc;
		scrollInc *= fabs(attilaXAxis) * ATTILA_FACTOR;
		scrollLeft();
		scrollInc = oldScrollInc;
	}
	
	if (attilaXAxis > ATTILA_THRESHOLD)
	{
		float oldScrollInc = scrollInc;
		scrollInc *= fabs(attilaXAxis) * ATTILA_FACTOR;
		scrollRight();
		scrollInc = oldScrollInc;
	}
	
	if (attilaYAxis < -ATTILA_THRESHOLD)
	{
		float oldScrollInc = scrollInc;
		scrollInc *= fabs(attilaYAxis) * ATTILA_FACTOR;
		scrollUp();
		scrollInc = oldScrollInc;
	}
	
	if (attilaYAxis > ATTILA_THRESHOLD)
	{
		float oldScrollInc = scrollInc;
		scrollInc *= fabs(attilaYAxis) * ATTILA_FACTOR;
		scrollDown();
		scrollInc = oldScrollInc;
	}
 	
	if (attilaRZAxis < -ATTILA_THRESHOLD)
	{
		eye->rotateLeft(rotationInc * fabs(attilaRZAxis) * frameLength * ATTILA_ROTATION_FACTOR);
	}
	
	if (attilaRZAxis > ATTILA_THRESHOLD)
	{
		eye->rotateRight(rotationInc * fabs(attilaRZAxis) * frameLength * ATTILA_ROTATION_FACTOR);
	}
	
 	//------------------------------------------------
	// Right drag is camera rotation and tilt now.
	if (cameraClicked)
	{
		bRetVal = 1;
		int mouseXDelta = userInput->getMouseXDelta();
		float actualRot = rotationInc * 0.1f * abs(mouseXDelta);
		if (mouseXDelta > 0)
		{
			eye->rotateRight(actualRot);
			bRetVal = 1;	
		}
		else if (mouseXDelta < 0)
		{
			eye->rotateLeft(actualRot);
			bRetVal = 1;	
		}
		
		int mouseYDelta = userInput->getMouseYDelta();
		float actualTilt = rotationInc * 0.1f * abs(mouseYDelta);
		if (mouseYDelta > 0)
		{
			eye->tiltDown(actualTilt);
			bRetVal = 1;
	
		}
		else if (mouseYDelta < 0)
		{
			eye->tiltUp(actualTilt);
			bRetVal = 1;
	
		}
		// Manual right-drag rotate/tilt while overview is active = the user took
		// over the camera; suppress the auto-return on exit.
		if (mouseXDelta || mouseYDelta)
			g_tacticalOverview.notifyUserPan();
		userInput->setMouseCursor( mState_ROTATE_CAMERA );

		//--------------------------------------------------
		// Zoom Camera based on Mouse Wheel input.
		long mouseWheelDelta = userInput->getMouseWheelDelta();
		if (mouseWheelDelta)
		{
			g_tacticalOverview.onWheel(-mouseWheelDelta,
			                           /*atCeiling=*/(g_tacticalOverview.setpoint() > 0.0f) || (eye->getCameraAltitude() >= 5900.0f),
			                           frameLength, /*worldOwnsWheel=*/true);
			//Mouse wheel just picks zooms now.
			//float actualZoom = zoomInc * abs(mouseWheelDelta) * 0.0001f * eye->getScaleFactor();
			// [MOUSE-ANCHORED-ZOOM-1 / FIX-4] zoom toward cursor when gated on.
			if (mouseWheelDelta > 0)
			{
				anchoredZoom(userInput->getMouseX(), userInput->getMouseY(), &MissionInterfaceManager::zoomChoiceOut);
			}
			else
			{
				anchoredZoom(userInput->getMouseX(), userInput->getMouseY(), &MissionInterfaceManager::zoomChoiceIn);
			}

			if ( target )
				userInput->setMouseCursor( makeTargetCursor( lineOfSight, moverCount, nonMoverCount ) );
			else
				userInput->setMouseCursor( makeNoTargetCursor( passable, lineOfSight, ctrl, bGui, moverCount, nonMoverCount ) );
			bRetVal = 1;
		}

		return bRetVal;
	}
	
	//--------------------------------------------------
	// Zoom Camera based on Mouse Wheel input.
	long mouseWheelDelta = userInput->getMouseWheelDelta();
	if (mouseWheelDelta)
	{
		g_tacticalOverview.onWheel(-mouseWheelDelta,
		                           /*atCeiling=*/(g_tacticalOverview.setpoint() > 0.0f) || (eye->getCameraAltitude() >= 5900.0f),
		                           frameLength, /*worldOwnsWheel=*/true);
		//Mouse wheel just picks zooms now.
		//float actualZoom = zoomInc * abs(mouseWheelDelta) * 0.0001f * eye->getScaleFactor();
		// [MOUSE-ANCHORED-ZOOM-1 / FIX-4] zoom toward cursor when gated on.
		if (mouseWheelDelta > 0)
		{
			anchoredZoom(userInput->getMouseX(), userInput->getMouseY(), &MissionInterfaceManager::zoomChoiceOut);
		}
		else
		{
			anchoredZoom(userInput->getMouseX(), userInput->getMouseY(), &MissionInterfaceManager::zoomChoiceIn);
		}

		if ( target )
			userInput->setMouseCursor( makeTargetCursor( lineOfSight, moverCount, nonMoverCount ) );
		else
			userInput->setMouseCursor( makeNoTargetCursor( passable, lineOfSight, ctrl, bGui, moverCount, nonMoverCount ) );
		bRetVal = 1;
	}

	//-----------------------------------------------------------------
	// If middle mouse button is pressed, go to normal tilt, 50% zoom	
	if (middleClicked)
	{
		cameraNormal();
	}				
	
	doDrag(bGui);

	return bRetVal;

}

bool MissionInterfaceManager::canJump()
{
	bool canJump = true;
	long i=0;

	Team* pTeam = Team::home;
	
	while ((i<pTeam->getRosterSize()))
	{
		Mover* pMover = (Mover*)pTeam->getMover( i );
		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
		{
			if ( !pMover->canJump() )
			{
				canJump = false;
				break;
			}
		}
		i++;
	}

	return canJump;

}

bool MissionInterfaceManager::canJumpToWPos()
{
	bool passable = 0;
	if ( Terrain::IsGameSelectTerrainPosition( wPos ) )
	{
		int cellR, cellC;
		land->worldToCell(wPos, cellR, cellC);
		passable = GameMap->getPassable(cellR,cellC);
	}
	
	if (passable)
	{
		bool canJump = true;
		long i=0;

		Team* pTeam = Team::home;
		
		while ((i<pTeam->getRosterSize()))
		{
			Mover* pMover = (Mover*)pTeam->getMover( i );
			if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
			{
				if ( pMover->canJump() )
				{
					TacticalOrder lastQueuedTacOrder;
					long err = pMover->getPilot()->peekQueuedTacOrder(-1, &lastQueuedTacOrder);
					Stuff::Vector3D lastWayPoint;
					if (err)
						lastWayPoint = pMover->getPosition();
					else
						lastWayPoint = lastQueuedTacOrder.getWayPoint(0);
					Stuff::Vector3D distance;
					distance.Subtract(lastWayPoint,wPos);

					//JUMP distance is always JUST horizontal.  NO Vertical!!
					distance.z = 0.0f;
					float dist = distance.GetLength();
					
					if (dist > pMover->getJumpRange())
						canJump = false;
				}
				else
					canJump = false;
			}
			i++;
		}

		return canJump;
	}

	return 0;
	
}

void MissionInterfaceManager::doDrag(bool bGui)
{
	// Formation line draw mode owns the left-drag gesture: no select box and
	// no drag-select while armed or drawing.
	if ( TacticalOverview::formationLineEnabled()
		&& g_tacticalOverview.flState() != TacticalOverview::FL_IDLE )
	{
		dragStart.Zero();
		dragEnd.Zero();
		isDragging = FALSE;
		return;
	}

	//---------------------------------------------------------------------------
	// Check if we wanted to select all visible.  If so, do it!
//	if ( ((GetAsyncKeyState( VK_LBUTTON ))) && !bGui && !cameraClicked )
	if ( ((gos_GetKeyStatus( KEY_LMOUSE ) == KEY_PRESSED)) && !bGui && !cameraClicked
		&& !dragStart.x)
	{
	
 			dragStart.x = wPos.x;
			dragStart.y = wPos.y;
			dragStart.z = wPos.z;
			isDragging = 0;			
	}
	
	else if ((gos_GetKeyStatus( KEY_LMOUSE ) == KEY_HELD)
		&& !isDragging && ( !(dragStart.x == 0.f && dragStart.y == 0.f )
		&& (dragStart.x != wPos.x && dragStart.y != wPos.y ) ) )
	{
		isDragging = TRUE;
		dragEnd.x = userInput->getRawMouseX();
		dragEnd.y = userInput->getRawMouseY();
	}

	else if (isDragging)
	{
		dragEnd.x = userInput->getRawMouseX();
		dragEnd.y = userInput->getRawMouseY();
	
		if ( (gos_GetKeyStatus( KEY_LMOUSE ) == KEY_RELEASED
			|| gos_GetKeyStatus( KEY_LMOUSE ) == KEY_FREE) && isDragging)
		{
			//----------------------------------------------
			// We should select everything in the drag box.

		
			// AND deselect anything outside of it.

			GameObject*	objsInRect[MAX_ICONS];
			int count = 0;
			memset( objsInRect, 0, sizeof( GameObject* ) * 16 );
			for (long i=0;i<ObjectManager->getNumMovers();i++)
			{
				Stuff::Vector3D screenStart;
				Stuff::Vector4D screenPos;
				// [PROJECTZ:SelectionPicking id=picking_drag_select_origin]
				PROJECTZ_SITE("picking_drag_select_origin", "SelectionPicking");
				eye->projectForSelectionPicking( dragStart, screenPos );
				screenStart.x = screenPos.x;
				screenStart.y = screenPos.y;
			   GameObjectPtr	mover = ObjectManager->getMover(i);
			   if (ObjectManager->moverInRect(i,screenStart,dragEnd) && 
				   mover->getCommanderId() == Commander::home->getId() && 
				   count < MAX_ICONS
				   && !mover->isDisabled())
				{
				   objsInRect[count] = mover;
				   count++;
				}
			}

			if ( count )
			{
				// empty the info window of any previously selected guys
				controlGui.setInfoWndMover( NULL );

				if ( !userInput->shift() )
				{
					for (int i=0;i<ObjectManager->getNumMovers();i++)
					{
						ObjectManager->getMover(i)->setSelected( 0 );
					}
				}

				for (int i = 0; i < count; i ++ )
				{
					objsInRect[i]->setSelected( true );
				}

				if ( count == 1 )
					(controlGui).setInfoWndMover( (Mover*)objsInRect[0] );
			}
			
			dragStart.Zero();
			dragEnd.Zero();		
			isDragging = FALSE;
		}
	}
	else if ( gos_GetKeyStatus(KEY_LMOUSE) == KEY_RELEASED )
	{
		dragStart.Zero();
	}
}

bool MissionInterfaceManager::canAddVehicle( const Stuff::Vector3D& pos )
{
	//We're probably going to use this alot!
	long commanderID = Commander::home->getId();

 	if ( paintingVtol[commanderID] )
		return 0;
	int tileI, tileJ;
	land->worldToTile( pos, tileJ, tileI );

	if ( tileJ > -1 && tileJ < land->realVerticesMapSide 
		 && tileI > -1 && tileI < land->realVerticesMapSide )
	{
		if ( Team::home->teamLineOfSight(pos, 0.0f) )
		{
			if ( GameMap->getPassable( pos )
				&& (!TerrainRuntime::sampleWaterClass(pos) ))
				return true;
			else 
				return false;
		}	
	}
	

	return false;
}

bool MissionInterfaceManager::canRecover( const Stuff::Vector3D& pos )
{
	//We're probably going to use this alot!
	long commanderID = Commander::home->getId();

 	if ( paintingVtol[commanderID] )
		return 0;
		
	int tileI, tileJ;
	land->worldToTile( pos, tileJ, tileI );

	if ( tileJ > -1 && tileJ < land->realVerticesMapSide 
		 && tileI > -1 && tileI < land->realVerticesMapSide )
	{
		if ( Team::home->teamLineOfSight(pos, 0.0f) )
		{
			if ( target && (target->getObjectClass() == BATTLEMECH) && target->isDisabled() && !target->isDestroyed() )
				return true;
			else 
				return false;
		}	
	}
	

	return false;
}

long MissionInterfaceManager::makeTargetCursor( bool lineOfSight, long moverCount, long nonMoverCount)
{
	//We're probably going to use this alot!
	long commanderID = Commander::home->getId();

 	long currentCursor = makeRangeCursor( lineOfSight );
	if ( !moverCount && !nonMoverCount )
		currentCursor = mState_NORMAL;

	if ( ( userInput->getKeyDown( WAYPOINT_KEY ) ) )
	{
		currentCursor = mState_DONT;
		return currentCursor;
	}

	
	if ( controlGui.isSelectingInfoObject()  )
	{
		if ( target->isMover() )
		{
			currentCursor = mState_INFO;
		}
		else 
			currentCursor = mState_DONT;
		
		return currentCursor;
	}
	
	else if ( controlGui.isAddingVehicle() && !paintingVtol[commanderID] )
	{
		currentCursor = mState_DONT;
		return currentCursor;
	}

	else if ( controlGui.isAddingSalvage() && !paintingVtol[commanderID] )
	{
		if ( canSalvage(target) )
		{
			currentCursor = mState_SALVAGE;
			target->setTargeted(true);
		}
		else
			currentCursor = mState_XSALVAGE;

		return currentCursor;
	}
	else if ( controlGui.isAddingAirstrike() && !paintingVtol[commanderID] )
	{
		if ( controlGui.getButton( ControlGui::SENSOR_PROBE )->state == ControlButton::PRESSED )
		{
			currentCursor = lineOfSight ? mState_SENSORSTRIKE : mState_UNCERTAIN_AIRSTRIKE ;
		}
		else
		{
			currentCursor = lineOfSight ? mState_AIRSTRIKE : mState_UNCERTAIN_AIRSTRIKE ;
		}
		return currentCursor;
	}
	else if ( controlGui.getSalvage()  )
	{
		if ( canSalvage( target ) )
		{
			currentCursor = mState_SALVAGE;
			target->setTargeted(true);
		}
		else
			currentCursor = mState_XSALVAGE;
		
		return currentCursor;
	}
	else if ( controlGui.getRepair() )
	{
		if ( canRepair( target ) )
			currentCursor = mState_REPAIR;
		else
			currentCursor = mState_XREPAIR;

		if (target)
			target->setTargeted(true);

		return currentCursor;
	}
	else if ( controlGui.getMines() )
	{
		currentCursor = mState_DONT;
		return currentCursor;
	}
	else if ( controlGui.getGuardTower() )
	{
		currentCursor = lineOfSight ? mState_GUARDTOWER_LOS : mState_GUARDTOWER;
		return currentCursor;
	}
	else if ( target->isCaptureable(Team::home->getId() ) && moverCount )
	{
		for ( int i = 0; i < Team::home->getRosterSize(); i++ )
		{
			Mover* pMvr = Team::home->getMover( i );
			if ( pMvr->isSelected() && pMvr->getCommander()->getId() == Commander::home->getId() )
			{
				if ((pMvr->getObjectClass() == BATTLEMECH) &&
					(pMvr->getMoveType() == MOVETYPE_GROUND))
				{
					if ( target->getCaptureBlocker( pMvr) )
					{
						currentCursor = mState_XCAPTURE;
						break;
					}
					else
					{
						currentCursor = mState_CAPTURE;
					}
				}
				else
				{
					currentCursor = mState_XCAPTURE;
				}
			}
		}
	}	

			
	switch (target->getObjectClass())
	{
		case BATTLEMECH:
		case GROUNDVEHICLE:
		case ELEMENTAL:
		{
			//---------------------------------------------------------------------------------------
			// if target is enemy battleMech, select it, change cursor and check below for leftClick
			if (target->getTeam() && !target->getTeam()->isFriendly(Team::home) && !target->getTeam()->isNeutral( Team::home ) )
			{
				target->setTargeted(true); 
				if ( target->isDisabled() )
				{
					currentCursor = mState_NORMAL;
	//				target = NULL;
				}
			}
			else
			{
				//-------------------------------------------------------------------
				// Otherwise we over a friendly.  Assume we intend to select them if
				// we click.  If we shift-click, just add them.
				target->setTargeted(true);		//For friendlies, let us know we need to draw their names!

				currentCursor = mState_NORMAL;
				controlGui.setRolloverHelpText( IDS_UNIT_SELECT_HELP );
			}
		}
		break;
			
		case BUILDING:
		case TREEBUILDING:			
		case TURRET:
		case GATE:
		case TREE:
		case TERRAINOBJECT:
		{
			if ( target->getObjectType()->getSubType() == BUILDING_SUBTYPE_LANDBRIDGE )
			{
				//Check if forcing fire or fire from current.  If so, shoot away.
				if ( gos_GetKeyStatus( (gosEnum_KeyIndex)commands[FORCE_FIRE_KEY].key ) == KEY_HELD || 
					 controlGui.getFireFromCurrentPos() )
				{
					target->setTargeted(true);
				}
				else
				{
					target = NULL;
					// find out if this position is passable.  Landbridges kinda are, or they'd be useless.
					int cellR, cellC;
					bool passable = 1;
					bool lineOfSight = 0;
					if ( Terrain::IsGameSelectTerrainPosition( wPos ) )
					{
						land->worldToCell(wPos, cellR, cellC);
						passable = GameMap->getPassable(cellR, cellC);
					}
	
					return makeNoTargetCursor( passable, lineOfSight, 0, 0, moverCount, nonMoverCount );
				}
			}
			//------------------------------------------------------------------
			// No matter what side this is on, change the cursor and select.
			target->setTargeted(true);
			TeamPtr targetTeam = target->getTeam();

			if ( targetTeam && Team::home->isFriendly( targetTeam ) && 
				target->getFlag(OBJECT_FLAG_CANREFIT) && target->getFlag(OBJECT_FLAG_MECHBAY) )  
			{
				if (canRepairBay(target))
				{
					currentCursor = mState_REPAIR;
				}
				else
				{
					currentCursor = mState_XREPAIR;
				}
			}
			else if ( targetTeam && !controlGui.getGuard() &&
				Team::home->isFriendly( targetTeam ) && 				
				gos_GetKeyStatus( (gosEnum_KeyIndex)commands[FORCE_FIRE_KEY].key ) != KEY_HELD // not forcing fire
				&& controlGui.getCurrentRange() == FIRERANGE_OPTIMAL ) // range attacks now are force fire
			{
				return makeNoTargetCursor( false, lineOfSight, 0, 0, moverCount, nonMoverCount );
			}
		}
		break;

		case BRIDGE:
			target = NULL;
			return makeNoTargetCursor( false, lineOfSight, 0, 0, moverCount, nonMoverCount );
			break;

			
		case DEBRIS:
		{
			//----------------------------------
			// Do nothing for Debris right now.
		}
		break;			
	}
	
	if ( controlGui.getGuard() )
	{
		currentCursor = mState_GUARD;
		if (target)
			target->setTargeted(true);
	}
	else if ( gos_GetKeyStatus( (gosEnum_KeyIndex)commands[FORCE_FIRE_KEY].key ) == KEY_HELD
		|| ( controlGui.getCurrentRange() != FIRERANGE_OPTIMAL && controlGui.getCurrentRange() != FIRERANGE_CURRENT ) )
	{
		currentCursor = makeRangeCursor( lineOfSight );
	}



	return currentCursor;
}

long MissionInterfaceManager::makeRangeCursor( bool lineOfSight)
{
	long currentCursor = mState_NORMAL;
	long currentRange = controlGui.getCurrentRange();

	if ( bEnergyWeapons )
		return lineOfSight ? mState_ENERGY_WEAPONS_LOS : mState_ENERGY_WEAPONS;
	
	switch( currentRange )
	{
	case FIRERANGE_SHORT:
		currentCursor = lineOfSight ? mState_SHRTRNG_LOS :  mState_SHRTRNG_ATTACK;
		break;
	case FIRERANGE_MEDIUM:
		currentCursor = lineOfSight ? mState_MEDRNG_LOS :   mState_MEDRNG_ATTACK;
		break;
	case FIRERANGE_LONG:
		currentCursor =  lineOfSight ? mState_LONGRNG_LOS :  mState_LONGRNG_ATTACK;
		break;
	case FIRERANGE_OPTIMAL:
		currentCursor =  lineOfSight ? mState_ATTACK_LOS :  mState_GENERIC_ATTACK;
		break;

	case FIRERANGE_CURRENT:
		currentCursor = lineOfSight ? mState_CURPOS_ATTACK_LOS : mState_CURPOS_ATTACK;
		break;
	}

	if ( controlGui.getFireFromCurrentPos() )
		currentCursor = lineOfSight ? mState_CURPOS_ATTACK_LOS : mState_CURPOS_ATTACK;

	return currentCursor;
}

long MissionInterfaceManager::makeNoTargetCursor( bool passable, bool lineOfSight, bool bCtrl, bool bGui,
												 long moverCount, long nonMoverCount )
{
	//-------------------------------------------------------------------------
	// We are not over enything.  Switch to move cursor based on input.
	// If we are on passable terrain, move.
	// If passable and runCommand, run.
	// If passable and jumpCommand and canJump, jump.
	// If not passable, dont.
	// If passable and jumpCommand and cantJump, dont.

	//We're probably going to use this alot!
	long commanderID = Commander::home->getId();
 	int cursorType = makeMoveCursor( lineOfSight );
	if ( !moverCount )
		cursorType = mState_NORMAL;

	if ( gos_GetKeyStatus( (gosEnum_KeyIndex)commands[FORCE_FIRE_KEY].key ) == KEY_HELD // not forcing fire
				|| controlGui.getFireFromCurrentPos() )
	{
		return makeRangeCursor( lineOfSight );
	}
	

	if ( !passable && !selectionIsHelicopters() )
		cursorType = mState_DONT;
		
	if ( controlGui.isSelectingInfoObject() )
	{
		cursorType = mState_DONT;
		return cursorType;
	}
	else if ( controlGui.isAddingAirstrike() && !paintingVtol[commanderID] )
	{
		if ( bGui && (!controlGui.isOverTacMap() || !controlGui.isButtonPressed( ControlGui::SENSOR_PROBE)) )
			cursorType = mState_NORMAL;
		else
		{
			if ( controlGui.getButton( ControlGui::SENSOR_PROBE )->state == ControlButton::PRESSED )
			{
				cursorType = lineOfSight ? mState_SENSORSTRIKE : mState_UNCERTAIN_AIRSTRIKE ;
			}
			else
				cursorType = lineOfSight ? mState_AIRSTRIKE : mState_UNCERTAIN_AIRSTRIKE;
		}
		return cursorType;
	}
	else if ( controlGui.isAddingVehicle() )
	{
		if ( canAddVehicle( wPos ) && !bGui)
			cursorType = mState_VEHICLE;
		else if ( !paintingVtol[commanderID] && ! bGui)
			cursorType = mState_CANTVEHICLE;
		else if ( bGui )
			cursorType = mState_NORMAL;
	}
	else if ( controlGui.isAddingSalvage())
	{
		if ( canRecover( wPos ) && !bGui )
			cursorType = mState_SALVAGE;
		else if ( !paintingVtol[commanderID] & !bGui) 
			cursorType = mState_XSALVAGE;
		else if ( bGui )
			cursorType = mState_NORMAL;

	}
	else if ( userInput->getKeyDown( WAYPOINT_KEY ) || controlGui.getMines() )
	{
		if ( controlGui.getMines() )
		{
			if ( !passable )
				cursorType = mState_XMINES;
			else
				cursorType = mState_LAYMINES;
		}

		else
		{
			cursorType  = lineOfSight ? mState_WALKWAYPT_LOS : mState_WALKWAYPT;
			if ( controlGui.getRun() )
				cursorType  = lineOfSight ? mState_RUNWAYPT_LOS : mState_RUNWAYPT;
			else if ( controlGui.getJump() )
				cursorType  = lineOfSight ? mState_JUMPWAYPT_LOS: mState_JUMPWAYPT;
		}

		return cursorType;
		

	}

	else if ( controlGui.getJump() )
	{
		if ( canJumpToWPos() )
		{	
			if ( bCtrl )
				cursorType = lineOfSight ? mState_JUMPWAYPT_LOS : mState_JUMPWAYPT;
			else
				cursorType = makeJumpCursor( lineOfSight );

			controlGui.setRolloverHelpText( IDS_UNIT_HELP );
		}
		else
			cursorType = mState_DONT;
			
		if ( !passable && !selectionIsHelicopters() )
			cursorType = mState_DONT;


	}
	else if ( controlGui.getRun() && moverCount )
	{
		if ( bCtrl )
			cursorType = lineOfSight ? mState_RUNWAYPT_LOS : mState_RUNWAYPT;
		else
			cursorType = makeRunCursor( lineOfSight );
			
		if ( !passable && !selectionIsHelicopters() )
			cursorType = mState_DONT;
		else
			controlGui.setRolloverHelpText( IDS_UNIT_HELP );
	}
	else if ( controlGui.getMines() )
	{
		cursorType = mState_LAYMINES;
		if ( !passable )
			cursorType = mState_XMINES;
	}
	else if ( controlGui.getSalvage() )
	{
		cursorType = makeMoveCursor( lineOfSight );
		if ( !passable )
			cursorType = mState_DONT;
	}
	else if ( controlGui.getRepair() )
	{
		cursorType = makeMoveCursor( lineOfSight );
		if ( !passable )
			cursorType = mState_DONT;
	}
	else if ( controlGui.getGuard() )
	{
		cursorType = lineOfSight ? mState_GUARD_LOS : mState_GUARD;
		if ( !passable )
			cursorType = mState_DONT;
	}
	else if ( controlGui.getGuardTower() )
	{
		cursorType = lineOfSight ? mState_GUARDTOWER_LOS : mState_GUARDTOWER;
	}
	else if ( moverCount )
	{
		controlGui.setRolloverHelpText( IDS_UNIT_HELP );
	}
	
	userInput->setMouseFrame(0);		

	return cursorType;
}

void MissionInterfaceManager::addAirstrike()
{
	if ( controlGui.isButtonPressed( ControlGui::LARGE_AIRSTRIKE ) )
		bigAirStrike();
	else if ( controlGui.isButtonPressed( ControlGui::LARGE_AIRSTRIKE ) )
		smlAirStrike();
	else if ( controlGui.isButtonPressed( ControlGui::SENSOR_PROBE ) )
		snsAirStrike();
	else
		gosASSERT( false );

	controlGui.unPressAllVehicleButtons();
}

//-----------------------------------------------------------------------------

MoverPtr BringInReinforcement (long vehicleID, long rosterIndex, long commanderID, Stuff::Vector3D pos, bool exists) {

	MoverInitData data;
	memset( &data, 0, sizeof( data ) );
	char* vehicleFile = (char*)MissionInterfaceManager::instance()->getSupportVehicleNameFromID(vehicleID);
	if (!vehicleFile) {
		char s[1024];
		sprintf(s, "BringInReinforcement: null behicleFile (%d)", vehicleID);
		STOP((s));
		//return(NULL);
	}
	strcpy( data.pilotFileName, vehicleFile );
	strcpy( data.brainFileName, "pbrain" );
	data.controlType = 2;
	data.controlDataType = 2;
	data.rosterIndex = rosterIndex;
	data.objNumber = vehicleID;
	data.position = pos;
	data.rotation = 0;
	data.teamID = Commander::commanders[commanderID]->team->getId();
	data.commanderID = commanderID;
	data.active = 1;
	data.exists = exists;
	data.capturable = 0;
	data.baseColor = prefs.baseColor;
	data.highlightColor1 = prefs.highlightColor;
	data.highlightColor2 = prefs.highlightColor;

	if ( MPlayer )
	{
		MC2Player* pInfo = MPlayer->getPlayerInfo( commanderID );
		if ( pInfo )
		{
			data.baseColor = MPlayer->colors[pInfo->baseColor[BASECOLOR_TEAM]];
			data.highlightColor1 = MPlayer->colors[pInfo->stripeColor];
			data.highlightColor2 = MPlayer->colors[pInfo->stripeColor];
		}
	}

	long moverId = mission->addMover( &data );
	
	if ((moverId != -1) && (moverId != 0))
	{
		//Start the helicopters landed!!
		MoverPtr newMover = (MoverPtr)ObjectManager->get(moverId);
		if (!newMover)
			return NULL;

		if (newMover->getMoveType() == MOVETYPE_AIR)
			newMover->startShutDown();

		if (!MPlayer)
			newMover->setLocalMoverId(0);

		//Somehow this is set during a quickLoad?
		newMover->disableThisFrame = false;

		return(newMover);
	}

	return NULL;
}

//-----------------------------------------------------------------------------

void MissionInterfaceManager::addVehicle( const Stuff::Vector3D& pos )
{
	
//	LogisticsPilot* pPilot = LogisticsData::instance->getFirstAvailablePilot();

//	const char* pChar = pPilot->getFileName();
//	if ( !pChar )
//		return;

	// Check if vehicleFile is NULL.  If it is, simply get it.
	// Can happen if we get here BEFORE beginVtol?
	// Happens when we select a vehicle which costs exactly the number of points we have left.
	// Heidi deletes the points, which shuts off the button, which causes getVehicleName to return NULL.
	// D'OH!!!
	// Delete the points in getVehicleName when the vehicle is actually added!!!
	//
	// -fs
	if (!vehicleFile)
		vehicleFile = controlGui.getVehicleName(vehicleID[Commander::home->getId()]);

	if ( !vehicleFile )
		return;

	reinforcement = BringInReinforcement(vehicleID[Commander::home->getId()], 255, Commander::home->getId(), pos, true);
}

void MissionInterfaceManager::beginVtol (long supportID, long commanderID, Stuff::Vector3D* reinforcePos, MoverPtr salvageTarget)
{
	Stuff::Vector3D vtolPos = wPos;
	if (reinforcePos)
		vtolPos = *reinforcePos;

	if (supportID == -1) {
		vehicleFile = controlGui.getVehicleName( vehicleID[commanderID] );
		}
	else 
	{
		//-----------------------
		// used in multiplayer...
		vehicleID[commanderID] = supportID;
		vehicleFile = controlGui.getVehicleNameFromID(supportID);
	}

	if (vehicleID[commanderID] == 0) {
		char s[1024];
		sprintf(s, "beginvtol: supportID = %d, commanderID = %d, vehicleID = %d, vehicleFile = %s", supportID, commanderID, vehicleID[commanderID], vehicleFile);
		PAUSE((s));
	}

	if ( !vTol[commanderID] )
	{
		if (vehicleID[commanderID] != 147)		//NOT a recovery vehicle.  Standard VTOL
		{
			if (supportID == -1) 
			{
				//Do not call this unless the commanderID passed in equals us or will you do something outside here?
				if (MPlayer && !reinforcePos) 
				{
					vPos[commanderID] = vtolPos;
					MPlayer->sendReinforcement(vehicleID[commanderID], 255, "noone", commanderID, vPos[commanderID], 0);
					return;
				}
			}
			
			AppearanceType* appearanceType = appearanceTypeList->getAppearance( BLDG_TYPE << 24, "vtol" );
			vTol[commanderID] = new BldgAppearance;
			vTol[commanderID]->init( appearanceType );
			
			if (!dustCloud[commanderID] && prefs.useNonWeaponEffects)
			{
				if (strcmp(weaponEffects->GetEffectName(VTOL_DUST_CLOUD),"NONE") != 0)
				{
					//--------------------------------------------
					// Yes, load it on up.
					unsigned flags = 0;
		
					Check_Object(gosFX::EffectLibrary::Instance);
					gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(VTOL_DUST_CLOUD));
					
					if (gosEffectSpec)
					{
						dustCloud[commanderID] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
						gosASSERT(dustCloud[commanderID] != NULL);
						
						MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
					}
				}
			}
		}
		else						//IS a recovery vehicle.  DO NOT create VTOL, use KARNOV instead.  NO Vehicle created either
		{
			if (supportID == -1) 
			{
				if (MPlayer && !reinforcePos) 	//Probably same as above here, too.
				{
					vPos[commanderID] = vtolPos;
					if (!salvageTarget)
						STOP(("MissionGUI.beginvtol: null target PASSED IN."));
					char* newPilotName = (char*)LogisticsData::instance->getBestPilot( salvageTarget->tonnage );
					if (vehicleID[commanderID] != 147)
						STOP(("missiongui.beginvtol: bad vehicleID"));
					MPlayer->sendReinforcement(vehicleID[commanderID], salvageTarget->netRosterIndex, newPilotName, commanderID, vPos[commanderID], 3);
					return;
				}
			}

			AppearanceType* appearanceType = appearanceTypeList->getAppearance( BLDG_TYPE << 24, "karnov" );
			vTol[commanderID] = new BldgAppearance;
			vTol[commanderID]->init( appearanceType );
			
			if (MPlayer)
				mechToRecover[commanderID] = MPlayer->moverRoster[MPlayer->reinforcements[commanderID][1]];
			else
				mechToRecover[commanderID] = salvageTarget;
			mechRecovered[commanderID] = false;	//Need to know when mech is done so Karnov can fly away.
			
			if (!dustCloud[commanderID] && prefs.useNonWeaponEffects)
			{
				if (strcmp(weaponEffects->GetEffectName(KARNOV_DUST_CLOUD),"NONE") != 0)
				{
					//--------------------------------------------
					// Yes, load it on up.
					unsigned flags = 0;
		
					Check_Object(gosFX::EffectLibrary::Instance);
					gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(KARNOV_DUST_CLOUD));
					
					if (gosEffectSpec)
					{
						dustCloud[commanderID] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
						gosASSERT(dustCloud[commanderID] != NULL);
						
						MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
					}
				}
			}
			
			if (!recoveryBeam[commanderID])
			{
				if (strcmp(weaponEffects->GetEffectName(KARNOV_RECOVERY_BEAM),"NONE") != 0)
				{
					//--------------------------------------------
					// Yes, load it on up.
					unsigned flags = 0;
		
					Check_Object(gosFX::EffectLibrary::Instance);
					gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(KARNOV_RECOVERY_BEAM));
					
					if (gosEffectSpec)
					{
						recoveryBeam[commanderID] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
						gosASSERT(recoveryBeam[commanderID] != NULL);
						
						MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
					}
				}
			}
		}
	}

	if (commanderID == Commander::home->getId())
	{
		switch (vehicleID[commanderID])
		{
			case 120:	//Minelayer
				soundSystem->playSupportSample(SUPPORT_MINELAYER);
			break;

			case 182:	//Repair
				soundSystem->playSupportSample(SUPPORT_REPAIR);
			break;

			case 393:	//Scout
				soundSystem->playSupportSample(SUPPORT_SCOUT);
			break;

			case 147:	//Recovery
				soundSystem->playSupportSample(SUPPORT_RECOVER);
			break;

			case 415:	//Artillery
				soundSystem->playSupportSample(SUPPORT_ARTILLERY);

			break;
		}
	}

	Stuff::Vector3D newPos = vtolPos;
	float rotation = 0.f;  // this needs to be pointing 180 degrees from drop point
	
	if (vehicleID[commanderID] == 147)
	{
		//Move wPos to be over the cockpit of the downed mech.
		if (mechToRecover[commanderID] && mechToRecover[commanderID]->appearance)
			newPos = mechToRecover[commanderID]->appearance->getNodeNamePosition("cockpit");
	}
	
	if (vehicleID[commanderID] != 147)
		soundSystem->playDigitalSample(VTOL_ANIMATE,newPos);
	else
		soundSystem->playDigitalSample(SALVAGE_CRAFT,newPos);
	
	vTol[commanderID]->setObjectParameters( newPos, 0, false, Team::home->id, 0);
	eye->update();
	vTol[commanderID]->update();
	vTol[commanderID]->setInView( true );
	vTol[commanderID]->setVisibility( 1, 1 );
	vTol[commanderID]->rotation = rotation;
	
	if (vehicleID[commanderID] != 147)
		vTol[commanderID]->setGesture(0);		//Start Animation at beginning
	else
		vTol[commanderID]->setGesture(1);		//Start Animation with Landing -- KARNOV

	if (dustCloud[commanderID])
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
					
		Stuff::Vector3D dustPos = vTol[commanderID]->position;
		Stuff::Point3D wakePos;
		wakePos.x = -dustPos.x;
		wakePos.y = dustPos.z;
		wakePos.z = dustPos.y;
			
		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(wakePos);
					
		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
	
		dustCloud[commanderID]->SetLoopOff();
		dustCloud[commanderID]->SetExecuteOn();
		dustCloud[commanderID]->Start(&info);
	}
				
  	paintingVtol[commanderID] = 1;
	vTolSpeed = -75.f;

	vPos[commanderID] = vtolPos;

	vehicleDropped[commanderID] = false;
	
	if ( !MPlayer || commanderID == MPlayer->commanderID )
		controlGui.disableAllVehicleButtons();
}

bool MissionInterfaceManager::canSalvage( GameObject* pMover )
{
	bool lineOfSight = Team::home->teamLineOfSight(pMover->getPosition(),0.0f);

	return (controlGui.forceGroupBar.getIconCount() < MAX_ICONS
		&& target->isDisabled() && lineOfSight && 
		target->getObjectClass() == BATTLEMECH ) ? 1 : 0;
}

void MissionInterfaceManager::doSalvage()
{
	if ( canSalvage(target) )
	{
		TacticalOrder tacOrder;
	
		tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_RECOVER );
		tacOrder.targetWID = target->getWatchID();
		tacOrder.attackParams.type = ATTACK_NONE;
		tacOrder.attackParams.method = ATTACKMETHOD_RAMMING;
		tacOrder.attackParams.pursue = true;
		tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;

		handleOrders( tacOrder );
	}

}

bool MissionInterfaceManager::canRepair( GameObject* pMover )
{
	if ( !pMover )
		return 0;
	
	if ( !pMover->isMover() )
		return 0;

	if ( Team::home->isEnemy( pMover->getTeam() ) )
		return 0;

	if (pMover->isDisabled() || pMover->isDestroyed())
		return 0;

	// find the selected guy
	int watchID = 0;
	Team* pTeam = Team::home;
	for (long i = 0; i < pTeam->getRosterSize(); i++)
	{
		Mover* pTmpMover = (Mover*)pTeam->getMover( i );
		if ( pTmpMover->isSelected() && pTmpMover->getCommander()->getId() == Commander::home->getId() )
		{
			if ( !watchID )
				watchID = pTmpMover->getWatchID();
			else
				watchID = -1;
		}
	}
	
	if  ( ((Mover*)pMover)->needsRefit() || ( ((Mover*)pMover)->refitBuddyWID == watchID && watchID) )
		return true;

	return 0;

}
void MissionInterfaceManager::doRepair(GameObject* who)
{
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_REFIT, false);
	tacOrder.targetWID = who->getWatchID();
	tacOrder.selectionIndex = -1;
	tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
	tacOrder.moveParams.faceObject = true;
	tacOrder.moveParams.fromArea = -1;
	tacOrder.moveParams.wait = false;

	handleOrders( tacOrder );

	soundSystem->playDigitalSample(BUTTON5);
	controlGui.setDefaultSpeed();
}

bool MissionInterfaceManager::canRepairBay( GameObject* bay)
{
	if (bay && bay->getRefitPoints() > 0.0f)
	{
		Team* pTeam = Team::home;
		for (long i = 0; i < pTeam->getRosterSize(); i++)
		{
			Mover* pMover = (Mover*)pTeam->getMover( i );
			if ( pMover->isSelected() && pMover->needsRefit() && pMover->getCommander()->getId() == Commander::home->getId() )
			{
				return true;
			}
		}
	}

	return false;
}
void MissionInterfaceManager::doRepairBay(GameObject* who)
{
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_GETFIXED, false);
	tacOrder.targetWID = who->getWatchID();
	tacOrder.selectionIndex = -1;
	tacOrder.moveParams.wayPath.mode[0] = controlGui.getWalk() ?  TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
	tacOrder.moveParams.faceObject = true;
	tacOrder.moveParams.fromArea = -1;
	tacOrder.moveParams.wait = false;

	handleOrders( tacOrder );

	soundSystem->playDigitalSample(BUTTON5);
	controlGui.setDefaultSpeed();
}

int MissionInterfaceManager::vehicleCommand()
{
	if ( controlGui.getVehicleCommand() )
		controlGui.setVehicleCommand( false );
	else
		controlGui.setVehicleCommand( true );

	return 0;
}

int MissionInterfaceManager::showObjectives()
{
	controlGui.startObjectives( !controlGui.objectivesStarted( ) );
	return 0;
}

int MissionInterfaceManager::showObjectives(bool on)
{
	controlGui.startObjectives( on );
	return 0;
}

int MissionInterfaceManager::togglePause()
{
	if ( !bPausedWithoutMenu )
	{
		bPaused ^= 1;
	}
	else
		bPausedWithoutMenu = 0;

	soundSystem->playDigitalSample( LOG_NEXTBACKBUTTONS );
		

	if ( bPaused )
	{
		controlGui.beginPause();
	}
	else
	{
		controlGui.endPause();
	}

	return 1;
}

int MissionInterfaceManager::togglePauseWithoutMenu()
{
	//bPausedWithoutMenu;

	if ( !bPausedWithoutMenu )
	{
		if ( !bPaused )
		{
			bPausedWithoutMenu = 1;
			bPaused = 1;
			return 1;
		}
		else
		{
			return 0;
		}
	}
	else
	{
		gosASSERT(bPaused);
		bPausedWithoutMenu = 0;
		bPaused = 0;
		return 1;
	}
}

bool MissionInterfaceManager::isPaused()
{
	return bPaused;
}

// [VISUAL_CAPTURE v1.5] Freeze/unfreeze the sim for a deterministic capture
// sweep WITHOUT raising the pause menu or playing a sound. We mirror the
// menu-less pause flags directly: scenarioTime + unit/sensor/collision updates
// are gated on isPaused() (mission.cpp:531), so this is exactly the clock the
// capture needs frozen. We do NOT touch controlGui (no beginPause/endPause) so
// no HUD pause overlay is drawn into the captured frame. The pre-sweep flag
// pair is restored verbatim by the caller via getCapturePauseState(), so a
// continuing sim resumes exactly as before.
void MissionInterfaceManager::setPausedForCapture(bool paused)
{
	bPaused = paused ? 1 : 0;
	bPausedWithoutMenu = paused ? 1 : 0;
}

int MissionInterfaceManager::getCapturePauseState()
{
	return ((bPaused ? 1 : 0) << 1) | (bPausedWithoutMenu ? 1 : 0);
}

// Restore the exact (bPaused, bPausedWithoutMenu) pair captured before the
// sweep. Resumes a continuing sim exactly as it was, including the case where
// the player had paused without the menu before the capture fired.
void MissionInterfaceManager::restoreCapturePauseState(int packed)
{
	bPaused = (packed >> 1) & 1;
	bPausedWithoutMenu = packed & 1;
}

// [VISUAL_CAPTURE v1.5] extern-C shims so the GameOS-side capture TU
// (gos_visual_capture.cpp) can drive the mission pause flags WITHOUT including
// missiongui.h (which pulls in mclib.h/mover.h/controlgui.h, not on that TU's
// include path). Return -1 when no mission interface exists yet (e.g. in menus)
// so the capture site can no-op gracefully.
extern "C" int mc2VisualCaptureGetPauseState()
{
	MissionInterfaceManager* mim = MissionInterfaceManager::instance();
	return mim ? mim->getCapturePauseState() : -1;
}

extern "C" void mc2VisualCaptureSetPaused(int paused)
{
	MissionInterfaceManager* mim = MissionInterfaceManager::instance();
	if (mim) mim->setPausedForCapture(paused != 0);
}

extern "C" void mc2VisualCaptureRestorePauseState(int packed)
{
	MissionInterfaceManager* mim = MissionInterfaceManager::instance();
	if (mim) mim->restoreCapturePauseState(packed);
}

bool MissionInterfaceManager::isPausedWithoutMenu()
{
	return bPausedWithoutMenu;
}

void	MissionInterfaceManager::swapResolutions()
{
	controlGui.swapResolutions( Environment.screenWidth, Environment.screenHeight );
	resolution = Environment.screenWidth;
	keyboardRef->init();
}

int MissionInterfaceManager::switchTab()
{
	controlGui.switchTabs(1);		
	return 1;
}

int MissionInterfaceManager::reverseSwitchTab()
{
	controlGui.switchTabs(-1);		
	return 1;
}


int MissionInterfaceManager::infoCommand()
{
	if ( !controlGui.infoButtonPressed() )
		controlGui.pressInfoButton();
	return 1;
}

int MissionInterfaceManager::infoButtonReleased()
{
	if ( controlGui.infoButtonPressed() )
		controlGui.pressInfoButton();

	return 1;
}
int MissionInterfaceManager::energyWeapons()
{
	if ( gos_GetKeyStatus( (gosEnum_KeyIndex)(commands[ENERGY_WEAPON_INDEX].key & 0x0000ffff) ) == KEY_HELD )
		bEnergyWeapons = 1;	
	else
		bEnergyWeapons = 0;
	return 0;
}
int MissionInterfaceManager::sendAirstrike()
{
	controlGui.pressAirstrikeButton();
	return 1;
}
int MissionInterfaceManager::sendLargeAirstrike()
{
	controlGui.pressLargeAirstrikeButton();
	return 1;
}
int MissionInterfaceManager::gotoNextNavMarker()
{
	// need to find the next nav marker....
	for ( CObjectives::EIterator iter = Team::home->objectives.Begin();
		!iter.IsDone(); iter++ )
		{
			if ( (*iter)->Status(Team::home->objectives) == OS_UNDETERMINED  && (*iter)->DisplayMarker() == NAV )
			{
				Stuff::Vector3D pos;
				pos.x = (*iter)->MarkerX();
				pos.y = (*iter)->MarkerY();
				if ( !pos.x && !pos.y )
					continue;
				pos.z = land->getTerrainElevation( pos );
				LocationNode path;
				path.location = pos;
				path.run = true;
				path.next = NULL;

				TacticalOrder tacOrder;
				tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_MOVETO_POINT, false);
				tacOrder.initWayPath(&path);
				tacOrder.moveParams.wait = false;
				tacOrder.moveParams.wayPath.mode[0] =  controlGui.getWalk() ? TRAVEL_MODE_SLOW : TRAVEL_MODE_FAST;
				if ( controlGui.getMines() )
					tacOrder.moveParams.mode = MOVE_MODE_MINELAYING;
				tacOrder.pack(NULL, NULL);


				handleOrders(tacOrder);
				
				soundSystem->playDigitalSample(BUTTON5);

				return 1;
			}
		}

	// if we got here there weren't any valid markers, play bad sound
	soundSystem->playDigitalSample( INVALID_GUI );
	return 0;

}
int MissionInterfaceManager::sendSensorStrike()
{
	controlGui.pressSensorStrikeButton();
	return 1;
}

int MissionInterfaceManager::quickDebugInfo() {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		int row, col;
		land->worldToCell(wPos, row, col);
		char debugString[256];
		// NULL guards for bail'd-out GlobalMoveMap[] (mod content).
		int infoML = target ? target->getMoveLevel() : 0;
		int infoArea = (GlobalMoveMap[infoML]) ? GlobalMoveMap[infoML]->calcArea(row, col) : -1;
		int infoArea0 = (GlobalMoveMap[0]) ? GlobalMoveMap[0]->calcArea(row, col) : -1;
		if (target)
			sprintf(debugString, "INFO = %s(%c%c) %d(W%d) [%d, %d] (%.4f, %.4f, %.4f) #Pth=%d(%d) OBJ = %s\n",
				terrainStr[GameMap->getTerrain(row, col)],
				GameMap->getPassable(row, col) ? 'T' : 'F',
				GameMap->getForest(row, col) ? 'T' : 'F',
				infoArea,
				GameMap->getShallowWater(row, col) ? 1 : (GameMap->getDeepWater(row, col) ? 2 : 0),
				row, col,
				wPos.x, wPos.y, wPos.z,
				PathManager->numPaths, PathManager->peakPaths, target->getName());
		else
			sprintf(debugString, "INFO = %s(%c%c) %d(W%d) [%d, %d] (%.4f, %.4f, %.4f) #Pth=%d(%d)\n",
				terrainStr[GameMap->getTerrain(row, col)],
				GameMap->getPassable(row, col) ? 'T' : 'F',
				GameMap->getForest(row, col) ? 'T' : 'F',
				infoArea0,
				GameMap->getShallowWater(row, col) ? 1 : (GameMap->getDeepWater(row, col) ? 2 : 0),
				row, col,
				wPos.x, wPos.y, wPos.z,
				PathManager->numPaths, PathManager->peakPaths);
		DEBUGWINS_print(debugString);
		sprintf(debugString, "REQ =");
		char s[10];
		for (long i = 0; i < 25; i++) {
			sprintf(s, " %02d", PathManager->sourceTally[i]);
			strcat(debugString, s);
		}
		DEBUGWINS_print(debugString);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::setGameObjectWindow() {
	
	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		DEBUGWINS_setGameObject(-1, target);
		lastTime = gos_GetElapsedTime();
	}	
	#endif
	return(1);
}

int MissionInterfaceManager::pageGameObjectWindow1() {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (DebugGameObject[0] && DebugGameObject[0]->isMover())
			((MoverPtr)DebugGameObject[0])->debugPage++;
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::pageGameObjectWindow2() {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (DebugGameObject[1] && DebugGameObject[1]->isMover())
			((MoverPtr)DebugGameObject[1])->debugPage++;
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::pageGameObjectWindow3() {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (DebugGameObject[2] && DebugGameObject[2]->isMover())
			((MoverPtr)DebugGameObject[2])->debugPage++;
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::rotateObjectLeft () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			target->rotate(4.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::rotateObjectRight () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			target->rotate(-4.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::jumpToDebugGameObject1() {

	#ifndef FINAL
	DEBUGWINS_viewGameObject(0);
	#endif
	return(1);
}

int MissionInterfaceManager::jumpToDebugGameObject2() {

	#ifndef FINAL
	DEBUGWINS_viewGameObject(1);
	#endif
	return(1);
}

int MissionInterfaceManager::jumpToDebugGameObject3() {

	#ifndef FINAL
	DEBUGWINS_viewGameObject(2);
	#endif
	return(1);
}

int MissionInterfaceManager::toggleDebugWins() {

	#ifndef FINAL
	static long debugWinsState = 0;
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		debugWinsState = (++debugWinsState % 6);
		bool debugStateFlags[6][7] = {
			{false, false, false, false, false, false, false},
			{false, false, false, false, false, true, false},
			{false, false, false, false, false, true, true},
			{false, true, true, true, false, true, true},
			{true, true, true, true, false, true, true},
			{false, false, false, false, true, false, true}
		};
		DEBUGWINS_display(debugStateFlags[debugWinsState]);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

extern bool ShowMovers;
int MissionInterfaceManager::showMovers() {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		ShowMovers = !ShowMovers;
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::cullPathAreas () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		CullPathAreas = !CullPathAreas;
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::zeroHPrime() {

	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		ZeroHPrime = !ZeroHPrime;
		lastTime = gos_GetElapsedTime();
	}
	return(1);
}

int MissionInterfaceManager::calcValidAreaTable() {

	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		CalcValidAreaTable = !CalcValidAreaTable;
		lastTime = gos_GetElapsedTime();
	}
	return(1);
}

int MissionInterfaceManager::teleport() {
	
	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		Team* pTeam = Team::home;
		Stuff::Vector3D moveGoals[MAX_MOVERS];
		long numMovers = 0;
		for (long i = 0; i < pTeam->getRosterSize(); i++) {
			Mover* mover = pTeam->getMover(i);
			if (mover->isSelected() && mover->getCommander()->getId() == Commander::home->getId())
				numMovers++;
		}
		MoverGroup::calcMoveGoals(wPos, numMovers, moveGoals);
		numMovers = 0;
		for (int i = 0; i < pTeam->getRosterSize(); i++) {
			Mover* mover = (Mover*)pTeam->getMover( i );
			if (mover->isSelected() && mover->getCommander()->getId() == Commander::home->getId() )
				((MoverPtr)mover)->setTeleportPosition(moveGoals[numMovers++]);
		}
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

extern GameLog*	LRMoveLog;

extern bool quitGame;

int MissionInterfaceManager::globalMapLog () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if (MPlayer)		//Otherwise we crash in singlePlayer!!
	{
		if ((lastTime + 0.5) < gos_GetElapsedTime()) 
		{
			//MPlayer->leaveSession();
			//quitGame = true;
			//GlobalMap::toggleLog();
			lastTime = gos_GetElapsedTime();
		}
	}
	#endif
	return(1);
}

int MissionInterfaceManager::brainDead () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		for (long i = 1; i < MAX_TEAMS; i++)
			MechWarrior::brainsEnabled[i] = !MechWarrior::brainsEnabled[i];
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::goalPlan() {
	
	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		Team* pTeam = Team::home;
		long numMovers = 0;
		for (long i = 0; i < pTeam->getRosterSize(); i++) {
			Mover* mover = pTeam->getMover(i);
			if (mover->isSelected() && mover->getCommander()->getId() == Commander::home->getId())
				numMovers++;
		}
		for (int i = 0; i < pTeam->getRosterSize(); i++) {
			Mover* mover = (Mover*)pTeam->getMover( i );
			if (mover->isSelected() && mover ->getCommander()->getId() == Commander::home->getId()) {
				((MoverPtr)mover)->getPilot()->setUseGoalPlan(!((MoverPtr)mover)->getPilot()->getUseGoalPlan());
				((MoverPtr)mover)->getPilot()->setMainGoal(GOAL_ACTION_NONE, NULL, NULL, -1.0);
			}
		}
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::enemyGoalPlan() {
	
	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		EnemiesGoalPlan = !EnemiesGoalPlan;
		for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
			Mover* mover = ObjectManager->getMover(i);
			if (mover->getTeam() != Team::home) {
				((MoverPtr)mover)->getPilot()->setUseGoalPlan(EnemiesGoalPlan);
				((MoverPtr)mover)->getPilot()->setMainGoal(GOAL_ACTION_NONE, NULL, NULL, -1.0);
			}
		}
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::showVictim () {
	
	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) 
	{
		if (DebugGameObject[0] && DebugGameObject[0]->isMover()) 
		{
			GameObjectPtr targetObj = ((MoverPtr)DebugGameObject[0])->getPilot()->getCurTacOrder()->getTarget();
			if (targetObj)
				DEBUGWINS_setGameObject(-1, targetObj);
		}
		lastTime = gos_GetElapsedTime();
	}	
	#endif
	return(1);
}

void damageObject (GameObjectPtr victim, float damage) {

	WeaponShotInfo shot;
	switch (victim->getObjectClass()) {
		case BATTLEMECH:
		case GROUNDVEHICLE:
			shot.init(0, -2, damage, victim->calcHitLocation(NULL,-1,ATTACKSOURCE_WEAPONFIRE,0), 0);
			victim->handleWeaponHit(&shot, (MPlayer != NULL));
			break;
		default:
			shot.init(0, -1, damage, 0, 0);
			victim->handleWeaponHit(&shot, (MPlayer != NULL));
			break;
	}
}

int MissionInterfaceManager::damageObject1 () 
{
	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) 
	{
		if (target)
			target->repairAll();

		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject2 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 4.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject3 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 9.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject4 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 16.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject5 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 25.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject6 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 36.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject7 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 49.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject8 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 64.0);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject9 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) {
		if (target)
			damageObject(target, 81);
		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::damageObject0 () {

	#ifndef FINAL
	static double lastTime = 0.0;
	if ((lastTime + 0.5) < gos_GetElapsedTime()) 
	{
		if (target)
		{
			if (target->isMover())
				((MoverPtr)target)->disable(0);
			else
				damageObject(target, 100.0);
		}

		lastTime = gos_GetElapsedTime();
	}
	#endif
	return(1);
}

int MissionInterfaceManager::toggleCompass()
{
	((GameCamera*)eye)->toggleCompass();
	return 0;
}

bool MissionInterfaceManager::selectionIsHelicopters( )
{
	Team* pTeam = Team::home;
   	for (long i=0;i<pTeam->getRosterSize();i++)
   	{
   		Mover* pMover = (Mover*)pTeam->getMover( i );
   		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
   		{
			if ( pMover->getMoveType() != MOVETYPE_AIR )
			{
				return 0;
			}
   		}
   	}

	return true;
}

int MissionInterfaceManager::saveHotKeys( FitIniFile& file )
{
	file.writeBlock( "Keyboard" );
	// Version sentinel so stale saves from a different MAX_COMMAND are rejected on load.
	file.writeIdLong( "KeyboardVersion", MAX_COMMAND );
	for ( int i = 0; i < MAX_COMMAND; i++ )
	{
		char header[256];
		sprintf( header, "Key%ld", i );
		file.writeIdLong( header, commands[i].key );
	}

	file.writeIdLong( "WayPointKey", WAYPOINT_KEY );
	file.writeIdBoolean( "UseLeftRightMouseProfile", useLeftRightMouseProfile );

	return 0;

}
int MissionInterfaceManager::loadHotKeys( FitIniFile& file )
{
	if ( OldKeys[0] == -1 )
	{
		for ( int i = 0; i < MAX_COMMAND; i++ )
		{
			OldKeys[i] = commands[i].key;
		}
	}

	if ( NO_ERR == file.seekBlock( "Keyboard" ) )
	{
		long version = -1;
		file.readIdLong( "KeyboardVersion", version );
		if ( version != MAX_COMMAND )
		{
			// Stale save (different command count — e.g. a key was added/removed).
			// Silently discard and keep compiled-in defaults.
			return -1;
		}

		for ( int i = 0; i < MAX_COMMAND; i++ )
		{
			char header[256];
			sprintf( header, "Key%ld", i );
			file.readIdLong( header, commands[i].key );
		}

		long tmp;
		file.readIdLong( "WayPointKey", tmp );
		WAYPOINT_KEY = gosEnum_KeyIndex( tmp );

		file.readIdBoolean( "UseLeftRightMouseProfile", useLeftRightMouseProfile );
		return 0;
	}


	return -1;
}

int MissionInterfaceManager::setHotKey( int whichCommand, gosEnum_KeyIndex newKey, bool bShift, bool bControl, bool bAlt )
{
	gosASSERT( whichCommand < MAX_COMMAND );
	long oldKey = commands[whichCommand].key;
	long key = newKey;
	if ( bShift )
		key |= SHIFT;
	if ( bControl )
		key |= CTRL;
	if ( bAlt )
		key |= ALT;

	if ( commands[whichCommand].key & WAYPT )
		key |= WAYPT;
	else
	{
		// change corresponding waypoint keys
		for ( int i = 0; i < MAX_COMMAND; i++ )
		{
			if ( ((commands[i].key & 0x0000ffff) == oldKey)  && (commands[i].key & WAYPT) )
			{
				commands[i].key = key | WAYPT;
			}
		}
	}

	commands[whichCommand].key = key;

	return 0;
}

int MissionInterfaceManager::getHotKey( int whichCommand, gosEnum_KeyIndex& newKey, bool& bShift, bool& bControl, bool& bAlt )
{
	gosASSERT( whichCommand < MAX_COMMAND );
	
	long key = commands[whichCommand].key;

	if ( key & SHIFT )
		bShift = true;
		
	if ( key & CTRL )
		bControl = true;
	
	if ( key & ALT )
		bAlt = true;

	key &= 0x0000ffff;
	newKey = (gosEnum_KeyIndex)key;

	return 0;
}

int MissionInterfaceManager::setWayPointKey( gosEnum_KeyIndex key )
{
	WAYPOINT_KEY = key;

	return 0;
}

void MissionInterfaceManager::setAOEStyle()
{
	useLeftRightMouseProfile = true;
}
void MissionInterfaceManager::setMCStyle()
{
	useLeftRightMouseProfile = false;
}

bool MissionInterfaceManager::isAOEStyle()
{
	return useLeftRightMouseProfile == true;
}
bool MissionInterfaceManager::isMCStyle()
{
	return useLeftRightMouseProfile == false;
}

void MissionInterfaceManager::doEject( GameObject* who )
{
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_EJECT, true);
	tacOrder.pack(NULL, NULL);
	MoverPtr pMover = (MoverPtr)who;
	if (MPlayer && !MPlayer->isServer())
		MPlayer->sendPlayerOrder(&tacOrder, false, 1, &pMover);
	else
		pMover->handleTacticalOrder(tacOrder);
}

bool MissionInterfaceManager::hotKeyIsPressed( int whichCommand )
{
	long key = commands[whichCommand].key;
	bool bShift = 0;
	bool bControl = 0;
	bool bAlt = 0;

	if ( key & SHIFT )
		bShift = true;
		
	if ( key & CTRL )
		bControl = true;
	
	if ( key & ALT )
		bAlt = true;

	key &= 0x0000ffff;

	bool shiftDn = userInput->shift();
	bool ctrlDn = userInput->ctrl();
	bool altDn = userInput->alt();

	if ( gos_GetKeyStatus( (gosEnum_KeyIndex)(key&0x000000ff) ) == KEY_PRESSED ||
		gos_GetKeyStatus( (gosEnum_KeyIndex)(key&0x000000ff) ) == KEY_HELD )
	{
		if ( shiftDn == bShift 
			&& ctrlDn == bControl
			&& altDn == bAlt )
		{
			return true;
		}
	}

	return false;

}

void testKeyStuff()
{
	bool bShift, bCtrl, bAlt;
	gosEnum_KeyIndex key = KEY_F9;
	MissionInterfaceManager::instance()->getHotKey( 5, key, bShift, bCtrl, bAlt );
	MissionInterfaceManager::instance()->setHotKey( 5, KEY_L, 0, 0, 0 );
	MissionInterfaceManager::instance()->getHotKey( 6, key, bShift, bCtrl, bAlt );

	FitIniFile file;
	file.open( "keyboards.fit" );
	MissionInterfaceManager::instance()->loadHotKeys( file );
	MissionInterfaceManager::instance()->getHotKey( 6, key, bShift, bCtrl, bAlt );

}

int MissionInterfaceManager::toggleHotKeys()
{
	bDrawHotKeys ^= 1;
	if ( bDrawHotKeys )
	{
		keyboardRef->reseed( commands );

		if ( !isPaused() )
			togglePauseWithoutMenu();
	}
	else if ( !bDrawHotKeys && isPaused() && isPausedWithoutMenu() )
		togglePauseWithoutMenu();
	return 1;
}

void MissionInterfaceManager::drawHotKeys()
{

	keyboardRef->render();
	return;


}

void MissionInterfaceManager::drawHotKey( const char* keyString, const char* descString, long x, long y )
{
	hotKeyFont.render( keyString, 0, y, x, Environment.screenHeight, 0xffffffff, 0, 1 );

	hotKeyFont.render( descString,  x + (10 * Environment.screenWidth/640.f), y, 
		Environment.screenWidth, Environment.screenHeight, 0xffffffff, 0, 0 );

}

void MissionInterfaceManager::doGuardTower()
{

	for (long i = 0; i < Team::home->getRosterSize(); i++)
	{
		Mover* pMover = Team::home->getMover( i );
		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId())
		{
			pMover->suppressionFire = true;
		}
	}
			
	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_POINT);
	//tacOrder.init(ORDER_ORIGIN_PLAYER,  target ? TACTICAL_ORDER_ATTACK_OBJECT : TACTICAL_ORDER_ATTACK_POINT);
	//tacOrder.targetWID = target ? target->getWatchID() : 0;
	//if ( !target )
	tacOrder.attackParams.targetPoint = wPos;			
	tacOrder.attackParams.type = bEnergyWeapons ? ATTACK_CONSERVING_AMMO : ATTACK_TO_DESTROY;
	tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
	tacOrder.attackParams.range = FIRERANGE_CURRENT;
	tacOrder.attackParams.pursue = false;
	tacOrder.moveParams.faceObject = true;
	tacOrder.moveParams.fromArea = -1;
	tacOrder.moveParams.wait = false;

	handleOrders( tacOrder );

	controlGui.setDefaultSpeed();
}

int		MissionInterfaceManager::toggleHoldPosition()
{
	controlGui.toggleHoldPosition();

	return 1;
}

int MissionInterfaceManager::handleChatKey()
{
	controlGui.toggleChat(0);
	return 1;
}

int	MissionInterfaceManager::handleTeamChatKey()
{
	controlGui.toggleChat(1);
	return 1;
}

float slopeTest[8] = {0.09849140f, 0.30334668f, 0.53451114f, 0.82067879f, 1.21850353f, 1.87086841f, 3.29655821f, 10.15317039f};
long MissionInterfaceManager::makeMoveCursor( bool bLineOfSite )
{

	long rotation = calcRotation();

	// leigh put these in the file funny
	// OK, rotations go clockwise starting from 180...
	gosASSERT( rotation >= 0 && rotation < 32 );
	return mState_WALK1 + 32 * bLineOfSite + rotation;
}

long MissionInterfaceManager::makeJumpCursor( bool bLineOfSite )
{
	long rotation = calcRotation();

	// leigh put these in the file funny
	// OK, rotations go clockwise starting from 180...
	gosASSERT( rotation >= 0 && rotation < 32 );
	return mState_JUMP1 + 32 * bLineOfSite + rotation;

}


long MissionInterfaceManager::makeRunCursor( bool bLineOfSite )
{
	long rotation = calcRotation();

	// leigh put these in the file funny
	// OK, rotations go clockwise starting from 180...
	gosASSERT( rotation >= 0 && rotation < 32 );
	return mState_RUN1 + 32 * bLineOfSite + rotation;

}

long MissionInterfaceManager::calcRotation()
{
	Stuff::Vector3D pos;
	pos.x = 0.f;
	pos.y = 0.f;
	pos.z = 0.f;
	float count = 0;
	for (long i = 0; i < Team::home->getRosterSize(); i++)
	{
		Mover* pMover = Team::home->getMover( i );
		if ( pMover->isSelected() && pMover->getCommander()->getId() == Commander::home->getId() )
		{
			pos.x += pMover->getPosition().x;
			pos.y += pMover->getPosition().y;
			pos.z += pMover->getPosition().z;
			count += 1.f;
		}
	}

	if ( count && (turn > 3))
	{
		pos.x /= count;
		pos.y /= count;
		pos.z /= count;
	}
	else
		return mState_NORMAL;

	//Now works by finding the vector in world space between the mouse cursor
	// and the movers.  It then normalizes that vector and multiplies by 200
	// to make large.  It then adds that vector to the camera position and we
	// do the projections based on the camera center (which is always the
	// center of the screen) and the new vector.  Complex but it works!!
	// -fs
	Stuff::Vector3D actualPos;
	actualPos.Subtract(pos, wPos);
	actualPos.z = 0.0f;
	
	float actualLength = actualPos.GetLength();
	if (actualLength > Stuff::SMALL)
		actualPos /= actualLength;
	actualPos *= 200.0f;
	
	Stuff::Vector3D camPos = eye->getPosition();
	
	Stuff::Vector4D screenPosMover;
	Stuff::Vector4D screenPosGoal;
	actualPos.Add(camPos,actualPos);
	// [PROJECTZ:ScreenXYOracle id=gui_mover_rotation_pos]
	eye->projectForScreenXY( actualPos, screenPosMover );

	// need to find second position
	// [PROJECTZ:ScreenXYOracle id=gui_cam_rotation_goal]
	eye->projectForScreenXY( camPos, screenPosGoal );
	
	//CRAZY ATAN code.  Out for now.
	// -fs
	/*
	// determine angle between the two
	float deltaY = -(screenPosGoal.y - screenPosMover.y);
	float deltaX = screenPosGoal.x - screenPosMover.x;

	float theta = 0;
	if ( deltaY && deltaX )
	{
		theta = atan( deltaY/deltaX );
		theta *= RADS_TO_DEGREES; 
	}

	if ( deltaX < 0 )
		theta += 180.f;

	long rotation = ((theta/360.f * 32.f));
	*/

	//Do it the MC1 way!
	float slope = 0.0f;
	if (fabs(screenPosGoal.x - screenPosMover.x) > Stuff::SMALL)
		slope = fabs(screenPosGoal.y - screenPosMover.y) / fabs(screenPosGoal.x - screenPosMover.x);

	long counter = 0;
	long rotation = 0;
	//while (slope > slopeTest[counter] && counter < 8)
    // sebi: ORIG BUG FIX: thanks AddressSanitizer
	while (counter < 8 && slope > slopeTest[counter])
		counter++;
	if (screenPosGoal.y < screenPosMover.y)
	{
		if (screenPosGoal.x < screenPosMover.x)	// quadrant 0
			rotation = counter;
		else							// quadrant 1
			rotation = 8 + (8 - counter);
	}
	else
	{
		if (screenPosGoal.x > screenPosMover.x)	// quadrant 2
		{
			rotation = 16 + counter;
		}
		else							// quadrant 3
		{
			rotation = 24 + (8 - counter);
			if (rotation == 32)
				rotation = 0;
		}
	}

	return rotation;
}

//--------------------------------------------------------------------------------------
void MissionInterfaceManager::Save (FitIniFilePtr file)
{
	//If VTOL or Karnov are active AND HAVEN'T FINISHED THEIR OPERATION, save them, what vehicle they are going to drop or what mech they were fixing.
	long vtolNum = Commander::home->getId();

	if (vTol[vtolNum] && paintingVtol[vtolNum] && (!vehicleDropped[vtolNum] && !mechRecovered[vtolNum]))
	{
		//Save 'em
		file->writeBlock("VTOLData");

		file->writeIdFloat("VtolPosX",vPos[vtolNum].x);
		file->writeIdFloat("VtolPosY",vPos[vtolNum].y);
		file->writeIdFloat("VtolPosZ",vPos[vtolNum].z);

		file->writeIdLong("VehicleID",vehicleID[vtolNum]);

		if (mechToRecover[vtolNum])
			file->writeIdLong("SalvageMechWID", mechToRecover[vtolNum]->getWatchID());
		else
			file->writeIdLong("SalvageMechWID", 0);

		file->writeIdLong("VTOLFrame",vTol[vtolNum]->currentFrame);
		file->writeIdLong("VTOLGesture",vTol[vtolNum]->getCurrentGestureId());
	}
}

//--------------------------------------------------------------------------------------
void MissionInterfaceManager::Load (FitIniFilePtr file)
{
	//Is there any VTOL data here?  OK if not!
	long vtolNum = Commander::home->getId();
	long commanderID = vtolNum;

	if (file->seekBlock("VTOLData") == NO_ERR)
	{
		//Load 'em
		file->readIdFloat("VtolPosX",vPos[vtolNum].x);
		file->readIdFloat("VtolPosY",vPos[vtolNum].y);
		file->readIdFloat("VtolPosZ",vPos[vtolNum].z);

		file->readIdLong("VehicleID",vehicleID[vtolNum]);

		if (vehicleID[vtolNum] != 147)		//NOT a recovery vehicle.  Standard VTOL
		{
			//Go ahead and make a VTOL.  We're gonna need it!
			AppearanceType* appearanceType = appearanceTypeList->getAppearance( BLDG_TYPE << 24, "vtol" );
			vTol[commanderID] = new BldgAppearance;
			vTol[commanderID]->init( appearanceType );
		}
		else
		{
			AppearanceType* appearanceType = appearanceTypeList->getAppearance( BLDG_TYPE << 24, "karnov" );
			vTol[commanderID] = new BldgAppearance;
			vTol[commanderID]->init( appearanceType );
		}

		long mechWID = 0;
		file->readIdLong("SalvageMechWID", mechWID);
		mechToRecover[vtolNum] = (MoverPtr)ObjectManager->getByWatchID(mechWID);

		file->readIdFloat("VTOLFrame",vTol[vtolNum]->currentFrame);

		long gestureId = 0;
		file->readIdLong("VTOLGesture",gestureId);
		vTol[vtolNum]->setGesture(gestureId);

		//Now setup everything else.
		// Pretty much same code as beginVtol
		vehicleFile = controlGui.getVehicleNameFromID(vehicleID[vtolNum]);

//		if ( !vTol[vtolNum] )
		{
			if (vehicleID[vtolNum] != 147)		//NOT a recovery vehicle.  Standard VTOL
			{
				if (!dustCloud[commanderID] && prefs.useNonWeaponEffects)
				{
					if (strcmp(weaponEffects->GetEffectName(VTOL_DUST_CLOUD),"NONE") != 0)
					{
						//--------------------------------------------
						// Yes, load it on up.
						unsigned flags = 0;

						Check_Object(gosFX::EffectLibrary::Instance);
						gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(VTOL_DUST_CLOUD));

						if (gosEffectSpec)
						{
							dustCloud[commanderID] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
							gosASSERT(dustCloud[commanderID] != NULL);

							MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
						}
					}
				}
			}
			else						//IS a recovery vehicle.  DO NOT create VTOL, use KARNOV instead.  NO Vehicle created either
			{
				mechRecovered[commanderID] = false;	//Need to know when mech is done so Karnov can fly away.

				if (!dustCloud[commanderID] && prefs.useNonWeaponEffects)
				{
					if (strcmp(weaponEffects->GetEffectName(KARNOV_DUST_CLOUD),"NONE") != 0)
					{
						//--------------------------------------------
						// Yes, load it on up.
						unsigned flags = 0;

						Check_Object(gosFX::EffectLibrary::Instance);
						gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(KARNOV_DUST_CLOUD));

						if (gosEffectSpec)
						{
							dustCloud[commanderID] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
							gosASSERT(dustCloud[commanderID] != NULL);

							MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
						}
					}
				}

				if (!recoveryBeam[commanderID])
				{
					if (strcmp(weaponEffects->GetEffectName(KARNOV_RECOVERY_BEAM),"NONE") != 0)
					{
						//--------------------------------------------
						// Yes, load it on up.
						unsigned flags = 0;

						Check_Object(gosFX::EffectLibrary::Instance);
						gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(KARNOV_RECOVERY_BEAM));

						if (gosEffectSpec)
						{
							recoveryBeam[commanderID] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
							gosASSERT(recoveryBeam[commanderID] != NULL);

							MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
						}
					}
				}
			}
		}

		//Should we play another sample to let them know what is coming in after the load?
		//For now, no!
		/*
		switch (vehicleID)
		{
			case 120:	//Minelayer
				soundSystem->playSupportSample(SUPPORT_MINELAYER);
			break;

			case 182:	//Repair
				soundSystem->playSupportSample(SUPPORT_REPAIR);
			break;

			case 221:	//Scout
				soundSystem->playSupportSample(SUPPORT_SCOUT);
			break;

			case 147:	//Recovery
				soundSystem->playSupportSample(SUPPORT_RECOVER);
			break;

			case 415:	//Artillery
				soundSystem->playSupportSample(SUPPORT_RECOVER);
			break;
		}
		*/

		if (vehicleID[commanderID] != 147)
			soundSystem->playDigitalSample(VTOL_ANIMATE,vPos[commanderID]);
		else
			soundSystem->playDigitalSample(SALVAGE_CRAFT,vPos[commanderID]);

		Stuff::Vector3D newPos = vPos[commanderID];
		float rotation = 0.f;  // this needs to be pointing 180 degrees from drop point

		vTol[commanderID]->setObjectParameters( newPos, 0, false, Team::home->id, 0);
		//eye->update();
		vTol[commanderID]->update();
		vTol[commanderID]->setInView( true );
		vTol[commanderID]->setVisibility( 1, 1 );
		vTol[commanderID]->rotation = rotation;

		if (dustCloud[commanderID])
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;

			Stuff::Vector3D dustPos = vTol[commanderID]->position;
			Stuff::Point3D wakePos;
			wakePos.x = -dustPos.x;
			wakePos.y = dustPos.z;
			wakePos.z = dustPos.y;

			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(wakePos);

			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);

			dustCloud[commanderID]->SetLoopOff();
			dustCloud[commanderID]->SetExecuteOn();
			dustCloud[commanderID]->Start(&info);
		}

		paintingVtol[commanderID] = 1;
		vTolSpeed = -75.f;

		vehicleDropped[commanderID] = false;
		mechRecovered[commanderID] = false;

		controlGui.disableAllVehicleButtons();
	}
}

void MissionInterfaceManager::updateRollovers()
{
/*	For all (both LOS and non-LOS) the run cursors, use strings 45149/45150
	For all the jump cursors, use strings 45151/45152
	For all the walk waypoint cursors, use strings 45153/45154
	For all the run waypoint cursors, use strings 45155/45156
	For all the jump waypoint cursors, use strings 45157/45158
	For all the patrol path cursors, use strings 45159/45160
	For all the standard attack cursors, use strings 45161/45162
	For all the ammo conservation cursors, use strings 45163/45164
	For all the fire from current position cursors, use strings 45165/45166
	For all the short-range attack cursors, use strings 45167/45168
	For all the medium-range attack cursors, use strings 45169/45170
	For all the long-range attack cursors, use strings 45171/45172
	For all the force-fire attack cursors, use strings 45173/45174
	For the guard cursor, use strings 45147/45148 (one each for right-click and left-click models).*/


	long cursor = userInput->getMouseCursor();

	if ( ( cursor >= mState_RUN1 && cursor <= mState_RUN1 + 64) )
	{
		helpTextID = 0;
		controlGui.setRolloverHelpText( IDS_RUN_CURSOR_LEFT_HELP );
	}

	else if ( ( cursor >= mState_JUMP1 && cursor <= mState_JUMP1 + 64 ) )
	{
		helpTextID = 0;
		controlGui.setRolloverHelpText( IDS_JUMP_CURSOR_LEFT_HELP );
	}


 
	else
	{


		switch (cursor)
		{
			case mState_GUARD:
				if ( target )
				{
					helpTextID = 0;
					controlGui.setRolloverHelpText( IDS_GUARD_RECIPIENT_LEFT_HELP );
				}

			break;

			case mState_JUMPWAYPT:
			case mState_JUMPWAYPT_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_JUMP_WAYPOINT_CURSOR_LEFT_HELP );
				break;
			case mState_RUNWAYPT:
			case mState_RUNWAYPT_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_RUN_WAYPOINT_CURSOR_LEFT_HELP );

				break;
			case mState_WALKWAYPT:
			case mState_WALKWAYPT_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_WALK_WAYPOINT_CURSOR_LEFT_HELP );

			break;

			case mState_LINK:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_PATROL_CURSOR_LEFT_HELP );
				break;

			case mState_GENERIC_ATTACK:
			case mState_ATTACK_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_ATTACK_CURSOR_LEFT_HELP );
				break;

			case mState_ENERGY_WEAPONS:
			case mState_ENERGY_WEAPONS_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_AMMO_CONSERVE_CURSOR_LEFT_HELP );
				break;

			case mState_CURPOS_ATTACK:
			case mState_CURPOS_ATTACK_LOS:
				helpTextID = 0;
				if ( gos_GetKeyStatus( (gosEnum_KeyIndex)commands[FORCE_FIRE_KEY].key ) == KEY_HELD )
					controlGui.setRolloverHelpText(  IDS_FORCE_FIRE_CURSOR_LEFT_HELP );
				else
					controlGui.setRolloverHelpText( IDS_FIRE_FROM_CURRENT_POSITION_CURSOR_LEFT_HELP );
				break;

			case mState_SHRTRNG_ATTACK:
			case mState_SHRTRNG_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_SHORT_RANGE_CURSOR_LEFT_HELP );
				break;

			case mState_MEDRNG_ATTACK:
			case mState_MEDRNG_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_MEDIUM_RANGE_CURSOR_LEFT_HELP );
				break;
			
			case mState_LONGRNG_ATTACK:
			case mState_LONGRNG_LOS:
				helpTextID = 0;
				controlGui.setRolloverHelpText( IDS_LONG_RANGE_CURSOR_LEFT_HELP );
				break;



		}
	}

}

//--------------------------------------------------------------------------------------

// M1.6 + M2-pre: env-gated static-prop pick CALLER WRAPPER.
//
// The M1.6 inline machinery (substrate gate, gesture gates, mover gate,
// viewport query, off-screen guard, coord scaling, lookupAtPixel call)
// migrated to code/gameplay_pick.cpp at M2-pre T1+T2 as
// tryGameplayPick(). This function is now a thin caller wrapper that:
//   1. Applies the category gate (IsStaticPropPickEnabled) -- STAYS in
//      caller; M2.6 mech-pickup will have its own category gate.
//   2. Builds a GameplayPickRequest from the 7 input params.
//   3. Dispatches to tryGameplayPick(req).
//   4. Switches on result.outcome to do category-specific side effects
//      (debug-state mutation + [GAMEPLAY_PICK v1] kind=StaticProp hit/miss logs).
//
// Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md
// Sections 4 (algorithm), 6 (caller responsibilities), 11 (Section 11
// invariant preserved via Outcome::gated no-op).
//
// Original M1.6 spec still authoritative for behavior:
// docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md
// Sections 3 (detection condition), 4 (fallback order), 5 (y-flip), 11
// (forbidden behaviors), Q6 (4-site mover observable), Q8 (legacy
// preservation invariant).
void MissionInterfaceManager::tryStaticPropPick(bool moverSelectedThisFrame,
                                                bool shiftDn,
                                                bool leftClicked,
                                                bool bGui,
                                                bool bLeftDouble,
                                                int  mouseX,
                                                int  mouseY)
{
    // Category gate STAYS in caller. Fast path: env-OFF default. Two
    // cached bools; no per-frame getenv. M2.6 mech-pickup will gate on
    // its own IsMechPickEnabled() before dispatching to the spine.
    if (!RenderWorld::IsStaticPropPickEnabled())
        return;

    // Build request from the 7 missiongui-side inputs. All shared
    // machinery (substrate gate, gesture gates, mover gate, viewport
    // query, off-screen guard, coord scaling, pixel readback) lives in
    // the spine now.
    GameplayPickRequest req;
    req.mouseX                  = mouseX;
    req.mouseY                  = mouseY;
    req.shiftDn                 = shiftDn;
    req.leftClicked             = leftClicked;
    req.bGui                    = bGui;
    req.bLeftDouble             = bLeftDouble;
    req.moverSelectedThisFrame  = moverSelectedThisFrame;

    const GameplayPickResult r = tryGameplayPick(req);

    switch (r.outcome) {
    case GameplayPickResult::Outcome::hit: {
        // M2.6 latent-bug fix: post-M2.5, the substrate may return a
        // Mech handle. The static-prop caller MUST guard before
        // consuming -- a Mech handle has no recipe index, and the M2.6
        // mech caller (tryMechPick) owns mech-kind hits.
        if (r.lookup.kind != RenderWorld::RenderObjectKind::StaticProp) {
            // Not our category. Silent skip; the mech caller (or future
            // terrain/VFX caller) handles its own kinds.
            break;
        }
        // Update RenderWorld debug state. Single-slot; latest wins.
        RenderWorld::setLastGameplayPick(RenderWorld::RenderObjectKind::StaticProp,
                                         r.lookup,
                                         r.ctx.mouseX, r.ctx.mouseY,
                                         r.ctx.glX,    r.ctx.glY);
        // Sample back the unified debug-state struct so the log can
        // include the recipeIndex (LookupResult itself does not carry
        // it; the recipe lookup is done inside setLastGameplayPick).
        const RenderWorld::GameplaySelectionDebugState picked =
            RenderWorld::getLastGameplayPick();
        // Unified hit log (META-FIX of the M1.6 static-prop schema);
        // coord-diag fields BYTE-IDENTICAL to M1.6 to keep the
        // user-driven canary semantically stable across rename.
        std::fprintf(stderr,
            "[GAMEPLAY_PICK v1] hit kind=StaticProp handle=%u idx=%u gen=%u "
            "recipe=%d screen=(%d,%d) gl=(%d,%d) fbo=(%d,%d) "
            "vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)\n",
            r.lookup.handle.bits,
            (unsigned)r.lookup.handle.index(),
            (unsigned)r.lookup.handle.generation(),
            (int)picked.recipeIndex,
            r.ctx.mouseX, r.ctx.mouseY,
            r.ctx.glX,    r.ctx.glY,
            r.ctx.fboX,   r.ctx.fboY,
            r.ctx.vMulX,  r.ctx.vMulY,
            r.ctx.vAddX,  r.ctx.vAddY,
            r.ctx.drawableWidth, r.ctx.drawableHeight);
        break;
    }
    case GameplayPickResult::Outcome::miss: {
        // Clear the unified debug-state struct on empty Shift+click so
        // a stale prior pick does not survive an empty-click gesture.
        RenderWorld::clearLastGameplayPick();
        // Verbose miss log only when MC2_STATIC_PROP_PICK_DEBUG=1;
        // coord-diag BYTE-IDENTICAL to M1.6 modulo the schema prefix.
        if (RenderWorld::IsStaticPropPickDebugEnabled()) {
            std::fprintf(stderr,
                "[GAMEPLAY_PICK v1] miss kind=StaticProp screen=(%d,%d) gl=(%d,%d) "
                "fbo=(%d,%d) vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) "
                "draw=(%d,%d)\n",
                r.ctx.mouseX, r.ctx.mouseY,
                r.ctx.glX,    r.ctx.glY,
                r.ctx.fboX,   r.ctx.fboY,
                r.ctx.vMulX,  r.ctx.vMulY,
                r.ctx.vAddX,  r.ctx.vAddY,
                r.ctx.drawableWidth, r.ctx.drawableHeight);
        }
        break;
    }
    case GameplayPickResult::Outcome::gated:
    case GameplayPickResult::Outcome::skipped:
        // Section 11 invariant: no-op, no log. The legacy path either
        // already consumed this click (gated) or a gesture/substrate
        // gate failed (skipped) before any lookup ran.
        break;
    }
}

void MissionInterfaceManager::tryMechPick(bool moverSelectedThisFrame,
                                          bool shiftDn,
                                          bool leftClicked,
                                          bool bGui,
                                          bool bLeftDouble,
                                          int  mouseX,
                                          int  mouseY)
{
    // M2.6: category gate. Master enable for mech pick wiring.
    if (!RenderWorld::IsMechPickEnabled())
        return;

    // Build the same shape of request as tryStaticPropPick. The spine
    // (tryGameplayPick) is shared and unchanged.
    GameplayPickRequest req;
    req.mouseX                  = mouseX;
    req.mouseY                  = mouseY;
    req.shiftDn                 = shiftDn;
    req.leftClicked             = leftClicked;
    req.bGui                    = bGui;
    req.bLeftDouble             = bLeftDouble;
    req.moverSelectedThisFrame  = moverSelectedThisFrame;

    const GameplayPickResult r = tryGameplayPick(req);

    switch (r.outcome) {
    case GameplayPickResult::Outcome::hit: {
        // Kind guard: M2.6 caller only handles Mech kind. Static-prop
        // and future terrain/VFX kinds are owned by their callers.
        if (r.lookup.kind != RenderWorld::RenderObjectKind::Mech)
            break;

        // Reverse-resolve to BattleMech via the M2.6 linear-scan resolver.
        BattleMech* bm =
            GameAdapters::Mech::findMechByHandle(r.lookup.handle);
        if (bm == nullptr) {
            // Stale handle race (mech destroyed between readback and
            // resolver) -- impossible in practice (single-threaded), but
            // defensive design treats it as a benign miss for the click.
            if (RenderWorld::IsMechPickDebugEnabled()) {
                std::fprintf(stderr,
                    "[GAMEPLAY_PICK v1] miss kind=Mech reason=stale_handle "
                    "handle=%u idx=%u gen=%u screen=(%d,%d) gl=(%d,%d)\n",
                    r.lookup.handle.bits,
                    (unsigned)r.lookup.handle.index(),
                    (unsigned)r.lookup.handle.generation(),
                    r.ctx.mouseX, r.ctx.mouseY,
                    r.ctx.glX,    r.ctx.glY);
            }
            break;
        }

        // Fog-of-war gate. Mirrors the FULL CPU-pick predicate at
        // code/missiongui.cpp:1272-1278 verbatim:
        //   target->isMover() &&
        //   !ShowMovers &&
        //   !(MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]) &&
        //   target->getTeamId() != Team::home->getId() &&
        //   !target->isDisabled() &&
        //   ((Mover*)target)->conStat < CONTACT_SENSOR_QUALITY_1
        // i.e. fog SUPPRESSES the pick ONLY when all five hold; ShowMovers
        // (debug) and the multiplayer-defeat carve-out are the bypasses
        // the CPU pick honors and that M2.6 MUST mirror or the inspect
        // log will be silent on a mech the CPU pick successfully selects.
        //
        // MC2_MECH_PICK_PIERCE_FOG=1 short-circuits the whole predicate.
        const bool pierce = RenderWorld::IsMechPickPierceFogEnabled();
        bool visible;
        if (pierce) {
            visible = true;
        } else {
            const bool showMovers = (ShowMovers != 0);
            const bool mpDefeat   = (MPlayer && MPlayer->allUnitsDestroyed[MPlayer->commanderID]);
            const bool hostile    = (bm->getTeamId() != Team::home->getId());
            const bool disabled   = bm->isDisabled();
            const bool sub_q1     = (((Mover*)bm)->conStat < CONTACT_SENSOR_QUALITY_1);
            const bool fogSuppresses =
                !showMovers && !mpDefeat && hostile && !disabled && sub_q1;
            visible = !fogSuppresses;
        }
        if (!visible) {
            // Fog-gated; silent on default path so the substrate cannot
            // become a sensor cheat. Diagnostic log under DEBUG=1.
            if (RenderWorld::IsMechPickDebugEnabled()) {
                std::fprintf(stderr,
                    "[GAMEPLAY_PICK v1] gated kind=Mech reason=fog_of_war "
                    "handle=%u screen=(%d,%d) gl=(%d,%d)\n",
                    r.lookup.handle.bits,
                    r.ctx.mouseX, r.ctx.mouseY,
                    r.ctx.glX,    r.ctx.glY);
            }
            break;
        }

        // Visible hit. Inspect-only: update unified debug state + log.
        RenderWorld::setLastGameplayPick(RenderWorld::RenderObjectKind::Mech,
                                         r.lookup,
                                         r.ctx.mouseX, r.ctx.mouseY,
                                         r.ctx.glX,    r.ctx.glY);
        // Log line carries handle bits/index/generation + the resolved
        // BattleMech pointer (debug; not a stable cookie). NO
        // gameObjectId / partId fields per CRITICAL-1.
        std::fprintf(stderr,
            "[GAMEPLAY_PICK v1] hit kind=Mech handle=%u idx=%u gen=%u "
            "mech=%p screen=(%d,%d) gl=(%d,%d) "
            "fbo=(%d,%d) vMul=(%.0f,%.0f) vAdd=(%.0f,%.0f) draw=(%d,%d)%s\n",
            r.lookup.handle.bits,
            (unsigned)r.lookup.handle.index(),
            (unsigned)r.lookup.handle.generation(),
            (void*)bm,
            r.ctx.mouseX, r.ctx.mouseY,
            r.ctx.glX,    r.ctx.glY,
            r.ctx.fboX,   r.ctx.fboY,
            r.ctx.vMulX,  r.ctx.vMulY,
            r.ctx.vAddX,  r.ctx.vAddY,
            r.ctx.drawableWidth, r.ctx.drawableHeight,
            pierce ? " pierce_fog=1" : "");
        break;
    }
    case GameplayPickResult::Outcome::miss: {
        // Mech caller silent on plain miss (background-pixel Shift+click
        // is high-frequency for terrain but uninteresting for mechs).
        // Verbose miss only under MC2_MECH_PICK_DEBUG=1.
        if (RenderWorld::IsMechPickDebugEnabled()) {
            std::fprintf(stderr,
                "[GAMEPLAY_PICK v1] miss kind=Mech screen=(%d,%d) gl=(%d,%d)\n",
                r.ctx.mouseX, r.ctx.mouseY,
                r.ctx.glX,    r.ctx.glY);
        }
        break;
    }
    case GameplayPickResult::Outcome::gated:
    case GameplayPickResult::Outcome::skipped:
        // Mover-first short-circuit consumed the click (gated) or a
        // gesture/substrate gate failed (skipped). No-op, no log.
        break;
    }
}
