//***************************************************************************
//
//	movemgr.h -- Class definitions for the move path manager.
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MOVEMGR_H
#define MOVEMGR_H

//---------------------------------------------------------------------------

#ifndef DMOVEMGR_H
#include"dmovemgr.h"
#endif

#ifndef DWARRIOR_H
#include"dwarrior.h"
#endif

//***************************************************************************

using PathQueueRecPtr = struct _PathQueueRec*;

struct _PathQueueRec {
	long				num;
	MechWarriorPtr		pilot;
	long				selectionIndex;
	unsigned long		moveParams;
	bool				initPath;
	bool				faceObject;
	long				enqueueFrame;	// sim tick of first pending request; replacements retain it
	PathQueueRecPtr		prev;
	PathQueueRecPtr		next;
};
using PathQueueRec = _PathQueueRec;

//---------------------------------------------------------------------------

class MovePathManager {

	public:

		PathQueueRec		pool[MAX_MOVERS];
		PathQueueRecPtr		queueFront;
		PathQueueRecPtr		queueEnd;
		PathQueueRecPtr		freeList;
		static long			numPaths;
		static long			peakPaths;
		static long			sourceTally[50];

	public:


		
		MovePathManager (void) {
			init();
		}
		
		~MovePathManager (void) {
			destroy();
		}
		
		void destroy (void);
		
		long init (void);

		void remove (PathQueueRecPtr rec);

		PathQueueRecPtr remove (MechWarriorPtr pilot);

		void request (MechWarriorPtr pilot, long selectionIndex, unsigned long moveParams, long source);

		void calcPath (void);

		void update (void);
};

using MovePathManagerPtr = MovePathManager*;

// Path-manager frame counter (advanced once per update()); the jump-fail backoff and its save record use this clock.
extern long MovePathManager_getFrameCounter (void);

#endif

//***************************************************************************




