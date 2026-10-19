#ifndef CONTROLGUI_H
#define CONTROLGUI_H
/*************************************************************************************************\
ControlGui.h			: Interface for the ControlGui component.  This thing holds the tac map
							and everything else on the left hand side of the screen.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef GAMETACMAP_H
#include <memory>
#include"gametacmap.h"
#endif

#ifndef FORCEGROUPBAR_H
#include"forcegroupbar.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef UTILITIES_H
#include"utilities.h"
#endif

#ifndef PAUSEWINDOW_H
#include"pausewindow.h"
#endif

#ifndef AFONT_H
#include"afont.h"
#endif

#ifndef AEDIT_H
#include"aedit.h"
#endif

#ifndef MC2movie_H
#include"mc2movie.h"
#endif

#ifndef MPSTATS_H
#include"mpstats.h"
#endif

#ifndef HUDTABREGISTRY_H
#include"hudtabregistry.h"   // slice C: MC2_HUD_IMGUI_TABS tab registry
#endif
class CObjective;
class InfoWindow;
class PauseWindow;

// OBJ-IMGUI-SLICE-1: one display row of the objectives overlay, fully resolved
// by the shared ControlGui::gatherObjectiveRows so the legacy and ImGui draw
// paths enumerate/color objectives through the SAME logic and can never drift.
struct ObjectiveUiRow
{
	CObjective*	obj;		// live objective (description read at draw time)
	bool		primary;	// 1 == Priority()
	long		color;		// ARGB status color (incl. activation flash)
	bool		succeeded;	// Status == OS_SUCCESSFUL
	bool		failed;		// Status == OS_FAILED
	long		points;		// ResourcePoints()
};
//*************************************************************************************************


#define LAST_VEHICLE MAX_VEHICLE - LARGE_AIRSTRIKE
inline constexpr int OBJECTVE_MOVE_COUNT = 2;
inline constexpr int RESULTS_MOVE_COUNT = 3;
inline constexpr int MAX_CHAT_COUNT = 5;
inline constexpr int RPTOTAL_CALLOUT = 21185;
/**************************************************************************************************
CLASS DESCRIPTION
ControlGui:
**************************************************************************************************/


struct	ButtonData
{
	long			ID;
	long			helpTextHeader;
	long			helpTextID;
	long			textID;
	long			textColors[4];
	aFont			textFont;

	char			fileName[32];
	long			stateCoords[4][2];	
	long			textureWidth;
	long			textureHeight;
	int				fileWidth;
	int				fileHeight;
	unsigned long	textureHandle;
	bool			textureRotated;
	
};

class ControlButton
{
	public:
	gos_VERTEX		location[4];
	long ID;

	ButtonData*			data;
	int					state;

	void render();
	void press( bool );
	void toggle();
	void disable( bool );
	bool isEnabled();
	void makeAmbiguous( bool bAmbiguous );
	void hide( bool );
	void move( float deltaX, float deltaY );
	void setColor( unsigned long newColor );
	static void makeUVs( gos_VERTEX* vertices, int State, ButtonData& data );

	static void initButtons( FitIniFile& file, long buttonCount, 
			ControlButton* buttons, ButtonData* buttonData, const char* str, aFont* font = 0 );


	enum States
	{
		ENABLED = 0,
		PRESSED,
		DISABLED,
		AMBIGUOUS,
		HIDDEN
	};

};


class ControlGui
{
	public:

		static ControlGui*	instance;

		static long			hiResOffsetX;
		static long			hiResOffsetY;

		ControlGui();
		~ControlGui();
		bool inRegion( int mouseX, int mouseY, bool bPaused );
		void render( bool bPaused );
		void update( bool bPaused, bool bLOS );
		void initTacMapBuildings( unsigned char* data, int size ){ tacMap.initBuildings( data, size ); }
		void initTacMap( unsigned char* data, int size ){ tacMap.init( data, size ); }
		void initMechs();
		void unPressAllVehicleButtons();
		void disableAllVehicleButtons();
		void addMover (MoverPtr mover);
		void removeMover (MoverPtr mover);

		// META-FIX design A (MC2_MOVER_COHERENCE_AUDIT): read-only roster
		// queries surfaced for the mover-registry coherence audit
		// (code/mission.cpp auditMoverCoherence). Pure passthroughs to
		// ForceGroupBar — no state change.
		bool   rosterHasMover (const Mover* mover) const { return forceGroupBar.hasMover(mover); }
		int    rosterIconCount (void) const { return const_cast<ForceGroupBar&>(forceGroupBar).getIconCount(); }
		Mover* rosterIconMover (int index) const { return forceGroupBar.getIconMover(index); }

		int  updateChat();

		void beginPause();
		void endPause();

		bool resultsDone();

		void startObjectives( bool bStart );
		bool objectivesStarted() { return renderObjectives; }

		void setInfoWndMover( Mover* pMover );
		void setVehicleCommand( bool );
		bool getVehicleCommand( );

		void playMovie( const char* fileName );
		bool isMoviePlaying();

		bool playPilotVideo( MechWarrior* pPilot, char movieCode );
		void endPilotVideo();
		bool isSelectingInfoObject();

		bool isOverTacMap();

		bool isChatting() { return bChatting; }
		
		//TUTORIAL
		bool animateTacMap (int buttonId,float timeToScroll,long numFlashes);
		bool pushButton (long buttonId);
		bool flashRPTotal (long numFlashes);

		const char* getVehicleName( long& ID );
		const char* getVehicleNameFromID (long ID);
		void swapResolutions( int newResolutionX, int resolutionY );

		GameTacMap						tacMap;
		ForceGroupBar					forceGroupBar;

		bool				mouseInVehicleStopButton;

		enum Commands
		{
			DEFAULT_RANGE = 0,
			SHORT_RANGE ,
			MED_RANGE,
			LONG_RANGE,
			JUMP_COMMAND,
			RUN_COMMAND,
			GUARD_COMMAND,
			FIRE_FROM_CURRENT_POS,
			STOP_COMMAND,
			INFO_COMMAND,
			OBJECTIVES_COMMAND,
			SAVE_COMMAND,
			TACMAP_TAB,
			INFO_TAB,
			VEHICLE_TAB,
			LAYMINES,
			REPAIR,
			SALVAGE,
			GUARDTOWER,
			CUR_RANGE, // fire from right where I AM
			LAST_COMMAND

		};

		enum VehicleCommands
		{
			LARGE_AIRSTRIKE = 100,
			GUARD_TOWER,
			SENSOR_PROBE,
			REPAIR_VEHICLE,
			PEGASUS_SCOUT,
			MINELAYER,
			RECOVERY_TEAM,	
			STOP_VEHICLE,
			MAX_VEHICLE
		};

		bool	isDefaultSpeed();
		void	toggleDefaultSpeed( );
		void	toggleJump( );
		bool	getJump( );
		bool	getWalk( );
		bool	getRun( );
		void	toggleGuard( );
		bool	getGuard( );
		void	setDefaultSpeed();
		void	toggleHoldPosition();

		void	setRange( int Range );
		void	doStop();
		void	toggleFireFromCurrentPos();
		bool	getFireFromCurrentPos(){ return fireFromCurrentPos; }
		void    setFireFromCurrentPos( bool bset ) { fireFromCurrentPos = bset; }
		bool	isAddingVehicle(){ return addingVehicle; }
		bool	isAddingAirstrike() { return addingArtillery; }
		bool	isAddingSalvage() { return addingSalvage; }
		bool	isButtonPressed( int ID ) { return getButton( ID )->state & ControlButton::PRESSED; }
		bool	getMines();
		bool	getSalvage();
		bool	getRepair();
		bool	getGuardTower();
		void	switchTabs( int direction );

		// ---- slice C: data-driven tab registry (MC2_HUD_IMGUI_TABS, default OFF).
		// Gate OFF -> registry never consulted, every legacy branch runs unchanged.
		bool	hudTabsGateOn() const;           // main gate + not pinned-to-legacy
		void	initHudTabRegistry();            // (re)registers the stock 3 (+TEST)
		void	setActiveHudTab( int index, const char* via );  // syncs button PRESSED bits
		HudTabs::Registry	hudTabRegistry;
		void	renderObjective( const ObjectiveUiRow& row, long xPos, long yPos, bool bDrawTotal );
		// OBJ-IMGUI-SLICE-1: shared objective-row gather (legacy + ImGui paths).
		int		gatherObjectiveRows( ObjectiveUiRow* rows, int maxRows );
		static bool	imguiObjEnabled();
		void	renderObjectivesImGui();
		// SMALLPANELS-IMGUI-SLICE-1: mission-timer + comm-video panels.
		static bool	imguiTimerEnabled();
		static bool	imguiVideoEnabled();
		void	renderTimerImGui( const char* buffer, unsigned long color );
		void	renderVideoImGui();
		void	renderMissionStatus( bool bRender){ renderStatusInfo = bRender; }		
		
		int		getCurrentRange();
		void	pressInfoButton( ){ handleClick( INFO_COMMAND ); }
		bool	infoButtonPressed() { return getButton( INFO_COMMAND )->state & ControlButton::PRESSED; }

		void	showServerMissing();

		void	pressAirstrikeButton() 
		{ 
			for ( int i = 0; i < LAST_VEHICLE; i++ )
			{
				if (( vehicleButtons[i].ID == LARGE_AIRSTRIKE) &&
					!( vehicleButtons[i].state & ControlButton::PRESSED ))
				{
					handleVehicleClick( LARGE_AIRSTRIKE ); 
				}
			}
		}		
		void	pressLargeAirstrikeButton() { handleVehicleClick( LARGE_AIRSTRIKE ); }		
		void	pressSensorStrikeButton() 
		{ 
			for ( int i = 0; i < LAST_VEHICLE; i++ )
			{
				if (( vehicleButtons[i].ID == SENSOR_PROBE) &&
					!( vehicleButtons[i].state & ControlButton::PRESSED ))
				{
					handleVehicleClick( SENSOR_PROBE ); 
				}
			}
		}		

		void	setRolloverHelpText( unsigned long textID );


		void	setChatText( const char* playerName, const char* message, unsigned long backgroundColor, 
			unsigned long textColor );
		void	toggleChat( bool setTeamOnly );
		void eatChatKey();
		void	cancelInfo();
		
		ControlButton*		getButton( int ID );

		struct RectInfo
		{
			GUI_RECT rect;
			long color;
		};

	private:

		struct ChatInfo
		{
			char playerName[32];
			char message[128];
			unsigned long backgroundColor;
			unsigned long time;
			unsigned long messageLength; // number of lines
			unsigned long chatTextColor;
		};

		ChatInfo		chatInfos[MAX_CHAT_COUNT]; // max five lines -- could change

		std::unique_ptr<RectInfo[]>		rectInfos;
		long			rectCount;
		
		//static	ButtonFile		vehicleFileData[LAST_VEHICLE];
		static unsigned long RUN;
		static unsigned long WALK;
		static unsigned long GUARD;
		static unsigned long JUMP;

		std::unique_ptr<ControlButton[]>		buttons;
		std::unique_ptr<ControlButton[]>		vehicleButtons;
		static ButtonData*	buttonData;
		static ButtonData*	vehicleData;
		static const char*	vehicleNames[5];
		static long			vehicleIDs[5];
		static const char*	vehiclePilots[5];


		static long			vehicleCosts[LAST_VEHICLE];

		std::unique_ptr<InfoWindow>			infoWnd;
		std::unique_ptr<PauseWindow>		pauseWnd;

		std::unique_ptr<StaticInfo[]>			staticInfos;
		long				staticCount;

		std::unique_ptr<StaticInfo[]>			objectiveInfos; //2nd to last one is check, last is x
		long				objectiveInfoCount;

		std::unique_ptr<StaticInfo[]>			missionStatusInfos;
		long				missionStatusInfoCount;
		RectInfo			missionStatusRect;

		bool				renderStatusInfo;
		float				resultsTime;
		bool				renderObjectives;
		float				objectiveTime;
		
		float				tabFlashTime;
		static long OBJECTIVESTOP;
		static long OBJECTIVESLEFT;
		static long OBJECTIVESSKIP;
		static long OBJECTIVESTOTALRIGHT;
		static long OBJEECTIVESHEADERSKIP;
		static long OBJECTIVESHEADERTOP;
		static long OBJECTIVEBOXX;
		static long OBJECTIVEBOXSKIP;
		static long OBJECTIVECHECKSKIP;
		static long OBJECTIVEHEADERLEFT;
		static long HELPAREA_LEFT;
		static long HELPAREA_BOTTOM;
		static long RPLEFT;
		static long RPTOP;

		static MoveInfo objectiveMoveInfo[OBJECTVE_MOVE_COUNT];
		static MoveInfo missionResultsMoveInfo[RESULTS_MOVE_COUNT];

		std::unique_ptr<StaticInfo[]>			videoInfos;
		long				videoInfoCount;
		GUI_RECT			videoRect;
		GUI_RECT			videoTextRect;
		MC2MoviePtr			bMovie;

		std::unique_ptr<StaticInfo[]>			timerInfos;
		long				timerInfoCount;
		RectInfo			timerRect;

		//TUTORIAL!!
		RectInfo			rpCallout;
		long				rpNumFlashes;
		float				rpFlashTime;
		long				buttonToPress;

		unsigned long		curOrder;
		bool				fireFromCurrentPos;
		bool				addingVehicle;
		bool				addingArtillery;
		bool				addingSalvage;
		bool				wasLayingMines;

		bool				moviePlaying;
		bool				twoMinWarningPlayed;
		bool				thirtySecondWarningPlayed;
		bool				bChatting;

		long					idToUnPress;
		aFont					guiFont;
		aFont					helpFont;
		aFont					vehicleFont;
		aFont					timerFont;
		aFont					missionResultsFont;
		
		aText					chatEdit;
		aEdit					playerNameEdit;
		aEdit					personalEdit;

		void handleClick( int ID );

		// slice C: registry render/update thunks (static members so they can
		// reach private renderInfoTab/renderVehicleTab; user == ControlGui*).
		static void hudTabRenderTacMap( void* user );
		static void hudTabRenderInfo( void* user );
		static void hudTabRenderVehicle( void* user );
		static void hudTabUpdateTacMap( void* user );
		static void hudTabUpdateInfo( void* user );
		static void hudTabRenderTest( void* user );   // MC2_HUD_TABS_TEST witness tab
		static void hudTabRenderSimSpeed( void* user ); // MC2_HUD_SIM_SPEED speed tab (SIM-SPEED-BUTTON-1)
		void renderHudTabChromeImGui();               // tab-strip chrome for buttonCommand<0 tabs

		void updateVehicleTab( int mouseX, int mouseY, bool bLOS );
		void renderVehicleTab();
		void RenderObjectives();
		void renderResults();
		void handleVehicleClick( int ID );
		void renderHelpText();
		void renderInfoTab();
		void renderChatText();

		void initStatics( FitIniFile& file );
		void initRects( FitIniFile& file );

		void renderPlayerStatus( float delta);

		MPStatsEntry			mpStats[9];

		bool					chatIsTeamOnly;
		bool					bServerWarningShown;

		public:

		// UI-COMMAND-CHANNEL-1: pause-window access for the file command
		// channel (missiongui.cpp) — routes `press pause.*` through the same
		// PauseWindow::handleClick the ImGui/legacy buttons use.
		PauseWindow* getPauseWnd() { return pauseWnd.get(); }

		RectInfo *getRect (long id)
		{
			if ((id >= 0) && (id < rectCount))
			{
				return &(rectInfos[id]);
			}

			if (id == RPTOTAL_CALLOUT)
			{
				return &rpCallout;
			}

			return nullptr;
		}

};


//*************************************************************************************************
#endif  // end of file ( ControlGui.h )
