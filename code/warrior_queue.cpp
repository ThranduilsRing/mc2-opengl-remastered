// MechWarrior: tactical-order queue API.
// Declared in the class headers; shared names in warrior_internal.h.
#include "warrior_internal.h"
#include "mc2fmt.h"


// QUEUED PLAYER ORDERS routines

bool MechWarrior::enableTacOrderQueue (void) {

	// Capacity first: on failure the whole old binding (pointer, count, records, every marker owner) is preserved (B7o.4).
	return(mc2::rebindQueue(tacOrderQueue, TacOrderQueue, MAX_QUEUED_TACORDERS, TacOrderQueuePos, numTacOrdersQueued, queueMarkers));
}

long MechWarrior::addQueuedTacOrder (TacticalOrder tacOrder) {

	if (!tacOrderQueue)
	{
		gosASSERT( !"No TacOrder Queue" );
	}

	if (numTacOrdersQueued == MAX_QUEUED_TACORDERS_PER_WARRIOR)
		return(2);

	tacOrderQueue[numTacOrdersQueued].moveMode = tacOrder.moveParams.wayPath.mode[0];

	if ( tacOrder.moveParams.wayPath.mode[0] == TRAVEL_MODE_SLOW )
		queueMarkers[numTacOrdersQueued] = getWayPointMarker( tacOrder.getWayPoint(0), "WalkWayPoint" );

	else if ( tacOrder.moveParams.wayPath.mode[0] == TRAVEL_MODE_FAST )
		queueMarkers[numTacOrdersQueued] = getWayPointMarker( tacOrder.getWayPoint(0), "RunWayPoint" );

	else if ( tacOrder.moveParams.wayPath.mode[0] == TRAVEL_MODE_JUMP )
		queueMarkers[numTacOrdersQueued] = getWayPointMarker( tacOrder.getWayPoint(0), "JumpWayPoint" );

	tacOrderQueue[numTacOrdersQueued].point = tacOrder.getWayPoint(0);
	tacOrderQueue[numTacOrdersQueued].tactic = tacOrder.attackParams.tactic;
	tacOrderQueue[numTacOrdersQueued].packedData[0] = tacOrder.data[0];
	tacOrderQueue[numTacOrdersQueued].packedData[1] = tacOrder.data[1];
	numTacOrdersQueued++;

	if (numTacOrdersQueued == 1) {
		if (!newTacOrderReceived[ORDERSTATE_PLAYER]) {
			if (!curPlayerOrderFromQueue || (curTacOrder.origin != ORDER_ORIGIN_PLAYER)) {
				// First queued order just given, so begin executing it immediately... NO, don't do 
				if ( tacOrderQueueExecuting )
					executeTacOrderQueue();
			}
		}
	}

	return(0);
}

long MechWarrior::removeQueuedTacOrder (TacticalOrderPtr tacOrder) {

	if (!tacOrderQueue)
		return(1);

	if (numTacOrdersQueued == 0)
		return(2);

	tacOrderQueueIndex = 0;
	tacOrder->data[0] = tacOrderQueue[tacOrderQueueIndex].packedData[0];
	tacOrder->data[1] = tacOrderQueue[tacOrderQueueIndex].packedData[1];
	Stuff::Vector3D point = tacOrderQueue[tacOrderQueueIndex].point;
	TacticType tactic = (TacticType)tacOrderQueue[tacOrderQueueIndex].tactic;

	// Head marker deleted on purpose, records and owners shift (B7o.4: the legacy loop's slot-16 read at full capacity is gone).
	mc2::shiftQueueDown(tacOrderQueue, queueMarkers, numTacOrdersQueued);
	numTacOrdersQueued--;

	tacOrder->unpack();
	tacOrder->attackParams.tactic = tactic;
	tacOrder->setWayPoint(0, point);

	return(0);
}

long MechWarrior::getNextQueuedTacOrder (TacticalOrderPtr tacOrder) {

	if (!tacOrderQueue)
		return(1);

	if (numTacOrdersQueued == 0)
		return(2);

	if (!tacOrderQueueLooping)
		return(removeQueuedTacOrder(tacOrder));

	tacOrder->data[0] = tacOrderQueue[tacOrderQueueIndex].packedData[0];
	tacOrder->data[1] = tacOrderQueue[tacOrderQueueIndex].packedData[1];
	tacOrder->unpack();
	tacOrder->setWayPoint(0, tacOrderQueue[tacOrderQueueIndex].point);
	tacOrder->attackParams.tactic = (TacticType)tacOrderQueue[tacOrderQueueIndex].tactic;

	tacOrderQueueIndex++;
	if (tacOrderQueueIndex >= numTacOrdersQueued)
		tacOrderQueueIndex = 0;
	return(0);
}

long MechWarrior::peekQueuedTacOrder (long index, TacticalOrderPtr tacOrder) {

	if (!tacOrderQueue)
		return(1);

	if (numTacOrdersQueued == 0)
		return(2);

	if (index == -1)
		index = numTacOrdersQueued - 1;

	tacOrder->data[0] = tacOrderQueue[index].packedData[0];
	tacOrder->data[1] = tacOrderQueue[index].packedData[1];
	Stuff::Vector3D point = tacOrderQueue[index].point;
	TacticType tactic = (TacticType)tacOrderQueue[index].tactic;
	long tacOrderId = tacOrderQueue[index].id;

	tacOrder->unpack();
	tacOrder->setWayPoint(0, point);
	tacOrder->attackParams.tactic = tactic;
	tacOrder->setId(tacOrderId);

	return(0);
}

void MechWarrior::clearTacOrderQueue (void) {

	resetQueueMarkers();

	numTacOrdersQueued = 0;
	tacOrderQueueIndex = 0;
	tacOrderQueueExecuting = false;
	tacOrderQueueLooping = false;
}

void MechWarrior::executeTacOrderQueue (void) {

	if (numTacOrdersQueued > 0) {
		tacOrderQueueExecuting = true;
		TacticalOrder tacOrder;
		if (getNextQueuedTacOrder(&tacOrder) == 0) {
			setPlayerTacOrder(tacOrder, true);
		}
		}
	else
		tacOrderQueueExecuting = false;
}

void MechWarrior::lockTacOrderQueue (void) {

	tacOrderQueueLocked = true;
}

void MechWarrior::unlockTacOrderQueue (void) {

	tacOrderQueueLocked = false;
}

long MechWarrior::getTacOrderQueue (QueuedTacOrderPtr list) {

	long numQueuedOrders = 0;
	if (curPlayerOrderFromQueue) {
		// First queued order just given, so begin executing it immediately...
		list[0].point = tacOrder[ORDERSTATE_PLAYER].getWayPoint(0);
		list[0].packedData[0] = tacOrder[ORDERSTATE_PLAYER].data[0];
		list[0].packedData[1] = tacOrder[ORDERSTATE_PLAYER].data[1];
		numQueuedOrders = 1;
	}

	if (numTacOrdersQueued > 0) {
		for (long i = 0; i < numTacOrdersQueued; i++) {
			list[i + 1].point = tacOrderQueue[i].point;
			list[i + 1].tactic = tacOrderQueue[i].tactic;
			list[i + 1].packedData[0] = tacOrderQueue[i].packedData[0];
			list[i + 1].packedData[1] = tacOrderQueue[i].packedData[1];
			numQueuedOrders += numTacOrdersQueued;
		}
	}

	return(numQueuedOrders);
}

long compareTacOrderId (long id1, long id2) {

	if (id1 > 240) {
		if (id2 < 16)
			return(id1 - (id2 + 255));
		else
			return(id1 - id2);
		}
	else if (id1 < 16) {
		if (id2 > 240)
			return((id1 + 255) - id2);
		else
			return(id1 - id2);
	}
	return(id1 - id2);
}

void MechWarrior::updateClientOrderQueue (long curTacOrderId) {

	TacticalOrder nextTacOrder;
	long peekResult = peekQueuedTacOrder(0, &nextTacOrder);

	// Basically, used in MPlayer games to make sure the ordersQueue
	// is current with what the server says the pilot has queued (for
	// UI purposes--e.g. displaying waypts on the client).
	if (curTacOrderId == 0) {
		if ((peekResult == 0) && (nextTacOrder.getId() == lastTacOrderId))
			removeQueuedTacOrder(&nextTacOrder);
		return;
	}

	lastTacOrderId = curTacOrderId;
	while (peekResult == 0) {
		long compareResult = compareTacOrderId(nextTacOrder.getId(), curTacOrderId);
		if (compareResult < 0) {
			removeQueuedTacOrder(&nextTacOrder);
			/*
			char s[256];
			sprintf(s, "%s - removed tacorder, %d remaining\n", getName(), numTacOrdersQueued);
			mc2::fmt::debug_out(s);
			*/
			}
		else
			return;
		peekResult = peekQueuedTacOrder(0, &nextTacOrder);
	}
}
