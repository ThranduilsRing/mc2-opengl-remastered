#include "Gates/preferences_legacy.h"
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

inline constexpr int MAX_MECHS = 1;
glm::vec3 debugMechActorPosition[MAX_MECHS];
float mechDebugAngle[MAX_MECHS];
float torsoDebugAngle[MAX_MECHS];

// The tick's entry to the sim half of the appearance. Same gesture lock as setObjectParameters / setMoverParameters.
void Mech3DAppearance::setSimInput (const SimInput& in)
{
	const auto g = unitAnim().currentGestureId;
	if (!((g == 23) || (g == 24) || (g == 21) || (g == 22) || (g == 14) || (g == 15)))
	{
		simRotation_ = in.rotation;
		if (simRotation_ > 180)
			simRotation_ -= 360;
		if (simRotation_ < -180)
			simRotation_ += 360;

		unitAnim().torsoRotation = in.torsoRotation;
		if (unitAnim().torsoRotation > 180)
			unitAnim().torsoRotation -= 360;
		if (unitAnim().torsoRotation < -180)
			unitAnim().torsoRotation += 360;
	}
	simPosition_ = in.position;
	if (in.hasStatus)
		simStatus_ = in.status;
	if (in.debugMove)
		simDebugMove_ = true;
}

void Mech3DAppearance::setObjectParameters (const glm::vec3 &pos, float Rot, long sel, long team, long homeRelations)
{
	// Load and init paths seed the sim copy through here (then refreshSimPose); the tick uses setSimInput.
	if (simulationOwned() && !simPresentationPass)
	{
		SimInput seed;
		seed.position = pos;
		seed.rotation = Rot;
		seed.torsoRotation = unitAnim().torsoRotation;
		setSimInput(seed);
	}

	movedThisFrame = false;
	if ((rotation != Rot) || (!mc2::math::closeEnough(pos, position)))
		movedThisFrame = true;
 
	if ((unitAnim().currentGestureId == 23) || (unitAnim().currentGestureId == 24) || (unitAnim().currentGestureId == 21) || (unitAnim().currentGestureId == 22) ||
		(unitAnim().currentGestureId == 14) || (unitAnim().currentGestureId == 15))
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

	if ((unitAnim().currentGestureId == 23) || (unitAnim().currentGestureId == 24) || (unitAnim().currentGestureId == 21) || (unitAnim().currentGestureId == 22) ||
		(unitAnim().currentGestureId == 14) || (unitAnim().currentGestureId == 15))
	{
	}
	else
	{
		unitAnim().torsoRotation = turretRot;
		leftArmRotation = lArmRot;
		rightArmRotation = rArmRot;

		if (unitAnim().torsoRotation > 180)
			unitAnim().torsoRotation -= 360;

		if (unitAnim().torsoRotation < -180)
			unitAnim().torsoRotation += 360;
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
    mc2mechanim::UnregisterImportedActor(simulationShape_.get());
    simulationShape_.reset();

	// (E) T1.8: paired cleanup for SpotLight_-child illumination from T1.6.
	// Walks CACHED state only (spotlightLights_/spotlightSlotIds_); does NOT
	// call getNodeIdPosition or any mechShape method — same destroy-ordering
	// discipline as bdactor.cpp T1.5 (C-r2 M5).
	// The Anubis searchlight owns a camera slot and tag too. Retire it before
	// replacing the appearance, rather than leaving an ownerless mover light.
	if (pointLight)
	{
		if (eye)
			eye->removeWorldLight(lightId, pointLight);
		free(pointLight);
		pointLight = nullptr;
		lightId = 0xffffffff;
	}
	for (size_t k = 0; k < spotlightLights_.size(); ++k)
	{
		if (eye)
			eye->removeWorldLight(spotlightSlotIds_[k], spotlightLights_[k]);
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
	
	releaseAppearanceType(registryGeneration, mechType); // skipped when the registry that issued mechType is gone

	if (InEditor)
	{
		std::vector<long>().swap(unitAnim().nodeUsed);
		std::vector<float>().swap(unitAnim().nodeRecycle);
	}
}

void Mech3DAppearance::copyTo (MechAppearanceData *data)
{
	data->frameNum = frameNum;
	data->mechFrameRate = unitAnim().mechFrameRate;

	data->leftArmOff = unitAnim().leftArmOff;
	data->rightArmOff = unitAnim().rightArmOff;
	data->fallen = unitAnim().fallen;
	data->forceStop = unitAnim().forceStop;
	data->atTransitionToNextGesture = unitAnim().atTransitionToNextGesture;
	data->inReverse = unitAnim().inReverse;
	data->inJump = unitAnim().inJump;
	data->jumpSetup = unitAnim().jumpSetup;
	data->jumpFXSetup = jumpFXSetup;
	data->jumpAirborne = unitAnim().jumpAirborne;
	data->oncePerFrame = unitAnim().oncePerFrame;
	data->lockRotation = unitAnim().lockRotation;

	data->velocity = unitAnim().velocity;
	data->status = simulationOwned() ? simStatus_ : status;

	data->currentStateGoal = unitAnim().currentStateGoal;
	data->currentGestureId = unitAnim().currentGestureId;
	data->transitionState = unitAnim().transitionState;
	data->oldStateGoal = unitAnim().oldStateGoal;
	data->hitGestureId = unitAnim().hitGestureId;

	data->currentFrame = unitAnim().currentFrame;
	data->currentLOD = currentLOD;

	// Keep the legacy 10-byte prefix unchanged. ANIM saves the complete counted
	// tables; Mission::save requires WNOD for tables beyond the old 10-node cap.
	memcpy(data->nodeUsed,unitAnim().nodeUsed.data(),std::min<size_t>(10,unitAnim().nodeUsed.size()*sizeof(long)));		 
	memcpy(data->nodeRecycle,unitAnim().nodeRecycle.data(),std::min<size_t>(10,unitAnim().nodeRecycle.size()*sizeof(float)));

	data->isSmoking = isSmoking;
	data->isWaking = isWaking;
	data->isDusting = isDusting;
	data->fallDust = fallDust;
	data->isHelicopter = isHelicopter;

	data->baseRootNodeHeight = baseRootNodeHeight;
	data->jumpDestination = unitAnim().jumpDestination;
	data->jumpVelocity = unitAnim().jumpVelocity;
}

void Mech3DAppearance::copyFrom (MechAppearanceData *data)
{
	frameNum = data->frameNum;
	unitAnim().mechFrameRate = data->mechFrameRate;

	unitAnim().leftArmOff = data->leftArmOff;
	unitAnim().rightArmOff = data->rightArmOff;
	unitAnim().fallen = data->fallen;
	unitAnim().forceStop = data->forceStop;
	unitAnim().atTransitionToNextGesture = data->atTransitionToNextGesture;
	unitAnim().inReverse = data->inReverse;
	unitAnim().inJump = data->inJump;
	unitAnim().jumpSetup = data->jumpSetup;
	jumpFXSetup = data->jumpFXSetup;
	unitAnim().jumpAirborne = data->jumpAirborne;
	unitAnim().oncePerFrame = data->oncePerFrame;
	unitAnim().lockRotation = data->lockRotation;

	unitAnim().velocity = data->velocity;

	//Let mech reset status when it reloads so that the destroyed shapes come up.
//	status = data->status;

	unitAnim().currentStateGoal = data->currentStateGoal;
	unitAnim().currentGestureId = data->currentGestureId;
	unitAnim().transitionState = data->transitionState;
	unitAnim().oldStateGoal = data->oldStateGoal;
	unitAnim().hitGestureId = data->hitGestureId;

	unitAnim().currentFrame = data->currentFrame;
	currentLOD = data->currentLOD;

	// Mirror of copyTo: only 10 bytes were saved, and a table shorter than that must not be over-written.
	memcpy(unitAnim().nodeUsed.data(),data->nodeUsed,std::min<size_t>(10,unitAnim().nodeUsed.size()*sizeof(long)));		 
	memcpy(unitAnim().nodeRecycle.data(),data->nodeRecycle,std::min<size_t>(10,unitAnim().nodeRecycle.size()*sizeof(float)));

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
	unitAnim().jumpDestination = 	data->jumpDestination;
	unitAnim().jumpVelocity = 		data->jumpVelocity;

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
    if (!simPresentationPass && simPose->ready) return simPose->hitLeft;

	POSE_VIEW_LOCALS
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
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().torsoRotation * DEGREES_TO_RADS),0.0f));
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
    if (!simPresentationPass && simPose->ready) return simPose->hitRight;

	POSE_VIEW_LOCALS
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
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().torsoRotation * DEGREES_TO_RADS),0.0f));
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
