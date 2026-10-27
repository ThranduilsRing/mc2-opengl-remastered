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

inline constexpr int MAX_MOVERS = 1024; // D4 ruling; contact indices are 16-bit (65535 absent)
// Frozen legacy save geometry: warrior kill history stays 85 entries (255 / 3) whatever MAX_MOVERS is.
inline constexpr int MAX_WARRIOR_KILLED = 255 / 3;
static_assert(MAX_MOVERS < 65535, "contact and team indices are 16-bit");

using PathQueueRecPtr = struct _PathQueueRec*;

class MoveMap;
using MoveMapPtr = MoveMap*;

class MovePathManager;
using MovePathManagerPtr = MovePathManager*;

#endif

//***************************************************************************




