#define LOGISTICSCOMPONENT_CPP
/*************************************************************************************************\
LogisticsComponent.cpp			: Implementation of the LogisticsComponent component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/
#include"logisticscomponent.h"
#include"logistics_csv.h"
#include<gameos.hpp>
#include"cmponent.h"

#include"../resource.h"
#include"utilities.h"

#include "platform_str.h"


extern const char* ComponentFormString[];

long LogisticsComponent::XICON_FACTOR = 48;
long LogisticsComponent::YICON_FACTOR = 32;
float LogisticsComponent::MAX_HEAT = 20.f;
float LogisticsComponent::MAX_DAMAGE = 15.f;
float LogisticsComponent::MAX_RECYCLE = 10.75f;
float LogisticsComponent::MAX_RANGE = 3.f;


/*char* LogisticsComponent::ComponentFormString[NUM_COMPONENT_FORMS] = 
{
	"Simple",
	"Cockpit",
	"Sensor",
	"Actuator",
	"Engine",
	"HeatSink",
	"Weapon",]
	"EnergyWeapon",
	"BallsticWeapon",
	"MissileWeapon",
	"Ammo",
	"JumpJet",
	"Case",
	"LifeSuport",
	"Gyroscope",
	"PowerAmplifier",
	"ECM",
	"Probe",
	"Jammer",
	"Bulk",
};*/

LogisticsComponent::LogisticsComponent()
{
	name = NULL;
	ID = -1;
	flavorText = NULL;		
	bHead = bTorso = bLegs = 0;
	iconFileName = 0;
	pictureFileName = 0;
	iconX = iconY = 0;
	bAvailable = 0;
	recycleTime = 0.f;
}

//-------------------------------------------------------------------------------------------------

LogisticsComponent::~LogisticsComponent()
{
	if ( name )
		delete[] name;

	if ( iconFileName )
		delete[] iconFileName;

	if ( pictureFileName )
		delete[] pictureFileName;
}

int LogisticsComponent::init( char* dataLine )
{
	char* line = dataLine;

	char* pLine = line;

	char pBuffer[1025];

	ID = (extractInt( pLine ));

	// the type
	extractString( pLine, pBuffer, 1024 );
    int i = 0;
	for ( ; i < NUM_COMPONENT_FORMS; ++i )
	{
		if ( 0 == S_stricmp( ComponentFormString[i], pBuffer ) )
		{
			Type = i;
			break;
		}
	}

	if ( i == NUM_COMPONENT_FORMS )
		return -1;

	// name, probably aren't going to use this, they should be in the RC.
	extractString( pLine, pBuffer, 1024 );

	// name, probably aren't going to use this, they should be in the RC.
	extractString( pLine, pBuffer, 1024 ); // ignore critical hits

	recycleTime = extractFloat( pLine );
	heat = extractFloat( pLine );
	// weight
	weight = extractFloat( pLine );
	damage = extractFloat( pLine );
	// ignore battle rating
	extractString( pLine, pBuffer, 1024 );
	// cost
	cost = extractInt( pLine );

	// range
	extractString( pLine, pBuffer, 1024 );
	if ( !isWeapon() )
		rangeType = NO_RANGE;
	else if ( !strcmp( pBuffer, "long" ) )
		rangeType = LONG;
	else if ( !strcmp( pBuffer, "medium" ) )
		rangeType = MEDIUM;
	else
		rangeType = SHORT;

	// we need to figure out where things can go
	extractString( pLine, pBuffer, 1024 );
	bHead = S_stricmp( pBuffer, "Yes" ) ? false : true;
	
	extractString( pLine, pBuffer, 1024 );
	bTorso = S_stricmp( pBuffer, "Yes" ) ? false : true;
	
	// ignore the next 4 columns
	for ( i = 0; i < 4; ++i )
		extractString( pLine, pBuffer, 1024 );
	
	extractString( pLine, pBuffer, 1024 );
	bLegs = S_stricmp( pBuffer, "Yes" ) ? false : true;
	
	// ignore the next 4 columns
	for ( i = 0; i < 4; ++i )
		extractString( pLine, pBuffer, 1024 );

	Ammo = extractInt( pLine );

	// now read in icon info
	extractString( pLine, pBuffer, 1024 );
	if ( *pBuffer && (pBuffer[0] != '0') )
	{
		iconFileName = new char[strlen( pBuffer ) + 1];
		strcpy( iconFileName, pBuffer );
	}
	else
		return -1; // fail if no picture

	extractString( pLine, pBuffer, 1024 );
	if ( *pBuffer )
	{
		pictureFileName = new char[strlen( pBuffer ) + 1];	//Forgot the NULL all over the place did we?
		strcpy( pictureFileName, pBuffer );
	}

	stringID = extractInt( pLine );
	helpStringID = extractInt( pLine );
	iconX = extractInt( pLine );
	iconY = extractInt( pLine );
	
	char nameBuffer[256];
	cLoadString( stringID, nameBuffer, 256 );

	name = flavorText = new char[strlen( nameBuffer ) + 1];		//Lets not forget the NULL!!!
	strcpy( name, nameBuffer );

	return ID;
}

// LOGISTICS-CSV-TOKENIZER-FIX-1: bodies delegate to the shared, hardened
// tokenizer in code/logistics_csv.h (fixes the past-NUL cursor walk on malformed
// rows; behavior-preserving on well-formed compbas.csv). The same header is
// exercised game-free by tools/logistics_csv_harness/. Member signatures kept so
// every call site in init() is unchanged.
int LogisticsComponent::extractString( char*& pFileLine, char* pBuffer, int bufferLength )
{
	return logistics_csv::extractField( pFileLine, pBuffer, bufferLength );
}

int LogisticsComponent::extractInt( char*& pFileLine )
{
	return logistics_csv::extractInt( pFileLine );
}

float LogisticsComponent::extractFloat( char*& pFileLine )
{
	return logistics_csv::extractFloat( pFileLine );
}

// EQUIP-ADD-FILL-1: overwrite the string-table-sourced name with the def's
// DisplayName. name/flavorText alias ONE allocation (init() invariant) -- both
// are re-pointed here and the dtor frees `name` only, exactly as after init().
void LogisticsComponent::setDisplayName( const char* n )
{
	if ( !n )
		n = "";
	if ( name )
		delete[] name;
	name = flavorText = new char[strlen( n ) + 1];
	strcpy( name, n );
}

bool LogisticsComponent::compare( LogisticsComponent* second, int type )
{
	switch( type )
	{
		case DAMAGE:
			return second->damage > damage;
			break;
		case WEIGHT:
			return second->weight > damage;
			break;
		case HEAT:
			return second->heat > heat;
			break;
		case NAME:
			return S_stricmp( name, second->name ) > 0;
			break;
		case RANGE:
			return second->damage > damage;
			break;

	}

	return 0;
}

bool LogisticsComponent::isWeapon()
{
	return Type ==	COMPONENT_FORM_WEAPON ||
		Type == COMPONENT_FORM_WEAPON_ENERGY ||
		Type == COMPONENT_FORM_WEAPON_BALLISTIC ||
		Type == COMPONENT_FORM_WEAPON_MISSILE;

}


//*************************************************************************************************
// end of file ( LogisticsComponent.cpp )
