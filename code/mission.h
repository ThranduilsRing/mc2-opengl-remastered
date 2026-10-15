//******************************************************************************************
//	mission.h - This file contains the mission class header
//		Missions are what scenarios were in MechCommander 1.
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MISSION_H
#define MISSION_H
#include "save_archive.h"
#include "tactical_order_queue.h"
#include <algorithm>
//----------------------------------------------------------------------------------
// Include Files
#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef WARRIOR_H
#include"warrior.h"
#endif

#ifndef MISSIONGUI_H
#include"missiongui.h"
#endif

#ifndef OBJECTIVE_H
#include"objective.h"
#endif

#ifndef LOGISTICSMECH_H
#include"logisticsmech.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

namespace mc2::sim {
// Access resolves mission-owned recipient slots, independent of gated watch generations.
template<class LegacyOrder, class Access>
class MoverOrderApplication final : public OrderAdmissionValidator, public OrderApplication {
public:
    explicit MoverOrderApplication(Access access) : access_(access) {}
    bool permitted(const OrderSource& source, const OrderEnvelope& e) const override {
        if (source.kind != SourceKind::MissionContent && source.kind != SourceKind::UI) return false;
        if (source.kind == SourceKind::MissionContent && e.payload.origin != OrderOrigin::Commander) return false;
        if (source.kind == SourceKind::UI && e.payload.origin != OrderOrigin::Player) return false;
        for (std::uint32_t i = 0; i < e.recipients.size(); ++i)
            if (validateRecipient(e, e.recipients[i], i)) return false;
        return true;
    }
    std::uint32_t validateCommand(const OrderEnvelope& e) const override {
        if (e.queueMode != QueueMode::Replace) return reason(MoverOrderRejection::UnsupportedMode);
        const auto& p = e.payload;
        if (p.origin != OrderOrigin::Player && p.origin != OrderOrigin::Commander)
            return reason(MoverOrderRejection::UnsupportedPayload);
        const bool route = p.kind == OrderKind::PatrolPath || p.kind == OrderKind::TraversePath;
        if (p.kind != OrderKind::Stop && p.kind != OrderKind::MoveToPoint && !route)
            return reason(MoverOrderRejection::UnsupportedKind);
        if ((route || p.kind == OrderKind::MoveToPoint) && !p.move)
            return reason(MoverOrderRejection::UnsupportedPayload);
        if (p.orderId > 0x7fffffffULL || p.formationOrdinal || p.groupFlags || p.subOrder || p.unitOrder)
            return reason(MoverOrderRejection::UnsupportedPayload);
        if (p.move) {
            const auto& m = *p.move;
            if (m.wayPath.points.empty() || m.wayPath.points.size() > (route ? 8u : 1u) || m.wayPath.curPoint != 0 ||
                m.wayPath.mode.size() != m.wayPath.points.size() ||
                std::ranges::any_of(m.wayPath.mode, [](auto mode) { return mode == TravelMode::Jump; }) ||
                m.faceObject || m.wait || m.mode != MoveMode::Normal || m.escapeTile ||
                m.jump || m.fromArea != -1 || m.keepMoving)
                return reason(MoverOrderRejection::UnsupportedPayload);
        }
        return 0;
    }
    std::uint32_t validateRecipient(const OrderEnvelope& e, ObjectRef ref, std::uint32_t) const override {
        auto* mover = access_.resolve(ref);
        if (!mover || !mover->getExists() || mover->isDestroyed() || !mover->getPilot())
            return reason(MoverOrderRejection::MissingRecipient);
        const auto* commander = std::get_if<CommanderId>(&e.issuer);
        if (!commander || mover->getCommanderId() < 0 ||
            commander->value != static_cast<std::uint32_t>(mover->getCommanderId()))
            return reason(MoverOrderRejection::Permission);
        if (e.payload.move && (mover->isDisabled() || !mover->canMove()))
            return reason(MoverOrderRejection::CannotMove);
        return 0;
    }
    ApplicationOutcome applyRecipient(const OrderEnvelope& e, ObjectRef ref, std::uint32_t ordinal) override {
        if (auto r = validateCommand(e)) return {RecipientStatus::Rejected, r};
        if (auto r = validateRecipient(e, ref, ordinal)) return {RecipientStatus::Rejected, r};
        LegacyOrder legacy;
        // Admitted kind/origin tags match the legacy enums.
        legacy.code = static_cast<decltype(legacy.code)>(e.payload.kind);
        legacy.origin = static_cast<decltype(legacy.origin)>(e.payload.origin);
        legacy.id = static_cast<decltype(legacy.id)>(e.payload.orderId);
        legacy.time = access_.simulationTime();
        legacy.unitOrder = false;
        legacy.subOrder = false;
        legacy.selectionIndex = -1;
        legacy.groupFlags = 0;
        if (e.payload.move) {
            const auto& m = *e.payload.move;
            legacy.moveParams.wayPath.numPoints = static_cast<long>(m.wayPath.points.size());
            legacy.moveParams.wayPath.curPoint = 0;
            for (std::size_t i = 0; i < m.wayPath.points.size(); ++i) {
                legacy.moveParams.wayPath.points[i * 3] = m.wayPath.points[i].x;
                legacy.moveParams.wayPath.points[i * 3 + 1] = m.wayPath.points[i].y;
                legacy.moveParams.wayPath.points[i * 3 + 2] = m.wayPath.points[i].z;
                legacy.moveParams.wayPath.mode[i] = static_cast<unsigned char>(m.wayPath.mode[i]);
            }
            legacy.moveParams.faceObject = false;
            legacy.moveParams.wait = false;
            legacy.moveParams.mode = static_cast<decltype(legacy.moveParams.mode)>(0);
            legacy.moveParams.escapeTile = false;
            legacy.moveParams.jump = false;
            legacy.moveParams.fromArea = -1;
            legacy.moveParams.keepMoving = false;
        }
        auto* mover = access_.resolve(ref);
        auto* stored = mover->getPilot()->getTacOrder(e.payload.origin == OrderOrigin::Player ? 1 : 0);
        const auto previousCode = stored->code;
        // A distinct marker detects suppressed handoffs even when the old order matches.
        stored->code = static_cast<decltype(stored->code)>(0);
        const auto result = mover->handleTacticalOrder(legacy, 0, false);
        const bool installed = stored->code != static_cast<decltype(stored->code)>(0);
        if (!installed) stored->code = previousCode;
        if (result != 0 || !installed)
            return {RecipientStatus::Rejected, reason(MoverOrderRejection::HandlerFailure)};
        if (stored->code != legacy.code || stored->origin != legacy.origin ||
            stored->time != legacy.time || stored->unitOrder != legacy.unitOrder ||
            stored->subOrder != legacy.subOrder || stored->selectionIndex != legacy.selectionIndex ||
            stored->groupFlags != legacy.groupFlags)
            return {RecipientStatus::Rejected, reason(MoverOrderRejection::HandlerFailure)};
        if (e.payload.move && (stored->moveParams.wayPath.numPoints != legacy.moveParams.wayPath.numPoints ||
            stored->moveParams.wayPath.curPoint != 0 ||
            stored->moveParams.wayPath.mode[0] != legacy.moveParams.wayPath.mode[0] ||
            stored->moveParams.wayPath.points[0] != legacy.moveParams.wayPath.points[0] ||
            stored->moveParams.wayPath.points[1] != legacy.moveParams.wayPath.points[1] ||
            stored->moveParams.wayPath.points[2] != legacy.moveParams.wayPath.points[2] ||
            stored->moveParams.faceObject != legacy.moveParams.faceObject ||
            stored->moveParams.wait != legacy.moveParams.wait ||
            stored->moveParams.mode != legacy.moveParams.mode ||
            stored->moveParams.escapeTile != legacy.moveParams.escapeTile ||
            stored->moveParams.jump != legacy.moveParams.jump ||
            stored->moveParams.fromArea != legacy.moveParams.fromArea ||
            stored->moveParams.keepMoving != legacy.moveParams.keepMoving))
            return {RecipientStatus::Rejected, reason(MoverOrderRejection::HandlerFailure)};
        if (e.payload.move) {
            for (std::size_t i = 0; i < e.payload.move->wayPath.points.size(); ++i) {
                if (stored->moveParams.wayPath.mode[i] != legacy.moveParams.wayPath.mode[i])
                    return {RecipientStatus::Rejected, reason(MoverOrderRejection::HandlerFailure)};
                for (std::size_t axis = 0; axis < 3; ++axis)
                    if (stored->moveParams.wayPath.points[i * 3 + axis] != legacy.moveParams.wayPath.points[i * 3 + axis])
                        return {RecipientStatus::Rejected, reason(MoverOrderRejection::HandlerFailure)};
            }
        }
        if constexpr (requires { access_.installed(e, ref); }) access_.installed(e, ref);
        return {};
    }
    ApplicationOutcome applySupport(const OrderEnvelope&) override {
        return {RecipientStatus::Rejected, reason(MoverOrderRejection::UnsupportedKind)};
    }
private:
    Access access_;
    static constexpr std::uint32_t reason(MoverOrderRejection r) { return static_cast<std::uint32_t>(r); }
};
} // namespace mc2::sim

//----------------------------------------------------------------------------------
// Macro Definitions
#define mis_PLAYING						0
inline constexpr int mis_PLAYER_LOST_BIG = 1;
inline constexpr int mis_PLAYER_LOST_SMALL = 2;
inline constexpr int mis_PLAYER_DRAW = 3;
inline constexpr int mis_PLAYER_WIN_SMALL = 4;
#define mis_PLAYER_WIN_BIG	 			5

extern unsigned long scenarioResult;

//----------------------------------------------------------------------------------
// These are placeholders for the scenario specific objects.  This allows us to
// create objects which the scenario can manipulate using other scenario data.
// For example, a part may be assigned to a certain area.  This would inform
// the part of where its supposed to be for AI AND allow us to remove the part
// if all player controlled objects leave that area.  What the folks downstairs
// would call an interest manager except that its simple and it works!
struct Part
{
	GameObjectWatchID	objectWID;			//Pointer to my physical incarnation
	unsigned long		objNumber;			//What kind of object am I?
	DWORD				baseColor;			//Base color of mech -- Overrides the RED in RGB
	DWORD				highlightColor1;	//First Highlight Color -- Overrides the GREEN in RGB
	DWORD				highlightColor2;	//Second Highlight Color -- Overrides the BLUE in RGB
	long				active;				//Am I currently awake?
	long				exists;				//Am I currently in Existance?
	bool				destroyed;			//Have I been destroyed for this scenario?
	Stuff::Vector3D		position;			//Where do I start?  (relative to area)
	float				velocity;			//How fast am I going?
	float				rotation;			//Which direction am I facing?
	unsigned long		gestureId;			//What gesture do I start in?
	char				alignment;			//Who do I fight for?
	char				teamId;				//Which team am I on?
	long				commanderID;		//Used when setting up multiplayer
	char				squadId;				//Which team am I on?
	char				myIcon;				//If set, start with Icon on Screen for this part.
	unsigned long		controlType;		//How am I controlled?
	unsigned long		controlDataType;	//What data do you need to control me?
	char				profileName[9];		//Name of Object Profile file.
	char				csvFile[50];		//Mod-tolerance: CSVFile from mission .fit — used to back-fill stub ObjectType::appearName when pak lookup fails.
	unsigned long		pilot;				//Name of Pilot File.
	bool				captureable;		//Is this a capturable "enemy" mech?
	DWORD				variantNum;			//Variant number of the Part.

	void Save (FitIniFilePtr file, long partNum);
	void Load (FitIniFilePtr file, long partNum);
	mc2::save::Part saveRecord() const;
	bool restoreRecord(const mc2::save::Part& record, mc2::save::Diagnostic diagnostic = {});
};

typedef Part *PartPtr;

//----------------------------------------------------------------------------------
// This is the struct definition for the Scenario Objectives.
// These can be displayed in a window.  For now they are drawn with just text.
// There are a whole series of ABL commands to address these objectives.
typedef unsigned long	ObjectiveType;
inline constexpr int InvisibleGoal = -1;
inline constexpr int PrimaryGoal = 0;
inline constexpr int SecondaryGoal = 1;
inline constexpr int OtherGoal = 2;
inline constexpr int BonusGoal = 3;
inline constexpr int InvalidGoal = 9999;

typedef unsigned long	ObjectiveStatus;
#define					Incomplete		0
#define					Success			1
#define					Failed			2
inline constexpr int InvalidStatus = 9999;

#define					SCENARIO_TIMER_1		7
#define					SCENARIO_TIMER_2		(SCENARIO_TIMER_1 + 1)
#define					SCENARIO_TIMER_3		(SCENARIO_TIMER_1 + 2)
#define					SCENARIO_TIMER_4		(SCENARIO_TIMER_1 + 3)
#define					SCENARIO_TIMER_5		(SCENARIO_TIMER_1 + 4)
#define					SCENARIO_TIMER_6		(SCENARIO_TIMER_1 + 5)
#define					SCENARIO_TIMER_7		(SCENARIO_TIMER_1 + 6)
#define					SCENARIO_TIMER_8		(SCENARIO_TIMER_1 + 7)

#define					OBJECTIVE_1_TIMER		24
#define					OBJECTIVE_2_TIMER		(OBJECTIVE_1_TIMER + 1)
#define					OBJECTIVE_3_TIMER		(OBJECTIVE_1_TIMER + 2)
#define					OBJECTIVE_4_TIMER		(OBJECTIVE_1_TIMER + 3)
#define					OBJECTIVE_5_TIMER		(OBJECTIVE_1_TIMER + 4)
#define					OBJECTIVE_6_TIMER		(OBJECTIVE_1_TIMER + 5)
#define					OBJECTIVE_7_TIMER		(OBJECTIVE_1_TIMER + 6)
#define					OBJECTIVE_8_TIMER		(OBJECTIVE_1_TIMER + 7)

inline constexpr int MAX_OBJECTIVES = 50;

typedef enum {
	MISSION_LOAD_SP_QUICKSTART,
	MISSION_LOAD_SP_LOGISTICS,
	MISSION_LOAD_MP_QUICKSTART,
	MISSION_LOAD_MP_LOGISTICS
} MissionLoadType;

struct Objective
{
	char				name[80];
	ObjectiveType		type;
	float				timeLeft;
	ObjectiveStatus		status;
	Stuff::Vector3D		position;
	long				points;
	float				radius;
};

typedef Objective *ObjectivePtr;

//----------------------------------------------------------------------------------
// SIM-SPEED-BUTTON-1 (mission.cpp): runtime-settable sim-speed multiplier,
// seeded from MC2_SIM_SPEED (default 1.0), clamped [0.25, 4.0]. Applied at the
// two drivers of gameplay time in lockstep: scenarioTime advance (mission.cpp)
// and frameLength (mechcmd2.cpp). Setters log a [SIM-SPEED v1] stderr line.
// Reset to the env seed at every Mission::init so a HUD-set speed is scoped to
// the mission that set it (logistics / next mission start back at the seed).
float simSpeedGetMultiplier();
void  simSpeedSetMultiplier( float v );
void  simSpeedResetToSeed();

//----------------------------------------------------------------------------------
// To operate, simply call init with the filename of the mission.
// that's all she wrote.  update and render return the mission completion status.
// Phase-helper state of Mission::init (defined in mission_init_frame.h).
struct MissionInitFrame;
// Phase-helper state of Mission::load (defined in mission_load_frame.h).
struct MissionLoadFrame;

class Mission
{
	friend struct MissionPartArchiveTest;

	//Data Members
	//-------------
	protected:
		mc2::sim::MissionOrderIngress orderIngress;
		using OrderRecipientBinding = mc2::sim::MissionRecipientBinding<Mover, MechWarrior>;
		std::vector<OrderRecipientBinding> orderRecipients;
		std::string authoredMissionText;
		std::vector<std::uint32_t> authoredOpordPilots;
		std::vector<mc2::sim::AuthoredRoute> authoredRoutes;
		std::vector<std::string> authoredDiagnostics;
		std::optional<mc2::sim::MissionReceipt> orderFixtureReceipt;
		bool orderFixtureApplied = false;
		std::uint32_t orderFixtureSubmitted = 0, orderFixtureCompleted = 0;
	
		FitIniFilePtr					missionFile;
										
		long							operationId;		// aka operation id
		long							missionId;			// aka mission id
										
		char							missionFileName[80];
		char							missionScriptName[80];
		ABLModulePtr					missionBrain;
		ABLParamPtr						missionParams;
		SymTableNodePtr					missionBrainCallback;
										
		unsigned long					numParts;
		PartPtr							parts;
										
		bool							active;

		static double					missionTerminationTime;

										
	public:								

		static bool						terminationCounterStarted;
		static unsigned long			terminationResult;

		// GAMEPLAY-DRIVER-2 (MC2_UI_COMMAND_FILE `end_mission` verb): force the
		// mission-termination counter from outside. Sets exactly the three
		// fields the authored win/loss paths set (mission.cpp:1315 / 1357 /
		// 1414) so the NORMAL terminationCounterStarted -> resultsDone() ->
		// scenarioResult -> log_RESULTS flow runs; it does not shortcut it.
		// missionTerminationTime is protected, which is the only reason this
		// needs to be a member rather than a free function.
		// result: one of mis_PLAYER_WIN_BIG / mis_PLAYER_LOST_BIG / etc.
		void forceTerminate (unsigned long result);

		// GAMEPLAY-DRIVER-2: sticky latch for forceTerminate. mis_PLAYING (0)
		// == unarmed. Needed because the mission ABL brain re-writes
		// terminationResult from its return value EVERY frame
		// (mission.cpp, "if (!MPlayer) terminationResult = missionResult;" --
		// the site the neighbouring comment calls "mission brain frequently
		// trashes the termination result"). Without the latch a forced result
		// survived exactly one frame: end_mission acked ok and mission_status
		// still read status=0 six seconds later. Re-asserted immediately after
		// the brain runs.
		static unsigned long forcedTerminationResult;

		unsigned long					numObjectives;
		ObjectivePtr					objectives;
		float						m_timeLimit;
		//CObjectives						missionObjectives;
		//int						numPrimaryObjectives;
		unsigned long					duration;
		bool							warning1;
		bool							warning2;
										
		float							actualTime;
		float							runningTime;
										
		long 							numSmallStrikes;
		long 							numLargeStrikes;
		long 							numSensorStrikes;
		long 							numCameraStrikes;
										
		unsigned char					missionTuneNum;
										
		long							missionScriptHandle;
		ABLParam						*missionBrainParams;
		
		MissionInterfaceManagerPtr		missionInterface;

		Stuff::Vector3D					dropZone;
		
		long							theSkyNumber;

		static bool						statisticsInitialized;

	//Member Functions
	//-----------------
	protected:
	
	public:
	
		void init (void)
		{
			missionFile = NULL;

			operationId = missionId = -1;

			missionBrain = NULL;
			missionParams = NULL;
			missionBrainCallback = NULL;

			numParts = 0;
			parts = NULL;

			numObjectives = 0;
			objectives = NULL;

			duration = 0;

			active = FALSE;
			
			numSmallStrikes = 0;			
			numLargeStrikes = 0;
			numSensorStrikes = 0;
			numCameraStrikes = 0;
			
			missionTuneNum = 0;
			
			missionScriptHandle = -1;
			
			missionInterface = NULL;

			missionFileName[0] = 0;

			missionBrainParams = NULL;
		}
		
		Mission (void)
		{
			init();
		}

		bool calcComplexDropZones (char* missionName, char dropZoneList[MAX_MC_PLAYERS]);
		void resetOrderIngress();
		void admitAuthoredRoutes();
		void recordAuthoredInstallation(const mc2::sim::OrderEnvelope&, mc2::sim::ObjectRef);
		std::string authoredRouteEvidence() const;
		void diagnoseAuthoredRoute(std::uint32_t original, std::uint32_t selected, std::uint32_t pilot,
			std::string_view status, std::string_view reason);
		void bindOrderRecipients();
		void beginOrderSimulationTick();
		void completeOrderSimulationTick();
		MoverPtr resolveOrderRecipient(mc2::sim::ObjectRef ref);
		std::uint64_t orderSession() const { return orderIngress.session(); }
		std::optional<mc2::sim::ObjectRef> orderRecipient(long partId) const;
		std::expected<mc2::sim::MissionReceipt, mc2::sim::OrderError> submitMissionOrder(
			std::uint64_t session, mc2::sim::OrderEnvelope envelope);
		bool runOrderIngressFixture();
		bool orderIngressFixtureApplied() const {
			return orderFixtureReceipt && orderFixtureApplied && orderFixtureSubmitted != 0 &&
				orderFixtureCompleted == orderFixtureSubmitted && orderIngress.queue().receipts().empty() &&
				orderIngress.queue().snapshotPending().empty() && !orderIngress.receipt(*orderFixtureReceipt);
		}
		std::expected<mc2::sim::OrderReceipt, mc2::sim::OrderError> missionOrderReceipt(
			mc2::sim::MissionReceipt receipt) const { return orderIngress.receipt(receipt); }
		std::expected<void, mc2::sim::OrderError> acknowledgeMissionOrder(
			mc2::sim::MissionReceipt receipt) { return orderIngress.acknowledge(receipt); }

		void init (const char *missionName, long loadType, long dropZoneID, Stuff::Vector3D* dropZoneList, char commandersToLoad[8][3], long numMoversPerCommander);

		// Explicit developer import of a Part fragment; the runtime save/load format remains legacy.
		bool loadParts(const mc2::save::Section& section, std::uint32_t schemaVersion,
			mc2::save::Diagnostic diagnostic = {});

	// Mission::init phases (mission_init.cpp, mission_init_load.cpp); callers use init().
	private:
		void initPrologue(MissionInitFrame& f);
		void initTeamRelations(long loadType);
		void initCoreSystems();
		void initGameSystemRanges(MissionInitFrame& f);
		void initGameSystemGeneral(MissionInitFrame& f);
		void initGameSystemHazards(MissionInitFrame& f);
		void initMissionFile(Stuff::Vector3D* dropZoneList, MissionInitFrame& f);
		void initSkyAndTeamReset(MissionInitFrame& f);
		void initTeamsAndCommanders(long loadType, Stuff::Vector3D* dropZoneList, char (*commandersToLoad)[3], MissionInitFrame& f);
		void initTriggerAndDropZone(MissionInitFrame& f);
		void initMissionSettings(MissionInitFrame& f);
		void initTerrain(MissionInitFrame& f);
		void initPathfinding(MissionInitFrame& f);
		void initAblCoreLibs(MissionInitFrame& f);
		void initMissionScript(MissionInitFrame& f);
		void initWarriorsHeader(MissionInitFrame& f);
		void loadWarriors(MissionInitFrame& f);
		void soakAutowinCheck(MissionInitFrame& f);
		void loadBrainParamsPhase(MissionInitFrame& f);
		void loadMissionFitOpord(MissionInitFrame& f);
		void loadBrainRuntime(MissionInitFrame& f);
		void loadTacticWeights(MissionInitFrame& f);
		void loadBrainDispatch(MissionInitFrame& f);
		void initPartsHeader(MissionInitFrame& f);
		void initPartsLoad(long loadType, long dropZoneID, Stuff::Vector3D* dropZoneList, char (*commandersToLoad)[3], long numMoversPerCommander, MissionInitFrame& f);
		void initObjectCounts(long loadType, MissionInitFrame& f);
		void initGpuAndLighting();
		void initSpawnActors(long loadType);
		void initTerrainObjectsLoad(char (*commandersToLoad)[3], MissionInitFrame& f);
		void initDisableAndObjectives(MissionInitFrame& f);
		void initCommanders(char (*commandersToLoad)[3], MissionInitFrame& f);
		void initWeatherCamera(MissionInitFrame& f);
		void initInterface(MissionInitFrame& f);
		void initLabTimingAndClose();
		void initFinalizeGeometry();

	// Mission::load phases (saveload.cpp, saveload_load.cpp); callers use load().
		void loadPrologue(MissionLoadFrame& f);
		bool openSaveFile(MissionLoadFrame& f);
		void loadDestroyAndRestore();
		void loadResetSimState(MissionLoadFrame& f);
		void loadGameSystemRanges(MissionLoadFrame& f);
		void loadGameSystemGeneral(MissionLoadFrame& f);
		void loadGameSystemHazards(MissionLoadFrame& f);
		void loadMissionSettings(MissionLoadFrame& f);
		void loadAblAndManagers(MissionLoadFrame& f);
		void loadTerrain(MissionLoadFrame& f);
		void loadMovement(MissionLoadFrame& f);
		void loadMissionScript(MissionLoadFrame& f);
		void loadWarriorsAndPartsHeader(MissionLoadFrame& f);
		void loadParts(MissionLoadFrame& f);
		void loadTeamsAndObjects(MissionLoadFrame& f);
		void loadObjectivesCommanders(MissionLoadFrame& f);
		void loadWeatherCamera(MissionLoadFrame& f);
		void loadInterface(MissionLoadFrame& f);
		void loadLogistics(MissionLoadFrame& f);
		void loadActivateAndFinalize();
		void loadRestoreInput(MissionLoadFrame& f);

	// Mission::update phases (mission_update.cpp); callers use update().
		void updateDevHotkeys();
		void updateFrameClock();
		void updateFrontEnd();
		void updateTerrainGeometry();
		void updateObjectManager();
		void updateCratersAndDamp();
		void updateSimSystems();
		void updateTraceHooks();
		void updateTermination();
		void updateCheatsAndFrameRate();
		void updateSoakAutoWin();
		void updateSoakKillEnemy();
		void updateOrderLiveness();
		void updateSoakPilotPromote();
	public:

		// TEAM-COMMANDER-OWNERSHIP-1: single teardown authority for the
		// Team::teams[]/Commander::commanders[] static arrays. Idempotent,
		// null-safe; frees every live slot, resets numTeams/numCommanders to 0,
		// and nulls the Team::home/Commander::home raw aliases into those slots.
		// Called by BOTH Mission::init's re-init path AND Mission::destroy so the
		// two can never diverge (was duplicated inline -> dual-free / dangling
		// home alias risk across mission cycles). Save/load-neutral.
		static void resetTeamsAndCommanders();

		static void initBareMinimum();
		
		static void initTGLForMission();
		static void initTGLForLogistics();
		
		void start (void);

		Stuff::Vector3D getDropZone( ) const { return dropZone; }
		
		long update (void);
		long render (void);
		
		void destroy (bool initLogistics = true);
		
		~Mission (void)
		{
			destroy(false);
		}

		long getStatus (void);

		long getNumParts (void) 
		{
			return(numParts);
		}

		PartPtr getPart (long partNumber) 
		{
			return(&parts[partNumber]);
		}

		long getPartTeamId (long partNumber) 
		{
			return(parts[partNumber].teamId);
		}

		GameObjectPtr getPartObject (long partNumber)
		{
			if ((partNumber <= 0) || ((unsigned long)partNumber > numParts))
				return NULL;
			if (!ObjectManager)
				return(NULL);
			return ((GameObjectPtr) ObjectManager->getByWatchID(parts[partNumber].objectWID));
		}

		long addMover (MoverInitData* moveSpec);
		long addMover (MoverInitData* moveSpec, LogisticsMech* mechData);
		long addMover (MoverInitData* moveSpec, CompressedMech* mechData);
		
		long removeMover (MoverPtr mover);

		void tradeMover (MoverPtr mover, long newTeamID, long newCommanderID, char* pilotFileName, const char* brainFileName);

		void createPartObject (long objectId, MoverPtr mover);
		
		void createMissionObject (long partId);		//Moves object from holding area to real world.
		
		ABLModulePtr getBrain (void) 
		{
			return(missionBrain);
		}

		void handleMultiplayMessage (long code, long param1);

		//-----------------------------------------------------
		// Objective Routines
		void startObjectiveTimers (void);
		
		long setObjectiveTimer (long objectiveNum, float timeLeft);
		float checkObjectiveTimer (long objectiveNum);
		
		long setObjectiveStatus (long objectiveNum, ObjectiveStatus status);
		ObjectiveStatus checkObjectiveStatus (long objectiveNum);
		
		long setObjectiveType (long objectiveNum, ObjectiveType type);
		ObjectiveType checkObjectiveType (long objectiveNum);
		
		void setObjectivePos (long objectiveNum, float realX, float realY, float realZ);
		
		void setupBonus (void);
		
		//Checks if any objective has succeeded or failed since we last checked.
		bool checkObjectiveSuccess (void);
		bool checkObjectiveFailed (void);
		//-----------------------------------------------------

		long GetOperationID(void) 
		{
			return operationId;
		}

		long GetMissionID(void) 
		{
			return missionId;
		}

		char * getMissionName (void)
		{
			return missionScriptName;
		}

		long getMissionTuneId (void)
		{
			return missionTuneNum;
		}

		const char* getMissionFileName(){ return missionFileName; }
		
		static void initializeStatistics();

		void load (const char *filename);
		void save (const char *filename);

		bool isActive (void)
		{
			return active;
		}

};

//----------------------------------------------------------------------------------

inline constexpr int MAX_GLOBAL_MISSION_VALUES = 50;

extern float worldUnitsPerMeter;
extern float metersPerWorldUnit;
extern float maxVisualRange;
extern float fireVisualRange;
extern bool CantTouchThis;

extern float MineDamage;
extern float MineSplashDamage;
extern float MineSplashRange;
extern long MineExplosion;

extern float globalMissionValues [];

extern unsigned char godMode;

extern Mission *mission;
extern unsigned long scenarioResult;

extern UserHeapPtr missionHeap;
//----------------------------------------------------------------------------------
#endif
