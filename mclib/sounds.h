//---------------------------------------------------------------------------
// sounds.h - This file is the list of available sound bites
//
// MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef SOUNDS_H
#define SOUNDS_H
//---------------------------------------------------------------------------
// Macro Definitions

inline constexpr int BOMB_EXPLODE = 0;		//Should never be used
inline constexpr int ER_LASER_FIRE = 1;		//ER Laser Fire
inline constexpr int BIGDEATH = 2;		//Mech Explosion
inline constexpr int LASERHITS = 3;		//All other Laser Fire/Hit
inline constexpr int MECH_COLLIDE = 4;		//Mech Collisions
inline constexpr int LRM_HIT = 5;		//LRM Hit
inline constexpr int FLAMER_FIRE = 6;		//Flamer Fire
inline constexpr int AC2_FIRE = 7;		//AutoCannon Fire
inline constexpr int LRM_FIRE = 8;		//Lrm Fire
inline constexpr int SRM_HIT = 9;		//SRM Hit
inline constexpr int PPC_FIRE = 10;		//PPC Fire
inline constexpr int SRM_FIRE = 11;		//SRM Fire
inline constexpr int GAUSS_FIRE = 12;		//Gauss Rifle Fire
inline constexpr int FOOTFALL_SMALL = 13;		//Mech Footstep
inline constexpr int TREEFALL = 14;		//TreeFall
inline constexpr int BUTTON5 = 15;		//GUI SFX
inline constexpr int BUTTON7 = 16;		//GUI SFX
inline constexpr int BUTTON6 = 17;		//GUI SFX
inline constexpr int BUZZER1 = 18;		//GUI SFX
inline constexpr int CRITICAL_HIT_SFX = 19;		//Mech Critical Hit
inline constexpr int PING_SFX = 20;		//Sensor Contact 
inline constexpr int MGUN_FIRE = 21;		//Machine Gun Fire
inline constexpr int OBJECT_FIRE = 22;		//Burning Fire
inline constexpr int ER_LASER_HIT = 23;		//ER Laser Hit
inline constexpr int GAUSS_HIT = 24;		//Gauss Rifle Hit
inline constexpr int AC2_HIT = 25;		//AutoCannon Hit
inline constexpr int MGUN_HIT = 26;		//Machine Gun Hit
inline constexpr int EJECT_SFX = 27;		//Eject
inline constexpr int TREEFALL1 = 28;		//Treefall
inline constexpr int MECH_FALL_SOUND = 29;		//Mech Fall Sound
inline constexpr int ELEMENTAL_HIT = 30;		//Elemental Shredding a Mech Sound
inline constexpr int GENERAL_ALARM_SFX = 34;		//Perimeter alarm general alarm
inline constexpr int BUILDING_EXPLODE = 35;
inline constexpr int ELECTRIC_HUM = 36;
inline constexpr int GENERATOR_HUM = 37;
inline constexpr int RADAR_HUM = 38;
inline constexpr int HEAVY_ROTATION = 39;
inline constexpr int ORBITAL_GUN = 40;
inline constexpr int DEFENSE_SILO = 41;
inline constexpr int GATE_OPEN = 42;
inline constexpr int TRAIN_XING = 43;
inline constexpr int HPG_ANTENNA = 44;
inline constexpr int GATE_CLOSE = 42;
inline constexpr int INVALID_GUI = 46;
inline constexpr int ZOOM_GUI = 47;
inline constexpr int FORCE_GUI = 48;
inline constexpr int SENSOR_GUI = 49;
inline constexpr int MAPPLACE_GUI = 50;
inline constexpr int MAPBUTTONS_GUI = 51;
inline constexpr int CLICKOFF_GUI = 52;
inline constexpr int CLICKON_GUI = 53;
inline constexpr int FOLDER_GUI = 54;
inline constexpr int OUTSIDESELECT_GUI = 55;
inline constexpr int PAGEGUI = 56;
inline constexpr int SELECT_GUI = 57;
inline constexpr int STARTMISSION_GUI = 58;
inline constexpr int TAB_GUI = 59;
inline constexpr int POWERDOWN_SFX = 60;
inline constexpr int POWERUP_SFX = 61;
inline constexpr int INCOMING_ARTILLIRY = 62;
inline constexpr int INCOMING_AIRSTRIKE = 63;
inline constexpr int WINDOW_OPEN = 64;
inline constexpr int WINDOW_CLOSE = 65;
inline constexpr int START_GROUPSELECT = 66;
inline constexpr int END_GROUPSELECT = 67;
inline constexpr int ZOOMIN_GUI = 68;
inline constexpr int ZOOMOUT_GUI = 69;
inline constexpr int WRONG_GUI = 70;
inline constexpr int FRP_BUTTON_GUI = 71;
inline constexpr int BREAKINGFENCE = 72;
inline constexpr int BUILDING_COLLAPSE = 73;
inline constexpr int VTOL_ANIMATE = 74;
inline constexpr int VTOL_DROP = 75;
inline constexpr int JUMPJETS = 76;
inline constexpr int THUNDER1 = 77;
inline constexpr int THUNDER2 = 78;
inline constexpr int THUNDER3 = 79;
inline constexpr int THUNDER4 = 80;
inline constexpr int RAIN1 = 81;
inline constexpr int RAIN2 = 82;
inline constexpr int RAIN3 = 83;
inline constexpr int RAIN4 = 84;
inline constexpr int SENSOR1 = 85;
inline constexpr int SENSOR2 = 86;
inline constexpr int SENSOR3 = 87;
inline constexpr int SENSOR4 = 88;
inline constexpr int LOG_WRONGBUTTON = 89;
inline constexpr int LOG_VIDEOBUTTONS = 90;
inline constexpr int LOG_STARTMISSION = 91;
inline constexpr int LOG_SELECT = 90;	// changing to be the same as video buttons
inline constexpr int LOG_NEXTBACKBUTTONS = 93;	
inline constexpr int LOG_MISSIONSELECT = 94;
inline constexpr int LOG_MAINMENUBUTTON = 95;
inline constexpr int LOG_HIGHLIGHTBUTTONS = 96;
inline constexpr int LOG_EXITGAME = 97;
inline constexpr int LOG_CLICKONBUTTON = 98;
inline constexpr int MINE_EXPLOSION = 99;
inline constexpr int LOG_DIGITALHIGHLIGHT = 96;
inline constexpr int REPAIRBAY_FX = 101;
inline constexpr int LOG_PILOTMEDAL = 102;
inline constexpr int LOG_KILLMARKER = 103;
inline constexpr int LOG_PROMOTED = 104;
inline constexpr int MECH_EXPLODE = 105;
inline constexpr int COPTER_POWERUP = 106;
inline constexpr int COPTER_POWERDN = 107;
inline constexpr int VEHICLE_POWERUP = 108;
inline constexpr int VEHICLE_POWERDN = 109;
inline constexpr int LOAD_DOORS_CLOSING = 110;
inline constexpr int LOAD_DOORS_OPENING = 111;
inline constexpr int LOG_TEAM_CHAT = 112;
inline constexpr int LOG_CHAT = 113;
inline constexpr int HELICOPTER_DEATH = 114;
inline constexpr int LOG_UNREADY = 16;
inline constexpr int SALVAGE_CRAFT = 116;

//---------------------------------------------------------------------------

inline constexpr int MISSION_START_0 = 0;		//Mission Start Tune
inline constexpr int MISSION_START_1 = 1;		//Mission Start Tune
inline constexpr int MISSION_START_2 = 2;		//Mission Start Tune
inline constexpr int MISSION_START_3 = 3;		//Mission Start Tune
inline constexpr int MISSION_START_4 = 4;		//Mission Start Tune
inline constexpr int NUM_MISSION_TUNES = 5;

inline constexpr int CONTACT_TUNE_0 = 5;
inline constexpr int CONTACT_TUNE_1 = 6;
inline constexpr int CONTACT_TUNE_2 = 7;
inline constexpr int NUM_CONTACT_TUNES = 3;

inline constexpr int WIN_TUNE_0 = 8;
inline constexpr int LOSE_TUNE_0 = 9;

inline constexpr int OBJECTIVE_SUCCESS = 10;
inline constexpr int OBJECTIVE_FAILURE = 11;

inline constexpr int ENEMY_DONE = 12;
inline constexpr int FRIENDLY_DONE = 13;

inline constexpr int COMBAT_TUNE_0 = 14;		//Combat Tune
inline constexpr int COMBAT_TUNE_1 = 15;		//Combat Tune
inline constexpr int COMBAT_TUNE_2 = 16;		//Combat Tune
inline constexpr int COMBAT_TUNE_3 = 17;		//Combat Tune
inline constexpr int COMBAT_TUNE_4 = 18;		//Combat Tune
inline constexpr int COMBAT_TUNE_5 = 19;		//Combat Tune
inline constexpr int NUM_COMBAT_TUNES = 6;

inline constexpr int GAME_FINALE = 20;		//Played when no other music is playing
inline constexpr int BACKGROUND_AMBIENT = 21;		//Played when no other music is playing

inline constexpr int LOGISTICS_LOOP = 22;
inline constexpr int CREDITS_TUNE = 23;

inline constexpr int RESULTS_LOOP = 28;

//---------------------------------------------------------------------------
inline constexpr int BETTY_DROP_WEIGHT_EXCEEDED = 0;
inline constexpr int BETTY_AIR_SUPERIORITY = 1;
inline constexpr int BETTY_COMPONENTS_CAPTURED = 2;
inline constexpr int BETTY_CANNOT_COMP_OBJ = 3;
inline constexpr int BETTY_OBJECTIVE_COMPLETE = 4;
inline constexpr int BETTY_NEW_LEADER = 5;
inline constexpr int BETTY_RESOURCE_CAPTURED = 6;
inline constexpr int BETTY_THIRTY_LEFT = 7;
inline constexpr int BETTY_TWO_MIN_LEFT = 8;
inline constexpr int BETTY_PERIMETER_BREACH = 9;
inline constexpr int BETTY_LOW_DROP_WEIGHT = 10;
inline constexpr int BETTY_MISSION_LOST = 11;
inline constexpr int BETTY_PRI_OBJ_1_WON = 12;
inline constexpr int BETTY_PRI_OBJ_2_WON = 13;
inline constexpr int BETTY_PRI_OBJ_3_WON = 14;
inline constexpr int BETTY_SEC_OBJ_1_WON = 15;
inline constexpr int BETTY_SEC_OBJ_2_WON = 16;
inline constexpr int BETTY_SEC_OBJ_3_WON = 17;
inline constexpr int BETTY_MISSION_WON = 18;
inline constexpr int BETTY_DEPLOY_MSG = 19;
inline constexpr int BETTY_REPAIR_COMPLETE = 20;
inline constexpr int BETTY_REPAIR_INCOMPLETE = 21;
inline constexpr int BETTY_ARTILLERY_AVAILABLE = 22;
inline constexpr int BETTY_SENSOR_PROBE_AVAIL = 23;
inline constexpr int BETTY_CAMERA_DRONE_AVAIL = 24;
inline constexpr int BETTY_NEW_CAMPAIGN = 25;
inline constexpr int BETTY_OP_VIDEO = 26;
inline constexpr int BETTY_BAD_FORCE_SELECTION = 27;
inline constexpr int BETTY_NEW_EQUIPMENT = 28;
inline constexpr int BETTY_NEW_COMPONENT = 29;
inline constexpr int BETTY_NEW_MECH = 30;
inline constexpr int BETTY_NEW_PILOT = 31;
inline constexpr int BETTY_NEW_MECHWARRIOR = 32;
inline constexpr int BETTY_NEW_VEHICLE = 33;
inline constexpr int BETTY_SENS_TWR_CONTACT = 34;
inline constexpr int BETTY_SENS_TWR_DESTROYED = 35;
inline constexpr int BETTY_PERIMETER_ALARM_DEST = 36;
inline constexpr int BETTY_SECOND_OBJ_NO_COMP = 37;
inline constexpr int BETTY_POWERUP = 38;
inline constexpr int BETTY_RESOURCES = 39;
inline constexpr int BETTY_GUARDATTACK = 40;
inline constexpr int BETTY_GUARDDESSTROYED = 41;
inline constexpr int BETTY_PROMOREG = 42;
inline constexpr int BETTY_PROMOVET = 43;
inline constexpr int BETTY_PROMOELI = 44;
inline constexpr int BETTY_PROMOACE = 45;
inline constexpr int BETTY_NEW_MECHS = 46;
inline constexpr int BETTY_NEW_PILOTS = 47;	
inline constexpr int BETTY_NEW_WEAPONS = 48;
inline constexpr int BETTY_BUILDING_RECAPTURED = 49;
inline constexpr int BETTY_GENERATOR_DESTROYED = 50;
inline constexpr int BETTY_REPAIR_GONE = 51;
inline constexpr int BETTY_MINES_GONE = 52;
inline constexpr int BETTY_SENSOR_CAPTURED = 53;


inline constexpr int MAX_BETTY_SAMPLES = 54;

//---------------------------------------------------------------------------
inline constexpr int SUPPORT_AIRSTRIKE = 0;
inline constexpr int SUPPORT_MINELAYER = 1;
inline constexpr int SUPPORT_PROBE = 2;
inline constexpr int SUPPORT_RECOVER = 3;
inline constexpr int SUPPORT_REPAIR = 4;
inline constexpr int SUPPORT_SCOUT = 5;
inline constexpr int SUPPORT_ARTILLERY = 6;

inline constexpr int MAX_SUPPORT_SAMPLES = 7;

//---------------------------------------------------------------------------
inline constexpr int ACKNOWLEDGE = 2;
inline constexpr int CALLSIGN = 10;
//---------------------------------------------------------------------------
#endif
