#ifndef LOGISTICSDATA_H
#define LOGISTICSDATA_H
/*************************************************************************************************\
LogisticsData.h			: Interface for the LogisticsData component.  This thing holds the inventory
							lists and the component lists
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"stuff/stuff.hpp"
#include<elist.h>
#include <string>
#include <vector>

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
	void			ensureMechBayDataLoaded(const char* trigger = nullptr);

	bool			rpJustAdded;

	static LogisticsData*		instance;

	LogisticsComponent* getComponent( int componentID );
	LogisticsPilot*		getPilot( int pilotID );
	LogisticsPilot*		getPilot( const char* pilotName );
	int					getPilotCount();
	int					getPilots( LogisticsPilot**, long& count );
	const char*			getBestPilot( long mechWeight );
	bool				gotPilotsLeft();

	void				setPilotUnused( const char* pPilot );

	int					getVariantsInInventory( LogisticsVariant* pVar, bool bIncludeForceGroup );
	int					getPlayerVariantNames( const char**, int& count );
	int					getChassisVariants( const LogisticsChassis* pChassis, LogisticsVariant** pVar, int& count );
	
	LogisticsVariant*	getVariant( int ID );
	LogisticsVariant*	getVariant( const char* mechName );
	LogisticsVariant*	getVariant( const char* pCSVFileNaem, int VariantNum );
	int					removeVariant( const char* varName );


	LogisticsMech*		getMech( int ID );
	LogisticsMech*		getMech( const char* MechName, const char* pilotName );
	LogisticsMech*		getMechWithoutForceGroup( LogisticsMech* clone );

	void				addMechToInventory( LogisticsVariant* pVar, LogisticsPilot* pPilot, int ForceGrup,
											bool bSubtractPts = 0);




	int createInstanceID( LogisticsVariant* pVar );
	LogisticsPilot*		getFirstAvailablePilot();
	// VEHICLE-AUTO-PILOT-1: a deployed vehicle/support unit auto-gets a standard
	// vehicle pilot so it satisfies the launch pilot-requirement without consuming a
	// roster mech-pilot. isVehicleUnit() = object class != BATTLEMECH; getVehiclePilot()
	// returns a shared, hidden-from-roster (setAvailable(false)) standard vehicle pilot.
	bool				isVehicleUnit( class LogisticsMech* pMech );
	LogisticsPilot*		getVehiclePilot();
	void				getForceGroup( EList<LogisticsMech*, LogisticsMech*>& newList );
	void				getInventory( EList<LogisticsMech*, LogisticsMech*>& newList );


	void				removeMechsInForceGroup(); // takes mechs that are in the force group out of the inventory
	void				addMechToInventory( LogisticsVariant* pVar,
											int addToForceGroup, LogisticsPilot*,
											unsigned long color1 = 0xffffffff, 
											unsigned long color2 = 0xffffffff,
											unsigned long color3 = 0xffffffff);
	void				removeMechFromInventory( const char* mechName, const char* pilotName );

	int					addMechToForceGroup( LogisticsMech* pMech, int slot );
	int					removeMechFromForceGroup( LogisticsMech* pMech, bool bRemovePilot );
	int					removeMechFromForceGroup( int slot );

	bool				canAddMechToForceGroup( LogisticsMech* pMech );


	long				save( FitIniFile& file );
	long				load( FitIniFile& file );

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

	long				loadMech( FitIniFile& file, int& count );

	void				setMissionCompleted();
	long				updateAvailability();

	// Front-end screen state capture: emits a "LOGISTICS" diagnostic-trace event
	// (MCP-readable) with the current screen + bay/purchase/loadout/launch state
	// (purchasable mechs, inventory, force group, pilots, CB/RP, drop weight,
	// mission info). Self-gates on the LOGISTICS diag tag being enabled.
	void				dumpFrontEndState( const char* screenName );

	const std::string&	getCurrentMission() const;
	const std::string&	getLastMission() const;
	void				setSingleMission( const char* pName );
	bool				isSingleMission();
	long				getCurrentMissionTune();
	// The text getters below return a copy; an empty string means "there is none" (logisticsmissioninfo.h).
	std::string			getCurrentABLScript() const;
	long 				getCurrentMissionId();

	void				clearInventory();

	int					getPurchasableMechs( LogisticsVariant**, int& count );
	int					canPurchaseMech( LogisticsVariant* pVar );
	int					purchaseMech( LogisticsVariant* pVar );
	int					sellMech( LogisticsMech* pVar );
	int					getEncyclopediaMechs( const LogisticsVariant**, int& count );
	int					getHelicopters( const LogisticsVariant**, int& count );
	
	int					getVehicles( const LogisticsVehicle**, int& count );
	LogisticsVehicle*	getVehicle( const char* pName );


	int					setMechToModify( LogisticsMech* pVariant );
	LogisticsMech*		getMechToModify( ){ return currentlyModifiedMech; }
	int					acceptMechModifications( const char* pNewVariantName );
	int					cancelMechModfications();
	bool				canReplaceVariant( const char* name );
	bool				canDeleteVariant( const char* name );

	void				setCurrentMissionNum (long cMission);
	long				getCurrentMissionNum (void);

	int					getAvailableComponents( LogisticsComponent** pComps, int& maxCount );
	int					getAllComponents( LogisticsComponent** pComps, int& maxCount );
	std::string			getCurrentOperationFileName();
	std::string			getCurrentVideoFileName();
	std::string			getCurrentMissionDescription();
	int					getAvailableMissions( const char** missionNames, long& count );
	int					getCurrentMissions( const char** missionNames, long& count );
	bool				getMissionAvailable( const char* missionName );
	std::string			getCurrentMissionFriendlyName( );
	std::string			getMissionFriendlyName( const char* missionName );
	long				getMaxTeams() const;
	long				getMaxPlayers() const;

	int					acceptMechModificationsUseOldVariant( const char* name );

	const std::string&	getCampaignName() const;




	int					setCurrentMission( const char* missionName );
	// Cross-stage mission set for the headless boot driver (MC2_BOOT_TO_MISSION):
	// jump to any mission regardless of stage/progression, then updateAvailability.
	int					setCurrentMissionAnyStage( const char* missionName );

	void				startNewCampaign( const char* fileName = "campaign");
	void				startMultiPlayer();
	void				setPurchaseFile( const char* fileName );

	Building*			getBuilding( int nameID );
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

	void				addNewBonusPurchaseFile( const char* pFileName ); 
	void				appendAvailability(const char* pFileName, bool* availableArray );

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

	typedef EList< LogisticsComponent, const LogisticsComponent& >	COMPONENT_LIST;
	typedef EList< LogisticsPilot, const LogisticsPilot& >			PILOT_LIST;
	typedef EList< LogisticsVariant*, LogisticsVariant* >			VARIANT_LIST;
	typedef EList< LogisticsMech*, LogisticsMech* >					MECH_LIST;
	typedef EList< LogisticsVehicle*, LogisticsVehicle* >			VEHICLE_LIST;
	typedef EList< Building, const Building& >						BUILDING_LIST;
	
	VARIANT_LIST									variants;
	MECH_LIST										inventory;
	COMPONENT_LIST									components;
	PILOT_LIST										pilots;
	VEHICLE_LIST									vehicles;
	BUILDING_LIST									buildings;
	
	LogisticsMissionInfo*							missionInfo;

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
	
	
	int												resourcePoints; // C-Bills for buying mechs

	long	loadVariant( FitIniFile& file );


	

	// HELPERS
	void RegisterFunctions();
	void UnRegisterFunctions();

	void initComponents();
	// EQUIP-ADD-FILL-1 (S2): idempotent append of ADD-pass rows to `components`
	// (second consumer). Called from initComponents + updateAvailability.
	void appendEquipmentAddFillComponents();
	void initPilots();
	void initVariants();
	void initPhaseA();
	bool m_phaseAComplete = false;
	bool m_phaseBComplete = false;

	void addVehicle( long fitID, PacketFile& objectFile, float scale, long nameID = 0 );
	int  addBuilding( long fitID, PacketFile& objectFile, float scale );
	void removeDeadWeight();
	void clearVariants();




	

};


//*************************************************************************************************
#endif  // end of file ( LogisticsData.h )
