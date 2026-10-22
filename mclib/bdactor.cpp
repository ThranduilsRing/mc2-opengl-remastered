//	bdactor.cpp - This file contains the code for the building and tree appearance classes
//	MechCommander 2
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "texture_assets.h"
#ifndef BDACTOR_H
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include"bdactor.h"
#endif

#include "bdactor_internal.h"
#include "appearance_rng.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"

// File-scope gate: one gate read per TU, process-start-constant, shared across
// BldgAppearance::recalcBounds and TreeAppearance::recalcBounds. Matches the
// file-scope pattern in code/terrobj.cpp.
bool s_tobjSplitBdOn = mc2::gate::get<"MC2_TOBJ_COST_SPLIT">();

// BLDG-TYPE-ANIM-GATE-FIX-1 diagnostic counters (cumulative).
// s_animTypeIdleNowStatic is atomic: FRAME-JOBS-2 will run BldgAppearance::touch()
// on worker threads; this counter is incremented there. relaxed ordering is
// sufficient — it is a diagnostic accumulator, not a synchronisation point.
std::atomic<uint32_t> s_animTypeIdleNowStatic{0};
uint32_t s_animStartInvalidated    = 0;
uint32_t s_animStateToStateGesture = 0;

// Slim-deploy texture gate (LOAD_TGA_THEN_GPU_KTX). The static-prop / building /
// tree appearance loaders below gate loadTexture() behind fileExists(<name>.tga).
// fileExists is .ktx2-blind, so when a slim deploy drops the redundant .tga the
// gate fails, loadTexture is never called, and route-2 (txmmgr.cpp,
// MC2_TEXMGR_KTX_PRIMARY) -- which builds a usable node from the BC7 .ktx2
// sidecar alone -- is never reached; the static-prop batcher then skips the
// packet (layer=-1) and buildings vanish. This helper lets the gate also pass
// when a same-stem .ktx2 sidecar exists, gated in parity with route-2 (only when
// MC2_TEXMGR_KTX_PRIMARY is enabled). bdactor-only by design: mech/vehicle paint
// (CPU_RGBA_REQUIRED, gos_LockTexture rewrite) must stay .tga and is NOT routed
// here. See docs/texture-residency-registry-recon.md.
bool textureOrKtxSidecarExists(const char* tgaPath)
{
	if (fileExists(tgaPath))
		return true;
	const bool ktxPrimary = (mc2::gate::get<"MC2_TEXMGR_KTX_PRIMARY">() != 0);   // default mode 1
	if (!ktxPrimary)
		return false;
	char ktx[1024];
	strncpy(ktx, tgaPath, sizeof(ktx) - 1);
	ktx[sizeof(ktx) - 1] = 0;
	char* dot   = strrchr(ktx, '.');
	char* slash = strrchr(ktx, '/');
	if (dot && (!slash || dot > slash)) *dot = 0;
	if (strlen(ktx) + 6 < sizeof(ktx)) strcat(ktx, ".ktx2");
	return fileExists(ktx);
}

bool staticRegIsNaturalBuildingName(const char* name)
{
    if (!name)
        return false;
    return (std::strncmp(name, "Pine", 4) == 0) ||
           (std::strncmp(name, "Oak", 3) == 0) ||
           (std::strncmp(name, "Birch", 5) == 0) ||
           (std::strncmp(name, "Willow", 6) == 0) ||
           (std::strncmp(name, "rock_", 5) == 0);
}

// STATIC-PROP REGISTRATION CONTRACT v1: surface (rate-limited) when the GPU static
// registry REJECTED a visibility submit for a registered actor — i.e. the actor held a
// dead/tombstoned recipe handle and would have vanished, but the contract routed it to
// the legacy full-bake fallback instead. Capped at 32 lines so it confirms the failure
// mode without spamming a long session (AMD never hits it — its registry isn't stale).
// reason values match GpuStaticPropRegistry::StaticSubmitResult.
void bdLogStaticRegInvalid(const char* kind, int reason, int regIdx, const char* name) {
	static int s_logged = 0;
	if (s_logged >= 32) return;
	++s_logged;
	static const char* const kReason[] = {
		"Submitted", "NotRegistered", "MissingRange", "Tombstoned", "InvalidRecipe" };
	const char* rn = (reason >= 0 && reason < 5) ? kReason[reason] : "?";
	mc2::fmt::print_err("[STATIC_REG_INVALID] kind={} reason={} regIdx={:d} actor={} (falling back to legacy re-bake)\n", mc2::fmt::safe(kind), mc2::fmt::safe(rn), mc2::fmt::as_i32(regIdx), mc2::fmt::safe(name ? name : "(null)"));
	fflush(stderr);
}

// NS3 boundary: engine-canonical definitions (sole consumer is this TU + editor).
// Previously redefined in every game/tool main. Now defined here so the editor
// can link mclib directly without code/mechcmd2.cpp.
int ObjectTextureSize = 128;
bool reloadBounds = false;
MidLevelRenderer::MLRClipper * theClipper = nullptr;

// class BldgAppearance
void BldgAppearance::setWeaponNodeUsed (long weaponNode)
{
	weaponNode -= appearType->numWeaponNodes;
   	if ((weaponNode >= 0) && (weaponNode < appearType->numWeaponNodes))
	{
		unitAnim().nodeUsed[weaponNode]++;
		unitAnim().nodeRecycle[weaponNode] = BASE_NODE_RECYCLE_TIME;
	}
}

glm::vec3 BldgAppearance::getWeaponNodePosition (long nodeId)
{
	glm::vec3 result = position;
	if ((nodeId < 0) || (nodeId >= appearType->numWeaponNodes))
		return result;
	
	//We already know we are using this node.  Do NOT increment recycle or nodeUsed!
		
   	// Create Matrix to conform to.
   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = TerrainRuntime::groundElevation(position);
   	xlatPosition.z = position.y;
   
   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(turretYaw * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeId == -1)
	{
		if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
			rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
		else
			rotationalNodeId = -2;
	}
   
	if (rotationalNodeId >= 0)
	   	bldgShape->SetNodeRotation(rotationalNodeId,&torsoRot);

	result = bldgShape->GetTransformedNodePosition(&xlatPosition,&qRotation,appearType->nodeData[nodeId].nodeId);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
 	return result;
}

glm::vec3 BldgAppearance::getHitNode (void)
{
	if (hitNodeId == -1)
		hitNodeId = bldgShape->GetNodeNameId("hitnode");

	glm::vec3 result = getNodeIdPosition(hitNodeId);
 	return result;
}

long BldgAppearance::getWeaponNode (long weaponType)
{
	// Scan all weapon nodes and find least used one.
	long leastUsed = 999999999;
	long bestNode = -1;
	for (long i=0;i<appearType->numWeaponNodes;i++)
	{
		if (unitAnim().nodeUsed[i] < leastUsed)
		{
			leastUsed = unitAnim().nodeUsed[i];
			bestNode = i;
		}
	}
		
   	if ((bestNode < 0) || (bestNode >= appearType->numWeaponNodes))
   		return -1;

 	return bestNode;
}
		
float BldgAppearance::getWeaponNodeRecycle (long node)
{
	if ((node >= 0) && (node < appearType->numWeaponNodes))
		return unitAnim().nodeRecycle[node];
		
	return 0.0f;
}

void BldgAppearance::init (AppearanceTypePtr tree, GameObjectPtr obj)
{
	Appearance::init(tree,obj);
	appearType = static_cast<BldgAppearanceType *>(tree);

	shapeMin.x = shapeMin.y = -25;
	shapeMax.x = shapeMax.y = 50;

    status = OBJECT_STATUS_NORMAL; // sebi: init so will not be garbage

	unitAnim().bdAnimationState =-1;
	unitAnim().currentFrame = 0.0f;
	unitAnim().bdFrameRate = 0.0f;
	unitAnim().isReversed = false;
	unitAnim().isLooping = false;
	unitAnim().setFirstFrame = false;
	unitAnim().canTransition = true;

	// Slice 2 (object-offload) substrate: never set true in Stage 2.A.
	needsFullBakeNextFrame = false;

	// Stage 3.D: zero-init static-registry state (mirror of TreeAppearance::init).
	staticReg = {};

	paintScheme = -1;
	objectNameId = 30469;
	hazeFactor = 0.0f;

	rotationalNodeId = -1;
	hitNodeId = activityNodeId = activityNode1Id = -1;

	currentFlash = duration = flashDuration = 0.0f;
	flashColor = 0x00000000;
	drawFlash = false;

	pointLight = nullptr;
	lightId = 0xffffffff;
	forceLightsOut = false;

	// (E) T1.4: lazy-init key for SpotLight_ children. Vectors default-init
	// to empty; the first-night-visibility check in update() switches this
	// to true after the one-shot bldgShape walk. World position is invalid
	// (zero) at init() time per C-r1 C1, so registration is deferred.
	spotlightsRegistered_ = false;
	
	screenPos.x = screenPos.y = screenPos.z = screenPos.w = -999.0f;
	position = glm::vec3(0.0f);
	rotation = 0.0f;
	selected = 0;
	teamId = -1;
	homeTeamRelationship = 0;
	actualRotation = rotation;

	currentLOD = 0;
 	
	turretYaw = turretPitch = 0.0f;
	
	destructFX = nullptr;
	activity = nullptr;
	activity1 = nullptr;
	isActivitying = false;

	OBBRadius = -1.0f;
	highZ = -1.0f;
	
	
	beenInView = false;
	buildingPbrRenderActive = false;

	fogLightSet = false;
	if (appearType)
	{
		// MODEL-OVERRIDE dual-shape: build the per-instance RENDER shape from the
		// render accessor (override if present, else stock). Collision rebuilds
		// this same member from stock bldgShape[lod] when it needs passability.
		bldgShape = appearType->getBldgRenderShape(0)->CreateFrom();
		buildingPbrRenderActive = appearType->buildingPbrEnabled;

		// Load the texture and store its handle.
		for (int i=0;i<bldgShape->GetNumTextures();i++)
		{
			char txmName[1024];
			bldgShape->GetTextureName(i,txmName,256);

			char texturePath[1024];
			mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
	
			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");
	
			if (textureOrKtxSidecarExists(textureName))
			{
				const bool forceOpaquePbrTexture = buildingPbrRenderActive &&
					!(appearType && appearType->buildingPbrPreserveLegacyAlpha);
				if (!forceOpaquePbrTexture && S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					bldgShape->SetTextureHandle(i,gosTextureHandle);
					bldgShape->SetTextureAlpha(i,true);
				}
				else
				{
					DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					bldgShape->SetTextureHandle(i,gosTextureHandle);
					bldgShape->SetTextureAlpha(i,false);
				}
			}
			else
			{
				//PAUSE(("Warning: %s texture name not found",textureName));
				bldgShape->SetTextureHandle(i,0xffffffff);
			}
		}
		
		glm::vec3 boxCoords[8];
		glm::vec3 nodeCenter = bldgShape->GetRootNodeCenter();

		boxCoords[0].x = position.x + bldgShape->GetMinBox().x + nodeCenter.x;
		boxCoords[0].y = position.y + bldgShape->GetMinBox().z + nodeCenter.z;
		boxCoords[0].z = position.z + bldgShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[1].x = position.x + bldgShape->GetMinBox().x + nodeCenter.x;
		boxCoords[1].y = position.y + bldgShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[1].z = position.z + bldgShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[2].x = position.x + bldgShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[2].y = position.y + bldgShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[2].z = position.z + bldgShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[3].x = position.x + bldgShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[3].y = position.y + bldgShape->GetMinBox().z + nodeCenter.z;
		boxCoords[3].z = position.z + bldgShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[4].x = position.x + bldgShape->GetMinBox().x + nodeCenter.x;
		boxCoords[4].y = position.y + bldgShape->GetMinBox().z + nodeCenter.z;
		boxCoords[4].z = position.z + bldgShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[5].x = position.x + bldgShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[5].y = position.y + bldgShape->GetMinBox().z + nodeCenter.z;
		boxCoords[5].z = position.z + bldgShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[6].x = position.x + bldgShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[6].y = position.y + bldgShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[6].z = position.z + bldgShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[7].x = position.x + bldgShape->GetMinBox().x + nodeCenter.x;
		boxCoords[7].y = position.y + bldgShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[7].z = position.z + bldgShape->GetMinBox().y + nodeCenter.y;
		
 		float testRadius = 0.0;
		
		for (int i=0;i<8;i++)
		{
			testRadius = glm::length(boxCoords[i]);
			if (OBBRadius < testRadius)
				OBBRadius = testRadius;

			if (boxCoords[i].z > highZ)
				highZ = boxCoords[i].z;
		}
		
		appearType->boundsUpperLeftX = (-OBBRadius * 2.0);
		appearType->boundsUpperLeftY = (-OBBRadius * 2.0);
		   					 
		appearType->boundsLowerRightX = (OBBRadius * 2.0);
		appearType->boundsLowerRightY = (OBBRadius);
		
		if (!appearType->getDesignerTypeBounds())
		{
			glm::vec3 nodeCenter = bldgShape->GetRootNodeCenter();
			appearType->typeUpperLeft = bldgShape->GetMinBox() + nodeCenter;
			appearType->typeLowerRight = bldgShape->GetMaxBox() + nodeCenter;
		}
		
		if (appearType->numWeaponNodes > 0)
		{
			unitAnim().nodeUsed.assign(static_cast<size_t>(appearType->numWeaponNodes), 0);
			unitAnim().nodeRecycle.assign(static_cast<size_t>(appearType->numWeaponNodes), 0.0f);
		}

		// Register this building's TG_TypeShape variants with the GPU static
		// prop batcher. Idempotent -- duplicate calls across instances are
		// cheap. Covers all LOD base shapes plus destroyed/damaged variants
		// and their shadow proxies. Registration happens after texture
		// handles are resolved so packets capture the correct GL handle.
		// MODEL-OVERRIDE: register the RENDER shape (override-or-stock) so the
		// batcher's s_typeIndex holds the override type-shapes that the
		// per-instance bldgShape (CreateFrom'd from getBldgRenderShape) submits
		// against. Registering the stock shape here would make submit() miss
		// (render type-shape not in s_typeIndex) -> CPU-fallback/cull, and the
		// override would never rasterize. Damage stays stock (out of MVP).
		const bool _bldgIsOverride = (appearType->bldgRenderShape != nullptr);
		for (int i = 0; i < MAX_LODS; ++i)
			GpuStaticPropBatcher::instance().registerMultiShape(appearType->getBldgRenderShape(i), _bldgIsOverride);
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->bldgDmgShape);
	}
}

// STATIC-BLDG-SHADOW-REBAKE-1: forward decl -- defined near
// BldgAppearance::invalidateStaticRegistration() further down this file (its
// comment there has the full rationale); needed here because the REAL
// in-game combat-destroy event is this function's OBJECT_STATUS_DESTROYED
// transition, which runs long before that definition in file order.

void BldgAppearance::setObjStatus (long oStatus)
{
	if (status != oStatus)
	{
		// STATIC-BLDG-SHADOW-REBAKE-1: this is the REAL mid-mission "building
		// destroyed" event (combat damage reaching the threshold ->
		// Building::setDamage -> here). Only OBJECT_STATUS_DESTROYED, not
		// OBJECT_STATUS_DISABLED (disabled is a reversible non-destroy state
		// -- e.g. a powered-down building -- and should keep its shadow).
		// MEASURED (2026-07-16, MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_
		// DESTROY_AT_FRAME repro on mc2_24): re-arming the shadow latch alone
		// and relying on the ordinary shape-swap auto-detection (a LATER
		// touch()/update() noticing staticReg.shape != bldgShape and calling
		// invalidateStaticRegistration() + re-registering) is too slow -- the
		// rebake this trigger causes fires on the VERY NEXT FRAME, before
		// that lazy detection has run, so the first replay still baked the
		// stale pre-destruction shadow (inst count unchanged: 299 -> 299).
		// Fix: tombstone the static registration SYNCHRONOUSLY here (the same
		// invalidateStaticRegistration() the lazy path would eventually call)
		// so the registry is already correct by the time the rebake reads it.
		// Safe/idempotent: invalidateStaticRegistration() is already called
		// unconditionally from many shape-swap sites regardless of the
		// shadow gate; clearing staticReg here just means the NEXT touch()
		// takes the ordinary fresh-registration path (registered==false) for
		// the new rubble shape instead of the shape-swap path -- same
		// eventual outcome, no double-invalidate risk (the guard inside is
		// `if (staticReg.registered ...)`).
		if (oStatus == OBJECT_STATUS_DESTROYED) {
			const bool wasRegistered = staticReg.registered;
			invalidateStaticRegistration();
		}

		if ((oStatus == OBJECT_STATUS_DESTROYED) || (oStatus == OBJECT_STATUS_DISABLED))
		{
			if (appearType->bldgDmgShape)
			{
				if (bldgShape)
				{
					bldgShape->ClearAnimation();
					delete bldgShape;
					bldgShape = nullptr;
				}
				
				bldgShape = appearType->bldgDmgShape->CreateFrom();
				buildingPbrRenderActive = false;
				if (unitAnim().bdAnimationState != -1)
					appearType->setAnimation(bldgShape,unitAnim().bdAnimationState);
				
				beenInView = false; 
				currentLOD = 0;
			}
			
			stopActivity();
		}
		
		if (oStatus == OBJECT_STATUS_NORMAL)
		{
			if (appearType->bldgShape[0])
			{
				if (bldgShape)
				{
					bldgShape->ClearAnimation();
					delete bldgShape;
					bldgShape = nullptr;
				}

				// MODEL-OVERRIDE dual-shape: restore the per-instance RENDER
				// shape via the render accessor (override if present, else stock).
				bldgShape = appearType->getBldgRenderShape(0)->CreateFrom();
				buildingPbrRenderActive = appearType->buildingPbrEnabled;
				if (unitAnim().bdAnimationState != -1)
					appearType->setAnimation(bldgShape,unitAnim().bdAnimationState);

				beenInView = false;
			}

		}

		if (bldgShape)
		{
			// Load the texture and store its handle.
			for (long i=0;i<bldgShape->GetNumTextures();i++)
			{
				char txmName[1024];
				bldgShape->GetTextureName(i,txmName,256);
	
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
						bldgShape->SetTextureHandle(i,gosTextureHandle);
						bldgShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						bldgShape->SetTextureHandle(i,gosTextureHandle);
						bldgShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					//PAUSE(("Warning: %s texture name not found",textureName));
					bldgShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}

	}

	status = oStatus;
}

glm::vec3 BldgAppearance::getNodeNamePosition (const char *nodeName)
{
	glm::vec3 result = position;
	
   	// Create Matrix to conform to.
   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);
   
	result = bldgShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeName);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

glm::vec3 BldgAppearance::getNodeIdPosition (long nodeId)
{
	glm::vec3 result = position;
	
   	// Create Matrix to conform to.
   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);
   
	result = bldgShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeId);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

bool BldgAppearance::PerPolySelect (long mouseX, long mouseY)
{
	return bldgShape->PerPolySelect(mouseX,mouseY);
}

void BldgAppearance::setGesture (unsigned long gestureId)
{
	// Check if state is possible.
	if (gestureId >= MAX_BD_ANIMATIONS)
		return;

	// Check if object destroyed.  If so, no animation!
	if ((status == OBJECT_STATUS_DESTROYED) || (status == OBJECT_STATUS_DISABLED))
		return;
		
	if (gestureId == unitAnim().bdAnimationState)
		return;

	const bool naturalStaticExempt =
		appearType && staticRegIsNaturalBuildingName(appearType->name) && isStaticEligible();

	// BLDG-TYPE-ANIM-GATE-FIX-1: invalidate static registration on idle→animated.
	// When an idle building (bdAnimationState == -1) that is registered as a
	// static prop receives its first gesture, the GPU registry holds a stale
	// static-pose recipe. Invalidate before state change so the static draw is
	// removed before CPU animation begins. needsFullBakeNextFrame is NOT set —
	// invalidateStaticRegistration() clears staticReg, making IsStaticNow()
	// false, and the next update() runs the full bake naturally.
	// State-to-state (bdAnimationState != -1 → new state): already dynamic,
	// not registered, no action needed.
	if (unitAnim().bdAnimationState == -1 && staticReg.registered) {
		// Some natural props are authored as BUILDING appearances and receive a
		// dummy gesture=0 startup state even though there is no real animation
		// payload. Keep their static registration intact so pure render-static
		// trees/rocks remain skippable, while real animated/service buildings
		// still invalidate and wake normally.
		if (!naturalStaticExempt) {
			invalidateStaticRegistration();
			++s_animStartInvalidated;
		}
	}
	// Count all setGesture calls while already in a non-idle state.
	// Not an animation-end counter — bdAnimationState never returns to -1 after
	// the first setGesture() call: BldgAppearance has no idle-reset path, and
	// setGesture() itself guards gestureId >= MAX_BD_ANIMATIONS (blocking the
	// unsigned cast of -1). If a future refactor adds an idle-reset, this block
	// will need re-evaluation.
	if (unitAnim().bdAnimationState != -1) {
		++s_animStateToStateGesture;
	}

	// If state is OK, set animation data, set first frame, set loop and
	// reverse flag, and start it going until you hear otherwise.
	appearType->setAnimation(bldgShape,gestureId);
	unitAnim().bdAnimationState = gestureId;
	unitAnim().currentFrame = 0.0f;
	if (appearType->bdStartF[gestureId])
		unitAnim().currentFrame = appearType->bdStartF[gestureId];
		
	unitAnim().isReversed = false;
	
	if (appearType->isReversed(unitAnim().bdAnimationState))
	{
		unitAnim().currentFrame = appearType->getNumFrames(unitAnim().bdAnimationState)-1;
		unitAnim().isReversed = true;
	}
	
	if (appearType->isRandom(unitAnim().bdAnimationState))
	{
		unitAnim().currentFrame = mc2_anim_rng::randomNumber(appearType->getNumFrames(unitAnim().bdAnimationState)-1);
	}
	
	unitAnim().isLooping = appearType->isLooped(unitAnim().bdAnimationState);
	
	unitAnim().bdFrameRate = appearType->getFrameRate(unitAnim().bdAnimationState);
	
	unitAnim().setFirstFrame = true;
	if (unitAnim().bdFrameRate > Stuff::SMALL)
		unitAnim().canTransition = false;
	else
		unitAnim().canTransition = true;		//We can change immediately to another animation because we have no animation for this state!
}

void BldgAppearance::setMoverParameters (float turretRot, float lArmRot, float rArmRot, bool isAirborne)
{
	turretYaw = turretRot;
	turretPitch = lArmRot;
}

void BldgAppearance::setObjectParameters (const glm::vec3 &pos, float Rot, long sel, long team, long homeRelations)
{
	rotation = Rot;

	position = pos;

	selected = sel;

	actualRotation = Rot;

	teamId = team;
	homeTeamRelationship = homeRelations;
}

bool BldgAppearance::isMouseOver (float px, float py)
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

bool BldgAppearance::recalcBounds (void)
{
	// F3 CPU projection cost-baseline: aggregate per-actor scope into the
	// recalcBounds_perframe bucket. No-op when env OFF.
	::mc2_cpu_proj_cost::Scope _f3_recalcBounds_scope(
	    ::mc2_cpu_proj_cost::BUCKET_RECALCBOUNDS_PERFRAME);
	::mc2_cpu_proj_cost::add_workload_recalcbounds(1);
	// [TOBJSPLIT v1] accumulators declared in code/static_update_counters.h
	// (included above via ../code/static_update_counters.h).
	// Gate: file-scope s_tobjSplitBdOn (defined above, shared with TreeAppearance).

	setVisibilityGatesFromLegacy(false);

	if (eye)
	{
		//NEW METHOD from the WAY BACK Days
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
			float extent_angle = bldgShape->GetExtentRadius() / distanceToEye;
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

		// recalcBounds projection body deleted 2026-05-18 (Task 2): the GPU
		// compute cull (gpu_cull::readback_isActorVisibleLagged) is the
		// substitutive twin of the per-frame screen projection. inView is now
		// coarse-angular-only -- a strict superset of the old projected value;
		// over-inclusion is correctness-safe (cull_gates_are_load_bearing.md).
		// screenPos/upperLeft/lowerRight are computed lazily at pick time
		// (objmgr.cpp findTerrainObjectByMouse, Task 4).
		// LATENT HAZARD: the deleted block also held the per-LOD-swap texture
		// (re)loader, dead today under the 2026-05-12 TEMP LOD-0 pin
		// (selectLOD forced 0 / (void)useHighObjectDetail in this function).
		// If that pin is reverted (when the LOD-1 invisibility root cause is
		// fixed), LOD selection + the per-LOD texture loader MUST be re-homed
		// BEFORE the revert lands -- BldgAppearance::init loads LOD-0 textures
		// ONLY; LOD-1+ would be unloaded after any LOD swap.
	}

	return(inView);
}

void BldgAppearance::recalcBoundsAndStamp() {
	// FRAME-JOBS-1 worker path. Do not call from game logic.
	extern uint32_t g_mc2FrameCounter;
	if (boundsFrame == g_mc2FrameCounter) return;
	recalcBounds();
	boundsFrame = g_mc2FrameCounter;
}

bool BldgAppearance::playDestruction (void)
{
	//Check if there is a Destruct FX
	if (appearType->destructEffect[0])
	{
		// Yes, load it on up.
		unsigned flags = gosFX::Effect::ExecuteFlag;

		Check_Object(gosFX::EffectLibrary::Instance);
		gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(appearType->destructEffect);
		
		if (gosEffectSpec)
		{
			destructFX = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(destructFX != nullptr);
		
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		
			glm::vec3			tPosition;
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			
            //Stuff::Vector3D offsetPosition;
			//offsetPosition.x = Terrain::worldUnitsPerVertex / 3.0f;
			//offsetPosition.y = -(Terrain::worldUnitsPerVertex / 3.0f);
			//offsetPosition.z = 0.0f;

			//OppRotate(offsetPosition,rotation);

			glm::vec3 actualPosition = position;
			//actualPosition.Add(position,offsetPosition);

			mc2ToRender(tPosition, actualPosition);

			float yaw = (180.0f + rotation) * DEGREES_TO_RADS;
			glm::quat rot;
			rot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));

			mc2::math::buildRotation(shapeOrigin, rot);
			mc2::math::buildTranslation(shapeOrigin, tPosition);
			
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,nullptr);
			destructFX->Start(&info);
			
			return true;
		}
		
		return false;
	}
	
	return false;		//We didn't have a destruct effect.  Tell the object to play its default.
}

// BLDG-TYPE-ANIM-GATE-FIX-1 counter accessors (header: bldg_anim_gate_counters.h).
uint32_t g_bldgAnimGate_typeIdleNowStatic()    { return s_animTypeIdleNowStatic.load(std::memory_order_relaxed); }
uint32_t g_bldgAnimGate_animStartInvalidated() { return s_animStartInvalidated; }
uint32_t g_bldgAnimGate_animStateToState()     { return s_animStateToStateGesture; }
