//--------------------------------------------------------------------------------------
//
// MechCommander 2 -- Copyright (c) 1998 FASA Interactive
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

#ifndef MISSIONGUI_H
#define MISSIONGUI_H
#include <memory>
//--------------------------------------------------------------------------------------
// Include Files
#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef CONTROLGUI_H
#include"controlgui.h"
#endif


class KeyboardRef;
enum COMMAND_KEY_INDEX
{
	ENERGY_WEAPON_INDEX = 3,
	GUARD_COMMAND_INDEX = 8,
	FORCE_FIRE_KEY = 11,
	EJECT_COMMAND_INDEX = 27,
	OBJECTVIES_COMMAND_KEY = 59,
	INFO_COMMAND_INDEX = 64
	
	
};
//--------------------------------------------------------------------------------------
// Externs
void *gMalloc (long size);
void gFree (void *me);

inline constexpr int MAX_COMMAND = 107;
//--------------------------------------------------------------------------------------
struct UiCmdCtx;

class MissionInterfaceManager
{
	//-------------------------------------------------------------------
	// This baby runs it all for the Missions
	//
	// It has an update which is called every frame.
	//
	// It has an init which takes a FIT file and creates the interface
	// from the .FIT file.  This allows artists to correctly setup
	// the interface for each screen using data files.
	//
	// Its also has a render which is called AFTER the camera render
	// so that the interface ALWAYS sits on top of the world.
	//
	// It has an event constructor to create events and pass them down
	// the interface chain.
	//
	// Essentially, this is the iface of MechCommander 2

public:

	typedef int (MissionInterfaceManager::*COMMAND)( void );

	struct Command
		{
			long	key;
			long	cursor;
			long	cursorLOS;
			bool	singleClick;
			COMMAND function;
			COMMAND	releaseFunction;
			long	hotKeyDescriptionText;
		};

	static Command*		getCommands() { return commands; }
	static long*		getOldKeys() { return OldKeys; }
	
	protected:


	   static int mouseX;
	   static int mouseY;
	   static gosEnum_KeyIndex WAYPOINT_KEY;

	   static float pauseWndVelocity;

	

		static Command		commands[MAX_COMMAND];
		static long			OldKeys[MAX_COMMAND];

		long terrainLineChanged;

		//-------------------------------------------
		// Data to control scroll, rotation and zoom
		float						baseFrameLength;
		
		float						zoomInc;
		float						rotationInc;
		float						scrollInc;
		
		float						screenScrollLeft;
		float						screenScrollRight;
		float						screenScrollUp;
		float						screenScrollDown;
		
		float						realRotation;
		float						degPerSecRot;
		
		long						zoomChoice;			//Can range from 0 to 3 right now.
		
		//----------------------------------------------
		// Mouse Event Data
		bool						isDragging;
		float						vTolSpeed;
		const char*					vehicleFile;
		long						vehicleID[MAX_TEAMS];
		glm::vec3				dragStart;
		glm::vec3				dragEnd;
		bool						selectClicked;			// left click
		bool						commandClicked;			// usually right clicked
		bool						cameraClicked;
		
		ControlGui					controlGui;
		glm::vec3				wPos;					//World Position of camera cursor.

		// VPL-retirement Step 3 3b: mandatory per-frame inverseProject
		// delta-cache. update() runs every frame and calls eye->inverseProject
		// (now a frustum-AABB + forward-proj walk). Skip the walk when neither
		// the cursor pixel nor the camera matrix changed since last projection.
		long						prevMouseX;
		long						prevMouseY;
		glm::vec3				cachedWPos;
		glm::mat4				cachedWorldToClip;
		bool						inverseProjectCacheValid;
		
		//vTol
		glm::vec3				vPos[MAX_TEAMS];		// vehicle position
		std::unique_ptr<BldgAppearance>				vTol[MAX_TEAMS] = {};	// B7o.5: owned drop-ship VTOL / Karnov per team
		bool						paintingVtol[MAX_TEAMS];
		float						vTolTime[MAX_TEAMS];
		gosFX::EffectPtr				recoveryBeam[MAX_TEAMS];
		gosFX::EffectPtr				dustCloud[MAX_TEAMS];
		bool						mechRecovered[MAX_TEAMS];
		bool						vehicleDropped[MAX_TEAMS];
		MoverPtr					mechToRecover[MAX_TEAMS];
		
		bool						bPaused;
		bool						bPausedWithoutMenu;
		bool						bEnergyWeapons;
		bool						bDrawHotKeys;
		long						resolution;
		
		//-----------------------------------------------
		// Attila (Strategic Commander) Data
		float						attilaXAxis;
		float						attilaYAxis;
		float						attilaRZAxis;

		//-----------------------------------------------
		// Tutorial Data
		bool						animationRunning;
		float						timeLeftToScroll;
		long						targetButtonId;
		long						buttonNumFlashes;
		float						buttonFlashTime;
		bool						targetIsButton;
		bool						targetIsPressed;
		bool						guiFrozen;

		bool						bForcedShot;
		bool						bAimedShot;
		
		static	MissionInterfaceManager* s_instance;
				
	public:
	
		MissionInterfaceManager (void);
		
		~MissionInterfaceManager (void);

		static MissionInterfaceManager* instance(){ return s_instance; }
		
		void init (void);
		
		void init (FitIniFilePtr loader);
		void initTacMap( PacketFile* file, int packet );
	
		virtual void destroy (void);
		
		virtual void update (void);
		virtual void drawVTOL (void);
		virtual void render (void);
						
		void	initMechs(){ controlGui.initMechs(); }	

		const char* getSupportVehicleNameFromID (long ID) { return controlGui.getVehicleNameFromID(ID); }

		void	addMover (MoverPtr mover){ controlGui.addMover(mover); }
		void	removeMover (MoverPtr mover){ controlGui.removeMover(mover); }

		// META-FIX design A (MC2_MOVER_COHERENCE_AUDIT): read-only roster
		// queries for the mover-registry coherence audit. Passthroughs.
		bool	rosterHasMover (const Mover* mover) const { return controlGui.rosterHasMover(mover); }
		int 	rosterIconCount (void) const { return controlGui.rosterIconCount(); }
		Mover*	rosterIconMover (int index) const { return controlGui.rosterIconMover(index); }

		int update( bool bLeftClick, bool bRightClick, int mouseX, int mouseY, GameObject* pTarget, bool bLOS ); // check for each key in the list
		
		void	updateVTol();
		
		ControlGui *getControlGui (void)
		{
			return &controlGui;
		}

		void playMovie (char *filename)
		{
			controlGui.playMovie(filename);
		}

		int handleOrders( TacticalOrder& order );
		bool anySelectedWithoutAreaEffect (void);
		void updateWaypoints (void);
		
		void setTarget( GameObject* pTarget ){ target = pTarget; }
		// MISSION-START-HOVER-TARGET-LIFETIME-1: invalidate stale hover/pick/target caches at
		// mission begin+end; arm re-enables picking once the mission world is live.
		static void invalidateHoverTarget (void);
		static void armHoverTarget (void);
		static void selectForceGroup( int forceGroup, bool deselectOthers );
		void doRepair( GameObject* who);
		void doRepairBay( GameObject* who);
		void doEject( GameObject* who );
		bool	canRepair( GameObject* pMover );
		bool	canRepairBay ( GameObject* bay );

		void setMechRecovered (long teamID, bool set) {
			mechRecovered[teamID] = set;
		}

		void updateOldStyle(bool shiftDn, bool altDn, bool ctrlDn, bool bGui, 
			bool lineOfSight, bool passable, long moverCount, long nonMoverCount );
		void updateAOEStyle(bool shiftDn, bool altDn, bool ctrlDn, bool bGui,
			bool lineOfSight, bool passable, long moverCount, long nonMoverCount );

		// M1.6: env-gated static-prop pick helper. Called from the tail of
		// both updateOldStyle and updateAOEStyle when leftClicked && shiftDn
		// && !bGui. Short-circuits when moverSelectedThisFrame == true so
		// the legacy Shift+LMB additive-select gesture on a friendly mover
		// is preserved verbatim (no log line in that case). Emits
		// [GAMEPLAY_PICK v1] hit kind=StaticProp / miss and updates the
		// unified RenderWorld debug state.
		//
		// Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md
		void tryStaticPropPick(bool moverSelectedThisFrame,
		                       bool shiftDn,
		                       bool leftClicked,
		                       bool bGui,
		                       bool bLeftDouble,
		                       int  mouseX,
		                       int  mouseY);

		// M2.6: mech-pick consumer. Mirrors tryStaticPropPick shape;
		// dispatches through the SAME tryGameplayPick spine; kind-guards
		// on r.lookup.kind == Mech; reverse-resolves to BattleMech via
		// GameAdapters::Mech::findMechByHandle; applies fog-of-war
		// predicate (mirrors code/missiongui.cpp:1272-1278); emits
		// [GAMEPLAY_PICK v1] hit kind=Mech ... on a visible-hostile pick.
		// Inspect-only v1 (no selection, no attack routing).
		//
		// Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-6-mech-pickup-spec.md
		void tryMechPick(bool moverSelectedThisFrame,
		                 bool shiftDn,
		                 bool leftClicked,
		                 bool bGui,
		                 bool bLeftDouble,
		                 int  mouseX,
		                 int  mouseY);

		static int saveHotKeys( FitIniFile& file );
		static int loadHotKeys( FitIniFile& file );

		static int setHotKey( int whichCommand, gosEnum_KeyIndex key, bool bShift, bool bControl, bool bAlt );
		int getHotKey( int whichCommand, gosEnum_KeyIndex& newKey, bool& bShift, bool& bControl, bool& bAlt );
		bool hotKeyIsPressed( int whichCommand );

		int setWayPointKey( gosEnum_KeyIndex key );

		void setAOEStyle();
		void setMCStyle();

		bool isAOEStyle();
		bool isMCStyle();

		bool	isPaused();
		bool	isPausedWithoutMenu();
		// [VISUAL_CAPTURE v1.5] Deterministic sim-freeze for the bookmark sweep.
		// Sets/clears bPaused + bPausedWithoutMenu directly (no menu UI, no
		// sound), freezing scenarioTime + all sim updates (see mission.cpp:531).
		// getCapturePauseState() returns the (bPaused<<1 | bPausedWithoutMenu)
		// pair so the caller can fully restore the pre-sweep pause state.
		void	setPausedForCapture(bool paused);
		int		getCapturePauseState();
		void	restoreCapturePauseState(int packed);
		int		togglePause();
		int		togglePauseWithoutMenu();
		int		toggleHoldPosition();
		int		showObjectives();
		int		showObjectives(bool on);
		void	doMove(const glm::vec3& pos);
		void	doGuard(GameObject* pObj);
		int toggleHotKeys();
		void	beginVtol(long supportID, long commanderID = 0, glm::vec3* reinforcePos = NULL, MoverPtr salvageTarget = NULL);

		//Tutorial Stuff goes here.
		bool startAnimation(long buttonId,bool isButton,bool isPressed,float timeToScroll,long numFlashes);
		void setTutorialText(const char *text);

		bool isInCalloutAnimation ()
		{
			return animationRunning;
		}
		
		void freezeGUI (bool flag)
		{
			guiFrozen = flag;
		}

		//In-Mission Save
		void Save (FitIniFilePtr file);
		void Load (FitIniFilePtr file);

	private:

		// UI-COMMAND-CHANNEL-1: MC2_UI_COMMAND_FILE polling (see missiongui.cpp)
		void processUiCommandChannel();
		// One group of the verb chain per function; each returns true when a verb matched (see missiongui_uicmd*.cpp).
		bool uiCmdBasic(UiCmdCtx& c);
		bool uiCmdWorld(UiCmdCtx& c);
		bool uiCmdReadback(UiCmdCtx& c);
		bool uiCmdProbe(UiCmdCtx& c);
		bool uiCmdFixture(UiCmdCtx& c);

		void makeForceGroup( int forceGroup );
		bool moveCameraAround( bool lineOfSight, bool passable, bool ctrl, bool bGui, long moverCount, long nonMoverCount );
		bool canJump(); // selected units can jump
		bool canJumpToWPos();
		void doDrag(bool bGui);


		int attackShort();
		int attackMedium();
		int attackLong();
		int alphaStrike();
		int defaultAttack();
		int jump();
		int stopJump();
		int fireFromCurrentPos();
		int stopFireFromCurrentPos();
		int guard();
		int stopGuard();
		int conserveAmmo();
		int selectVisible();
		int addVisibleToSelection();
		int aimLeg();
		int aimArm();
		int aimHead();
		int removeCommand();
		int powerUp();
		int powerDown();
		int changeSpeed();
		int stopChangeSpeed();
		int eject();
		int forceShot();

		int scrollUp();
		int scrollDown();
		int scrollLeft();
		int scrollRight();
		int zoomOut();
		int zoomIn();
		int zoomChoiceOut();
		int zoomChoiceIn();
		// [MOUSE-ANCHORED-ZOOM-1 / FIX-4] mouse-anchored zoom wrapper (gated).
		void anchoredZoom(long mx, long my, int (MissionInterfaceManager::*zoomFn)());
		int rotateLeft();
		int rotateRight();
		int tiltUp();
		int tiltDown();
		int centerCamera();
		int rotateLightLeft();
		int rotateLightRight();
		int rotateLightUp();
		int rotateLightDown();
		int switchTab();
		int reverseSwitchTab();

		int drawTerrain();
		int drawOverlays();
		int drawBuildings();
		int showGrid();
		int recalcLights();
		int drawFog();
		int usePerspective();
		int drawWaterEffects();
		int recalcWater();
		int drawShadows();
		int changeLighting();
		int vehicleCommand();
		int toggleGUI();
		int	drawTGLShapes();
		int infoCommand();
		int infoButtonReleased();
		int energyWeapons();
		int sendAirstrike();
		int sendLargeAirstrike();
		int gotoNextNavMarker();
		int sendSensorStrike();
		
		int	handleChatKey();
		int	handleTeamChatKey();


		bool makePatrolPath();
		
		int bigAirStrike();
		int smlAirStrike();
		int snsAirStrike();
		
		int cameraNormal();
		int cameraDefault();
		int cameraMaxIn();
		int cameraMaxOut();
		int cameraTight();
		int cameraFour();
		int toggleCompass();
		
		int cameraAssign0();
		int cameraAssign1();
		int cameraAssign2();
		int cameraAssign3();

		static GameObject*	target;

		void printDebugInfo();

		void doAttack();
		void doJump();
		void doGuardTower();
		
		void doSalvage();

		int quickDebugInfo ();
		int setGameObjectWindow ();
		int pageGameObjectWindow1 ();
		int pageGameObjectWindow2 ();
		int pageGameObjectWindow3 ();
		int jumpToDebugGameObject1 ();
		int jumpToDebugGameObject2 ();
		int jumpToDebugGameObject3 ();
		int toggleDebugWins ();
		int teleport ();
		int showMovers ();
		int cullPathAreas ();
		int zeroHPrime ();
		int calcValidAreaTable ();
		int globalMapLog ();
		int brainDead ();
		int goalPlan ();
		int enemyGoalPlan ();
		int showVictim ();
		int damageObject1 ();
		int damageObject2 ();
		int damageObject3 ();
		int damageObject4 ();
		int damageObject5 ();
		int damageObject6 ();
		int damageObject7 ();
		int damageObject8 ();
		int damageObject9 ();
		int damageObject0 ();
		int rotateObjectLeft ();
		int rotateObjectRight ();

		bool	canAddVehicle( const glm::vec3& pos );
		bool	canRecover( const glm::vec3& pos );
		long	makeNoTargetCursor( bool passable, bool lineOfSight, bool ctrl, bool bGui,
			long moverCount, long nonMoverCount );
		long	makeRangeCursor( bool LOS);
		long	makeTargetCursor( bool lineOfSight, long moverCount, long nonMoverCount );
		long	makeMoveCursor( bool bLineOfSite );
		long	makeJumpCursor( bool bLineOfSite );
		long	makeRunCursor( bool bLineOfSite );
		void	updateRollovers();



		void	addAirstrike();
		void	addVehicle( const glm::vec3& pos );
		void	updateTarget(bool bCursorIsInGui);
		void	drawWayPointPaths();

		long	calcRotation();



		void	swapResolutions();
		

		bool	canSalvage( GameObject* pMover );
		bool	selectionIsHelicopters( );
		void	drawHotKey( const char* string, const char* descStr, long x, long y );
		void	drawHotKeys();



		glm::vec3 makeAirStrikeTarget( const glm::vec3& pos );

		GameObject*		oldTargets[MAX_ICONS];

		aFont			hotKeyFont;
		aFont			hotKeyHeaderFont;

		bool			lastUpdateDoubleClick;

		float			swapTime;

		std::unique_ptr<KeyboardRef>	keyboardRef;

		MoverPtr		reinforcement;
};

typedef MissionInterfaceManager *MissionInterfaceManagerPtr;

//--------------------------------------------------------------------------------------
#endif