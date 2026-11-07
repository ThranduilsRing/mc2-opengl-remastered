// BldgAppearance / TreeAppearance: TreeAppearance init, status, bounds, update, terrain marking, LOD select and destroy.
// Declared in the class headers; shared names in bdactor_internal.h.
#include <algorithm>
#include <atomic>
#include <optional>
#include "bdactor_internal.h"
#include "texture_assets.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"

void TreeAppearance::init (AppearanceTypePtr tree, GameObjectPtr obj)
{
	Appearance::init(tree,obj);
	appearType = static_cast<TreeAppearanceType *>(tree);

	shapeMin.x = shapeMin.y = -25;
	shapeMax.x = shapeMax.y = 50;
	
	paintScheme = -1;
	objectNameId = 30862;
	
	hazeFactor = 0.0f;

	screenPos.x = screenPos.y = screenPos.z = screenPos.w = -999.0f;
	position = glm::vec3(0.0f);
	rotation = 0.0;;
	selected = 0;
	teamId = 0;
	homeTeamRelationship = 0;
	actualRotation = rotation;

	OBBRadius = -1.0f;

	currentLOD = 0;
	
	beenInView = false;
	
	fogLightSet = false;
	lightRGB = fogRGB = 0xffffffff;

    status = OBJECT_STATUS_NORMAL;
    forceLightsOut = false;
    // Slice 2 (object-offload) substrate flag; set true by GPU batcher on late
    // registration to force a full TransformMultiShape next frame.
    needsFullBakeNextFrame = false;
    for (long _l = 0; _l < MAX_LODS; ++_l)
        staticReg[_l] = StaticRegistration{};
    activeLOD = 0;  // pinned 0 until Task 5 adds distance selection
    treeShape = nullptr;

	if (appearType)
	{
		// MODEL-OVERRIDE dual-shape: build the per-instance RENDER shape from the
		// render accessor (override if present, else stock). Collision (markTerrain/
		// markLOS) rebuilds this member from stock treeShape[lod] when needed.
		treeShapeOwner_.reset(appearType->getTreeRenderShape(0)->CreateFrom());
		treeShape = treeShapeOwner_.get();

		for (long i=0;i<treeShape->GetNumTextures();i++)
		{
			char txmName[1024];
			treeShape->GetTextureName(i,txmName,256);
	
			char texturePath[1024];
			mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
	
			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");
	
			if (textureOrKtxSidecarExists(textureName))
			{
				if (S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					treeShape->SetTextureHandle(i,gosTextureHandle);
					treeShape->SetTextureAlpha(i,true);
				}
				else
				{
					DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					treeShape->SetTextureHandle(i,gosTextureHandle);
					treeShape->SetTextureAlpha(i,false);
				}
			}
			else
			{
				treeShape->SetTextureHandle(i,0xffffffff);
			}
		}
		
		glm::vec3 boxCoords[8];
		glm::vec3 nodeCenter = treeShape->GetRootNodeCenter();

		boxCoords[0].x = position.x + treeShape->GetMinBox().x + nodeCenter.x;
		boxCoords[0].y = position.y + treeShape->GetMinBox().z + nodeCenter.z;
		boxCoords[0].z = position.z + treeShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[1].x = position.x + treeShape->GetMinBox().x + nodeCenter.x;
		boxCoords[1].y = position.y + treeShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[1].z = position.z + treeShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[2].x = position.x + treeShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[2].y = position.y + treeShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[2].z = position.z + treeShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[3].x = position.x + treeShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[3].y = position.y + treeShape->GetMinBox().z + nodeCenter.z;
		boxCoords[3].z = position.z + treeShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[4].x = position.x + treeShape->GetMinBox().x + nodeCenter.x;
		boxCoords[4].y = position.y + treeShape->GetMinBox().z + nodeCenter.z;
		boxCoords[4].z = position.z + treeShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[5].x = position.x + treeShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[5].y = position.y + treeShape->GetMinBox().z + nodeCenter.z;
		boxCoords[5].z = position.z + treeShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[6].x = position.x + treeShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[6].y = position.y + treeShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[6].z = position.z + treeShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[7].x = position.x + treeShape->GetMinBox().x + nodeCenter.x;
		boxCoords[7].y = position.y + treeShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[7].z = position.z + treeShape->GetMinBox().y + nodeCenter.y;
		
		float testRadius = 0.0;
		
		for (int i=0;i<8;i++)
		{
			testRadius = glm::length(boxCoords[i]);
			OBBRadius = std::max(OBBRadius, testRadius);
		}

		appearType->boundsUpperLeftX = (-OBBRadius * 2.0);
		appearType->boundsUpperLeftY = (-OBBRadius * 2.0);
		   					 
		appearType->boundsLowerRightX = (OBBRadius * 2.0);
		appearType->boundsLowerRightY = (OBBRadius);
		
		if (!appearType->getDesignerTypeBounds())
		{
			if (auto* collision = appearType->getTreeCollisionShape(0)) {
				appearType->typeUpperLeft = collision->minBox;
				appearType->typeLowerRight = collision->maxBox;
			}
		}

		// GPU static-prop batcher: register the RENDER shape (override-or-stock)
		// so override geometry actually rasterizes (see BldgAppearance reg site
		// for the s_typeIndex rationale). Damage stays stock (out of MVP).
		// TREE-OVERRIDE-LOD-MVP M-C: for override types iterate ONLY populated
		// LODs (treeRenderShapeLodCount) so we register each distinct override
		// LOD shape once — NOT MAX_LODS times (which would upload LOD0 3× since
		// getTreeRenderShape clamps unpopulated indices back to LOD0). Stock
		// types keep the full MAX_LODS loop: getTreeRenderShape(i) returns the
		// distinct stock treeShape[i] per LOD, all of which must register.
		appearType->registerRenderShapes();
	}

	pitch = yaw = 0.0f;
	shadowMoving = false;
}

void TreeAppearance::setObjStatus (long oStatus)
{
	if (simulationOwned() && !simPresentationPass)
	{
		simStatus_ = oStatus;
		recordSimStatus(oStatus);
		return;
	}
	if (status != oStatus)
	{
        invalidateStaticRegistration();
        activeLOD = 0;
		if ((oStatus == OBJECT_STATUS_DESTROYED) || (oStatus == OBJECT_STATUS_DISABLED))
		{
			if (appearType->getTreeDamageRenderShape())
			{
				if (treeShape)
				{
					treeShape->ClearAnimation();
					treeShapeOwner_.reset();
					treeShape = nullptr;
				}
				
				treeShapeOwner_.reset(appearType->getTreeDamageRenderShape()->CreateFrom());
				treeShape = treeShapeOwner_.get();
				beenInView = false; 
			}
			
		}

		if (oStatus == OBJECT_STATUS_NORMAL)
		{
			if (appearType->treeShape[0])
			{
				if (treeShape)
				{
					treeShape->ClearAnimation();
					treeShapeOwner_.reset();
					treeShape = nullptr;
				}

				// MODEL-OVERRIDE dual-shape: restore per-instance RENDER shape
				// via the render accessor (override if present, else stock).
				treeShapeOwner_.reset(appearType->getTreeRenderShape(0)->CreateFrom());
				treeShape = treeShapeOwner_.get();
				beenInView = false;
			}

		}

		if (treeShape)
		{
			for (long i=0;i<treeShape->GetNumTextures();i++)
			{
				char txmName[1024];
				treeShape->GetTextureName(i,txmName,256);
		
				char texturePath[1024];
				mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (textureOrKtxSidecarExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						treeShape->SetTextureHandle(i,gosTextureHandle);
						treeShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						treeShape->SetTextureHandle(i,gosTextureHandle);
						treeShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					treeShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}

	}

	status = oStatus;
}

void TreeAppearance::setObjectParameters (const glm::vec3 &pos, float Rot, long sel, long team, long homeRelations)
{
	rotation = Rot;

	position = pos;

	selected = sel;

	actualRotation = Rot;

	teamId = team;
	homeTeamRelationship = homeRelations;
}

void TreeAppearance::setMoverParameters (float pitchAngle, float lArmRot, float rArmRot, bool isAirborne)
{
	if (pitchAngle != pitch) invalidateStaticRegistration();
	pitch = pitchAngle;
}

bool TreeAppearance::isMouseOver (float px, float py)
{
	if (inView)
	{
		if ((px <= lowerRight.x) && (py <= lowerRight.y) &&
			(px >= upperLeft.x) &&
			(py >= upperLeft.y))
		{
			return inView;
		}
		else
		{
			return FALSE;
		}
	}
	
	return(inView);
}	

bool TreeAppearance::recalcBounds (void)
{
	// F3 CPU projection cost-baseline: aggregate per-actor scope into the
	// recalcBounds_perframe bucket. No-op when env OFF.
	// Inline fast path: the Scope ctor/dtor and workload counter are out-of-line calls per object per frame.
	std::optional<::mc2_cpu_proj_cost::Scope> _f3_recalcBounds_scope;
	if (::mc2_cpu_proj_cost::g_cpuProjEnabled) {
		_f3_recalcBounds_scope.emplace(::mc2_cpu_proj_cost::BUCKET_RECALCBOUNDS_PERFRAME);
		::mc2_cpu_proj_cost::add_workload_recalcbounds(1);
	}
	// [TOBJSPLIT v1] accumulators declared in code/static_update_counters.h
	// (included above via ../code/static_update_counters.h).
	// Gate: file-scope s_tobjSplitBdOn (defined above, shared with BldgAppearance).

	setVisibilityGatesFromLegacy(false);

	if (eye)
	{
		setVisibilityGatesFromLegacy(true);

		// [TOBJSPLIT v1] ANGULAR bracket: matrix-free sphere angular clip.
		// Reads cycle counter immediately before/after.
		{
		unsigned long long _tsA = s_tobjSplitBdOn ? __rdtsc() : 0ULL;
		if (eye->usePerspective)
		{
			glm::vec3 cameraPos;
			cameraPos.x = -eye->getCameraOrigin().x;
			cameraPos.y = eye->getCameraOrigin().z;
			cameraPos.z = eye->getCameraOrigin().y;
			float vClipConstant = eye->verticalSphereClipConstant;
			float hClipConstant = eye->horizontalSphereClipConstant;

			glm::vec3 objectCenter;
			objectCenter = position - cameraPos;
			Camera::cameraFrame.trans_to_frame(objectCenter);
			float distanceToEye = glm::length(objectCenter);
			float clip_distance = fabs(1.0f / objectCenter.y);

			//Is vertex on Screen OR close enough to screen that its triangle MAY be visible?
			// Uses the tan of the angle we want above (no atans needed here).
			float object_angle = fabs(objectCenter.z) * clip_distance;
			float extent_angle = treeShape->GetExtentRadius() / distanceToEye;
			if (object_angle > (vClipConstant + extent_angle))
			{
				//In theory, we would return here.  Object is NOT on screen.
				setVisibilityGatesFromLegacy(false);
			}
			else
			{
				object_angle = fabs(objectCenter.x) * clip_distance;
				if (object_angle > (hClipConstant + extent_angle))
				{
					//In theory, we would return here.  Object is NOT on screen.
					setVisibilityGatesFromLegacy(false);
				}
			}
		}
		if (s_tobjSplitBdOn) g_tobjAngularCyc.fetch_add(__rdtsc() - _tsA, std::memory_order_relaxed);
		}  // end ANGULAR bracket

		// recalcBounds projection body deleted 2026-05-18 (Task 3, Tree mirror of Task 2):
		// the GPU compute cull (gpu_cull::readback_isActorVisibleLagged) is the
		// substitutive twin of the per-frame screen projection. inView is now
		// coarse-angular-only -- a strict superset of the old projected value;
		// over-inclusion is correctness-safe (cull_gates_are_load_bearing.md).
		// Trees are never pick targets (objmgr.cpp findObjectByMouse skips
		// getObjectClass()==TREE), so screenPos/upperLeft/lowerRight have no
		// pick-path consumer -- no Task-4 re-home needed for Tree.
		// LATENT HAZARD: the deleted block also held the per-LOD-swap texture
		// (re)loader, dead today under the 2026-05-12 TEMP LOD-0 pin
		// (selectLOD forced 0 in this function).
		// If that pin is reverted (when the LOD-1 invisibility root cause is
		// fixed), LOD selection + the per-LOD texture loader MUST be re-homed
		// BEFORE the revert lands -- TreeAppearance::init loads LOD-0 textures
		// ONLY; LOD-1+ would be unloaded after any LOD swap.
	}

	return(inView);
}

void TreeAppearance::recalcBoundsAndStamp() {
	// FRAME-JOBS-1 worker path. Do not call from game logic.
	extern std::atomic<uint32_t> g_mc2FrameCounter;
	if (boundsFrame == g_mc2FrameCounter.load(std::memory_order_relaxed)) return;
	recalcBounds();
	boundsFrame = g_mc2FrameCounter.load(std::memory_order_relaxed);
}

long TreeAppearance::update (bool animate)
{
	// Trees have no simulation animation; presentation owns their geometry and lights.
	if (!simPresentationPass && (simulationOwned() || mc2::overlap::onSimWorker())) return TRUE;
	mc2::overlap::assertMainThread("TreeAppearance::update");
	::mc2_object_recon::Scope _recon_tree_(
		&::mc2_object_recon::g_per_frame.tree_update_ns,
		&::mc2_object_recon::g_per_frame.tree_update_calls);
	if (rotation > 180)
		rotation -= 360;

	if (rotation < -180)
		rotation += 360;

	// TREE-OVERRIDE-LOD-MVP-1: per-frame active-LOD pick. Runs in BOTH update()
	// and touch() — registered trees take the touch() skip path every frame, so
	// selecting only in update() froze activeLOD at first registration.
	selectActiveLOD();

	glm::quat rot;
	float yawAngle = (rotation * DEGREES_TO_RADS) + (yaw * DEGREES_TO_RADS);
	float pitchAngle = (pitch * DEGREES_TO_RADS);
	rot = mc2::math::quatFromEuler(mc2::math::Euler(pitchAngle, yawAngle, 0.0f));

	glm::vec3 xlatPosition;
	mc2ToRender(xlatPosition, position);

	if (!fogLightSet)
	{
		unsigned char lightr,lightg,lightb;
		float lightIntensity = 1.0f;
		if (land)
			lightIntensity = land->getTerrainLight(position);

		lightr = eye->getLightRed(lightIntensity);
		lightg = eye->getLightGreen(lightIntensity);
		lightb = eye->getLightBlue(lightIntensity);

		lightRGB = (lightr<<16) + (lightg<<8) + lightb;

		fogRGB = 0xff<<24;
		float fogStart = eye->fogStart;
		float fogFull = eye->fogFull;

		if (xlatPosition.y < fogStart)
		{
			float fogFactor = fogStart - xlatPosition.y;
			if (fogFactor < 0.0)
				fogRGB = 0xff<<24;
			else
			{
				fogFactor /= (fogStart - fogFull);
				if (fogFactor <= 1.0)
				{
					fogFactor *= fogFactor;
					fogFactor = 1.0 - fogFactor;
					fogFactor *= 256.0;
				}
				else
				{
					fogFactor = 256.0;
				}

				unsigned char fogResult = float2long(fogFactor);
				fogRGB = fogResult << 24;
			}
		}
		else
		{
			fogRGB = 0xff<<24;
		}

		fogLightSet = true;
	}

	if (useFog)
		treeShape->SetFogRGB(fogRGB);
	else
		treeShape->SetFogRGB(0xffffffff);

	DWORD oldRGB = eye->getLightColor(1);

	eye->setLightColor(1,lightRGB);
	eye->setLightIntensity(1,1.0);

	if (forceLightsOut)
		treeShape->SetLightsOut(true);

	// Under the GPU static-prop path we need listOfColors / shapeToWorld
	// fresh every frame regardless of inView so submitMultiShape can safely
	// read shape->listOfVertices during submit().
	if (inView)
	{
		treeShape->SetUseShadow(false);

		TG_LightPtr light = eye->getWorldLight(0);
		light->active = false;


		treeShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
		// Slice 2 (object-offload) — Stage 2.B: eligibility hoist.
		// See BldgAppearance::update for the full rationale; same shape.
		// Branch lives INSIDE the existing inView cull
		// gate to preserve slice 1's R1 invariant.
		// PERF DIAGNOSTIC 2026-05-06: Tracy zones — see BldgAppearance::update
		// for the same instrumentation set. Same theories under investigation.
		bool gpuEligible;
		{
			gpuEligible = !needsFullBakeNextFrame &&
			              GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(treeShape);
		}

		if (gpuEligible)
		{
			// Stage 2.D.2 fix: cache GPU light data while lights are per-actor-correct.
			{
				// TREE-OVERRIDE-LOD-MVP K×M FIX (2026-06-03): per-instance light slot is
				// LOD-INDEPENDENT. treeShape is always the LOD0 render-instance and the
				// gather is position-keyed (terrain light by world pos), not geometry-keyed.
				// Bake ONCE keyed on LOD0 and SHARE that single slot with whatever LOD is
				// active -> light table stays U~=K (pre-LOD baseline) flat across LOD count,
				// never K×M. Drawn geometry still swaps per-LOD via staticReg[lod].recipeIndex.
				mc2CacheOrBakeStaticGpuLight(treeShape, staticReg[0].registered, staticReg[0].recipeIndex);
				mc2PropLightContractCheck(treeShape, land ? land->getTerrainLight(position) : 1.0f, mc2::prop_light::kSunOff);
				// 2026-05-11 per-instance capture (mirror of BldgAppearance::update).
				const uint32_t _sharedLightSlot = treeShape->getCachedGpuLightIndex();
				staticReg[0].lightDataIndex = _sharedLightSlot;
				staticReg[activeLOD].lightDataIndex = _sharedLightSlot;  // share LOD0 slot
				// [LIGHTSLOT v1] Task 0 cardinality gate (diagnostic only).
				mc2_lightslot_trace::recordInstance(
					appearType,
					appearType && appearType->treeRenderShapeLodCount > 0,
					staticReg[activeLOD].lightDataIndex);
			}
			{
				// GPU-INSTANCE-SKIP-POOLS-1 (2026-06-03): gpuEligible already
				// guarantees every leaf is registered in s_typeIndex, so geometry
				// is in the immutable per-type VBO and lighting is O(1) via
				// lightDataIndex. Legacy _PositionsOnly allocated all six TGL
				// frame pools sized to the mesh per visible instance (peak = Σ
				// over the forest → overflow on a heavy override mesh), yet the
				// GPU draw consumes NONE of that content (submit reads
				// rec.shapeToWorld + the debug-only zero-padded Colors SSBO). Run
				// the ZERO-POOL hierarchy walk instead — still populates
				// rec.shapeToWorld per leaf, no pool alloc.
				treeShape->TransformMultiShape_HierarchyOnly (&xlatPosition,&rot);
			}
		}
		else
		{
			// GPU-INSTANCE-SKIP-POOLS-1 (2026-06-03): this "full-bake" else-branch
			// is where registered override trees land — the gpuEligible branch
			// above is gated by !needsFullBakeNextFrame.
			// CORRECTION (2026-06-03, recon docs/model-override-lighting-lod-recon.md):
			// the earlier claim here — "32-slot light UBO returns UINT32_MAX under
			// forest contention, perpetually re-arming the full-bake latch" — is
			// STALE/FALSE. The static-prop light table is an UNBOUNDED grow-on-demand
			// SSBO (b41baec); there is no 32-slot cap and no allocator overflow
			// sentinel. cachedGpuLightIndex_==0xFFFFFFFF means "not yet cached / gather
			// didn't run", NOT overflow. Registered override trees use the persistent
			// baked-light table (MC2_LIGHTBAKE, O(1) re-ship) after frame 1. The
			// needsFullBakeNextFrame re-arm is the LOD-swap-black guard + mission-load
			// transient, not a lighting overflow. The forest's real cost is GPU
			// LOD/overdraw (trees pinned LOD0, full 706k-tri at all distances), not
			// CPU lighting. The pool-skip rationale below is independently verified and
			// stands regardless. Verified A/B: this
			// branch full-baking the 6×~535k-vert lush forest pegged the TGL pools
			// to 99% (legacy), yet the trees draw via the GPU registry/substrate
			// indirect path (buckets=227), NOT via the CPU TG_Shape::Render() that
			// reads the pools (submit_trees=0, gpu indirect_draw present). Pixel
			// A/B (same camera, legacy 99% vs skip 0%) shows IDENTICAL tree canopy.
			// So for a registered type the full-bake pool content is vestigial:
			// run the ZERO-POOL hierarchy walk to populate rec.shapeToWorld and
			// still call mc2CacheOrBakeStaticGpuLight below (pool-independent — it
			// only walks the hierarchy + GatherGpuObjectLightDataOnly). Result:
			// pool peak 99%→0% on the heavy forest, no visual change.
			treeShape->TransformMultiShape_HierarchyOnly (&xlatPosition,&rot);
			if (!GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(treeShape))
				treeShape->ReserveCpuDraw();	// no batcher takes it: the CPU fallback draws it
			// 2026-05-10: mirror of the BldgAppearance fix.
			// Seed cachedGpuLightIndex_ in the full-bake branch so the
			// next render() doesn't fail the UINT32_MAX gate
			// and invalidate the freshly-set staticReg.
			// TREE-OVERRIDE-LOD-MVP K×M FIX (2026-06-03): bake keyed on LOD0, share slot
			// to active LOD (see gpuEligible branch above for rationale).
			mc2CacheOrBakeStaticGpuLight(treeShape, staticReg[0].registered, staticReg[0].recipeIndex);
			mc2PropLightContractCheck(treeShape, land ? land->getTerrainLight(position) : 1.0f, mc2::prop_light::kSunOff);
			// 2026-05-11 per-instance capture (mirror of gpuEligible branch).
			const uint32_t _sharedLightSlot = treeShape->getCachedGpuLightIndex();
			staticReg[0].lightDataIndex = _sharedLightSlot;
			staticReg[activeLOD].lightDataIndex = _sharedLightSlot;  // share LOD0 slot
			// [LIGHTSLOT v1] Task 0 cardinality gate (diagnostic only). This is
			// the branch registered override trees actually land in (gpuEligible
			// above is gated by !needsFullBakeNextFrame).
			mc2_lightslot_trace::recordInstance(
				appearType,
				appearType && appearType->treeRenderShapeLodCount > 0,
				staticReg[activeLOD].lightDataIndex);
			needsFullBakeNextFrame = false;
		}

		light->active = true;

		if ((turn > 3) && useShadows)
			beenInView = true;
	}
	
	eye->setLightColor(1,oldRGB);

	return TRUE;
}

void TreeAppearance::markTerrain (_ScenarioMapCellInfo* pInfo, int type, int counter)
{
	//MUST force tree to HIGHEST LOD!!!  Impassability data is only valid at this LOD!!
	// Tree will reset its LOD on next draw!!
	if (currentLOD)
	{
		currentLOD = 0;
	
		treeShape->ClearAnimation();
		treeShapeOwner_.reset();
		treeShape = nullptr;
	
		treeShapeOwner_.reset(appearType->treeShape[currentLOD]->CreateFrom());
		treeShape = treeShapeOwner_.get();
	}

	for (int i=0;i<treeShape->GetNumShapes();i++)
	{
		//Check if the artists meant for this piece to NOT block passability!!
		if (S_strnicmp(treeShape->GetNodeId(i),"_PAB",4) != 0)
		{
			for (int j=0;j<treeShape->GetNumVerticesInShape(i);j++)
			{
				glm::vec3 vertexPos, worldPos;
				vertexPos = treeShape->GetShapeVertexInEditor(i,j,-rotation);
				worldPos = position + vertexPos;
	
				int cellR, cellC;
				land->worldToCell(worldPos,cellR,cellC);
				if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
					(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
				{
					continue;
				}
				_ScenarioMapCellInfo*pTmp = &(pInfo[cellR * Terrain::realVerticesMapSide * MAPCELL_DIM + cellC]);

				if (vertexPos.z >= 1.0f)
				{
					pTmp->forest = true;
						
					float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
					cellLocalHeight = std::min(cellLocalHeight, 15.0f);
						
					if (pTmp->lineOfSight < cellLocalHeight)
						pTmp->lineOfSight = cellLocalHeight+0.5f;
				}
			}
		}
	}
}

void TreeAppearance::markLOS (bool clearIt)
{
	// Line of sight is sim state: read it from the stock LOD0 shape for the sim status, never from the draw shape (its LOD
	// follows the camera and its damage swap is deferred to presentation).
	TG_MultiShapePtr los = treeShape;
	std::unique_ptr<TG_MultiShape> stockLos;
	if (simulationOwned())
	{
		const bool wrecked = simStatus_ == OBJECT_STATUS_DESTROYED || simStatus_ == OBJECT_STATUS_DISABLED;
		TG_TypeMultiShape* source = (wrecked && appearType->treeDmgShape) ? appearType->treeDmgShape.get() : appearType->treeShape[0].get();
		stockLos.reset(source->CreateFrom());
		los = stockLos.get();
	}
	else if (currentLOD)
	{
		currentLOD = 0;
	
		treeShape->ClearAnimation();
		treeShapeOwner_.reset();
		treeShape = nullptr;
	
		treeShapeOwner_.reset(appearType->treeShape[currentLOD]->CreateFrom());
		treeShape = treeShapeOwner_.get();
		los = treeShape;
	}

	for (int i=0;i<los->GetNumShapes();i++)
	{
		//Check if the artists meant for this piece to NOT block LOS!!
		// Probably should check for light cones,too!
		
		if ((S_strnicmp(los->GetNodeId(i),"LOS_",4) != 0) &&
			(S_strnicmp(los->GetNodeId(i),"SpotLight_",10) != 0))
		{
			for (int j=0;j<los->GetNumVerticesInShape(i);j++)
			{
				glm::vec3 vertexPos, worldPos;
				vertexPos = los->GetShapeVertexInEditor(i,j,-rotation);
				worldPos = position + vertexPos;
	
				int cellR, cellC;
				land->worldToCell(worldPos,cellR,cellC);
				if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
					(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
				{
					continue;
				}
				
				MapCellPtr curCell = GameMap->getCell(cellR, cellC);

				float currentCellHeight = curCell->getLocalHeight();
				
				float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
				cellLocalHeight = std::min(cellLocalHeight, 15.0f);
				
				if (!clearIt)
				{
					if (cellLocalHeight > currentCellHeight)
						GameMap->setLocalHeight(cellR, cellC, cellLocalHeight+0.5f);
				}
				else	//We want to clear all LOS height INFO.  We're about to change shape!!
				{
					GameMap->setLocalHeight(cellR, cellC, 0.0f);
				}
			}
		}
	}

	if (stockLos)
		stockLos->ClearAnimation();
}

bool TreeAppearance::IsStaticNow() const
{
	// TREE-OVERRIDE-LOD-MVP Task 2: the live per-instance render shape is the
	// active LOD's shape (LOD0 today; activeLOD pinned 0). The shape-key check
	// guards against treeShape reassignment (LOD/damage swaps).
	// TREE-OVERRIDE-LOD-MVP forced-LOD FIX (2026-06-03): the shape-key guards
	// treeShape reassignment, which only applies to the live LOD0 render-instance.
	// Higher LODs have no live per-instance shape (staticReg[lod>=1].shape==nullptr),
	// so the key is bypassed for activeLOD!=0 — markVisible replays the pre-registered
	// LOD recipe directly (drawing the correct LOD geometry).
    return !shadowIsMoving() && staticReg[activeLOD].registered
		&& (activeLOD == 0 ? staticReg[0].shape == treeShape : true)
		&& !needsFullBakeNextFrame;
}

void TreeAppearance::selectActiveLOD()
{
	// Per-frame distance-driven LOD pick. MUST run on BOTH update() and touch()
	// (registered trees take the touch() skip path under MC2_STATIC_UPDATE_SKIP=1).
	int _maxAvailLOD = 0;
	for (int _l = MAX_LODS - 1; _l > 0; --_l)
		if (staticReg[_l].registered) { _maxAvailLOD = _l; break; }
	int _wantLOD = activeLOD;   // default: STAY (deadband) — anti-thrash
	if (_maxAvailLOD >= 1 && eye) {
		constexpr float s_impostorDist = 800.0f;   // world units: the impostor switch distance
		// HYSTERESIS: separate near/far thresholds so a tree sitting near the
		// boundary does NOT oscillate activeLOD every frame. Oscillation forced a
		// full update()+submitMultiShape+re-register churn each frame, backing up
		// the GL command queue that then drained at SDL_GL_SwapWindow (25-47ms).
		const float _far  = s_impostorDist * 1.08f;
		const float _near = s_impostorDist * 0.92f;
		glm::vec3 _camP;
		_camP.x = -eye->getCameraOrigin().x;
		_camP.y =  eye->getCameraOrigin().z;
		_camP.z =  eye->getCameraOrigin().y;
		glm::vec3 _d; _d = position - _camP;
		const float _dist = glm::length(_d);
		if (activeLOD < _maxAvailLOD && _dist > _far)  _wantLOD = _maxAvailLOD; // far->impostor
		else if (activeLOD > 0       && _dist < _near) _wantLOD = 0;            // near->LOD0
	}
	_wantLOD = std::clamp(_wantLOD, 0, _maxAvailLOD);
	if (_wantLOD != activeLOD) {
		activeLOD = _wantLOD;
		// Only force a full re-bake if the target LOD isn't already registered+baked.
		// Switching between pre-registered LODs (shared LOD0 light slot) needs NO
		// re-bake — stay on the cheap touch() path; markVisible(staticReg[activeLOD])
		// in render() picks up the new recipe. This avoids the per-frame re-register
		// churn that stalled SDL_GL_SwapWindow.
		if (!staticReg[activeLOD].registered)
			needsFullBakeNextFrame = true;
	}
}

void TreeAppearance::destroy (void)
{
	invalidateStaticRegistration(); // Stage 3.C: NULL RecipeRange::multi before treeShape is freed

	if ( treeShape )
	{
		treeShapeOwner_.reset();
		treeShape = nullptr;
	}

	releaseAppearanceType(registryGeneration, appearType); // skipped when the registry that issued appearType is gone
}
