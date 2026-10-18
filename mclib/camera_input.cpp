// Camera: Camera::setPosition, zoom, tilt, rotate, scroll and the input smoothing.
// Declared in the class headers; shared names in camera_internal.h.
#include "camera_internal.h"
#include "sim_trace.h"


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
	MC2_SIM_TRACE_LINE("SETPOS new=%g,%g old=%g,%g", static_cast<double>(newPosition.x), static_cast<double>(newPosition.y), static_cast<double>(position.x), static_cast<double>(position.y));
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

		if (position.x > maxVisual)
			position.x = maxVisual;

		if (position.y > maxVisual)
			position.y = maxVisual;

		if (position.x < -maxVisual)
			position.x = -maxVisual;

		if (position.y < -maxVisual)
			position.y = -maxVisual;

		cameraShiftZ = land->getTerrainElevation(position);
		if (cameraShiftZ < Terrain::waterElevation)
			cameraShiftZ = Terrain::waterElevation;

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

#if 0 //Used to restrict camera to the diamond.  Not used Anymore!	
	//--------------------------------------------------------------
	// Use a different clip constant for the corner to clip them
	// off diagonally!  Or the designers must wet them!!!!!
	float maxVisual = Terrain::worldUnitsPerVertex * Terrain::blocksMapSide * Terrain::verticesBlockSide / 2;
	float zoomOutClip = 550.0;
	float zoomInClip = 375.0;
	float MAX_CORNER_DISTANCE = 1300.0;
	
	glm::vec3 cornerPos1(maxVisual,0,0), cornerPos2(0,maxVisual,0), cornerPos3(-maxVisual,0,0), cornerPos4(0,-maxVisual,0);
	glm::vec3 distance1, distance2, distance3, distance4;

	distance1.Subtract(newPosition,cornerPos1);
	distance2.Subtract(newPosition,cornerPos2);
	distance3.Subtract(newPosition,cornerPos3);
	distance4.Subtract(newPosition,cornerPos4);

	float dist1 = distance1.GetLength();
	float dist2 = distance2.GetLength();
	float dist3 = distance3.GetLength();
	float dist4 = distance4.GetLength();
	
	if (newScaleFactor <= 0.5)
	{
		float realMaxDistance = zoomOutClip + MAX_CORNER_DISTANCE;
		float clipConstant = zoomOutClip;
		if (dist1 < realMaxDistance)
		{
			clipConstant = dist1 - MAX_CORNER_DISTANCE;
		}
		else if (dist2 < realMaxDistance)
		{
			clipConstant = dist2 - MAX_CORNER_DISTANCE;
		}
		else if (dist3 < realMaxDistance)
		{
			clipConstant = dist3 - MAX_CORNER_DISTANCE;
		}
		else if (dist4 < realMaxDistance)
		{
			clipConstant = dist4 - MAX_CORNER_DISTANCE;
		}
		
		if (clipConstant > 0.0)
		{
			zoomOutClip = MAX_CORNER_DISTANCE - (clipConstant / zoomOutClip * (MAX_CORNER_DISTANCE-zoomOutClip));
		}
		else
		{
			zoomOutClip = MAX_CORNER_DISTANCE;
		}
	}
	else
	{
		float realMaxDistance = zoomInClip + MAX_CORNER_DISTANCE;
		float clipConstant = zoomInClip;
		if (dist1 < realMaxDistance)
		{
			clipConstant = dist1 - MAX_CORNER_DISTANCE;
		}
		else if (dist2 < realMaxDistance)
		{
			clipConstant = dist2 - MAX_CORNER_DISTANCE;
		}
		else if (dist3 < realMaxDistance)
		{
			clipConstant = dist3 - MAX_CORNER_DISTANCE;
		}
		else if (dist4 < realMaxDistance)
		{
			clipConstant = dist4 - MAX_CORNER_DISTANCE;
		}
		
		if (clipConstant > 0.0)
		{
			zoomInClip = MAX_CORNER_DISTANCE - (clipConstant / zoomInClip * (MAX_CORNER_DISTANCE-zoomInClip));
		}
		else
		{
			zoomInClip = MAX_CORNER_DISTANCE;
		}
	}
	
	//--------------------------------------------------------------
	// New camera clip code here.  Actually easy once thought out!
	if (newScaleFactor <= 0.5)
		maxVisual -= zoomOutClip;
	else
		maxVisual -= zoomInClip;
		
	float clipChk1 = newPosition.y - newPosition.x;
	float clipChk2 = newPosition.y + newPosition.x;
	
	bool clipA = (clipChk1 > maxVisual);
	bool clipB = (clipChk1 < -maxVisual);
	bool clipC = (clipChk2 > maxVisual);
	bool clipD = (clipChk2 < -maxVisual);
	
	if (!clipA && !clipB && !clipC && !clipD)
		return;		//No worries.  Camera on MAP.
		
	//------------------------------------------------------------
	// Simple checks first.  Off of the corner.  Force to corner.
	if (clipA && clipD)
	{
		position.x = -maxVisual;
		position.y = 0.0;
		return;
	}
	
	if (clipA && clipC)
	{
		position.x = 0.0;
		position.y = maxVisual;
		return;
	}
	
	if (clipB && clipD)
	{
		position.x = 0;
		position.y = -maxVisual;
		return;
	}
	
	if (clipB && clipC)
	{
		position.x = maxVisual;
		position.y = 0;
		return;
	}
	
	//---------------------------------------------------
	// Calc based on Single Clip value
	if (clipA)
	{
		float newX = (newPosition.y + newPosition.x - maxVisual) / 2.0;
		float newY = newX + maxVisual;
		position.x = newX;
		position.y = newY;
		return;
	}
	
	if (clipB)
	{
		float newX = (newPosition.y + newPosition.x + maxVisual) / 2.0;
		float newY = newX - maxVisual;
		position.x = newX;
		position.y = newY;
		return;
	}
	
	if (clipC)
	{
		float newX = (-newPosition.y + newPosition.x + maxVisual) / 2.0;
		float newY = -newX + maxVisual;
		position.x = newX;
		position.y = newY;
		return;
	}
	
	if (clipD)
	{
		float newX = (-newPosition.y + newPosition.x - maxVisual) / 2.0;
		float newY = -newX - maxVisual;
		position.x = newX;
		position.y = newY;
		return;
	}

#ifdef _DEBUG
	bool CameraClipImpossible = FALSE;
#endif

	gosASSERT(CameraClipImpossible);
#endif
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
		if (newScaleFactor > zoomMax)
			newScaleFactor = zoomMax;
	}
	else
	{
		cameraAltitude = value;
		cameraAltitudeDesired = cameraAltitude;
		if (cameraAltitude < AltitudeMinimum)
			cameraAltitude = AltitudeMinimum;
			
		float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
		float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);

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
		if (newScaleFactor > zoomMax)
			newScaleFactor = zoomMax;
	}
	else
	{
		// Proportional zoom: strip the scaleFactor baked into amount so step is
		// a fixed % of remaining altitude range regardless of current zoom level.
		// Momentum boosts fast scrolling up to ~2.5x; slow scroll stays at 1x.
		s_scrollMomentum += 1.0f;
		if (s_scrollMomentum > 5.0f) s_scrollMomentum = 5.0f;
		float speedMult = 1.0f + s_scrollMomentum * 0.3f;
		float sf = newScaleFactor > 0.01f ? newScaleFactor : 0.01f;
		float altRange = max(1.0f, cameraAltitudeDesired - AltitudeMinimum);
		cameraAltitudeDesired -= altRange * (amount / sf) * 0.045f * speedMult;
		if (cameraAltitudeDesired < AltitudeMinimum)
			cameraAltitudeDesired = AltitudeMinimum;
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
		if (newScaleFactor < zoomMin)
			newScaleFactor = zoomMin;
	}
	else
	{
		float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
		float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);

		// Proportional zoom: strip scaleFactor bake-in, step = % of remaining range.
		// Momentum boosts fast scrolling up to ~2.5x; slow scroll stays at 1x.
		s_scrollMomentum += 1.0f;
		if (s_scrollMomentum > 5.0f) s_scrollMomentum = 5.0f;
		float speedMult = 1.0f + s_scrollMomentum * 0.3f;
		float sf = newScaleFactor > 0.01f ? newScaleFactor : 0.01f;
		float altRange = max(1.0f, testMax - cameraAltitudeDesired);
		cameraAltitudeDesired += altRange * (amount / sf) * 0.045f * speedMult;
		if (cameraAltitudeDesired > testMax)
			cameraAltitudeDesired = testMax;
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
		camera_fov = 40.0f;
		cosHalfFOV = cos(camera_fov * DEGREES_TO_RADS);
		float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
		float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);
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
		camera_fov = 40.0f;
		cosHalfFOV = cos(camera_fov * DEGREES_TO_RADS);
		float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
		float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);
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
		camera_fov = 40.0f;
		cosHalfFOV = cos(camera_fov * DEGREES_TO_RADS);
		cameraAltitude = AltitudeDefault;
		cameraAltitudeDesired = cameraAltitude;
		
		float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
		float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);

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
		camera_fov = 40.0f;
		cosHalfFOV = cos(camera_fov * DEGREES_TO_RADS);
		cameraAltitude = AltitudeTight;
		cameraAltitudeDesired = cameraAltitude;
		
		float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
		float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);

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
		camera_fov = 40.0f;
		cosHalfFOV = cos(camera_fov * DEGREES_TO_RADS);
		cameraAltitude = AltitudeMinimum;
		cameraAltitudeDesired = cameraAltitude;
		
		float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
		float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);

		newScaleFactor = 1.0f - ((cameraAltitude - AltitudeMinimum) / testMax);
	}
		
	cameraShiftZ = position.z;
}

//---------------------------------------------------------------------------
void Camera::tiltValue (float value)
{
	projectionAngle = value;
	if (usePerspective)
	{
		if (projectionAngle < MIN_PERSPECTIVE)
			projectionAngle = MIN_PERSPECTIVE;
	}
	else
	{
		if (projectionAngle < MIN_ORTHO)
			projectionAngle = MIN_ORTHO;
	}

	calculateProjectionConstants();
}

//---------------------------------------------------------------------------
void Camera::tiltUp (float amount)
{
	projectionAngle -= amount;
	if (usePerspective)
	{
		if (projectionAngle < MIN_PERSPECTIVE)
			projectionAngle = MIN_PERSPECTIVE;
	}
	else
	{
		if (projectionAngle < MIN_ORTHO)
			projectionAngle = MIN_ORTHO;
	}

	calculateProjectionConstants();
}

//---------------------------------------------------------------------------
void Camera::tiltDown (float amount)
{
	projectionAngle += amount;

	if (projectionAngle > MAX_PERSPECTIVE)
		projectionAngle = MAX_PERSPECTIVE;

	calculateProjectionConstants();
}

//---------------------------------------------------------------------------
void Camera::tiltNormal (void)
{
	projectionAngle = NORM_PERSPECTIVE;
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
	MC2_SIM_TRACE_LINE("SCROLL dx=%g dy=%g", static_cast<double>(dx), static_cast<double>(dy));
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
		if (scrollSmoothGoalX >  maxVisual) scrollSmoothGoalX =  maxVisual;
		if (scrollSmoothGoalX < -maxVisual) scrollSmoothGoalX = -maxVisual;
		if (scrollSmoothGoalY >  maxVisual) scrollSmoothGoalY =  maxVisual;
		if (scrollSmoothGoalY < -maxVisual) scrollSmoothGoalY = -maxVisual;
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
					if (scrollSmoothGoalX >  maxVisual) scrollSmoothGoalX =  maxVisual;
					if (scrollSmoothGoalX < -maxVisual) scrollSmoothGoalX = -maxVisual;
					if (scrollSmoothGoalY >  maxVisual) scrollSmoothGoalY =  maxVisual;
					if (scrollSmoothGoalY < -maxVisual) scrollSmoothGoalY = -maxVisual;
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
	lightPitch += amount;
	if (lightPitch > 90.0)
		lightPitch = 90.0;

	//Replace with TGL Lights
	glm::mat4 lightToWorldMatrix;
	
	mc2::math::buildTranslation(lightToWorldMatrix, glm::vec3(0.0,0.0,0.0));
	mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));

	worldLights[0]->SetLightToWorld(&lightToWorldMatrix);
}	

//---------------------------------------------------------------------------
void Camera::rotateLightDown(float amount)
{
	lightPitch -= amount;
	if (lightPitch < 5.0f)
		lightPitch = 5.0f;

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

#define OBSCURED_FACTOR	0.5f;
