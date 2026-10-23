//***************************************************************************
//
//	dmovemgr.h -- Class definitions for the move path manager.
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DMOVEMGR_H
#define DMOVEMGR_H

//***************************************************************************

inline constexpr int MAX_MOVERS = 255; // Should probably equal that in Collision System
static_assert(MAX_MOVERS <= 255, "Mover identities and contact sentinels reserve byte value 255");

using PathQueueRecPtr = struct _PathQueueRec*;

class MoveMap;
using MoveMapPtr = MoveMap*;

class MovePathManager;
using MovePathManagerPtr = MovePathManager*;

#endif

//***************************************************************************




