// Mission Part initialization shared by the engine and offline fixtures.
#include <algorithm>

#include "mission_init_frame.h"
#include "mc2fmt.h"

using enum MissionLoadType;

void Mission::diagnoseAuthoredRoute(std::uint32_t original, std::uint32_t selected, std::uint32_t pilot,
    std::string_view status, std::string_view reason) {
    auto diagnostic = std::format("{{\"status\":\"{}\",\"original\":{},\"selected\":{},\"pilot\":{},\"reason\":\"{}\"}}",
        status, original, selected, pilot, reason);
    authoredDiagnostics.push_back(diagnostic);
    // Loader/admission failures have no receipt; application rejection is reported by the same owner.
    mc2::fmt::print_err("[AUTHORED_ORDER v1] level={} {}\n", status == "Suppressed" ? "info" : "error", diagnostic);
}

constexpr auto MAX_SQUADS = 256;
constexpr auto MAX_ALTERNATIVES = 15;

// Read the parts block header.
void Mission::initPartsHeader(MissionInitFrame& f) {
	auto& result = f.result;
#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeWarriorLoad=x-x1;
#endif

	loadProgress = 43.0f;

	result = missionFile->seekBlock("Parts");
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
	// the SP object-instantiation pass; a missing [Parts] block leaves numParts
	// uninitialized and sizes the parts[] Malloc below off garbage.
	MC2_VERIFY(result == NO_ERR, "Mission::init: mission .fit missing [Parts] block (load pass)");

	result = missionFile->readIdULong("NumParts",numParts);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
	// numParts sizes std::malloc(sizeof(Part)*(numParts+1)) below and
	// bounds every part loop; a failed read leaves it uninitialized.
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Parts] NumParts read failed (load pass)");

	//--------------------------------------------------------------------------------
	// IMPORTANT NOTE: mission parts should always start with Part 1.
	// Part 0 is reserved as a "NULL" id for routines that reference the mission
	// parts. AI routines, Brain keywords, etc. use PART ID 0 as an "object not found"
	// error code. DO NOT USE PART 0!!!!!!! Start with Part 1...

#define	USE_ALTERNATES

	loadProgress = 43.5f;
}

// Create every mover and part named by the mission file.
void Mission::initPartsLoad(MissionLoadType loadType, long dropZoneID, glm::vec3* dropZoneList, char (*commandersToLoad)[3], long numMoversPerCommander, MissionInitFrame& f) {
	auto& result = f.result;
	long numMoversLoaded[MAX_MC_PLAYERS] = {0, 0, 0, 0, 0, 0, 0, 0};
	long numDropZonePositions = 0;
	if (numParts)
	{
		//-----------------------------------------------------
		// Since we leave part 0 unused, malloc numParts + 1...
		parts = static_cast<PartPtr>(std::malloc(sizeof(Part) * (numParts + 1)));
		gosASSERT(parts != nullptr);

		memset(parts,0,sizeof(Part) * (numParts + 1));

#ifdef USE_ALTERNATES
		long numSquads = 0;
		long squadMap[MAX_SQUADS];
		for (long & i : squadMap)
			i = -1;
		long maxAlternatives[MAX_SQUADS];
		long randomAlternative[MAX_SQUADS];
		for (long s = 0; s < MAX_SQUADS; s++) {
			maxAlternatives[s] = 0;
			randomAlternative[s] = -1;
		}

		long bigAlternatives = 0;

		for (int i = 1; i < static_cast<long>(numParts + 1); i++) {
			char partName[12];
			mc2::fmt::to_buf(partName, "Part{:d}", mc2::fmt::as_i32(i));

			result = missionFile->seekBlock(partName);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
			// alternatives-prep pass; NumParts promised this Part block.
			MC2_VERIFY(result == NO_ERR, "Mission::init: mission .fit missing [%s] block (alt-prep pass)", partName);
			unsigned long squadNum;
			result = missionFile->readIdULong("squadNum", squadNum);
			long squadIndex = 0;
			for (squadIndex = 0; squadIndex < numSquads; squadIndex++)
				if (squadMap[squadIndex] == squadNum)
					break;
			if (squadIndex == numSquads)
				squadMap[numSquads++] = squadNum;

			long alternatives[MAX_ALTERNATIVES];
			result = missionFile->readIdLongArray("IndicesOfAlternatives", alternatives, MAX_ALTERNATIVES);
			long numAlternatives = 0;
			for (numAlternatives = 0; numAlternatives < MAX_ALTERNATIVES; numAlternatives++)
				if (alternatives[numAlternatives] == -1)
					break;
			maxAlternatives[squadIndex] = std::max(maxAlternatives[squadIndex], numAlternatives);

			bigAlternatives = std::max(bigAlternatives, numAlternatives);
		}

		if (MPlayer && (bigAlternatives > 0)) {
			PAUSE(("Mission.init: multiplayer map has random squads"));
			bigAlternatives = 0;
		}

		long alternateChoice = RandomNumber(bigAlternatives + 1);
		for (int s = 0; s < numSquads; s++)
		{
			randomAlternative[s] = alternateChoice;
			if (GameDifficulty >= 2)
			{
				randomAlternative[s]--;
				randomAlternative[s] = std::max<long>(randomAlternative[s], 1);
			}
		}

		//--------------------------------------------------------
		// This block is optional, and is used for testing only...
		result = missionFile->seekBlock("Squads");
		if (result == NO_ERR)
			for (long i = 0; i < numSquads; i++) {
				char s[128];
				mc2::fmt::to_buf(s, "Squad{:d}", mc2::fmt::as_i32(i));
				unsigned long alternate = -1;
				result = missionFile->readIdULong(s, alternate);
				if (result == NO_ERR)
					randomAlternative[i] = alternate;
			}
#else
		long i;
#endif

		for (int i = 1; i < static_cast<long>(numParts + 1); i++)
		{
			long selectedPartId = i;
			char partName[12];
			mc2::fmt::to_buf(partName, "Part{:d}", mc2::fmt::as_i32(i));

			result = missionFile->seekBlock(partName);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
			// NumParts promised this Part block; a missing block leaves parts[i]
			// zeroed and spawns a garbage object type at loadObjectType(objNumber).
			MC2_VERIFY(result == NO_ERR, "Mission::init: mission .fit missing [%s] block", partName);

#ifdef USE_ALTERNATES
			//----------------------------------------------------------------------
			// If we have alternatives, choose which one we're taking before we read
			// anything else in...
			bool usingAlternate = false;
			unsigned long realPilot = 0;
			unsigned long squadNum;
			result = missionFile->readIdULong("squadNum", squadNum);
			parts[i].squadId = squadNum;
			long squadIndex = 0;
			for (squadIndex = 0; squadIndex < numSquads; squadIndex++)
				if (squadMap[squadIndex] == squadNum)
					break;

			long alternatives[MAX_ALTERNATIVES];
			result = missionFile->readIdLongArray("IndicesOfAlternatives", alternatives, MAX_ALTERNATIVES);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing IndicesOfAlternatives", partName);
			if (maxAlternatives[squadIndex])
			{
				long partId = i;
				if (randomAlternative[squadIndex] > 0)
					partId = alternatives[randomAlternative[squadIndex] - 1];
				if (partId == -1)
					continue;
				Assert(partId > 0, partId, " Mission.init: Bad Alternate ");

				//MUST save off ORIGINAL Pilot.  WE don't load the alternate pilots!!!!!
				usingAlternate = true;
				result = missionFile->readIdULong("Pilot", realPilot);
				// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
				MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing Pilot (alternate)", partName);

				mc2::fmt::to_buf(partName, "Part{:d}", mc2::fmt::as_i32(partId));
				selectedPartId = partId;
				result = missionFile->seekBlock(partName);
				// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
				MC2_VERIFY(result == NO_ERR, "Mission::init: alternate part block [%s] missing", partName);
			}
#endif

			result = missionFile->readIdULong("ObjectNumber",parts[i].objNumber);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
			// objNumber feeds loadObjectType() below; a failed read leaves it
			// uninitialized-then-used to spawn the wrong/garbage object type.
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing ObjectNumber", partName);

			result = missionFile->readIdULong("ControlType", parts[i].controlType);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing ControlType", partName);

			result = missionFile->readIdULong("ControlDataType", parts[i].controlDataType);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing ControlDataType", partName);

			result = missionFile->readIdString("ObjectProfile", parts[i].profileName, 9);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing ObjectProfile", partName);

			// Mod-tolerance: read CSVFile so we can back-fill the ObjectType
			// appearance name when the pak lookup fails for a high ObjectNumber.
			parts[i].csvFile[0] = 0;
			missionFile->readIdString("CSVFile", parts[i].csvFile, sizeof(parts[i].csvFile) - 1);

			result = missionFile->readIdULong("VariantNumber", parts[i].variantNum);
			if (result != NO_ERR)
				parts[i].variantNum = 0;		//FOR NOW!!!!!!!!!!!!!!!!

			if (usingAlternate)
			{
				parts[i].pilot = realPilot;
			}
			else
			{
				result = missionFile->readIdULong("Pilot", parts[i].pilot);
				// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
				MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing Pilot", partName);
			}

			const auto route = mc2::sim::readAuthoredRoute(authoredMissionText, static_cast<std::uint32_t>(i),
				static_cast<std::uint32_t>(selectedPartId), static_cast<std::uint32_t>(parts[i].pilot), Terrain::worldUnitsMapSide);
			if (!route)
				diagnoseAuthoredRoute(i, selectedPartId, parts[i].pilot, "Rejected", route.error());
			else if (*route && (*route)->kind != mc2::sim::OrderKind::None) {
				if (std::ranges::find(authoredOpordPilots, (*route)->pilot) != authoredOpordPilots.end())
					diagnoseAuthoredRoute(i, selectedPartId, parts[i].pilot, "Suppressed", "enhanced_opord");
				else authoredRoutes.push_back(**route);
			}

			result = missionFile->readIdFloat("PositionX",parts[i].position.x);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing PositionX", partName);

			result = missionFile->readIdFloat("PositionY",parts[i].position.y);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing PositionY", partName);

			parts[i].position.z = -1.0;

			result = missionFile->readIdFloat("Rotation",parts[i].rotation);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing Rotation", partName);

			result = missionFile->readIdChar("TeamId",parts[i].teamId);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing TeamId", partName);
			//--------------------------------------------------------------------------
			// Hack for singleplayer, until editor spits this out properly for allies...
			if (!MPlayer && (parts[i].teamId == 2))
				parts[i].teamId = 0;

			if (dropZoneList && (dropZoneID == parts[i].teamId))
				dropZoneList[numDropZonePositions++] = parts[i].position;

			char cmdId = 0;
			result = missionFile->readIdChar("CommanderId", cmdId);
			if (result != NO_ERR)
			{
				result = missionFile->readIdLong("CommanderId", parts[i].commanderID);
				gosASSERT(result == NO_ERR);
			}
			else
			{
				parts[i].commanderID = cmdId;
			}

			if (loadType == MISSION_LOAD_MP_QUICKSTART) {
				long origCommanderID = parts[i].commanderID;
				parts[i].commanderID = commandersToLoad[origCommanderID][0];
				parts[i].teamId = commandersToLoad[origCommanderID][1];
				if (commandersToLoad[origCommanderID][0] > -1) {
					if (numMoversLoaded[commandersToLoad[origCommanderID][0]] == numMoversPerCommander) {
						parts[i].commanderID = -1;
						parts[i].teamId = -1;
						}
					else
						numMoversLoaded[commandersToLoad[origCommanderID][0]]++;
				}
			}

			parts[i].gestureId = 2; // this has never changed

			result = missionFile->readIdULong("BaseColor",parts[i].baseColor);
			if (result != NO_ERR || MPlayer )
				parts[i].baseColor = prefs.baseColor;

			result = missionFile->readIdULong("HighlightColor1",parts[i].highlightColor1);
			if (result != NO_ERR || MPlayer)
				parts[i].highlightColor1 = prefs.highlightColor;

			result = missionFile->readIdULong("HighlightColor2",parts[i].highlightColor2);
			if (result != NO_ERR || MPlayer )
				parts[i].highlightColor2 = prefs.highlightColor;

			parts[i].velocity = 0;

			result = missionFile->readIdLong("Active",parts[i].active);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing Active", partName);

			result = missionFile->readIdLong("Exists",parts[i].exists);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing Exists", partName);

			float fDamage = 0.0f;
			result = missionFile->readIdFloat("Damage",fDamage);
			if (result == NO_ERR) {
				if (fDamage >= 1.0) {
					parts[i].destroyed = true;
				};
			}

			result = missionFile->readIdChar("MyIcon", parts[i].myIcon);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing MyIcon", partName);

			result = missionFile->readIdChar("MyIcon", parts[i].myIcon);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing MyIcon", partName);

			result = missionFile->readIdBoolean("Captureable", parts[i].captureable);
			if (result != NO_ERR)
				parts[i].captureable = FALSE;

			float increment = 5.0f/(numParts + 1);
			loadProgress += increment;

		}
	}
}
