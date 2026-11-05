//******************************************************************************************
//
//	comndr.cpp - This file contains the Commander Class header
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MCLIB_H
#include"mclib.h"
#endif
#include "warrior.h"

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef UNITDESG_H
#include"unitdesg.h"
#endif

#include"team.h"

#include<stdio.h>

//---------------------------------------------------------------------------

long				Commander::numCommanders = 0;
CommanderPtr		Commander::commanders[MAX_COMMANDERS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
CommanderPtr		Commander::home = nullptr;

//***************************************************************************
// COMMANDER class
//***************************************************************************



//---------------------------------------------------------------------------



//---------------------------------------------------------------------------

void Commander::init (void) {

	id = -1;
	team = nullptr;
	for (long i = 0; i < MAX_MOVERGROUPS; i++) {
		groups[i] = std::make_unique_for_overwrite<MoverGroup>();
		groups[i]->setId(i);
	}

	numCommanders++;
}

//---------------------------------------------------------------------------

void Commander::destroy (void) {

	for (long i = 0; i < MAX_MOVERGROUPS; i++) {
		groups[i].reset();
	}

	numCommanders--;
	Assert(numCommanders >= 0, numCommanders, " Commander.destroy: numCommanders is neg ");
}

//---------------------------------------------------------------------------

long Commander::setGroup (long id, long numMates, MoverPtr* moverList, long point) {

	// First, does the unit currently have objects set up there already?
	// If so, we need to determine
	Assert((id >= 0) && (id < MAX_MOVERGROUPS), 0, " Commander::bad id in setGroup ");

	MoverGroupPtr curGroup = groups[id].get();

	Assert(curGroup != nullptr, 0, " Commander::setGroup has null group ");

	//-----------------------------------------------
	// The current group in this slot must disband...
	curGroup->disband();

	//-----------------------------------------------
	// Now, slip the new group into this unit slot...
	for (long i = 0; i < numMates; i++) {
		MoverPtr mover = moverList[i];

		//-----------------------------------------
		// Pull the mover from its current group...
		MoverGroupPtr oldGroup = mover->getGroup();
		if (oldGroup)
			oldGroup->remove(mover);

		//----------------------------
		// Add 'em to the new group...
		curGroup->add(mover);
	}

	//-----------------------------------
	// Set the point for the new group...
	//
	// The MoverList passed in is INVALID passed
	// the numMates count.  If you select a point
	// which is dead, it invariably points to a 
	// MUCH higher location in the moverList then is valid.
	// We now guard against this...
	if ((point < numMates) && (point >= 0))
		curGroup->setPoint(moverList[point]);

	return(NO_ERR);
}

//---------------------------------------------------------------------------

void Commander::setLocalMoverId (long localMoverId) 
{
	if (getTeam())
	{
		for (long j = 0; j < getTeam()->getRosterSize(); j++)
			getTeam()->getMover(j)->setLocalMoverId(localMoverId);
	}
}

//---------------------------------------------------------------------------

void Commander::eject (void) {

	for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
		MoverPtr mover = ObjectManager->getMover(i);
		if (mover && (mover->getCommanderId() == id)) {
			if (mover->getObjectClass() == BATTLEMECH) {
				if (mover->getPilot())
					mover->getPilot()->orderEject(false, true, ORDER_ORIGIN_COMMANDER);
				}
			else {
				WeaponShotInfo shot;
				shot.init(0, -3, 254.0, 0, 0);
				mover->handleWeaponHit(&shot, (MPlayer != nullptr));
			}
		}
	}
}

//---------------------------------------------------------------------------

void Commander::addToGUI (bool visible) {

	//----------------------------------
	// The GUI only supports 4 groups...
	for (long i = 0; i < 4; i++)
		groups[i]->addToGUI(visible);
}

//---------------------------------------------------------------------------
#include "save_struct_zero.h"
long Commander::Save (PacketFilePtr file, long packetNum)
{
	MC2_SAVE_STRUCT(StaticCommanderData, staticData);
	staticData.numCommanders = numCommanders;
	staticData.homeCommanderId = home->getId();

	auto bytes = mc2::save::encodeTaggedRecord(staticData, mc2::save::commanderStaticTag);
	file->writePacket(packetNum, bytes.data(), static_cast<unsigned int>(bytes.size()), STORAGE_TYPE_RAW);
	packetNum++;

	for (long i=0;i<numCommanders;i++)
	{
		MC2_SAVE_STRUCT(CommanderData, data);
		data.id = commanders[i]->getId();

		//Allied commanders have NO team!!??!
		// Just save the home team with them then.
		if (commanders[i]->getTeam())
			data.teamId = commanders[i]->getTeam()->getId();
		else
			data.teamId = Team::home->getId();

		for (long j=0;j<MAX_MOVERGROUPS;j++) {
			auto& group = data.groups[j];
			commanders[i]->groups[j]->copyTo(group);
			if (group.numMovers < 0 || group.numMovers > MAX_MOVERGROUP_COUNT)
				Fatal(0, "Invalid commander group count");
			std::fill(std::begin(group.moverWIDs) + group.numMovers, std::end(group.moverWIDs), 0);
		}

		bytes = mc2::save::encodeTaggedRecord(data, mc2::save::commanderDataTag);
		file->writePacket(packetNum, bytes.data(), static_cast<unsigned int>(bytes.size()), STORAGE_TYPE_RAW);
		packetNum++;
	}

	return packetNum;
}

//---------------------------------------------------------------------------
long Commander::Load (PacketFilePtr file, long packetNum, bool values)
{
	auto read = [&](auto& value, std::uint32_t tag) {
		file->seekPacket(packetNum);
		const auto size = file->getPacketSize();
		if (size <= 0 || size > sizeof(CommanderData) + 8) Fatal(0, "Invalid commander packet size");
		mc2::save::Bytes bytes(size);
		if (file->readPacket(packetNum, bytes.data()) != size) Fatal(0, "Short commander packet");
		if (values) {
			if (!mc2::save::decodeTaggedRecord(bytes, value, tag)) Fatal(0, "Invalid commander value record");
		} else {
			if (size != sizeof(value)) Fatal(0, "Invalid legacy commander packet");
			memcpy(&value, bytes.data(), sizeof(value));
		}
	};
	StaticCommanderData staticData{};
	read(staticData, mc2::save::commanderStaticTag);
	if (staticData.numCommanders < 1 || staticData.numCommanders > MAX_COMMANDERS ||
		staticData.homeCommanderId < 0 || staticData.homeCommanderId >= MAX_COMMANDERS)
		Fatal(0, "Invalid commander header");
	packetNum++;

	numCommanders = staticData.numCommanders;
	home = nullptr;

	for (long i=0;i<numCommanders;i++)
	{
		CommanderData data{};
		read(data, mc2::save::commanderDataTag);
		if (data.id < 0 || data.id >= MAX_COMMANDERS || data.teamId < 0 || data.teamId >= MAX_TEAMS)
			Fatal(0, "Invalid commander identity");
		for (const auto& group : data.groups)
			if (group.numMovers < 0 || group.numMovers > MAX_MOVERGROUP_COUNT)
				Fatal(0, "Invalid commander group");
		packetNum++;

		commanders[i] = new Commander;

		//Same drill as all the others.  HACK!
		// We increment numCommanders in the INIT.
		// Decrement here to maintain sanity.
		numCommanders--;

		commanders[i]->setId(data.id);
		for (auto & team : Team::teams)
		{
			if (team && (team->getId() == data.teamId))
				commanders[i]->team = team;
		}

		for (long j=0;j<MAX_MOVERGROUPS;j++)
		{
//			commanders[i]->groups[j] = new MoverGroup;
			commanders[i]->groups[j]->init(data.groups[j]);
		}

		if (staticData.homeCommanderId == commanders[i]->getId())
			home = commanders[i];
	}

	if (!home) Fatal(0, "Missing saved home commander");
	return packetNum;
}

//***************************************************************************

