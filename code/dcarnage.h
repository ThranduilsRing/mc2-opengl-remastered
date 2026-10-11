//***************************************************************************
//
//	dcarnage.h -- File contains the Carnage Definitions
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DCARNAGE_H
#define DCARNAGE_H

//---------------------------------------------------------------------------

enum class CarnageEnumType {
	CARNAGE_NONE,
	CARNAGE_FIRE,
	CARNAGE_EXPLOSION,
	NUM_CARNAGE_TYPES
};
using enum CarnageEnumType;

class CarnageType;
typedef CarnageType* CarnageTypePtr;

class Carnage;
typedef Carnage* CarnagePtr;

class Light;
typedef Light *LightPtr;

class FireType;
typedef FireType *FireTypePtr;

class ExplosionType;
typedef ExplosionType* ExplosionTypePtr;

class LightType;
typedef LightType *LightTypePtr;

//***************************************************************************

#endif