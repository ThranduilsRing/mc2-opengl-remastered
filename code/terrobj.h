//---------------------------------------------------------------------------
//
//	terrobj.h -- File contains the Terrain Object Definition
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef TERROBJ_H
#define TERROBJ_H

#ifndef DTERROBJ_H
#include"dterrobj.h"
#endif

#ifndef OBJTYPE_H
#include"objtype.h"
#endif

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef DCARNAGE_H
#include"dcarnage.h"
#endif

#include<gosfx/gosfxheaders.hpp>
//---------------------------------------------------------------------------
// Macro Definitions
//#define NO_RAM_FOR_BUILDING				0xDCDC0006
//#define NO_APPEARANCE_TYPE_FOR_BLD		0xDCDC0007
//#define NO_APPEARANCE_FOR_BLD			0xDCDC0008
//#define APPEARANCE_NOT_VFX_APPEAR		0xDCDC0009

//---------------------------------------------------------------------------

typedef enum {
	TERROBJ_NONE,
	TERROBJ_TREE,
	TERROBJ_BRIDGE,
	TERROBJ_FOREST,
	TERROBJ_WALL_HEAVY,
	TERROBJ_WALL_MEDIUM,
	TERROBJ_WALL_LIGHT,
	NUM_TERROBJ_SUBTYPES
} TerrainObjectSubType;

class TerrainObjectType : public ObjectType {

	public:

		char			subType;
		float			damageLevel;
		long			collisionOffsetX;
		long			collisionOffsetY;
		bool			setImpassable;
		long			xImpasse;
		long			yImpasse;
		float			explDmg;
		float			explRad;
		unsigned long	fireTypeHandle;
		
	public:

		void init (void);
		
		TerrainObjectType (void) {
			ObjectType::init();
			init();
		}
		
		virtual void initMiscTerrObj (long objTypeNum);

		virtual long init (FilePtr objFile, unsigned long fileSize);

		long init (FitIniFilePtr objFile);
		
		~TerrainObjectType (void) {
			destroy();
		}

		float getDamageLevel (void) {
			return(damageLevel);
		}
				
		virtual void destroy (void);
		
		virtual GameObjectPtr createInstance (void);

		virtual float getBurnDmg (void) {
			return(0.0);
		}

		virtual float getBurnTime (void) {
			return(0.0);
		}
		
		virtual bool handleCollision (GameObjectPtr collidee, GameObjectPtr collider);

		virtual bool handleDestruction (GameObjectPtr collidee, GameObjectPtr collider);
};

//---------------------------------------------------------------------------
typedef struct _TerrainObjectData : public GameObjectData
{
	float						damage;
	int32_t						vertexNumber;
	int32_t						blockNumber;
	float						pitchAngle;
	float						fallRate;
	GameObjectWatchID			powerSupply;
	short						cellFootprint[4];
	glm::vec3				vectorFootprint[4];
	short						numSubAreas0;
	short						numSubAreas1;
	short						subAreas0[MAX_SPECIAL_SUB_AREAS];
	short						subAreas1[MAX_SPECIAL_SUB_AREAS];
	unsigned char				listID;

	unsigned char				numCellsCovered;
	short						cellsCovered[81];
} TerrainObjectData;

inline constexpr int MAXLEN_TERRAINOBJECT_NAME = 35;

class TerrainObject : public GameObject {

	public:

		float						damage;
		int32_t						vertexNumber;
		int32_t						blockNumber;
		float						pitchAngle;
		float						fallRate;
		GameObjectWatchID			powerSupply;
		short						cellFootprint[4];
		glm::vec3				vectorFootprint[4];
		short						numSubAreas0;
		short						numSubAreas1;
		std::unique_ptr<short[]>	subAreas0;		// B7o.8a: owned (MAX_SPECIAL_SUB_AREAS entries), released in destroy()
		std::unique_ptr<short[]>	subAreas1;
		unsigned char				listID;
		
		unsigned char				numCellsCovered;
		short*						cellsCovered;
		gosFX::EffectPtr				bldgDustPoofEffect;
			
		static long					cellArray[9];

	public:

		virtual void init (bool create) {
			objectClass = TERRAINOBJECT;
			setFlag(OBJECT_FLAG_JUSTCREATED, true);
			adoptAppearance(NULL);
			vertexNumber = 0;
			blockNumber = 0;
			damage = 0.0;
			pitchAngle = 0.0f;
			fallRate = 0.0f;
			powerSupply = 0;
			cellFootprint[0] = -1;
			cellFootprint[1] = -1;
			cellFootprint[2] = -1;
			cellFootprint[3] = -1;
			numSubAreas0 = 0;
			numSubAreas1 = 0;
			subAreas0 = NULL;
			subAreas1 = NULL;
			listID = 255;
			numCellsCovered = 0;
			cellsCovered = NULL;

			bldgDustPoofEffect = NULL;
		}

	   	TerrainObject (void) : GameObject() {
			init(true);
		}

		~TerrainObject (void) {
			destroy();
		}

		virtual void updateDebugWindow (GameDebugWindow* debugWindow);

		virtual char* getName (void);

		virtual void killFire (void);
		
		void lightOnFire (float timeToBurn);
		
		virtual void destroy (void);
		
		void primeAppearanceForMissionLoad (void);

		virtual long update (void);

		virtual void render (void);

		virtual void renderShadows (void);
		
		virtual void init (bool create, ObjectTypePtr objType);

		virtual long handleWeaponHit (WeaponShotInfoPtr shotInfo, bool addMultiplayChunk = false);

		virtual void setTerrainPosition (const glm::vec3& position, const Stuff::Vector2DOf<long>& numbers);

		virtual void setDamage (long newDamage);		//Damage encodes which groundtile to use, too.

		virtual void setRotation( float rot );
		
		virtual float getDamage (void) 
		{
			return(damage);
		}

		virtual float getDamageLevel (void) 
		{
			return (static_cast<TerrainObjectTypePtr>(getObjectType()))->getDamageLevel();
		}

		virtual float getDestructLevel (void)
		{
			return (getDamageLevel() - damage);
		}

		virtual float getStatusRating (void);

		long getSubType (void) {
			return((static_cast<TerrainObjectTypePtr>(getObjectType()))->subType);
		}

		virtual long kill (void) {
			//Do nothing for now.  Later, Buildings may do something.
			return NO_ERR;
		}
		
		bool isVisible (void);

		virtual long getLineOfSightNodes (long eyeCellRow, long eyeCellCol, long* cells);
		
		virtual bool isTerrainObject (void) {
			return(true);
		}

		virtual void getBlockAndVertexNumber (int& blockNum, int& vertexNum) {
			blockNum = blockNumber;
			vertexNum = vertexNumber;
		}
		
		virtual void rotate (float yaw, float pitch);
		
		virtual float getAppearRadius (void)
		{
			return appearance->getRadius();
		}
		
		virtual void setPowerSupply (GameObjectPtr power)
		{
			powerSupply = power->getWatchID();
		}

		virtual void calcCellFootprint (glm::vec3& pos);

		virtual bool calcAdjacentAreaCell (long moveLevel, long areaID, long& adjRow, long& adjCol);

		void calcSubAreas (long numCells, short cells[MAX_GAME_OBJECT_CELLS][2]);

		void markMoveMap (bool passable);

		void openSubAreas (void);

		void closeSubAreas (void);

		void setSubAreasTeamId (long id);

		virtual void Save (PacketFilePtr file, long packetNum);

		void Load (TerrainObjectData *data);

		void CopyTo (TerrainObjectData *data);

		virtual glm::vec3 getPositionFromHS (long weaponType) 
		{
			//-----------------------------------------
			// Hotspot for buildings is position plus 
			// some Z based on OBB to make Effect visible.
			// If this doesn't work, replace with art defined site.
			glm::vec3 hsPos = position;

			if (appearance)
				hsPos.z += appearance->getTopZ() * 0.5f;

			return(hsPos);
		}

		virtual glm::vec3 getLOSPosition (void) 
		{
			//-----------------------------------------
			// Hotspot for buildings is position plus 
			// some Z based on OBB to make Effect visible.
			//
			// Use THIS position for LOS Calc!!!
			glm::vec3 hsPos = position;

			if (appearance)
				hsPos.z += appearance->getTopZ() * 0.5f;

			return(hsPos);
		}
};

//---------------------------------------------------------------------------
#endif



