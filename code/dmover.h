//***************************************************************************
//
//	dmover.h - This file contains the Mover Class header definitions
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DMOVER_H
#define DMOVER_H

//---------------------------------------------------------------------------

inline constexpr int MAX_WEAPONS_PER_MOVER = 32;

inline constexpr int REFIT_VEHICLE = 0;
inline constexpr int REFIT_BAY = 1;
inline constexpr int NUM_FIXERS = 2;

inline constexpr int ARMOR_REFIT_COST = 0;
inline constexpr int INTERNAL_REFIT_COST = 1;
inline constexpr int AMMO_REFIT_COST = 2;
inline constexpr int NUM_COSTS = 3;

struct LocationNode;
using LocationNodePtr = LocationNode *;

class Mover;
using MoverPtr = Mover*;

//******************************************************************************************

#endif
