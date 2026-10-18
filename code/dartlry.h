//***************************************************************************
//
//	dartlry.h -- File contains the Artillery Strike Object
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DARTLRY_H
#define DARTLRY_H

//---------------------------------------------------------------------------

class ArtilleryChunk;
using ArtilleryChunkPtr = ArtilleryChunk*;

class ArtilleryType;
using ArtilleryTypePtr = ArtilleryType*;

class Artillery;
using ArtilleryPtr = Artillery*;

class CameraDroneType;
using CameraDroneTypePtr = CameraDroneType*;

class CameraDrone;
using CameraDronePtr = CameraDrone*;

//---------------------------------------------------------------------------

enum {
	ARTILLERY_SMALL,
	ARTILLERY_LARGE,
	ARTILLERY_SENSOR,
	NUM_ARTILLERY_TYPES
};

//***************************************************************************

#endif

