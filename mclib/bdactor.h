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
#include <algorithm>
#include <memory>
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

inline constexpr int MAX_BD_ANIMATIONS = 10;
struct ModelOverrideRecord;
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
		std::unique_ptr<TG_TypeMultiShape> bldgRenderLods[MAX_LODS];
		// Destroyed-state render override (the cooked "destroyed" chain); NULL = stock bldgDmgShape.
		std::unique_ptr<TG_TypeMultiShape>	bldgDmgRenderShape;
        const ModelOverrideRecord* propMaterialSource = nullptr; // transient; registry owns it

		TG_TypeMultiShapePtr		bldgDmgShape;
		
		TG_AnimateShapePtr			bdAnimData[MAX_BD_ANIMATIONS];
		bool						bdAnimLoop[MAX_BD_ANIMATIONS];
		bool						bdReverse[MAX_BD_ANIMATIONS];
		bool						bdRandom[MAX_BD_ANIMATIONS];
		long						bdStartF[MAX_BD_ANIMATIONS];
		
		char						rotationalNodeId[TG_NODE_ID];
		char elevationNodeId[TG_NODE_ID] = "NONE";
		char						destructEffect[60];
		
		bool						spinMe;
		bool						isForestClump;
		
		DWORD						terrainLightRGB;
		float						terrainLightIntensity;
		float						terrainLightInnerRadius;
		float						terrainLightOuterRadius;
		bool stockTerrainLamp = false;
		
		long						numWeaponNodes;
		NodeData					*nodeData;			//View of nodeDataStore
		std::unique_ptr<NodeData[]>	nodeDataStore;		//Owns the node table, built once by init(file)
		
	public:
	
		void init (void)
		{
			long i=0;
			for (i=0;i<MAX_LODS;i++)
			{
				bldgShape[i] = nullptr;
				lodDistance[i] = 0.0f;
			}

			bldgRenderShape = nullptr;   // MODEL-OVERRIDE dual-shape: no override by default
			bldgDmgRenderShape = nullptr;
            propMaterialSource = nullptr;
			bldgDmgShape = nullptr;
			
			for (i=0;i<MAX_BD_ANIMATIONS;i++)
            {
				bdAnimData[i] = nullptr;

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
            nodeData = nullptr;
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
		// Render authority: cooked LOD when available, else override LOD0 or stock.
		TG_TypeMultiShape* getBldgRenderShape (long lod)
		{
			if (!bldgRenderShape) return bldgShape[lod];
			return bldgRenderLods[lod] ? bldgRenderLods[lod].get() : bldgRenderShape;
		}
		// Collision authority: ALWAYS stock. Collision must never call the
		// render accessor (it already reads bldgShape[lod] directly).
		TG_TypeMultiShape* getBldgCollisionShape (long lod) { return bldgShape[lod]; }
		// Destroyed render authority: the override's destroyed chain if cooked, else stock damage.
		TG_TypeMultiShape* getBldgDmgRenderShape (void) { return bldgDmgRenderShape ? bldgDmgRenderShape.get() : bldgDmgShape; }
		// Registers every render shape (LODs + destroyed) with the GPU static-prop batcher, tagging
		// override shapes with their material source. Idempotent; the per-instance init and the
		// mission-load pre-pass both go through here.
		void registerRenderShapes (void);

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
		std::unique_ptr<TG_MultiShape> simulationShape_;
		TG_TypeMultiShape* simulationType_ = nullptr;
		long simulationAnimationState_ = -1;
		float simulationFrame_ = 0.0f;
		// Simulation inputs: the tick writes them (setSimInput, setObjStatus, or setObjectParameters on the init/load paths);
		// only updateSimPose reads them. The draw path reads position / rotation / status, which presentation writes.
		glm::vec3 simPosition_{};
		float simRotation_ = 0.0f;
		long simStatus_ = 0;
		// What the sim must read: the draw copy (status) lags the tick, and the camera picks the draw shape.
		long simStatusView() const { return simulationOwned() ? simStatus_ : status; }
		void setSimInput (const SimInput& in) override;
		void updateSimPose();
		long desiredRenderLOD() const;
		void updateRenderLOD();
		void refreshSimPose() override { updateSimPose(); }

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








		
		
		float										hazeFactor;
		
		long										status;
		
		gosFX::EffectPtr								destructFX;
		gosFX::EffectPtr								activity;
		gosFX::EffectPtr								activity1;
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
		void clearSpotlights();
		// Presentation only: never serialized or copied to unitAnim/simulationShape_.
		float searchlightYaw_ = 0.0f, searchlightPitch_ = 0.0f, searchlightTime_ = 0.0f;
		void updateSearchlight();
		void releaseBldgShape();
		
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

		virtual void init (AppearanceTypePtr tree = nullptr, GameObjectPtr obj = nullptr);

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
		// UI preview (SimpleCamera): sets the highlight, returns the shape for TG_MultiShape::PreviewDraw.
		TG_MultiShape* previewShape (void);

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
			return (unitAnim().canTransition == false);
		}

		void setFadeTable (MemoryPtr fTable)
		{
			fadeTable = fTable;
		}

		virtual void setGesture (unsigned long gestureId);
		
		virtual long getCurrentGestureId (void)
		{
			return unitAnim().bdAnimationState;
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
		
		virtual void setObjectParameters (const glm::vec3 &pos, float rot, long selected, long alignment, long homeRelations);
		
		virtual void setMoverParameters (float turretRot, float lArmRot = 0.0f, float rArmRot = 0.0f, bool isAirborne = false);
		
		virtual void setObjStatus (long oStatus);
		
		virtual long calcCellsCovered (glm::vec3& pos, short* cellList);

		virtual void markTerrain(_ScenarioMapCellInfo* pInfo, int type, int counter);
		
		virtual long markMoveMap (bool passable, long* lineOfSightRect, bool useheight = false, short* cellList = nullptr);

		virtual void markLOS (bool clearIt = false);
		
 		void calcAdjCell (long& row, long& col);

		virtual void scale (float scaleFactor)
		{
			bldgShape->ScaleShape(scaleFactor);
		}
		
		virtual bool playDestruction (void);
		
		virtual float getRadius (void)
		{
			if (!appearType || (!appearType->bldgRenderShape && !appearType->bldgDmgRenderShape)) {
				float ext = bldgShape ? bldgShape->GetExtentRadius() : 0.0f;
				return OBBRadius > ext ? OBBRadius : ext;
			}
			auto* source = appearType->getBldgCollisionShape(0);
			if (source && status == OBJECT_STATUS_DESTROYED && appearType->bldgDmgShape)
				source = appearType->bldgDmgShape;
			float ext = source ? source->extentRadius : 0.0f;
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
		
		virtual glm::vec3 getWeaponNodePosition (long node);

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
		
		virtual glm::vec3 getRootNodeCenter (void)
		{
			glm::vec3 result = bldgShape->GetRootNodeCenter();
			return result;
		}
		
		virtual glm::vec3 getNodeNamePosition (const char *nodeName);
		
		virtual void startActivity (long effectId, bool loop);
		virtual void stopActivity (void);

		virtual glm::vec3 getHitNode (void);

		virtual bool hasAnimationData (long gestureId)
		{
			return (appearType->bdAnimData[gestureId] != nullptr);
		}

		virtual glm::vec3 getNodeIdPosition (long nodeId);
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
        std::unique_ptr<TG_TypeMultiShape> treeDmgRenderShape;
        const ModelOverrideRecord* propMaterialSource = nullptr;
		
		TG_AnimateShapePtr			treeAnimData[MAX_BD_ANIMATIONS];
		bool						isForestClump;
		
	public:
	
		void init (void)
		{
			long i=0;
			for (i=0;i<MAX_LODS;i++)
			{
				treeShape[i] = nullptr;
				lodDistance[i] = 0.0f;
			}

			for (i=0;i<MAX_BD_ANIMATIONS;i++)
				treeAnimData[i] = nullptr;

			// TREE-OVERRIDE-LOD-MVP Task 1: no override by default; all LODs NULL.
			for (i=0;i<MAX_LODS;i++)
				treeRenderShape[i] = nullptr;
			treeRenderShapeLodCount = 0;
			treeDmgShape = nullptr;
            treeDmgRenderShape.reset();
            propMaterialSource = nullptr;
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
			lod = std::clamp(lod, 0L, static_cast<long>(MAX_LODS) - 1);
			if (treeRenderShape[lod])
				return treeRenderShape[lod];
			// clamp to the highest populated override LOD <= lod
			for (long i = lod - 1; i >= 0; --i)
				if (treeRenderShape[i])
					return treeRenderShape[i];
			// no override populated for this or any lower index → stock
			return treeShape[lod];
		}
        TG_TypeMultiShape* getTreeDamageRenderShape() { return treeDmgRenderShape ? treeDmgRenderShape.get() : treeDmgShape; }
        void registerRenderShapes();
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
		long										simStatus_ = 0;   // tick copy of the status; the draw swap is deferred to presentation

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
		bool shadowMoving = false;
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

		virtual void init (AppearanceTypePtr tree = nullptr, GameObjectPtr obj = nullptr);

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
		// Fixed terrain tilt is a pose; the owner supplies actual falling motion.
		bool shadowIsMoving() const { return shadowMoving; }
		void setShadowMoving(bool moving)
		{
			if (shadowMoving != moving) invalidateStaticRegistration();
			shadowMoving = moving;
		}
		virtual void invalidateStaticRegistration() override;
		// Task 5 (Track B): mission-load bulk registration overrides.
		virtual void registerStatic()              override;
		virtual bool isStaticRegistered()    const override;
		virtual int32_t getStaticRecipeIndex() const override;
		virtual int32_t getShadowCasterRecipeIndex() const override;
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

		virtual void setObjectParameters (const glm::vec3 &pos, float rot, long selected, long alignment, long homeRelations);

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

 		virtual glm::vec3 getRootNodeCenter (void)
		{
			glm::vec3 result = treeShape->GetRootNodeCenter();
			return result;
		}

		virtual void markLOS (bool clearIt = false);
};

//***************************************************************************

// EDITOR-STATIC-TEXTURE-PREWARM-1: public seam over the static helper
// LoadOverrideRenderShapeTextures (bdactor.cpp). The editor palette prime path
// (EditorObjectMgr::primeAllBuildingAppearanceTypes) calls this to force every
// placeable type's render-shape textures resident on disk BEFORE the one-shot
// finalizeGeometry() probe, which reads the texture source logical size.
// Without it the probe sees width 0 -> layerForPacket=-1 -> packet dropped ->
// invisible body. The helper stays static; this wrapper is the only export.
void Bldg_ForceRenderShapeTexturesResident(TG_TypeMultiShape* rs);

#endif
