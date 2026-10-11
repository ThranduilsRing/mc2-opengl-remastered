//***************************************************************************
//
//	unitdesg.h - This file contains the unit designation definitions
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef UNITDESG_H
#define UNITDESG_H

//***************************************************************************

inline constexpr int OBJ_ID_FIRST_COMMANDER = 492;
inline constexpr int OBJ_ID_LAST_COMMANDER = 499;

inline constexpr int OBJ_ID_FIRST_TEAM = 500;
inline constexpr int OBJ_ID_LAST_TEAM = 508;

//#define	OBJ_ID_FIRST_GROUP
inline constexpr int PlayerId = 500;
inline constexpr int ClanId = 501;
inline constexpr int AlliedId = 502;

inline constexpr int PlayerLance0 = 16;
inline constexpr int PlayerLance1 = 17;
inline constexpr int PlayerLance2 = 18;
inline constexpr int PlayerLance3 = 19;
inline constexpr int PlayerLance4 = 20;
inline constexpr int PlayerLance5 = 21;
inline constexpr int PlayerLance6 = 22;
inline constexpr int PlayerLance7 = 23;

inline constexpr int ClanStar0 = 165;
inline constexpr int ClanStar1 = 166;
inline constexpr int ClanStar2 = 167;
inline constexpr int ClanStar3 = 168;
inline constexpr int ClanStar4 = 169;
inline constexpr int ClanStar5 = 170;
inline constexpr int ClanStar6 = 171;
inline constexpr int ClanStar7 = 172;

inline constexpr int AlliedLance0 = 329;
inline constexpr int AlliedLance1 = 330;
inline constexpr int AlliedLance2 = 331;
inline constexpr int AlliedLance3 = 332;
inline constexpr int AlliedLance4 = 333;
inline constexpr int AlliedLance5 = 334;
inline constexpr int AlliedLance6 = 335;
inline constexpr int AlliedLance7 = 336;


inline constexpr int MIN_UNIT_PART_ID = 1;
inline constexpr int MAX_UNIT_PART_ID = 511;

inline constexpr int MIN_TEAM_PART_ID = 500;			//Teams are specialized units...
inline constexpr int MAX_TEAM_PART_ID = 511;

inline constexpr int MIN_MOVER_PART_ID = 512;		//Movers now start here
inline constexpr int MIN_REINFORCEMENT_PART_ID = 2050;	//Reinforcements start here...
inline constexpr int MAX_REINFORCEMENT_PART_ID = 3550;	//Only 1500 reinforcements allowed...
inline constexpr int MAX_MOVER_PART_ID = 4095;	//MIN_MOVER_PART_ID +
											//MAX_COMMANDERS * MAX_MOVERGROUPS * MAX_MOVERGROUP_COUNT_START

#define	MIN_TERRAIN_PART_ID		4096
inline constexpr int MAX_TERRAIN_PART_ID = 464895;		//MIN_TERRAIN_PART_ID +
											//block * (MAX_TERRAIN_BLOCK_VERTICES * MAX_TERRAIN_TILE_ITEMS) +
											//vertex * MAX_TERRAIN_TILE_ITEMS +
											//item

inline constexpr int MIN_TRAIN_PART_ID = 512000;
inline constexpr int MAX_TRAIN_PART_ID = 521999;		//MIN_TRAIN_PART_ID +
											//train * MAX_TRAIN_CARS +
											//car

inline constexpr int MIN_CAMERA_DRONE_ID = 525000;
inline constexpr int MAX_CAMERA_DRONE_ID = 525999;		//MIN_CAMERA_DRONE_ID + cameraDrone

#define	MAX_COMMANDERS				8
inline constexpr int MAX_MOVERGROUPS = 16;
inline constexpr int MAX_MOVERGROUP_COUNT_START = 12;
#define MAX_MOVERGROUP_COUNT		16
inline constexpr int MAX_TRAIN_CARS = 100;
inline constexpr int MAX_TERRAIN_BLOCKS = 144;
inline constexpr int MAX_TERRAIN_BLOCK_VERTICES = 400;
inline constexpr int MAX_TERRAIN_TILE_ITEMS = 8;
inline constexpr int MAX_CAMERA_DRONES = 1000;
										
//***************************************************************************

#endif
