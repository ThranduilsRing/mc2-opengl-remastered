//---------------------------------------------------------------------------
//
// DAprType.h -- File contains the Basic Game Appearance Type Definitions
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DAPRTYPE_H
#define DAPRTYPE_H
//---------------------------------------------------------------------------
// Include Files

//---------------------------------------------------------------------------
// Macro definitions
inline constexpr long BASE_APPEARANCE = 0L;
inline constexpr int SPRITE_TREE = 0x01;
inline constexpr int VFX_APPEAR = 0x02;
inline constexpr int FSY_APPEAR = 0x03;
inline constexpr int LINE_APPEAR = 0x04;
inline constexpr int GV_TYPE = 0x05;
inline constexpr int ARM_APPEAR = 0x06;
inline constexpr int BUILD_APPEAR = 0x07;
inline constexpr int ELM_TREE = 0x08;
inline constexpr int PU_TYPE = 0x09;
inline constexpr int SMOKE_TYPE = 0x0a;
inline constexpr int POLY_APPEARANCE = 0x0b;
inline constexpr int MLR_APPEARANCE = 0x0c;
inline constexpr int MECH_TYPE = 0x0d;
inline constexpr int BLDG_TYPE = 0x0e;
inline constexpr int TREED_TYPE = 0x0f;

inline constexpr int BUILDING_APPR_TYPE = 0x10;
inline constexpr int TREE_APPR_TYPE = 0x11;
inline constexpr int VEHICLE_APPR_TYPE = 0x12;
inline constexpr int MECH_APPR_TYPE = 0x13;
inline constexpr int GENERIC_APPR_TYPE = 0x14;

inline constexpr int MAX_LODS = 3;  // appearance LOD slots (apprtype.h, camera.h)
//---------------------------------------------------------------------------
// Class definitions

class AppearanceType;
using AppearanceTypePtr = AppearanceType*;

class AppearanceTypeList;
using AppearanceTypeListPtr = AppearanceTypeList*;

//---------------------------------------------------------------------------
#endif