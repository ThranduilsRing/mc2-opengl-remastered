#ifndef LOGISTICSDATA_H
#define LOGISTICSDATA_H
/*************************************************************************************************\
LogisticsData.h			: Interface for the LogisticsData component.  This thing holds the inventory
							lists and the component lists
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include <string_view>
#include "mc2_czstring.h"
#include <expected>
#include <optional>
#include <functional>
#include"stuff/stuff.hpp"
#include <string>
#include <vector>
#include <deque>
#include <memory>

#include"logisticsmech.h"
#include"logisticscomponent.h"
#include"logisticspilot.h"

class FitIniFile;
class LogisticsMissionInfo;
class PacketFile;

class LogisticsData
{

public:

	struct Building;

	LogisticsData();
	~LogisticsData();
	void			init();
	void			initFast();
	// Null trigger means an unspecified diagnostic source.
	void			ensureMechBayDataLoaded(mc2::czstring trigger = nullptr);

	bool			rpJustAdded;

	static LogisticsData*		instance;

	std::optional<std::reference_wrapper<LogisticsComponent>> getComponent( int componentID );
	LogisticsPilot*		getPilot( int pilotID );
	// Nullable lookup names retain their historical null-as-empty contract.
	std::optional<std::reference_wrapper<LogisticsPilot>> getPilot( mc2::czstring pilotName );
	int					getPilotCount();
	int					getPilots( LogisticsPilot**, long& count );
	std::optional<std::reference_wrapper<const std::string>> getBestPilot( long mechWeight );
	bool				gotPilotsLeft();

	void				setPilotUnused( mc2::czstring pPilot );

	int					getVariantsInInventory( LogisticsVariant* pVar, bool bIncludeForceGroup );
	int					getPlayerVariantNames( const char**, int& count );
	int					getChassisVariants( const LogisticsChassis* pChassis, LogisticsVariant** pVar, int& count );
	
	std::optional<std::reference_wrapper<LogisticsVariant>> getVariant( int ID );
	std::optional<std::reference_wrapper<LogisticsVariant>> getVariant( mc2::czstring mechName );
	std::optional<std::reference_wrapper<LogisticsVariant>> getVariant( mc2::czstring pCSVFileNaem, int VariantNum );
	std::expected<void, int> removeVariant( mc2::czstring varName );


	std::optional<std::reference_wrapper<LogisticsMech>> getMech( int ID );
	// Null pilotName selects an unpiloted mech; MechName null means empty.
	std::optional<std::reference_wrapper<LogisticsMech>> getMech( mc2::czstring MechName, mc2::czstring pilotName );
	std::optional<std::reference_wrapper<LogisticsMech>> getMechWithoutForceGroup( LogisticsMech* clone );

	void				addMechToInventory( LogisticsVariant* pVar, LogisticsPilot* pPilot, int ForceGrup,
											bool bSubtractPts = 0);




	int createInstanceID( LogisticsVariant* pVar );
	std::optional<std::reference_wrapper<LogisticsPilot>> getFirstAvailablePilot();
	// VEHICLE-AUTO-PILOT-1: a deployed vehicle/support unit auto-gets a standard
	// vehicle pilot so it satisfies the launch pilot-requirement without consuming a
	// roster mech-pilot. isVehicleUnit() = object class != BATTLEMECH; getVehiclePilot()
	// returns a shared, hidden-from-roster (setAvailable(false)) standard vehicle pilot.
	bool				isVehicleUnit( class LogisticsMech* pMech );
	LogisticsPilot*		getVehiclePilot();
	// A snapshot of the pointers, taken by value: a caller may sell and delete from the live inventory while it walks its copy.
	using MechList = std::vector<LogisticsMech*>;
	MechList			getForceGroup();
	MechList			getInventory();


	void				removeMechsInForceGroup(); // takes mechs that are in the force group out of the inventory
	void				addMechToInventory( LogisticsVariant* pVar,
											int addToForceGroup, LogisticsPilot*,
											unsigned long color1 = 0xffffffff, 
											unsigned long color2 = 0xffffffff,
											unsigned long color3 = 0xffffffff);
	void				removeMechFromInventory( mc2::czstring mechName, mc2::czstring pilotName );

	std::expected<void, int> addMechToForceGroup( LogisticsMech* pMech, int slot );
	std::expected<void, int> removeMechFromForceGroup( LogisticsMech* pMech, bool bRemovePilot );
	std::expected<void, int> removeMechFromForceGroup( int slot );

	bool				canAddMechToForceGroup( LogisticsMech* pMech );
	int getDeploymentCapacity() const;


	long				save( FitIniFile& file );
	std::expected<void, long> load( FitIniFile& file );

	void				setResourcePoints(long rp) {resourcePoints = rp; rpJustAdded = 0;}
	int					getResourcePoints() { return resourcePoints; }
	void				addResourcePoints( int amount ){ resourcePoints+= amount; rpJustAdded = true;}
	void				decrementResourcePoints( int amount ){ resourcePoints -= amount; }

	int					getCBills();
	void				addCBills( int amount );
	void				decrementCBills( int amount );

	int					comparePilots( LogisticsPilot* p1, LogisticsPilot* p2, long weight );
	int					getCurrentDropWeight() const;
	int					getMaxDropWeight() const;

	std::expected<void, long> loadMech( FitIniFile& file, int& count, long version );

	void				setMissionCompleted();
	long				updateAvailability();

	// Front-end screen state capture: emits a "LOGISTICS" diagnostic-trace event
	// (MCP-readable) with the current screen + bay/purchase/loadout/launch state
	// (purchasable mechs, inventory, force group, pilots, CB/RP, drop weight,
	// mission info). Self-gates on the LOGISTICS diag tag being enabled.
	// Null screenName is emitted as an empty diagnostic label.
	void				dumpFrontEndState( mc2::czstring screenName );

	const std::string&	getCurrentMission() const;
	const std::string&	getLastMission() const;
	void				setSingleMission( mc2::czstring pName );
	bool				isSingleMission();
	std::optional<long> getCurrentMissionTune();
	// The text getters below return a copy; an empty string means "there is none" (logisticsmissioninfo.h).
	std::string			getCurrentABLScript() const;
	long 				getCurrentMissionId();

	void				clearInventory();

	int					getPurchasableMechs( LogisticsVariant**, int& count );
	std::expected<void, int> canPurchaseMech( LogisticsVariant* pVar );
	std::expected<void, int> purchaseMech( LogisticsVariant* pVar );
	std::expected<void, int> sellMech( LogisticsMech* pVar );
	int					getEncyclopediaMechs( const LogisticsVariant**, int& count );
	int					getHelicopters( const LogisticsVariant**, int& count );
	
	int					getVehicles( const LogisticsVehicle**, int& count );
	std::optional<std::reference_wrapper<LogisticsVehicle>> getVehicle( mc2::czstring pName );


	std::expected<void, int> setMechToModify( LogisticsMech* pVariant );
	LogisticsMech*		getMechToModify( ){ return currentlyModifiedMech; }
	std::expected<void, int> acceptMechModifications( mc2::czstring pNewVariantName );
	std::expected<void, int> cancelMechModfications();
	bool				canReplaceVariant( mc2::czstring name );
	bool				canDeleteVariant( mc2::czstring name );

	void				setCurrentMissionNum (long cMission);
	long				getCurrentMissionNum (void);

	int					getAvailableComponents( LogisticsComponent** pComps, int& maxCount );
	int					getAllComponents( LogisticsComponent** pComps, int& maxCount );
	std::string			getCurrentOperationFileName();
	std::string			getCurrentVideoFileName();
	std::string			getCurrentMissionDescription();
	int					getAvailableMissions( const char** missionNames, long& count );
	int					getCurrentMissions( const char** missionNames, long& count );
	bool				getMissionAvailable( mc2::czstring missionName );
	std::string			getCurrentMissionFriendlyName( );
	std::string			getMissionFriendlyName( mc2::czstring missionName );
	long				getMaxTeams() const;
	long				getMaxPlayers() const;

	std::expected<void, int> acceptMechModificationsUseOldVariant( mc2::czstring name );

	const std::string&	getCampaignName() const;




	// Null or empty mission names are rejected by the mission-info boundary.
	std::expected<void, long> setCurrentMission( mc2::czstring missionName );
	// Cross-stage mission set for the headless boot driver (MC2_BOOT_TO_MISSION):
	// jump to any mission regardless of stage/progression, then updateAvailability.
	std::expected<void, long> setCurrentMissionAnyStage( mc2::czstring missionName );

	void				startNewCampaign( mc2::czstring fileName = "campaign");
	void				startMultiPlayer();
	void				setPurchaseFile( mc2::czstring fileName );

	std::optional<std::reference_wrapper<Building>> getBuilding( int nameID );
	int					getBuildings( Building** bdgs, int& count );

	bool				campaignOver();
	std::string			getCurrentBigVideo() const;
	std::string			getFinalVideo() const;

	bool				newMechsAvailable() { return bNewMechs; }
	bool				newPilotsAvailable() { return bNewPilots; }
	bool				newWeaponsAvailable() { return bNewWeapons; }

	void				setNewPilotsAcknowledged(){ bNewPilots = 0; }
	void				setNewWeaponsAcknowledged(){ bNewWeapons = 0; }
	void				setNewMechsAcknowledged(){ bNewMechs = 0; }

	// A null bonus filename is ignored.
	void				addNewBonusPurchaseFile( mc2::czstring pFileName );
	void				appendAvailability(mc2::czstring pFileName, bool* availableArray );

	bool				skipLogistics();
	void				setSkipLogistics(bool skip);
	bool				showChooseMission();
	bool				skipPilotReview();
	bool				skipSalvageScreen();
	bool				skipPurchasing();

	bool				canHaveSalavageCraft();
	bool				canHaveRepairTruck();
	bool				canHaveScoutCopter();
	bool				canHaveArtilleryPiece();
	bool				canHaveAirStrike();
	bool				canHaveSensorStrike();
	bool				canHaveMineLayer();

	bool				getVideoShown();
	void				setVideoShown();




	struct Building
	{
		long nameID;
		long weight;
		long encycloID;
		long componentIDs[4];
		char fileName[64];
		float scale;
	};




private:

	bool		bNewMechs;
	bool		bNewPilots;
	bool		bNewWeapons;

	// The three by-value lists are APPEND-ONLY and filled once while the front-end data loads (initComponents, initPilots, addBuilding): an element address handed out
	// by &(*it) (getFirstAvailablePilot, the component and building arrays, LogisticsMech::setPilot) stays valid because std::deque never relocates its elements
	// on push_back / pop_back. Nothing erases from the middle. The only way to empty `pilots` is clearPilots(), which needs an empty inventory, because every mech
	// holds a LogisticsPilot* into it.
	using COMPONENT_LIST = std::deque<LogisticsComponent>;
	using PILOT_LIST = std::deque<LogisticsPilot>;
	// Owners preserve insertion order and stable pointee addresses; public lists are borrowed snapshots.
	using VARIANT_LIST = std::vector<std::unique_ptr<LogisticsVariant>>;
	using MECH_LIST = std::vector<std::unique_ptr<LogisticsMech>>;
	using VEHICLE_LIST = std::vector<std::unique_ptr<LogisticsVehicle>>;
	using BUILDING_LIST = std::deque<Building>;
	
	VARIANT_LIST									variants;
	MECH_LIST										inventory;
	COMPONENT_LIST									components;
	PILOT_LIST										pilots;
	VEHICLE_LIST									vehicles;
	BUILDING_LIST									buildings;
	
	// Explicitly destroyed with LogisticsData, before engine heaps shut down.
	std::unique_ptr<LogisticsPilot> vehiclePilot;
	bool vehiclePilotTried = false;
	std::unique_ptr<LogisticsMissionInfo>			missionInfo;

	// The mechs that were deployed in a mission, captured before they leave the inventory, so a mech that survives the mission can be put back by
	// its variant name even when the variants list no longer resolves that name (a mod-purchased variant). The variant is owned by the variants list.
	// The snapshot is cleared by removeMechsInForceGroup, which runs at every mission launch, so it never holds a variant of a campaign that was loaded over.
	struct DeployedMechSnapshot
	{
		std::string			variantName;
		LogisticsVariant*	pVariant = nullptr;
	};
	std::vector<DeployedMechSnapshot>				deployedMechSnapshot;
	

	LogisticsMech*									currentlyModifiedMech;
	LogisticsVariant*								oldVariant;
	std::unique_ptr<LogisticsVariant> modifiedVariant;
	
	
	int												resourcePoints; // C-Bills for buying mechs

	std::expected<void, long> loadVariant( FitIniFile& file );


	

	// HELPERS
	void RegisterFunctions();
	void UnRegisterFunctions();

	void initComponents();
	// EQUIP-ADD-FILL-1 (S2): idempotent append of ADD-pass rows to `components`
	// (second consumer). Called from initComponents + updateAvailability.
	void appendEquipmentAddFillComponents();
	void initPilots();
	void clearPilots();   // empties `pilots`; the inventory must be empty first (see the list contract above)
	void initVariants();
	void initPhaseA();
	bool m_phaseAComplete = false;
	bool m_phaseBComplete = false;

	void addVehicle( long fitID, PacketFile& objectFile, float scale, long nameID = 0, std::string_view csvName = {} );
	bool openLooseChassisFit( FitIniFile& file, std::string_view csvName ) const;
	std::expected<void, int> addBuilding( long fitID, PacketFile& objectFile, float scale, std::string_view csvName = {} );
	void removeDeadWeight();
	void clearVariants();




	

};


//*************************************************************************************************
#endif  // end of file ( LogisticsData.h )
