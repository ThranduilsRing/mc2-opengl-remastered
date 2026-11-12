//***************************************************************************
//
//	movemgr.cpp -- File contains the MovePathManager class code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef MOVEMGR_H
#include"movemgr.h"
#endif

#ifndef WARRIOR_H
#include"warrior.h"
#endif

#include"gameos.hpp"
#include"gos_profiler.h"
#include"path_trace.h"  // PATHFINDING-FACTS-1 gated PATH telemetry
#include "sim_trace.h"
#include "mc2fmt.h"
#include "path_work.h"

extern unsigned long beginPlayerPathResult(MechWarrior*);
extern void recordPlayerPathResult(MechWarrior*, long, unsigned long);
extern void dumpPlayerPathResults();
#if MC2_SIM_TRACE
#include "mover.h"   // the trace names the mover behind a path request; the file otherwise knows only the forward declaration
#endif


long MovePathManager::numPaths = 0;
long MovePathManager::peakPaths = 0;
long MovePathManager::sourceTally[50];
MovePathManagerPtr PathManager = nullptr;

// PATHFINDING-FACTS-1: per-frame path-queue activity counters (telemetry only).
static long s_pathQueuedThisFrame = 0;

// PATHFINDING-FACTS-2: monotonic path-manager frame counter, advanced once per
// update(). Used to stamp request enqueue frames and compute oldest-request age.
// Sim-owned: also clocks failed-jump backoff. Not gated by PATH telemetry.
static long s_pathFrameCounter = 0;

// PATHFINDING-JUMP-FAIL-1: expose the monotonic path-manager frame counter so the
// per-pilot jump-fail backoff (warrior.cpp) can stamp/compare failure frames on the
// same clock the queue uses. Advances unconditionally once per update() (line below).
long MovePathManager_getFrameCounter (void) { return s_pathFrameCounter; }

//***************************************************************************
// PATH MANAGER class
//***************************************************************************



//---------------------------------------------------------------------------



//---------------------------------------------------------------------------

long MovePathManager::init (void) {

	for (long i = 0; i < MAX_MOVERS; i++) {
		pool[i].pilot = nullptr;
		pool[i].selectionIndex = 0;
		pool[i].moveParams = 0;
		if (i > 0)
			pool[i].prev = &pool[i - 1];
		else
			pool[i].prev = nullptr;
		if (i < (MAX_MOVERS - 1))
			pool[i].next = &pool[i + 1];
		else
			pool[i].next = nullptr;
	}

	//------------------------------
	// All start on the free list...
	queueFront = nullptr;
	queueEnd = nullptr;
	freeList = &pool[0];

	numPaths = 0;
	peakPaths = 0;
	for (auto & sourceTallyElem : sourceTally)
		sourceTallyElem = 0;

	return(NO_ERR);
}

//---------------------------------------------------------------------------

void MovePathManager::destroy (void) {
	dumpPlayerPathResults();
}

//---------------------------------------------------------------------------

void MovePathManager::remove (PathQueueRecPtr rec) {

	//------------------------------------
	// Remove it from the pending queue...
	if (rec->prev)
		rec->prev->next = rec->next;
	else
		queueFront = rec->next;
	if (rec->next)
		rec->next->prev = rec->prev;
	else
		queueEnd = rec->prev;

	//------------------------------------
	// Return the QRec to the free list...
	rec->prev = nullptr;
	rec->next = freeList;
	freeList = rec;

	sourceTally[rec->num]--;
	numPaths--;
}

//---------------------------------------------------------------------------

PathQueueRecPtr MovePathManager::remove (MechWarriorPtr pilot) {

	PathQueueRecPtr rec = pilot->getMovePathRequest();
	if (rec) {
		remove(rec);
		pilot->setMovePathRequest(nullptr);
		return(rec);
	}
	return(nullptr);
}

//---------------------------------------------------------------------------

constexpr int DEBUG_MOVEPATH_QUEUE = 0;

void MovePathManager::request (MechWarriorPtr pilot, long selectionIndex, unsigned long moveParams, long source) {

	// Updating an outstanding request keeps its queue position and original age.
	// Repeated retries must not hide starvation or defer the player deadline.
	if (auto* pending = pilot->getMovePathRequest()) {
		sourceTally[pending->num]--;
		pending->num = source;
		sourceTally[source]++;
		pending->selectionIndex = selectionIndex;
		pending->moveParams = moveParams;
		++s_pathQueuedThisFrame;
		return;
	}
	if (!freeList)
		Fatal(0, " Too many pilots calcing paths ");

	//---------------------------------------------
	// Grab the first free move path rec in line...
	PathQueueRecPtr pathQRec = freeList;

	//-----------------------------------------
	// Cut the new record from the free list...
	freeList = freeList->next;
	if (freeList)
		freeList->prev = nullptr;

	//---------------------------------------------------
	// New record has no next. Already has no previous...
	pathQRec->num = source;
	pathQRec->pilot = pilot;
	pathQRec->selectionIndex = selectionIndex;
	pathQRec->moveParams = moveParams;

	if (queueEnd) {
		queueEnd->next = pathQRec;
		pathQRec->prev = queueEnd;
		pathQRec->next = nullptr;
		queueEnd = pathQRec;
		}
	else {
		pathQRec->prev = nullptr;
		pathQRec->next = nullptr;
		queueFront = queueEnd = pathQRec;
	}
	pilot->setMovePathRequest(pathQRec);
	
	numPaths++;
	sourceTally[source]++;
	s_pathQueuedThisFrame++;  // PATHFINDING-FACTS-1: enqueue tally (telemetry only)

	// Always stamp: queue age is part of deterministic deadline scheduling.
	pathQRec->enqueueFrame = s_pathFrameCounter;

	peakPaths = std::max(peakPaths, numPaths);
}

//---------------------------------------------------------------------------

void MovePathManager::calcPath (void) {

	if (queueFront) {
		//------------------------------
		// Grab the next in the queue...
		PathQueueRecPtr curQRec = queueFront;
		remove(queueFront);

		//--------------------------------------------------
		// If the mover is no longer around, don't bother...
		MechWarriorPtr pilot = curQRec->pilot;
		pilot->setMovePathRequest(nullptr);
		MoverPtr mover = pilot->getVehicle();
		if (!mover)
			return;

		MC2_SIM_TRACE_LINE("CALC pilot={:d} src={:d}", static_cast<int32_t>(mover->getPartId()), static_cast<int32_t>(curQRec->num));
		const auto orderSample = beginPlayerPathResult(pilot);
		const long err = pilot->calcMovePath(curQRec->selectionIndex, curQRec->moveParams);
		recordPlayerPathResult(pilot, err, orderSample);
		MC2_SIM_TRACE_LINE("CALCDONE pilot={:d} p0={:d}/{:d} p1={:d}/{:d}", static_cast<int32_t>(mover->getPartId()), static_cast<int32_t>(pilot->getMovePath(0)->curStep), static_cast<int32_t>(pilot->getMovePath(0)->numStepsWhenNotPaused), static_cast<int32_t>(pilot->getMovePath(1)->curStep), static_cast<int32_t>(pilot->getMovePath(1)->numStepsWhenNotPaused));
	}
}

//----------------------------------------------------------------------------------
void DEBUGWINS_print (char* s, long window);
void MovePathManager::update (void) {

	ZoneScopedN("GameLogic.PathManager.Update");

	#ifdef MC_PROFILE
	QueryPerformanceCounter(startCk);
	#endif

	// Every solve started here completes, so this is a soft admission budget.
	// Snapshotting the entry count also bounds zero-work/reentrant requests.
	const auto startExpansions = mc2::path_work::expansions;
	const long requestsAtEntry = numPaths;
	long oldestAge = -1;
	long playerServed = 0;
	long playerMaxAge = -1;
	long processedThisFrame = 0;
	const auto isPlayer = [](PathQueueRecPtr rec) {
		return rec->pilot->getCurTacOrder()->origin == ORDER_ORIGIN_PLAYER &&
		       rec->pilot->getCurTacOrder()->code != TACTICAL_ORDER_NONE;
	};
	while (queueFront && processedThisFrame < requestsAtEntry) {
		auto* next = mc2::path_work::nextRequest(queueFront,
			mc2::path_work::expansions - startExpansions, s_pathFrameCounter, isPlayer);
		if (!next)
			break;
		const long age = s_pathFrameCounter - next->enqueueFrame;
		oldestAge = std::max(oldestAge, age);
		if (isPlayer(next)) {
			++playerServed;
			playerMaxAge = std::max(playerMaxAge, age);
		}
		mc2::path_work::promote(queueFront, queueEnd, next);
		calcPath();
		++processedThisFrame;
	}
	const auto work = mc2::path_work::expansions - startExpansions;
	const int capHit = (work >= mc2::path_work::kTickBudget && queueFront) ? 1 : 0;
	long playerPending = 0;
	long playerPendingMaxAge = -1;
	for (auto* rec = queueFront; rec; rec = rec->next) {
		if (isPlayer(rec)) {
			++playerPending;
			playerPendingMaxAge = std::max(playerPendingMaxAge, s_pathFrameCounter - rec->enqueueFrame);
		}
	}

	mc2_path_trace::emitFrame(s_pathQueuedThisFrame, processedThisFrame, numPaths, peakPaths,
	                          oldestAge, capHit, work, playerServed, playerMaxAge,
	                          playerPending, playerPendingMaxAge);
	s_pathQueuedThisFrame = 0;

	// PATHFINDING-FACTS-2: advance the frame counter once per update().
	s_pathFrameCounter++;

//	char s[50];
//	sprintf(s, "num paths = %d", numPaths);
//	DEBUGWINS_print(s, 0);

	#ifdef MC_PROFILE
	QueryPerformanceCounter(endCk);
	srMvPathUpd += (endCk.LowPart - startCk.LowPart);
	#endif
}

//***************************************************************************
