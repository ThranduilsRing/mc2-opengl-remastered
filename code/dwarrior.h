//***************************************************************************
//
//	dwarrior.h - This file contains the MechWarrior Class header definitions
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DWARRIOR_H
#define DWARRIOR_H

inline constexpr int MAX_WAYPTS = 15;

inline constexpr int WEAPONS_STATUS_CANNOT_FIRE = -1;
inline constexpr int WEAPONS_STATUS_NO_TARGET = -2;
inline constexpr int WEAPONS_STATUS_OUT_OF_RANGE = -3;

inline constexpr int WEAPON_STATUS_NOT_READY = -1;
inline constexpr int WEAPON_STATUS_OUT_OF_AMMO = -2;
inline constexpr int WEAPON_STATUS_OUT_OF_RANGE = -3;
inline constexpr int WEAPON_STATUS_NOT_LOCKED = -4;
inline constexpr int WEAPON_STATUS_NO_CHANCE = -5;
inline constexpr int WEAPON_STATUS_TOO_HOT = -6;

class GoalObject;
typedef GoalObject* GoalObjectPtr;

class MechWarrior;
typedef MechWarrior* MechWarriorPtr;

//------------------------------------------------------------------------------
#endif
