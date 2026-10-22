//---------------------------------------------------------------------------
//
//	gvactor.h - This file contains the header for the Ground Vehicle Actor class
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef GVACTOR_H
#define GVACTOR_H

//---------------------------------------------------------------------------
// Include files
#ifndef APPEAR_H
#include"appear.h"
#endif

#ifndef APPRTYPE_H
#include"apprtype.h"
#endif

#ifndef MSL_H
#include"msl.h"
#endif

#include"vis_semantics.h"   // VISIBILITY-SEMANTICS-1

#ifndef OBJECTAPPEARANCE_H
#include"objectappearance.h"
#endif

#include<gosfx/gosfxheaders.hpp>
#include <vector>  // T1.10: GVAppearance::spotlight{Lights,SlotIds,NodeIds}_
//**************************************************************************************
#ifndef NO_ERR
#define NO_ERR						0
#endif

inline constexpr int GV_PART_BODY = 0;
inline constexpr int GV_PART_TURRET = 1;
inline constexpr int NUM_GV_PARTS = 2;

inline constexpr int MAX_GV_ANIMATIONS = 10;
//***********************************************************************
//
// GVAppearanceType
//
//***********************************************************************
class GVAppearanceType : public AppearanceType
{
	public:
	
		TG_TypeMultiShapePtr		gvShape[MAX_LODS];
		float						lodDistance[MAX_LODS];
		
		TG_TypeMultiShapePtr		gvShadowShape;
		
 		TG_TypeMultiShapePtr		gvDmgShape;
		
		char						destructEffect[60];
		
		TG_AnimateShapePtr			gvAnimData[MAX_GV_ANIMATIONS];
		bool						gvAnimLoop[MAX_GV_ANIMATIONS];
		bool						gvReverse[MAX_GV_ANIMATIONS];
		bool						gvRandom[MAX_GV_ANIMATIONS];
		long						gvStartF[MAX_GV_ANIMATIONS];
 		
		char						rotationalNodeId[TG_NODE_ID];
		
		long						numSmokeNodes;		//Where damage smoke comes from.
		long						numWeaponNodes;		//Where weapons fire from.
		long						numFootNodes;		//Where dust trail, contrail comes out of.
		NodeData					*nodeData;			//View of nodeDataStore
		std::unique_ptr<NodeData[]>	nodeDataStore;		//Owns the node table, built once by init(file)
		
		static TG_TypeMultiShapePtr	SensorTriangleShape;
		static TG_TypeMultiShapePtr	SensorCircleShape;
		
	public:
	
		void init (void)
		{
			long i=0;
			for (i=0;i<MAX_LODS;i++)
			{
				gvShape[i] = nullptr;
				lodDistance[i] = 0.0f;
			}
			
			gvShadowShape = nullptr;
			
			gvDmgShape = nullptr;

			for (i=0;i<MAX_GV_ANIMATIONS;i++)
			{
				gvAnimData[i] = nullptr;
				gvAnimLoop[i] = false;
				gvReverse[i] = false;
				gvRandom[i] = false;
				gvStartF[i] = 0;			
			}
				
			destructEffect[0] = 0;
		}
	
		GVAppearanceType (void)
		{
			init();
		}

		~GVAppearanceType (void)
		{
			destroy();
		}

		virtual void init (const char *fileName);
		
		virtual void destroy (void);
		
		void setAnimation (TG_MultiShapePtr shape, DWORD animationNum);
		
		long getNumFrames (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MAX_GV_ANIMATIONS) && (gvAnimData[animationNum]))
				return gvAnimData[animationNum]->GetNumFrames();

			return 0.0f;
		}

		float getFrameRate (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MAX_GV_ANIMATIONS) && (gvAnimData[animationNum]))
				return gvAnimData[animationNum]->GetFrameRate();

			return 0.0f;
		}

		bool isReversed (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MAX_GV_ANIMATIONS) && (gvAnimData[animationNum]))
				return gvReverse[animationNum];

			return false;
		}
		
		bool isLooped (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MAX_GV_ANIMATIONS) && (gvAnimData[animationNum]))
				return gvAnimLoop[animationNum];

			return false;
		}
		
		bool isRandom (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MAX_GV_ANIMATIONS) && (gvAnimData[animationNum]))
				return gvRandom[animationNum];

			return false;
		}
		
		long getTotalNodes (void)
		{
			return numSmokeNodes + numWeaponNodes + numFootNodes;
		}

};

//***********************************************************************
//
// GVAppearance
//
//***********************************************************************
class GVAppearance : public ObjectAppearance
{
	public:

		GVAppearanceType*							appearType;
		
		TG_MultiShapePtr							gvShape;
		TG_MultiShapePtr							gvShadowShape;
		
		TG_MultiShapePtr							sensorCircleShape;
		TG_MultiShapePtr							sensorTriangleShape;
		float										sensorSpin;
									
		long										objectNameId;
	



		float										actualTurretRotation;

		bool										inDebugMoveMode;
		long										sensorLevel;
		float										hazeFactor;
		long										status;
			
		gosFX::EffectPtr								destructFX;
		gosFX::EffectPtr								waterWake;
		gosFX::EffectPtr								dustCloud;
		gosFX::EffectPtr								activity;
		bool										isWaking;
		bool										isActivitying;
		bool										movedThisFrame;
		bool										dustCloudStart;
		
		float										OBBRadius;

		// C3: cached GameObjectHandle from init(obj), used by GPU-cull lifecycle gates
		// in node-position functions. -1 = no owner; set in init().
		long										actorHandle_ = -1;








		bool										isInfantry;
		
		long										currentLOD;
		


		
		DWORD										localTextureHandle;
		
		DWORD										psRed;
		DWORD										psBlue;
		DWORD										psGreen;

		float										flashDuration;
		float										duration;
		float										currentFlash;
		bool										drawFlash;
		DWORD										flashColor;
		
		long										rotationalNodeIndex;
		long										dustNodeIndex;
		long										activityNodeIndex;
		long										hitNodeId;
		long										weaponNodeId[4];

		// (E) T1.10: generalised SpotLight_-child illumination on ground
		// vehicles. Mirrors the mech3d.h T1.6 layout (mech3d.h ~:362) and
		// the bdactor.h T1.4 layout. PER-CHILD-SPOTLIGHT-NODE TG_LIGHT_POINT
		// registered into eye->worldLights[] on the first night frame,
		// updated in-place each frame after that, freed in destroy().
		// GVAppearance has no pre-existing pointLight/lightId pair (unlike
		// Mech3DAppearance's anubis searchlight) so there is no
		// double-registration concern.
		std::vector<long>							spotlightNodeIds_;   // gvShape NodeNameId
		std::vector<TG_LightPtr>					spotlightLights_;    // owned via malloc/free
		std::vector<DWORD>							spotlightSlotIds_;   // worldLights[] indices
		bool										spotlightsRegistered_;

 	public:

		virtual void init (AppearanceTypePtr tree = nullptr, GameObjectPtr obj = nullptr);

		virtual AppearanceTypePtr getAppearanceType (void)
		{
			return appearType;
		}

		// C1a: expose OBBRadius via the base-class virtual so emitGpuCullRecord
		// can call app->getRadius() uniformly.
		virtual float getRadius (void)
		{
			return OBBRadius > 0.0f ? OBBRadius : 0.0f;
		}

		GVAppearance (void)
		{
			init();
		}

		virtual long update (bool animate = true);
		void updateGeometry (void);
        void updateSimPose();
        glm::quat poseRotation() const;
        void refreshSimPose() override { updateSimPose(); }
		
		virtual long render (long depthFixup = 0);

		virtual long renderShadows (void);
		
		virtual void destroy (void);

		virtual unsigned long getAppearanceClass (void)
		{
			return VEHICLE_APPR_TYPE;
		}
	
		~GVAppearance (void)
		{
			destroy();
		}

		virtual void setPaintScheme (void);

		virtual void setPaintScheme (DWORD red, DWORD green, DWORD blue);

		virtual void getPaintScheme (DWORD &red, DWORD &green, DWORD &blue);

		virtual void resetPaintScheme (DWORD red, DWORD green, DWORD blue);
		
 		virtual bool recalcBounds (void);

		virtual void flashBuilding (float duration, float flashDuration, DWORD color);

		
		void setFadeTable (MemoryPtr fTable)
		{
			fadeTable = fTable;
		}
		
		virtual void setObjectNameId (long objId)
		{
			objectNameId = objId;
		}

		virtual bool isMouseOver (float px, float py);
		
		virtual void setObjectParameters (const glm::vec3 &pos, float rot, long selected, long team, long homeRelations);

		virtual void setMoverParameters (float turretRot, float lArmRot = 0.0f, float rArmRot = 0.0f, bool isAirborne = false);

		void debugUpdate (void);

		virtual void setSensorLevel (long lvl)
		{
			sensorLevel = lvl;
		}

		// VISIBILITY-SEMANTICS-1 — see mech3d.h counterpart. NOTE for vehicles
		// sensorLevel is NEVER 5: code/gvehicl.cpp only ever passes 0..4, so
		// any `sensorLevel > 4` predicate is dead for the entire class.
		mc2_vis::MoverVis buildVisState (void) const
		{
			mc2_vis::MoverVis v;
			v.sensorLevel   = sensorLevel;
			v.meshAlpha     = gvShape ? (unsigned)gvShape->GetAlphaValue() : 255u;
			v.status        = status;
			v.relation      = homeTeamRelationship;
			v.legacyVisible = visible;
						return v;
		}

		virtual void setObjStatus (long oStatus);
		
		virtual bool playDestruction (void);
		
		virtual bool getInTransition (void)
		{
			return (unitAnim().canTransition == false);
		}

		virtual void setGesture (unsigned long gestureId);
		
		virtual long getCurrentGestureId (void)
		{
			return unitAnim().gvAnimationState;
		}

		//--------------------------------------------
		// Once site Objects are in place, go get 'em
		virtual void setWeaponNodeUsed (long nodeId);
		
		virtual glm::vec3 getWeaponNodePosition (long nodeId);
		
		virtual glm::vec3 getSmokeNodePosition (long nodeId);
		
		virtual glm::vec3 getDustNodePosition (long nodeId);
		
		virtual long getWeaponNode (long weapontype);
		
		virtual float getWeaponNodeRecycle (long node);

		virtual long getLowestWeaponNode (void);
		
		virtual glm::vec3 getNodeNamePosition (const char *nodeName);
		
 		virtual bool PerPolySelect (long mouseX, long mouseY);
		
		virtual glm::vec3 getRootNodeCenter (void)
		{
			glm::vec3 result = gvShape->GetRootNodeCenter();
			return result;
		}
		
		virtual void setAlphaValue (BYTE aVal)
		{
			gvShape->SetAlphaValue(aVal);
			
			//Sensor shape fades in opposite direction from mover
			sensorCircleShape->SetAlphaValue(0xff - aVal);
			sensorTriangleShape->SetAlphaValue(0xff - aVal);
		}

		virtual void scale (float scaleFactor)
		{
			gvShape->ScaleShape(scaleFactor);
		}
		
		virtual void startWaterWake (void);
		virtual void stopWaterWake (void);
		
		virtual void startActivity (long effectId, bool loop);
		virtual void stopActivity (void);

		virtual glm::vec3 getNodeIdPosition (long nodeId);

		virtual glm::vec3 getHitNode (void);
};

//***************************************************************************

#endif


