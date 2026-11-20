#define LOGISTICSDATA_CPP
/*************************************************************************************************\
LogisticsData.cpp			: Implementation of the LogisticsData component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "logisticsdata_internal.h"
#include "mc2fmt.h"
#ifndef VIEWER
#include "net/runtime.h"
#endif
#include "vfs.h"
#include "utils/mc2log.h"


#ifdef VIEWER
	bool MissionResults::FirstTimeResults = false;
#endif

//----------------------------------------------------------------------
// WARNING!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
//PLEASE CHANGE THIS IF THE SAVEGAME FILE FORMAT CHANGES!!!!!!!!!!!!!
long SaveGameVersionNumber = 10004;
//----------------------------------------------------------------------

LogisticsData* LogisticsData::instance = nullptr;

//*************************************************************************************************
LogisticsData::LogisticsData()
{
	gosASSERT( !instance );
	instance = this;
	resourcePoints = 0;

	currentlyModifiedMech = nullptr;
	missionInfo = nullptr;
	rpJustAdded = false;

	bNewMechs = bNewWeapons = bNewPilots = 0;
	m_phaseAComplete = false;
	m_phaseBComplete = false;
}

LogisticsData::~LogisticsData()
{
	mc2::prewarm::service().cancelMain(m_phaseBJob);
	variants.clear();
	vehicles.clear();
	inventory.clear();
	vehiclePilot.reset();

	missionInfo.reset();

#ifndef VIEWER
	ChatWindow::static_destroy();
#endif
}

const mc2::rules::GameRules& LogisticsData::rules() const
{
	static const mc2::rules::GameRules defaults;
	return missionInfo ? missionInfo->getRules() : defaults;
}

void LogisticsData::setRules( const mc2::rules::GameRules& r )
{
	if ( missionInfo )
		missionInfo->setRules( r );
}

void LogisticsData::publishRules() const
{
#ifndef VIEWER
	// Multiplayer (the legacy MPlayer or the lockstep mc2::net session) plays under the defaults: the wire carries no rules.
	const bool networked = MPlayer || mc2::net::active();
	const mc2::rules::GameRules published = mc2::rules::rulesToPublish( networked, rules() );
	mc2::rules::publish( published );
	MC2_LOG(Io, Info, "[GAME_RULES] snapshot networked={} calledShots={} pilotSkillWeight={}", networked, published.calledShots, published.pilotSkillWeight);
#else
	mc2::rules::publish( rules() );
#endif
}

/*long				LogisticsData::getMaxTeams() const
{
/	return missionInfo->getMaxTeams( );
}*/


bool				LogisticsData::startNewCampaign( mc2::czstring fileName )
{
	// A missing or unreadable campaign file is reported and refused BEFORE any state is cleared; it used to be a STOP (fatal).
	ActivateCampaignMod(fileName);

	FitIniFile file;

	FullPathFileName path;
	path.init( campaignPath, fileName, ".fit" );
	if ( NO_ERR != file.open( path ) )
	{
		MC2_LOG(Io, Error, "campaign: could not find file {} to load campaign, staying on the current screen", static_cast<const char*>(path));
		return false;
	}

	// Parse and validate into a throwaway first: a broken campaign file must leave the running campaign, pilots and inventory untouched.
	{
		LogisticsMissionInfo probe;
		if ( !probe.init( file ) )
		{
			MC2_LOG(Io, Error, "campaign: file {} could not be initialized as a campaign, nothing was changed", static_cast<const char*>(path));
			return false;
		}
		int probeCount = 32;
		const char* probeNames[32];
		probe.getAvailableMissions( probeNames, probeCount );
		if ( probeCount < 1 )
		{
			MC2_LOG(Io, Error, "campaign: file {} lists no available mission, nothing was changed", static_cast<const char*>(path));
			return false;
		}
	}

	ensureMechBayDataLoaded("campaign-start");
#ifndef VIEWER
	if ( MPlayer )
	{
		delete MPlayer;
		MPlayer = nullptr;
	}
#endif

	clearInventory();
	resourcePoints = 0;
	clearPilots();
	initPilots();

	if ( !missionInfo->init( file ) )
	{
		MC2_LOG(Io, Error, "campaign: file {} could not be initialized as a campaign", static_cast<const char*>(path));
		return false;
	}
	missionInfo->setRules( prefs.gameRules );	// the last-used rules from options.cfg (defaults until the rules dialog saves some)

	// temporary, just so we can test
	int count = 32;
	const char* missionNames[32];
	missionInfo->getAvailableMissions( missionNames, count );
	if ( count < 1 )
	{
		MC2_LOG(Io, Error, "campaign: file {} lists no available mission", static_cast<const char*>(path));
		return false;
	}

	setCurrentMission( missionNames[0] );

	soundSystem->setMusicVolume( prefs.MusicVolume );
	soundSystem->playDigitalMusic(missionInfo->getCurrentLogisticsTuneId().value_or(-1));
	return true;
}

void LogisticsData::startMultiPlayer()
{
	clearInventory();
	resourcePoints = 0;
	clearPilots();
	initPilots();

	// kill all old designer mechs
	clearVariants();

	// need to initialize multiplayer variants here...
	// Variants come from the merged VFS index (mods + loose + FST), on every platform (the old
	// Linux branch entered the debugger). decryptFile opens through File::open, so the logical
	// path resolves to whichever provider wins.
	for ( const auto& varEntry : mc2::vfs::enumerate( "data/multiplayer/", ".var" ) )
	{
		FullPathFileName path;
		path.init("data" PATH_SEPARATOR "multiplayer" PATH_SEPARATOR, varEntry.name.c_str(),"");
		decryptFile(path,"tmp.fit");

		FitIniFile file;
		file.open("tmp.fit");
		file.seekBlock( "Variant0" );

		loadVariant(file);
		file.close();

		DeleteFile("tmp.fit");
	}

	missionInfo->setMultiplayer();

#if !defined(VIEWER) && MC2_MULTIPLAYER
	// The one site that creates the session layer; without MC2_MULTIPLAYER (CMake, default OFF) MPlayer stays null for the
	// life of the process and every `if (MPlayer)` path is the single-player one.
	if ( !MPlayer )
	{
		MPlayer = new MultiPlayer;
		MPlayer->setup();

		if ( !strlen( &prefs.playerName[0][0] ) )
		{
			cLoadString( IDS_UNNAMED_PLAYER, &prefs.playerName[0][0], 255 );
		}

		ChatWindow::init();
	}
#endif

}
void				LogisticsData::setPurchaseFile( mc2::czstring fileName )
{
	missionInfo->setPurchaseFile( fileName );
	if ( MPlayer )
		clearInventory();
	updateAvailability();

}


int					LogisticsData::getCBills() 
{ 
	return missionInfo->getCBills(); 
}
void				LogisticsData::addCBills( int amount )
{ 
	missionInfo->incrementCBills(amount); 
}
void				LogisticsData::decrementCBills( int amount ) 
{ 
	missionInfo->decrementCBills(amount); 
}

int					LogisticsData::getPlayerVariantNames( const char** array, int& count )
{
	int maxCount = count;
	count = 0;

	int retVal = 0;
	for (auto & variant : variants)
		{
			if ( !variant->isDesignerMech() )
			{
				if ( count < maxCount )
				{
					array[count] = variant->getName().c_str();   // points into the variant; the caller uses it before the variants change
				}
				else
				{
					retVal = NEED_BIGGER_ARRAY;
				}
				count++;
			}
		}

		return retVal;
}

int		LogisticsData::getEncyclopediaMechs( const LogisticsVariant** pChassis, int& count )
{
	int retVal = 0;
	int maxCount = count;
	count = 0;
	for (auto & variant : variants)
	{
		if ( variant->getName().find( "Prime" ) != std::string::npos && variant->isDesignerMech() )
		{
			if ( count < maxCount )
				pChassis[count] = variant.get();
			else 
				retVal = NEED_BIGGER_ARRAY;

			count++;

		}

	}

	return retVal;
}

int	LogisticsData::getHelicopters( const LogisticsVariant** pChassis, int& count )
{
	int retVal = 0;
	int maxCount = count;
	count = 0;
	for (auto & variant : variants)
	{
		if ( ((variant->getVariantID() >> 16 & 0xff) == 0) && 
			variant->getName().find( "Prime" ) == std::string::npos && variant->isDesignerMech() )
		{
			if ( count < maxCount )
				pChassis[count] = variant.get();
			else 
				retVal = NEED_BIGGER_ARRAY;

			count++;

		}

	}

	return retVal;
}


int		LogisticsData::getVehicles( const LogisticsVehicle** pChassis, int& count )
{
	int retVal = 0;
	int maxCount = count;
	count = 0;
	for (auto & vehicle : vehicles)
	{
			if ( count < maxCount )
				pChassis[count] = vehicle.get();
			else 
				retVal = NEED_BIGGER_ARRAY;

			count++;

		

	}

	return retVal;
}


std::optional<std::reference_wrapper<LogisticsVehicle>> LogisticsData::getVehicle( mc2::czstring pName )
{
	char tmpStr[256];
	for (auto & vehicle : vehicles)
	{
		cLoadString( vehicle->getNameID(), tmpStr, 255 );
		if ( S_stricmp( tmpStr, pName ) == 0 )
		{
			return *vehicle;
		}
	}

	return std::nullopt;
}

std::expected<void, int> LogisticsData::addBuilding( long fitID, PacketFile& objectFile, float scale, std::string_view csvName )
{
	// Mod buildings.csv rows (MCO) carry FitIDs past a mod-sized object2.pak; take their definition from the loose
	// data/objects/<File Name>.fit instead (same CSV-chassis path as ObjectTypeManager and addVehicle).
	FitIniFile file;
	const bool inPak = fitID >= 0 && fitID < objectFile.getNumPackets();
	if ( inPak ? ( NO_ERR != objectFile.seekPacket(fitID) || !objectFile.getPacketSize() || NO_ERR != file.open(&objectFile, objectFile.getPacketSize()) )
	           : !openLooseChassisFit( file, csvName ) )
		return std::unexpected(-1);

	{
		Building bldg;

		if ( NO_ERR != file.seekBlock( "ObjectType" ) )
			gosASSERT( 0 );

		file.readIdString( "AppearanceName", bldg.fileName, 63 );
		file.readIdLong( "EncyclopediaID", bldg.encycloID );


		bool bIsTurret = 0;

		if ( NO_ERR != file.seekBlock( "BuildingData" ) )
		{
			if ( NO_ERR != file.seekBlock( "GateData" ) )
			{
				if ( NO_ERR != file.seekBlock( "TurretData" ) )
				{
					if ( NO_ERR != file.seekBlock( "General" ) ) // hack for artillery piece
					{
						char errorStr[256];
						mc2::fmt::to_buf(errorStr, "coudn't find appropriate block in file {}", mc2::fmt::safe(bldg.fileName));
						Assert( 0, 0, errorStr  );
					}
				}
				else
					bIsTurret = true;
			}
		}
		unsigned long tmp;
		file.readIdLong( "BuildingName", bldg.nameID );
		file.readIdULong( "DmgLevel", tmp );
		bldg.weight = tmp;
		if ( bIsTurret )
		{
			char weaponNameStr[64];
			strcpy( weaponNameStr, "WeaponType" );
			for ( int i = 0; i < 4; i++ )
			{
				
				file.readIdLong( weaponNameStr, bldg.componentIDs[i] );
				mc2::fmt::to_buf(weaponNameStr, "WeaponType{:d}", mc2::fmt::as_i32(i+1));
			}
			
		}
		else
		{
			for (long & componentID : bldg.componentIDs)
			{
				componentID = 0;
			}
		}
		
		bldg.scale = scale;
		buildings.push_back( bldg );

		
	}

	return {};
}

//*************************************************************************************************
std::optional<std::reference_wrapper<LogisticsComponent>> LogisticsData::getComponent( int componentID )
{
	for (auto & component : components)
	{
			if ( (component.getID() & 0x000000ff) == (componentID & 0x000000ff) )
				return component;
	}

	return std::nullopt;
}

//*************************************************************************************************
std::optional<std::reference_wrapper<LogisticsData::Building>> LogisticsData::getBuilding( int nameID )
{
	for (auto & building : buildings)
		{
			if ( building.nameID == nameID )
				return building;
		}

	return std::nullopt;
}


//*************************************************************************************************
int					LogisticsData::getBuildings( Building** bdgs, int& count )
{
	int maxCount = count;
	count = 0;
	int retVal = 0;

	for (auto & building : buildings)
	{
		if ( count < maxCount )
		{
			bdgs[count] = &building;
		}
		else
			retVal = NEED_BIGGER_ARRAY;

		count++;
	}


	return retVal;
	
}

const std::string&	LogisticsData::getCampaignName() const
{ 
	return missionInfo->getCampaignName();
}



bool				LogisticsData::campaignOver() 
{ 
	return missionInfo->campaignOver();
}
std::string			LogisticsData::getCurrentBigVideo() const 
{ 
	return missionInfo->getCurrentBigVideo(); 
}
std::string			LogisticsData::getFinalVideo() const
{ 
	return missionInfo->getFinalVideo();
}

void				LogisticsData::addNewBonusPurchaseFile( mc2::czstring pFileName )
{
	missionInfo->addBonusPurchaseFile( pFileName );
}

bool				LogisticsData::skipLogistics()
{
	return missionInfo->skipLogistics();
}
bool				LogisticsData::skipPilotReview()
{
	return missionInfo->skipPilotReview();
}
bool				LogisticsData::skipSalvageScreen()
{
	return missionInfo->skipSalvageScreen();
}
bool				LogisticsData::skipPurchasing()
{
	return missionInfo->skipPurchasing();
}

bool				LogisticsData::showChooseMission()
{
	return missionInfo->showChooseMission();
}

void	LogisticsData::setSingleMission( mc2::czstring pName )
{
	missionInfo->setSingleMission( pName );
	missionInfo->setRules( prefs.gameRules );	// after clear(): solo remembers the last-used rules (options.cfg)
	clearVariants();
	clearInventory();
	initPilots(); // reset pilotsb
	updateAvailability();
}

bool	LogisticsData::isSingleMission()
{
	if ( missionInfo )
	{
		return missionInfo->isSingleMission();
	}
	
	return 0;
}

bool LogisticsData::canHaveSalavageCraft()
{
	if ( !missionInfo )
		return true;
		
	return missionInfo->canHaveSalavageCraft();
}
bool LogisticsData::canHaveRepairTruck()
{
	if ( !missionInfo )
		return true;
	return missionInfo->canHaveRepairTruck();
}
bool LogisticsData::canHaveScoutCopter()
{
	if ( !missionInfo )
		return true;
	return missionInfo->canHaveScoutCopter();
}
bool LogisticsData::canHaveArtilleryPiece()
{
	if ( !missionInfo )
		return true;
	return missionInfo->canHaveArtilleryPiece();
}
bool LogisticsData::canHaveAirStrike()
{
	if ( !missionInfo )
		return true;
	return missionInfo->canHaveAirStrike();
}
bool LogisticsData::canHaveSensorStrike()
{
	if ( !missionInfo )
		return true;
	return missionInfo->canHaveSensorStrike();
}
bool LogisticsData::canHaveMineLayer()
{
	if ( !missionInfo )
		return true;
	return missionInfo->canHaveMineLayer();
}

bool				LogisticsData::getVideoShown()
{
	if ( !missionInfo )
		return true;
	return missionInfo->getVideoShown();
}
void				LogisticsData::setVideoShown()
{
	if ( missionInfo )
		missionInfo->setVideoShown();

}
void	LogisticsData::setPilotUnused( mc2::czstring pName )
{
	const std::string wanted = mc2::str::from_c( pName );
	for ( auto iter = pilots.begin(); iter != pilots.end(); ++iter )
	{
		if ( mc2::str::iequals( (*iter).getFileName(), wanted ) )
		{
			(*iter).setUsed( 0 );
			break;
		}
	}


}

void LogisticsData::setCurrentMissionNum (long cMission)
{
	missionInfo->setCurrentMissionNumber(cMission);
}

long LogisticsData::getCurrentMissionNum (void)
{
	return missionInfo->getCurrentMissionNumber();
}

void LogisticsData::setSkipLogistics(bool skip)
{
	if (missionInfo) {
		missionInfo->setSkipLogistics(skip);
	}
}
