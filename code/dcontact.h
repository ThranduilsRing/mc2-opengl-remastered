//***************************************************************************
//
//	dcontact.h - This file contains the Contact Class header definitions
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DCONTACT_H
#define DCONTACT_H

inline constexpr int MAX_SENSORS = 512;	// # sensor systems allowed per mission (raised from 150: stub-substituted chassis in mod campaigns e.g. keid-v flood the pool; pool is per-mission missionHeap, sensors are small)
inline constexpr int MAX_ECMS = 50;
#include "dmovemgr.h"
inline constexpr int MAX_CONTACTS_PER_SENSOR = MAX_MOVERS;
inline constexpr unsigned short NO_SENSOR_CONTACT = 0xFFFF;
static_assert(MAX_CONTACTS_PER_SENSOR <= NO_SENSOR_CONTACT);
static_assert(MAX_SENSORS <= 65535);

enum _ContactStatus {
	CONTACT_NONE,
	CONTACT_SENSOR_QUALITY_1,
	CONTACT_SENSOR_QUALITY_2,
	CONTACT_SENSOR_QUALITY_3,
	CONTACT_SENSOR_QUALITY_4,
	CONTACT_VISUAL,
	NUM_CONTACT_STATUSES
}; using ContactStatus = _ContactStatus;

enum ContactCriteria {
	CONTACT_CRITERIA_NONE = 0,
	CONTACT_CRITERIA_ENEMY = 1,
	CONTACT_CRITERIA_VISUAL = 2,
	CONTACT_CRITERIA_GUARD_BREACH = 4,
	CONTACT_CRITERIA_NOT_CHALLENGED = 8,
	CONTACT_CRITERIA_SENSOR = 16,
	CONTACT_CRITERIA_VISUAL_OR_SENSOR = 32,
	CONTACT_CRITERIA_NOT_DISABLED = 64,
	CONTACT_CRITERIA_ARMED = 128,
	NUM_CONTACT_CRITERIA = 9
};

enum ContactSortType {
	CONTACT_SORT_NONE,
	CONTACT_SORT_CV,
	CONTACT_SORT_DISTANCE,
	NUM_CONTACT_SORTS
};

class ContactInfo;
using ContactInfoPtr = ContactInfo*;

class SensorSystem;
using SensorSystemPtr = SensorSystem*;

class TeamSensorSystem;
using TeamSensorSystemPtr = TeamSensorSystem*;

class SensorSystemManager;
using SensorSystemManagerPtr = SensorSystemManager*;

#endif

//***************************************************************************

