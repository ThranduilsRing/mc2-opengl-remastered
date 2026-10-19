#define MISSION2_CPP
/*************************************************************************************************\
mission2.cpp			: The parts of mission.cpp that we need for the mech viewer
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"mission.h"
#include "render_resource_owners.h"

#include"gos_profiler.h"

std::unique_ptr<MC_TextureManager> ownedTextureManager;
std::unique_ptr<WeaponEffects> ownedWeaponEffects;

bool Mission::statisticsInitialized = 0;



#ifdef LAB_ONLY
__int64 MCTimeABLLoad 			= 0;
__int64 MCTimeMiscToTeamLoad 	= 0; 
__int64 MCTimeTeamLoad 			= 0; 
__int64 MCTimeObjectLoad 		= 0; 
__int64 MCTimeTerrainLoad 		= 0; 
__int64 MCTimeMoveLoad 			= 0; 
__int64 MCTimeMissionABLLoad 	= 0; 
__int64 MCTimeWarriorLoad 		= 0; 
__int64 MCTimeMoverPartsLoad	= 0; 
__int64 MCTimeObjectiveLoad 	= 0; 
__int64 MCTimeCommanderLoad 	= 0; 
__int64 MCTimeMiscLoad 			= 0; 
__int64 MCTimeGUILoad 			= 0; 

__int64 x1;

__int64 MCTimeMultiplayerUpdate = 0;
__int64 MCTimeTerrainUpdate 	= 0;
__int64 MCTimeCameraUpdate 		= 0;
__int64 MCTimeWeatherUpdate 	= 0;
__int64 MCTimePathManagerUpdate = 0;

__int64 MCTimeTerrainGeometry 	= 0; 
__int64 MCTimeCraterUpdate 		= 0; 
__int64 MCTimeTXMManagerUpdate 	= 0; 
__int64 MCTimeSensorUpdate 		= 0; 
__int64 MCTimeLOSUpdate			= 0; 
__int64 MCTimeCollisionUpdate 	= 0; 
__int64 MCTimeMissionScript 	= 0; 
__int64 MCTimeInterfaceUpdate 	= 0; 
__int64 MCTimeMissionTotal 		= 0; 

__int64 MCTimeLOSCalc				= 0;
__int64 MCTimeTerrainObjectsUpdate	= 0;
__int64 MCTimeMechsUpdate			= 0;
__int64 MCTimeVehiclesUpdate		= 0;
__int64 MCTimeTurretsUpdate			= 0;
__int64 MCTimeAllElseUpdate			= 0;

__int64 MCTimeTerrainObjectsTL		= 0;
__int64 MCTimeMechsTL				= 0;
__int64 MCTimeVehiclesTL			= 0;
__int64 MCTimeTurretsTL				= 0;

__int64 x                       = 0;
__int64 MCTimeAnimationCalc     = 0;
__int64 MCTimeRunBrainUpdate    = 0;
__int64 MCTimePath1Update       = 0;
__int64 MCTimePath2Update       = 0;
__int64 MCTimePath3Update       = 0;
__int64 MCTimePath4Update       = 0;
__int64 MCTimePath5Update       = 0;
__int64 MCTimeCalcPath1Update   = 0;
__int64 MCTimeCalcPath2Update   = 0;
__int64 MCTimeCalcPath3Update   = 0;
__int64 MCTimeCalcPath4Update   = 0;
__int64 MCTimeCalcPath5Update   = 0;
__int64 MCTimeCalcGoal1Update   = 0;
__int64 MCTimeCalcGoal2Update   = 0;
__int64 MCTimeCalcGoal3Update   = 0;
__int64 MCTimeCalcGoal4Update   = 0;
__int64 MCTimeCalcGoal5Update   = 0;
__int64 MCTimeCalcGoal6Update   = 0;

extern __int64 MCTimeTerrainGeometry 	; 
extern __int64 MCTimeCraterUpdate 		; 
extern __int64 MCTimeTXMManagerUpdate 	; 
extern __int64 MCTimeSensorUpdate 		; 
extern __int64 MCTimeLOSUpdate			; 
extern __int64 MCTimeCollisionUpdate 	; 
extern __int64 MCTimeMissionScript 	; 
extern __int64 MCTimeInterfaceUpdate 	; 
extern __int64 MCTimeMissionTotal; 

extern __int64 MCTimeLOSCalc;

extern float OneOverProcessorSpeed;
#endif


void Mission::initBareMinimum()
{
	ZoneScopedN("Mission::initBareMinimum");

	long result = 0;

	if ( !mcTextureManager )
	{
		ownedTextureManager = std::make_unique_for_overwrite<MC_TextureManager>();
		mcTextureManager = ownedTextureManager.get();
		mcTextureManager->start();

	}

	// Vertex storage covers forests of detailed model overrides.
	mcTextureManager->startVertices(32000000);
	mcTextureManager->startShapes(10000);

	initTGLForLogistics();

	if ( !appearanceTypeList )
	{
		mission->appearanceTypes = std::make_unique<AppearanceTypeList>();
		appearanceTypeList = mission->appearanceTypes.get();
		gosASSERT(appearanceTypeList != nullptr);
		gosASSERT(result == NO_ERR);

	}

	if ( !weaponEffects )
	{
		ownedWeaponEffects = std::make_unique_for_overwrite<WeaponEffects>();
		weaponEffects = ownedWeaponEffects.get();
		weaponEffects->init("Effects");
	}

	if ( statisticsInitialized == 0 )
	{
		initializeStatistics();
	}
}

void Mission::initializeStatistics()
{
#ifdef LAB_ONLY	
	//Add Mission Load statistics to GameOS Debugger screen!
	MCTimeABLLoad       *= OneOverProcessorSpeed;
	MCTimeMiscToTeamLoad*= OneOverProcessorSpeed;
	MCTimeTeamLoad      *= OneOverProcessorSpeed;
	MCTimeObjectLoad    *= OneOverProcessorSpeed;
	MCTimeTerrainLoad   *= OneOverProcessorSpeed;
	MCTimeMoveLoad      *= OneOverProcessorSpeed;
	MCTimeMissionABLLoad*= OneOverProcessorSpeed;
	MCTimeWarriorLoad   *= OneOverProcessorSpeed;
	MCTimeMoverPartsLoad*= OneOverProcessorSpeed;
	MCTimeObjectiveLoad *= OneOverProcessorSpeed;
	MCTimeCommanderLoad *= OneOverProcessorSpeed;
	MCTimeMiscLoad      *= OneOverProcessorSpeed;
	MCTimeGUILoad       *= OneOverProcessorSpeed;
	
	//Add Mission Run statistics to GameOS Debugger screen!
	StatisticFormat( "" );
	StatisticFormat( "MechCommander 2 GameLogic" );
	StatisticFormat( "=========================" );
	StatisticFormat( "" );

	AddStatistic( "Terrain Update",					"%", gos_timedata, static_cast<void*>(&MCTimeTerrainUpdate)		,		0 );
	AddStatistic( "Camera Update",					"%", gos_timedata, static_cast<void*>(&MCTimeCameraUpdate)       ,       0 );
	AddStatistic( "Weather Update",					"%", gos_timedata, static_cast<void*>(&MCTimeWeatherUpdate)      ,       0 );
	AddStatistic( "PathManager Update",				"%", gos_timedata, static_cast<void*>(&MCTimePathManagerUpdate)  ,       0 );
	AddStatistic( "Terrain Geometry",				"%", gos_timedata, static_cast<void*>(&MCTimeTerrainGeometry)    ,       0 );
	AddStatistic( "Interface Update",				"%", gos_timedata, static_cast<void*>(&MCTimeInterfaceUpdate)    ,       0 );
	AddStatistic( "Crater Update",					"%", gos_timedata, static_cast<void*>(&MCTimeCraterUpdate)       ,       0 );
	AddStatistic( "TXM Mgr Update",					"%", gos_timedata, static_cast<void*>(&MCTimeTXMManagerUpdate)   ,       0 );
	AddStatistic( "Sensor Update",					"%", gos_timedata, static_cast<void*>(&MCTimeSensorUpdate)       ,       0 );
	AddStatistic( "LOS Update",						"%", gos_timedata, static_cast<void*>(&MCTimeLOSUpdate)			,       0 );
	AddStatistic( "Collision Update",				"%", gos_timedata, static_cast<void*>(&MCTimeCollisionUpdate)    ,       0 );
	AddStatistic( "Mission Script",					"%", gos_timedata, static_cast<void*>(&MCTimeMissionScript)      ,       0 );
	AddStatistic( "Multiplayer Update",				"%", gos_timedata, static_cast<void*>(&MCTimeMultiplayerUpdate)  ,       0 );
	StatisticFormat( "=========================" );
	AddStatistic( "Total Mission Time", 			"%", gos_timedata, static_cast<void*>(&MCTimeMissionTotal)		,       0 );
	StatisticFormat( "=========================" );
	AddStatistic( "Total LOS Calc Time", 			"%", gos_timedata, static_cast<void*>(&MCTimeLOSCalc)    		,       0 );

	statisticsInitialized = true;

	TerrainTextures::initializeStatistics();

#endif
}


void Mission::initTGLForLogistics()
{
	// A mission-ending flash must not carry into logistics.
	TG_Shape::lighteningLevel = 0;

	if (colorPool || vertexPool || facePool || shadowPool || trianglePool)
	{
		if (colorPool)
		{
			colorPool->destroy();
			delete colorPool;
			colorPool = nullptr;
		}
		
		if (vertexPool)
		{
			vertexPool->destroy();
			delete vertexPool;
			vertexPool = nullptr;
		}

		if (facePool)
		{
			facePool->destroy();
			delete facePool;
			facePool = nullptr;
		}

		if (shadowPool)
		{
			shadowPool->destroy();
			delete shadowPool;
			shadowPool = nullptr;
		}

		if (trianglePool)
		{
			trianglePool->destroy();
			delete trianglePool;
			trianglePool = nullptr;
		}
	}

	{
		// Pools cover one large preview object, including its shadow and Sensor shapes.
		colorPool 		= new TG_VertexPool;
		colorPool->init(64000);

		vertexPool 		= new TG_GOSVertexPool;
		vertexPool->init(64000);

		facePool 		= new TG_DWORDPool;
		facePool->init(128000);

		shadowPool 		= new TG_ShadowPool;
		shadowPool->init(64000);

		trianglePool 	= new TG_TrianglePool;
		trianglePool->init(64000);
	}
}



//*************************************************************************************************
// end of file ( mission2.cpp )
