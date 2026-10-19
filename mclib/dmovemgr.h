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

#define	MAX_MOVERS		255	// Should probably equal that in Collision System

using PathQueueRecPtr = struct _PathQueueRec*;

class MoveMap;
using MoveMapPtr = MoveMap*;

class MovePathManager;
using MovePathManagerPtr = MovePathManager*;

#endif

//***************************************************************************




