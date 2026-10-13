// LogisticsMissionInfo: the per-mission accessors (purchase / operation files, videos, tune, flags) and the skip / multiplayer switches.
// Declared in the class headers; shared names in logisticsmissioninfo_internal.h.
#include "logisticsmissioninfo_internal.h"
#include "mc2str.h"


LogisticsMissionInfo::MissionInfo::~MissionInfo()
{
}

const std::string& LogisticsMissionInfo::getCurrentPurchaseFile() const
{
	MissionGroup* pGroup = &groups[currentStage];

	return pGroup->infos[currentMission]->purchaseFileName;
}

long LogisticsMissionInfo::getAllPurchaseFiles( const char** list, long& maxCount )
{
	long count = 0;
	long maxOut = maxCount;
	for ( long g = 0; g < groupCount; g++ )
	{
		MISSION_LIST::EIterator iter = groups[g].infos.Begin();
		while ( !iter.IsDone() )
		{
			// The pointer is into a mission record that stays alive until clear().
			const char* pf = (*iter)->purchaseFileName.c_str();
			iter++;
			if ( !pf[0] )
				continue;
			bool dup = false;
			for ( long k = 0; k < count && k < maxOut; k++ )
				if ( list[k] && _stricmp( list[k], pf ) == 0 ) { dup = true; break; }
			if ( dup )
				continue;
			if ( count < maxOut )
				list[count] = pf;
			count++;
		}
	}
	maxCount = count;
	return ( count > maxOut ) ? NEED_BIGGER_ARRAY : 0;
}

long LogisticsMissionInfo::getCurrentDropWeight() const
{
	MissionGroup* pGroup = &groups[currentStage];

	if ( currentMission != -1 )	
		return pGroup->infos[currentMission]->dropWeight;
	else
		return 10000;
}

std::string LogisticsMissionInfo::getCurrentOperationFile() const
{
	// Defense-in-depth: bg init (STARTUP-INIT-ASYNC-1) may not have populated
	// groups yet. &groups[currentStage] is address-of an element, so the old
	// `if (pGroup)` was dead-code-eliminated and the deref was unconditional UB.
	if ( !groups || currentStage < 0 || currentStage >= groupCount )
		return std::string();

	return groups[currentStage].operationFileName;
}

std::string LogisticsMissionInfo::getCurrentVideo() const
{
	if ( !groups || currentStage < 0 || currentStage >= groupCount )
		return std::string();

	MissionGroup* pGroup = &groups[currentStage];

	MissionInfo* pInfo = pGroup->infos[currentMission];
	if ( pInfo && !pInfo->videoName.empty() )
		return pInfo->videoName;
	return pGroup->videoFileName;
}

long LogisticsMissionInfo::getCurrentLogisticsTuneId()
{
	//sebi: ORIG BUG FIX: when campaign is finished currentStage is increased and finally reaches groupCount
	if (currentStage >= groupCount)
		return -1;

	MissionGroup* pGroup = &groups[currentStage];

	if ( pGroup )
		return pGroup->logisticsTuneId;	

	return -1;
}

std::string LogisticsMissionInfo::getCurrentMissionDescription() const
{
	//sebi
	if ( !groups || currentStage < 0 || currentStage >= groupCount )
		return std::string();

	return groups[currentStage].infos[currentMission]->description;
}

bool LogisticsMissionInfo::getMissionAvailable( const char* missionName )
{
	MissionGroup* pGroup = &groups[currentStage];

	for ( int i = 0; i < pGroup->infos.Count(); i++ )
	{
		if ( mc2::str::iequals( pGroup->infos[i]->fileName, mc2::str::from_c( missionName ) ) )
		{
			return !pGroup->infos[i]->completed;
		}
	}
	return false;
}

std::string LogisticsMissionInfo::getCurrentMissionFriendlyName() const
{
	if ( currentStage >= groupCount )
		return std::string();

	return groups[currentStage].infos[currentMission]->missionDescriptiveName;
}

std::string LogisticsMissionInfo::getCurrentABLScriptName() const
{
	if ( currentStage >= groupCount )
		return std::string();

	return groups[currentStage].ablBrainName;
}

std::string LogisticsMissionInfo::getMissionFriendlyName( const char* missionName ) const
{
	if ( currentStage >= groupCount )
		return std::string();
	MissionGroup* pGroup = &groups[currentStage];

	const std::string wanted = mc2::str::from_c( missionName );
	for ( int i = 0; i < pGroup->infos.Count(); i++ )
	{
		if ( mc2::str::iequals( pGroup->infos[i]->fileName, wanted ) )
		{
			return pGroup->infos[i]->missionDescriptiveName;
		}
	}
	return std::string();
}

int		LogisticsMissionInfo::getCurrentRP() const
{
	if ( currentMission == -1 )
		return 0;

	
	MissionGroup* pGroup = &groups[currentStage];
	return pGroup->infos[currentMission]->resourcePoints;

}

std::string LogisticsMissionInfo::getCurrentBigVideo() const
{
	// WHAT: hands out the group's pre-video once and marks it shown. WHY: the screen that plays it asks on every frame and must play it once.
	if ( !groups || currentStage < 0 || currentStage >= groupCount )
		return std::string();
	
	MissionGroup* pGroup = &groups[currentStage];

	if ( pGroup->bigVideoShown )
		return std::string();

	pGroup->bigVideoShown = true;
	return pGroup->bigVideoName;
}

std::string LogisticsMissionInfo::getFinalVideo() const
{
	return finalVideoName;
}

bool LogisticsMissionInfo::campaignOver() const
{
	return currentStage == groupCount;
}

void		LogisticsMissionInfo::setMultiplayer()
{
	clear();
	bMultiplayer = true;
	groups = new MissionGroup[1];

	currentStage = 0;

	MissionInfo* pInfo = new MissionInfo;
	groups[0].infos.Append( pInfo );

	currentMission = 0;
	groupCount = 1;


			

}
void		LogisticsMissionInfo::setPurchaseFile( const char* fileName )
{
	gosASSERT( bMultiplayer) ;

	MissionInfo* pInfo = groups[0].infos[0];

	pInfo->purchaseFileName = std::string( missionPath ) + mc2::str::from_c( fileName ) + ".fit";
}

int			LogisticsMissionInfo::getAdditionalPurachaseFiles( const char** list, long& maxCount )
{
	if ( maxCount >=  additionalPurchaseFiles.Count() )
	{
		long i = 0;
		
		for ( FILE_LIST::EIterator iter = additionalPurchaseFiles.Begin(); !iter.IsDone(); iter++ )
		{
			list[i++] = *iter;	
		}
	}

	maxCount = additionalPurchaseFiles.Count();

	return additionalPurchaseFiles.Count();
}
void				LogisticsMissionInfo::addBonusPurchaseFile( const char* fileName )
{
	if ( !fileName )
		return;

	char* pNewFile = new char[strlen( fileName )+1];
	strcpy( pNewFile, fileName ); 

 	additionalPurchaseFiles.Append( pNewFile );
}

bool	LogisticsMissionInfo::skipLogistics()
{
	if ( MPlayer )
		return 0;

	if (currentStage >= groupCount)
		return false;

	MissionGroup* pGroup = &groups[currentStage];
	return !pGroup->infos[currentMission]->playLogistics;

}

void LogisticsMissionInfo::setSkipLogistics(bool skip)
{
	// Smoke-mode helper. Bypasses the .fit-file-derived PlayLogistics flag
	// on the currently-selected mission info. Safe to call only after
	// setSingleMission() / setNextMission() has populated groups[currentStage].
	if (currentStage < groupCount && !groups[currentStage].infos.IsEmpty()) {
		groups[currentStage].infos[currentMission]->playLogistics = !skip;
	}
}
bool	LogisticsMissionInfo::skipPilotReview()
{
	// need to subtract a mission, since we already set this one complete if we
	// got here
	MissionInfo* pInfo = getPreviousMission();
	if ( !pInfo )
		return 0;
	return !pInfo->playPilotPromotion;
	

}
bool	LogisticsMissionInfo::skipSalvageScreen()
{
	// need to subtract a mission, since we already set this one complete if we
	// got here
	MissionInfo* pInfo = getPreviousMission();
	if ( !pInfo )
		return 0;
	
	return !pInfo->playSalvage;

}
bool	LogisticsMissionInfo::skipPurchasing()
{
	if ( MPlayer )
		return 0;

	MissionGroup* pGroup = &groups[currentStage]; 
	return !pGroup->infos[currentMission]->playPurchasing;
}

LogisticsMissionInfo::MissionInfo* LogisticsMissionInfo::getPreviousMission()
{
	if ( currentMission > 0 )
	{
		MissionGroup* pGroup = &groups[currentStage]; 
		return pGroup->infos[currentMission-1];
	}
	else if ( currentStage > 0 )
	{
		MissionGroup* pGroup = &groups[currentStage]-1; 
		int Count  = pGroup->infos.Count();
		if ( Count )
			return pGroup->infos[Count-1];
		else
			return nullptr;
	}
	else if ( currentStage == 0 )	//There may be only one stage.  I.e. Tutorials!
	{
		MissionGroup* pGroup = &groups[currentStage]; 
		int Count  = pGroup->infos.Count();
		if ( Count )
			return pGroup->infos[Count-1];
		else
			return nullptr;
	}

	return nullptr;
}

bool LogisticsMissionInfo::isSingleMission() const
{
	if ( groupCount == 1 )
	{
		if ( groups[0].infos.Count() == 1 )
			return 1;
	}

	return 0;
}

bool LogisticsMissionInfo::	showChooseMission()
{
	if ( MPlayer )
		return 0;

	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->infos[currentMission]->playMissionSelection;

}

bool	LogisticsMissionInfo::canHaveSalavageCraft()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->infos[currentMission]->enableSalavageCraft;

}

bool LogisticsMissionInfo::canHaveRepairTruck()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->infos[currentMission]->enableRepairTruck;
}
bool	LogisticsMissionInfo::canHaveScoutCopter()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->infos[currentMission]->enableScoutCopter;
}
bool	LogisticsMissionInfo::canHaveArtilleryPiece()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->infos[currentMission]->enableArtilleryPiece;
}
bool	LogisticsMissionInfo::canHaveAirStrike()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->infos[currentMission]->enableAirStrike;
}
bool	LogisticsMissionInfo::canHaveSensorStrike()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->infos[currentMission]->enableSensorStrike;
}
bool	LogisticsMissionInfo::canHaveMineLayer()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->infos[currentMission]->enableMineLayer;
}

bool LogisticsMissionInfo::getVideoShown()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->videoShown;

}

void LogisticsMissionInfo::setVideoShown()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	pGroup->videoShown = true;

}
