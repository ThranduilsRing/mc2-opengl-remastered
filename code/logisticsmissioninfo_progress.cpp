// LogisticsMissionInfo: available / current mission lists, mission selection and completion.
// Declared in the class headers; shared names in logisticsmissioninfo_internal.h.
#include "logisticsmissioninfo_internal.h"
#include "mc2fmt.h"
#include "mc2str.h"


long LogisticsMissionInfo::getAvailableMissions( const char** missions, int& numberOfEm )
{
	if ( !groups || currentStage < 0 || currentStage >= groupCount )
	{
		numberOfEm = 0;
		return 0;
	}

	MissionGroup* pGroup = &groups[currentStage];

	int count = 0;
	int maxOut = numberOfEm;

	auto iter = pGroup->infos.begin();
	while ( iter != pGroup->infos.end() )
	{
		if ( !(*iter)->completed )
		{
			if ( count < maxOut )
			{
				// A pointer into a mission record that stays alive until clear().
				missions[count] = (*iter)->fileName.c_str();
			}

			count++;

		}
		iter++;
	}

	numberOfEm = count;

	if ( count > maxOut )
	{
		return NEED_BIGGER_ARRAY;
	}

	return 0;
}

long LogisticsMissionInfo::getCurrentMissions( const char** missions, int& numberOfEm )
{
	if ( !groups || currentStage < 0 || currentStage >= groupCount )
	{
		numberOfEm = 0;
		return 0;
	}

	MissionGroup* pGroup = &groups[currentStage];

	int count = 0;
	int maxOut = numberOfEm;

	auto iter = pGroup->infos.begin();
	while ( iter != pGroup->infos.end() )
	{
		if ( !(*iter)->hidden )
		{
			if ( count < maxOut )
			{
				missions[count] = (*iter)->fileName.c_str();
			}

			count++;
		}

		iter++;
	}

	numberOfEm = count;

	if ( count > maxOut )
	{
		return NEED_BIGGER_ARRAY;
	}

	return 0;
}
std::expected<void, long> LogisticsMissionInfo::setNextMission( mc2::czstring missionName )
{
	if ( !missionName || !strlen( missionName ) )
		return std::unexpected(-1L);
	if ( bMultiplayer )
	{
			const std::string path = std::string( missionPath ) + missionName + ".fit";

			currentMission = 0;
			currentStage = 0;
			MissionInfo* pInfo = groups[0].mission(0);
				
			pInfo->fileName = missionName;
			pInfo->completed = 0;
			pInfo->playPurchasing = true;

			currentMissionName = missionName;
			
				
			FitIniFile missionFile;
			
			if ( NO_ERR != missionFile.open( path.c_str() ) )
			{
				char errorStr[256];
				mc2::fmt::to_buf(errorStr, "couldn't open file {}", mc2::fmt::safe(missionName));
				Assert( 0, 0, errorStr );
				return std::unexpected(-1L);
			}

			readMissionInfo( missionFile, pInfo );
	
//			result = missionFile.readIdLong( "MaximumNumberOfTeams", maxTeams );
//			result = missionFile.readIdLong( "MaximumNumberOfPlayers", maxPlayers );
			
			MC2Player* pPlayerInfo =MPlayer->getPlayerInfo( MPlayer->commanderID );;
			if ( pPlayerInfo )
			{
				pInfo->resourcePoints = MPlayer->missionSettings.resourcePoints;
				pInfo->additionalCBills = pPlayerInfo->cBills;
				// need to put dropweight in here as sooon as we have it
				
			}

			pInfo->dropWeight = MPlayer->missionSettings.dropWeight;
			missionFile.close();	

			CBills = pInfo->additionalCBills;

			pInfo->enableAirStrike = MPlayer->missionSettings.airStrike;
			pInfo->enableArtilleryPiece = MPlayer->missionSettings.guardTower;
			pInfo->enableRepairTruck = MPlayer->missionSettings.repairVehicle;
			pInfo->enableSensorStrike = MPlayer->missionSettings.sensorProbe;
			pInfo->enableScoutCopter = MPlayer->missionSettings.scoutCopter;
			pInfo->enableMineLayer = MPlayer->missionSettings.mineLayer;
			pInfo->enableSalavageCraft = MPlayer->missionSettings.recoveryTeam;
	}
	else
	{
		if ( !groups || currentStage < 0 || currentStage >= groupCount )
			return std::unexpected(INVALID_MISSION);
		MissionGroup* pGroup = &groups[currentStage];

		int count = 0;
		bool bFound = 0;

		auto iter = pGroup->infos.begin();
		while ( iter != pGroup->infos.end() )
		{
			if ( mc2::str::iequals( (*iter)->fileName, missionName ) )
			{
				bFound = 1;
			
				if ( (*iter)->completed )
				{
					return std::unexpected(MISSION_ALREADY_DONE);
				}
				else
				{
					if ( currentMission != -1 )
					{
						MissionInfo* pInfo = pGroup->mission(currentMission);
						if ( !(pInfo)->completed )
							CBills -= pInfo->additionalCBills;
					}
		
					currentMission = count;
					currentMissionName = mc2::str::erase_first( (*iter)->fileName, ".fit" );
					CBills += (*iter)->additionalCBills;
				}

				break;
			}
			count++;
			iter++;
		}

		if ( !bFound )
			return std::unexpected(INVALID_MISSION);
	}

	

	return {};

}

std::expected<void, long> LogisticsMissionInfo::setMissionAnyStage( mc2::czstring missionName )
{
	// Cross-stage variant of setNextMission for the headless boot driver: find the
	// mission by FileName across ALL groups (not just the current stage) and set
	// currentStage + currentMission so its logistics/purchase screen loads, with no
	// campaign-progression requirement. Ignores the completed gate on purpose.
	if ( !missionName || !strlen( missionName ) )
		return std::unexpected(-1L);
	for ( long s = 0; s < groupCount; s++ )
	{
		MissionGroup* pGroup = &groups[s];
		long count = 0;
		for (auto & info : pGroup->infos)
		{
			if ( mc2::str::iequals( info->fileName, missionName ) )
			{
				currentStage = s;
				currentMission = count;
				currentMissionName = mc2::str::erase_first( info->fileName, ".fit" );
				return {};
			}
			count++;
		}
	}
	return std::unexpected(INVALID_MISSION);
}

void LogisticsMissionInfo::setSingleMission( mc2::czstring missionFileName )
{
	clear();
	campaignName.clear();   // a skirmish save must not carry the last campaign's file name
	bMultiplayer = true;
	groups = std::make_unique_for_overwrite<MissionGroup[]>(1);

	currentStage = 0;

	auto info = std::make_unique<MissionInfo>();
	MissionInfo* pInfo = info.get();
	groups[0].infos.push_back( std::move(info) );

	groups[0].operationFileName = "MCL_CM_Op1_1.fit";
	groups[0].ablBrainName = "";
	groups[0].bigVideoName = "";
	groups[0].bigVideoShown = true;
	groups[0].logisticsTuneId = 0;
	groups[0].numberToBeCompleted = 1;
	groups[0].videoFileName = "";

	groupCount = 1;


	
	const std::string path = std::string( missionPath ) + mc2::str::from_c( missionFileName ) + ".fit";

	currentMission = 0;
	currentStage = 0;
		
	pInfo->fileName = mc2::str::from_c( missionName );
	pInfo->completed = 0;
	pInfo->playMissionSelection = 0;
	pInfo->playPurchasing = true;
	pInfo->playSalvage = 0;
	pInfo->playPilotPromotion = 0;

	currentMissionName = mc2::str::from_c( missionFileName );
	
		
	FitIniFile missionFile;
	
	if ( NO_ERR != missionFile.open( path.c_str() ) )
	{
		char errorStr[256];
		mc2::fmt::to_buf(errorStr, "couldn't open file {}", mc2::fmt::safe(missionName));
		Assert( 0, 0, errorStr );
		return;
	}


	readMissionInfo( missionFile, pInfo );

	float fTmp;
	long result = missionFile.readIdFloat( "DropWeightLimit", fTmp );
	pInfo->dropWeight = fTmp;

	result = missionFile.readIdLong("AdditionalCBills", pInfo->additionalCBills );
	if ( result != NO_ERR || !pInfo->additionalCBills )
		pInfo->additionalCBills = 1200 * fTmp + 100000;
	CBills = pInfo->additionalCBills;
	
}

long LogisticsMissionInfo::getCurrentMissionId( )
{
	long missionId = 0;
	for (long i=0;i<=lastStage;i++)
	{
		MissionGroup* pGroup = &groups[i];

		if (i == lastStage)
		{
			missionId += lastMission;
		}
		else
		{
			long count = 0;
			auto iter = pGroup->infos.begin();
			while ( iter != pGroup->infos.end() )
			{
				count++;
				iter++;
			}

			missionId += count;
		}
	}

	return missionId;
}

void LogisticsMissionInfo::setMissionComplete( )
{
	if ( currentStage >= groupCount )
	{
		return;
	}

	//Needed to tell pilots which mission they just completed.
	lastStage = currentStage;
	lastMission = currentMission;
	lastMissionName = currentMissionName;

	MissionGroup* pGroup = &groups[currentStage];
	if ( pGroup && currentMission > -1 )
	{
		CBills += pGroup->mission(currentMission)->additionalCBills;
	}
	int count = 0;
	int numberCompleted = 0;
	bool bAllMandatoryCompleted = 1;
	int nextAvailableMission = -1;

	// figure out whether to go to the next stage orn not
	auto iter = pGroup->infos.begin();
	while ( iter != pGroup->infos.end() )
	{
		if ( currentMission == count )
		{
			(*iter)->completed = 1;
		}
		if ( (*iter)->completed )
		{
			numberCompleted ++;
		}
		else 
		{
			if ( nextAvailableMission == -1 )
				nextAvailableMission = count;

			if ( (*iter)->mandatory )
				bAllMandatoryCompleted = 0;
		}
				
		count++;
		iter++;
	}

	if ( numberCompleted >= pGroup->numberToBeCompleted  )
	{
		if ( bAllMandatoryCompleted )
		{
			currentStage++;
			currentMission = 0;
		}
		else 
		{
			auto iter = pGroup->infos.begin();
			while ( iter != pGroup->infos.end() )
			{
				if ( (*iter)->hidden ) // use the hidden video file name for this
				{
					if ( !(*iter)->videoName.empty() )
					{
						pGroup->videoFileName = (*iter)->videoName;
						pGroup->videoShown = 0;
					}
				}
				(*iter)->hidden = 0;
				iter++;
			}
			currentMission = nextAvailableMission;
		}
	}
	else
	{
		gosASSERT( nextAvailableMission != -1 );
		currentMission = nextAvailableMission;
	}

	if ( currentStage < groupCount )
	{
		currentMissionName = groups[currentStage].mission(currentMission)->fileName;
	}
}
