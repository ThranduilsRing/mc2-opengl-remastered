#define OBJECTIVE_CPP
/*************************************************************************************************\
Objective.cpp			: Implementation of the Objective component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"terrain.h"
#include"gamecam.h"   // Camera::projectModernClipGL for the overview marker overlay

#include"objective.h"

#ifndef INIFILE_H
#include"inifile.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef BLDNG_H
#include"bldng.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef TACMAP_H
#include"tacmap.h"
#endif

#ifndef AFONT_H
#include"afont.h"
#endif

#include"logisticsdata.h"
#include"team.h"
#include"comndr.h"

#include"controlgui.h"

#include "../resource.h"

#include "platform_tchar.h"
#include<assert.h>
#include "mc2fmt.h"
#include "objective_text.h"

#include <format>
#include <optional>
#include <string>
#include <cmath>
#include <filesystem>
#include "save_archive.h"
#include "timing.h"
#include "../GameOS/gameos/diagnostic_trace.h"

static const char *g_actionSpeciesStringArray[] = {
	"PlayBIK", "PlayWAV", "DisplayTextMessage", "DisplayResourceTextMessage",
	"SetBooleanFlag", "MakeNewTechnologyAvailable", "_RemoveStructure",
};

bool objectiveObservationEnabled() {
	return mc2_diag::tagEnabled("OBJECTIVES") && mc2_diag::tagRequested("OBJECTIVES");
}

std::string objectiveObservationQuote(std::string_view text) {
	std::string result = "\"";
	for (unsigned char c : text) {
		if (c == '\"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
		else if (c < 32) { result += std::format("\\u{:04x}", c); }
		else { result += static_cast<char>(c); }
	}
	return result + '"';
}

CObjectives::Observation* objectiveObservation() {
	if (!objectiveObservationEnabled() || !Team::teams[0]) { return nullptr; }
	return &Team::teams[0]->objectives.observation;
}

namespace {
struct ObservationTraversal {
	CObjectives::Observation* observation = objectiveObservation();
	bool outer = observation && !observation->traversal;
	ObservationTraversal(const char* source) {
		if (outer) {
			observation->traversal = observation->nextTraversal++;
			observation->traversalSource = source;
		}
	}
	~ObservationTraversal() {
		if (outer) { observation->traversal = 0; observation->traversalSource = "write"; }
	}
};
}

void CObjectives::Observation::Begin(FitIniFile& file) {
	*this = Observation{};
	if (!objectiveObservationEnabled()) { return; }
	mc2::save::Bytes bytes;
	const char domain[] = "MC2-OBJECTIVES/1";
	bytes.insert(bytes.end(), domain, domain + sizeof(domain));
	auto append = [&](File& input) {
		const auto length = input.getLength();
		for (int i = 0; i < 8; ++i) { bytes.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(length) >> (8 * i))); }
		const auto offset = bytes.size();
		bytes.resize(offset + length);
		const auto position = input.getLogicalPosition();
		const bool read = input.read(0, bytes.data() + offset, length) == static_cast<long>(length);
		input.seek(position);
		return read;
	};
	File pak;
	const auto pakPath = std::filesystem::path(file.getFilename() ? file.getFilename() : "").replace_extension(".pak").string();
	if (!append(file) || pak.open(pakPath.c_str()) != NO_ERR || !append(pak)) {
		rejected = true;
		mc2::fmt::print_err("[OBJECTIVES] rejected: exact FIT/PAK inputs unavailable\n");
		return;
	}
	for (auto byte : mc2::save::sha256(bytes)) { missionHash += std::format("{:02x}", byte); }
}

unsigned long long CObjectives::Observation::Emit(const char* kind, unsigned long long cause,
                                                 const std::string& data, unsigned long long id) {
	if (!objectiveObservationEnabled() || completed || rejected || missionHash.empty()) { return 0; }
	const double time = mission ? mission->actualTime : 0;
	if (!std::isfinite(time) || time < 0 || turn < 0) {
		rejected = true;
		mc2::fmt::print_err("[OBJECTIVES] rejected: invalid simulation clock\n");
		return 0;
	}
	if (!id) { id = nextId++; }
	const std::string record = std::format("{{\"mission\":\"{}\",\"seq\":{},\"id\":\"o{}\",\"frame\":{},\"time\":{},\"kind\":\"{}\",\"cause\":{},\"data\":{}}}",
		missionHash, sequence + 1, id, turn, time, kind,
		cause ? std::format("\"o{}\"", cause) : "null", data);
	if (record.size() + sizeof("OBJECTIVES/1 ") - 1 > 16384) {
		rejected = true;
		mc2::fmt::print_err("[OBJECTIVES] rejected: oversized observation\n");
		return 0;
	}
	if (!mc2_diag::writeEvent("OBJECTIVES", 1, turn, record.c_str())) {
		rejected = true;
		mc2::fmt::print_err("[OBJECTIVES] rejected: diagnostic write failed\n");
		return 0;
	}
	++sequence;
	mc2::fmt::print_out("OBJECTIVES/1 {}\n", record);
	fflush(stdout);
	return id;
}

void CObjectives::Observation::SinkResult(const char* sink, const char* stage, const char* result,
                                          const std::string& payload, unsigned long long cause) {
	if (!cause) { cause = dispatch; }
	if (!cause || rejected) { return; }
	const std::string data = std::format("{{\"sink\":{},\"stage\":{},\"result\":{},\"payload\":{}}}",
		objectiveObservationQuote(sink), objectiveObservationQuote(stage), objectiveObservationQuote(result), payload);
	if (dispatch) { pendingSinks.push_back({cause, data}); }
	else { Emit("sink", cause, data); }
}

void objectiveObservationSink(const char* sink, const char* stage, const char* result, const char* payload) {
	if (auto* observation = objectiveObservation()) {
		observation->SinkResult(sink, stage, result, objectiveObservationQuote(payload ? payload : ""));
	}
}

void objectiveObservationWav(const char* requested, const char* resource, int channel, const char* result, bool accepted) {
	if (auto* observation = objectiveObservation()) {
		observation->SinkResult("wav", "resource_channel", result,
			std::format("{{\"requested\":{},\"resource\":{},\"channel\":{},\"resourceAccepted\":{},\"channelAssigned\":{}}}",
				objectiveObservationQuote(requested), objectiveObservationQuote(resource), channel, accepted, accepted));
	}
}

void CObjectives::Observation::Finish() {
	if (finalResult == -1 || dispatch || completed || (finalResult == mis_PLAYER_WIN_BIG && !settled)) { return; }
	if (Emit("complete", 0, std::format("{{\"count\":{},\"result\":{}}}", sequence, finalResult))) { completed = true; }
}

void CObjective::ObserveState(const char* kind, int oldState, int newState) {
	if (m_fitOrdinal < 0) { return; }
	if (auto* observation = objectiveObservation()) {
		m_observationState = observation->Emit(kind, m_observationState,
			std::format("{{\"team\":{},\"objective\":{},\"old\":{},\"new\":{},\"traversal\":{},\"source\":\"{}\"}}",
			m_alignment, m_fitOrdinal, oldState, newState, observation->traversal, observation->traversalSource));
	}
}

void CObjective::IsActive(bool active) {
	const bool old = m_isActive;
	m_isActive = active;
	if (old != active) { ObserveState("activate", old, active); }
}

void CObjective::ObserveCBills(int before, int after) {
	if (m_fitOrdinal >= 0) {
		if (auto* observation = objectiveObservation()) {
			observation->Emit("resolve", m_observationState,
				std::format("{{\"scope\":\"cbills\",\"team\":{},\"objective\":{},\"old\":{},\"new\":{}}}",
				m_alignment, m_fitOrdinal, before, after));
		}
	}
}

void CObjective::Dispatch(CObjectiveAction& action, const char* list) {
	auto* observation = m_fitOrdinal >= 0 ? objectiveObservation() : nullptr;
	if (!observation || !m_observationState || observation->rejected) {
		action.Execute();
		return;
	}
	const auto parentDispatch = observation->dispatch;
	const auto id = observation->nextId++;
	observation->dispatch = id;
	const auto firstSink = observation->pendingSinks.size();
	const int result = action.Execute();
	observation->Emit("dispatch", m_observationState,
		std::format("{{\"team\":{},\"objective\":{},\"list\":\"{}\",\"entry\":{},\"species\":{},\"payload\":{},\"result\":{}}}",
			m_alignment, m_fitOrdinal, list, action.observationOrdinal,
			objectiveObservationQuote(g_actionSpeciesStringArray[action.Species()]), action.observationPayload, result), id);
	observation->dispatch = parentDispatch;
	for (auto i = firstSink; i < observation->pendingSinks.size(); ++i) {
		const auto& sink = observation->pendingSinks[i];
		observation->Emit("sink", sink.cause, sink.data);
	}
	observation->pendingSinks.resize(firstSink);
}

float CObjective::s_blinkLength = .5;
float CObjective::s_lastBlinkTime = 0.f;
unsigned long CObjective::s_blinkColor = SB_YELLOW;
std::unique_ptr<aFont> CObjective::s_markerFont = nullptr;
float MaxExtractUnitDistance = 0.0f;


const char *g_conditionSpeciesStringArray[] = {
	"DestroyAllEnemyUnits",
		"DestroyNumberOfEnemyUnits",
		"DestroyEnemyUnitGroup",
		"DestroySpecificEnemyUnit",
		"DestroySpecificStructure",

		"CaptureOrDestroyAllEnemyUnits",
		"CaptureOrDestroyNumberOfEnemyUnits",
		"CaptureOrDestroyEnemyUnitGroup",
		"CaptureOrDestroySpecificEnemyUnit",
		"CaptureOrDestroySpecificStructure",

		"DeadOrFledAllEnemyUnits",
		"DeadOrFledNumberOfEnemyUnits",
		"DeadOrFledEnemyUnitGroup",
		"DeadOrFledSpecificEnemyUnit",

		"CaptureSpecificUnit",
		"CaptureSpecificStructure",

		"GuardSpecificUnit",
		"GuardSpecificStructure",

		"MoveAnyUnitToArea",
		"MoveAllUnitsToArea",
		"MoveAllSurvivingUnitsToArea",
		"MoveAllSurvivingMechsToArea",

		"BooleanFlagIsSet",
		"ElapsedMissionTime",
};


static bool MoverIsDeadOrDisabled(const long pMoverWID) 
{
	bool retval = false;

	Mover *pMover = (MoverPtr)ObjectManager->getByWatchID(pMoverWID);
	assert(pMover);

	if (pMover->isDisabled() || pMover->isDestroyed()) {
		retval = true;
	} else {
		assert(pMover->getPilot());
		long wstatus = pMover->getPilot()->getStatus();
		if ((WARRIOR_STATUS_DEAD == wstatus)
			|| (WARRIOR_STATUS_EJECTED == wstatus)) {
			retval = true;
		}
	}
	return retval;
}

static bool MoverIsDeadOrDisabled(const MoverPtr pMover) 
{
	bool retval = false;
	assert(pMover);

	if (pMover->isDisabled() || pMover->isDestroyed()) {
		retval = true;
	} else {
		assert(pMover->getPilot());
		long wstatus = pMover->getPilot()->getStatus();
		if ((WARRIOR_STATUS_DEAD == wstatus)
			|| (WARRIOR_STATUS_EJECTED == wstatus)) {
			retval = true;
		}
	}
	return retval;
}

static bool MoverIsDeadOrDisabledOrFled(const MoverPtr pMover) {
	assert(pMover);
	bool retval = false;
	if (pMover->isDisabled() || pMover->isDestroyed()) {
		retval = true;
	} else {
		assert(pMover->getPilot());
		long wstatus = pMover->getPilot()->getStatus();
		if ((WARRIOR_STATUS_DEAD == wstatus)
			|| (WARRIOR_STATUS_EJECTED == wstatus)) {
			retval = true;
		} else if (WARRIOR_STATUS_WITHDRAWN == wstatus) {
			retval = true;
		}
	}
	return retval;
}

static bool MoverIsCaptured(const MoverPtr pMover, const long teamID) {
	assert(pMover);
	bool retval = false;
	if (pMover->isDestroyed()) {
		retval = false;
	} else {
		if ( pMover->getTeamId() == teamID ) {
			retval = true;
		}
	}
	return retval;
}

static bool MoverIsCaptured(const long pMoverWID, const long teamID) 
{
	Mover *pMover = (MoverPtr)ObjectManager->getByWatchID(pMoverWID);
	assert(pMover);

	bool retval = false;
	if (pMover->isDestroyed()) 
	{
		retval = false;
	} 
	else 
	{
		if ( pMover->getTeamId() == teamID ) 
		{
			retval = true;
		}
	}

	return retval;
}

static long sReadIdFloat(FitIniFile* missionFile, const char *varName, float &value) {
	long result = 0;
	float tmpFloat;
	result = missionFile->readIdFloat((char *)varName, tmpFloat);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpFloat;
	}
	return result;
}

static long sReadIdBoolean(FitIniFile* missionFile, const char *varName, bool &value) {
	long result = 0;
	bool tmpBool;
	result = missionFile->readIdBoolean((char *)varName, tmpBool);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpBool;
	}
	return result;
}

static long sReadIdWholeNum(FitIniFile* missionFile, const char *varName, int &value) {
	long result = 0;
	unsigned long tmpULong;
	result = missionFile->readIdULong((char *)varName, tmpULong);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpULong;
	}
	return result;
}

static long sReadIdLongInt(FitIniFile* missionFile, const char *varName, int &value) {
	long result = 0;
	long tmpLong;
	result = missionFile->readIdLong((char *)varName, tmpLong);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpLong;
	}
	return result;
}

static bool areCloseEnough(float f1, float f2)
{
	bool retval = false;
	float diff = fabs(f1 - f2);
	float bigger = fabs(f1);
	if (fabs(f2) > bigger) {
		bigger = fabs(f2);
	}
	float proportion_diff = 0.0;
	if (0.0 != bigger) {
		proportion_diff = diff / bigger;
	}
	if ((pow(10.0, -FLT_DIG) * 10.0/*arbitrary*/) > proportion_diff) {
		retval = true;
	}
	return retval;
}

objective_status_type CDestroyAllEnemyUnits::Status() {
	long numMovers = ObjectManager->getNumMovers();
	long i;
	for (i = 0; i < numMovers; i+= 1) {
		MoverPtr pMover = ObjectManager->getMover(i);
		assert(pMover);
		if (pMover->getTeamId() != Alignment())
			if (!(MoverIsDeadOrDisabled(pMover) || MoverIsCaptured(pMover, Alignment()))) {
				return OS_UNDETERMINED;
		}
	}
	return OS_SUCCESSFUL;
}

bool CNumberOfUnitsObjectiveCondition::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = sReadIdWholeNum(missionFile, "Num", m_num);
	if (NO_ERR != result) { return false; }

	return true;
}

objective_status_type CDestroyNumberOfEnemyUnits::Status() {
	int numDestroyed = 0;
	long numMovers = ObjectManager->getNumMovers();
	long i;
	for (i = 0; i < numMovers; i+= 1) {
		MoverPtr pMover = ObjectManager->getMover(i);
		assert(pMover);
		if (pMover->getTeamId() != Alignment())
			if (MoverIsDeadOrDisabled(pMover) || MoverIsCaptured(pMover, Alignment())) {
				numDestroyed += 1;
			}
	}

	if (numDestroyed >= m_num) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

bool CSpecificEnemyUnitObjectiveCondition::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdInt("MoverWID", m_pUnitWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		MoverPtr pMover = nullptr;
		if (ObjectManager) {
			long numMovers = ObjectManager->getNumMovers();
			long i;
			for (i = 0; i < numMovers; i+= 1) {
				pMover = ObjectManager->getMover(i);
				bool close_x = areCloseEnough(pMover->getPosition().x, positionX);
				bool close_y = areCloseEnough(pMover->getPosition().y, positionY);
				// FIXME: sebi: one of the missions has bad data and so this condition never worked which led to assert(false)
				// I could ont change it in data, so had to disable this check in the code ;-(
				if (pMover && /*(pMover->getTeamId() != Alignment()) &&*/ close_x && close_y) {
					break;
				}
			}
			if (!(i < numMovers)) { assert(false); return false; }
		}
		if (pMover)
			m_pUnitWID = pMover->getWatchID();
		else
			m_pUnitWID = 0;
	}

	return true;
}

objective_status_type CDestroySpecificEnemyUnit::Status() {
	assert(m_pUnitWID);
	if (MoverIsDeadOrDisabled(m_pUnitWID) || MoverIsCaptured(m_pUnitWID, Alignment())) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

bool CSpecificStructureObjectiveCondition::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdLong("BuildingWID", m_pBuildingWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		BuildingPtr pBuilding = nullptr;
		if (ObjectManager) {
			pBuilding = ObjectManager->getBuilding(0);
			long i;
			for (i = 0; (nullptr != pBuilding); i+= 1) {
				pBuilding = ObjectManager->getBuilding(i);
				if ((nullptr != pBuilding) && areCloseEnough(pBuilding->getPosition().x, positionX)  && areCloseEnough(pBuilding->getPosition().y, positionY)) {
					break;
				}
			}
			if (!pBuilding) { assert(false); return false; }
		}
		if (pBuilding)
			m_pBuildingWID = pBuilding->getWatchID();
		else
			m_pBuildingWID = 0;
	}

	return true;
}

objective_status_type CDestroySpecificStructure::Status() 
{
	Building *m_pBuilding = (Building *)ObjectManager->getByWatchID(m_pBuildingWID);
	if (m_pBuilding->isDestroyed()) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

objective_status_type CCaptureOrDestroyAllEnemyUnits::Status() {
	long numMovers = ObjectManager->getNumMovers();
	long i;
	for (i = 0; i < numMovers; i+= 1) {
		MoverPtr pMover = ObjectManager->getMover(i);
		assert(pMover);
		if (pMover->getTeamId() != Alignment())
			if (!MoverIsDeadOrDisabled(pMover)) {
				if (!MoverIsCaptured(pMover, Alignment())) { /*Is this necessary? When a mover is captured, is it no longer returned as a "BadMover"? */
					return OS_UNDETERMINED;
				}
		}
	}
	return OS_SUCCESSFUL;
}

objective_status_type CCaptureOrDestroyNumberOfEnemyUnits::Status() {
	int numCapturedOrDestroyed = 0;
	long numMovers = ObjectManager->getNumMovers();
	long i;
	for (i = 0; i < numMovers; i+= 1) {
		MoverPtr pMover = ObjectManager->getMover(i);
		assert(pMover);
		if (pMover->getTeamId() != Alignment())
			if (MoverIsDeadOrDisabled(pMover) || MoverIsCaptured(pMover, Alignment())/*redundant?*/) {
				numCapturedOrDestroyed += 1;
			}
	}

	if (numCapturedOrDestroyed >= m_num) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

bool CCaptureOrDestroySpecificEnemyUnit::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdInt("MoverWID", m_pUnitWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		MoverPtr pMover = nullptr;
		if (ObjectManager) {
			long numMovers = ObjectManager->getNumMovers();
			long i;
			for (i = 0; i < numMovers; i+= 1) {
				pMover = ObjectManager->getMover(i);
				if (pMover && (pMover->getTeamId() != Alignment()) && areCloseEnough(pMover->getPosition().x, positionX)  && areCloseEnough(pMover->getPosition().y, positionY)) {
					break;
				}
			}
			if (!(i < numMovers)) { assert(false); return false; }
			pMover->setFlag(OBJECT_FLAG_CAPTURABLE, true);
		}
		if (pMover)
			m_pUnitWID = pMover->getWatchID();
		else
			m_pUnitWID = 0;
	}

	return true;
}

objective_status_type CCaptureOrDestroySpecificEnemyUnit::Status() 
{
	MoverPtr m_pUnit = (MoverPtr)ObjectManager->getByWatchID(m_pUnitWID);
	assert(m_pUnit);
	if (MoverIsDeadOrDisabled(m_pUnit)|| MoverIsCaptured(m_pUnit, Alignment())/*redundant?*/) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

bool CCaptureOrDestroySpecificStructure::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdLong("BuildingWID",m_pBuildingWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		BuildingPtr pBuilding = nullptr;
		if (ObjectManager) {
			pBuilding = ObjectManager->getBuilding(0);
			long i;
			for (i = 0; (nullptr != pBuilding); i+= 1) {
				pBuilding = ObjectManager->getBuilding(i);
				if ((nullptr != pBuilding) && areCloseEnough(pBuilding->getPosition().x, positionX)  && areCloseEnough(pBuilding->getPosition().y, positionY)) {
					break;
				}
			}
			if (!pBuilding) { assert(false); return false; }
			pBuilding->setFlag(OBJECT_FLAG_CAPTURABLE, true);
		}
		if (pBuilding)
			m_pBuildingWID = pBuilding->getWatchID();
		else
			m_pBuildingWID = 0;
	}

	return true;
}

objective_status_type CCaptureOrDestroySpecificStructure::Status() 
{
	Building* m_pBuilding = (Building *)ObjectManager->getByWatchID(m_pBuildingWID);
	if (m_pBuilding->isDestroyed()) 
	{
		return OS_SUCCESSFUL;
	} 
	else if ( m_pBuilding->getTeamId() == Alignment() ) 
	{
		return OS_SUCCESSFUL;
	} 
	else 
	{
		return OS_UNDETERMINED;
	}
}

objective_status_type CDeadOrFledAllEnemyUnits::Status() {
	long numMovers = ObjectManager->getNumMovers();
	long i;
	for (i = 0; i < numMovers; i+= 1) {
		MoverPtr pMover = ObjectManager->getMover(i);
		assert(pMover);
		if (!MoverIsDeadOrDisabledOrFled(pMover)) {
			return OS_UNDETERMINED;
		}
	}
	return OS_SUCCESSFUL;
}

objective_status_type CDeadOrFledNumberOfEnemyUnits::Status() {
	int numDeadOrFled = 0;
	long numMovers = ObjectManager->getNumMovers();
	long i;
	for (i = 0; i < numMovers; i+= 1) {
		MoverPtr pMover = ObjectManager->getMover(i);
		assert(pMover);
		if (MoverIsDeadOrDisabledOrFled(pMover)) {
			numDeadOrFled += 1;
		}
	}

	if (numDeadOrFled >= m_num) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

objective_status_type CDeadOrFledSpecificEnemyUnit::Status() 
{
	MoverPtr m_pUnit = (MoverPtr)ObjectManager->getByWatchID(m_pUnitWID);
	assert(m_pUnit);
	if (MoverIsDeadOrDisabledOrFled(m_pUnit)) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

bool CCaptureUnit::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdInt("MoverWID", m_pUnitWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		MoverPtr pMover = nullptr;
		if (ObjectManager) {
			long numMovers = ObjectManager->getNumMovers();
			long i;
			for (i = 0; i < numMovers; i+= 1) {
				pMover = ObjectManager->getMover(i);
				if (pMover && (pMover->getTeamId() != Alignment()) && areCloseEnough(pMover->getPosition().x, positionX)  && areCloseEnough(pMover->getPosition().y, positionY)) {
					break;
				}
			}
			if (!(i < numMovers)) { assert(false); return false; }
			pMover->setFlag(OBJECT_FLAG_CAPTURABLE, true);
		}
		if (pMover)
			m_pUnitWID = pMover->getWatchID();
		else
			m_pUnitWID = 0;
	}

	return true;
}

objective_status_type CCaptureUnit::Status() 
{
	MoverPtr m_pUnit = (MoverPtr)ObjectManager->getByWatchID(m_pUnitWID);

	if (!m_pUnit || m_pUnit->isDestroyed()) 
	{
		return OS_FAILED;
	} 
	else if (m_pUnit && m_pUnit->getTeamId() == Alignment() ) 
	{
		return OS_SUCCESSFUL;
	} 
	else 
	{
		return OS_UNDETERMINED;
	}
}

bool CCaptureStructure::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = missionFile->readIdLong("BuildingWID", m_pBuildingWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		BuildingPtr pBuilding = nullptr;
		if (ObjectManager) 
		{
			pBuilding = ObjectManager->getBuilding(0);
			long i;
			for (i = 0; (nullptr != pBuilding); i+= 1) 
			{
				pBuilding = ObjectManager->getBuilding(i);
				if ((nullptr != pBuilding) && areCloseEnough(pBuilding->getPosition().x, positionX)  && areCloseEnough(pBuilding->getPosition().y, positionY)) 
				{
					break;
				}
			}
			if (!pBuilding) { assert(false); return false; }
			pBuilding->setFlag(OBJECT_FLAG_CAPTURABLE, true);
		}
		if (pBuilding)
			m_pBuildingWID = pBuilding->getWatchID();
		else
			m_pBuildingWID = 0;
	}

	return true;
}

objective_status_type CCaptureStructure::Status() 
{
	Building *m_pBuilding = (Building *)ObjectManager->getByWatchID(m_pBuildingWID);
	if (m_pBuilding->isDestroyed()) 
	{
		return OS_FAILED;
	} 
	else if ( m_pBuilding->getTeamId() == Alignment() ) 
	{
		return OS_SUCCESSFUL;
	} 
	else 
	{
		return OS_UNDETERMINED;
	}
}

bool CGuardSpecificUnit::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdInt("MoverWID", m_pUnitWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		MoverPtr pMover = nullptr;
		if (ObjectManager) {
			long numMovers = ObjectManager->getNumMovers();
			long i;
			for (i = 0; i < numMovers; i+= 1) {
				pMover = ObjectManager->getMover(i);
				if (pMover && (pMover->getTeamId() == Alignment()) && areCloseEnough(pMover->getPosition().x, positionX)  && areCloseEnough(pMover->getPosition().y, positionY)) {
					break;
				}
			}
			if (!(i < numMovers)) { assert(false); return false; }
		}
		if (pMover)
			m_pUnitWID = pMover->getWatchID();
		else
			m_pUnitWID = 0;
	}

	return true;
}

objective_status_type CGuardSpecificUnit::Status() 
{
	Mover *m_pUnit = (Mover *)ObjectManager->getByWatchID(m_pUnitWID);

	if (m_pUnit->isDestroyed()) {
		return OS_FAILED;
	} else if (  m_pUnit->getTeamId() != Alignment() ) {
		return OS_FAILED;
	} else {
		return OS_UNDETERMINED;
	}
}

objective_status_type CGuardSpecificStructure::Status() 
{
	Building *m_pBuilding = (Building *)ObjectManager->getByWatchID(m_pBuildingWID);
	if (m_pBuilding->isDestroyed()) 
	{
		return OS_FAILED;
	}
	else if ( m_pBuilding->getCaptured() && (m_pBuilding->getTeamId() != Alignment())) 
	{
		return OS_FAILED;
	} 
	else if ((mission->m_timeLimit != -1.0f) && (mission->actualTime > mission->m_timeLimit)) 
	{
		return OS_SUCCESSFUL;
	} 
	else 
	{
		return OS_UNDETERMINED;
	}
}

bool CAreaObjectiveCondition::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = sReadIdFloat(missionFile, "TargetCenterX", m_targetCenterX);
	if (NO_ERR != result) { return false; }
	result = sReadIdFloat(missionFile, "TargetCenterY", m_targetCenterY);
	if (NO_ERR != result) { return false; }
	result = sReadIdFloat(missionFile, "TargetRadius", m_targetRadius);
	if (NO_ERR != result) { return false; }
	return true;
}

objective_status_type CMoveAnyUnitToArea::Status() 
{
	long numMovers = ObjectManager->getNumMovers();
	long i;
	if (MPlayer) {
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);

			if (pMover->getTeamId() == Alignment())
			{
				float dx = pMover->getPosition().x - m_targetCenterX;
				float dy = pMover->getPosition().y - m_targetCenterY;
				if (dx*dx + dy*dy < m_targetRadius*m_targetRadius) 
				{
					return OS_SUCCESSFUL;
				}
			}
		}
		}
	else {
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);

			if (pMover->isOnGUI() && (pMover->getCommanderId() == Commander::home->getId()))
			{
				float dx = pMover->getPosition().x - m_targetCenterX;
				float dy = pMover->getPosition().y - m_targetCenterY;
				if (dx*dx + dy*dy < m_targetRadius*m_targetRadius) 
				{
					return OS_SUCCESSFUL;
				}
			}
		}
	}
	return OS_UNDETERMINED;
}

objective_status_type CMoveAllUnitsToArea::Status() 
{
	long numMovers = ObjectManager->getNumMovers();
	long i;
	if (MPlayer) {
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);

			if (pMover->isRecover()) 
				continue;

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->getTeamId() == Alignment())
			{
				float dx = pMover->getPosition().x - m_targetCenterX;
				float dy = pMover->getPosition().y - m_targetCenterY;
				if (dx*dx + dy*dy > m_targetRadius*m_targetRadius) 
				{
					return OS_UNDETERMINED;
				}
			}
		}
		}
	else {
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);

			if (pMover->isRecover()) 
				continue;

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->isOnGUI() && (pMover->getCommanderId() == Commander::home->getId()))
			{
				float dx = pMover->getPosition().x - m_targetCenterX;
				float dy = pMover->getPosition().y - m_targetCenterY;
				if (dx*dx + dy*dy > m_targetRadius*m_targetRadius) 
				{
					return OS_UNDETERMINED;
				}
			}
		}
	}

	return OS_SUCCESSFUL;
}

objective_status_type CMoveAllSurvivingUnitsToArea::Status() 
{
	long numMovers = ObjectManager->getNumMovers();
	long i;
	if (MPlayer) {
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);
			if (pMover->isRecover()) 
				continue;

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->getTeamId() == Alignment())
			{
				if (!MoverIsDeadOrDisabledOrFled(pMover)) 
				{
					float dx = pMover->getPosition().x - m_targetCenterX;
					float dy = pMover->getPosition().y - m_targetCenterY;
					if (dx*dx + dy*dy > m_targetRadius*m_targetRadius) 
					{
						return OS_UNDETERMINED;
					}
				}
			}
		}
		}
	else {
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);
			if (pMover->isRecover()) 
				continue;

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->isOnGUI() && (pMover->getCommanderId() == Commander::home->getId()))
			{
				if (!MoverIsDeadOrDisabledOrFled(pMover)) 
				{
					float dx = pMover->getPosition().x - m_targetCenterX;
					float dy = pMover->getPosition().y - m_targetCenterY;
					if (dx*dx + dy*dy > m_targetRadius*m_targetRadius) 
					{
						return OS_UNDETERMINED;
					}
				}
			}
		}
	}

	return OS_SUCCESSFUL;
}

objective_status_type CMoveAllSurvivingMechsToArea::Status() 
{
	long numMovers = ObjectManager->getNumMovers();
	long i;
	if (MPlayer) 
	{
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);
			if (!pMover->isMech() || (pMover->isMech() && (pMover->getMoveType() == MOVETYPE_AIR))) 
				continue;

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->getTeamId() == Alignment())
			{
				if (!MoverIsDeadOrDisabledOrFled(pMover)) 
				{
					float dx = pMover->getPosition().x - m_targetCenterX;
					float dy = pMover->getPosition().y - m_targetCenterY;
					if (dx*dx + dy*dy > m_targetRadius*m_targetRadius) 
					{
						return OS_UNDETERMINED;
					}
				}
			}
		}		
	}
	else 
	{
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);
			if (!pMover->isMech() || (pMover->isMech() && (pMover->getMoveType() == MOVETYPE_AIR))) 
				continue;

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->isOnGUI() && (pMover->getCommanderId() == Commander::home->getId()))
			{
				if (!MoverIsDeadOrDisabledOrFled(pMover)) 
				{
					float dx = pMover->getPosition().x - m_targetCenterX;
					float dy = pMover->getPosition().y - m_targetCenterY;
					if (dx*dx + dy*dy > m_targetRadius*m_targetRadius) 
					{
						return OS_UNDETERMINED;
					}
				}
			}
		}

		//All of our units are in the area.  NOW check to make no ENEMY units are within x Distance of the area.
		float MaxUnitDistanceSquared = (MaxExtractUnitDistance * MaxExtractUnitDistance);
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->getCommanderId() != Commander::home->getId())
			{
				if (!MoverIsDeadOrDisabledOrFled(pMover)) 
				{
					float dx = pMover->getPosition().x - m_targetCenterX;
					float dy = pMover->getPosition().y - m_targetCenterY;
					if (dx*dx + dy*dy < MaxUnitDistanceSquared)
					{
						return OS_UNDETERMINED;
					}
				}
			}
		}
	}

	return OS_SUCCESSFUL;
}

bool CBooleanFlagIsSet::Read( FitIniFile* missionFile )
{
	std::optional<std::string> flagID = objective_text::readFlagID(*missionFile, "FlagID", "FlagIndex");
	if (!flagID) { return false; }
	m_flagID = std::move(*flagID);
	sReadIdBoolean(missionFile, "Value", m_value);
	return true;
}

objective_status_type CBooleanFlagIsSet::Status() 
{
	//if (mission->missionObjectives.boolFlags.getElementValue(m_flagID) == m_value) {
	if (Team::teams[Alignment()]->objectives.boolFlags.getElementValue(m_flagID) == m_value) {
		return OS_SUCCESSFUL;
	}
	return OS_UNDETERMINED;
}

bool CElapsedMissionTime::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = sReadIdFloat(missionFile, "Time", m_time);
	if (NO_ERR != result) { return false; }
	return true;
}

objective_status_type CElapsedMissionTime::Status() {
	if (mission->actualTime >= m_time) {
		return OS_SUCCESSFUL;
	}
	return OS_UNDETERMINED;
}

bool CPlayBIK::Read( FitIniFile* missionFile )
{
	std::optional<std::string> fileName = objective_text::read(*missionFile, "AVIFileName");
	if (!fileName) { return false; }
	m_pathname = "./" + *fileName;
	if (objectiveObservationEnabled()) { observationPayload = objectiveObservationQuote(*fileName); }
	return true;
}

int CPlayBIK::Execute()
{
	ControlGui::instance->playMovie( m_pathname.c_str() );
	return true;
}

bool CPlayWAV::Read( FitIniFile* missionFile )
{
	std::optional<std::string> fileName = objective_text::read(*missionFile, "WAVFileName");
	if (!fileName) { return false; }
	m_pathname = "./" + *fileName;
	if (objectiveObservationEnabled()) { observationPayload = objectiveObservationQuote(*fileName); }
	return true;
}

int CPlayWAV::Execute()
{
	soundSystem->playDigitalStream(m_pathname.c_str());
	return true;
}

bool CDisplayTextMessage::Read( FitIniFile* missionFile )
{
	std::optional<std::string> message = objective_text::read(*missionFile, "Message");
	if (!message) { return false; }
	m_message = std::move(*message);
	if (objectiveObservationEnabled()) { observationPayload = objectiveObservationQuote(m_message); }
	return true;
}

int CDisplayTextMessage::Execute()
{
	ControlGui::instance->setChatText("", m_message.c_str(), 0xf0000000, 0);
	return true;
}

bool CDisplayResourceTextMessage::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = sReadIdWholeNum(missionFile, "ResourceStringID", m_resourceStringID);
	if (NO_ERR != result) { return false; }
	if (objectiveObservationEnabled()) { observationPayload = std::to_string(m_resourceStringID); }
	return true;
}

int CDisplayResourceTextMessage::Execute()
{
	const std::string resourceText = loadResourceString(m_resourceStringID);
	ControlGui::instance->setChatText("", resourceText.c_str(), 0xf0000000, 0);
	return true;
}

bool CSetBooleanFlag::Read( FitIniFile* missionFile )
{
	std::optional<std::string> flagID = objective_text::readFlagID(*missionFile, "FlagID", "FlagIndex");
	if (!flagID) { return false; }
	m_flagID = std::move(*flagID);
	sReadIdBoolean(missionFile, "Value", m_value);
	if (objectiveObservationEnabled()) {
		observationPayload = std::format("{{\"FlagID\":{},\"Value\":{}}}", objectiveObservationQuote(m_flagID), m_value);
	}
	return true;
}

int CSetBooleanFlag::Execute()
{
	Team::teams[Alignment()]->objectives.boolFlags.setElementValue(m_flagID, m_value);
	objectiveObservationSink("flag", "written", m_value ? "true" : "false", m_flagID.c_str());
	return true;
}

bool CMakeNewTechnologyAvailable::Read( FitIniFile* missionFile )
{
	std::optional<std::string> fileName = objective_text::read(*missionFile, "PurchaseFileName");
	if (!fileName) { return false; }
	m_purchaseFilePathname = std::move(*fileName);
	if (objectiveObservationEnabled()) { observationPayload = objectiveObservationQuote(m_purchaseFilePathname); }
	return true;
}

int CMakeNewTechnologyAvailable::Execute()
{
	LogisticsData::instance->addNewBonusPurchaseFile( m_purchaseFilePathname.c_str() );
	return true;
}

bool C_RemoveStructure::SetParams(float positionX, float positionY) {

	BuildingPtr pBuilding = nullptr;
	if (ObjectManager) 
	{
		pBuilding = ObjectManager->getBuilding(0);
		long i;
		for (i = 0; (nullptr != pBuilding); i+= 1) {
			pBuilding = ObjectManager->getBuilding(i);
			if ((nullptr != pBuilding) && (pBuilding->getPosition().x == positionX) && (pBuilding->getPosition().y == positionY)) {
				break;
			}
		}
		if (!pBuilding) { assert(false); return false; }
	}
	if (pBuilding)
		m_pBuildingWID = pBuilding->getWatchID();
	else
		m_pBuildingWID = 0;

	return true;
}

bool C_RemoveStructure::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdLong("BuildingWID",m_pBuildingWID);
	if (result == NO_ERR && objectiveObservationEnabled()) {
		observationPayload = std::format("{{\"BuildingWID\":{}}}", m_pBuildingWID);
	}
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		if (objectiveObservationEnabled()) {
			observationPayload = std::format("{{\"PositionX\":{},\"PositionY\":{}}}", static_cast<double>(positionX), static_cast<double>(positionY));
		}
		BuildingPtr pBuilding = nullptr;
		if (ObjectManager) {
			pBuilding = ObjectManager->getBuilding(0);
			long i;
			for (i = 0; (nullptr != pBuilding); i+= 1) {
				pBuilding = ObjectManager->getBuilding(i);
				if ((nullptr != pBuilding) && (pBuilding->getPosition().x == positionX) && (pBuilding->getPosition().y == positionY)) {
					break;
				}
			}
			if (!pBuilding) { assert(false); return false; }
		}
		if (pBuilding)
			m_pBuildingWID = pBuilding->getWatchID();
		else
			m_pBuildingWID = 0;
	}

	return true;
}

std::unique_ptr<CObjectiveCondition> CObjective::new_CObjectiveCondition(condition_species_type conditionSpecies, int alignment) {
	std::unique_ptr<CObjectiveCondition> retval;
	switch (conditionSpecies) {
	case DESTROY_ALL_ENEMY_UNITS: retval = std::make_unique<CDestroyAllEnemyUnits>(alignment); break;
	case DESTROY_NUMBER_OF_ENEMY_UNITS: retval = std::make_unique<CDestroyNumberOfEnemyUnits>(alignment); break;
	case DESTROY_SPECIFIC_ENEMY_UNIT: retval = std::make_unique<CDestroySpecificEnemyUnit>(alignment); break;
	case DESTROY_SPECIFIC_STRUCTURE: retval = std::make_unique<CDestroySpecificStructure>(alignment); break;
	case CAPTURE_OR_DESTROY_ALL_ENEMY_UNITS: retval = std::make_unique<CCaptureOrDestroyAllEnemyUnits>(alignment); break;
	case CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS: retval = std::make_unique<CCaptureOrDestroyNumberOfEnemyUnits>(alignment); break;
	case CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT: retval = std::make_unique<CCaptureOrDestroySpecificEnemyUnit>(alignment); break;
	case CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE: retval = std::make_unique<CCaptureOrDestroySpecificStructure>(alignment); break;
	case DEAD_OR_FLED_ALL_ENEMY_UNITS: retval = std::make_unique<CDeadOrFledAllEnemyUnits>(alignment); break;
	case DEAD_OR_FLED_NUMBER_OF_ENEMY_UNITS: retval = std::make_unique<CDeadOrFledNumberOfEnemyUnits>(alignment); break;
	case DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT: retval = std::make_unique<CDeadOrFledSpecificEnemyUnit>(alignment); break;
	case CAPTURE_UNIT: retval = std::make_unique<CCaptureUnit>(alignment); break;
	case CAPTURE_STRUCTURE: retval = std::make_unique<CCaptureStructure>(alignment); break;
	case GUARD_SPECIFIC_UNIT: retval = std::make_unique<CGuardSpecificUnit>(alignment); break;
	case GUARD_SPECIFIC_STRUCTURE: retval = std::make_unique<CGuardSpecificStructure>(alignment); break;
	case MOVE_ANY_UNIT_TO_AREA: retval = std::make_unique<CMoveAnyUnitToArea>(alignment); break;
	case MOVE_ALL_UNITS_TO_AREA: retval = std::make_unique<CMoveAllUnitsToArea>(alignment); break;
	case MOVE_ALL_SURVIVING_UNITS_TO_AREA: retval = std::make_unique<CMoveAllSurvivingUnitsToArea>(alignment); break;
	case MOVE_ALL_SURVIVING_MECHS_TO_AREA: retval = std::make_unique<CMoveAllSurvivingMechsToArea>(alignment); break;
	case BOOLEAN_FLAG_IS_SET: retval = std::make_unique<CBooleanFlagIsSet>(alignment); break;
	case ELAPSED_MISSION_TIME: retval = std::make_unique<CElapsedMissionTime>(alignment); break;
	default:
		// CRASH-HARDEN-OBJECTIVE-ICON-1: a bad/mod .fit can map to a condition
		// species with no factory case (e.g. *_ENEMY_UNIT_GROUP). assert() is a
		// no-op in RelWithDebInfo, so this silently returned null -> null-deref at
		// the CastAndCopy consumers. Warn loudly; consumers now null-guard.
		PAUSE(("CObjective: unimplemented condition species %d (skipping)", (int)conditionSpecies));
		retval = nullptr;
		break;
	}
	return retval;
}

std::unique_ptr<CObjectiveAction> CObjective::new_CObjectiveAction(action_species_type actionSpecies, int alignment) {
	std::unique_ptr<CObjectiveAction> retval;
	switch (actionSpecies) {
	case PLAY_BIK: retval = std::make_unique<CPlayBIK>(alignment); break;
	case PLAY_WAV: retval = std::make_unique<CPlayWAV>(alignment); break;
	case DISPLAY_TEXT_MESSAGE: retval = std::make_unique<CDisplayTextMessage>(alignment); break;
	case DISPLAY_RESOURCE_TEXT_MESSAGE: retval = std::make_unique<CDisplayResourceTextMessage>(alignment); break;
	case SET_BOOLEAN_FLAG: retval = std::make_unique<CSetBooleanFlag>(alignment); break;
	case MAKE_NEW_TECHNOLOGY_AVAILABLE: retval = std::make_unique<CMakeNewTechnologyAvailable>(alignment); break;

	//ONLY used for in-mission saveLoad to avoid calling ReadNavMarkers
	case _REMOVE_STRUCTURE: retval = std::make_unique<C_RemoveStructure>(alignment); break;
	default:
		assert(false);
		retval = nullptr;
		break;
	}
	return retval;
}

CObjective::CObjective(int alignment) {
	m_alignment = alignment;
	m_titleUseResourceString = false;
	m_titleResourceStringID = 0;
	m_descriptionUseResourceString = false;
	m_descriptionResourceStringID = 0;
	m_priority = 1;
	m_resourcePoints = 0;
	m_previousPrimaryObjectiveMustBeComplete = false;
	m_allPreviousPrimaryObjectivesMustBeComplete = false;
	m_displayMarker = false;
	m_markerX = 0.0;
	m_markerY = 0.0;
	m_isHiddenTrigger = false;
	m_isActive = true;
	m_activateOnFlag = false;
	m_activationTime = -1.0;
	m_resetStatusOnFlag = false;
	m_resolved = false;
	m_changedStatus = false;
	m_resolvedStatus = OS_UNDETERMINED;
	m_modelScale = 1.0f;
}

CObjective &CObjective::operator=(const CObjective &master) {
	Clear();
	m_fitOrdinal = master.m_fitOrdinal;
	m_observationState = 0;
	m_alignment = master.m_alignment;
	m_text = master.m_text;
	m_titleUseResourceString = master.m_titleUseResourceString;
	m_titleResourceStringID = master.m_titleResourceStringID;
	m_descriptionUseResourceString = master.m_descriptionUseResourceString;
	m_descriptionResourceStringID = master.m_descriptionResourceStringID;
	m_priority = master.m_priority;
	m_resourcePoints = master.m_resourcePoints;
	m_previousPrimaryObjectiveMustBeComplete = master.m_previousPrimaryObjectiveMustBeComplete;
	m_allPreviousPrimaryObjectivesMustBeComplete = master.m_allPreviousPrimaryObjectivesMustBeComplete;
	m_displayMarker = master.m_displayMarker;
	m_markerX = master.m_markerX;
	m_markerY = master.m_markerY;
	m_isHiddenTrigger = master.m_isHiddenTrigger;
	m_isActive = master.m_isActive;
	m_activateOnFlag = master.m_activateOnFlag;
	m_activationTime = master.m_activationTime;
	m_resetStatusOnFlag = master.m_resetStatusOnFlag;
	m_modelType = master.m_modelType;
	m_modelBaseColor = master.m_modelBaseColor;
	m_modelHighlightColor = master.m_modelHighlightColor;
	m_modelHighlightColor2 = master.m_modelHighlightColor2;
	m_modelScale = master.m_modelScale;
	m_resolved = master.m_resolved;
	m_changedStatus = master.m_changedStatus;
	if (m_resolved) {
		m_resolvedStatus = master.m_resolvedStatus;
	}

	{
		for (auto* const pItem : master.m_conditionList) {
			condition_species_type conditionSpecies = pItem->Species();
			auto pTmpCondition = new_CObjectiveCondition(conditionSpecies, m_alignment);
			// Unimplemented species have no runtime instance to copy.
			if (pTmpCondition) {
				(*pTmpCondition).CastAndCopy(pItem);
				m_conditionList.Append(std::move(pTmpCondition));
			}
		}
	}

	{
		for (auto* const pItem : master.m_actionList) {
			action_species_type actionSpecies = pItem->Species();
			auto pTmpAction = new_CObjectiveAction(actionSpecies, m_alignment);
			// An action species the factory does not implement yields null: skip it, as the condition copies do.
			if (pTmpAction) {
				(*pTmpAction).CastAndCopy(pItem);
				m_actionList.Append(std::move(pTmpAction));
			}
		}
	}

	{
		for (auto* const pItem : master.m_failureConditionList) {
			condition_species_type conditionSpecies = pItem->Species();
			auto pTmpCondition = new_CObjectiveCondition(conditionSpecies, m_alignment);
			// Unimplemented species have no runtime instance to copy.
			if (pTmpCondition) {
				(*pTmpCondition).CastAndCopy(pItem);
				m_failureConditionList.Append(std::move(pTmpCondition));
			}
		}
	}

	{
		for (auto* const pItem : master.m_failureActionList) {
			action_species_type actionSpecies = pItem->Species();
			auto pTmpAction = new_CObjectiveAction(actionSpecies, m_alignment);
			// An action species the factory does not implement yields null: skip it, as the condition copies do.
			if (pTmpAction) {
				(*pTmpAction).CastAndCopy(pItem);
				m_failureActionList.Append(std::move(pTmpAction));
			}
		}
	}

	return (*this);
}

void CObjective::Clear() {
	m_resolved = false;
	m_changedStatus = false;

	// The lists own their elements and free them head to tail, in the order the parts were declared.
	m_conditionList.Clear();
	m_actionList.Clear();
	m_failureConditionList.Clear();
	m_failureActionList.Clear();
}

void CObjective::Alignment(int alignment) {
	m_alignment = alignment;
	{
		for (auto* const pItem : m_conditionList) {
			pItem->Alignment(m_alignment);
		}
	}

	{
		for (auto* const pItem : m_actionList) {
			pItem->Alignment(m_alignment);
		}
	}

	{
		for (auto* const pItem : m_failureConditionList) {
			pItem->Alignment(m_alignment);
		}
	}

	{
		for (auto* const pItem : m_failureActionList) {
			pItem->Alignment(m_alignment);
		}
	}
}

static condition_species_type ConditionSpeciesMap(const char *speciesString) {
	condition_species_type retval = DESTROY_ALL_ENEMY_UNITS;
	int i;
	for (i = 0; i < (int)NUM_CONDITION_SPECIES; i += 1) {
		if (0 == strcmp(speciesString, g_conditionSpeciesStringArray[i])) {
			retval = (condition_species_type)i;
			break;
		}
	}
	assert(i < (int)NUM_CONDITION_SPECIES);
	return retval;
}

static action_species_type ActionSpeciesMap(const char *speciesString) {
	action_species_type retval = PLAY_BIK;
	int i;
	for (i = 0; i < (int)NUM_ACTION_SPECIES; i += 1) {
		if (0 == strcmp(speciesString, g_actionSpeciesStringArray[i])) {
			retval = (action_species_type)i;
			break;
		}
	}
	assert(i < (int)NUM_ACTION_SPECIES);
	return retval;
}

bool CObjective::Read( FitIniFile* missionFile, int objectiveNum, int version, int MarkerNum, char secondary )
{
	m_fitOrdinal = objectiveNum;

	if ( !s_markerFont )
	{
		s_markerFont = std::make_unique_for_overwrite<aFont>();
		s_markerFont->init( IDS_OBJECTIVE_MARKER_FONT );
	}

	long result = 0;
	objective_text::readFields(*missionFile, m_text);
	result = sReadIdBoolean(missionFile, "TitleUseResourceString", m_titleUseResourceString);
	result = sReadIdWholeNum(missionFile, "TitleResourceStringID", m_titleResourceStringID);
	result = sReadIdBoolean(missionFile, "DescriptionUseResourceString", m_descriptionUseResourceString);
	result = sReadIdWholeNum(missionFile, "DescriptionResourceStringID", m_descriptionResourceStringID);

	result = sReadIdWholeNum(missionFile, "Priority", m_priority);
	result = sReadIdWholeNum(missionFile, "ResourcePoints", m_resourcePoints);
	result = sReadIdBoolean(missionFile, "PreviousPrimaryObjectiveMustBeComplete", m_previousPrimaryObjectiveMustBeComplete);
	if (NO_ERR != result) {
		result = sReadIdBoolean(missionFile, "PreviousObjectiveMustBeComplete", m_previousPrimaryObjectiveMustBeComplete);
	}
	result = sReadIdBoolean(missionFile, "AllPreviousPrimaryObjectivesMustBeComplete", m_allPreviousPrimaryObjectivesMustBeComplete);

	m_markerText[0] = 0;

	bool tmp;
	result = sReadIdBoolean(missionFile, "DisplayMarker", tmp);
	if ( tmp )
	{
		m_displayMarker = NUMERIC;
		if ( m_priority == 1)
			mc2::fmt::to_buf(m_markerText, "{:d}", mc2::fmt::as_i32(MarkerNum));
		else
			mc2::fmt::to_buf(m_markerText, "{:c}", mc2::fmt::as_char(secondary));
	}
	result = sReadIdFloat(missionFile, "MarkerX", m_markerX);
	result = sReadIdFloat(missionFile, "MarkerY", m_markerY);
	result = sReadIdBoolean(missionFile, "HiddenTrigger", m_isHiddenTrigger);
	result = sReadIdBoolean(missionFile, "ActivateOnFlag", m_activateOnFlag);
	if (m_activateOnFlag) { m_isActive = false; }
	result = sReadIdBoolean(missionFile, "ResetStatusOnFlag", m_resetStatusOnFlag);
	result = sReadIdLongInt(missionFile, "ModelType", m_modelType);
	result = sReadIdLongInt(missionFile, "BaseColor", m_modelBaseColor);
	result = sReadIdLongInt(missionFile, "HighlightColor", m_modelHighlightColor);
	result = sReadIdLongInt(missionFile, "HighlightColor2", m_modelHighlightColor2);
	result = sReadIdFloat(missionFile, "ModelScale", m_modelScale);

	{
		/*these items should only be present in "in-mission" saved games*/
		/*state data*/
		result = sReadIdBoolean(missionFile, "IsActive", m_isActive);
		float tempFloat;
		result = sReadIdFloat(missionFile, "ActivationTime", tempFloat);
		if (NO_ERR == result) {
			m_activationTime = tempFloat;
		}
		result = sReadIdBoolean(missionFile, "IsResolved", m_resolved);
		int tempInt;
		result = sReadIdLongInt(missionFile, "ResolvedStatus", tempInt);
		if (NO_ERR == result) {
			m_resolvedStatus = (objective_status_type)tempInt;
		}
		result = sReadIdBoolean(missionFile, "ChangedStatus", m_changedStatus);
		/*end state data*/
	}

	int numConditions = 0;
	result = sReadIdWholeNum(missionFile, "NumConditions", numConditions);
	if (NO_ERR != result) { return false; }
	int numActions = 0;
	result = sReadIdWholeNum(missionFile, "NumActions", numActions);
	int numFailureConditions = 0;
	result = sReadIdWholeNum(missionFile, "NumFailureConditions", numFailureConditions);
	int numFailureActions = 0;
	result = sReadIdWholeNum(missionFile, "NumFailureActions", numFailureActions);

	{
		int i;
		for (i = 0; i < numConditions; i += 1) {
			const std::string blockName = (2 == version) ? std::format("Objective{}Condition{}", objectiveNum, i)
			                                             : std::format("Team{}Objective{}Condition{}", Alignment(), objectiveNum, i);
			result = missionFile->seekBlock(blockName.c_str());
			if (NO_ERR != result) { assert(false); continue; }
			condition_species_type species;
			std::optional<std::string> speciesText = objective_text::read(*missionFile, "ConditionSpeciesString");
			if (!speciesText) {
				/* this is just for backward compatibility, this entry is depricated */
				int ispecies = 0;
				result = sReadIdWholeNum(missionFile, "ConditionSpecies", ispecies);
				if (NO_ERR != result) { continue; }
				species = (condition_species_type)ispecies;
			} else {
				species = ConditionSpeciesMap(speciesText->c_str());
			}

			auto pNewObjectiveCondition = new_CObjectiveCondition(species, m_alignment);
			if (!pNewObjectiveCondition) { assert(false); pNewObjectiveCondition.reset(); continue; }
			bool bresult = pNewObjectiveCondition->Read(missionFile);
			if (true != bresult) { assert(false); pNewObjectiveCondition.reset(); continue; }
			m_conditionList.Append(std::move(pNewObjectiveCondition));
		}
	}

	{
		int i;
		for (i = 0; i < numActions; i += 1) {
			result = missionFile->seekBlock(std::format("Team{}Objective{}Action{}", Alignment(), objectiveNum, i).c_str());
			if (NO_ERR != result) { assert(false); continue; }
			action_species_type species;
			std::optional<std::string> speciesText = objective_text::read(*missionFile, "ActionSpeciesString");
			if (!speciesText) { assert(false); continue; }
			species = ActionSpeciesMap(speciesText->c_str());

			auto pNewObjectiveAction = new_CObjectiveAction(species, m_alignment);
			if (!pNewObjectiveAction) { assert(false); pNewObjectiveAction.reset(); continue; }
			bool bresult = pNewObjectiveAction->Read(missionFile);
			if (true != bresult) { assert(false); pNewObjectiveAction.reset(); continue; }
			pNewObjectiveAction->observationOrdinal = i;
			m_actionList.Append(std::move(pNewObjectiveAction));
		}
	}

	{
		int i;
		for (i = 0; i < numFailureConditions; i += 1) {
			result = missionFile->seekBlock(std::format("Team{}Objective{}FailureCondition{}", Alignment(), objectiveNum, i).c_str());
			if (NO_ERR != result) { assert(false); continue; }
			condition_species_type species;
			std::optional<std::string> speciesText = objective_text::read(*missionFile, "FailureConditionSpeciesString");
			if (!speciesText) { assert(false); continue; }
			species = ConditionSpeciesMap(speciesText->c_str());

			auto pNewObjectiveFailureCondition = new_CObjectiveCondition(species, m_alignment);
			if (!pNewObjectiveFailureCondition) { assert(false); pNewObjectiveFailureCondition.reset(); continue; }
			bool bresult = pNewObjectiveFailureCondition->Read(missionFile);
			if (true != bresult) { assert(false); pNewObjectiveFailureCondition.reset(); continue; }
			m_failureConditionList.Append(std::move(pNewObjectiveFailureCondition));
		}
	}

	{
		int i;
		for (i = 0; i < numFailureActions; i += 1) {
			result = missionFile->seekBlock(std::format("Team{}Objective{}FailureAction{}", Alignment(), objectiveNum, i).c_str());
			if (NO_ERR != result) { assert(false); continue; }
			action_species_type species;
			std::optional<std::string> speciesText = objective_text::read(*missionFile, "FailureActionSpeciesString");
			if (!speciesText) { assert(false); continue; }
			species = ActionSpeciesMap(speciesText->c_str());

			auto pNewObjectiveFailureAction = new_CObjectiveAction(species, m_alignment);
			if (!pNewObjectiveFailureAction) { assert(false); pNewObjectiveFailureAction.reset(); continue; }
			bool bresult = pNewObjectiveFailureAction->Read(missionFile);
			if (true != bresult) { assert(false); pNewObjectiveFailureAction.reset(); continue; }
			pNewObjectiveFailureAction->observationOrdinal = i;
			m_failureActionList.Append(std::move(pNewObjectiveFailureAction));
		}
	}
	return true;
}

objective_status_type CObjective::Status() {
	ObservationTraversal traversal("direct");
	if (!IsActive()) {
		return OS_UNDETERMINED;
	}
	if (m_resolved) {
		return m_resolvedStatus;
	} else { 
		objective_status_type objectiveStatus = OS_SUCCESSFUL;
		{
			for (auto* const pItem : m_conditionList) {
				objective_status_type conditionStatus = pItem->Status();
				if (OS_FAILED == conditionStatus) {
					objectiveStatus = OS_FAILED;
					break;
				} else if (OS_UNDETERMINED == conditionStatus) {
					objectiveStatus = OS_UNDETERMINED;
				}
			}
		}
		if ((OS_FAILED != objectiveStatus) && (0 < m_failureConditionList.Count())) {
			objective_status_type failureStatus = OS_SUCCESSFUL;
			for (auto* const pItem : m_failureConditionList) {
				objective_status_type conditionStatus = pItem->Status();
				if (OS_FAILED == conditionStatus) {
					failureStatus = OS_FAILED;
					break;
				} else if (OS_UNDETERMINED == conditionStatus) {
					failureStatus = OS_UNDETERMINED;
					break;
				}
			}
			if (OS_SUCCESSFUL == failureStatus) {
				objectiveStatus = OS_FAILED;
			}
		}
		if (OS_UNDETERMINED == objectiveStatus) {
			return OS_UNDETERMINED;
		} else if (OS_FAILED == objectiveStatus) {
			m_resolved = true;
			m_changedStatus = true;
			m_resolvedStatus = OS_FAILED;
			ObserveState("resolve", OS_UNDETERMINED, m_resolvedStatus);
			if (!IsHiddenTrigger()) {
				soundSystem->playBettySample(BETTY_CANNOT_COMP_OBJ);
			}
			for (auto* const pItem : m_failureActionList) {
				Dispatch(*pItem, "failure");
			}
			return m_resolvedStatus;
		} else {
			m_resolved = true;
			m_changedStatus = true;
			m_resolvedStatus = OS_SUCCESSFUL;
			ObserveState("resolve", OS_UNDETERMINED, m_resolvedStatus);
			if (!IsHiddenTrigger()) {
				soundSystem->playBettySample(BETTY_OBJECTIVE_COMPLETE);
			}
			for (auto* const pItem : m_actionList) {
				Dispatch(*pItem, "success");
			}
			return m_resolvedStatus;
		}
	}
}

objective_status_type CObjective::Status(CObjectives &objectives) {
	ObservationTraversal traversal("context");
	if (!IsActive()) {
		return OS_UNDETERMINED;
	}

	if ((!PreviousPrimaryObjectiveMustBeComplete()) && !(AllPreviousPrimaryObjectivesMustBeComplete())) {
		return Status();
	}

	objective_status_type retval = OS_SUCCESSFUL;
	objective_status_type statusOfPreviousPrimaryObjective = OS_SUCCESSFUL;
	bool allPreviousPrimaryObjectivesHaveBeenCompleted = true;
	for (auto* const pItem : objectives) {
		objective_status_type status;
		if ((pItem->AllPreviousPrimaryObjectivesMustBeComplete()) && (!allPreviousPrimaryObjectivesHaveBeenCompleted)) {
			status = OS_UNDETERMINED;
		} else if ((pItem->PreviousPrimaryObjectiveMustBeComplete()) && (OS_SUCCESSFUL != statusOfPreviousPrimaryObjective)) {
			assert(!pItem->AllPreviousPrimaryObjectivesMustBeComplete());
			assert(OS_UNDETERMINED == statusOfPreviousPrimaryObjective);
			status = statusOfPreviousPrimaryObjective;
		} else {
			status = pItem->Status();
		}
		if (this == pItem) {
			return status;
		}
		assert (OS_FAILED != retval);
		if (1 == pItem->Priority()) {
			if (OS_FAILED ==  status) {
				allPreviousPrimaryObjectivesHaveBeenCompleted = false;
				retval = OS_FAILED;
				/* if one primary objective is failed, it's over */
				break;
			} else if (OS_UNDETERMINED == status) {
				allPreviousPrimaryObjectivesHaveBeenCompleted = false;
				retval = OS_UNDETERMINED;
			}
			statusOfPreviousPrimaryObjective = status;
		}
	}
	return retval;

}

void CObjective::Status(objective_status_type newStatus) {
	const auto oldStatus = m_resolved ? m_resolvedStatus : OS_UNDETERMINED;
	if (OS_UNDETERMINED == newStatus) {
		m_resolved = false;
	} else {
		m_resolved = true;
	}
	m_resolvedStatus = newStatus;
	m_changedStatus = true;
	ObserveState(newStatus == OS_UNDETERMINED ? "reset" : "resolve", oldStatus, newStatus);
}

bool CObjective::StatusChangedSuccess (void)
{
	if (!IsActive())
	{
		return false;
	}
	
	if (m_changedStatus)
	{
		m_changedStatus = false;
        //sebi:
		//m_resolvedStatus;
		if (m_resolvedStatus == OS_SUCCESSFUL)
			return true;
	}
	
	return false;
}
									  
bool CObjective::StatusChangedFailed (void)
{
	if (!IsActive())
	{
		return false;
	}
	
	if (m_changedStatus)
	{
		m_changedStatus = false;
		if (m_resolvedStatus == OS_FAILED)
			return true;
	}
	
	return false;
}

std::string CObjective::LocalizedTitle() const {
	return TitleUseResourceString() ? loadResourceString(TitleResourceStringID()) : Title();
}

std::string CObjective::LocalizedDescription() const {
	return DescriptionUseResourceString() ? loadResourceString(DescriptionResourceStringID()) : Description();
}

CObjectives &CObjectives::operator=(const CObjectives &master) {
	m_alignment = master.m_alignment;

	Clear();
	for (auto* const pItem : master) {
		//CObjective *tmpObjective = new CObjective(*pItem);
		auto tmpObjective = std::make_unique<CObjective>(m_alignment);
		(*tmpObjective) = (*pItem);
		m_list.Append(std::move(tmpObjective));
	}
	return (*this);
}

void CObjectives::Clear() {
	observation.Finish();
	m_list.Clear();
	boolFlags.Clear();
	observation = Observation{};
}

void CObjectives::Alignment(int alignment) {
	m_alignment = alignment;
	for (auto* const pItem : *this) {
		pItem->Alignment(m_alignment);
	}
}

bool CObjectives::Read( FitIniFile* missionFile )
{
	if (Alignment() == 0) { observation.Begin(*missionFile); }
	long result = 0;
	result = missionFile->seekBlock("Objectives Version");
	if (NO_ERR != result) { assert(false); }
	int objectivesVersion = 0;
	result = sReadIdWholeNum(missionFile, "Version", objectivesVersion);
	if (result != NO_ERR) 
	{
		assert(false); return false;
	} 
	else 
	{
		if (2 == objectivesVersion) 
		{
			if (0 != Alignment()) 
			{
				// Version 2 stores objectives for team 0 only.
				return true;
			}
			result = missionFile->seekBlock("Objectives");
		} 
		else 
		{
			result = missionFile->seekBlock(std::format("Team{}Objectives", Alignment()).c_str());
		}

		if (NO_ERR != result) 
		{ 
			assert(false); 
		}

		int numObjectives = 0;
		result = sReadIdWholeNum(missionFile, "NumObjectives", numObjectives);
		if (NO_ERR != result) 
		{ 
			return false; 
		}

		boolFlags.load(Alignment(),missionFile);

		int i;
		int markerI = 1;
		char secondaryMarkers = 'a';
		for (i = 0; i < numObjectives; i += 1) {
			const std::string blockName = (2 == objectivesVersion) ? std::format("Objective{}", i)
			                                                       : std::format("Team{}Objective{}", Alignment(), i);
			result = missionFile->seekBlock(blockName.c_str());
			if (NO_ERR != result) { assert(false); continue; }
			auto pNewObjective = std::make_unique<CObjective>(m_alignment);
			bool bresult = pNewObjective->Read(missionFile, i, objectivesVersion, markerI, secondaryMarkers);
			
			if ( 1 == pNewObjective->Priority() )
				markerI++;
			else
				secondaryMarkers++;
			
			if (true != bresult) { assert(false); pNewObjective.reset(); continue; }
			m_list.Append(std::move(pNewObjective));
		}
	}
	return true;
}

objective_status_type CObjectives::Status() {
	ObservationTraversal traversal("team");
	objective_status_type retval = OS_SUCCESSFUL;
	objective_status_type statusOfPreviousPrimaryObjective = OS_SUCCESSFUL;
	bool allPreviousPrimaryObjectivesHaveBeenCompleted = true;
	for (auto* const pItem : *this) {
		if (pItem->IsActive()) {
			if (pItem->ResetStatusOnFlag()) {
				if (Team::teams[Alignment()]->objectives.boolFlags.getElementValue(pItem->ResetStatusFlagID())) {
					Team::teams[Alignment()]->objectives.boolFlags.setElementValue(pItem->ResetStatusFlagID(), false);
					pItem->Status(OS_UNDETERMINED);
				}
			}
			objective_status_type status;
			if ((pItem->AllPreviousPrimaryObjectivesMustBeComplete()) && (!allPreviousPrimaryObjectivesHaveBeenCompleted)) {
				status = OS_UNDETERMINED;
			} else if ((pItem->PreviousPrimaryObjectiveMustBeComplete()) && (OS_SUCCESSFUL != statusOfPreviousPrimaryObjective)) {
				assert(!pItem->AllPreviousPrimaryObjectivesMustBeComplete());
				assert(OS_UNDETERMINED == statusOfPreviousPrimaryObjective);
				status = statusOfPreviousPrimaryObjective;
			} else {
				status = pItem->Status();
			}
			assert (OS_FAILED != retval);
			if (1 == pItem->Priority()) {
				if (OS_FAILED ==  status) {
					allPreviousPrimaryObjectivesHaveBeenCompleted = false;
					retval = OS_FAILED;
					/* if one primary objective is failed, it's over */
					break;
				} else if (OS_UNDETERMINED == status) {
					allPreviousPrimaryObjectivesHaveBeenCompleted = false;
					retval = OS_UNDETERMINED;
				}
				statusOfPreviousPrimaryObjective = status;
			}
		} else {
			if (pItem->ActivateOnFlag()) {
				if (1 == pItem->Priority()) {
					/* do not end the mission if a primary objective is still inactive */
					retval = OS_UNDETERMINED;
				}
				if (Team::teams[Alignment()]->objectives.boolFlags.getElementValue(pItem->ActivateFlagID())) {
					pItem->IsActive(true);
					pItem->ActivationTime(mission->actualTime);
					mission->missionInterface->showObjectives(true);
				}
			}
		}
	}

	return retval;
}

/*void CObjective::Render( unsigned long xPos, unsigned long yPos, HGOSFONT3D guiFont )
{
	unsigned long fontWidth, fontHeight;
	gos_TextStringLength( &fontWidth, &fontHeight, "ABC" );
	
	// draw little box first
	GUI_RECT rect = { xPos, yPos, xPos + fontHeight, yPos + fontHeight };
	drawEmptyRect( rect, 0xffffffff, 0xffffffff );

	unsigned long color = 0xffffffff;

	if ( m_resolved )
	{
		color = m_resolvedStatus == OS_FAILED ?  0xffff0000 : 0xff00ff00;
		gos_TextSetAttributes( guiFont, color, 1.0, false, true, false, false, 0 );	
		gos_TextSetPosition(xPos + (fontWidth >> 1), yPos );

		if ( m_resolvedStatus == OS_FAILED )
			gos_TextDraw( "X" );
		else if ( m_resolvedStatus == OS_SUCCESSFUL )
		{
			gos_TextDraw( "V" );
		}	
	}
	
	gos_TextSetAttributes( guiFont, color, 1.0, false, true, false, false, 0 );	
	gos_TextSetPosition(xPos + 2 * fontWidth, yPos );

	gos_TextDraw( (const char*)m_description );

}*/

bool CObjective::RenderMarkers(GameTacMap *tacMap, bool blink ) 
{
	bool bRetVal = 0;
	if (m_resolved || !m_displayMarker || !m_isActive)
	{
		//Draw Nothing.  Objective is resolved or hidden
	} 
	else 
	{
			glm::vec3 objectivePos;
			objectivePos.x = m_markerX;
			objectivePos.y = m_markerY;
			long color = SD_YELLOW;

			if ( m_displayMarker == NAV )
			{
				bRetVal = 1; // true if we drew something

				if ( blink )
				{
					s_lastBlinkTime += frameLength;
					if ( s_lastBlinkTime > s_blinkLength )
					{
						s_lastBlinkTime = 0.f;
						if ( !s_blinkColor )
							s_blinkColor = SD_YELLOW;
						else
							s_blinkColor = 0;
					}
					color = s_blinkColor;

				}

			}
			else
			{
				color = 0xffff0000;
				gos_VERTEX pos;
				tacMap->worldToTacMap(objectivePos, pos);

				unsigned long width, height;
				height = s_markerFont->height();
				width = s_markerFont->width( m_markerText );
				drawShadowText( 0xffffffff, 0xff000000, s_markerFont->getTempHandle(), pos.x - width/2, pos.y - height/2, true, m_markerText, 0, s_markerFont->getSize(), -2, 2 );

			}

	}

	return bRetVal;
}

void CObjective::RenderOverviewMarker( Camera* eye, float alpha )
{
	if ( !eye || alpha <= 0.0f )
		return;
	if ( m_resolved || !m_displayMarker || !m_isActive )
		return;
	if ( !m_markerText[0] || !s_markerFont )
		return;

	// World position of the marker, lifted to the terrain surface so it projects
	// onto the ground at altitude (objectives are authored at z=0).
	glm::vec3 wp;
	wp.x = m_markerX;
	wp.y = m_markerY;
	wp.z = 0.0f;
	if ( land )
		wp.z = land->getTerrainElevation( wp );

	// Project through the GL clip matrix the scene renders with (matches the unit
	// icon overlay; avoids the legacy projectZ split-brain lag under motion).
	ModernClipResult r = eye->projectModernClipGL( wp );
	if ( !r.admit || r.clip.w <= 0.05f )
		return;
	float vmx, vmy, vax, vay;
	gos_GetViewport( &vmx, &vmy, &vax, &vay );
	float sx = vax + ( r.clip.x / r.clip.w * 0.5f + 0.5f ) * vmx;
	float sy = vay + ( 1.0f - ( r.clip.y / r.clip.w * 0.5f + 0.5f ) ) * vmy;

	unsigned long aByte = (unsigned long)( alpha * 255.0f );
	if ( aByte > 255 ) aByte = 255;
	const unsigned long fg = ( aByte << 24 ) | 0x00ffe600; // warm yellow, cross-faded
	const unsigned long bg = ( aByte << 24 ) | 0x00000000; // black shadow

	unsigned long width  = s_markerFont->width( m_markerText );
	unsigned long height = s_markerFont->height();

	// Exempt from the bottom-band HUD shrink (world-anchored, like the icons).
	bool prevExempt = gos_GetHudScaleExempt();
	gos_SetHudScaleExempt( true );
	drawShadowText( fg, bg, s_markerFont->getTempHandle(),
	                (long)( sx - width / 2 ), (long)( sy - height / 2 ),
	                true, m_markerText, 0, s_markerFont->getSize(), -2, 2 );
	gos_SetHudScaleExempt( prevExempt );
}


/* Reads Nav Marker info and adds appropriate "hidden trigger" objectives. */
bool ReadNavMarkers( FitIniFile* missionFile, CObjectives &objectives )
{
	long result = 0;
	result = missionFile->seekBlock("NavMarkers");
	if (NO_ERR != result) { /*assert(false);*/ return false; }
	int numNavMarkers = 0;
	result = sReadIdWholeNum(missionFile, "NumNavMarkers", numNavMarkers);
	if (NO_ERR != result) { return false; }
	int i;
	for (i = 0; i < numNavMarkers; i += 1) {
		result = missionFile->seekBlock(std::format("NavMarker{}", i).c_str());
		if (NO_ERR != result) { assert(false); continue; }
		auto pNewObjective = std::make_unique<CObjective>(objectives.Alignment());

		pNewObjective->Title(std::format("Nav Marker {}", i));
		pNewObjective->Priority(2);
		pNewObjective->IsHiddenTrigger(true);
		pNewObjective->DisplayMarker( 0 );

		float targetCenterX, targetCenterY, targetRadius;
		result = sReadIdFloat(missionFile, "xPos", targetCenterX);
		if (NO_ERR != result) { assert(false); pNewObjective.reset(); continue; }
		result = sReadIdFloat(missionFile, "yPos", targetCenterY);
		if (NO_ERR != result) { assert(false); pNewObjective.reset(); continue; }
		result = sReadIdFloat(missionFile, "radius", targetRadius);
		if (NO_ERR != result) { assert(false); pNewObjective.reset(); continue; }
		pNewObjective->MarkerX( targetCenterX );
		pNewObjective->MarkerY( targetCenterY );

		auto pCondition = std::make_unique<CMoveAnyUnitToArea>(pNewObjective->Alignment());
		pCondition->SetParams(targetCenterX, targetCenterY, targetRadius);
		pNewObjective->m_conditionList.Append(std::move(pCondition));

		auto pAction = std::make_unique<C_RemoveStructure>(pNewObjective->Alignment());
		bool bresult = pAction->SetParams(targetCenterX, targetCenterY);
		if (true != bresult) { assert(false); pAction.reset(); pNewObjective.reset(); continue; }
		pNewObjective->m_actionList.Append(std::move(pAction));

		objectives.Append(std::move(pNewObjective));
	}
	return true;
}

int C_RemoveStructure::Execute()
{
	glm::vec3 ludicrousPos;
	ludicrousPos.x = 100000.f;
	ludicrousPos.y = 100000.f;
	ludicrousPos.z = 100000.f;

	Building * m_pBuilding = (Building *)ObjectManager->getByWatchID(m_pBuildingWID);
	m_pBuilding->position = ludicrousPos;
	m_pBuilding->update();
	if (objectiveObservationEnabled()) {
		objectiveObservationSink("structure", "updated", "removed", std::to_string(m_pBuildingWID).c_str());
	}
	return true;
}

//*************************************************************************************************
bool CNumberOfUnitsObjectiveCondition::Save( FitIniFile* file )
{
	file->writeIdULong( "Num", m_num );
	return true;
}

bool CSpecificUnitObjectiveCondition::Save( FitIniFile* file )
{
	if (ObjectManager->getByWatchID(m_pUnitWID) == nullptr)
		STOP(("Tried to save specific unit objective with NULL unit"));

	file->writeIdLong( "MoverWID", m_pUnitWID );

	return true;
}

bool C_RemoveStructure::Save( FitIniFile* file )
{
	if (ObjectManager->getByWatchID(m_pBuildingWID) == nullptr)
		STOP(("Tried to save specific Building objective with NULL Building"));

	file->writeIdLong( "BuildingWID", m_pBuildingWID );

	return true;
}

bool CSpecificStructureObjectiveCondition::Save( FitIniFile* file )
{
	if (ObjectManager->getByWatchID(m_pBuildingWID) == nullptr)
		STOP(("Tried to save specific Bduiling objective with NULL Building"));

	file->writeIdLong( "BuildingWID", m_pBuildingWID );

	return true;
}

bool CAreaObjectiveCondition::Save( FitIniFile* file )
{
	file->writeIdFloat( "TargetCenterX", m_targetCenterX );
	file->writeIdFloat( "TargetCenterY", m_targetCenterY );
	file->writeIdFloat( "TargetRadius", m_targetRadius );
	return true;
}

bool CBooleanFlagIsSet::Save( FitIniFile* file )
{
	objective_text::write(*file, "FlagID", m_flagID);
	file->writeIdBoolean("Value", m_value);
	return true;
}

bool CElapsedMissionTime::Save( FitIniFile* file )
{
	file->writeIdFloat("Time", m_time);
	return true;
}

bool CPlayBIK::Save( FitIniFile* file )
{
	objective_text::write(*file, "AVIFileName", m_pathname);
	return true;
}

bool CPlayWAV::Save( FitIniFile* file )
{
	objective_text::write(*file, "WAVFileName", m_pathname);
	return true;
}

bool CDisplayTextMessage::Save( FitIniFile* file )
{
	objective_text::write(*file, "Message", m_message);
	return true;
}

bool CDisplayResourceTextMessage::Save( FitIniFile* file )
{
	file->writeIdULong( "ResourceStringID", m_resourceStringID);
	return true;
}

bool CSetBooleanFlag::Save( FitIniFile* file )
{
	objective_text::write(*file, "FlagID", m_flagID);
	file->writeIdBoolean("Value", m_value);
	return true;
}

bool CMakeNewTechnologyAvailable::Save( FitIniFile* file )
{
	objective_text::write(*file, "PurchaseFileName", m_purchaseFilePathname);
	return true;
}

bool CObjective::Save( FitIniFile* file, int objectiveNum )
{
	objective_text::write(*file, "Title", m_text.title);
	file->writeIdBoolean( "TitleUseResourceString", m_titleUseResourceString );
	file->writeIdULong( "TitleResourceStringID", m_titleResourceStringID );
	objective_text::write(*file, "Description", m_text.description);
	file->writeIdBoolean( "DescriptionUseResourceString", m_descriptionUseResourceString );
	file->writeIdULong( "DescriptionResourceStringID", m_descriptionResourceStringID );
	file->writeIdULong( "Priority", m_priority );
	file->writeIdULong( "ResourcePoints", m_resourcePoints );
	file->writeIdBoolean( "PreviousPrimaryObjectiveMustBeComplete", m_previousPrimaryObjectiveMustBeComplete );
	file->writeIdBoolean( "AllPreviousPrimaryObjectivesMustBeComplete", m_allPreviousPrimaryObjectivesMustBeComplete );
	file->writeIdBoolean( "DisplayMarker", m_displayMarker );
	file->writeIdFloat( "MarkerX", m_markerX );
	file->writeIdFloat( "MarkerY", m_markerY );
	file->writeIdBoolean( "HiddenTrigger", m_isHiddenTrigger );
	file->writeIdBoolean( "ActivateOnFlag", m_activateOnFlag );
	objective_text::write(*file, "ActivateFlagID", m_text.activateFlagID);
	file->writeIdBoolean( "ResetStatusOnFlag", m_resetStatusOnFlag );
	objective_text::write(*file, "ResetStatusFlagID", m_text.resetStatusFlagID);

	file->writeIdULong( "NumConditions", m_conditionList.Count() );
	file->writeIdULong( "NumActions", m_actionList.Count() );
	file->writeIdULong( "NumFailureConditions", m_failureConditionList.Count() );
	file->writeIdULong( "NumFailureActions", m_failureActionList.Count() );

	objective_text::write(*file, "ObjectiveModel", m_text.modelName);

	file->writeIdLong( "ModelType", m_modelType );
	file->writeIdLong( "BaseColor", m_modelBaseColor );
	file->writeIdLong( "HighlightColor", m_modelHighlightColor );
	file->writeIdLong( "HighlightColor2", m_modelHighlightColor2 );
//	file->writeIdLong( "ModelID", m_modelID );
	file->writeIdFloat( "ModelScale", m_modelScale );

	/*state data*/
	file->writeIdBoolean( "IsActive", m_isActive );
	file->writeIdFloat( "ActivationTime", m_activationTime );
	file->writeIdBoolean( "IsResolved", m_resolved );
	file->writeIdLong( "ResolvedStatus", (int)m_resolvedStatus );
	file->writeIdBoolean( "ChangedStatus", m_changedStatus );
	/*end state data*/

	{
		int i = 0;
		for (auto* const pItem : m_conditionList) {
			file->writeBlock( std::format("Team{}Objective{}Condition{}", Alignment(), objectiveNum, i).c_str() );
			file->writeIdULong( "ConditionSpecies", (unsigned long)pItem->Species());
			file->writeIdString( "ConditionSpeciesString", g_conditionSpeciesStringArray[(int)pItem->Species()]);
			pItem->Save(file);
			i += 1;
		}
	}

	{
		int i = 0;
		for (auto* const pItem : m_actionList) {
			file->writeBlock( std::format("Team{}Objective{}Action{}", Alignment(), objectiveNum, i).c_str() );
			file->writeIdULong( "ActionSpecies", (unsigned long)pItem->Species());
			file->writeIdString( "ActionSpeciesString", g_actionSpeciesStringArray[(int)pItem->Species()]);
			pItem->Save(file);
			i += 1;
		}
	}

	{
		int i = 0;
		for (auto* const pItem : m_failureConditionList) {
			file->writeBlock( std::format("Team{}Objective{}FailureCondition{}", Alignment(), objectiveNum, i).c_str() );
			file->writeIdULong( "FailureConditionSpecies", (unsigned long)pItem->Species());
			file->writeIdString( "FailureConditionSpeciesString", g_conditionSpeciesStringArray[(int)pItem->Species()]);
			pItem->Save(file);
			i += 1;
		}
	}

	{
		int i = 0;
		for (auto* const pItem : m_failureActionList) {
			file->writeBlock( std::format("Team{}Objective{}FailureAction{}", Alignment(), objectiveNum, i).c_str() );
			file->writeIdULong( "FailureActionSpecies", (unsigned long)pItem->Species());
			file->writeIdString( "FailureActionSpeciesString", g_actionSpeciesStringArray[(int)pItem->Species()]);
			pItem->Save(file);
			i += 1;
		}
	}

	return true;
}

bool CObjectives::Save( FitIniFile* file )
{
 	file->writeBlock("Objectives Version");
	file->writeIdULong("Version", 3);

	file->writeBlock( std::format("Team{}Objectives", Alignment()).c_str() );
	file->writeIdULong( "NumObjectives", Count() );
	int i = 0;

	boolFlags.save(Alignment(),file);

	for (auto* const pItem : *this) {
		file->writeBlock( std::format("Team{}Objective{}", Alignment(), i).c_str() );
		pItem->Save(file, i);
		i += 1;
	}

	return true;
}

//*************************************************************************************************
// end of file ( Objective.cpp )
