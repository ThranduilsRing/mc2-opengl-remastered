// LogisticsMissionInfo: the per-mission accessors (purchase / operation files, videos, tune, flags) and the skip / multiplayer switches.
// Declared in the class headers; shared names in logisticsmissioninfo_internal.h.
#include "logisticsmissioninfo_internal.h"
#include <utility>
#include "mc2str.h"
#include "objective.h"
#include "team.h"


LogisticsMissionInfo::MissionInfo::~MissionInfo()
{
}

const std::string& LogisticsMissionInfo::getCurrentPurchaseFile() const
{
	MissionGroup* pGroup = &groups[currentStage];

	return pGroup->mission(currentMission)->purchaseFileName;
}

long LogisticsMissionInfo::getAllPurchaseFiles( const char** list, long& maxCount )
{
	long count = 0;
	long maxOut = maxCount;
	for ( long g = 0; g < groupCount; g++ )
	{
		auto iter = groups[g].infos.begin();
		while ( iter != groups[g].infos.end() )
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
		return pGroup->mission(currentMission)->dropWeight;
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

	MissionInfo* pInfo = pGroup->mission(currentMission);
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

	return groups[currentStage].mission(currentMission)->description;
}

bool LogisticsMissionInfo::getMissionAvailable( mc2::czstring missionName )
{
	MissionGroup* pGroup = &groups[currentStage];

	for ( int i = 0; i < std::ssize(pGroup->infos); i++ )
	{
		if ( mc2::str::iequals( pGroup->mission(i)->fileName, mc2::str::from_c( missionName ) ) )
		{
			return !pGroup->mission(i)->completed;
		}
	}
	return false;
}

std::string LogisticsMissionInfo::getCurrentMissionFriendlyName() const
{
	if ( currentStage >= groupCount )
		return std::string();

	return groups[currentStage].mission(currentMission)->missionDescriptiveName;
}

std::string LogisticsMissionInfo::getCurrentABLScriptName() const
{
	if ( currentStage >= groupCount )
		return std::string();

	return groups[currentStage].ablBrainName;
}

std::string LogisticsMissionInfo::getMissionFriendlyName( mc2::czstring missionName ) const
{
	if ( currentStage >= groupCount )
		return std::string();
	MissionGroup* pGroup = &groups[currentStage];

	const std::string wanted = mc2::str::from_c( missionName );
	for ( int i = 0; i < std::ssize(pGroup->infos); i++ )
	{
		if ( mc2::str::iequals( pGroup->mission(i)->fileName, wanted ) )
		{
			return pGroup->mission(i)->missionDescriptiveName;
		}
	}
	return std::string();
}

int		LogisticsMissionInfo::getCurrentRP() const
{
	if ( currentMission == -1 )
		return 0;

	
	MissionGroup* pGroup = &groups[currentStage];
	return pGroup->mission(currentMission)->resourcePoints;

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
	groups = std::make_unique_for_overwrite<MissionGroup[]>(1);

	currentStage = 0;

	groups[0].infos.push_back( std::make_unique<MissionInfo>() );

	currentMission = 0;
	groupCount = 1;


			

}
void		LogisticsMissionInfo::setPurchaseFile( mc2::czstring fileName )
{
	gosASSERT( bMultiplayer) ;

	MissionInfo* pInfo = groups[0].mission(0);

	pInfo->purchaseFileName = std::string( missionPath ) + mc2::str::from_c( fileName ) + ".fit";
}

int			LogisticsMissionInfo::getAdditionalPurachaseFiles( const char** list, long& maxCount )
{
	if ( maxCount >=  std::ssize(additionalPurchaseFiles) )
	{
		long i = 0;
		
		for (auto & additionalPurchaseFile : additionalPurchaseFiles)
		{
			list[i++] = additionalPurchaseFile.get();
		}
	}

	maxCount = static_cast<int>(std::ssize(additionalPurchaseFiles));

	return static_cast<long>(std::ssize(additionalPurchaseFiles));
}
void				LogisticsMissionInfo::addBonusPurchaseFile( mc2::czstring fileName )
{
	if ( !fileName )
		return;

	auto newFile = std::make_unique_for_overwrite<char[]>(strlen( fileName )+1);
	char* pNewFile = newFile.get();
	strcpy( pNewFile, fileName ); 

	additionalPurchaseFiles.push_back( std::move(newFile) );
	if (auto* observation = objectiveObservation(); observation && observation->dispatch) {
		observation->technologies.push_back({pNewFile, observation->dispatch});
		observation->SinkResult("technology", "queued", "accepted", objectiveObservationQuote(pNewFile));
	}
}

bool	LogisticsMissionInfo::skipLogistics()
{
	if ( MPlayer )
		return 0;

	if (currentStage >= groupCount)
		return false;

	MissionGroup* pGroup = &groups[currentStage];
	return !pGroup->mission(currentMission)->playLogistics;

}

void LogisticsMissionInfo::setSkipLogistics(bool skip)
{
	// Smoke-mode helper. Bypasses the .fit-file-derived PlayLogistics flag
	// on the currently-selected mission info. Safe to call only after
	// setSingleMission() / setNextMission() has populated groups[currentStage].
	if (currentStage < groupCount && !groups[currentStage].infos.empty()) {
		// at(), not mission(): with no mission chosen (currentMission -1) there is no record to change, and the default record mission() returns would swallow the write.
		if (MissionInfo* info = groups[currentStage].at(currentMission))
			info->playLogistics = !skip;
	}
}
bool	LogisticsMissionInfo::skipPilotReview()
{
	// need to subtract a mission, since we already set this one complete if we
	// got here
	const auto pInfo = getPreviousMission();
	if ( !pInfo )
		return 0;
	return !pInfo->get().playPilotPromotion;
	

}
bool	LogisticsMissionInfo::skipSalvageScreen()
{
	// need to subtract a mission, since we already set this one complete if we
	// got here
	const auto pInfo = getPreviousMission();
	if ( !pInfo )
		return 0;
	
	return !pInfo->get().playSalvage;

}
bool	LogisticsMissionInfo::skipPurchasing()
{
	if ( MPlayer )
		return 0;

	MissionGroup* pGroup = &groups[currentStage]; 
	return !pGroup->mission(currentMission)->playPurchasing;
}

std::optional<std::reference_wrapper<LogisticsMissionInfo::MissionInfo>> LogisticsMissionInfo::getPreviousMission()
{
	if ( currentMission > 0 )
	{
		MissionGroup* pGroup = &groups[currentStage]; 
		return *pGroup->mission(currentMission-1);
	}
	else if ( currentStage > 0 )
	{
		MissionGroup* pGroup = &groups[currentStage]-1; 
		int Count  = static_cast<int>(std::ssize(pGroup->infos));
		if ( Count )
			return *pGroup->mission(Count-1);
		else
			return std::nullopt;
	}
	else if ( currentStage == 0 )	//There may be only one stage.  I.e. Tutorials!
	{
		MissionGroup* pGroup = &groups[currentStage]; 
		int Count  = static_cast<int>(std::ssize(pGroup->infos));
		if ( Count )
			return *pGroup->mission(Count-1);
		else
			return std::nullopt;
	}

	return std::nullopt;
}

bool LogisticsMissionInfo::isSingleMission() const
{
	if ( groupCount == 1 )
	{
		if ( std::ssize(groups[0].infos) == 1 )
			return 1;
	}

	return 0;
}

bool LogisticsMissionInfo::	showChooseMission()
{
	if ( MPlayer )
		return 0;

	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->mission(currentMission)->playMissionSelection;

}

bool	LogisticsMissionInfo::canHaveSalavageCraft()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->mission(currentMission)->enableSalavageCraft;

}

bool LogisticsMissionInfo::canHaveRepairTruck()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->mission(currentMission)->enableRepairTruck;
}
bool	LogisticsMissionInfo::canHaveScoutCopter()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->mission(currentMission)->enableScoutCopter;
}
bool	LogisticsMissionInfo::canHaveArtilleryPiece()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->mission(currentMission)->enableArtilleryPiece;
}
bool	LogisticsMissionInfo::canHaveAirStrike()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->mission(currentMission)->enableAirStrike;
}
bool	LogisticsMissionInfo::canHaveSensorStrike()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->mission(currentMission)->enableSensorStrike;
}
bool	LogisticsMissionInfo::canHaveMineLayer()
{
	MissionGroup* pGroup = &groups[currentStage]; 
	return pGroup->mission(currentMission)->enableMineLayer;
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
