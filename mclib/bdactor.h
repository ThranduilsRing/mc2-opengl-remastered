//---------------------------------------------------------------------------
//
//	dbactor.h - This file contains the header for the Static ground object appearance class
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef BDACTOR_H
#define BDACTOR_H

//---------------------------------------------------------------------------
// Include files
#ifndef APPEAR_H
#include"appear.h"
#endif

#ifndef APPRTYPE_H
#include"apprtype.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif
#ifndef MSL_H
#include"msl.h"
#endif

#ifndef OBJECTAPPEARANCE_H
#include"objectappearance.h"
#endif

#include<gosfx/gosfxheaders.hpp>
#include <vector>  // T1.4: BldgAppearance::spotlightLights_/spotlightSlotIds_/spotlightNodeIds_
//**************************************************************************************
#ifndef NO_ERR
#define NO_ERR						0
#endif

#define MAX_BD_ANIMATIONS			10
//***********************************************************************
//
// BldgAppearanceType
//
//***********************************************************************
class BldgAppearanceType : public AppearanceType
{
	public:
	
		TG_TypeMultiShapePtr		bldgShape[MAX_LODS];
		float						lodDistance[MAX_LODS];

		// MODEL-OVERRIDE dual-shape (Slice 2): nullable type-level render shape.
		// NULL = no override → render falls back to stock bldgShape[lod].
		// Collision/damage NEVER read this; they read bldgShape[lod] directly.
		TG_TypeMultiShapePtr		bldgRenderShape;
		bool						buildingPbrEnabled;
		HGOSRENDERMATERIAL			buildingPbrProgram;
		DWORD						buildingPbrNormalTexture;
		DWORD						buildingPbrOrmTexture;
		DWORD						buildingPbrMaterialSsbo;
		float						buildingPbrTileScale;
		float						buildingPbrRoughnessBias;
		float						buildingPbrMetallicInfluence;
		bool						buildingPbrPreserveLegacyAlpha;
		bool						buildingFootprintShadowEnabled;
		float						buildingFootprintShadowStrength;
		float						buildingFootprintShadowSoftness;
		float						buildingFootprintShadowHeightBias;

		TG_TypeMultiShapePtr		bldgDmgShape;
		
		TG_AnimateShapePtr			bdAnimData[MAX_BD_ANIMATIONS];
		bool						bdAnimLoop[MAX_BD_ANIMATIONS];
		bool						bdReverse[MAX_BD_ANIMATIONS];
		bool						bdRandom[MAX_BD_ANIMATIONS];
		long						bdStartF[MAX_BD_ANIMATIONS];
		
		char						rotationalNodeId[TG_NODE_ID];
		char						destructEffect[60];
		
		bool						spinMe;
		bool						isForestClump;
		
		DWORD						terrainLightRGB;
		float						terrainLightIntensity;
		float						terrainLightInnerRadius;
		float						terrainLightOuterRadius;
		
		long						numWeaponNodes;
		NodeData					*nodeData;
		
	public:
	
		void init (void)
		{
			long i=0;
			for (i=0;i<MAX_LODS;i++)
			{
				bldgShape[i] = NULL;
				lodDistance[i] = 0.0f;
			}

			bldgRenderShape = NULL;   // MODEL-OVERRIDE dual-shape: no override by default
			buildingPbrEnabled = false;
			buildingPbrProgram = NULL;
			buildingPbrNormalTexture = 0;
			buildingPbrOrmTexture = 0;
			buildingPbrMaterialSsbo = 0;
			buildingPbrTileScale = 1.0f;
			buildingPbrRoughnessBias = 0.0f;
			buildingPbrMetallicInfluence = 0.0f;
			buildingPbrPreserveLegacyAlpha = false;
			buildingFootprintShadowEnabled = false;
			buildingFootprintShadowStrength = 0.0f;
			buildingFootprintShadowSoftness = 0.0f;
			buildingFootprintShadowHeightBias = 0.05f;
			bldgDmgShape = NULL;
			
			for (i=0;i<MAX_BD_ANIMATIONS;i++)
            {
				bdAnimData[i] = NULL;

                //sebi: init so will not contain garbage
                bdAnimLoop[i] = false;
                bdReverse[i] = false;
                bdRandom[i] = false;
                bdStartF[i] = 0;
                //
            }
				
			destructEffect[0] = 0;
			
			spinMe = false;

            //sebi: init so will not contain garbage
            nodeData = NULL;
            numWeaponNodes = 0;
            isForestClump = false;
            MemSet(rotationalNodeId, 0);//sebi
            //
		}
	
		BldgAppearanceType (void)
		{
			init();
		}

		~BldgAppearanceType (void)
		{
			destroy();
		}

		void setAnimation (TG_MultiShapePtr shape, DWORD animationNum);
		
		long getNumFrames (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MAX_BD_ANIMATIONS) && (bdAnimData[animationNum]))
				return bdAnimData[animationNum]->GetNumFrames();

			return 0.0f;
		}

		float getFrameRate (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MAX_BD_ANIMATIONS) && (bdAnimData[animationNum]))
				return bdAnimData[animationNum]->GetFrameRate();

			return 0.0f;
		}

		void setFrameRate (long animationNum, float nFrameRate)
		{
			if ((animationNum >= 0) && (animationNum < MAX_BD_ANIMATIONS) && (bdAnimData[animationNum]))
				bdAnimData[animationNum]->SetFrameRate(nFrameRate);
		}

		bool isReversed (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MAX_BD_ANIMATIONS) && (bdAnimData[animationNum]))
				return bdReverse[animationNum];

			return false;
		}
		
		bool isLooped (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MAX_BD_ANIMATIONS) && (bdAnimData[animationNum]))
				return bdAnimLoop[animationNum];

			return false;
		}
		
		bool isRandom (long animationNum)
		{
			if ((animationNum >= 0) && (animationNum < MAX_BD_ANIMATIONS) && (bdAnimData[animationNum]))
				return bdRandom[animationNum];

			return false;
		}
		
  		virtual void init (const char *fileName);

		virtual void destroy (void);

		// MODEL-OVERRIDE dual-shape accessors.
		// Render authority: override if present, else stock LOD. LOD-collapses
		// under override (returns the single override shape for every LOD).
		TG_TypeMultiShape* getBldgRenderShape (long lod)
		{
			return bldgRenderShape ? bldgRenderShape : bldgShape[lod];
		}
		// Collision authority: ALWAYS stock. Collision must never call the
		// render accessor (it already reads bldgShape[lod] directly).
		TG_TypeMultiShape* getBldgCollisionShape (long lod) { return bldgShape[lod]; }

};

//***********************************************************************
//
// BldgAppearance
//
//***********************************************************************
class BldgAppearance : public ObjectAppearance
{
	public:

		BldgAppearanceType*							appearType;
		TG_MultiShapePtr							bldgShape;
		bool										buildingPbrRenderActive;

		// Slice 2 (object-offload): set true by the GPU-object batcher's
		// late-registration branch when a leaf type is encountered for the
		// first time. The next BldgAppearance::update sees this flag and
		// forces a full TransformMultiShape (not _PositionsOnly) so the
		// per-vertex CPU bake refreshes after type registration. Stage 2.A:
		// declared and false-init; never mutated. Stage 2.B wires the
		// late-reg setter and the eligibility-hoist consumer.
		bool										needsFullBakeNextFrame;

		// Stage 3.D: per-instance static-registry state. Mirrors the struct
		// inside TreeAppearance — same fields, same semantics. Set when
		// render() successfully baked + registered a recipe; cleared by
		// invalidateStaticRegistration() on shape swap, fall, or destroy.
		struct StaticRegistration {
			bool             registered;   // true iff recipeIndex is valid
			TG_MultiShapePtr shape;        // bldgShape at registration time; detects swap
			int32_t          recipeIndex;  // index into GpuStaticPropRegistry s_recipeRanges
			// ZERO-LIGHT-ROW-1 (2026-07-31): this was labelled "diagnostic-only"
			// and it is NOT. It is load-bearing: bdactor.cpp:5147 uses it as a
			// term of `stableLightSkipEligible`, which decides whether the
			// STABLE_LIGHT_SKIP optimisation may skip a light resubmit. A field
			// that looks like decoration but gates real work is its own hazard,
			// so the label is corrected rather than the field removed.
			// Maintained ONLY by the building paths (:5102, :5214, :5447);
			// TreeAppearance's mirror struct has no equivalent and its touch
			// paths perform no sentinel test at all.
			bool             hasValidStaticLight = false;  // load-bearing: gates STABLE_LIGHT_SKIP
			uint64_t         lastLightEnvGen = 0;          // coarse mission light generation
			// 2026-05-11: captured at update/touch time, immediately after
			// CacheGpuLightData / ResubmitCachedGpuLightData write multi's
			// per-type cachedGpuLightIndex_ — before sibling instances of the
			// same multi-type overwrite it. Ferried into RecipeRange via
			// markVisible() at render time. UINT32_MAX = uncaptured; flush
			// falls back to multi cache when sentinel is seen.
			uint32_t         lightDataIndex = 0xFFFFFFFFu;
		};
		StaticRegistration							staticReg;

		// Static terrain-object pick screen-rect cache.
		// Invalidated by camera revision only — safe because terrain statics never
		// move post-spawn. Dynamic/movable objects must NOT use this cache without
		// also tracking an object transform revision.
		struct PickScreenRectCache {
			uint32_t cameraRevision = 0;	// 0 = uninitialized/invalid sentinel
			long     ulx = 0, uly = 0;
			long     lrx = 0, lry = 0;
			bool     valid = false;
		};
		PickScreenRectCache							pickCache_;

		long										bdAnimationState;
		float										currentFrame;
		float										bdFrameRate;
		bool										isReversed;
		bool										isLooping;
		bool										setFirstFrame;
		bool										canTransition;
		
		float										turretYaw;
		float										turretPitch;
		
		float										hazeFactor;
		
		long										status;
		
   		gosFX::Effect								*destructFX;
   		gosFX::Effect								*activity;
   		gosFX::Effect								*activity1;
		bool										isActivitying;
		
		float										OBBRadius;
		float										highZ;
		
		float										SpinAngle;
		
		float										flashDuration;
		float										duration;
		float										currentFlash;
		bool										drawFlash;
		DWORD										flashColor;
		
		long										currentLOD;
		
 		long										*nodeUsed;				//Used to stagger the weapon nodes for firing.
		float										*nodeRecycle;			//Used for ripple fire to find out if the node has fired recently.
		
		TG_LightPtr									pointLight;
		DWORD										lightId;
		bool										forceLightsOut;
		bool										beenInView;

		// (E) T1.4: SpotLight_-child illumination. PER-CHILD-SPOTLIGHT-NODE
		// (TG_LIGHT_POINT). NOT the same as `pointLight`/`lightId` above —
		// that pair is per-building TERRAIN ambient (TG_LIGHT_TERRAIN,
		// sourced from appearType->terrainLightRGB at bdactor.cpp:1933-1972).
		// Distinct concerns; both coexist. See plan R7.
		std::vector<TG_LightPtr>					spotlightLights_;
		std::vector<DWORD>							spotlightSlotIds_;
		std::vector<int>							spotlightNodeIds_;  // bldgShape NodeNameId
		bool										spotlightsRegistered_;
		
		bool										fogLightSet;
		DWORD										lightRGB;
		DWORD										fogRGB;

		long										rotationalNodeId;
		long										hitNodeId;
		long										activityNodeId;
		long										activityNode1Id;

		// FRAME-JOBS-2F: per-frame stamp set by touchSerialCommit() so touch() (Path B,
		// terrain object loop) can return early and avoid redundant EmitBakedGpuLightData.
		// Initialized to 0xFFFFFFFFu to avoid false match on frame 0.
		// When MC2_FRAME_JOBS_TOUCH=0, touchSerialCommit() is never called and stamp is
		// never set, so touch() always runs normally — the guard is a no-op in that mode.
		uint32_t									touchSerialCommitFrame = 0xFFFFFFFFu;

 	public:

		virtual void init (AppearanceTypePtr tree = NULL, GameObjectPtr obj = NULL);

		virtual AppearanceTypePtr getAppearanceType (void)
		{
			return appearType;
		}

		BldgAppearance (void)
		{
			init();
		}

		virtual long update (bool animate = true);
		virtual long render (long depthFixup = 0);

		virtual long renderShadows (void);

		virtual void destroy (void);

		// Stage 3.D: static-registry overrides (mirror of TreeAppearance).
		virtual bool IsStaticNow() const override;
		virtual void touch() override;
		virtual void invalidateStaticRegistration() override;
		// Eligibility split out from IsStaticNow so the registration block
		// can pre-filter at submit time (don't bake a recipe for an animated
		// or actively-fx'd building — it would never enter the static path).
		bool isStaticEligible() const;
		// Task 5 (Track B): mission-load bulk registration overrides.
		virtual void registerStatic()              override;
		virtual bool isStaticRegistered()    const override;
		virtual int32_t getStaticRecipeIndex() const override;
		// STATIC-REG-PREWARM-QUEUE-1: off-screen light bake at mission load.
		virtual bool prewarmStaticLightBake(Camera* cam) override;
		virtual bool needsPrewarmBake() const override { return needsFullBakeNextFrame && staticReg.registered; }

		~BldgAppearance (void)
		{
			destroy();
		}

		virtual bool recalcBounds (void);
		// FRAME-JOBS-1: worker-path bounds. Calls recalcBounds() then stamps boundsFrame.
		void recalcBoundsAndStamp();
		bool isRecalcBoundsWorkerSafe() const override { return true; }
		bool isTouchWorkerSafe() const override { return true; }

		// FRAME-JOBS-2D: split touch prepass
		bool isTouchSplitSafe() const override { return true; }
		void touchWorkerPrepass() override;
		void touchSerialCommit() override;

		virtual bool getInTransition (void)
		{
			return (canTransition == false);
		}

		void setFadeTable (MemoryPtr fTable)
		{
			fadeTable = fTable;
		}

		virtual void setGesture (unsigned long gestureId);
		
		virtual long getCurrentGestureId (void)
		{
			return bdAnimationState;
		}

		virtual unsigned long getAppearanceClass (void)
		{
			return BUILDING_APPR_TYPE;
		}
			
		virtual void setObjectNameId (long objId)
		{
			objectNameId = objId;
		}

		virtual bool isMouseOver (float px, float py);
		
		virtual void setObjectParameters (const Stuff::Vector3D &pos, float rot, long selected, long alignment, long homeRelations);
		
		virtual void setMoverParameters (float turretRot, float lArmRot = 0.0f, float rArmRot = 0.0f, bool isAirborne = false);
		
		virtual void setObjStatus (long oStatus);
		
		virtual long calcCellsCovered (Stuff::Vector3D& pos, short* cellList);

		virtual void markTerrain(_ScenarioMapCellInfo* pInfo, int type, int counter);
		
		virtual long markMoveMap (bool passable, long* lineOfSightRect, bool useheight = false, short* cellList = NULL);

		virtual void markLOS (bool clearIt = false);
		
 		void calcAdjCell (long& row, long& col);

		virtual void scale (float scaleFactor)
		{
			bldgShape->ScaleShape(scaleFactor);
		}
		
		virtual bool playDestruction (void);
		
		virtual float getRadius (void)
		{
			float ext = bldgShape ? bldgShape->GetExtentRadius() : 0.0f;
			return OBBRadius > ext ? OBBRadius : ext;
		}

		virtual void flashBuilding (float duration, float flashDuration, DWORD color);

		virtual float getTopZ (void)
		{
			return highZ;
		}
		
		virtual void setWeaponNodeUsed (long nodeId);
		
		virtual long getWeaponNode (long weapontype);
		
		virtual float getWeaponNodeRecycle (long node);
		
		virtual Stuff::Vector3D getWeaponNodePosition (long node);

		virtual bool isSelectable()
		{
			return !appearType->spinMe;
		}

		virtual void setFilterState (bool state)
		{
			bldgShape->GetMultiType()->SetFilter(state);
		}
		
		virtual void setIsHudElement (void)
		{
			bldgShape->SetIsHudElement();
		}
		
		virtual bool getIsLit (void)
		{
			return (appearType->terrainLightRGB != 0xffffffff);
		}
		
		virtual void setLightsOut (bool lightFlag)
		{
			forceLightsOut = lightFlag;
		}

		virtual bool PerPolySelect (long mouseX, long mouseY);

		virtual bool isForestClump (void)
		{	
			return appearType->isForestClump;
		}
		
		virtual Stuff::Point3D getRootNodeCenter (void)
		{
			Stuff::Point3D result = bldgShape->GetRootNodeCenter();
			return result;
		}
		
		virtual Stuff::Vector3D getNodeNamePosition (const char *nodeName);
		
		virtual void startActivity (long effectId, bool loop);
		virtual void stopActivity (void);

		virtual Stuff::Vector3D getHitNode (void);

		virtual bool hasAnimationData (long gestureId)
		{
			return (appearType->bdAnimData[gestureId] != NULL);
		}

		virtual Stuff::Vector3D getNodeIdPosition (long nodeId);
};

//***************************************************************************

//***********************************************************************
//
// TreeAppearanceType
//
//***********************************************************************
class TreeAppearanceType : public AppearanceType
{
	public:
	
		TG_TypeMultiShapePtr		treeShape[MAX_LODS];
		float						lodDistance[MAX_LODS];

		// MODEL-OVERRIDE dual-shape (Slice 2): nullable type-level render shape.
		// NULL = no override → render falls back to stock treeShape[lod].
		// Collision/damage NEVER read this; they read treeShape[lod] directly.
		// TREE-OVERRIDE-LOD-MVP Task 1: per-LOD render chain. treeRenderShape[0]
		// is the override LOD0 (was the single member); higher indices hold
		// auto/offline-generated lower LODs (populated in Task 3+). All NULL =
		// no override. treeRenderShapeLodCount = number of populated LODs.
		TG_TypeMultiShapePtr		treeRenderShape[MAX_LODS];
		long						treeRenderShapeLodCount;

		TG_TypeMultiShapePtr		treeDmgShape;
		
		TG_AnimateShapePtr			treeAnimData[MAX_BD_ANIMATIONS];
		bool						isForestClump;
		
	public:
	
		void init (void)
		{
			long i=0;
			for (i=0;i<MAX_LODS;i++)
			{
				treeShape[i] = NULL;
				lodDistance[i] = 0.0f;
			}

			for (i=0;i<MAX_BD_ANIMATIONS;i++)
				treeAnimData[i] = NULL;

			// TREE-OVERRIDE-LOD-MVP Task 1: no override by default; all LODs NULL.
			for (i=0;i<MAX_LODS;i++)
				treeRenderShape[i] = NULL;
			treeRenderShapeLodCount = 0;
			treeDmgShape = NULL;
		}
	
		TreeAppearanceType (void)
		{
			init();
		}

		~TreeAppearanceType (void)
		{
			destroy();
		}

		virtual void init (const char *fileName);

		virtual void destroy (void);

		// MODEL-OVERRIDE dual-shape accessors (mirror of BldgAppearanceType).
		// TREE-OVERRIDE-LOD-MVP Task 1: real per-LOD indexing.
		//  - exact override LOD if populated;
		//  - else clamp DOWN to the highest populated override LOD <= lod
		//    (so a request beyond the populated chain reuses the lowest-detail
		//    override we have, never falls back to stock mid-chain);
		//  - else (no override at all) stock treeShape[lod].
		// NEVER returns NULL. Collision reads stock treeShape[] via
		// getTreeCollisionShape — unchanged, LOD-independent.
		TG_TypeMultiShape* getTreeRenderShape (long lod)
		{
			if (lod < 0)        lod = 0;
			if (lod >= MAX_LODS) lod = MAX_LODS - 1;
			if (treeRenderShape[lod])
				return treeRenderShape[lod];
			// clamp to the highest populated override LOD <= lod
			for (long i = lod - 1; i >= 0; --i)
				if (treeRenderShape[i])
					return treeRenderShape[i];
			// no override populated for this or any lower index → stock
			return treeShape[lod];
		}
		TG_TypeMultiShape* getTreeCollisionShape (long lod) { return treeShape[lod]; }
};

//***********************************************************************
//
// TreeAppearance
//
//***********************************************************************
class TreeAppearance : public ObjectAppearance
{
	public:

		TreeAppearanceType*							appearType;
		TG_MultiShapePtr							treeShape;

		// Slice 2 (object-offload): see BldgAppearance::needsFullBakeNextFrame.
		bool										needsFullBakeNextFrame;

		// Stage 3.C: per-instance static-registry state.
		struct StaticRegistration {
			bool             registered;   // true iff recipeIndex is valid
			TG_MultiShapePtr shape;        // treeShape at registration time; detects swap
			int32_t          recipeIndex;  // index into GpuStaticPropRegistry s_recipeRanges
			// 2026-05-11: see BldgAppearance::StaticRegistration::lightDataIndex.
			uint32_t         lightDataIndex = 0xFFFFFFFFu;
		};
		// TREE-OVERRIDE-LOD-MVP Task 2: per-LOD static registration. Each
		// populated render LOD owns its own recipe + permanent baked light slot,
		// registered before finalizeGeometry. activeLOD selects which tuple the
		// per-frame replay path uses; pinned 0 in Tasks 1-4 (distance selection
		// arrives in Task 5) so behavior == single-LOD today.
		StaticRegistration							staticReg[MAX_LODS];
		long										activeLOD;

		float										hazeFactor;
		float										pitch;
		float										yaw;
		long										status;
		
		float										OBBRadius;
		
		long										currentLOD;
		
		bool										forceLightsOut;
		bool										beenInView;

		bool										fogLightSet;
		DWORD										lightRGB;
		DWORD										fogRGB;

		// FRAME-JOBS-2F: per-frame stamp set by touchSerialCommit() so touch() (Path B,
		// terrain object loop) can return early and avoid redundant EmitBakedGpuLightData.
		// See BldgAppearance::touchSerialCommitFrame for full contract.
		uint32_t									touchSerialCommitFrame = 0xFFFFFFFFu;

	public:

		virtual void init (AppearanceTypePtr tree = NULL, GameObjectPtr obj = NULL);

		virtual AppearanceTypePtr getAppearanceType (void)
		{
			return appearType;
		}

		TreeAppearance (void)
		{
			init();
		}

		virtual long update (bool animate = true);
		virtual long render (long depthFixup = 0);

		virtual long renderShadows (void);

		virtual void destroy (void);

		// Stage 3.C: static-registry overrides
		virtual bool IsStaticNow() const override;
		virtual void touch() override;
		void selectActiveLOD();  // TREE-OVERRIDE-LOD-MVP: per-frame distance LOD pick (update+touch)
		virtual void invalidateStaticRegistration() override;
		// Task 5 (Track B): mission-load bulk registration overrides.
		virtual void registerStatic()              override;
		virtual bool isStaticRegistered()    const override;
		virtual int32_t getStaticRecipeIndex() const override;
		// STATIC-REG-PREWARM-QUEUE-1: off-screen light bake at mission load.
		virtual bool prewarmStaticLightBake(Camera* cam) override;
		virtual bool needsPrewarmBake() const override { return needsFullBakeNextFrame && staticReg[0].registered; }

		~TreeAppearance (void)
		{
			destroy();
		}

		virtual unsigned long getAppearanceClass (void)
		{
			return TREE_APPR_TYPE;
		}

		virtual bool recalcBounds (void);
		// FRAME-JOBS-1: worker-path bounds. Calls recalcBounds() then stamps boundsFrame.
		void recalcBoundsAndStamp();
		bool isRecalcBoundsWorkerSafe() const override { return true; }
		bool isTouchWorkerSafe() const override { return true; }

		// FRAME-JOBS-2D: split touch prepass
		bool isTouchSplitSafe() const override { return true; }
		void touchWorkerPrepass() override;
		void touchSerialCommit() override;

		void setFadeTable (MemoryPtr fTable)
		{
			fadeTable = fTable;
		}

		virtual void setObjectNameId (long objId)
		{
			objectNameId = objId;
		}

		virtual bool isMouseOver (float px, float py);

		virtual void setObjectParameters (const Stuff::Vector3D &pos, float rot, long selected, long alignment, long homeRelations);

		virtual void setMoverParameters (float pitchAngle, float lArmRot = 0.0f, float rArmRot = 0.0f, bool isAirborne = false);
		
		virtual void setObjStatus (long oStatus);

		virtual void scale (float scaleFactor)
		{
			treeShape->ScaleShape(scaleFactor);
		}
		
		virtual float getRadius (void)
		{
			float ext = treeShape ? treeShape->GetExtentRadius() : 0.0f;
			return OBBRadius > ext ? OBBRadius : ext;
		}

		virtual void setLightsOut (bool lightFlag)
		{
			forceLightsOut = lightFlag;
		}
		
		virtual bool isForestClump (void)
		{	
			return appearType->isForestClump;
		}
		
		virtual void markTerrain(_ScenarioMapCellInfo* pInfo, int type, int counter);

 		virtual Stuff::Point3D getRootNodeCenter (void)
		{
			Stuff::Point3D result = treeShape->GetRootNodeCenter();
			return result;
		}

		virtual void markLOS (bool clearIt = false);
};

//***************************************************************************

// EDITOR-STATIC-TEXTURE-PREWARM-1: public seam over the static helper
// LoadOverrideRenderShapeTextures (bdactor.cpp). The editor palette prime path
// (EditorObjectMgr::primeAllBuildingAppearanceTypes) calls this to force every
// placeable type's render-shape textures resident on disk BEFORE the one-shot
// finalizeGeometry() probe, which reads mcTextureManager->getWidth(nodeIdx).
// Without it the probe sees width 0 -> layerForPacket=-1 -> packet dropped ->
// invisible body. The helper stays static; this wrapper is the only export.
void Bldg_ForceRenderShapeTexturesResident(TG_TypeMultiShape* rs);

#endif
