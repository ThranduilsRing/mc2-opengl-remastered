//---------------------------------------------------------------------------
//
// DAppear.h -- File contains the Basic Game Appearance Definition
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DAPPEAR_H
#define DAPPEAR_H
//---------------------------------------------------------------------------
// Include Files
	
//---------------------------------------------------------------------------
// Macro definitions
#include "dstd.h"  // NO_ERR

// Shared by the building, ground-vehicle and mech appearances (bdactor, gvactor, mech3d).
inline constexpr float BASE_NODE_RECYCLE_TIME = 0.25f;
inline constexpr int MAX_WEAPON_NODES = 4;
inline constexpr float SPIN_RATE = 90.0f;
inline constexpr float EXPAND_FACTOR = 1.25f;

//---------------------------------------------------------------------------
// Class definitions
class Appearance;
using AppearancePtr = Appearance*;

class GameObject;
using GameObjectPtr = GameObject*;
//---------------------------------------------------------------------------
#endif