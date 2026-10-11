//---------------------------------------------------------------------------
//
//	DObjNum.h -- File contains the Basic Game Object Type definitions
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DOBJNUM_H
#define DOBJNUM_H
//---------------------------------------------------------------------------
// Include Files
	
//---------------------------------------------------------------------------
// Macro Definitions
// These are very important.  They are the basis that both the data files and
// game code use to determine who is what kind of thing.  This file must be
// kept current to both data and code.
inline constexpr int INVALID_OBJECT = -1;

inline constexpr int BASETHORMECH = 0x1;
inline constexpr int BASELOKIMECH = 0x2;
inline constexpr int BASEAWESOMEMECH = 0x3;
inline constexpr int RED_LASER = 0x4;
inline constexpr int GREEN_LASER = 0x5;
//#define EXPLOSION				0x6

inline constexpr int BETULA = 0x7;
inline constexpr int PINE = 0x8;
inline constexpr int PALM = 0x9;

inline constexpr int REAL_FARM_BUILDING = 0xA;

inline constexpr int WHITE_SMOKE_GEN = 0xB;
inline constexpr int BLACK_SMOKE_GEN = 0xC;

inline constexpr int BULLET_GEN = 0xD;
inline constexpr int MISSILE_GEN = 0xE;

inline constexpr int BASEULLERMECH = 0xF;
inline constexpr int BASEHUNCHBACKMECH = 0x10;
inline constexpr int BASECOUGARMECH = 0x11;
inline constexpr int BASECATAPULTMECH = 0x12;

inline constexpr int BARNA_A1 = 0x13;
inline constexpr int BARNB_A1 = 0x14;
inline constexpr int BARNC_A1 = 0x15;
inline constexpr int BARND_A1 = 0x16;
inline constexpr int COMM_A1 = 0x17;
inline constexpr int FRMHSA_A1 = 0x18;
inline constexpr int FRMHSB_A1 = 0x19;
inline constexpr int FRMHSD_A1 = 0x1A;
inline constexpr int GRNSTR_A1 = 0x1B;
inline constexpr int HBGEODESIC_A1 = 0x1C;
inline constexpr int HPMPB_A1 = 0x1D;
inline constexpr int HVINDYB_A1 = 0x1E;
inline constexpr int INDYPLANT_A1 = 0x1F;
inline constexpr int LTINDYSIMPLE_A1 = 0x20;
inline constexpr int PWRPLT_A1 = 0x21;
inline constexpr int SILOB_A1 = 0x22;
inline constexpr int STGBOX_A1 = 0x23;
inline constexpr int STRA_A1 = 0x24;
inline constexpr int TOMSILO_A1 = 0x25;
inline constexpr int TRNLDSILOS_A1 = 0x26;

inline constexpr int LASERHIT = 0x27;
inline constexpr int AC20HIT = 0x28;
inline constexpr int MECHDESTRUCT = 0x29;
inline constexpr int BUILDBOOM = 0x2A;

inline constexpr int HBSTRIKER = 0x2B;

inline constexpr int BASECOMMANDOMECH = 0x2C;
inline constexpr int BASECOMMANDO2CMECH = 0x2D;
inline constexpr int BASEFIRESTARTERMECH = 0x2E;
inline constexpr int BASEHOLLANDERMECH = 0x2F;
inline constexpr int BASERAVENMECH = 0x30;
inline constexpr int BASECENTURIONMECH = 0x31;
inline constexpr int BASEJAGERMECHMECH = 0x32;
inline constexpr int BASEATLASMECH = 0x33;
inline constexpr int BASEMADCATMECH = 0x34;
inline constexpr int BASEMASAKARIMECH = 0x35;
inline constexpr int BASEVULTUREMECH = 0x36;
inline constexpr int BASEPUMAMECH = 0x37;

inline constexpr int THORLARM = 0x38;
inline constexpr int THORRARM = 0x39;
inline constexpr int THORDEST = 0x3A;

inline constexpr int WAYPNT_ICON = 0x3B;
inline constexpr int MINE_ICON = 0x3C;
inline constexpr int MOVE_ICON = 0x3D;
inline constexpr int ARTLRY_ICON = 0x3E;
inline constexpr int CRITICAL_HIT = 0x3F;

inline constexpr int BASEFIRESTARTER2MECH = 0x40;
inline constexpr int BASECENTURTION2MECH = 0x41;

inline constexpr int DAMAGE_SMOKE = 0x42;
inline constexpr int PPC_FX = 0x43;

inline constexpr int BARNA_A2 = 0x44;
inline constexpr int BARNB_A2 = 0x45;
inline constexpr int BARNC_A2 = 0x46;
inline constexpr int BARND_A2 = 0x47;
inline constexpr int BARND_A3 = 0x48;
inline constexpr int BARND_A4 = 0x49;
inline constexpr int COMM_A2 = 0x4A;
inline constexpr int COMM_A3 = 0x4B;
inline constexpr int COMM_A4 = 0x4C;
inline constexpr int FRMHSA_A2 = 0x4D;
inline constexpr int FRMHSB_A2 = 0x4E;
inline constexpr int GRNSTR_A2 = 0x4F;
inline constexpr int HBBARRACK_A1 = 0x50;
inline constexpr int HBBARRACK_A2 = 0x51;
inline constexpr int HBBARRACK_A3 = 0x52;
inline constexpr int HBBARRACK_A4 = 0x53;
inline constexpr int HBGEODESIC_A2 = 0x54;
inline constexpr int HBMPERM1_A1 = 0x55;
inline constexpr int HBMPERM1_A2 = 0x56;
inline constexpr int HBMPERM1_A3 = 0x57;
inline constexpr int HBMPERM1_A4 = 0x58;
inline constexpr int HBMPERM2_A1 = 0x59;
inline constexpr int HBMPERM2_A2 = 0x5A;
inline constexpr int HBMPERM2_A3 = 0x5B;
inline constexpr int HBMPERM2_A4 = 0x5C;
inline constexpr int HBTRKBNK_A1 = 0x5D;
inline constexpr int HBTRKBNK_A2 = 0x5E;
inline constexpr int HBTRKBNK_A3 = 0x5F;
inline constexpr int HBTRKBNK_A4 = 0x60;
inline constexpr int HBTRKBUNKER_A1 = 0x61;
inline constexpr int HBTRKBUNKER_A2 = 0x62;
inline constexpr int HBTRKBUNKER_A3 = 0x63;
inline constexpr int HBTRKBUNKER_A4 = 0x64;
inline constexpr int HBWAREHSEA_A1 = 0x65;
inline constexpr int HBWAREHSEA_A2 = 0x66;
inline constexpr int HBWAREHSEA_A3 = 0x67;
inline constexpr int HBWAREHSEA_A4 = 0x68;
inline constexpr int HPMPA_A2 = 0x69;
inline constexpr int HPMPA_A1 = 0x6A;
inline constexpr int HPMPB_A2 = 0x6B;
inline constexpr int HPMPB_A3 = 0x6C;
inline constexpr int HPMPB_A4 = 0x6D;
inline constexpr int HVINDYA_A1 = 0x6E;
inline constexpr int HVINDYA_A2 = 0x6F;
inline constexpr int HVINDYA_A3 = 0x70;
inline constexpr int HVINDYA_A4 = 0x71;
//#define HVINDYB_A1				0x72
inline constexpr int HVINDYB_A2 = 0x73;
inline constexpr int HVINDYB_A3 = 0x74;
inline constexpr int HVINDYB_A4 = 0x75;
inline constexpr int HVINDYC_A1 = 0x76;
inline constexpr int HVINDYC_A2 = 0x77;
inline constexpr int HVINDYC_A3 = 0x78;
inline constexpr int HVINDYC_A4 = 0x79;
inline constexpr int HVINDYD_A1 = 0x7A;
inline constexpr int HVINDYD_A2 = 0x7B;
inline constexpr int INDYPLANT_A2 = 0x7C;
inline constexpr int LTINDYSIMPLE_A2 = 0x7D;
inline constexpr int PWRPLT_A2 = 0x7E;
inline constexpr int PWRPLT_A3 = 0x7F;
inline constexpr int PWRPLT_A4 = 0x80;
inline constexpr int SILOB_A2 = 0x81;
inline constexpr int STGBOX_A2 = 0x82;
inline constexpr int STGBOX_A3 = 0x83;
inline constexpr int STGBOX_A4 = 0x84;
inline constexpr int STRA_A2 = 0x85;
inline constexpr int TRNLDSILOS_A2 = 0x86;
inline constexpr int TRNLDSILOS_A3 = 0x87;
inline constexpr int TRNLDSILOS_A4 = 0x88;
inline constexpr int INDYHOUSE_A1 = 0x89;
inline constexpr int INDYHOUSE_A2 = 0x8A;
inline constexpr int TOMSILO_A2 = 0x8B;
inline constexpr int WTRTWR_A1 = 0x8C;
inline constexpr int WTRTWR_A2 = 0x8D;
inline constexpr int WTRCANS_A1 = 0x8E;
inline constexpr int WTRCANS_A2 = 0x8F;
inline constexpr int URBANA_A1 = 0x90;
inline constexpr int URBANA_A2 = 0x91;
inline constexpr int URBANA_A3 = 0x92;
inline constexpr int URBANA_A4 = 0x93;
inline constexpr int URBANB_A1 = 0x94;
inline constexpr int URBANB_A2 = 0x95;
inline constexpr int URBANB_A3 = 0x96;
inline constexpr int URBANB_A4 = 0x97;
inline constexpr int URBANE_A1 = 0x98;
inline constexpr int URBANE_A2 = 0x99;
inline constexpr int URBANE_A3 = 0x9A;
inline constexpr int URBANE_A4 = 0x9B;
inline constexpr int FRMHSD_A2 = 0x9C;

inline constexpr int ATLARM = 0x9d;
inline constexpr int ATRARM = 0x9e;
inline constexpr int CMLARM = 0x9f;
inline constexpr int CMRARM = 0xa0;
inline constexpr int CNLARM = 0xa1;
inline constexpr int CNRARM = 0xa2;
inline constexpr int FSLARM = 0xa3;
inline constexpr int FSRARM = 0xa4;
inline constexpr int KLARM = 0xa5;
inline constexpr int KRARM = 0xa6;
inline constexpr int MCLARM = 0xa7;
inline constexpr int MCRARM = 0xa8;
inline constexpr int PLARM = 0xa9;
inline constexpr int PRARM = 0xaa;
inline constexpr int AWLARM = 0xab;
inline constexpr int AWRARM = 0xac;
inline constexpr int CGLARM = 0xad;
inline constexpr int CGRARM = 0xae;
inline constexpr int HBLARM = 0xaf;
inline constexpr int HBRARM = 0xb0;
inline constexpr int JMLARM = 0xb1;
inline constexpr int JMRARM = 0xb2;
inline constexpr int LKLARM = 0xb3;
inline constexpr int LKRARM = 0xb4;
inline constexpr int ULLARM = 0xb5;
inline constexpr int ULRARM = 0xb6;

inline constexpr int BIG_ARTLRY = 0xf8;	// 248
inline constexpr int SMALL_ARTLRY = 0xf9;	// 249
inline constexpr int SENSOR_ARTLRY = 0xfa;	// 250

inline constexpr int BIG_STRIKE = 0x1fb;	// 507
inline constexpr int SMALL_STRIKE = 0x1fc;	// 508
inline constexpr int SENSOR_STRIKE = 0x1fd;	// 509

inline constexpr int CAMERA_CALL = 0x204;	// 516
inline constexpr int CAMERA_DRONE = 0x205;	// 517

//---------------------------------------------------------------------------
#endif
