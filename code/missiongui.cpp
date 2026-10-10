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

#include "missiongui_shared_internal.h"


	const char* terrainStr[NUM_TERRAIN_TYPES] = {
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

float MissionInterfaceManager::pauseWndVelocity = -50.f;

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
int           s_hoverLastMouseX = -1;
int           s_hoverLastMouseY = -1;
GameObject*   s_hoverCachedTarget = nullptr;
unsigned long s_hoverCachedTargetWID = 0;   // watch-ID of the cached hover target — validate by handle before any deref of the raw pointer
bool          s_hoverCacheValid  = false;
// Generation stamp: bumped on every mission begin/end invalidate. A cached target is only
// trusted while its stamp matches; any boundary crossing discards it cheaply.
unsigned      s_hoverMissionSerial = 0;
// Suppress hover picking until the mission is fully live (objects loaded). Set true by
// invalidateHoverTarget() at mission END (and at init), cleared at mission BEGIN once the
// world is ready. While true updateTarget() is a no-op for picking.
bool          s_hoverSuppressed = true;
// After arming, keep picking OFF for a few live frames: the first updates of a mission can
// still pick a not-yet-settled world (findObjectByMouse returns a stale/garbage pointer).
long          s_hoverSettleFrames = 0;
// Watch-IDs of the persistent target + oldTargets, captured when each was set to a LIVE
// object. updateTarget re-validates via ObjectManager->getByWatchID() (registry lookup, no
// deref) before touching them, so a target whose object DIED since last frame is dropped,
// not crashed-on. MAX_ICONS == oldTargets[] size (forcegroupbar.h).
unsigned long  s_targetWatchID = 0;
unsigned long  s_oldTargetWatchID[MAX_ICONS] = {0};
bool drawGUIOn = true;				//Used to shut off GUI for Screen Shots and Movie Mode
bool toggledGUI = true;				//Used to tell if I shut the GUI off or if its in movieMode
long lightState = 0;
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
				mc2ToRender(wakePos, dustPos);
				
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
				mc2ToRender(wakePos, dustPos);
				
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
				mc2ToRender(objPosition, wPos);
		
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
	{ GosGlLostDrawsScope _lostSc_("hud.control_gui_tacmap"); controlGui.render( isPaused() && !isPausedWithoutMenu() ); }
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
