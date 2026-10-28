//***************************************************************************
//
//	contact.cpp - This file contains the Contact and Sensor Classes code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef WARRIOR_H
#include"warrior.h"
#endif

#ifndef CONTACT_H
#include"contact.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

//#ifndef GVEHICL_H
#include"gvehicl.h"
//#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef UNITDESG_H
#include"unitdesg.h"
#endif

#ifndef SOUNDSYS_H
#include"soundsys.h"
#endif

#include<gameos.hpp>

#include "gos_profiler.h"
#include"sensor_trace.h"		// SENSOR-SCAN-FACTS-1 (gated, default-OFF)
#include "sim_trace.h"
#include "mover_los_epoch.h"
#include "mc2fmt.h"
#include "score_sort.h"
#include <cstdint>
#include <span>
#include <array>
#include <vector>
#include <utility>
#include "../GameOS/gameos/utils/mc2log.h"
#include "mc2fmt.h"

//***************************************************************************

// Sensor-scoped endpoint memoization. Never reused across mover updates, terrain changes or mission loads.
static bool s_sensorLosScan = false;
static std::vector<std::pair<GameObjectPtr, glm::vec3>> s_sensorLosPositions;

glm::vec3 SensorSystemManager::losPosition(GameObjectPtr object)
{
	const auto handle = static_cast<size_t>(object->getHandle());
	if (!s_sensorLosScan || !object->isMover() || handle >= s_sensorLosPositions.size())
		return object->getLOSPosition();
	auto& [cachedObject, cachedPosition] = s_sensorLosPositions[handle];
	if (cachedObject != object) {
		cachedPosition = object->getLOSPosition();
		cachedObject = object;
	}
	return cachedPosition;
}

// SENSOR-SCAN-FACTS-1: TU-local telemetry accumulator (default-OFF).
namespace mc2_sensor_trace { Counters g_counters = {false,0,0,0,0,0,0}; }

std::unique_ptr<SensorSystemManager>		SensorManager;

long						SensorSystem::numSensors = 0;
float						SensorSystem::scanFrequency = 0.5;

bool						TeamSensorSystem::homeTeamInContact = false;
bool						SensorSystemManager::enemyInLOS = true;

extern float scenarioTime;

constexpr uint32_t VISUAL_CONTACT_FLAG = 0x80000000U;

//***************************************************************************
// CONTACT INFO class
//***************************************************************************



//---------------------------------------------------------------------------



//***************************************************************************
// SENSOR SYSTEM routines
//***************************************************************************



//---------------------------------------------------------------------------



//---------------------------------------------------------------------------

void SensorSystem::init (void) {

	//id = 0			// this should be set by the sensor system manager only!
	master = nullptr;
	masterIndex = -1;
	owner = nullptr;
	range = -1.0;
	skill = -1;
	broken = false;
	notShutdown = true;

	ecmEffect = 1.0;

	//ALWAYS true UNLESS this is a sensor probe or a sensor tower!!
	hasLOSCapability = true;
	
	//----------------------------------------------------
	// Don't update any sensors on the very first frame...
	nextScanUpdate = 0.5;
	
	lastScanUpdate = 0.0;
	
	numContacts = 0;
	id = numSensors++;

	numExclusives = 0;
	totalContacts = 0;
}

//---------------------------------------------------------------------------

void SensorSystem::destroy (void) {

	numSensors--;
}

//---------------------------------------------------------------------------

void SensorSystem::setMaster (TeamSensorSystemPtr newMaster) {

	master = newMaster;
	broken = false;
}

//---------------------------------------------------------------------------

void SensorSystem::setOwner (GameObjectPtr newOwner) {

	owner = newOwner;
}

//---------------------------------------------------------------------------

void SensorSystem::disable (void) {

	clearContacts();
}

//---------------------------------------------------------------------------

void SensorSystem::setShutdown (bool setting) {

	// Mod-tolerance: stub-substituted BattleMech instances may never have
	// gotten a sensorSystem allocated (no COMPONENT_FORM_SENSOR in their
	// inventory). Callers throughout mech.cpp call sensorSystem->setShutdown
	// unconditionally; guard here instead of sprinkling checks across all
	// 7 call sites. Same rationale for broken/numContacts accessors below.
	// MSVC accepts comparing `this` to a cast NULL; formally UB under strict
	// C++ but de-facto portable on this target.
	if (static_cast<void*>(this) == nullptr) return;

	if (setting) {
		if (notShutdown) {
			long i = 0;
			while (i < numContacts) {
				MoverPtr mover = static_cast<MoverPtr>(ObjectManager->get(contacts[i] & 0x7FFFFFFF));
				if (contacts[i] & VISUAL_CONTACT_FLAG) {
					master->removeContact(this, mover);
					i++;
					}
				else
					removeContact(i);
			}
		}
//		clearContacts();
		notShutdown = false;
		}
	else {
		if (!notShutdown)
			for (long i = 0; i < numContacts; i++) {
				MoverPtr mover = static_cast<MoverPtr>(ObjectManager->get(contacts[i] & 0x7FFFFFFF));
				if (contacts[i] & VISUAL_CONTACT_FLAG)
					master->addContact(this, mover, i, CONTACT_VISUAL);
				else
					master->addContact(this, mover, i, getSensorQuality());
			}
		notShutdown = true;
	}
}

//---------------------------------------------------------------------------

bool SensorSystem::enabled (void) {

	if (master && (masterIndex > -1)) {
		if (!owner->getExistsAndAwake())
			return(false);
		if (owner->isDisabled())
			return(false);
		if (owner->isMover()) {
			MoverPtr mover = static_cast<MoverPtr>(owner);
			return((mover->sensor != 255) && !mover->inventory[mover->sensor].disabled);
		}
		return(true);
	}
	return(false);
}

//---------------------------------------------------------------------------
void SensorSystem::setRange (float newRange) {

	range = newRange;
}

//---------------------------------------------------------------------------
float SensorSystem::getRange (void) 
{
	float result = range;

	/*		NO More recon specialist
	if (owner && owner->isMover())
	{
		MoverPtr mover = (MoverPtr)owner;
		if (mover->pilot && mover->pilot->isReconSpecialist())
			result += range * 0.2f;
	}
	*/
	
	return result;
}

//---------------------------------------------------------------------------

void SensorSystem::setSkill (long newSkill) {

	skill = newSkill;
}

//---------------------------------------------------------------------------

float SensorSystem::getEffectiveRange (void) {

	return(range * ecmEffect);
}

//---------------------------------------------------------------------------

long SensorSystem::calcContactStatus (MoverPtr mover) {

	if (!owner->getTeam())
		return(CONTACT_NONE);

	if (mover->getFlag(OBJECT_FLAG_REMOVED)) {
		return(CONTACT_NONE);
	}

	//----------------------------------------------------------
	//If object we are looking for is at the edge, NO CONTACT!!
	if (!Terrain::IsGameSelectTerrainPosition(mover->getPosition()))
		return CONTACT_NONE;
		
	//-------------------------------------------------------------
	// Should be properly set when active probes are implemented...
	long newContactStatus = CONTACT_NONE;
	if (!notShutdown || (range == 0.0) && !broken) 
	{
		if (owner->lineOfSight(mover) && !mover->isDisabled())
			newContactStatus = CONTACT_VISUAL;
		return(newContactStatus);
	}

	if ((masterIndex == -1) || (range < 0.0)) {
		return(CONTACT_NONE);
	}

	if (owner->isMover()) {
		MoverPtr mover = static_cast<MoverPtr>(owner);
		if ((mover->sensor == 255) || mover->inventory[mover->sensor].disabled || broken) {
			return(CONTACT_NONE);
		}
	}

	if (mover->getFlag(OBJECT_FLAG_SENSOR) && !mover->isDisabled()) 
	{
		bool moverNotContact = mover->hasNullSignature() || (mover->getEcmRange() != 0.0f);
		bool moverInvisible = (mover->getStatus() == OBJECT_STATUS_SHUTDOWN);
		if (!moverInvisible && !moverNotContact)
		{
			float distanceToMover = owner->distanceFrom(mover->getPosition());
			float sensorRange = getEffectiveRange();
			if (distanceToMover <= sensorRange)
			{
				//-------------------------------------------
				//No need to check shutdown and probe AGAIN!
				newContactStatus = getSensorQuality();

				//---------------------------------------
				// If ecm affecting me, my skill drops...
				// CUT, per Mitch 2/10/00
				if ((ecmEffect < 1.0) && (newContactStatus > CONTACT_SENSOR_QUALITY_1))
					newContactStatus--;

				//---------------------------------------------------
				// We now we are within sensor range, check visual.
				float startRadius = 0.0f;
				if (!owner->isMover())
					startRadius = owner->getAppearRadius();

				if (hasLOSCapability && owner->lineOfSight(mover,startRadius))
					newContactStatus = CONTACT_VISUAL;
			}
			else	//Still need to check if visual!!! ECM and Lookout Towers!!
			{
				float startRadius = 0.0f;
				if (!owner->isMover())
					startRadius = owner->getAppearRadius();

				if (hasLOSCapability && owner->lineOfSight(mover,startRadius))
					newContactStatus = CONTACT_VISUAL;
			}
		}
		else
		{
			//Target is shutdown, can ONLY be visual cause this platform has no probe.
			float startRadius = 0.0f;
			if (!owner->isMover())
				startRadius = owner->getAppearRadius();

			if (hasLOSCapability && owner->lineOfSight(mover,startRadius))
		    	newContactStatus = CONTACT_VISUAL;
		}
	}

	//Let us know that we can see something, sensor or otherwise!!
	if (mover->getTeam() && (owner->getTeam() == Team::home) &&	mover->getTeam()->isEnemy(Team::home) &&
		(newContactStatus != CONTACT_NONE))
	{
		SensorSystemManager::enemyInLOS = true;
	}
 
	return(newContactStatus);
}

//---------------------------------------------------------------------------

long SensorSystem::getSensorQuality (void) {

	if (owner && owner->getPilot())
	{
		if (owner->getPilot()->isSensorProfileSpecialist())
		{
			if (owner->getPilot()->getRank() < 4)
			{
				return(owner->getPilot()->getRank() + CONTACT_SENSOR_QUALITY_1);
			}
			else if (owner->getPilot()->getRank() == 4)
			{
				return(CONTACT_SENSOR_QUALITY_4);
			}
		}
	}
	
	return(CONTACT_SENSOR_QUALITY_1);
}

//---------------------------------------------------------------------------

bool SensorSystem::isContact (MoverPtr mover) {

	if (mover->getFlag(OBJECT_FLAG_REMOVED))
		return(false);
	if (notShutdown)
		return(mover->getContactInfo()->getSensor(id) != NO_SENSOR_CONTACT);
	for (long i = 0; i < numContacts; i++)
		if ((contacts[i] & 0x7FFFFFFF) == mover->getHandle())
			return(true);
	return(false);
}

//---------------------------------------------------------------------------

void SensorSystem::addContact (MoverPtr mover, bool visual) {
	MC2_SIM_TRACE_LINE("SADD owner={:d} mv={:d} vis={:d}", static_cast<int32_t>(owner->getPartId()), static_cast<int32_t>(mover->getPartId()), static_cast<int32_t>(static_cast<int>(visual)));

	Assert(!isContact(mover), 0, " SensorSystem.addContact: already contact ");
	if (numContacts < MAX_CONTACTS_PER_SENSOR) {
		contacts[numContacts] = mover->getHandle();
		if (visual)
			contacts[numContacts] |= VISUAL_CONTACT_FLAG;
		if (notShutdown)
			master->addContact(this, mover, numContacts, visual ? CONTACT_VISUAL :  getSensorQuality());
		numContacts++;
	}
}

//---------------------------------------------------------------------------

void SensorSystem::modifyContact (MoverPtr mover, bool visual) {

	long contactNum = mover->getContactInfo()->getSensor(id);
	if (contactNum < MAX_CONTACTS_PER_SENSOR)
	{
		if (visual)
			contacts[contactNum] =  mover->getHandle() | VISUAL_CONTACT_FLAG;
		else
			contacts[contactNum] =  mover->getHandle();
	}

	if (notShutdown)
		master->modifyContact(this, mover, visual ? CONTACT_VISUAL :  getSensorQuality());
}

//---------------------------------------------------------------------------

void SensorSystem::removeContact (long contactIndex) {

	//-----------------------------------------------
	// This assumes the contactIndex is legitimate...

	MoverPtr contact = static_cast<MoverPtr>(ObjectManager->get(contacts[contactIndex] & 0x7FFFFFFF));
	Assert(contact != nullptr, contacts[contactIndex] & 0x7FFFFFFF, " SensorSystem.removeContact: bad contact ");
	MC2_SIM_TRACE_LINE("SREM owner={:d} mv={:d}", static_cast<int32_t>(owner->getPartId()), static_cast<int32_t>(contact->getPartId()));
	
	numContacts--;
	if ((numContacts > 0) && (contactIndex != numContacts)) {
		//-----------------------------------------------
		// Fill vacated slot with contact in last slot...
		contacts[contactIndex] = contacts[numContacts];
		MoverPtr contact = static_cast<MoverPtr>(ObjectManager->get(contacts[numContacts] & 0x7FFFFFFF));
		contact->getContactInfo()->sensors[id] = contactIndex;
	}

	if (notShutdown)
		master->removeContact(this, contact);
}

//---------------------------------------------------------------------------

void SensorSystem::removeContact (MoverPtr contact) {

	long contactIndex = contact->getContactInfo()->getSensor(id);
	if (contactIndex != NO_SENSOR_CONTACT)
		removeContact(contactIndex);
}

//---------------------------------------------------------------------------

void SensorSystem::clearContacts (void) {

	while (numContacts)
		removeContact(static_cast<long>(0));
}

//---------------------------------------------------------------------------

void SensorSystem::updateContacts (void) {

	if ((masterIndex == -1) || (range < 0.0))
		return;

	if (!enabled()) {
		clearContacts();
		return;
	}

	//---------------------------------------------------------------------
	// If we've already scanned this frame, don't bother updating contacts.
	// Otherwise, update contacts...
	if (scenarioTime == lastScanUpdate)
		return;

	long i = 0;
	while (i < numContacts) 
	{
		MoverPtr contact = static_cast<MoverPtr>(ObjectManager->get(contacts[i] & 0x7FFFFFFF));
		long contactStatus = calcContactStatus(contact);
		if (contactStatus == CONTACT_NONE)
			removeContact(i);
		else 
		{
			contacts[i] =  contact->getHandle();
			if (contactStatus == CONTACT_VISUAL)
				contacts[i] |= VISUAL_CONTACT_FLAG;
			modifyContact(contact, contactStatus == CONTACT_VISUAL);
			
/*			if (teamContactStatus < contactStatus) {
				//--------------------------------------------------
				// Better sensor info, so update the team sensors...
				contactInfo->contactStatus[owner->getTeamId()] = contactStatus;
				contactInfo->teamSpotter[owner->getTeamId()] = static_cast<uint32_t>(owner->getHandle());
			}
*/			i++;
		}
	}
}

//---------------------------------------------------------------------------
constexpr int CONTACT_TYPE1 = 0;
constexpr int CONTACT_TYPE2 = 1;
constexpr int CONTACT_TYPE3 = 2;
constexpr int CONTACT_TYPE4 = 3;

void SensorSystem::updateScan (bool forceUpdate) {

	if (!forceUpdate)
		if ((masterIndex == -1) || (range < 0.0) || (turn < 2))
			return;

	if (!enabled()) {
		clearContacts();
		return;
	}

	if (!owner->getTeam())
		return;

	if (1/*(nextScanUpdate < scenarioTime) || forceUpdate*/) 
	{
	
		long currentScan = -1;
		if ((currentScan = scanBattlefield()) > -1)		//No returns size of largest contact.
		{
			if (owner->isMover() && 
				(owner->getTeam() == Team::home) && 
				(currentScan > SoundSystem::largestSensorContact))
			{
				SoundSystem::largestSensorContact = currentScan;
			}
		}

		lastScanUpdate = scenarioTime;
		if (!forceUpdate)
			nextScanUpdate += scanFrequency;
	}
}

//---------------------------------------------------------------------------
__inline void getLargest (long &currentLargest, MoverPtr mover, long contactStatus)
{
	long thisMoverSize = -1;
	switch (contactStatus)
	{
		case	CONTACT_SENSOR_QUALITY_1:
		case	CONTACT_SENSOR_QUALITY_2:
			thisMoverSize = 0;
			break;

		case	CONTACT_SENSOR_QUALITY_3:
		case	CONTACT_SENSOR_QUALITY_4:
		case	CONTACT_VISUAL:
			float tonnage = mover->getTonnage();
			if (tonnage < 35.0f)
				thisMoverSize = 1;
			else if (tonnage < 55.0f)
				thisMoverSize = 2;
			else if (tonnage < 75.0f)
				thisMoverSize = 3;
			else
				thisMoverSize = 4;
			break;
	}

	if (thisMoverSize > currentLargest)
		currentLargest = thisMoverSize;
}

//---------------------------------------------------------------------------
long SensorSystem::scanBattlefield (void) 
{
	//NOW returns size of largest contact!
	long currentLargest = -1;
	
	if (!owner)
		Fatal(0, " Sensor has no owner ");

	if (!master)
		Fatal(0, " Sensor has no master ");

	if ((masterIndex == -1) || (range < 0.0))
		return(0);

	long numNewContacts = 0;

	long numMovers = ObjectManager->getNumMovers();
	const float radius = std::max(owner->lineOfSightRange(), getEffectiveRange() / metersPerWorldUnit);
	// Visit set, ascending mover-list order: grid candidates plus movers that are already contacts
	// (so their removals still happen in list order). Every other mover cannot change state.
	struct Visit { int ordinal; bool candidate; };
	static thread_local std::vector<Visit> visit;
	static thread_local std::vector<int> contactOrdinals;
	visit.clear();
	contactOrdinals.clear();
	for (long c = 0; c < numContacts; c++) {
		const int o = ObjectManager->moverOrdinal(static_cast<GameObjectHandle>(contacts[c] & 0x7FFFFFFF));
		if (o >= 0) contactOrdinals.push_back(o);
	}
	std::sort(contactOrdinals.begin(), contactOrdinals.end());
	{
		const auto candidates = ObjectManager->nearbyMovers(owner->getPosition(), radius);
		size_t ci = 0;
		for (const auto& cand : candidates) {
			while (ci < contactOrdinals.size() && contactOrdinals[ci] < int(cand.ordinal)) visit.push_back({contactOrdinals[ci++], false});
			while (ci < contactOrdinals.size() && contactOrdinals[ci] == int(cand.ordinal)) ci++;
			visit.push_back({int(cand.ordinal), true});
		}
		for (; ci < contactOrdinals.size(); ci++) visit.push_back({contactOrdinals[ci], false});
	}
	// SENSOR-SCAN-FACTS-1: cache the gate once outside the hot loop (OFF = one bool).
	const bool _sfacts = mc2_sensor_trace::g_counters.active;
	// Stage only missing directional mover rays. Contacts cannot change poses or terrain;
	// workers read the snapshot, and the original loop below still owns every contact update.
	if (s_sensorLosScan && owner->isMover() && owner->getTeam() &&
		ObjectManager->useMoverLineOfSightTable) {
		const bool visualOnly = !notShutdown || ((range == 0.0) && !broken);
		MoverPtr source = static_cast<MoverPtr>(owner);
		const bool canScan = visualOnly || (hasLOSCapability && source->sensor != 255 &&
			!source->inventory[source->sensor].disabled && !broken);
		if (canScan) {
			static thread_local std::vector<mc2::los::Query> rays;
			static thread_local std::vector<long> cells;
			static thread_local std::vector<uint8_t> hits;
			rays.clear();
			cells.clear();
			const float visualRange = owner->lineOfSightRange();
			for (const Visit& v : visit) {
				MoverPtr target = ObjectManager->getMover(v.ordinal);
				if (!target->getExists() || target->getTeamId() == owner->getTeamId() ||
					(!v.candidate && !isContact(target)) || target->getFlag(OBJECT_FLAG_REMOVED) ||
					!Terrain::IsGameSelectTerrainPosition(target->getPosition())) continue;
				if (!visualOnly && (!target->getFlag(OBJECT_FLAG_SENSOR) || target->isDisabled())) continue;
				if (glm::length(target->getPosition() - owner->getPosition()) > visualRange) continue;
				const long cell = owner->getHandle() * ObjectManager->maxMovers + target->getHandle();
				if (mc2::losEpochState(ObjectManager->moverLineOfSightTable[cell], ObjectManager->moverLosEpoch)) continue;
				mc2::los::Query ray;
				ray.position = SensorSystemManager::losPosition(owner);
				ray.target = SensorSystemManager::losPosition(target);
				ray.teamId = owner->getTeamId();
				ray.extRad = target->getAppearRadius();
				rays.push_back(ray);
				cells.push_back(cell);
			}
			if (!rays.empty()) {
				hits.resize(rays.size());
				Team::lineOfSightBatch(rays, hits);
				for (size_t i = 0; i < hits.size(); ++i)
					ObjectManager->moverLineOfSightTable[cells[i]] = mc2::losEpochCell(ObjectManager->moverLosEpoch, hits[i] != 0);
			}
		}
	}
	for (const Visit& v : visit)
	{
		if (_sfacts) mc2_sensor_trace::g_counters.pairsTested++;
		MoverPtr mover = ObjectManager->getMover(v.ordinal);
		if (mover->getExists() && (mover->getTeamId() != owner->getTeamId()))
		{
			if (!v.candidate && !isContact(mover)) continue;
			long contactStatus = calcContactStatus(mover);
			if (isContact(mover))
			{
				if (contactStatus == CONTACT_NONE)
				{
					removeContact(mover);
					if (_sfacts) mc2_sensor_trace::g_counters.removes++;
				}
				else
				{
					modifyContact(mover, contactStatus == CONTACT_VISUAL ? true : false);
					getLargest(currentLargest,mover,contactStatus);
					if (_sfacts) mc2_sensor_trace::g_counters.modifies++;
				}
			}
			else
			{
				if (contactStatus != CONTACT_NONE)
				{
					addContact(mover, contactStatus == CONTACT_VISUAL ? true : false);
					getLargest(currentLargest,mover,contactStatus);
					numNewContacts++;
					if (_sfacts) mc2_sensor_trace::g_counters.adds++;
				}
			}
		}
	}
	if (_sfacts) mc2_sensor_trace::g_counters.numMovers = numMovers;

	totalContacts += numNewContacts;
	
	return(currentLargest);
}

//---------------------------------------------------------------------------

long SensorSystem::scanMover (Mover* mover) {

	//---------------------------------------------------------------------
	// For now, I DO NOT return the largest contact. This should be
	// okay, since it'll just get caught during the next normal update next
	// frame.

	if (!enabled())
		return(0);

	if (mover->getExists() && (mover->getTeamId() != owner->getTeamId())) {
		long contactStatus = calcContactStatus(mover);
		if (isContact(mover)) 
		{
			if (contactStatus == CONTACT_NONE)
				removeContact(mover);
			else
			{
				modifyContact(mover, contactStatus == CONTACT_VISUAL ? true : false);
				//getLargest(currentLargest,mover,contactStatus);
			}
		}
		else 
		{
			if (contactStatus != CONTACT_NONE) 
			{
				addContact(mover, contactStatus == CONTACT_VISUAL ? true : false);
				//getLargest(currentLargest,mover,contactStatus);
				totalContacts++;
			}
		}
	}
	return(0);
}

//---------------------------------------------------------------------------

long SensorSystem::getTeamContacts (int* contactList, int contactCriteria, int sortType) {

	Assert(master != nullptr, 0, " SensorSystem.getTeamContacts: null master ");
	return(master->getContacts(owner, contactList, contactCriteria, sortType));
}

//***************************************************************************
// TEAM SENSOR SYSTEM class
//***************************************************************************



//---------------------------------------------------------------------------



//---------------------------------------------------------------------------

void TeamSensorSystem::init (void) {

	teamId = -1;
	nextContactId = 0;
	numContactUpdatesPerPass = NUM_CONTACT_UPDATES_PER_PASS;
	curContactUpdate = 0;
	numContacts = 0;
	numEnemyContacts = 0;
	numSensors = 0;
	capLoggedThisMission = false;
	ecms = nullptr;
	jammers = nullptr;
}

//---------------------------------------------------------------------------
void TeamSensorSystem::destroy (void)
{
	init();
}

//---------------------------------------------------------------------------

void TeamSensorSystem::setTeam (TeamPtr newTeam) {

	teamId = newTeam->getId();

	if (Team::teams[teamId]->rosterSize < NUM_CONTACT_UPDATES_PER_PASS)
		numContactUpdatesPerPass = Team::teams[teamId]->rosterSize;
	else
		numContactUpdatesPerPass = NUM_CONTACT_UPDATES_PER_PASS;
}

//---------------------------------------------------------------------------

void TeamSensorSystem::addSensor (SensorSystemPtr sensor) {

	if (numSensors == MAX_SENSORS_PER_TEAM)
	{
		if (!capLoggedThisMission)
		{
			capLoggedThisMission = true;
			char _cbbuf[128];
			(*std::format_to_n(_cbbuf, (sizeof(_cbbuf)) - 1,
				"[CONTACT v1] event=sensor_cap_hit team={:d} numSensors={:d} max={:d}",
				mc2::fmt::as_i32(teamId), mc2::fmt::as_i32(numSensors), mc2::fmt::as_i32(MAX_SENSORS_PER_TEAM)).out = '\0');
			puts(_cbbuf);
			fflush(stdout);
		}
		Fatal(0, " TeamSensorSystem.addSensor: too many sensors ");
	}

	sensor->setMasterIndex(numSensors);
	sensors[numSensors++] = sensor;
}

//---------------------------------------------------------------------------

void TeamSensorSystem::removeSensor (SensorSystemPtr sensor) {

	sensor->clearContacts();
	long index = sensor->getMasterIndex();
	sensor->setMasterIndex(-1);

	sensors[index] = nullptr;
	if (index < (numSensors - 1)) {
		sensors[index] = sensors[numSensors - 1];
		sensors[index]->setMasterIndex(index);
		sensors[numSensors - 1] = nullptr;
	}
	numSensors--;
}

//---------------------------------------------------------------------------

void TeamSensorSystem::update (void) {

	if (numSensors > 0) {
		
		if (Team::teams[teamId] == Team::home)
			SoundSystem::largestSensorContact = -1;

		//---------------------------------
		// First, update actual scanning...
		// SENSOR-SCAN-FACTS-1: RAII timer/emitter around the O(sensors x movers)
		// scan (gated, default-OFF -> byte-identical). ScanScope resets counters
		// on entry (when active) and emits one "SENSOR" JSONL frame on dtor.
		{
			ZoneScopedN("GameLogic.Sensors.Scan");
			mc2_sensor_trace::ScanScope _sscope(teamId);
			if (_sscope.active)
				mc2_sensor_trace::g_counters.numSensors = numSensors;
			for (long i = 0; i < numSensors; i++)
				sensors[i]->updateScan();
		}

		if (Team::teams[teamId]->rosterSize < NUM_CONTACT_UPDATES_PER_PASS)
			numContactUpdatesPerPass = Team::teams[teamId]->rosterSize;
		else
			numContactUpdatesPerPass = NUM_CONTACT_UPDATES_PER_PASS;

		//--------------------------------
		// Now, update current contacts...
		{ ZoneScopedN("GameLogic.Sensors.Contacts");
		for (int i = 0; i < numContactUpdatesPerPass; i++) {
			if (curContactUpdate >= numSensors)
				curContactUpdate = 0;
			sensors[curContactUpdate]->updateContacts();
			curContactUpdate++;
		} }
	}
}

//---------------------------------------------------------------------------

long TeamSensorSystem::getVisualContacts (MoverPtr* moverList) {

	long numVisualContacts = 0;
	for (long i = 0; i < numContacts; i++) {
		MoverPtr contact = static_cast<MoverPtr>(ObjectManager->get(contacts[i]));
		if (!contact->getFlag(OBJECT_FLAG_REMOVED))
			if (contact->getContactStatus(teamId, true) == CONTACT_VISUAL)
				moverList[numVisualContacts++] = contact;
	}
	return(numVisualContacts);
}

//---------------------------------------------------------------------------

long TeamSensorSystem::getSensorContacts (MoverPtr* moverList) {

	static bool isSensor[NUM_CONTACT_STATUSES] = {false, true, true, true, true, false};
	long numSensorContacts = 0;
	for (long i = 0; i < numContacts; i++) {
		MoverPtr contact = static_cast<MoverPtr>(ObjectManager->get(contacts[i]));
		if (!contact->getFlag(OBJECT_FLAG_REMOVED))
			if (isSensor[contact->getContactStatus(teamId, true)])
				moverList[numSensorContacts++] = contact;
	}
	return(numSensorContacts);
}

//---------------------------------------------------------------------------

bool TeamSensorSystem::meetsCriteria (GameObjectPtr looker, MoverPtr mover, long contactCriteria) {

	bool isSensor[NUM_CONTACT_STATUSES] = {false, true, true, true, true, false};
	long status = mover->getContactStatus(teamId, true);

	if (mover->getFlag(OBJECT_FLAG_REMOVED))
		return(false);

	if (status == CONTACT_NONE)
		return(false);

	if (contactCriteria & CONTACT_CRITERIA_VISUAL_OR_SENSOR)
		if ((status != CONTACT_VISUAL) && !isSensor[status])
			return(false);

	if (contactCriteria & CONTACT_CRITERIA_VISUAL)
		if (status != CONTACT_VISUAL)
			return(false);

	if (contactCriteria & CONTACT_CRITERIA_SENSOR)
		if (!isSensor[status])
			return(false);

	if (contactCriteria & CONTACT_CRITERIA_NOT_CHALLENGED) {
		if (mover->getChallenger() != nullptr)
			return(false);
	}

	if (contactCriteria & CONTACT_CRITERIA_NOT_DISABLED) {
		if (mover->isDisabled())
			return(false);
	}

	if (contactCriteria & CONTACT_CRITERIA_ENEMY) {
		if ((teamId == -1) || !mover->getTeam())
			return(false);
		if (!Team::teams[teamId]->isEnemy(mover->getTeam()))
			return(false);
	}

	if (contactCriteria & CONTACT_CRITERIA_ARMED) {
		if (mover->numFunctionalWeapons == 0)
			return(false);
	}

	return(true);
}

//---------------------------------------------------------------------------

bool TeamSensorSystem::hasSensorContact (long teamID) {

	for (long i = 0; i < numContacts; i++) {
		MoverPtr mover = static_cast<MoverPtr>(ObjectManager->get(contacts[i]));
		if (!mover->getFlag(OBJECT_FLAG_REMOVED)) {
			static bool isSensor[NUM_CONTACT_STATUSES] = {false, true, true, true, true, false};
			if (isSensor[mover->getContactStatus(teamID, true)])
				return(true);
		}
	}
	return(false);
}

//---------------------------------------------------------------------------

long TeamSensorSystem::getContacts (GameObjectPtr looker, int* contactList, int contactCriteria, int sortType) {

	if ((sortType != CONTACT_SORT_NONE) && !looker)
		return(0);

	// A team can hold MAX_CONTACTS_PER_TEAM contacts and every one may pass the criteria, so the buffers are that long; the caller's contactList must be too.
	static_assert(sizeof(TeamSensorSystem::contacts) / sizeof(long) == MAX_CONTACTS_PER_TEAM, "getContacts buffers follow the team contact list");
	mc2::Scored scored[MAX_CONTACTS_PER_TEAM];

	float CV = 0;
	long numValidContacts = 0;
	for (long i = 0; i < numContacts; i++) {
		MoverPtr mover = static_cast<MoverPtr>(ObjectManager->get(contacts[i]));
		if (!meetsCriteria(looker, mover, contactCriteria))
			continue;
		scored[numValidContacts].id = static_cast<std::int32_t>(mover->getHandle());
		switch (sortType) {
			case CONTACT_SORT_NONE:
				scored[numValidContacts].score = 0.0;
				break;
			case CONTACT_SORT_CV:
				CV = static_cast<float>(mover->getCurCV());
				scored[numValidContacts].score = CV;
				break;
			case CONTACT_SORT_DISTANCE:
				scored[numValidContacts].score = looker->distanceFrom(mover->getPosition());
				break;
		}
		numValidContacts++;
	}

	// Distance is nearest first, value strongest first; equal keys keep scan order. All valid contacts are sorted: the team list never holds more than the buffer.
	if (!contactList)
		MC2_LOG_ONCE(Sim, Warn, "getContacts: no output list, {} valid contacts counted but none returned", numValidContacts);
	if (contactList) {
		if ((numValidContacts > 0) && (sortType != CONTACT_SORT_NONE))
			mc2::sort_scored(std::span<mc2::Scored>(scored, numValidContacts),
				(sortType == CONTACT_SORT_DISTANCE) ? mc2::SortOrder::Ascending : mc2::SortOrder::Descending);
		for (long contact = 0; contact < numValidContacts; contact++)
			contactList[contact] = scored[contact].id;
	}


	return(numValidContacts);
}

//---------------------------------------------------------------------------

long TeamSensorSystem::getContactStatus (MoverPtr mover, bool includingAllies) {

	if (mover->getFlag(OBJECT_FLAG_REMOVED))
		return(CONTACT_NONE);
	return(mover->getContactInfo()->getContactStatus(Team::teams[teamId]->getId(), true));
}

//---------------------------------------------------------------------------

void TeamSensorSystem::scanBattlefield (void) {

	if (numSensors)
		for (long i = 0; i < numSensors; i++)
			sensors[i]->updateScan(true);
}

//---------------------------------------------------------------------------

void TeamSensorSystem::scanMover (Mover* mover) {

	if (numSensors)
		for (long i = 0; i < numSensors; i++)
			sensors[i]->scanMover(mover);
}

//---------------------------------------------------------------------------

void TeamSensorSystem::incNumEnemyContacts (void) {

	numEnemyContacts++;
	if ((Team::teams[teamId] == Team::home) && numEnemyContacts)
		homeTeamInContact = true;
}

//---------------------------------------------------------------------------

void TeamSensorSystem::decNumEnemyContacts (void) {

	numEnemyContacts--;
	if (!numEnemyContacts && (Team::teams[teamId] == Team::home))
		homeTeamInContact = false;
		
	if (numEnemyContacts < 0)
		Fatal(0, " Negative Team Contact Count ");
}

//---------------------------------------------------------------------------

void TeamSensorSystem::addContact (SensorSystemPtr sensor, MoverPtr contact, long contactIndex, long contactStatus) {

	Assert(numContacts < MAX_CONTACTS_PER_TEAM, numContacts, " TeamSensorSystem.addContact: max team contacts ");
	ContactInfoPtr contactInfo = contact->getContactInfo();
	contactInfo->sensors[sensor->id] = contactIndex;
	contactInfo->contactCount[teamId]++;
	if (contactInfo->contactStatus[teamId] < contactStatus) {
		contactInfo->contactStatus[teamId] = contactStatus;
		contactInfo->teamSpotter[teamId] = sensor->owner->getHandle();

	}
	if (contactInfo->contactCount[teamId] == 1) {
		contacts[numContacts] = contact->getHandle();
		contactInfo->teams[teamId] = numContacts;
		numContacts++;
		sensor->numExclusives++;
	}
}

//---------------------------------------------------------------------------

SensorSystemPtr TeamSensorSystem::findBestSpotter (MoverPtr contact, long* status) {

	ContactInfoPtr contactInfo = contact->getContactInfo();
	if (!contactInfo) {
		char s[256];
		(*std::format_to_n(s, sizeof(s) - 1, "TeamSensorSystem.findBestSpotter: NULL contactInfo for objClass {:d} partID {:d} team {:d}", mc2::fmt::as_i32(contact->getObjectClass()), mc2::fmt::as_i32(contact->getPartId()), mc2::fmt::as_i32(contact->getTeamId())).out = '\0');
		STOP((s));
	}
	SensorSystemPtr bestSensor = nullptr;
	long bestStatus = CONTACT_NONE;
	for (long i = 0; i < MAX_SENSORS; i++)
		if (contactInfo->sensors[i] != NO_SENSOR_CONTACT) {
			SensorSystemPtr sensor = SensorManager->getSensor(i);
			if (sensor && sensor->owner && (teamId == sensor->owner->getTeamId())) {
				long status = sensor->calcContactStatus(contact);
				if (status >= bestStatus) {
					bestSensor = sensor;
					bestStatus = status;
				}
			}
		}
	if (status)
		*status = bestStatus;
	return(bestSensor);
}

//---------------------------------------------------------------------------

void TeamSensorSystem::modifyContact (SensorSystemPtr sensor, MoverPtr contact, long contactStatus) {

	ContactInfoPtr contactInfo = contact->getContactInfo();
	if (contactInfo->teamSpotter[teamId] == sensor->owner->getHandle()) {
		long curStatus = contactInfo->contactStatus[teamId];
		if (contactStatus > curStatus)
			contactInfo->contactStatus[teamId] = contactStatus;
		else if (contactStatus < curStatus) {
			long bestStatus;
			SensorSystemPtr bestSensor = findBestSpotter(contact, &bestStatus);
			contactInfo->contactStatus[teamId] = bestStatus;
			contactInfo->teamSpotter[teamId] = bestSensor->owner->getHandle();
		}
		}
	else {
		long curStatus = contactInfo->contactStatus[teamId];
		if (contactStatus > curStatus) {
			contactInfo->contactStatus[teamId] = contactStatus;
			contactInfo->teamSpotter[teamId] = sensor->owner->getHandle();
		}
	}
}

//---------------------------------------------------------------------------

void TeamSensorSystem::removeContact (SensorSystemPtr sensor, MoverPtr contact) {

	ContactInfoPtr contactInfo = contact->getContactInfo();
	contactInfo->sensors[sensor->id] = NO_SENSOR_CONTACT;
	Assert(contactInfo->contactCount[teamId] > 0, 0, "Negative contact count");
	contactInfo->contactCount[teamId]--;

	if (contactInfo->contactCount[teamId] == 0) {
		long contactIndex = contactInfo->teams[teamId];
		Assert(contactIndex < 0xFFFF, contactIndex, " 0xFFFF ");
		MoverPtr removedContact = static_cast<MoverPtr>(ObjectManager->get(contacts[contactIndex]));
		Assert(removedContact == contact, contactIndex, " TeamSensorSystem.removeContact: bad contact ");
		
		contactInfo->teams[teamId] = 0xFFFF;
		contactInfo->contactStatus[teamId] = CONTACT_NONE;
		contactInfo->teamSpotter[teamId] = 0;         
	
		numContacts--;
		if ((numContacts > 0) && (contactIndex != numContacts)) {
			//-----------------------------------------------
			// Fill vacated slot with contact in last slot...
			contacts[contactIndex] = contacts[numContacts];
			MoverPtr mover = static_cast<MoverPtr>(ObjectManager->get(contacts[numContacts]));
			mover->getContactInfo()->teams[teamId] = contactIndex;
		}
		}
	else if (contactInfo->teamSpotter[teamId] == sensor->owner->getHandle()) {
		if (sensor->owner->getObjectClass() == BATTLEMECH)
			Assert(sensor != nullptr, 0, " dumb ");
		long bestStatus;
		SensorSystemPtr bestSensor = findBestSpotter(contact, &bestStatus);
//		Assert(bestSensor != NULL, 0, " hit ");
		if (bestSensor) {
			contact->contactInfo->contactStatus[teamId] = bestStatus;
			contact->contactInfo->teamSpotter[teamId] = bestSensor->owner->getHandle();
		}
	}
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------

void TeamSensorSystem::updateEcmEffects (void) {

	for (long i = 0; i < numSensors; i++)
		sensors[i]->ecmEffect = SensorManager->getEcmEffect(sensors[i]->owner); 
}

//***************************************************************************
// SENSOR SYSTEM MANAGER class
//***************************************************************************



//---------------------------------------------------------------------------



//---------------------------------------------------------------------------

long SensorSystemManager::init (bool debug) {

	if (MAX_SENSORS < 2)
		Fatal(0, " Way too few sensors in Sensor System Manager! ");

	sensorPool = static_cast<SensorSystemPtr*>(std::malloc(MAX_SENSORS * sizeof(SensorSystemPtr)));
	gosASSERT(sensorPool!=nullptr);

	for (long i = 0; i < MAX_SENSORS; i++)
		sensorPool[i] = new SensorSystem;

	//-----------------------------------------------------
	// This assumes we have at least 2 sensors in the pool
	// when initializing the pool...
	sensorPool[0]->id = 0;
	sensorPool[0]->prev = nullptr;
	sensorPool[0]->next = sensorPool[1];

	for (int i = 1; i < (MAX_SENSORS - 1); i++) {
		sensorPool[i]->id = i;
		sensorPool[i]->prev = sensorPool[i - 1];
		sensorPool[i]->next = sensorPool[i + 1];
	}

	sensorPool[MAX_SENSORS - 1]->id = MAX_SENSORS - 1;
	sensorPool[MAX_SENSORS - 1]->prev = sensorPool[MAX_SENSORS - 2];
	sensorPool[MAX_SENSORS - 1]->next = nullptr;

	//------------------------------
	// All start on the free list...
	freeList = sensorPool[0];
	freeSensors = MAX_SENSORS;

	Assert (!debug || (Team::numTeams > 0), 0, " SensorSystemManager.init: 0 teams ");

	for (int i = 0; i < Team::numTeams; i++) {
		teamSensors[i] = std::make_unique<TeamSensorSystem>();
		teamSensors[i]->teamId = i;
	}

	SensorSystem::numSensors = 0;

	return(NO_ERR);
}

long SensorSystemManager::checkIntegrity (void)
{
	//See if every pointer in sensorPool is still OK.
	// SOMETHING is trashing memory here.  Damned if I know what.
	long result = 0;

	for (long i=0;i<MAX_SENSORS;i++)
	{
		SensorSystem *sensor = getSensor(i);
		if (sensor->owner)
			result = 1;
		else
			result = 0;
	}

	return result;
}

//---------------------------------------------------------------------------

SensorSystemPtr SensorSystemManager::newSensor (void) {

	// Stub-substituted chassis (e.g. keid-v objType 252) may exhaust the pool
	// when the mech.cpp fallback allocates a default sensor for every chassis
	// missing a COMPONENT_FORM_SENSOR.  Return nullptr instead of Fatal so
	// that guarded callers (mech.cpp:3350 stub-chassis fallback) survive
	// gracefully.  Unguarded callers in normal load paths never hit this
	// because they only fire when a COMPONENT_FORM_SENSOR is present.
	if (!freeSensors)
		return nullptr;

	freeSensors--;

	//---------------------------------------
	// Grab the first free sensor in line...
	SensorSystemPtr sensor = freeList;

	//------------------------------------------
	// Cut the new sensor from the free list...
	freeList = freeList->next;
	if (freeList)
		freeList->prev = nullptr;

	//----------------------------------------------------
	// New system has no next. Already has no previous...
	sensor->next = nullptr;

	return(sensor);
}

//---------------------------------------------------------------------------

void SensorSystemManager::freeSensor (SensorSystemPtr sensor) {

	freeSensors++;

	sensor->prev = nullptr;
	sensor->next = freeList;
	freeList->prev = sensor;
	freeList = sensor;
}

//---------------------------------------------------------------------------

void SensorSystemManager::destroy (void) {

	if (sensorPool)
	{
		for (long i = 0; i < MAX_SENSORS; i++) 
		{
			delete sensorPool[i];
			sensorPool[i] = nullptr;
		}
		
		// Exit-path containment: normal teardown destroys the sensors while the heap is alive; a null heap means the process is ending.
		std::free(sensorPool);
		sensorPool = nullptr;
	}

	freeSensors = 0;
	freeList = nullptr;

	for (long i = 0; i < Team::numTeams; i++) 
	{
		teamSensors[i].reset();
	}
}

//---------------------------------------------------------------------------

void SensorSystemManager::addTeamSensor (long teamId, SensorSystemPtr sensor) 
{
	if ((teamId > -1) && (teamId < Team::numTeams))
	{
		teamSensors[teamId]->addSensor(sensor);
		sensor->setMaster(teamSensors[teamId].get());
	}
}

//---------------------------------------------------------------------------

void SensorSystemManager::removeTeamSensor (long teamId, SensorSystemPtr sensor) {

	if (teamId == -1)
		return;
	teamSensors[teamId]->removeSensor(sensor);
	sensor->setMaster(nullptr);
}

//---------------------------------------------------------------------------

void SensorSystemManager::update (void) 
{
	{ ZoneScopedN("GameLogic.Sensors.ECM"); updateEcmEffects(); }
	// Contact updates read poses and terrain; they do not advance either. Drop all entries before the next scan.
	s_sensorLosPositions.assign(static_cast<size_t>(ObjectManager->maxMovers), {});
	s_sensorLosScan = true;
	struct EndScan { ~EndScan() { s_sensorLosScan = false; } } endScan;
	{ ZoneScopedN("GameLogic.Sensors.Teams"); updateSensors(); }
//	updateTeamContactLists();
}

//---------------------------------------------------------------------------

void SensorSystemManager::updateEcmEffects (void) {

	for (long i = 0; i < Team::numTeams; i++)
		teamSensors[i]->updateEcmEffects();
}

//---------------------------------------------------------------------------

void SensorSystemManager::updateSensors (void) 
{
	/*
	for (long i = 0; i < Team::numTeams; i++)
		teamSensors[i]->update();
	*/
	
	//Let's try one team per frame update!!
	if (Team::teams[teamToUpdate] == Team::home)
		enemyInLOS = false;		//Resets every scan for HOME team!!!!
		
 	teamSensors[teamToUpdate]->update();
	teamToUpdate++;
	if (teamToUpdate == Team::numTeams)
		teamToUpdate = 0;
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------

void SensorSystemManager::addEcm (GameObjectPtr owner, float range) {

	if (numEcms == MAX_ECMS)
		Fatal(0, " SensorSystemManager.addEcm: too many ecms ");

	ecms[numEcms].owner = owner;
	ecms[numEcms].range = range;
	numEcms++;
}

//---------------------------------------------------------------------------

float SensorSystemManager::getEcmEffect (GameObjectPtr victim) {

	/*		ECM stealths the mech carrying it.  Period.
	for (long i = 0; i < numEcms; i++)
		if (!victim->isFriendly(ecms[i].owner))
			if (ecms[i].owner->distanceFrom(victim->getPosition()) <= ecms[i].range)
				return(0.5);
	*/
	return(1.0);
}

//***************************************************************************




