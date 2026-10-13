#define LOGISTICSDATA_CPP
/*************************************************************************************************\
LogisticsData.cpp			: Implementation of the LogisticsData component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "logisticsdata_internal.h"
#include "mc2fmt.h"


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
	for ( auto iter = variants.begin(); iter != variants.end(); ++iter )
	{
		delete( *iter );
	}

	for ( auto vIter = vehicles.begin(); vIter != vehicles.end(); ++vIter )
	{
		delete (*vIter );
	}

	for ( auto mIter = inventory.begin(); mIter != inventory.end(); ++mIter )
	{
		delete (*mIter);
	}

	variants.clear();
	vehicles.clear();

	delete missionInfo;
	missionInfo = nullptr;

#ifndef VIEWER
	ChatWindow::static_destroy();
#endif
}

/*long				LogisticsData::getMaxTeams() const
{
/	return missionInfo->getMaxTeams( );
}*/


void				LogisticsData::startNewCampaign( const char* fileName )
{
#ifndef VIEWER
	if ( MPlayer )
	{
		delete MPlayer;
		MPlayer = nullptr;
	}
#endif

	inventory.clear();
	resourcePoints = 0;
	pilots.Clear();
	initPilots();

	ActivateCampaignMod(fileName);

	FitIniFile file;

	FullPathFileName path;
	path.init( campaignPath, fileName, ".fit" );
	if ( NO_ERR != file.open( path ) )
	{
		STOP(("COuld not find file %s to load campaign",path));
	}
	missionInfo->init( file );

	// temporary, just so we can test
	int count = 32;
	const char* missionNames[32];
	missionInfo->getAvailableMissions( missionNames, count );

	setCurrentMission( missionNames[0] );

	soundSystem->setMusicVolume( prefs.MusicVolume );
	soundSystem->playDigitalMusic(missionInfo->getCurrentLogisticsTuneId());
}

void LogisticsData::startMultiPlayer()
{
	inventory.clear();
	resourcePoints = 0;
	pilots.Clear();
	initPilots();

	// kill all old designer mechs
	clearVariants();

	// need to initialize multiplayer variants here...
	char findString[512];
	mc2::fmt::to_buf(findString, "data" PATH_SEPARATOR "multiplayer" PATH_SEPARATOR "*.var");


#ifdef LINUX_BUILD
    // sebi !NB
    ENTER_DEBUGGER
#else
	WIN32_FIND_DATA	findResult;
	HANDLE searchHandle = FindFirstFile(findString,&findResult); 
	if (searchHandle != INVALID_HANDLE_VALUE)
	{
		do
		{
			if ((findResult.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			{
				FullPathFileName path;
				path.init("data" PATH_SEPARATOR "multiplayer" PATH_SEPARATOR, findResult.cFileName,"");
				decryptFile(path,"tmp.fit");

				FitIniFile file;
				file.open("tmp.fit");
				file.seekBlock( "Variant0" );

				loadVariant(file);
				file.close();

				DeleteFile("tmp.fit");
			}
		} while (FindNextFile(searchHandle,&findResult) != 0);

		FindClose(searchHandle);
	}
#endif

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
void				LogisticsData::setPurchaseFile( const char* fileName )
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
	for ( auto iter = variants.begin(); iter != variants.end(); ++iter )
		{
			if ( !(*iter)->isDesignerMech() )
			{
				if ( count < maxCount )
				{
					array[count] = (*iter)->getName().c_str();   // points into the variant; the caller uses it before the variants change
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
	for ( auto vIter = variants.begin(); vIter != variants.end(); ++vIter )
	{
		if ( (*vIter)->getName().find( "Prime" ) != std::string::npos && (*vIter)->isDesignerMech() )
		{
			if ( count < maxCount )
				pChassis[count] = (*vIter);
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
	for ( auto vIter = variants.begin(); vIter != variants.end(); ++vIter )
	{
		if ( (((*vIter)->getVariantID() >> 16 & 0xff) == 0) && 
			(*vIter)->getName().find( "Prime" ) == std::string::npos && (*vIter)->isDesignerMech() )
		{
			if ( count < maxCount )
				pChassis[count] = (*vIter);
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
	for ( auto vIter = vehicles.begin(); vIter != vehicles.end(); ++vIter )
	{
			if ( count < maxCount )
				pChassis[count] = (*vIter);
			else 
				retVal = NEED_BIGGER_ARRAY;

			count++;

		

	}

	return retVal;
}


LogisticsVehicle*	LogisticsData::getVehicle( const char* pName )
{
	char tmpStr[256];
	for ( auto vIter = vehicles.begin(); vIter != vehicles.end(); ++vIter )
	{
		cLoadString( (*vIter)->getNameID(), tmpStr, 255 );
		if ( S_stricmp( tmpStr, pName ) == 0 )
		{
			return *vIter;
		}
	}

	return nullptr;
}

int LogisticsData::addBuilding( long fitID, PacketFile& objectFile, float scale )
{
	if ( NO_ERR != objectFile.seekPacket(fitID) )
		return -1;

	int fileSize = objectFile.getPacketSize();

	if ( fileSize )
	{
		Building bldg;

		FitIniFile file;
		file.open(&objectFile, fileSize);
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
			for ( int i = 0; i < 4; i++ )
			{
				bldg.componentIDs[i] = 0;
			}
		}
		
		bldg.scale = scale;
		buildings.Append( bldg );

		
	}

	return 0;
}

//*************************************************************************************************
LogisticsComponent* LogisticsData::getComponent( int componentID )
{
	for ( COMPONENT_LIST::EIterator iter = components.Begin();
		!iter.IsDone(); iter++ )
	{
			if ( ((*iter).getID() & 0x000000ff) == (componentID & 0x000000ff) )
				return &(*iter);
	}

	return nullptr;
}

//*************************************************************************************************
LogisticsData::Building*			LogisticsData::getBuilding( int nameID )
{
	for ( BUILDING_LIST::EIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
		{
			if ( (*iter).nameID == nameID )
				return &(*iter);
		}

	return nullptr;
}


//*************************************************************************************************
int					LogisticsData::getBuildings( Building** bdgs, int& count )
{
	int maxCount = count;
	count = 0;
	int retVal = 0;

	for ( BUILDING_LIST::EIterator iter = buildings.Begin();
		!iter.IsDone(); iter++ )
	{
		if ( count < maxCount )
		{
			bdgs[count] = &(*iter);
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

void				LogisticsData::addNewBonusPurchaseFile( const char* pFileName )
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

void	LogisticsData::setSingleMission( const char* pName )
{
	missionInfo->setSingleMission( pName );
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
void	LogisticsData::setPilotUnused( const char* pName )
{
	const std::string wanted = mc2::str::from_c( pName );
	for ( PILOT_LIST::EIterator iter = pilots.Begin(); !iter.IsDone(); iter++  )
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
