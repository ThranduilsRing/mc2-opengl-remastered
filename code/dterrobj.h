//---------------------------------------------------------------------------
//
//	Dterrobj.h -- File contains the Misc Terrain Object Definition
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
#ifndef DTERROBJ_H
#define DTERROBJ_H

inline constexpr int BRIDGEY = 0x05;
inline constexpr int FORESTY = 0x06;
inline constexpr int WALLY = 0x07;
inline constexpr int MEDIUM_WALL = 0x08;
inline constexpr int LIGHT_WALL = 0x09;

class TerrainObjectType;
typedef TerrainObjectType* TerrainObjectTypePtr;

class TerrainObject;
typedef TerrainObject* TerrainObjectPtr;

//---------------------------------------------------------------------------

#endif

