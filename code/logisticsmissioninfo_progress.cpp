// LogisticsMissionInfo: available / current mission lists, mission selection and completion.
// Declared in the class headers; shared names in logisticsmissioninfo_internal.h.
#include "logisticsmissioninfo_internal.h"


long LogisticsMissionInfo::getAvailableMissions( const char** missions, int& numberOfEm )
{
	MissionGroup* pGroup = &groups[currentStage];

	int count = 0;
	int maxOut = numberOfEm;

	MISSION_LIST::EIterator iter = pGroup->infos.Begin();
	while( !iter.IsDone() )
	{
		if ( !(*iter)->completed )
		{
			if ( count < maxOut )
			{
				missions[count] = (*iter)->fileName;
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
	MissionGroup* pGroup = &groups[currentStage];

	int count = 0;
	int maxOut = numberOfEm;

	MISSION_LIST::EIterator iter = pGroup->infos.Begin();
	while( !iter.IsDone() )
	{
		if ( !(*iter)->hidden )
		{
			if ( count < maxOut )
			{
				missions[count] = (*iter)->fileName;
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
long LogisticsMissionInfo::setNextMission( const char* missionName )
{
	if ( !missionName || !strlen( missionName ) )
		return -1;
	if ( bMultiplayer )
	{
			EString path = missionPath;
			path += missionName;
			path += ".fit";

			currentMission = 0;
			currentStage = 0;
			MissionInfo* pInfo = groups[0].infos[0];
				
			pInfo->fileName = missionName;
			pInfo->completed = 0;
			pInfo->playPurchasing = true;

			currentMissionName = missionName;
			
				
			FitIniFile missionFile;
			
			if ( NO_ERR != missionFile.open( (char*)(const char*)path ) )
			{
				char errorStr[256];
				sprintf( errorStr, "couldn't open file %s", missionName );
				Assert( 0, 0, errorStr );
				return -1;
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
		MissionGroup* pGroup = &groups[currentStage];

		int count = 0;
		bool bFound = 0;

		MISSION_LIST::EIterator iter = pGroup->infos.Begin();
		while( !iter.IsDone() )
		{
			if ( S_stricmp( (*iter)->fileName, missionName ) == 0 )
			{
				bFound = 1;
			
				if ( (*iter)->completed )
				{
					return MISSION_ALREADY_DONE;
				}
				else
				{
					if ( currentMission != -1 )
					{
						MissionInfo* pInfo = pGroup->infos[currentMission];
						if ( !(pInfo)->completed )
							CBills -= pInfo->additionalCBills;
					}
		
					currentMission = count;
					currentMissionName = (*iter)->fileName;
					currentMissionName.Remove( EString(".fit") );
					CBills += (*iter)->additionalCBills;
				}

				break;
			}
			count++;
			iter++;
		}

		if ( !bFound )
			return INVALID_MISSION;
	}

	

	return 0;

}

long LogisticsMissionInfo::setMissionAnyStage( const char* missionName )
{
	// Cross-stage variant of setNextMission for the headless boot driver: find the
	// mission by FileName across ALL groups (not just the current stage) and set
	// currentStage + currentMission so its logistics/purchase screen loads, with no
	// campaign-progression requirement. Ignores the completed gate on purpose.
	if ( !missionName || !strlen( missionName ) )
		return -1;
	for ( long s = 0; s < groupCount; s++ )
	{
		MissionGroup* pGroup = &groups[s];
		long count = 0;
		for ( MISSION_LIST::EIterator iter = pGroup->infos.Begin(); !iter.IsDone(); iter++ )
		{
			if ( S_stricmp( (*iter)->fileName, missionName ) == 0 )
			{
				currentStage = s;
				currentMission = count;
				currentMissionName = (*iter)->fileName;
				currentMissionName.Remove( EString( ".fit" ) );
				return 0;
			}
			count++;
		}
	}
	return INVALID_MISSION;
}

void LogisticsMissionInfo::setSingleMission( const char* missionFileName )
{
	clear();
	bMultiplayer = true;
	groups = new MissionGroup[1];

	currentStage = 0;

	MissionInfo* pInfo = new MissionInfo;
	groups[0].infos.Append( pInfo );

	groups[0].operationFileName = "MCL_CM_Op1_1.fit";
	groups[0].ablBrainName = "";
	groups[0].bigVideoName = "";
	groups[0].bigVideoShown = true;
	groups[0].logisticsTuneId = 0;
	groups[0].numberToBeCompleted = 1;
	groups[0].videoFileName = "";

	groupCount = 1;


	
	EString path = missionPath;
	path += missionFileName;
	path += ".fit";

	currentMission = 0;
	currentStage = 0;
		
	pInfo->fileName = missionName;
	pInfo->completed = 0;
	pInfo->playMissionSelection = 0;
	pInfo->playPurchasing = true;
	pInfo->playSalvage = 0;
	pInfo->playPilotPromotion = 0;

	currentMissionName = missionFileName;
	
		
	FitIniFile missionFile;
	
	if ( NO_ERR != missionFile.open( (char*)(const char*)path ) )
	{
		char errorStr[256];
		sprintf( errorStr, "couldn't open file %s", missionName );
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
			MISSION_LIST::EIterator iter = pGroup->infos.Begin();
			while( !iter.IsDone() )
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
		CBills += pGroup->infos[currentMission]->additionalCBills;
	}
	int count = 0;
	int numberCompleted = 0;
	bool bAllMandatoryCompleted = 1;
	int nextAvailableMission = -1;

	// figure out whether to go to the next stage orn not
	MISSION_LIST::EIterator iter = pGroup->infos.Begin();
	while( !iter.IsDone() )
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
			MISSION_LIST::EIterator iter = pGroup->infos.Begin();
			while( !iter.IsDone() )
			{
				if ( (*iter)->hidden ) // use the hidden video file name for this
				{
					if ( (*iter)->videoName.Length() )
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
		currentMissionName = groups[currentStage].infos[currentMission]->fileName;
	}
}
