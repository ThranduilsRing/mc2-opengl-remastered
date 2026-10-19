#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include <algorithm>
#include "mc2fmt.h"
#include "utils/mc2log.h"
// Mech3DAppearance: Mech3DAppearance::init, initFX, hit reactions, paint schemata and paint scheme.
// Declared in the class headers; shared names in mech3d_internal.h.
#include "mech3d_internal.h"
#include "appearance_rng.h"

// Returns true if the texture file or its .ktx2 BC7 sidecar exists.
// Used to gate TGL texture loading so sensor shapes work when .tga is absent
// in a slim deploy (route-2 in txmmgr.cpp will decode the .ktx2 instead).
static bool textureOrKtxSidecarExists(const char* tgaPath)
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

#define EXPAND_FACTOR 		(1.25f)

constexpr int FX_POOF_ID = 32;
constexpr int FX_JUMP_ID = 43;
constexpr int FX_SMOKE_ID = 48;

void Mech3DAppearance::init (AppearanceTypePtr tree, GameObjectPtr obj)
{
	Appearance::init(tree,obj);
	// M2: defensive reset. Guards against in-place re-init without a prior
	// delete/new (which would carry a stale handle into the new instance).
	// The adapter's syncSpawn assert fires on a valid handle, making
	// accidental double-registration visible in debug builds.
	mechRenderHandle = RenderCore::RenderObjectHandle::invalid();
	mechType = static_cast<Mech3DAppearanceType *>(tree);

	// C3: cache owner handle for GPU-cull node-position early-outs.
	actorHandle_ = (obj != nullptr) ? obj->getHandle() : -1;
	LC3D_TRACE("event=init actorHandle=%ld lifecycle=%d", actorHandle_, static_cast<int>(s_gpuCullLifecycle));

	mechName[0] = 0;

	pilotNameID = -1;

	paintScheme = -1;
	objectNameId = 30862;

	baseRootNodeHeight = -99999.9f;
	baseRootNodeDifference = 0.0f;

	rotationalNodeIndex = -1;
	hitLeftNodeIndex = hitRightNodeIndex = -1;
	rootNodeIndex = lightCircleNodeIndex = -1;
	leftArmNodeIndex = rightArmNodeIndex = -1;

	// (E) T1.6: lazy-init key for SpotLight_ children. Vectors default-init
	// to empty. lightCircleNodeIndex above is for the legacy anubis
	// SLCircle_anubis path; SpotLight_ is a different node-name prefix.
	spotlightsRegistered_ = false;

	screenPos.x = screenPos.y = screenPos.z = screenPos.w = -999.0f;
	
	frameNum = 0.0f;
	
	rightShoulderPos.x = rightShoulderPos.y = rightShoulderPos.z = -99999.9f;
	leftShoulderPos.x = leftShoulderPos.y = leftShoulderPos.z = -99999.9f;

	idleTime = mc2_appearance_rng::randomNumber(idleMAX);
	
	hazeFactor = 0.0f;

	rotation = torsoRotation = leftArmRotation = rightArmRotation = 0;

	leftArmOff = rightArmOff = fallen = false;

	position.x = position.y = position.z = 0.0f;

	velocity = 0.0f;

	teamId = paintScheme = -1;
	homeTeamRelationship = 0;

	selected = 0;

	status = 0;

	lockRotation = oncePerFrame = false;

	sensorLevel = 0;
	sensorSpin = 0.0f;

	OBBRadius = 0.0f;
	currentLOD = 0;
	
	jumpVelocity = glm::vec3(0.0f);
	jumpFXSetup = false;
	

	currentRightPoof = currentLeftPoof = 0;
	
	localTextureHandle = 0xffffffff;
	
	isSmoking = -1;
	isWaking = false;
	isDusting = false;
	movedThisFrame = false;
	fallDust = false;
	criticalSmoke = nullptr;
	smokeEffect = nullptr;
	waterWake = nullptr;
	helicopterDustCloud = nullptr;
	isHelicopter = false;
	
	leftArm = rightArm = nullptr;
	rightArmSmoke = nullptr;
	leftArmSmoke = nullptr;

	currentFlash = duration = flashDuration = 0.0f;
	flashColor = 0x00000000;
	drawFlash = false;

	psRed = psGreen = psBlue = 0xffffffff;

	// DEGRADE-DON'T-CRASH: ensure the instance shape is NULL before the build
	// block so a partially-imported mech (missing appearance .ini / missing
	// TGLData / missing base-LOD .ase) leaves a well-defined NULL, not garbage.
	// Mech3DAppearanceType::init() falls through its STOP()/gosASSERT() (both
	// no-ops in RelWithDebInfo) when those assets are absent, leaving
	// mechType->mechShape[0] == NULL. Without this guard mechShape =
	// mechType->mechShape[0]->CreateFrom() below derefs NULL and crashes the
	// encyclopedia / mech-bay preview. The per-frame render()/update()/
	// recalcBounds() paths early-out on the resulting NULL mechShape.
	mechShape = nullptr;

	if (mechType && mechType->mechShape[0])
	{
		// MC2_LOAD_RACE_TRACE: count successful shape builds, split by whether
		// this is a real world actor (obj != NULL, e.g. BattleMech::init) or a
		// preview/logistics instance (obj == NULL, e.g. SimpleCamera::setMech).
		// Lets the verdict distinguish "mission mechs degraded" (the real bug)
		// from "only previews degraded" (benign).
		extern bool g_loadRaceTrace;
		extern long g_loadRaceMechOkReal;
		extern long g_loadRaceMechOkPreview;
		if (g_loadRaceTrace) {
			if (obj) g_loadRaceMechOkReal++; else g_loadRaceMechOkPreview++;
		}

		// Re-register all LODs with the GPU mech batcher. GpuMechBatcher::onMapLoad()
		// clears s_typeLodIndex between sessions, but AppearanceTypeList caches the
		// Mech3DAppearanceType so Mech3DAppearanceType::init() (which calls
		// registerTypeLod) is never called again for cached types. Without this,
		// submitActor() finds the type absent from s_typeLodIndex and returns false,
		// causing invisible mechs on any map after the first session. registerTypeLod
		// is idempotent (no-op if already registered in this session).
		for (int _lod = 0; _lod < MAX_LODS; ++_lod) {
			if (mechType->mechShape[_lod])
				GpuMechBatcher::instance().registerTypeLod(mechType, _lod);
		}

		mechShape = mechType->mechShape[0]->CreateFrom();
		
		sensorTriangleShape = GVAppearanceType::SensorTriangleShape->CreateFrom();
		
		if (mechType->leftArm)
			leftArm = mechType->leftArm->CreateFrom();
		
		if (mechType->rightArm)
			rightArm = mechType->rightArm->CreateFrom();
		
		for (long i=0;i<sensorTriangleShape->GetNumTextures();i++)
		{
			char txmName[1024];
			sensorTriangleShape->GetTextureName(i,txmName,256);

			char texturePath[1024];
			sprintf(texturePath,"%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);
	
			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");
	
			if (textureOrKtxSidecarExists(textureName))
			{
				if (S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
					sensorTriangleShape->SetTextureHandle(i,gosTextureHandle);
					sensorTriangleShape->SetTextureAlpha(i,true);
				}
				else
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
					sensorTriangleShape->SetTextureHandle(i,gosTextureHandle);
					sensorTriangleShape->SetTextureAlpha(i,false);
				}
			}
			else
			{
				sensorTriangleShape->SetTextureHandle(i,0xffffffff);
			}
		}
 
		if (mechType->numJumpNodes)
		{
			sensorSquareShape = Mech3DAppearanceType::SensorSquareShape->CreateFrom();
			for (int i=0;i<sensorSquareShape->GetNumTextures();i++)
			{
				char txmName[1024];
				sensorSquareShape->GetTextureName(i,txmName,256);
	
				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (textureOrKtxSidecarExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						sensorSquareShape->SetTextureHandle(i,gosTextureHandle);
						sensorSquareShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						sensorSquareShape->SetTextureHandle(i,gosTextureHandle);
						sensorSquareShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					sensorSquareShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}
		else
		{
			sensorSquareShape = GVAppearanceType::SensorCircleShape->CreateFrom();
			for (int i=0;i<sensorSquareShape->GetNumTextures();i++)
			{
				char txmName[1024];
				sensorSquareShape->GetTextureName(i,txmName,256);

				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);

				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");

				if (textureOrKtxSidecarExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						sensorSquareShape->SetTextureHandle(i,gosTextureHandle);
						sensorSquareShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						sensorSquareShape->SetTextureHandle(i,gosTextureHandle);
						sensorSquareShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					sensorSquareShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}
		
		if (mechType->numWeaponNodes > 0)
		{
			nodeUsed.assign(static_cast<size_t>(mechType->numWeaponNodes), 0);
			nodeRecycle.assign(static_cast<size_t>(mechType->numWeaponNodes), 0.0f);
		}
		
		for (int i=0;i<mechType->numWeaponNodes;i++)
		{
			if (mechShape->isChildOf(mechType->nodeData->nodeId,"joint_ruarm"))
				mechType->nodeData[i+mechType->numSmokeNodes].isRArmNode = true;
			else
				mechType->nodeData[i+mechType->numSmokeNodes].isRArmNode = false;
			
			if (mechShape->isChildOf(mechType->nodeData->nodeId,"joint_luarm"))
				mechType->nodeData[i+mechType->numSmokeNodes].isLArmNode = true;
			else
				mechType->nodeData[i+mechType->numSmokeNodes].isLArmNode = false;
		}

		glm::vec3 boxCoords[8];
		glm::vec3 nodeCenter = mechShape->GetRootNodeCenter();

		boxCoords[0].x = position.x + mechShape->GetMinBox().x + nodeCenter.x;
		boxCoords[0].y = position.y + mechShape->GetMinBox().z + nodeCenter.z;
		boxCoords[0].z = position.z + mechShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[1].x = position.x + mechShape->GetMinBox().x + nodeCenter.x;
		boxCoords[1].y = position.y + mechShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[1].z = position.z + mechShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[2].x = position.x + mechShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[2].y = position.y + mechShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[2].z = position.z + mechShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[3].x = position.x + mechShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[3].y = position.y + mechShape->GetMinBox().z + nodeCenter.z;
		boxCoords[3].z = position.z + mechShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[4].x = position.x + mechShape->GetMinBox().x + nodeCenter.x;
		boxCoords[4].y = position.y + mechShape->GetMinBox().z + nodeCenter.z;
		boxCoords[4].z = position.z + mechShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[5].x = position.x + mechShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[5].y = position.y + mechShape->GetMinBox().z + nodeCenter.z;
		boxCoords[5].z = position.z + mechShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[6].x = position.x + mechShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[6].y = position.y + mechShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[6].z = position.z + mechShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[7].x = position.x + mechShape->GetMinBox().x + nodeCenter.x;
		boxCoords[7].y = position.y + mechShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[7].z = position.z + mechShape->GetMinBox().y + nodeCenter.y;
		
		float testRadius = 0.0;
		
		for (int i=0;i<8;i++)
		{
			testRadius = glm::length(boxCoords[i]);
			if (OBBRadius < testRadius)
				OBBRadius = testRadius;
		}
		
		mechType->boundsUpperLeftX = (-OBBRadius);
		mechType->boundsUpperLeftY = (-OBBRadius * 2.0);
		  		     
		mechType->boundsLowerRightX = (OBBRadius);
		mechType->boundsLowerRightY = (OBBRadius);
		
		if (!mechType->getDesignerTypeBounds())
		{
			mechType->typeUpperLeft.x = mechShape->GetMaxBox().x + nodeCenter.x;
			mechType->typeUpperLeft.y = mechShape->GetMaxBox().z + nodeCenter.z;
			mechType->typeUpperLeft.z = mechShape->GetMaxBox().y + nodeCenter.y;

			mechType->typeLowerRight.x = mechShape->GetMinBox().x + nodeCenter.x;
			mechType->typeLowerRight.y = mechShape->GetMinBox().z + nodeCenter.z;
			mechType->typeLowerRight.z = mechShape->GetMinBox().y + nodeCenter.y;

			//Now expand box by some percentage to make selection easier.
			mechType->typeUpperLeft.x *= EXPAND_FACTOR;
			mechType->typeUpperLeft.y *= EXPAND_FACTOR;
			mechType->typeUpperLeft.z *= EXPAND_FACTOR;

			mechType->typeLowerRight.x *= EXPAND_FACTOR;
			mechType->typeLowerRight.y *= EXPAND_FACTOR;
			mechType->typeLowerRight.z *= EXPAND_FACTOR;
		}
	}
	else
	{
		// DEGRADE-DON'T-CRASH: partially-imported mech — appearance type exists
		// but has no usable base-LOD shape (missing .ini / TGLData / .ase). Skip
		// shape build; mechShape stays NULL and the render path renders nothing.
		// One-time warn so the missing asset is visible in logs (not silent).
		static bool s_warnedMissingMechShape = false;
		if (!s_warnedMissingMechShape)
		{
			s_warnedMissingMechShape = true;
			std::printf("[MECH_LOAD] WARN: mech appearance has no base shape "
				"(missing/partial import) -- rendering nothing. mechType=%p\n",
				static_cast<void*>(mechType));
			std::fflush(stdout);
		}

		// MC2_LOAD_RACE_TRACE: count + identify EVERY degraded instance (not
		// one-shot) so the residency-race wave can tell which mech lost the race
		// and how many instances were affected on this run.
		extern bool g_loadRaceTrace;
		extern long g_loadRaceMechDegrade;
		extern long g_loadRaceMechDegradeReal;
		if (g_loadRaceTrace)
		{
			g_loadRaceMechDegrade++;
			if (obj) g_loadRaceMechDegradeReal++;
			if (g_loadRaceMechDegrade <= 50)
			{
				std::printf("[LOADRACE] mech degrade #%ld: name=%s apprNum=0x%08lx "
					"mechType=%p shape0=%p obj=%p actorHandle=%ld\n",
					g_loadRaceMechDegrade,
					(mechType && mechType->name) ? mechType->name : "<null>",
					mechType ? mechType->appearanceNum : 0UL, static_cast<void*>(mechType),
					mechType ? static_cast<void*>(mechType->mechShape[0]) : nullptr,
					static_cast<void*>(obj), actorHandle_);
				std::fflush(stdout);
			}
		}
	}

	mechFrameRate = 30.0f;
		
 	forceStop = atTransitionToNextGesture = inReverse =	inJump = false;

	inDebugMoveMode = singleStepMode = nextStep = prevStep = false;

	currentStateGoal = -1;		//Always start ready to change gestures
	currentGestureId = 2;		//Always start in Stand Mode
    inCombatMode = false;       // sebi: init, so will not contain garbage

	transitionState = 0;
	oldStateGoal = 1;			//Always start in Stand Mode

	currentFrame = 0.0f;
	lastAnimAdvanceFrame = 0xFFFFFFFFu;	//ANIM-CADENCE-FIX: no advance yet

	pointLight = nullptr;
	lightId = 0xffffffff;
	
	hitGestureId = -1;
	
	jumpSetup = false;
	jumpAirborne = inJump = false;
	
	footPos[0].x = footPos[0].y = footPos[0].z = 1000.0f;
	footPos[1].x = footPos[1].y = footPos[1].z = 1000.0f;
	
	for (long i=0;i<MAX_DUST_POOFS;i++)
	{
		rightDustPoofEffect[i] = nullptr;
		leftDustPoofEffect[i] = nullptr;
	}
	
	smokeEffect = nullptr;
	jumpJetEffect = nullptr;
	rightShoulderBoom = nullptr;
	leftShoulderBoom = nullptr;
	
	leftFootDone0 = rightFootDone0 = leftFootDone1 = rightFootDone1 = false;
	
	leftFootPoofDraw[0] = leftFootPoofDraw[1] = leftFootPoofDraw[2] = 
	rightFootPoofDraw[0] = rightFootPoofDraw[1] = rightFootPoofDraw[2] = false;
	
	if (mechType && mechType->mechShadowShape)
	{
		mechShadowShape = mechType->mechShadowShape->CreateFrom();

		for (long i=0;i<mechShadowShape->GetNumTextures();i++)
		{
			char txmName[1024];
			mechShadowShape->GetTextureName(i,txmName,256);

			char texturePath[1024];
			sprintf(texturePath,"%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);

			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");

			if (fileExists(textureName))
			{
				if (S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					mechShadowShape->SetTextureHandle(i,gosTextureHandle);
					mechShadowShape->SetTextureAlpha(i,true);
				}
				else
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					mechShadowShape->SetTextureHandle(i,gosTextureHandle);
					mechShadowShape->SetTextureAlpha(i,false);
				}
			}
			else
			{
				mechShadowShape->SetTextureHandle(i,0xffffffff);
			}
		}
	}
	else
	{
		mechShadowShape = nullptr;
	}
}

void Mech3DAppearance::initFX (void)
{
	if (!InEditor)
	{
   		if (useNonWeaponEffects && strcmp(weaponEffects->GetEffectName(FX_POOF_ID),"NONE") != 0)
   		{
			unsigned flags = gosFX::Effect::ExecuteFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(FX_POOF_ID));
			
			if (gosEffectSpec)
			{
				for (long i=0;i<MAX_DUST_POOFS;i++)
				{
					rightDustPoofEffect[i] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
					gosASSERT(rightDustPoofEffect[i] != nullptr);
				}
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
		
   		if (useNonWeaponEffects && strcmp(weaponEffects->GetEffectName(FX_POOF_ID),"NONE") != 0)
   		{
			unsigned flags = gosFX::Effect::ExecuteFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(FX_POOF_ID));
			
			if (gosEffectSpec)
			{
				for (long i=0;i<MAX_DUST_POOFS;i++)
				{
					leftDustPoofEffect[i] = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
					gosASSERT(leftDustPoofEffect[i] != nullptr);
				}
				
  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
		
   		if (useNonWeaponEffects && strcmp(weaponEffects->GetEffectName(FX_SMOKE_ID),"NONE") != 0)
   		{
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(FX_SMOKE_ID));
			
			if (gosEffectSpec)
			{
				smokeEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(smokeEffect != nullptr);
			
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}

   		if (strcmp(weaponEffects->GetEffectName(FX_JUMP_ID),"NONE") != 0)
   		{
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(FX_JUMP_ID));
			
			if (gosEffectSpec)
			{
				jumpJetEffect = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(jumpJetEffect != nullptr);
			
				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
}

//Puts mech into hit mode IF and ONLY IF the mech is standing, walking, reversing, running or limping.
// NO OTHER GESTURE IS VALID!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
void Mech3DAppearance::hitFront (void)
{
	if ((currentGestureId == GestureStand) ||
		(currentGestureId == GestureWalk) || 
		(currentGestureId == GestureReverse) || 
		(currentGestureId == GestureRun) || 
		(currentGestureId == GestureLimpLeft) || 
		(currentGestureId == GestureLimpRight))
	{
		hitGestureId = currentGestureId;
		currentGestureId = GestureHitFront;
		
		long firstFrame = mechType->gestures[currentGestureId].frameStart;

		atTransitionToNextGesture = false;
		
		inReverse = false;
							  
		currentFrame = firstFrame;
	}
}

void Mech3DAppearance::hitBack (void)
{
	if ((currentGestureId == GestureStand) ||
		(currentGestureId == GestureWalk) || 
		(currentGestureId == GestureReverse) || 
		(currentGestureId == GestureRun) || 
		(currentGestureId == GestureLimpLeft) || 
		(currentGestureId == GestureLimpRight))
	{
		hitGestureId = currentGestureId;
		currentGestureId = GestureHitBack;
		
		long firstFrame = mechType->gestures[currentGestureId].frameStart;

		atTransitionToNextGesture = false;

		inReverse = false;
			
		currentFrame = firstFrame;
	}
}

void Mech3DAppearance::hitLeft (void)
{
	if ((currentGestureId == GestureStand) ||
		(currentGestureId == GestureWalk) || 
		(currentGestureId == GestureReverse) || 
		(currentGestureId == GestureRun) || 
		(currentGestureId == GestureLimpLeft) || 
		(currentGestureId == GestureLimpRight))
	{
		hitGestureId = currentGestureId;
		currentGestureId = GestureHitLeft;
		
		long firstFrame = mechType->gestures[currentGestureId].frameStart;

		atTransitionToNextGesture = false;
			
		inReverse = false;
			
		currentFrame = firstFrame;
	}
}

void Mech3DAppearance::hitRight (void)
{
	if ((currentGestureId == GestureStand) ||
		(currentGestureId == GestureWalk) || 
		(currentGestureId == GestureReverse) || 
		(currentGestureId == GestureRun) || 
		(currentGestureId == GestureLimpLeft) || 
		(currentGestureId == GestureLimpRight))
	{
		hitGestureId = currentGestureId;
		currentGestureId = GestureHitRight;
		
		long firstFrame = mechType->gestures[currentGestureId].frameStart;

		atTransitionToNextGesture = false;
		
		inReverse = false;
		
		currentFrame = firstFrame;
	}
}

bool Mech3DAppearance::PerPolySelect (long mouseX, long mouseY)
{
	// DEGRADE-DON'T-CRASH: partial import with no base shape -> not selectable.
	if (!mechShape)
		return false;
	return mechShape->PerPolySelect(mouseX,mouseY);
}

void Mech3DAppearance::setPaintScheme (void)
{
	// DEGRADE-DON'T-CRASH: partial import with no base shape -> nothing to paint.
	if (!mechShape)
		return;

	DWORD gosHandle = mcTextureManager->get_gosTextureHandle(mechShape->GetTextureHandle(0));

	if (gosHandle && gosHandle != 0xffffffff)
	{
		TEXTUREPTR textureData;
		gos_LockTexture(gosHandle, 0, 0, &textureData);

		// ENCYCLO-3D-2 diagnostic: is the locked readback real pixels or zeros?
		// A zeroed lock buffer here means the paint pass writes black back up.
		if ( mc2::gate::get<"MC2_LOG_PREVIEW">() )
		{
			if ( bool f = mc2::log::enabled(mc2::log::Cat::Render, mc2::log::Level::Info) )
			{
				// The surface can be empty or the 1x1 missing-texture placeholder: read only pixels that exist.
				const DWORD* p = textureData.pTexture;
				const long long pixelCount = static_cast<long long>(textureData.Width) * static_cast<long long>(textureData.Height);
				auto px = [&](long long idx) -> unsigned long { return (p && idx < pixelCount) ? static_cast<unsigned long>(p[idx]) : 0ul; };
				MC2_LOG(Render, Info, "[PREVIEW-PAINT] lock gosHandle={:d} {:d}x{:d} pitch={:d} px0={:08X} px1={:08X} pxMid={:08X}", mc2::fmt::as_u32(static_cast<unsigned long>(gosHandle)), mc2::fmt::as_i32(static_cast<long>(textureData.Width)), mc2::fmt::as_i32(static_cast<long>(textureData.Height)), mc2::fmt::as_i32(static_cast<long>(textureData.Pitch)), mc2::fmt::as_u32(px(0)), mc2::fmt::as_u32(px(1)), mc2::fmt::as_u32(px(static_cast<long long>(textureData.Height/2)*textureData.Width + textureData.Width/2)));

			}
		}

		// Dominant-channel paint classifier. A pixel belongs to a paint slot
		// iff its R/G/B dominates the other two by at least kRatio. Boundary
		// (antialiased) pixels blend tint vs original proportional to how
		// cleanly they dominate -- the original code required strict purity,
		// which missed every antialiased mask edge and every upscaled
		// gradient pixel.
		DWORD *textureMemory = textureData.pTexture;
		for (long i=0;i<textureData.Height;i++)
		{
			for (long j=0;j<textureData.Width;j++)
			{
				DWORD srcColor = *textureMemory;
				DWORD srcA = (srcColor & 0xff000000);
				int   srcR = static_cast<int>(((srcColor >> 16) & 0xff));
				int   srcG = static_cast<int>(((srcColor >>  8) & 0xff));
				int   srcB = static_cast<int>(( srcColor        & 0xff));

				// `shade`     = tint brightness from dominant-channel magnitude
				//                (preserves the original log-shade curve).
				// `mix`       = confidence this pixel is a mask pixel
				//                (0=not a mask, 1=comfortably past threshold).
				// Separating these two means a slightly-impure interior pixel
				// (e.g. (240,20,10) from an upscaled texture) gets mix~=1
				// and still receives full tint, while a boundary pixel whose
				// dominance barely clears threshold gets a soft blend.
				int   slot       = -1;
				DWORD slotColor  = 0;
				float shade      = 0.0f;
				float mix        = 0.0f;
				const int   kMinDom      = 16;
				const float kRatio       = 3.0f;
				const float kRatioMargin = 1.5f; // mix saturates at ratio = kRatio * kRatioMargin

				int domChan = 0, maxOther = 0;
				if (srcR >= kMinDom && static_cast<float>(srcR) >= kRatio * static_cast<float>(srcG) && static_cast<float>(srcR) >= kRatio * static_cast<float>(srcB))
				{
					slot = 0; slotColor = psRed;
					domChan = srcR;
					maxOther = srcG > srcB ? srcG : srcB;
				}
				else if (srcG >= kMinDom && static_cast<float>(srcG) >= kRatio * static_cast<float>(srcR) && static_cast<float>(srcG) >= kRatio * static_cast<float>(srcB))
				{
					slot = 1; slotColor = psGreen;
					domChan = srcG;
					maxOther = srcR > srcB ? srcR : srcB;
				}
				else if (srcB >= kMinDom && static_cast<float>(srcB) >= kRatio * static_cast<float>(srcR) && static_cast<float>(srcB) >= kRatio * static_cast<float>(srcG))
				{
					slot = 2; slotColor = psBlue;
					domChan = srcB;
					maxOther = srcR > srcG ? srcR : srcG;
				}

				if (slot != -1)
				{
					float d = static_cast<float>(domChan) / 255.0f;
					shade = 1.0f - (1.0f - d) * (1.0f - d);

					// Confidence saturates once the dominance ratio is
					// comfortably past threshold, so upscaled-texture
					// interior pixels (ratio ~10-15) get full tint while
					// near-threshold boundary pixels get a soft blend.
					float ratio = static_cast<float>(domChan) / static_cast<float>((maxOther + 1));
					float margin = (ratio - kRatio) / (kRatio * (kRatioMargin - 1.0f));
					margin = std::clamp(margin, 0.0f, 1.0f);
					mix = margin;
				}

				DWORD newColor = srcColor;
				if (slot != -1 && mix > 0.0f)
				{
					float tintR = static_cast<float>(((slotColor >> 16) & 0xff)) * shade;
					float tintG = static_cast<float>(((slotColor >>  8) & 0xff)) * shade;
					float tintB = static_cast<float>(( slotColor        & 0xff)) * shade;

					float outR = tintR * mix + static_cast<float>(srcR) * (1.0f - mix);
					float outG = tintG * mix + static_cast<float>(srcG) * (1.0f - mix);
					float outB = tintB * mix + static_cast<float>(srcB) * (1.0f - mix);

					unsigned char r = static_cast<unsigned char>((outR < 0.0f ? 0.0f : outR > 255.0f ? 255.0f : outR));
					unsigned char g = static_cast<unsigned char>((outG < 0.0f ? 0.0f : outG > 255.0f ? 255.0f : outG));
					unsigned char b = static_cast<unsigned char>((outB < 0.0f ? 0.0f : outB > 255.0f ? 255.0f : outB));

					newColor = srcA | (static_cast<DWORD>(r) << 16) | (static_cast<DWORD>(g) << 8) | static_cast<DWORD>(b);
				}

				*textureMemory = newColor;
				textureMemory++;
			}
		}

		gos_UnLockTexture(gosHandle);
	}
}	

DWORD bgrTorgb (DWORD frontRGB)
{
	DWORD tmp;
	tmp = (((0x00ff0000) & frontRGB)>>16) + ((0x0000ff00) & frontRGB) + (((0x000000ff) & frontRGB)<<16);
	
	return(tmp);
}

void Mech3DAppearance::setPaintScheme (DWORD mcRed, DWORD mcGreen, DWORD mcBlue)
{
	psRed = mcRed;
	psBlue = mcBlue;
	psGreen = mcGreen;

	setPaintScheme();	
}	

void Mech3DAppearance::getPaintScheme( DWORD& red, DWORD& green, DWORD& blue )
{
	red = psRed;
	blue = psBlue;
	green = psGreen;
}

void Mech3DAppearance::resetPaintScheme (DWORD red, DWORD green, DWORD blue)
{
	// DEGRADE-DON'T-CRASH: partial import with no base shape -> no texture to
	// repaint. Called directly from the encyclopedia / mech-bay preview setup
	// (SimpleCamera::setMech) before any frame; mechShape->GetTextureName below
	// would deref NULL.
	if (!mechShape)
		return;

	// META-FIX design B PART 1: the fail-loud placeholder registers a REAL
	// in-memory magenta texture at the TYPE level ("mc2_load_placeholder" is
	// never on disk). This name-based rebind would clobber the instance's
	// handle back to 0xffffffff (its file-missing else branch) and leave the
	// placeholder a black box — instead bind the type-level magenta handle
	// to THIS instance (the mech render path samples the instance's slot-0
	// handle) and skip the repaint. Cold on stock: no stock mech ever sets
	// baseShapeLoadFailed. (First live probe rendered visible-but-BLACK
	// boxes — the early-return alone left the instance handle unset.)
	if (mechType && mechType->baseShapeLoadFailed)
	{
		DWORD ph = mechType->mechShape[currentLOD]
			? mechType->mechShape[currentLOD]->GetTextureHandle(0)
			: 0xffffffff;
		if (ph != 0xffffffff)
		{
			// The GPU mech batcher binds SLOT 0 per-actor from
			// localTextureHandle (submitActor desc.slot0TexHandle, an
			// mcTextureManager node index — same space as GetTextureHandle).
			localTextureHandle = ph;
			if (mechShape->GetNumTextures() > 0)
			{
				mechShape->SetTextureHandle(0, ph);
				mechShape->SetTextureAlpha(0, false);
			}
		}
		return;
	}

	// AO-1: load the imported-mech AO map once (independent of paint scheme; AO is
	// linear grayscale, not recolored). Loaded here (before resetPaintScheme's early
	// returns) so it runs on every path. HAS_AO is asserted at submit ONLY if aoValid,
	// so a missing/failed AO never darkens the mech (white/no-op fallback).
	if (!aoValid && mechShape->GetNumShapes() > 0) {
		const void* aoTypeKey = mechType ? static_cast<const void*>(mechType->mechShape[currentLOD]) : nullptr;
		const char* aoName = mc2mechanim::ImportedMechAoTexName(aoTypeKey);
		if (aoName && aoName[0]) {
			char aoPath[1024];
			sprintf(aoPath, "%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);
			FullPathFileName aoFull;
			aoFull.init(aoPath, const_cast<char*>(aoName), "");
			if (textureOrKtxSidecarExists(aoFull)) {
				aoTextureHandle = mcTextureManager->loadTexture(aoFull, gos_Texture_Solid,
					gosHint_DontShrink);
				aoValid = (aoTextureHandle != 0 && aoTextureHandle != 0xFFFFFFFFu);
			}
			if (!aoValid)
				fprintf(stderr, "[MECH_IMPORT] AO '%s' not loaded -> no-op (white)\n", aoName);
		}
	}

	// NORMALS-1: load the imported-mech normal map once (same contract as AO: linear
	// BC7, base-chassis, skin-independent). HAS_NORMAL is asserted at submit ONLY if
	// normalValid, so a missing/failed normal never corrupts shading (v_normal fallback).
	if (!normalValid && mechShape->GetNumShapes() > 0) {
		const void* nrmTypeKey = mechType ? static_cast<const void*>(mechType->mechShape[currentLOD]) : nullptr;
		const char* nrmName = mc2mechanim::ImportedMechNormalTexName(nrmTypeKey);
		if (nrmName && nrmName[0]) {
			char nrmPath[1024];
			sprintf(nrmPath, "%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);
			FullPathFileName nrmFull;
			nrmFull.init(nrmPath, const_cast<char*>(nrmName), "");
			if (textureOrKtxSidecarExists(nrmFull)) {
				normalTextureHandle = mcTextureManager->loadTexture(nrmFull, gos_Texture_Solid,
					gosHint_DontShrink);
				normalValid = (normalTextureHandle != 0 && normalTextureHandle != 0xFFFFFFFFu);
			}
			if (!normalValid)
				fprintf(stderr, "[MECH_IMPORT] normal '%s' not loaded -> no-op (v_normal)\n", nrmName);
		}
	}


	mcTextureManager->releaseNode(localTextureHandle);
	
	char txmName[1024];
	mechShape->GetTextureName(0,txmName,256);

   	char texturePath[1024];
   	sprintf(texturePath,"%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);

   	FullPathFileName textureName;
   	textureName.init(texturePath,txmName,"");

	/* The texture manager asks for a unique 32bit identifier for every texture instance.
	However, it requires 72 bits to fully describe a mech texture (the base color (stored in
	the variable "red"), highlight color1 (blue), and highlight color2 (green), each of which
	is a 24bit number made up of 8bit r, g, and b components). Instead of creating a
	mapping between the mech textures used (probably less than 2^32 of them) and
	32bit identifiers, for the sake of expediency I'm just taking the 3 most significant bits of
	the 9 rgb components to make a 27 bit identifier. This means that two mech textures
	that are close in color (i.e. all of the 3 most significant bits of the 9 rgb components are
	the same) will be treated as the same texture, which is not necessarily a bad thing in
	our case.  LOD is never needed because if the texture is different, its NAME will be different!!*/
	DWORD ccbase = ((red >> 5) & 7) + (((red >> 13) & 7) << 3) + (((red >> 21) & 7) << 6);
	DWORD cchighlight1 = ((green >> 5) & 7) + (((green >> 13) & 7) << 3) + (((green >> 21) & 7) << 6);
	DWORD cchighlight2 = ((blue >> 5) & 7) + (((blue >> 13) & 7) << 3) + (((blue >> 21) & 7) << 6);
	DWORD paintInstance = (ccbase << 18) + (cchighlight1 << 9) + (cchighlight2);

	// KTX2-INFRA-1: accept a BC7 .ktx2 sidecar when the .tga is absent (homogeneous
	// imported-mech textures ship as .ktx2 only; txmmgr KTX_PRIMARY decodes it). Was
	// fileExists(textureName) — that skipped -> black mech for .ktx2-only deploys.
	if (textureOrKtxSidecarExists(textureName))
	{
		// gosHint_MipmapFilter0: opt this texture into GL mipmap+trilinear
		// via gosTexture::createHardwareTexture. Full mip chain is
		// regenerated after the paint-scheme edit in setPaintScheme() so
		// minified mechs at zoom-out no longer alias through the unpainted
		// mask-boundary pixels that fix A already resolved at mip 0.
		const DWORD mechHints = gosHint_MipmapFilter0 | gosHint_DontShrink;
		DWORD textureInstanceAlreadyExists = mcTextureManager->textureInstanceExists(textureName,gos_Texture_Solid,mechHints,paintInstance);
		if (!textureInstanceAlreadyExists)
			localTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,mechHints,paintInstance);
		else
			localTextureHandle = textureInstanceAlreadyExists;
			
		mechShape->SetTextureHandle(0,localTextureHandle);
		mechShape->SetTextureAlpha(0,false);
		
		if (leftArm)
		{
			leftArm->SetTextureHandle(0,localTextureHandle);
			leftArm->SetTextureAlpha(0,false);
		}
		
		if (rightArm)
		{
			rightArm->SetTextureHandle(0,localTextureHandle);
			rightArm->SetTextureAlpha(0,false);
		}
		
  		if (textureInstanceAlreadyExists)
		{
			/* In this case, the texture returned should already have the paint scheme
			applied. */

			//Still need to store psRed/psGreen/psBlue!!!!
			psRed = red;
			psGreen = green;
			psBlue = blue;
			return;
		}
	}
	else
	{
		mechShape->SetTextureHandle(0,0xffffffff);
		if (leftArm)
			leftArm->SetTextureHandle(0,0xffffffff);
			
		if (rightArm)
			rightArm->SetTextureHandle(0,0xffffffff);
	}
	
	setPaintScheme(red,green,blue);
}
