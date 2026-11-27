// Camera: Camera::setPosition, zoom, tilt, rotate, scroll and the input smoothing.
// Declared in the class headers; shared names in camera_internal.h.
#include <algorithm>

#include "camera_internal.h"
#include "sim_trace.h"
#include "camera_elevation.h"

long Camera::getCameraTargetId(void)
{
    return lookTargetObject;
}


static inline float camWrap180 (float a)
{
	while (a > 180.0f)  a -= 360.0f;
	while (a < -180.0f) a += 360.0f;
	return a;
}
 
const float CAM_THRESHOLD = 150.0f;

//---------------------------------------------------------------------------
void Camera::setPosition(glm::vec3 newPosition, bool swoopy)
{
#if MC2_SIM_TRACE
	MC2_SIM_TRACE_LINE("SETPOS new={:g},{:g} old={:g},{:g}", static_cast<double>(newPosition.x), static_cast<double>(newPosition.y), static_cast<double>(position.x), static_cast<double>(position.y));
	MC2_SIM_TRACE_STACK("SETPOS");
#endif
	position = newPosition;

	if (land)
	{
		float maxVisual = (Terrain::worldUnitsMapSide / 2) - (Terrain::worldUnitsPerVertex * 2.0f);

		//This keeps the camera from moving too close to the edge.
		//  -fs
		if (!drawTerrainGrid)
			maxVisual -= (Terrain::worldUnitsPerVertex * 6.0f);

		position.x = std::min(position.x, maxVisual);

		position.y = std::min(position.y, maxVisual);

		position.x = std::max(position.x, -maxVisual);

		position.y = std::max(position.y, -maxVisual);

		cameraShiftZ = land->getTerrainElevation(position);
		cameraShiftZ = std::max(cameraShiftZ, Terrain::waterElevation);

		if (swoopy)
		{
			if (fabs(position.z - cameraShiftZ) > CAM_THRESHOLD)
			{
				goalPositionZ = cameraShiftZ;
			}
	
			if (fabs(goalPositionZ - position.z) > 10.0f )
			{
				float offsetZ = goalPositionZ - position.z;
				offsetZ *= 0.5f * frameLength;
				position.z += offsetZ;
			}
	
			cameraShiftZ = position.z;
		}
		else
		{
			goalPositionZ = position.z = cameraShiftZ;
		}
	}

}

//---------------------------------------------------------------------------
void Camera::setCameraView (long viewNum)
{
	if ((viewNum >= 0) && (viewNum < MAX_VIEWS))
	{
		zoomValue(cameraZoom[viewNum]);
		tiltValue(cameraTilt[viewNum]);
	}
}

//---------------------------------------------------------------------------
void Camera::zoomValue (float value)
{
	if (!usePerspective)
	{
		newScaleFactor = value;
		newScaleFactor = std::min(newScaleFactor, zoomMax);
	}
	else
	{
		cameraAltitude = value;
		cameraAltitudeDesired = cameraAltitude;
		cameraAltitude = std::max(cameraAltitude, AltitudeMinimum);
			
		float testMax = altitudeMaxForAngle(projectionAngle);

		newScaleFactor = 1.0f - ((cameraAltitude - AltitudeMinimum) / testMax);
	}
		
	cameraShiftZ = position.z;
}

//---------------------------------------------------------------------------
void Camera::ZoomIn (float amount)
{
	if (!usePerspective)
	{
		newScaleFactor += amount;
		newScaleFactor = std::min(newScaleFactor, zoomMax);
	}
	else
	{
		// Proportional zoom: strip the scaleFactor baked into amount so step is
		// a fixed % of remaining altitude range regardless of current zoom level.
		// Momentum boosts fast scrolling up to ~2.5x; slow scroll stays at 1x.
		s_scrollMomentum += 1.0f;
		s_scrollMomentum = std::min(s_scrollMomentum, 5.0f);
		cameraAltitudeDesired = mc2cam::zoomElevationTarget(cameraAltitudeDesired,
			AltitudeMinimum, amount, newScaleFactor, s_scrollMomentum);
		cameraAltitudeDesired = std::max(cameraAltitudeDesired, AltitudeMinimum);
		// cameraAltitude lerps toward cameraAltitudeDesired in update()
	}

	cameraShiftZ = position.z;
}

//---------------------------------------------------------------------------
void Camera::ZoomOut (float amount)
{
	if (!usePerspective)
	{
		newScaleFactor -= amount;
		newScaleFactor = std::max(newScaleFactor, zoomMin);
	}
	else
	{
		float testMax = altitudeMaxForAngle(projectionAngle);

		// Proportional zoom: strip scaleFactor bake-in, step = % of remaining range.
		// Momentum boosts fast scrolling up to ~2.5x; slow scroll stays at 1x.
		s_scrollMomentum += 1.0f;
		s_scrollMomentum = std::min(s_scrollMomentum, 5.0f);
		cameraAltitudeDesired = mc2cam::zoomElevationTarget(cameraAltitudeDesired,
			testMax, amount, newScaleFactor, s_scrollMomentum);
		cameraAltitudeDesired = std::min(cameraAltitudeDesired, testMax);
		// cameraAltitude lerps toward cameraAltitudeDesired in update()
	}

	cameraShiftZ = position.z;
}

//---------------------------------------------------------------------------
void Camera::ZoomNormal (void)
{
	if (!usePerspective)
	{
		newScaleFactor = 0.5f;
	}
	else
	{
		camera_fov = DefaultFov;
		float testMax = altitudeMaxForAngle(projectionAngle);
		cameraAltitude = AltitudeDefault;
		cameraAltitudeDesired = cameraAltitude;
 		
		newScaleFactor = 1.0f - ((cameraAltitude - AltitudeMinimum) / testMax);
	}
		
	cameraShiftZ = position.z;
}

//---------------------------------------------------------------------------
void Camera::ZoomMin (void)
{
	if (!usePerspective)
	{
		newScaleFactor = 0.5f;
	}
	else
	{
		camera_fov = DefaultFov;
		float testMax = altitudeMaxForAngle(projectionAngle);
		cameraAltitude = testMax;
		cameraAltitudeDesired = cameraAltitude;
 		
		newScaleFactor = 1.0f - ((cameraAltitude - AltitudeMinimum) / testMax);
	}
		
	cameraShiftZ = position.z;
}

//---------------------------------------------------------------------------
void Camera::ZoomDefault (void)
{
	if (!usePerspective)
	{
		newScaleFactor = 0.5f;
	}
	else
	{
		camera_fov = DefaultFov;
		cameraAltitude = AltitudeDefault;
		cameraAltitudeDesired = cameraAltitude;
		
		float testMax = altitudeMaxForAngle(projectionAngle);

		newScaleFactor = 1.0f - ((cameraAltitude - AltitudeMinimum) / testMax);
	}
		
	cameraShiftZ = position.z;
}

//---------------------------------------------------------------------------
void Camera::ZoomTight (void)
{
	if (!usePerspective)
	{
		newScaleFactor = 0.5f;
	}
	else
	{
		camera_fov = DefaultFov;
		cameraAltitude = AltitudeTight;
		cameraAltitudeDesired = cameraAltitude;
		
		float testMax = altitudeMaxForAngle(projectionAngle);

		newScaleFactor = 1.0f - ((cameraAltitude - AltitudeMinimum) / testMax);
	}
		
	cameraShiftZ = position.z;
}

//---------------------------------------------------------------------------
void Camera::ZoomMax (void)
{
	if (!usePerspective)
	{
		newScaleFactor = 0.5f;
	}
	else
	{
		camera_fov = DefaultFov;
		cameraAltitude = AltitudeMinimum;
		cameraAltitudeDesired = cameraAltitude;
		
		float testMax = altitudeMaxForAngle(projectionAngle);

		newScaleFactor = 1.0f - ((cameraAltitude - AltitudeMinimum) / testMax);
	}
		
	cameraShiftZ = position.z;
}

//---------------------------------------------------------------------------
void Camera::tiltValue (float value)
{
	pitchActive = false;
	projectionAngle = value;
	if (usePerspective)
	{
		projectionAngle = std::max(projectionAngle, MIN_PERSPECTIVE);
	}
	else
	{
		projectionAngle = std::max(projectionAngle, MIN_ORTHO);
	}

	calculateProjectionConstants();
}

//---------------------------------------------------------------------------
void Camera::tiltUp (float amount)
{
	if (!pitchActive || projectionAngle != pitchLast) {
		pitchGoal = pitchLast = projectionAngle;
		pitchVelocity = 0.0f;
	}
	pitchActive = true;
	pitchGoal = std::clamp(pitchGoal - amount, usePerspective ? MIN_PERSPECTIVE : MIN_ORTHO, MAX_PERSPECTIVE);
}

//---------------------------------------------------------------------------
void Camera::tiltDown (float amount)
{
	tiltUp(-amount);
}

//---------------------------------------------------------------------------
void Camera::tiltNormal (void)
{
	pitchActive = false;
	projectionAngle = NORM_PERSPECTIVE + DefaultPitchDelta;
	calculateProjectionConstants();
}

//---------------------------------------------------------------------------
void Camera::rotateNormal (void)
{
	float newCamAngle = -170.0f;
	float newCamWorld = -170.0f;
	setCameraRotation(newCamAngle,newCamWorld);
	
	cameraShiftZ = position.z;
}

//---------------------------------------------------------------------------
void Camera::movePosLeft(float amount, glm::vec3 &pos)
{
	glm::vec3 direction;
	direction.x = -0.7071f;
	direction.y = -0.7071f;
	direction.z = 0.0;

	OppRotate(direction,cameraRotation);
	direction *= amount;

	pos.x += direction.x;
	pos.y += direction.y;
}

//---------------------------------------------------------------------------
void Camera::movePosRight(float amount, glm::vec3 &pos)
{
	glm::vec3 direction;
	direction.x = 0.7071f;
	direction.y = 0.7071f;
	direction.z = 0.0;

	OppRotate(direction,cameraRotation);
	direction *= amount;

	pos.x += direction.x;
	pos.y += direction.y;
}

//---------------------------------------------------------------------------
void Camera::movePosUp(float amount, glm::vec3 &pos)
{
	glm::vec3 direction;
	direction.x = -0.7071f;
	direction.y = 0.7071f;
	direction.z = 0.0;

	OppRotate(direction,cameraRotation);
	direction *= amount;

	pos.x += direction.x;
	pos.y += direction.y;
}

//---------------------------------------------------------------------------
void Camera::movePosDown(float amount, glm::vec3 &pos)
{
	glm::vec3 direction;
	direction.x = 0.7071f;
	direction.y = -0.7071f;
	direction.z = 0.0;

	OppRotate(direction,cameraRotation);
	direction *= amount;

	pos.x += direction.x;
	pos.y += direction.y;
}

//---------------------------------------------------------------------------
void Camera::moveLeft(float amount)
{
	glm::vec3 direction;
	
	if (!usePerspective)
	{
		direction.x = -1.0;
		direction.y = 0.0;
		direction.z = 0.0;
	}
	else
	{
		direction.x = 1.0;
		direction.y = 0.0;
		direction.z = 0.0;
	}

	OppRotate(direction,worldCameraRotation);
	direction *= amount;

	applyScrollDelta(direction.x, direction.y);
}

//---------------------------------------------------------------------------
void Camera::moveRight(float amount)
{
	glm::vec3 direction;
	
	if (!usePerspective)
	{
		direction.x = 1.0;
		direction.y = 0.0;
		direction.z = 0.0;
	}
	else
	{
		direction.x = -1.0;
		direction.y = 0.0;
		direction.z = 0.0;
	}
	
	OppRotate(direction,worldCameraRotation);
	direction *= amount;

	applyScrollDelta(direction.x, direction.y);
}

//---------------------------------------------------------------------------
void Camera::moveUp(float amount)
{
	glm::vec3 direction;
	
	if (!usePerspective)
	{
		direction.x = 0.0;
		direction.y = 1.0;
		direction.z = 0.0;
	}
	else
	{
		direction.x = 0.0;
		direction.y = -1.0;
		direction.z = 0.0;
	}

	OppRotate(direction,worldCameraRotation);
	direction *= amount;

	applyScrollDelta(direction.x, direction.y);
}

//---------------------------------------------------------------------------
void Camera::moveDown(float amount)
{
	glm::vec3 direction;
	
	if (!usePerspective)
	{
		direction.x = 0.0;
		direction.y = -1.0;
		direction.z = 0.0;
	}
	else
	{
		direction.x = 0.0;
		direction.y = 1.0;
		direction.z = 0.0;
	}

	OppRotate(direction,worldCameraRotation);
	direction *= amount;

	applyScrollDelta(direction.x, direction.y);
}

//---------------------------------------------------------------------------
// [CAMERA-SMOOTHING-1] Shared tail of moveLeft/Right/Up/Down. Legacy (knob=0):
// instant setPosition step, byte-identical to the old inline code. Smoothing
// on: accumulate the delta into a goal the update() step eases toward. The
// GOAL is clamped with setPosition's own map-bound rule so smoothing never
// chases an out-of-bounds target (no rubber-banding at the map edge).
void Camera::applyScrollDelta(float dx, float dy)
{
#if MC2_SIM_TRACE
	MC2_SIM_TRACE_LINE("SCROLL dx={:g} dy={:g}", static_cast<double>(dx), static_cast<double>(dy));
	MC2_SIM_TRACE_STACK("SCROLL");
#endif
	if (camSmoothTau() <= 0.0f)
	{
		glm::vec3 newPosition = position;
		newPosition.x += dx;
		newPosition.y += dy;
		setPosition(newPosition);
		return;
	}

	if (!scrollSmoothActive)
	{
		scrollSmoothGoalX = scrollSmoothLastX = position.x;
		scrollSmoothGoalY = scrollSmoothLastY = position.y;
		scrollSmoothActive = true;
	}
	scrollSmoothGoalX += dx;
	scrollSmoothGoalY += dy;

	if (land)
	{
		float maxVisual = (Terrain::worldUnitsMapSide / 2) - (Terrain::worldUnitsPerVertex * 2.0f);
		if (!drawTerrainGrid)
			maxVisual -= (Terrain::worldUnitsPerVertex * 6.0f);
		scrollSmoothGoalX = std::min(scrollSmoothGoalX, maxVisual);
		scrollSmoothGoalX = std::max(scrollSmoothGoalX, -maxVisual);
		scrollSmoothGoalY = std::min(scrollSmoothGoalY, maxVisual);
		scrollSmoothGoalY = std::max(scrollSmoothGoalY, -maxVisual);
	}
}

//---------------------------------------------------------------------------
// [CAMERA-SMOOTHING-1] Rotation-input accumulator (rotateLeft/rotateRight).
// Keeps the quantized 11.25-degree snap TARGETS from MissionInterfaceManager
// but eases the actual rotation toward them, turning visible snaps into a
// short animated turn.
void Camera::queueRotationDelta(float amount)
{
	if (!rotSmoothActive)
	{
		rotSmoothGoalCam   = rotSmoothLastCam = cameraRotation;
		rotSmoothGoalWorld = worldCameraRotation;
		rotSmoothActive = true;
	}
	rotSmoothGoalCam   = camWrap180(rotSmoothGoalCam + amount);
	rotSmoothGoalWorld = camWrap180(rotSmoothGoalWorld + amount);

	cameraShiftZ = position.z;	// legacy rotateLeft/rotateRight side-effect
}

//---------------------------------------------------------------------------
// [CAMERA-SMOOTHING-1] Exposed so input-layer callers (missiongui rotation
// quantizer) can switch to continuous dt-scaled feed when smoothing is on.
bool Camera::inputSmoothingEnabled (void)
{
	return camSmoothTau() > 0.0f;
}

//---------------------------------------------------------------------------
// [CAMERA-SMOOTHING-1] Per-frame exponential approach toward the accumulated
// input goals. Runs from Camera::update() right after the scripted-position
// pipeline so scripted/goal-seek motion wins: if anything OTHER than this
// smoother moved the pose since our last step (bookmark apply, scripted
// swoop, drag-jump, findObject), the goal is dropped immediately — hard-set
// paths stay hard-set and captures stay deterministic.
void Camera::updateInputSmoothing (glm::vec3 &newPosition)
{
	if (scrollSmoothActive)
	{
		// External motion since our last step (scripted pan, target tracking,
		// findObject, bookmark jump): REBASE the goal by the external delta so
		// pending input is neither lost nor replayed against the new pose. The
		// residual eased offset stays small (bounded by ~tau worth of input)
		// and decays in a few hundred ms, so hard-set jumps still land where
		// they aimed.
		{
			float ex = position.x - scrollSmoothLastX;
			float ey = position.y - scrollSmoothLastY;
			if ((fabsf(ex) > 0.01f) || (fabsf(ey) > 0.01f))
			{
				scrollSmoothGoalX += ex;
				scrollSmoothGoalY += ey;
				if (land)
				{
					float maxVisual = (Terrain::worldUnitsMapSide / 2) - (Terrain::worldUnitsPerVertex * 2.0f);
					if (!drawTerrainGrid)
						maxVisual -= (Terrain::worldUnitsPerVertex * 6.0f);
					scrollSmoothGoalX = std::min(scrollSmoothGoalX, maxVisual);
					scrollSmoothGoalX = std::max(scrollSmoothGoalX, -maxVisual);
					scrollSmoothGoalY = std::min(scrollSmoothGoalY, maxVisual);
					scrollSmoothGoalY = std::max(scrollSmoothGoalY, -maxVisual);
				}
			}
		}
		{
			float a  = camSmoothAlpha(frameLength);
			float dx = scrollSmoothGoalX - position.x;
			float dy = scrollSmoothGoalY - position.y;
			glm::vec3 p = position;
			if ((fabsf(dx) < 0.05f) && (fabsf(dy) < 0.05f))
			{
				p.x = scrollSmoothGoalX;
				p.y = scrollSmoothGoalY;
				scrollSmoothActive = false;	// settled
			}
			else
			{
				p.x += dx * a;
				p.y += dy * a;
			}
			setPosition(p);
			scrollSmoothLastX = position.x;
			scrollSmoothLastY = position.y;
			newPosition = position;			// downstream matrix setup sees the smoothed pose
		}
	}

	if (rotSmoothActive)
	{
		// External rotation since our last step: rebase goals by the external
		// delta (same reasoning as the scroll rebase above).
		{
			float er = camWrap180(cameraRotation - rotSmoothLastCam);
			if (fabsf(er) > 0.01f)
			{
				rotSmoothGoalCam   = camWrap180(rotSmoothGoalCam + er);
				rotSmoothGoalWorld = camWrap180(rotSmoothGoalWorld + er);
			}
		}
		{
			float a  = camSmoothAlpha(frameLength);
			float dc = camWrap180(rotSmoothGoalCam - cameraRotation);
			float dw = camWrap180(rotSmoothGoalWorld - worldCameraRotation);
			if ((fabsf(dc) < 0.02f) && (fabsf(dw) < 0.02f))
			{
				setCameraRotation(rotSmoothGoalCam, rotSmoothGoalWorld);
				rotSmoothActive = false;	// settled
			}
			else
			{
				setCameraRotation(cameraRotation + dc * a, worldCameraRotation + dw * a);
			}
			rotSmoothLastCam = cameraRotation;
		}
	}
}

//---------------------------------------------------------------------------
void Camera::rotateLightLeft(float amount)
{
	if (todCurve) { setSunPitch(lightPitch, true, lightYaw + amount); return; }
	lightYaw += amount;
	if (lightYaw > 360.0)
		lightYaw -= 360;

	//Replace with TGL Lights
	glm::mat4 lightToWorldMatrix;

	mc2::math::buildTranslation(lightToWorldMatrix, glm::vec3(0.0,0.0,0.0));
	mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));

	worldLights[0]->SetLightToWorld(&lightToWorldMatrix);
}	

//---------------------------------------------------------------------------
void Camera::rotateLightRight(float amount)
{
	if (todCurve) { setSunPitch(lightPitch, true, lightYaw - amount); return; }
	lightYaw -= amount;
	if (lightYaw < -360.0)
		lightYaw += 360;

	//Replace with TGL Lights
	glm::mat4 lightToWorldMatrix;
	mc2::math::buildTranslation(lightToWorldMatrix, glm::vec3(0.0,0.0,0.0));
	mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));

	worldLights[0]->SetLightToWorld(&lightToWorldMatrix);
}	

//---------------------------------------------------------------------------
void Camera::rotateLightUp(float amount)
{
	if (todCurve) { setSunPitch(std::min(lightPitch + amount, 90.0f)); return; }
	lightPitch += amount;
	lightPitch = std::min<double>(lightPitch, 90.0);

	//Replace with TGL Lights
	glm::mat4 lightToWorldMatrix;
	
	mc2::math::buildTranslation(lightToWorldMatrix, glm::vec3(0.0,0.0,0.0));
	mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));

	worldLights[0]->SetLightToWorld(&lightToWorldMatrix);
}	

//---------------------------------------------------------------------------
void Camera::rotateLightDown(float amount)
{
	if (todCurve) { setSunPitch(std::max(lightPitch - amount, 5.0f)); return; }
	lightPitch -= amount;
	lightPitch = std::max(lightPitch, 5.0f);

	//Replace with TGL Lights
	glm::mat4 lightToWorldMatrix;
	
	mc2::math::buildTranslation(lightToWorldMatrix, glm::vec3(0.0,0.0,0.0));
	mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));

	worldLights[0]->SetLightToWorld(&lightToWorldMatrix);
}	

//---------------------------------------------------------------------------
void Camera::rotateLeft(float amount)
{
	// [CAMERA-SMOOTHING-1] smoothing on: accumulate goal, ease in update().
	if (camSmoothTau() > 0.0f)
	{
		queueRotationDelta(amount);
		return;
	}

	float newCamAngle = cameraRotation+amount;
	float newCamWorld = worldCameraRotation+amount;
	setCameraRotation(newCamAngle,newCamWorld);

	cameraShiftZ = position.z;
}

//---------------------------------------------------------------------------
void Camera::rotateRight(float amount)
{
	// [CAMERA-SMOOTHING-1] smoothing on: accumulate goal, ease in update().
	if (camSmoothTau() > 0.0f)
	{
		queueRotationDelta(-amount);
		return;
	}

	float newCamAngle = cameraRotation-amount;
	float newCamWorld = worldCameraRotation-amount;
	setCameraRotation(newCamAngle,newCamWorld);

	cameraShiftZ = position.z;
}

//---------------------------------------------------------------------------
void Camera::setCameraRotation (float angle, float worldAngle)
{			
	worldCameraRotation = worldAngle;
	if (worldCameraRotation > 180)
		worldCameraRotation -= 360;

	if (worldCameraRotation < -180)
		worldCameraRotation += 360;

	cameraRotation = angle;
	if (cameraRotation > 180)
		cameraRotation -= 360;

	if (cameraRotation < -180)
		cameraRotation += 360;

	cameraDirection.yaw = cameraRotation * DEGREES_TO_RADS;
	cameraDirection.pitch = -projectionAngle * DEGREES_TO_RADS;
	cameraDirection.range = 0.0;
}

//---------------------------------------------------------------------------
float Camera::getCameraRotation (void)
{
	return worldCameraRotation;
}
