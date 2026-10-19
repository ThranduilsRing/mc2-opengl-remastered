// Mech 3D layer.  Controls how the mech moves through animations
// For MechCommander 2
// Replace Mactor for better looking mechs!
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include <algorithm>

#ifndef MECH3D_H
#include"mech3d.h"
#endif

#include "mech3d_internal.h"

#ifndef MSTATES_H
#include"mstates.h"
#include "../RenderCore/axis_convert.h"
#endif

// MC2_GPU_CULL_LIFECYCLE_TRACE=1: verbose per-actor lifecycle boundary prints.
// Default off (too noisy for production). Fires at: init, first GPU-cull skip per actor.
const bool s_lcTrace = mc2::gate::get<"MC2_GPU_CULL_LIFECYCLE_TRACE">();
bool useNonWeaponEffects = true;
bool useHighObjectDetail = true;
bool InEditor = false;

constexpr int BODYSTATE_NORMAL = 0;
constexpr int BODYSTATE_STANDING = 1;
constexpr int BODYSTATE_PARKED = 2;
constexpr int BODYSTATE_FALLEN_BACKWARD = 3;
constexpr int BODYSTATE_FALLEN_FORWARD = 4;

char MechStateByGesture[MAX_MECH_ANIMATIONS] = {
	BODYSTATE_PARKED,			// GesturePark					 0 -- Park 
	BODYSTATE_STANDING,			// GestureParkToStand			 1 -- ParkToStand 
	BODYSTATE_STANDING,		// GestureStand					 2 -- Stand 
	BODYSTATE_NORMAL,			// GestureStandToWalk			 3 -- StandToWalk 
	BODYSTATE_NORMAL,			// GestureWalk					 4 -- Walk 
	BODYSTATE_PARKED,		// GestureStandToPark			 5 -- StandToPark
	BODYSTATE_NORMAL,			// GestureWalkToRun				 6 -- WalkToRun
	BODYSTATE_NORMAL,			// GestureRun					 7 -- Run
	BODYSTATE_NORMAL,			// GestureRunToWalk				 8 -- RunToWalk 
	BODYSTATE_NORMAL,			// GestureReverse				 9 -- Reverse
	BODYSTATE_NORMAL,			// GestureStandToReverse		10 -- StandToReverse
	BODYSTATE_NORMAL,	// GestureLimpLeft				11 -- LimpLeft
	BODYSTATE_NORMAL,	// GestureLimpRight				12 -- LimpRight
	BODYSTATE_STANDING,		// GestureIdle					13 -- Idle
	BODYSTATE_FALLEN_BACKWARD,	// GestureFallBackward			14 -- FallBackward 
	BODYSTATE_FALLEN_FORWARD,	// GestureFallForward			15 -- FallForward 
	BODYSTATE_NORMAL,			// GestureHitFront				16 -- HitFront
	BODYSTATE_NORMAL,			// GestureHitBack				17 -- HitBack
	BODYSTATE_NORMAL,			// GestureHitLeft				18 -- HitLeft
	BODYSTATE_NORMAL,			// GestureHitRight				19 -- HitRight
	BODYSTATE_NORMAL,			// GestureJump					20 -- Jump
	BODYSTATE_FALLEN_FORWARD,	// GestureRollover				21 -- Rollover from FallBack to FallForward 
	BODYSTATE_FALLEN_FORWARD,	// GestureGetup					22 -- Get up from FallForward 
	BODYSTATE_FALLEN_FORWARD,	// GestureFallenForward			23 -- Fallen Forward (Single Frame, mech on ground)
	BODYSTATE_FALLEN_FORWARD,	// GestureFallenBackward		24 -- Fallen Backward (Single Frame, mech on ground)
};																

TG_TypeMultiShapePtr Mech3DAppearanceType::SensorSquareShape = nullptr;

bool Mech3DAppearanceType::animationLoadingEnabled = true;

#define MAX_MECHS					1
glm::vec3 debugMechActorPosition[MAX_MECHS];
float mechDebugAngle[MAX_MECHS];
float torsoDebugAngle[MAX_MECHS];

void Mech3DAppearance::setObjectParameters (const glm::vec3 &pos, float Rot, long sel, long team, long homeRelations)
{
	movedThisFrame = false;
	if ((rotation != Rot) || (!mc2::math::closeEnough(pos, position)))
		movedThisFrame = true;
 
	if ((currentGestureId == 23) || (currentGestureId == 24) || (currentGestureId == 21) || (currentGestureId == 22) ||
		(currentGestureId == 14) || (currentGestureId == 15))
	{
	}
	else
	{
		rotation = Rot;
		if (rotation > 180)
			rotation -= 360;

		if (rotation < -180)
			rotation += 360;
	}

	position = pos;

	selected = sel;

	teamId = team;
	homeTeamRelationship = homeRelations;
}

void Mech3DAppearance::setMoverParameters (float turretRot, float lArmRot, float rArmRot, bool isAirborne)
{
	isHelicopter = isAirborne;

	if ((currentGestureId == 23) || (currentGestureId == 24) || (currentGestureId == 21) || (currentGestureId == 22) ||
		(currentGestureId == 14) || (currentGestureId == 15))
	{
	}
	else
	{
		torsoRotation = turretRot;
		leftArmRotation = lArmRot;
		rightArmRotation = rArmRot;

		if (torsoRotation > 180)
			torsoRotation -= 360;

		if (torsoRotation < -180)
			torsoRotation += 360;
	}
}	

void Mech3DAppearance::updateFootprints (void)
{

}

bool Mech3DAppearance::isMouseOver (float px, float py)
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

bool oneMechPlease = false;

void Mech3DAppearance::destroy (void)
{
	// (E) T1.8: paired cleanup for SpotLight_-child illumination from T1.6.
	// Walks CACHED state only (spotlightLights_/spotlightSlotIds_); does NOT
	// call getNodeIdPosition or any mechShape method — same destroy-ordering
	// discipline as bdactor.cpp T1.5 (C-r2 M5). NOTE: the pre-existing
	// `pointLight` (anubis searchlight) is intentionally NOT cleaned up here
	// — that's a documented R1 leak in the existing code, out-of-scope for
	// (E) per plan "Vedette/LRMC anubis-leak audit" follow-up.
	for (size_t k = 0; k < spotlightLights_.size(); ++k)
	{
		if (eye)
			eye->removeWorldLight(spotlightSlotIds_[k], spotlightLights_[k]);
		// T1.16 — pair untag with removeWorldLight.
		mc2_spotlight_diag::untag_slot(static_cast<long>(spotlightSlotIds_[k]));
		free(spotlightLights_[k]);
	}
	spotlightLights_.clear();
	spotlightSlotIds_.clear();
	spotlightNodeIds_.clear();
	spotlightsRegistered_ = false;

	if ( mechShape )
	{
		mc2mechanim::UnregisterImportedActor(mechShape);	// IMPORTED-ACTOR-STABLE-KEY-1: final teardown
		delete mechShape;
	}
	mechShape = nullptr;

	if (mechShadowShape)
	{
		delete mechShadowShape;
		mechShadowShape = nullptr;
	}

	if ( leftArm )
		delete leftArm;
	leftArm = nullptr;

	if ( rightArm )
		delete rightArm;
	rightArm = nullptr;


	for (long i=0;i<MAX_DUST_POOFS;i++)
	{
		if (rightDustPoofEffect[i])
		{
			rightDustPoofEffect[i]->Kill();
			rightDustPoofEffect[i].reset();
		}
			
		if (leftDustPoofEffect[i])
		{
			leftDustPoofEffect[i]->Kill();
			leftDustPoofEffect[i].reset();
		}
	}

	if (smokeEffect)
	{
		smokeEffect->Kill();
		smokeEffect.reset();
	}
	
	if (jumpJetEffect)
	{
		jumpJetEffect->Kill();
		jumpJetEffect.reset();
	}
	
	if (rightShoulderBoom)
	{
		rightShoulderBoom->Kill();
		rightShoulderBoom.reset();
	}
	
	if (leftShoulderBoom)
	{
		leftShoulderBoom->Kill();
		leftShoulderBoom.reset();
	}

	if (sensorSquareShape)
	{
		delete sensorSquareShape;
		sensorSquareShape = nullptr;	
	}
		
	if (sensorTriangleShape)
	{
		delete sensorTriangleShape;
		sensorTriangleShape = nullptr;	
	}
	
	appearanceTypeList->removeAppearance(mechType);

	if (InEditor)
	{
		std::vector<long>().swap(nodeUsed);
		std::vector<float>().swap(nodeRecycle);
	}
}

void Mech3DAppearance::copyTo (MechAppearanceData *data)
{
	data->frameNum = frameNum;
	data->mechFrameRate = mechFrameRate;

	data->leftArmOff = leftArmOff;
	data->rightArmOff = rightArmOff;
	data->fallen = fallen;
	data->forceStop = forceStop;
	data->atTransitionToNextGesture = atTransitionToNextGesture;
	data->inReverse = inReverse;
	data->inJump = inJump;
	data->jumpSetup = jumpSetup;
	data->jumpFXSetup = jumpFXSetup;
	data->jumpAirborne = jumpAirborne;
	data->oncePerFrame = oncePerFrame;
	data->lockRotation = lockRotation;

	data->velocity = velocity;
	data->status = status;

	data->currentStateGoal = currentStateGoal;
	data->currentGestureId = currentGestureId;
	data->transitionState = transitionState;
	data->oldStateGoal = oldStateGoal;
	data->hitGestureId = hitGestureId;

	data->currentFrame = currentFrame;
	data->currentLOD = currentLOD;

	if (mechType->numWeaponNodes > 10)
		STOP(("Mech Has too many weapon nodes to save %d",mechType->numWeaponNodes));

	// The saved slot is 10 BYTES of a 10-element table (a save-format limit, docs/known_issues.md); a table shorter than the slot must not be over-read.
	memcpy(data->nodeUsed,nodeUsed.data(),std::min<size_t>(10,nodeUsed.size()*sizeof(long)));		 
	memcpy(data->nodeRecycle,nodeRecycle.data(),std::min<size_t>(10,nodeRecycle.size()*sizeof(float)));

	data->isSmoking = isSmoking;
	data->isWaking = isWaking;
	data->isDusting = isDusting;
	data->fallDust = fallDust;
	data->isHelicopter = isHelicopter;

	data->baseRootNodeHeight = baseRootNodeHeight;
	data->jumpDestination = jumpDestination;
	data->jumpVelocity = jumpVelocity;
}

void Mech3DAppearance::copyFrom (MechAppearanceData *data)
{
	frameNum = data->frameNum;
	mechFrameRate = data->mechFrameRate;

	leftArmOff = data->leftArmOff;
	rightArmOff = data->rightArmOff;
	fallen = data->fallen;
	forceStop = data->forceStop;
	atTransitionToNextGesture = data->atTransitionToNextGesture;
	inReverse = data->inReverse;
	inJump = data->inJump;
	jumpSetup = data->jumpSetup;
	jumpFXSetup = data->jumpFXSetup;
	jumpAirborne = data->jumpAirborne;
	oncePerFrame = data->oncePerFrame;
	lockRotation = data->lockRotation;

	velocity = data->velocity;

	//Let mech reset status when it reloads so that the destroyed shapes come up.
//	status = data->status;

	currentStateGoal = data->currentStateGoal;
	currentGestureId = data->currentGestureId;
	transitionState = data->transitionState;
	oldStateGoal = data->oldStateGoal;
	hitGestureId = data->hitGestureId;

	currentFrame = data->currentFrame;
	currentLOD = data->currentLOD;

	// Mirror of copyTo: only 10 bytes were saved, and a table shorter than that must not be over-written.
	memcpy(nodeUsed.data(),data->nodeUsed,std::min<size_t>(10,nodeUsed.size()*sizeof(long)));		 
	memcpy(nodeRecycle.data(),data->nodeRecycle,std::min<size_t>(10,nodeRecycle.size()*sizeof(float)));

	isSmoking = -1;
	if (data->isSmoking >= 0)
		startSmoking(data->isSmoking);

	isWaking = false;
	if (data->isWaking)
		startWaterWake();

	isDusting = false;
	if (data->isDusting)
		playEjection();

	fallDust = false;
	if (data->fallDust)
		playEjection();

	if (data->inJump)
	{
		long jumpNodeId = mechType->numSmokeNodes + mechType->numWeaponNodes;
		glm::vec3 jumpNodePos = getNodePosition(jumpNodeId);
		
		glm::vec3			actualPosition;
		glm::mat4 	shapeOrigin;
		glm::mat4	localToWorld;
		glm::mat4	localResult;
		
		mc2ToRender(actualPosition, jumpNodePos);
		
		mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
		mc2::math::buildTranslation(shapeOrigin, actualPosition);
		
		glm::quat effectRot;
		effectRot = mc2::math::quatFromEuler(mc2::math::Euler(JUMP_PITCH * DEGREES_TO_RADS,rotation * DEGREES_TO_RADS,0.0f));
		localToWorld = mc2::math::multiply(gosFX::Effect_Against_Motion, effectRot);
		localResult = localToWorld * shapeOrigin;
					
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&localResult,nullptr);
		jumpJetEffect->Start(&info);
	}

	isHelicopter = data->isHelicopter;
	baseRootNodeHeight = data->baseRootNodeHeight;
	jumpDestination = 	data->jumpDestination;
	jumpVelocity = 		data->jumpVelocity;

	// [MECHRESTORE v1] restore boundary. status is deliberately NOT
	// copied — emit the post-restore value + type/LOD so a
	// single savegame load correlates each mech's restored type against
	// its later submit/register events.
	{
		static const bool s_mechRestoreTrace = mc2::gate::get<"MC2_MECH_RESTORE_TRACE">();
		if (s_mechRestoreTrace)
			std::fprintf(stderr,
				"[MECHRESTORE v1] event=copyfrom type=%p restored_lod=%d "
				"status_restored=0 status_now=%d\n",
				static_cast<void*>(mechType), static_cast<int>(currentLOD), static_cast<int>(status));
	}
}

void Mech3DAppearance::flashBuilding (float dur, float fDuration, DWORD color)
{
	duration = dur;
	flashDuration = fDuration;
	flashColor = color;
	drawFlash = true;
	currentFlash = flashDuration;
}

glm::vec3 Mech3DAppearance::getHitNodeLeft (void)
{
	if (hitLeftNodeIndex == -1)
		hitLeftNodeIndex = mechShape->GetNodeNameId("hit_left");

	glm::vec3 result = position;
	// No visibility test: update() poses every mech each sim step, and bolts aim at this node in the simulation.

   	// Create Matrix to conform to.
   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));

   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);

   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(torsoRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

	mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);

	// 1A: imported mech -> manifest-resolved hit node ("hit_left").
	if (mechShape && mechShape->GetNumShapes() > 0) {
		const void* tKey = mechType ? static_cast<const void*>(mechType->mechShape[currentLOD]) : nullptr;
		float wp[3];
		if (mc2mechanim::GetImportedNodeWorld(static_cast<const void*>(mechShape), tKey, "hit_left",
				static_cast<const float*>(glm::value_ptr(mechShape->GetShapeRec(0)->shapeToWorld)), wp)) {
			result.x = wp[0]; result.y = wp[1]; result.z = wp[2];
			return result;
		}
	}

	result = mechShape->GetTransformedNodePosition(&xlatPosition,&qRotation,hitLeftNodeIndex);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) &&
		(result.z == 0.0f))
		result = position;

	return result;
}

glm::vec3 Mech3DAppearance::getHitNodeRight (void)
{
	if (hitRightNodeIndex == -1)
		hitRightNodeIndex = mechShape->GetNodeNameId("hit_right");

	glm::vec3 result = position;
	// No visibility test: update() poses every mech each sim step, and bolts aim at this node in the simulation.

   	// Create Matrix to conform to.
   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);
   
   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(torsoRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = mechShape->SetNodeRotation("joint_torso",&torsoRot);

	mechShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);

	// 1A: imported mech -> manifest-resolved hit node ("hit_right").
	if (mechShape && mechShape->GetNumShapes() > 0) {
		const void* tKey = mechType ? static_cast<const void*>(mechType->mechShape[currentLOD]) : nullptr;
		float wp[3];
		if (mc2mechanim::GetImportedNodeWorld(static_cast<const void*>(mechShape), tKey, "hit_right",
				static_cast<const float*>(glm::value_ptr(mechShape->GetShapeRec(0)->shapeToWorld)), wp)) {
			result.x = wp[0]; result.y = wp[1]; result.z = wp[2];
			return result;
		}
	}

	result = mechShape->GetTransformedNodePosition(&xlatPosition,&qRotation,hitRightNodeIndex);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}
