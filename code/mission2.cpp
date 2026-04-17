#define MISSION2_CPP
/*************************************************************************************************\
mission2.cpp			: The parts of mission.cpp that we need for the mech viewer
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"mission.h"

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

extern __int64 x;

extern float OneOverProcessorSpeed;
#endif


void Mission::initBareMinimum()
{

	long result = 0;

	if ( !mcTextureManager )
	{
		mcTextureManager = new MC_TextureManager;
		mcTextureManager->start();

	}

	//Startup the vertex array pool
	mcTextureManager->startVertices(500000);
	mcTextureManager->startShapes(10000);

	initTGLForLogistics();

	//----------------------------------------------
	// Start Appearance Type Lists.
	unsigned long spriteHeapSize = 3072000;
	if ( !appearanceTypeList )
	{
		appearanceTypeList = new AppearanceTypeList;
		gosASSERT(appearanceTypeList != NULL);
		appearanceTypeList->init(spriteHeapSize);
		gosASSERT(result == NO_ERR);

	}

	if ( !weaponEffects )
	{
		weaponEffects = new WeaponEffects;
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

	AddStatistic( "Terrain Update",					"%", gos_timedata, (void*)&MCTimeTerrainUpdate		,		0 );
	AddStatistic( "Camera Update",					"%", gos_timedata, (void*)&MCTimeCameraUpdate       ,       0 );
	AddStatistic( "Weather Update",					"%", gos_timedata, (void*)&MCTimeWeatherUpdate      ,       0 );
	AddStatistic( "PathManager Update",				"%", gos_timedata, (void*)&MCTimePathManagerUpdate  ,       0 ); 
	AddStatistic( "Terrain Geometry",				"%", gos_timedata, (void*)&MCTimeTerrainGeometry    ,       0 ); 
	AddStatistic( "Interface Update",				"%", gos_timedata, (void*)&MCTimeInterfaceUpdate    ,       0 ); 
	AddStatistic( "Crater Update",					"%", gos_timedata, (void*)&MCTimeCraterUpdate       ,       0 ); 
	AddStatistic( "TXM Mgr Update",					"%", gos_timedata, (void*)&MCTimeTXMManagerUpdate   ,       0 ); 
	AddStatistic( "Sensor Update",					"%", gos_timedata, (void*)&MCTimeSensorUpdate       ,       0 ); 
	AddStatistic( "LOS Update",						"%", gos_timedata, (void*)&MCTimeLOSUpdate			,       0 );
	AddStatistic( "Collision Update",				"%", gos_timedata, (void*)&MCTimeCollisionUpdate    ,       0 ); 
	AddStatistic( "Mission Script",					"%", gos_timedata, (void*)&MCTimeMissionScript      ,       0 ); 
	AddStatistic( "Multiplayer Update",				"%", gos_timedata, (void*)&MCTimeMultiplayerUpdate  ,       0 );
	StatisticFormat( "=========================" );
	AddStatistic( "Total Mission Time", 			"%", gos_timedata, (void*)&MCTimeMissionTotal		,       0 );
	StatisticFormat( "=========================" );
	AddStatistic( "Total LOS Calc Time", 			"%", gos_timedata, (void*)&MCTimeLOSCalc    		,       0 ); 

	statisticsInitialized = true;

	HeapList::initializeStatistics();
	TerrainTextures::initializeStatistics();

#endif
}


void Mission::initTGLForLogistics()
{
	//---------------------------------------------------------
	unsigned long tglHeapSize = 4 * 1024 * 1024;

	//---------------------------------------------------------
	//Reset the lightening in case they exitted with a flash on screen!!
	TG_Shape::lighteningLevel = 0;

	//---------------------------------------------------------
	// End the Tiny Geometry Layer Heap for the Mission
	if (TG_Shape::tglHeap)
	{
		//Shut down the TGL RAM pools.
		if (colorPool)
		{
			colorPool->destroy();
			delete colorPool;
			colorPool = NULL;
		}
		
		if (vertexPool)
		{
			vertexPool->destroy();
			delete vertexPool;
			vertexPool = NULL;
		}

		if (facePool)
		{
			facePool->destroy();
			delete facePool;
			facePool = NULL;
		}

		if (shadowPool)
		{
			shadowPool->destroy();
			delete shadowPool;
			shadowPool = NULL;
		}

		if (trianglePool)
		{
			trianglePool->destroy();
			delete trianglePool;
			trianglePool = NULL;
		}
		
		TG_Shape::tglHeap->destroy();

		delete TG_Shape::tglHeap;
		TG_Shape::tglHeap = NULL;
	}

	//------------------------------------------------------
	// Start the Tiny Geometry Layer Heap for Logistics
	if ( !TG_Shape::tglHeap )
	{
		TG_Shape::tglHeap = new UserHeap;
		TG_Shape::tglHeap->init(tglHeapSize,"TinyGeom");
		
		//Start up the TGL RAM pools.
		colorPool 		= new TG_VertexPool;
		colorPool->init(2000);
		
		vertexPool 		= new TG_GOSVertexPool;
		vertexPool->init(2000);
		
		facePool 		= new TG_DWORDPool;
		facePool->init(4000);
		
		shadowPool 		= new TG_ShadowPool;
		shadowPool->init(2000);
		
		trianglePool 	= new TG_TrianglePool;
		trianglePool->init(2000);
	}
}



//*************************************************************************************************
// end of file ( mission2.cpp )
