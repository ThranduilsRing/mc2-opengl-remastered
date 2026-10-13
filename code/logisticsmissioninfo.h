#ifndef LOGISTICSMISSIONINFO_H
#define LOGISTICSMISSIONINFO_H
/*************************************************************************************************\
LogisticsMissionInfo.h			: Interface for the LogisticsMissionInfo component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef ELIST_H
#include"elist.h"
#endif

#include <string>

inline constexpr int MAX_PLAYER_NAME_LENGTH = 64;

//*************************************************************************************************
class FitIniFile;
/**************************************************************************************************
CLASS DESCRIPTION
LogisticsMissionInfo:
**************************************************************************************************/
class LogisticsMissionInfo
{
	public:

		LogisticsMissionInfo();
		~LogisticsMissionInfo();
		
		
		long init( FitIniFile& file ); // init campaign
		void save( FitIniFile& file );
		long load( FitIniFile& file );// init previously saved stuff


		long getAvailableMissions( const char** missions, int& numberOfEm );
		long getCurrentMissions( const char** missions, int& numberOfEm );

		bool getMissionAvailable( const char* missionName );

		bool isMissionComplete( const char* missionName );
		bool isSingleMission() const;

		long setNextMission( const char* missionName );
		// Like setNextMission but searches ALL groups/stages (setNextMission only
		// searches the current stage). Used by the headless boot driver
		// (MC2_BOOT_TO_MISSION) to jump to any mission for logistics/icon capture
		// without campaign progression. Sets currentStage + currentMission.
		long setMissionAnyStage( const char* missionName );
		void setMissionComplete( );

		long getCurrentLogisticsTuneId();

		long getCurrentMissionId();
		
		const std::string& getCurrentPurchaseFile() const;
		const std::string& getCurrentMission() const { return currentMissionName; }
		const std::string& getLastMission() const { return lastMissionName; }

		// The text getters return a copy and an EMPTY string for "there is none" (no group, no key, nothing assigned): every caller only asks whether
		// there is text, and a copy can never dangle after clear().
		long				getCurrentDropWeight() const;
		std::string			getCurrentVideo() const;
		std::string			getCurrentOperationFile() const;
		std::string			getCurrentMissionDescription() const;
		std::string			getCurrentMissionFriendlyName() const;
		std::string			getMissionFriendlyName( const char* missionName ) const;
		std::string			getCurrentABLScriptName() const;

		int					getCurrentRP() const;
		int					getCBills() const { return CBills; }
		void				incrementCBills(int amount) { CBills += amount; }
		void				decrementCBills(int amount) { CBills -= amount; }

		const std::string& 	getCampaignName( ) const { return campaignName; }
		const std::string&	getCampaignDisplayName() const { return campaignDisplayName; }

		bool				campaignOver() const;

		// The group's pre-video, once: the first call after the group starts returns its name and every later call returns empty.
		std::string			getCurrentBigVideo() const;
		std::string			getFinalVideo() const;

		void				setMultiplayer();
		void				setPurchaseFile( const char* fileName );

//		long				getMaxTeams() const { return maxTeams; }
//		long				getMaxPlayers() const { return maxPlayers; }

		int					getAdditionalPurachaseFiles( const char** list, long& maxCount );
		// Every mission's purchase file across ALL campaign stages/groups
		// (campaign-wide, deduped). Used by MC2_PURCHASE_ALL to unlock the union
		// of all mechs the campaign ever offers, not just the current mission's.
		long				getAllPurchaseFiles( const char** list, long& maxCount );
		void				addBonusPurchaseFile( const char* fileName ); // extra bonus

		bool				skipLogistics();
		void				setSkipLogistics(bool skip);
		bool				showChooseMission();

		bool				skipPilotReview();
		bool				skipSalvageScreen();
		bool				skipPurchasing();

		void				setSingleMission( const char* pName );

		bool				canHaveSalavageCraft();
		bool				canHaveRepairTruck();
		bool				canHaveScoutCopter();
		bool				canHaveArtilleryPiece();
		bool				canHaveAirStrike();
		bool				canHaveSensorStrike();
		bool				canHaveMineLayer();
		bool				getVideoShown();
		void				setVideoShown();

		long				getCurrentMissionNumber()
		{
			return currentMission;
		}

		void				setCurrentMissionNumber(long cMission)
		{
			currentMission = cMission;
		}


	
	private:

	
		class MissionInfo
		{
		public:
			~MissionInfo();
			std::string missionDescriptiveName;
			std::string fileName;
			std::string description;
			std::string purchaseFileName;
			std::string videoName;
			// A flag the campaign file does not set is false; the readers apply the defaults of the keys they find missing.
			bool mandatory = false;
			bool completePrevious = false;
			bool completed = false;
			long resourcePoints = 0;
			long additionalCBills = 0;
			long dropWeight = 0;
			bool playLogistics = false;
			bool playSalvage = false;
			bool playPilotPromotion = false;
			bool playPurchasing = false;
			bool playMissionSelection = false;
			bool hidden = false;
			bool enableSalavageCraft = false;
			bool enableRepairTruck = false;
			bool enableScoutCopter = false;
			bool enableArtilleryPiece = false;
			bool enableAirStrike = false;
			bool enableSensorStrike = false;
			bool enableMineLayer = false;
		};	

		using MISSION_LIST = EList<MissionInfo*, MissionInfo*>;
		using FILE_LIST = EList< char*, char* >;


		class MissionGroup
		{
		public:
			long numberToBeCompleted = -1;
			std::string videoFileName;
			std::string operationFileName;
			std::string bigVideoName;
			std::string ablBrainName;
			long logisticsTuneId = 0;
			MISSION_LIST infos;
			bool	bigVideoShown = false;
			bool	videoShown = false;
		};


		// DATA

		std::string				campaignName;
		std::string				campaignDisplayName;
		std::string				playerName;
		std::string				currentMissionName;
		std::string				finalVideoName;
		long					currentStage;
		long					currentMission;

		long					lastStage;
		long					lastMission;
		std::string				lastMissionName;

		MissionGroup*			groups;
		long					groupCount;

		long					CBills;

		void					clear();
		bool					bMultiplayer;

		FILE_LIST				additionalPurchaseFiles;

		LogisticsMissionInfo::MissionInfo* getPreviousMission();
		void readMissionInfo( FitIniFile& file, LogisticsMissionInfo::MissionInfo* pInfo );




		// HELPER FUNCTIONS

};


//*************************************************************************************************
#endif  // end of file ( LogisticsMissionInfo.h )
