//***************************************************************************
//
//	comndr.h - This file contains the Commander Class header
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef COMNDR_H
#define COMNDR_H

//---------------------------------------------------------------------------

#ifndef DCOMNDR_H
#include"dcomndr.h"
#endif

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef DMOVER_H
#include"dmover.h"
#endif

#ifndef GROUP_H
#include"group.h"
#include <memory>
#endif

#ifndef DTEAM_H
#include"dteam.h"
#endif

#ifndef TACORDR_H
#include"tacordr.h"
#endif

#ifndef UNITDESG_H
#include"unitdesg.h"
#endif

//---------------------------------------------------------------------------
#include "commander_save_data.h"

class Commander {

	public:

		int                     id;
		TeamPtr					team;
		std::unique_ptr<MoverGroup>	groups[MAX_MOVERGROUPS];

		static long				numCommanders;
		static CommanderPtr		commanders[MAX_COMMANDERS];
		static CommanderPtr		home;

	public:


		virtual void init (void);
		
		Commander (void) {
			init();
		}
			
		virtual void destroy (void);

		~Commander (void) {
			destroy();
		}

		virtual int getId (void) {
			return(id);
		}

		virtual void setId (int _id) {
			id = _id;
			commanders[id] = this;
		}

		virtual TeamPtr getTeam (void) {
			return(team);
		}

		virtual void setTeam (TeamPtr _team) {
			team = _team;
		}

		virtual MoverGroupPtr getGroup (long groupNumber) {
			return(groups[groupNumber].get());
		}

		long setGroup (long id, long numMates, MoverPtr* moverList, long point);

		void setLocalMoverId (long localMoverId);
		
		void eject (void);

		void addToGUI (bool visible = true);

		void setHomeCommander (void) {
			home = this;
		}

		static CommanderPtr getCommander (long _id) {
			return(commanders[_id]);
		}

		static long Save (PacketFilePtr file, long packetNum);
		static long Load (PacketFilePtr file, long packetNum, bool values = false);
};

// Null-safe commander checks: Mover::getCommander() is null for an unfilled slot (the mc2_10 launch crash class), and Commander::home
// is null before the mission assigns it. Use these instead of getCommander()->getId().
inline bool commanderIdIs (CommanderPtr c, long id) {
	return c && c->getId() == id;
}

inline bool commanderIsHome (CommanderPtr c) {
	return c && Commander::home && c->getId() == Commander::home->getId();
}

//***************************************************************************

#endif
