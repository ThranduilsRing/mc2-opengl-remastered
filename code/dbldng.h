//---------------------------------------------------------------------------
//
//	dbldng.h -- File contains the Building Object Definition
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DBLDNG_H
#define DBLDNG_H

inline constexpr int MAXLEN_BUILDING_NAME = 40;

inline constexpr int GENERIC_HQ_BUILDING_OBJNUM = 637;
inline constexpr int GENERIC_DESTRUCTIBLE_RESOURCE_BUILDING_OBJNUM = 357;
inline constexpr int GENERIC_INDESTRUCTIBLE_RESOURCE_BUILDING_OBJNUM = 636;

class BuildingType;
using BuildingTypePtr = BuildingType*;

class Building;
using BuildingPtr = Building*;

#endif

//---------------------------------------------------------------------------


