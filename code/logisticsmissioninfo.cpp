#define LOGISTICSMISSIONINFO_CPP
/*************************************************************************************************\
LogisticsMissionInfo.cpp			: Implementation of the LogisticsMissionInfo component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "logisticsmissioninfo_internal.h"
#include "mc2fmt.h"



LogisticsMissionInfo::LogisticsMissionInfo()
{
	currentMission = -1;
	currentStage = 0;
	groups = nullptr;
	currentMissionName = missionName;
	groupCount = 0;
	CBills = 0;
	if ( currentMissionName.Length() < 2 )
		currentMissionName = "m0101";

	bMultiplayer = 0;
//	maxTeams = maxPlayers = 2;
}

LogisticsMissionInfo::~LogisticsMissionInfo()
{
	clear();
}

void LogisticsMissionInfo::clear()
{
	if ( groups )
	{
		for ( int i = 0; i < groupCount; i++ )
		{
			for ( MISSION_LIST::EIterator iter = groups[i].infos.Begin(); 
			!iter.IsDone(); iter++ )
			{
				delete (*iter);
			}
		}

		delete [] groups;
	}

	for ( FILE_LIST::EIterator iter = additionalPurchaseFiles.Begin();
	!iter.IsDone(); iter++ )
	{
		delete[] *iter;
	}

	additionalPurchaseFiles.Clear();



	currentMission = -1;
	currentStage = 0;
	groups = nullptr;
	currentMissionName = missionName;
	groupCount = 0;
	CBills = 0;

}


long LogisticsMissionInfo::init( FitIniFile& file )
{

	clear();

	bMultiplayer = 0;
	campaignName= file.getFilename();
	// read the number of mission groups
	file.seekBlock("Campaign" );

	char resultName[256];
	resultName[0] = 0;
	long lName = 0;

	long result = file.readIdLong( "NameID", lName );
	if ( result == NO_ERR )
	{
		cLoadString( lName, resultName, 255 );
	}
	else
	{
		result = file.readIdString( "CampaignName", resultName, 255 );
		Assert( result == NO_ERR, 0, "couldn't find the missionName" );
	}

	campaignDisplayName = resultName;


	result = file.readIdLong( "GroupCount", groupCount );
	if ( result != NO_ERR )
	{
		Assert( 0, 0, "No group count in campaign file" );
		return -1;
	}

	result = file.readIdLong( "CBills", CBills );

	char tmp[256];
	if ( NO_ERR == file.readIdString( "FinalVideo", tmp, 255 ) )
	{
		finalVideoName = tmp;
	}

	
	// create storage for 'em
	groups = new MissionGroup[groupCount];

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

		if ( NO_ERR != file.readIdString( "ABLScript", tmp, 255 ) )
		{
			//Perfectly valid to have no ABL brain for this group!
			groups[i].ablBrainName = "NONE";
		}
		else
		{
			groups[i].ablBrainName = tmp;
		}

		if ( NO_ERR != file.readIdString( "Video", tmp, 255 ) )
		{
			char errorStr[256];
			mc2::fmt::to_buf(errorStr, "couldn't find the video for operation {:d}", mc2::fmt::as_i32(i));
			Assert( 0, 0, errorStr );
			
		}
		groups[i].videoFileName = tmp;
		groups[i].videoShown = 0;

		if ( NO_ERR == file.readIdString( "PreVideo", tmp, 255 ) )
		{
			groups[i].bigVideoName = tmp;
			groups[i].bigVideoShown = 0;
		}

		if ( NO_ERR != file.readIdString( "OperationFile", tmp, 255  )
			|| !strlen( tmp ) )
		{
			strcpy( tmp, "mcl_cm_opplayer.fit" );			
		}
		groups[i].operationFileName = tmp;

		long missionCount;
		// read the number of missions in the group
		file.readIdLong( "MissionCount", missionCount );

		for ( int j = 0; j < missionCount; j++ )
		{
			MissionInfo* pInfo = new MissionInfo;

			mc2::fmt::to_buf(blockName, "Group{:d}Mission{:d}", mc2::fmt::as_i32(i), mc2::fmt::as_i32(j));
			if ( NO_ERR == file.seekBlock( blockName ) )
			{
				char fileName[1024];
				if ( NO_ERR != file.readIdString( "FileName", fileName, 1023 ) )
				{				
					Assert( 0, j, "couldn't find a mission file name in the campaign file" );
					continue;
				}

				groups[i].infos.Append( pInfo );

				EString path = missionPath;
				path += fileName;
				path += ".fit";
				
				pInfo->fileName = fileName;
				
				pInfo->completed = 0;
			
				
				FitIniFile missionFile;
				
				if ( NO_ERR != missionFile.open( (char*)(const char*)path ) )
				{
					char errorStr[256];
					mc2::fmt::to_buf(errorStr, "couldn't open file {}", mc2::fmt::safe(fileName));
					Assert( 0, 0, errorStr );
					continue;
				}

				readMissionInfo( missionFile, pInfo );

				char videoFileName[255];
				if ( NO_ERR == file.readIdString( "VideoOverride", videoFileName, 255 ) )
					pInfo->videoName = videoFileName; 

				file.readIdBoolean( "Mandatory", pInfo->mandatory );				

				file.readIdBoolean( "CompletePrevious", pInfo->completePrevious );
				
				long result = file.readIdString( "PurchaseFile", fileName, 1023 );
				if ((NO_ERR != result) || (0 == strcmp("", fileName)))
				{
					strcpy(fileName, "purchase_All");
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

				pInfo->purchaseFileName = missionPath;
				pInfo->purchaseFileName += fileName;
				pInfo->purchaseFileName += ".fit";
			}
		}

	}

	return 0;
}

void LogisticsMissionInfo::readMissionInfo( FitIniFile& file, LogisticsMissionInfo::MissionInfo* pInfo )
{

	long result = file.seekBlock( "MissionSettings" );
	Assert( result == NO_ERR, 0, "Coudln't find the mission settings block in the mission file" );

	char missionName[256];
	missionName[0] = 0;
	bool bRes = 0;

	result = file.readIdBoolean( "MissionNameUseResourceString", bRes );
	//Assert( result == NO_ERR, 0, "couldn't find the MissionNameUseResourceString" );
	if ( bRes )
	{
		unsigned long lRes;
		result = file.readIdULong( "MissionNameResourceStringID", lRes );
		Assert( result == NO_ERR, 0, "couldn't find the MissionNameResourceStringID" );
		cLoadString( lRes, missionName, 255 );
	}
	else
	{
		result = file.readIdString( "MissionName", missionName, 255 );
		Assert( result == NO_ERR, 0, "couldn't find the missionName" );
	}
	
	pInfo->missionDescriptiveName = missionName;

	char blurb[4096];
	
	result = file.readIdString( "Blurb2", blurb, 4095 );
	Assert( result == NO_ERR, 0, "couldn't find the mission blurb" );

	bool tmpBool = false;
	result = file.readIdBoolean("Blurb2UseResourceString", tmpBool);
	if (NO_ERR == result && tmpBool )
	{
		unsigned long tmpInt = 0;
		result = file.readIdULong("Blurb2ResourceStringID", tmpInt);
		if (NO_ERR == result)
		{
			cLoadString( tmpInt, blurb, 2047 );
		}
	}

	pInfo->description = blurb;
	
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
	char path[1024];
	char fileName[64];
	long result = file.seekBlock( "General" );
	Assert( result == NO_ERR, 0, "couldn't find general block in campaign file" );

	result = file.readIdString( "CampaignFile", fileName, 63);


	FitIniFile campaignFile;
	if ( NO_ERR != campaignFile.open( fileName ) )
	{
		char errorStr[256];
		mc2::fmt::to_buf(errorStr, "couldn't find file {}", mc2::fmt::safe(path));
		return -1;
	}

	campaignName = fileName;
	
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

		MissionInfo* pInfo = pGroup->infos[(unsigned long)cnt];

		pInfo->completed = 1;
		numberCompleted ++;

	}

	if ( numberCompleted >= pGroup->numberToBeCompleted  ) // unhide necessary missions
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
	}
		
	

	if ( count < pGroup->infos.Count() )
		setNextMission( pGroup->infos[count]->fileName );


	char tmpPlayerName[64];
	file.readIdString( "PlayerName", tmpPlayerName, 63 );
	playerName = tmpPlayerName;

	if ( NO_ERR == file.seekBlock( "AdditionalPurchaseFiles" ) )
	{
		int i = 0;
		while( true )
		{
			mc2::fmt::to_buf(header, "File{:d}", mc2::fmt::as_i32(i));
			char tmp[256];

			if ( NO_ERR == file.readIdString( header, tmp, 255 ) )
			{
				char* pfName = new char[strlen( tmp )+1 ];
				strcpy( pfName, tmp );
				additionalPurchaseFiles.Append( pfName );
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
	file.writeIdString( "CampaignFile", campaignName );
	file.writeIdString( "CampaignName", campaignDisplayName );

	// save the player name
	if ( playerName.Length() )
		file.writeIdString( "PlayerName", playerName );
	else
		file.writeIdString( "PlayerName", "" );

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
		MissionInfo* pInfo = pGroup->infos[currentMission];
		file.writeIdString( "MissionName", pInfo->missionDescriptiveName );
		file.writeIdString( "MissionFileName", pInfo->fileName );

		int curMission = 0;
		for ( MISSION_LIST::EIterator iter = pGroup->infos.Begin(); !iter.IsDone(); iter++, curMission++ )
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
	for ( FILE_LIST::EIterator fIter = additionalPurchaseFiles.Begin(); !fIter.IsDone(); fIter++ )
	{
		mc2::fmt::to_buf(header, "File{:d}", mc2::fmt::as_i32(i));

		file.writeIdString( header, (*fIter) );
		i++;
	}




}
