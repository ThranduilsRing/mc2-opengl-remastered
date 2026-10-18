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
using CarnageTypePtr = CarnageType*;

class Carnage;
using CarnagePtr = Carnage*;

class Light;
using LightPtr = Light *;

class FireType;
using FireTypePtr = FireType *;

class ExplosionType;
using ExplosionTypePtr = ExplosionType*;

class LightType;
using LightTypePtr = LightType *;

//***************************************************************************

#endif