//***************************************************************************
//
//	Dteam.h -- File contains the Team Definition
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DTEAM_H
#define DTEAM_H

//---------------------------------------------------------------------------

#define	MAX_TEAMS						8
#include "dmovemgr.h"
inline constexpr int MAX_MOVERS_PER_TEAM = MAX_MOVERS;
#define	MAX_CONTACTS_PER_TEAM			MAX_MOVERS
inline constexpr int NUM_CONTACT_UPDATES_PER_PASS = 3;

enum Relation {
	RELATION_FRIENDLY,
	RELATION_NEUTRAL,
	RELATION_ENEMY,
	NUM_RELATIONS
};

//---------------------------------------------------------------------------

class Team;
using TeamPtr = Team*;

struct _SystemTracker;
using SystemTrackerPtr = _SystemTracker*;

#endif

//***************************************************************************


