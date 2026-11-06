#ifndef LOGISTICSMISSIONINFO_H
#define LOGISTICSMISSIONINFO_H
/*************************************************************************************************\
LogisticsMissionInfo.h			: Interface for the LogisticsMissionInfo component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef ELIST_H
#endif

#include <string>
#include "mc2_czstring.h"
#include <optional>
#include <functional>
#include <vector>
#include <memory>

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

		// Null lookup names mean empty; selection rejects null/empty names.
		bool getMissionAvailable( mc2::czstring missionName );

		bool isMissionComplete( mc2::czstring missionName );
		bool isSingleMission() const;

		long setNextMission( mc2::czstring missionName );
		// Like setNextMission but searches ALL groups/stages (setNextMission only
		// searches the current stage). Used by the headless boot driver
		// (MC2_BOOT_TO_MISSION) to jump to any mission for logistics/icon capture
		// without campaign progression. Sets currentStage + currentMission.
		long setMissionAnyStage( mc2::czstring missionName );
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
		std::string			getMissionFriendlyName( mc2::czstring missionName ) const;
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
		void				setPurchaseFile( mc2::czstring fileName );

//		long				getMaxTeams() const { return maxTeams; }
//		long				getMaxPlayers() const { return maxPlayers; }

		int					getAdditionalPurachaseFiles( const char** list, long& maxCount );
		// Every mission's purchase file across ALL campaign stages/groups
		// (campaign-wide, deduped). Used by MC2_PURCHASE_ALL to unlock the union
		// of all mechs the campaign ever offers, not just the current mission's.
		long				getAllPurchaseFiles( const char** list, long& maxCount );
		// A null filename is ignored.
		void				addBonusPurchaseFile( mc2::czstring fileName ); // extra bonus

		bool				skipLogistics();
		void				setSkipLogistics(bool skip);
		bool				showChooseMission();

		bool				skipPilotReview();
		bool				skipSalvageScreen();
		bool				skipPurchasing();

		void				setSingleMission( mc2::czstring pName );

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

		// Both lists are filled while a campaign loads (and additionalPurchaseFiles when a purchase file is added) and never erased from; the pointees are
		// owned by the destructor path (clear()). Order is load order.
		using MISSION_LIST = std::vector<std::unique_ptr<MissionInfo>>;
		using FILE_LIST = std::vector<std::unique_ptr<char[]>>;


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

			// The record at `index`, or nullptr when the index is not one of this group's missions: the way to read an index a campaign save supplies.
			MissionInfo* at( long index ) const
			{
				return ( index >= 0 && index < std::ssize( infos ) ) ? infos[static_cast<size_t>( index )].get() : nullptr;
			}

			// The record at an index the campaign state itself holds (currentMission). currentMission is -1 before a mission is chosen, so the index is
			// not always a position of this group: an out-of-range index logs once and yields an all-default record, never a read outside `infos`.
			MissionInfo* mission( long index ) const;

			// What mission() hands back for an index outside the group; writes to it are discarded with the group. It is ONE record per group that every out-of-range
			// mission() call resets, so a pointer to it is valid only until the next mission() call on this group; do not keep two of them.
			mutable MissionInfo outOfRange;
			mutable bool outOfRangeLogged = false;
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

		std::unique_ptr<MissionGroup[]>			groups;
		long					groupCount;

		long					CBills;

		void					clear();
		bool					bMultiplayer;

		FILE_LIST				additionalPurchaseFiles;

		std::optional<std::reference_wrapper<MissionInfo>> getPreviousMission();
		void readMissionInfo( FitIniFile& file, LogisticsMissionInfo::MissionInfo* pInfo );




		// HELPER FUNCTIONS

};


//*************************************************************************************************
#endif  // end of file ( LogisticsMissionInfo.h )
