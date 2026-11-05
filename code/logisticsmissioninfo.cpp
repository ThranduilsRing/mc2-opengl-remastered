#define LOGISTICSMISSIONINFO_CPP
/*************************************************************************************************\
LogisticsMissionInfo.cpp			: Implementation of the LogisticsMissionInfo component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "logisticsmissioninfo_internal.h"
#include "mc2fmt.h"
#include "mc2str.h"
#include "objective_text.h"
#include "../GameOS/gameos/utils/mc2log.h"

#include <memory>
#include <utility>
#include <optional>




LogisticsMissionInfo::LogisticsMissionInfo()
{
	currentMission = -1;
	currentStage = 0;
	groups = nullptr;
	currentMissionName = mc2::str::from_c( missionName );
	groupCount = 0;
	CBills = 0;
	if ( currentMissionName.size() < 2 )
		currentMissionName = "m0101";

	bMultiplayer = 0;
//	maxTeams = maxPlayers = 2;
}

LogisticsMissionInfo::~LogisticsMissionInfo()
{
	clear();
}

LogisticsMissionInfo::MissionInfo* LogisticsMissionInfo::MissionGroup::mission( long index ) const
{
	if ( MissionInfo* found = at( index ) )
		return found;
	if ( !outOfRangeLogged )
	{
		outOfRangeLogged = true;
		MC2_LOG(Io, Warn, "campaign: mission index {} is not one of the {} missions of the group, an empty record is used", index, std::ssize( infos ));
	}
	outOfRange = MissionInfo();
	return &outOfRange;
}

void LogisticsMissionInfo::clear()
{
	if ( groups )
	{
		for ( int i = 0; i < groupCount; i++ )
		{
			for (auto & info : groups[i].infos)
			{
				delete info;
			}
		}

		groups.reset();
	}

	for (auto & additionalPurchaseFile : additionalPurchaseFiles)
	{
		additionalPurchaseFilereset();
	}

	additionalPurchaseFiles.clear();



	currentMission = -1;
	currentStage = 0;
	groups = nullptr;
	currentMissionName = mc2::str::from_c( missionName );
	groupCount = 0;
	CBills = 0;

}


long LogisticsMissionInfo::init( FitIniFile& file )
{

	clear();

	bMultiplayer = 0;
	campaignName = mc2::str::from_c( file.getFilename() );
	// read the number of mission groups
	file.seekBlock("Campaign" );

	char resultName[256];
	resultName[0] = 0;
	long lName = 0;

	long result = file.readIdLong( "NameID", lName );
	if ( result == NO_ERR )
	{
		cLoadString( lName, resultName, 255 );
		campaignDisplayName = resultName;
	}
	else
	{
		const std::optional<std::string> displayName = objective_text::readPlain( file, "CampaignName" );
		Assert( displayName.has_value(), 0, "couldn't find the missionName" );
		campaignDisplayName = displayName.value_or( std::string() );
	}


	result = file.readIdLong( "GroupCount", groupCount );
	if ( result != NO_ERR )
	{
		Assert( 0, 0, "No group count in campaign file" );
		return -1;
	}

	result = file.readIdLong( "CBills", CBills );

	finalVideoName = objective_text::readPlain( file, "FinalVideo" ).value_or( std::string() );


	// create storage for 'em
	groups = std::make_unique_for_overwrite<MissionGroup[]>(groupCount);

	currentStage = 0;

	char blockName[32];

	for ( int i = 0; i < groupCount; i++ )
	{
		groups[i].numberToBeCompleted = -1; // initialize

		mc2::fmt::to_buf(blockName, "Group{:d}", mc2::fmt::as_i32(i));
		if ( NO_ERR != file.seekBlock( blockName ) )
		{
			Assert( 0, i, "couldn't find this group in the campaign file" );
			continue;
		}

		file.readIdLong( "NumberToComplete", groups[i].numberToBeCompleted );
		file.readIdLong( "Tune", groups[i].logisticsTuneId);

		//Perfectly valid to have no ABL brain for this group!
		groups[i].ablBrainName = objective_text::readPlain( file, "ABLScript" ).value_or( "NONE" );

		const std::optional<std::string> video = objective_text::readPlain( file, "Video" );
		if ( !video )
		{
			char errorStr[256];
			mc2::fmt::to_buf(errorStr, "couldn't find the video for operation {:d}", mc2::fmt::as_i32(i));
			Assert( 0, 0, errorStr );

		}
		groups[i].videoFileName = video.value_or( std::string() );
		groups[i].videoShown = 0;

		// A group without a PreVideo key has no pre-video: the name stays empty.
		if ( const std::optional<std::string> preVideo = objective_text::readPlain( file, "PreVideo" ) )
		{
			groups[i].bigVideoName = *preVideo;
			groups[i].bigVideoShown = 0;
		}

		const std::optional<std::string> operationFile = objective_text::readPlain( file, "OperationFile" );
		groups[i].operationFileName = ( operationFile && !operationFile->empty() ) ? *operationFile : std::string( "mcl_cm_opplayer.fit" );

		long missionCount = 0;
		// read the number of missions in the group
		file.readIdLong( "MissionCount", missionCount );

		for ( int j = 0; j < missionCount; j++ )
		{
			std::unique_ptr<MissionInfo> info = std::make_unique<MissionInfo>();

			mc2::fmt::to_buf(blockName, "Group{:d}Mission{:d}", mc2::fmt::as_i32(i), mc2::fmt::as_i32(j));
			if ( NO_ERR == file.seekBlock( blockName ) )
			{
				const std::optional<std::string> fileName = objective_text::readPlain( file, "FileName", 1023 );
				if ( !fileName )
				{
					Assert( 0, j, "couldn't find a mission file name in the campaign file" );
					continue;
				}

				MissionInfo* pInfo = info.release();
				groups[i].infos.push_back( pInfo );

				const std::string path = std::string( missionPath ) + *fileName + ".fit";

				pInfo->fileName = *fileName;

				pInfo->completed = 0;


				FitIniFile missionFile;

				if ( NO_ERR != missionFile.open( path.c_str() ) )
				{
					char errorStr[256];
					mc2::fmt::to_buf(errorStr, "couldn't open file {}", *fileName);
					Assert( 0, 0, errorStr );
					continue;
				}

				readMissionInfo( missionFile, pInfo );

				pInfo->videoName = objective_text::readPlain( file, "VideoOverride", 254 ).value_or( std::string() );

				file.readIdBoolean( "Mandatory", pInfo->mandatory );

				file.readIdBoolean( "CompletePrevious", pInfo->completePrevious );

				std::string purchaseFile = objective_text::readPlain( file, "PurchaseFile", 1023 ).value_or( std::string() );
				if ( purchaseFile.empty() )
				{
					purchaseFile = "purchase_All";
				}

				file.readIdBoolean( "Hidden", pInfo->hidden );

				if ( NO_ERR != file.readIdBoolean( "PlayLogistics", pInfo->playLogistics ) )
					pInfo->playLogistics = true;


				if ( NO_ERR != file.readIdBoolean( "PlaySalvage", pInfo->playSalvage ) )
					pInfo->playSalvage = true;


				if ( NO_ERR != file.readIdBoolean( "PlayPilotPromotion", pInfo->playPilotPromotion ) )
					pInfo->playPilotPromotion = true;


				if ( NO_ERR != file.readIdBoolean( "PlayPurchasing", pInfo->playPurchasing ) )
					pInfo->playPurchasing = true;

				if ( NO_ERR != file.readIdBoolean( "PlaySelection", pInfo->playMissionSelection ) )
				{
					pInfo->playMissionSelection = false;
				}

				pInfo->purchaseFileName = std::string( missionPath ) + purchaseFile + ".fit";
			}
		}

	}

	return 0;
}

void LogisticsMissionInfo::readMissionInfo( FitIniFile& file, LogisticsMissionInfo::MissionInfo* pInfo )
{

	long result = file.seekBlock( "MissionSettings" );
	Assert( result == NO_ERR, 0, "Coudln't find the mission settings block in the mission file" );

	bool bRes = 0;

	result = file.readIdBoolean( "MissionNameUseResourceString", bRes );
	//Assert( result == NO_ERR, 0, "couldn't find the MissionNameUseResourceString" );
	if ( bRes )
	{
		char resourceName[256];
		unsigned long lRes = 0;
		result = file.readIdULong( "MissionNameResourceStringID", lRes );
		Assert( result == NO_ERR, 0, "couldn't find the MissionNameResourceStringID" );
		cLoadString( lRes, resourceName, 255 );
		pInfo->missionDescriptiveName = resourceName;
	}
	else
	{
		const std::optional<std::string> friendlyName = objective_text::readPlain( file, "MissionName" );
		Assert( friendlyName.has_value(), 0, "couldn't find the missionName" );
		pInfo->missionDescriptiveName = friendlyName.value_or( std::string() );
	}

	const std::optional<std::string> blurb = objective_text::readPlain( file, "Blurb2", 4095 );
	Assert( blurb.has_value(), 0, "couldn't find the mission blurb" );
	pInfo->description = blurb.value_or( std::string() );

	bool tmpBool = false;
	result = file.readIdBoolean("Blurb2UseResourceString", tmpBool);
	if (NO_ERR == result && tmpBool )
	{
		unsigned long tmpInt = 0;
		result = file.readIdULong("Blurb2ResourceStringID", tmpInt);
		if (NO_ERR == result)
		{
			char resourceBlurb[2048];
			cLoadString( tmpInt, resourceBlurb, 2047 );
			pInfo->description = resourceBlurb;
		}
	}

	result = file.readIdLong( "ResourcePoints", pInfo->resourcePoints );
	result = file.readIdLong("AdditionalCBills", pInfo->additionalCBills );
	float fTmp;
	result = file.readIdFloat( "DropWeightLimit", fTmp );
	if ( result != NO_ERR )
		pInfo->dropWeight = 300;
	else
		pInfo->dropWeight = fTmp;


	if ( NO_ERR != file.readIdBoolean( "AirStrikesEnabledDefault", pInfo->enableAirStrike) )
		pInfo->enableAirStrike = 1;

	if ( NO_ERR != file.readIdBoolean( "MineLayersEnabledDefault", pInfo->enableMineLayer) )
		pInfo->enableMineLayer = 1;

	if ( NO_ERR != file.readIdBoolean("ScoutCoptersEnabledDefault", pInfo->enableScoutCopter) )
		pInfo->enableScoutCopter = 1;

	if ( NO_ERR != file.readIdBoolean("SalvageCraftEnabledDefault", pInfo->enableSalavageCraft) )
		pInfo->enableSalavageCraft = 1;


	if ( NO_ERR != file.readIdBoolean( "SensorProbesEnabledDefault", pInfo->enableSensorStrike) )
		pInfo->enableSensorStrike = true;

	if ( NO_ERR != file.readIdBoolean( "RepairVehicleEnabledDefault", pInfo->enableRepairTruck ) )
		pInfo->enableRepairTruck = true;

	if ( NO_ERR != file.readIdBoolean("ArtilleryPieceEnabledDefault", pInfo->enableArtilleryPiece) )
		pInfo->enableArtilleryPiece = 0;
}

long LogisticsMissionInfo::load( FitIniFile& file )
{
	// read the campaign file name
	long result = file.seekBlock( "General" );
	Assert( result == NO_ERR, 0, "couldn't find general block in campaign file" );

	const std::optional<std::string> campaignFileName = objective_text::readPlain( file, "CampaignFile", 63 );
	if ( !campaignFileName )
	{
		MC2_LOG(Io, Warn, "campaign save: the General block has no usable CampaignFile, the save is not loaded");
		return -1;
	}

	FitIniFile campaignFile;
	if ( NO_ERR != campaignFile.open( campaignFileName->c_str() ) )
	{
		char errorStr[256];
		mc2::fmt::to_buf(errorStr, "couldn't find file {}", *campaignFileName);
		return -1;
	}

	campaignName = *campaignFileName;

	// initialize this with campaign info
	if ( NO_ERR != init( campaignFile ) )
	{
		Assert( 0, 0, "couldn't initialize the campaign" );
		return -1;
	}
	currentMission = 0;

	// reset rp
	file.readIdLong( "CBills", CBills );

	// set the current mission group
	file.readIdLong( "Group", currentStage );

	if ( currentStage >= groupCount )
		currentStage = groupCount-1;

	// set completed missions
	long count;
	file.readIdLong( "CompletedMissions", count );

	char header[32];

	MissionGroup* pGroup = &groups[currentStage];

	file.readIdBoolean( "BigMoviePlayed", pGroup->bigVideoShown);

	long numberCompleted = 0;

	for ( int i = 0; i < count; i++ )
	{
		long cnt = 0;
		mc2::fmt::to_buf(header, "Mission{:d}", mc2::fmt::as_i32(i));
		file.readIdLong( header, cnt );

		MissionInfo* pInfo = pGroup->at( cnt );
		if ( !pInfo )
		{
			// A save from another campaign, or a hand-edited one: the index names no mission of this group.
			MC2_LOG(Io, Warn, "campaign save: completed mission index {} is not one of the {} missions of stage {}, skipped", cnt, std::ssize( pGroup->infos ), currentStage);
			continue;
		}

		pInfo->completed = 1;
		numberCompleted ++;

	}

	if ( numberCompleted >= pGroup->numberToBeCompleted  ) // unhide necessary missions
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
	}



	if ( const MissionInfo* pNext = pGroup->at( count ) )
		setNextMission( pNext->fileName.c_str() );


	playerName = objective_text::readPlain( file, "PlayerName", MAX_PLAYER_NAME_LENGTH - 1 ).value_or( std::string() );

	if ( NO_ERR == file.seekBlock( "AdditionalPurchaseFiles" ) )
	{
		int i = 0;
		while( true )
		{
			mc2::fmt::to_buf(header, "File{:d}", mc2::fmt::as_i32(i));
			char tmp[256];

			if ( NO_ERR == file.readIdString( header, tmp, 255 ) )
			{
				auto pfName = std::make_unique_for_overwrite<char[]>(strlen( tmp )+1);
				strcpy( pfName.get(), tmp );
				additionalPurchaseFiles.push_back( std::move(pfName) );
				i++;
			}
			else
				break;
		}


	}

	return 0;

}
void LogisticsMissionInfo::save( FitIniFile& file )
{
	// save the campaign file name
	file.writeIdString( "CampaignFile", campaignName.c_str() );
	file.writeIdString( "CampaignName", campaignDisplayName.c_str() );

	// save the player name
	file.writeIdString( "PlayerName", playerName.c_str() );

	// save the rp
	file.writeIdLong( "CBills", CBills );

	// save the current mission group
	file.writeIdLong( "Group", currentStage );
	int count = 0;
	char header[32];

	if ( currentStage < groupCount )
	{

		// save the missions completed thus far in the mission group
		MissionGroup* pGroup = &groups[currentStage];

		file.writeIdBoolean( "BigMoviePlayed", pGroup->bigVideoShown);

		// write out the name of the mission, for ease of filling dialog
		MissionInfo* pInfo = pGroup->mission(currentMission);
		file.writeIdString( "MissionName", pInfo->missionDescriptiveName.c_str() );
		file.writeIdString( "MissionFileName", pInfo->fileName.c_str() );

		int curMission = 0;
		for ( auto iter = pGroup->infos.begin(); iter != pGroup->infos.end(); ++iter, curMission++ )
		{
			if ( (*iter)->completed )
			{
				mc2::fmt::to_buf(header, "Mission{:d}", mc2::fmt::as_i32(count));
				file.writeIdLong( header, curMission );
				count++;
			}
		}
	}

	file.writeIdLong( "CompletedMissions", count );


	file.writeBlock( "AdditionalPurchaseFiles" );
	int i = 0;
	for (auto & additionalPurchaseFile : additionalPurchaseFiles)
	{
		mc2::fmt::to_buf(header, "File{:d}", mc2::fmt::as_i32(i));

		file.writeIdString( header, additionalPurchaseFileget() );
		i++;
	}




}
