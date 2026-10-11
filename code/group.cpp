//******************************************************************************************
//
//	group.cpp - This file contains the MoverGroup Class header
//
//	MechCOmmander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "group_internal.h"


//#define	MAX_GROUPMOVE_OFFSETS	4

/*
long GroupMoveOffsetsIndex[MAX_GROUPMOVE_OFFSETS] = {0, 1, 3, 6};
float GroupMoveOffsets[10][2] = {
	// 2-member Group
	{180.0, 50.0},
	// 3-member Group
	{-135.0, 50.0},
	{135.0, 50.0},
	// 4-member Group
	{-135.0, 50.0},
	{135.0, 50.0},
	{180.0, 50.0},
	// 5-member Group
	{-135.0, 50.0},
	{135.0, 50.0},
	{180.0, 50.0},
	{180.0, 75.0}
};
*/
extern char OverlayIsBridge[NUM_OVERLAY_TYPES];
GoalMapNode* MoverGroup::goalMap = nullptr;

//***************************************************************************
// MOVERGROUP class
//***************************************************************************

void* MoverGroup::operator new (size_t ourSize) {

	void* result = systemHeap->Malloc(ourSize);
	return(result);
}

//---------------------------------------------------------------------------

void MoverGroup::operator delete (void* us) {

	systemHeap->Free(us);
}	

//---------------------------------------------------------------------------

void MoverGroup::destroy (void) {
}

//---------------------------------------------------------------------------

bool MoverGroup::add (MoverPtr mover) {

	if (numMovers == MAX_MOVERGROUP_COUNT) {
		Fatal(0, " MoverGroup.add: Group too big ");
		//----------------------------------------
		// Should we choose to remove the fatal...
		return(false);
	}

	moverWIDs[numMovers++] = mover->getWatchID();
	mover->setGroupId(id, true);

	return(true);
}

//---------------------------------------------------------------------------

bool MoverGroup::remove (MoverPtr mover) {

	GameObjectWatchID moverWID = mover->getWatchID();
	if (moverWID == pointWID) {
		disband();
		return(true);
		}
	else {
		for (long i = 0; i < numMovers; i++)
			if (moverWIDs[i] == moverWID) {
				mover->setGroupId(-1, true);
				moverWIDs[i] = moverWIDs[numMovers - 1];
				moverWIDs[numMovers - 1] = 0;
				numMovers--;
				return(true);
			}
	}
	return(false);
}

//---------------------------------------------------------------------------

bool MoverGroup::isMember (MoverPtr mover) {

	if (!mover)
		return(false);

	GameObjectWatchID moverWID = mover->getWatchID();
	for (long i = 0; i < numMovers; i++)
		if (moverWIDs[i] == moverWID)
			return(true);
	return(false);
}

//---------------------------------------------------------------------------

long MoverGroup::disband (void) {

	for (long i = 0; i < numMovers; i++) {
		MoverPtr mover = getMover(i);
		mover->setGroupId(-1, true);
	}
#ifdef USE_IFACE
	if (pointHandle)
		theInterface->setPoint(pointHandle, false);
#endif
	pointWID = 0;
	numMovers = 0;

	return(NO_ERR);
}

//---------------------------------------------------------------------------

long MoverGroup::setPoint (MoverPtr mover) {

	if (isMember(mover)) {
#ifdef USE_IFACE
		if (pointHandle)
			theInterface->setPoint(pointHandle, false);
#endif
		pointWID = mover->getWatchID();
#ifdef USE_IFACE
		theInterface->setPoint(mover->getPartId(), true);
#endif
	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------

MoverPtr MoverGroup::getPoint (void) {

	return(dynamic_cast<MoverPtr>(ObjectManager->getByWatchID(pointWID)));
}

//---------------------------------------------------------------------------

MoverPtr MoverGroup::getMover (long i) {

	MoverPtr mover = dynamic_cast<MoverPtr>(ObjectManager->getByWatchID(moverWIDs[i]));
	if (!mover)
		Fatal(0, " MoverGroup.getMover: NULL mover ");
	return(mover);
}

//---------------------------------------------------------------------------

MoverPtr MoverGroup::selectPoint (bool excludePoint) {

	for (long i = 0; i < numMovers; i++)
		if (!excludePoint || (moverWIDs[i] != pointWID)) {
			MoverPtr mover = getMover(i);
			MechWarriorPtr pilot = mover->getPilot();
			if (pilot && pilot->alive()) {
				//----------------------------------------
				// Found a legitimate point, so set him...
				setPoint(mover);
				return(mover);
			}
		}

	//-----------------------
	// No legitimate point...
	setPoint(nullptr);
	return(nullptr);
}

//---------------------------------------------------------------------------

long MoverGroup::getMovers (MoverPtr* moverList) {

	if (numMovers > 0)
		for (long i = 0; i < numMovers; i++)
			moverList[i] = getMover(i);
	return(numMovers);
}

//---------------------------------------------------------------------------

MechWarriorPtr MoverGroup::getPointPilot (void) {

	if (pointWID)
		return(ObjectManager->getByWatchID(pointWID)->getPilot());
	return(nullptr);
}

//---------------------------------------------------------------------------

void MoverGroup::statusCount (int* statusTally) {

	for (long i = 0; i < numMovers; i++) {
		MoverPtr mover = getMover(i);
		MechWarriorPtr pilot = mover->getPilot();
		if (!mover->getExists())
			statusTally[8]++;
		else if (!mover->getAwake())
			statusTally[7]++;
		else if (pilot && (pilot->getStatus() == WARRIOR_STATUS_WITHDRAWN))
			statusTally[6]++;
		else
			statusTally[mover->getStatus()]++;
	}
}

//---------------------------------------------------------------------------

void MoverGroup::addToGUI (bool visible) {

#ifdef USE_IFACE
	for (long i = 0; i < numMovers; i++)
		theInterface->AddMech(movers[i]->getPartId(), id, movers[i]->getAwake(), visible);
#endif
}

//***************************************************************************
void MoverGroup::copyTo (MoverGroupData &data)
{
	data.id = id;
	data.numMovers = numMovers;
	memcpy(data.moverWIDs,moverWIDs,sizeof(GameObjectWatchID) * MAX_MOVERGROUP_COUNT);
	data.pointWID = pointWID;
	data.disbandOnNoPoint = disbandOnNoPoint;
}

//***************************************************************************
void MoverGroup::init (MoverGroupData &data)
{
	id = data.id;
	numMovers = data.numMovers;
	memcpy(moverWIDs,data.moverWIDs,sizeof(GameObjectWatchID) * MAX_MOVERGROUP_COUNT);
	pointWID = data.pointWID;
	disbandOnNoPoint = data.disbandOnNoPoint;
}
