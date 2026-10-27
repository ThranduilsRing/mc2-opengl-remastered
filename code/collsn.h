//******************************************************************************
//	collsn.h - This file contains the Collision Detection System
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef COLLSN_H
#define COLLSN_H
//------------------------------------------------------------------------------
// Include Files
#ifndef MCLIB_H
#include"mclib.h"
#endif

#include<memory>
#include<vector>

#ifndef DCOLLSN_H
#include"dcollsn.h"
#endif

#ifndef DGAMEOBJ_H
#include"dgameobj.h"
#endif

//------------------------------------------------------------------------------
// Macro Definitions
#ifndef NO_ERR
#define NO_ERR	0
#endif

//------------------------------------------------------------------------------
// classes
struct CollisionGridNode
{
	GameObjectPtr			object;
	CollisionGridNodePtr	next;
};

//------------------------------------------------------------------------------
struct CollisionAlertRecord
{
	GameObjectPtr			object1;
	GameObjectPtr			object2;
	float					currentDistance;
	float					timeToImpact;
};

using CollisionAlertRecordPtr = CollisionAlertRecord *;
//------------------------------------------------------------------------------
class GlobalCollisionAlert
{
	//Data Members
	//-------------
	protected:
		std::vector<CollisionAlertRecord>	alertStore;		//Owns the records collisionAlerts views.
		CollisionAlertRecordPtr		collisionAlerts;
		unsigned long				maxAlerts;
		unsigned long				nextRecord;
		
	public:
		GlobalCollisionAlert (void)
		{
			collisionAlerts = nullptr;
			maxAlerts = 0;
		}

		long init (unsigned long maxCollisionAlerts);
		
		void destroy (void);
		
		~GlobalCollisionAlert (void)
		{
			destroy();
		}
		
		long addRecord (GameObjectPtr obj1, GameObjectPtr obj2, float distance, float time);
		
		CollisionAlertRecordPtr findAlert (GameObjectPtr object, CollisionAlertRecordPtr startRecord = nullptr);
		
		void purgeRecords (void);
};

extern GlobalCollisionAlert *globalCollisionAlert;

//------------------------------------------------------------------------------
class CollisionGrid
{
	//Data Members
	//-------------
	protected:
		unsigned long			xGridWidth;			//Number of gridNodes in x direction
		unsigned long			yGridWidth;			//Number of gridNodes in y direction
													//In theory we would need a z but not for a mech game!!
		
		unsigned long			maxGridRadius;		//Max radius in (m) of each grid node.
		
		struct Placed { unsigned long cell; GameObjectPtr object; };
		std::vector<Placed>			placed;			//Grid objects, insertion order.
		std::vector<GameObjectPtr>	giants;			//Objects larger than maxGridRadius, insertion order.
		std::vector<unsigned long>	cellStart;		//CSR offsets into 'ordered', one per cell plus one.
		std::vector<GameObjectPtr>	ordered;		//Cell-major; within a cell, newest first (the legacy list order).
		
		glm::vec3			gridOrigin;			//Center point of the grid.
		
		bool					gridIsGo;			//Have we already allocated everything?
		
		float					gridXOffset;
		float 					gridYOffset;
		
		float 					gridXCheck;
		float 					gridYCheck;
		
	//Member Functions
	//-----------------
	public:

		
		void init (void)
		{
			xGridWidth = yGridWidth = 0;
			
			maxGridRadius = 0;
			
			gridOrigin = glm::vec3(0.0f);
			
			gridIsGo = FALSE;
		}
		
		CollisionGrid (void)
		{
			init();
		}
		
		long init (glm::vec3 &newOrigin);
		
		void destroy (void);
		
		~CollisionGrid (void)
		{
			destroy();
		}
		
		long add (unsigned long gridIndex, GameObjectPtr object);
		long add (GameObjectPtr object);
		
		void traverse (void);
		void createGrid (void);		//Put all objects in world into grids
		
		void checkGrid (GameObjectPtr object, const GameObjectPtr* first, const GameObjectPtr* last);	//Check each object against grid
};

//------------------------------------------------------------------------------
struct CollisionRecord
{
	GameObjectPtr		obj1;		//Which objects hit each other
	GameObjectPtr		obj2;
	float				time;		//When did they do it relative to current frame.
};
	
//------------------------------------------------------------------------------
class CollisionSystem
{
	//Data Members
	//-------------
	protected:

		std::unique_ptr<CollisionGrid>		collisionGrid;
		
	public:
	
		static unsigned long	xGridSize;
		static unsigned long	yGridSize;
		static unsigned long	gridRadius;
		static unsigned long	maxObjects;
		static unsigned long	maxCollisions;
		static unsigned long	numCollisions;
		
		float					warningDist;		//Distance to worry about short term collision avoidance (in World Units!!)
		float					alertTime;			//Time to worry about short term collision avoidance.
		
	
	//Member Functions
	//-----------------
	protected:

		CollisionRecordPtr findNextPending (void);

	public:
	
		
		void init (void)
		{
			collisionGrid = nullptr;
		}
		
		CollisionSystem (void)
		{
			init();
		}

		long init (FitIniFile *scenarioFile);
				
		void destroy (void);
		
		~CollisionSystem (void)
		{
			destroy();
		}
		
		void checkObjects (void);
		
		static void detectCollision (GameObjectPtr obj1, GameObjectPtr obj2);
		
		void detectStaticCollision (GameObjectPtr obj1, GameObjectPtr obj2);

		float timeToImpact (GameObjectPtr obj1, GameObjectPtr obj2);
		
		static void checkExtents (GameObjectPtr obj1, GameObjectPtr obj2, float time);
};

extern CollisionSystem *collisionSystem;
//------------------------------------------------------------------------------
#endif