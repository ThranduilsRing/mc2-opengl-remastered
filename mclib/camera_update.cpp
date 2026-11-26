#include "footprints.h"
// Camera: the camera goal updates, Camera::update and render.
// Declared in the class headers; shared names in camera_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include <algorithm>
#include <chrono>
#include <vector>
#include "camera_internal.h"
#include "mc2fmt.h"
#include "Gates/render_options.h"
#include "camera_elevation.h"
#include "diagnostic_trace.h"
#include "terrain_runtime.h"

extern bool useFog;



//---------------------------------------------------------------------------
mc2::tgl::ViewUniforms Camera::getViewUniforms()
{
	auto view = tglView;
	view.perspective = usePerspective;
	view.night = isNight;
	view.nightFactor = nightFactor;
	view.fogEnabled = useFog;
	view.haze = HazeFactor;
	// The CPU fallback consumes only ambient and infinite lights. Preserve their
	// order and copy their values before any leaf draws; never retain TG_Light*.
	for (int32_t i = 0; i < numActiveLights && i < MAX_LIGHTS_IN_WORLD; ++i) {
		auto* light = activeLights[i];
		if (light && light->active && (light->lightType == TG_LIGHT_AMBIENT || light->lightType == TG_LIGHT_INFINITE))
			view.lights.push_back({light->lightType, light->GetaRGB(), light->lightToWorld});
	}
	return view;
}

void Camera::updateGoalVelocity (void)
{
	if (goalVelTime > 0.0)
	{
		glm::vec3 velDiff;
		velDiff = goalVelocity - velocity;
		velDiff /= goalVelTime;
		velDiff *= frameLength;
		goalVelTime -= frameLength;

		if (goalVelTime < 0.0)
			velocity = goalVelocity;
		else
			velocity = velocity + velDiff;
	}
	else
	{
		goalVelTime = 0.0;
		goalVelocity = velocity;
	}
}

//---------------------------------------------------------------------------
void Camera::updateGoalFOV (void)
{
	if (goalFOVTime > 0.0)
	{
		float fovDiff = goalFOV - camera_fov;
		fovDiff /= goalFOVTime;
		fovDiff *= frameLength;
		
 		goalFOVTime -= frameLength;

		if (goalFOVTime < 0.0)
		{
			camera_fov = goalFOV;
		}
		else
		{
			camera_fov += fovDiff;
		}
	}
	else
	{
		goalFOVTime = 0.0;
	}
}

//---------------------------------------------------------------------------
void Camera::updateGoalRotation (void)
{
	if (goalRotTime > 0.0)
	{
		float goalProjectionAngle = goalRotation.x - projectionAngle;
		goalProjectionAngle /= goalRotTime;
		goalProjectionAngle *= frameLength;
		
		float goalCameraRotation = goalRotation.y - cameraRotation;
		goalCameraRotation /= goalRotTime;
		goalCameraRotation *= frameLength;
		
 		goalRotTime -= frameLength;

		if (goalRotTime < 0.0)
		{
			projectionAngle = goalRotation.x;
			cameraRotation = goalRotation.y;
			setCameraRotation(cameraRotation,cameraRotation);
		}
		else
		{
			projectionAngle += goalProjectionAngle;
			cameraRotation += goalCameraRotation;
			setCameraRotation(cameraRotation,cameraRotation);
		}
	}
	else
	{
		goalRotTime = 0.0;
	}
}

//---------------------------------------------------------------------------
void Camera::updateGoalPosition (glm::vec3 &pos)
{
	if (goalPosTime > 0.0)
	{
		glm::vec3 posDiff;
		posDiff = goalPosition - pos;
		//sebi
		//posDiff /= goalPosTime;
		//posDiff *= frameLength;
		posDiff *= frameLength / goalPosTime;
		goalPosTime -= frameLength;

		if (goalPosTime < 0.0f)
			pos = goalPosition;
		else
			pos = position + posDiff;
	}
	else
	{
		goalPosTime = 0.0;
		goalPosition = pos;
	}
}

//---------------------------------------------------------------------------
// The sun direction from lightYaw / lightPitch (see the MC2_SUN_ELEVATION_FIX note in Camera::update). Also read at mission load, before the first
// update, so the load-time shadow bake keys on the same sun as frame 0.
glm::vec3 Camera::sunDirection (void) const
{
	return sunDirectionFor(lightPitch, lightYaw);
}

// The same expression for any pitch/yaw: the time-of-day curve's composed angles go through it, so the published direction is the
// one Camera::update() derives (not lighting_state.h's older non-spherical init expression).
glm::vec3 Camera::sunDirectionFor (float lightPitch, float lightYaw)
{
	static const bool s_sunElevationFix = mc2::options::current().sunElevationFix;

	const float sunAzimuthRad = (lightYaw + 45.0f) * DEGREES_TO_RADS;
	const float sunPitchRad   = lightPitch * DEGREES_TO_RADS;
	const float sunCosPitch   = cos(sunPitchRad);

	glm::vec3 d;
	if (s_sunElevationFix && sunCosPitch >= -1e-6f)
	{
		// True spherical direction at the AUTHORED elevation. Already unit
		// length, so the Normalize below is a no-op rather than a correction.
		d.x = sunCosPitch * cos(sunAzimuthRad);
		d.y = sunCosPitch * sin(sunAzimuthRad);
		d.z = sin(sunPitchRad);
	}
	else
	{
		d.x = cos(sunAzimuthRad);
		d.y = sin(sunAzimuthRad);
		d.z = sin(sunPitchRad);
	}
	return glm::normalize(d);
}

long Camera::update (void)
{
	// Measure the actual cadence of the scripted fly-in, independent of the
	// fixed sim clock and the clamped presentation delta. Bound storage and
	// emit each 120-frame window plus the final partial window.
	{
		using Clock = std::chrono::steady_clock;
		static auto previous = Clock::now();
		static bool wasMovie = false;
		static std::vector<double> samples;
		const auto now = Clock::now();
		if (inMovieMode && wasMovie) {
			const double ms = std::chrono::duration<double, std::milli>(now - previous).count();
			samples.push_back(ms);
			if (mc2_diag::tagRequested("HITCH")) {
				const glm::vec3 p(-physicalPos.x, physicalPos.z, physicalPos.y);
				mc2::fmt::print_err("[INTRO_GLIDE] ms={:.3f} eye_z={:.2f} ground_z={:.2f} margin={:.2f}\n",
					ms, p.z, TerrainRuntime::sampleVisibleGroundHeight(p), std::max(AltitudeMinimum, 10.0f));
			}
		}
		if (!samples.empty() && (samples.size() == 120 || !inMovieMode)) {
			std::sort(samples.begin(), samples.end());
			double sum = 0.0;
			unsigned over50 = 0, over10 = 0;
			for (double ms : samples) { sum += ms; if (ms > 50.0) ++over50; if (ms > 10.0) ++over10; }
			const size_t n = samples.size();
			if (mc2::gate::get<"MC2_DIAG_VERBOSE">() || mc2_diag::tagRequested("HITCH")) mc2::fmt::print_err("[CINEMATIC] frames={} avg_ms={:.3f} p50_ms={:.3f} p95_ms={:.3f} p99_ms={:.3f} max_ms={:.3f} over50={} over10={}\n",
				n, sum / n, samples[(n - 1) / 2], samples[(n - 1) * 95 / 100],
					samples[(n - 1) * 99 / 100], samples.back(), over50, over10);
			samples.clear();
		}
		if (wasMovie && !inMovieMode) mc2::fmt::print_err("[CINEMATIC_END] turn={}\n", turn);
		previous = now;
		wasMovie = inMovieMode;
	}
	//---------------------------------------------------------
	// This is the guts.  This routine will be used to
	// move the camera, cut to different shots, etc.

	switch (cameraClass)
	{
		case BASE_CAMERA:
			break;
			
		default:
			return static_cast<long>(INVALID_CAMERA);
	}

	float oldNightFactor = nightFactor;
	
	//updateDaylight();
	
	//----------------------------------------------------
	// Check if light moved.
	// Should not be necessary every frame.
	// TEST FOR NOW!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	//
	// SUN-ELEVATION-FIX-1 -- options.cfg SunElevationFix, DEFAULT OFF.
	//
	// THE DEFECT, and it is arithmetic, not taste. The legacy three lines below
	// build a vector whose HORIZONTAL part is already unit length and whose z is
	// sin(pitch), and only THEN normalize the whole thing. That is not a
	// direction at elevation `pitch`: its elevation is atan(sin(pitch)). Every
	// mission with a non-zero authored pitch therefore gets a sun LOWER than the
	// one its .fit asked for -- mc2_24 authors 75 deg and receives 44.0, mc2_01
	// authors 50 and receives 37.5, mc2_03 authors 46 and receives 35.7.
	// Measured across all 24 shipping missions by tools/class_light_band_harness
	// (test terrain_sun_elevation_is_atan_sin_pitch_not_pitch, Stage B5).
	//
	// WHY THIS IS GATED AND WHY IT IS OFF. lightDirection is NOT a local
	// terrain-shading detail -- it is the single source for gos_GetTerrainLightDir,
	// and through it for: the terrain lighting NdotL; the static-prop sun
	// (u_terrainSunMC2); the SHADOW projection matrix (gos_postprocess.cpp:455
	// buildStaticLightMatrix, :3214, :5208, :5502, :5688); SSAO/screen-FX; and --
	// via `intensity = terrainNormal . lightDirection` handed to
	// Camera::getLightRed (mclib/mapdata.cpp:1898) -- the SUN COLOUR BYTE of
	// every actor class. So this moves the sun's direction and the world's shadow
	// direction together, on every mission at once. That is far too large to
	// enable on arithmetic alone with launches blocked.
	//
	// WHAT IT DOES *NOT* REACH, checked by call order rather than assumed: the
	// pre-burned terrain colormap. burnInShadows (mclib/terrtxm2.cpp:609) reads
	// eye->lightDirection only under `if (eye)` at :816, and its call chain runs
	// inside land->init() at code/mission.cpp:3710 -- BEFORE `eye = new
	// GameCamera` at :5108, with eye nulled on teardown at :5541. At burn time
	// eye is NULL, so the bake takes the (0,0,1) straight-up fallback and the
	// mission sun never reaches it; it is also one-shot and disk-cached
	// (`if (!burnedIn)` then saveTGAFile). An earlier draft of this comment and
	// of the registry entry claimed the colormap moved. It does not.
	//
	// DEGENERATE-PITCH GUARD. mc2_10 authors pitch 180 and mc2_05 / mc2_21 author
	// 270, which are not solar elevations. For cos(pitch) < 0 the spherical form
	// flips the AZIMUTH 180 deg against every shipped build, so those missions
	// return the legacy vector VERBATIM -- among the 24 that is mc2_10 alone, one
	// of the user's regular check missions. cos(pitch) == 0 (pitch 270) is NOT
	// degenerate: the spherical form collapses to straight down (0,0,-1) and no
	// azimuth exists to flip. THE EPSILON IS LOAD-BEARING: cos(270 deg) evaluates
	// to -1.8e-16, not 0, so a bare `< 0.0` test would silently drop mc2_05 and
	// mc2_21 onto the legacy branch. The harness caught exactly that.
	//
	// WHEN ON, this also UNIFIES the world: the corrected terrain sun becomes
	// EXACTLY -swap(ObjectLights.light_dir), the vector mech specular already
	// uses and the one SunAxisFix gives mech/vehicle diffuse -- so terrain,
	// props, mech diffuse, mech specular and vehicles land on ONE sun vector by
	// construction. Neither gate alone achieves that (harness test
	// fix_b_unifies_terrain_sun_with_the_object_row).
	//
	// Unset/0 -> the legacy three lines, bit-for-bit.
	// Canonical DEFAULT-OFF OPT-IN idiom `(v && v[0] && v[0] != '0')`. This form
	// is mandatory, not stylistic: scripts/check-gate-default-agreement.py reads
	// a Feature gate's real default OUT OF ITS READER and compares it with the
	// registry. The first draft here used an early-return for the unset case and
	// then `return !(v[0] == '0' ...)`, and the checker latched onto that second
	// statement and classified the gate as DEFAULT-ON -- a direct contradiction
	// of the registry's stated OFF, reported as a DISAGREEMENT. The behaviour was
	// right and the idiom was unreadable, which is exactly how the four gate-
	// default defects found today survived. Do not "simplify" this back.
	lightDirection = sunDirection();

	//If we are not transitioning, we need to set the night/day flags and factors.
	//If we ARE transitioning, they have already been set in updateDaylight!
	//
	// WE ARE NEVER TRANSITIONING ANYMORE
	//if (day2NightTransitionTime <= 0.0f)
	{
   	 	//Set flags to help renderer with light sources.
   		float lightAltitude = lightPitch;
   		if (lightPitch > 90.0f)
   			lightAltitude = 180.0f - lightPitch;

		//--------------------------------------------------
		//Set is night based on pitch.
		//Do NOT draw shadows for infinite light if NIGHT!
		if (lightAltitude < NIGHT_LIGHT_PITCH)
		{
			isNight = true;
			nightFactor = 1.0f;	
		}
		else
		{
			isNight = false;
			if (lightAltitude > NIGHT_START_PITCH)
			{
				nightFactor = 0.0f;
			}
			else
			{
				nightFactor = (NIGHT_START_PITCH - lightPitch) / (NIGHT_START_PITCH - NIGHT_LIGHT_PITCH);
			}
		}
	}
	
	//-----------------------------------------------------
	// Check if we need to re-burn the terrain lights.
	if (!terrainLightCalc)
	{
		if ((!terrainLightNight) && (nightFactor > oldNightFactor) && (nightFactor > TERRAIN_LIGHTS_ON))
		{
			terrainLightCalc = true;
			terrainLightNight = true;
		}
		
		if ((terrainLightNight) && (nightFactor < oldNightFactor) && (nightFactor < TERRAIN_LIGHTS_ON))
		{
			terrainLightCalc = true;
			terrainLightNight = false;
		}
	}
	
 	//--------------------------------------------------
	// Camera Scripting Here.
	// Basically, update anything we have to do.
	glm::vec3 newPosition = position;
	
	//------------------
	// Velocity Update
	updateGoalVelocity();
	
	if ((velocity.x != 0.0) &&
		(velocity.y != 0.0) &&
		(velocity.z != 0.0))
	{
		glm::vec3 posOffset;
		posOffset = velocity;
		posOffset *= frameLength;
		
		newPosition += posOffset;
	}
	
	updateGoalPosition(newPosition);
	updateGoalRotation();
	if (pitchActive) {
		if (projectionAngle != pitchLast || goalRotTime > 0.0f)
			pitchActive = false;
		else {
			projectionAngle = mc2cam::easeElevation(projectionAngle, pitchGoal, pitchVelocity, frameLength,
				usePerspective ? MIN_PERSPECTIVE : MIN_ORTHO, MAX_PERSPECTIVE);
			pitchLast = projectionAngle;
		}
	}
	updateGoalFOV();
	
	setPosition(newPosition);		//THIS ALWAYS sets z position for us now!!!!!

	// [CAMERA-SMOOTHING-1] ease actual pose toward input-accumulated goals
	// (edge/keyboard scroll, rotation). No-op when inactive or knob=0.
	updateInputSmoothing(newPosition);

	//----------------------------------------------
	//Update fades and letterbox transitions here.
	updateLetterboxAndFade();
	
	//----------------------
	// Setup Matrices, etc.
	cameraShift.x = newPosition.x;
	cameraShift.y = newPosition.y;

	if (usePerspective)
		cameraShiftZ = newPosition.z;

	//--------------------------------------------------------
	// Get new viewport values to scale stuff.  No longer uses
	// VFX stuff for this.  ALL GOS NOW!
	//gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);

	if (land)
	{
		if (newScaleFactor >= 0.75)
			Terrain::terrainTextures->setMipLevel(0);
		else if (newScaleFactor >= 0.3)
		{
			Terrain::terrainTextures->setMipLevel(1);
		}
		else if (newScaleFactor >= 0.2)
		{
			Terrain::terrainTextures->setMipLevel(2);
		}
		else
			Terrain::terrainTextures->setMipLevel(3);
	}

	//--------------------------------------------------------
	// Get new viewport values to scale stuff.  No longer uses
	// VFX stuff for this.  ALL GOS NOW!
	screenResolution.x = viewMulX;
	screenResolution.y = viewMulY;

	// Smooth altitude with an exact critically damped step; hard-set poses reset momentum.
	// Also decay scroll momentum (used for adaptive scroll-speed zoom).
	{
		s_scrollMomentum -= frameLength * 4.0f;
		s_scrollMomentum = std::max(s_scrollMomentum, 0.0f);

		float _ap = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
		float _tm = AltitudeMaximumLo + ((AltitudeMaximumHi - AltitudeMaximumLo) * _ap);
		if (cameraAltitude != altitudeLast) altitudeVelocity = 0.0f;
		cameraAltitude = mc2cam::easeElevation(cameraAltitude, cameraAltitudeDesired, altitudeVelocity,
			frameLength, AltitudeMinimum, std::max(AltitudeMinimum, _tm));
		altitudeLast = cameraAltitude;
		if (_tm > 0.0f)
			newScaleFactor = 1.0f - ((cameraAltitude - AltitudeMinimum) / _tm);
	}

	// [MOUSE-ANCHORED-ZOOM-1 v2] eased pivot shift toward the cursor anchor,
	// phase-locked to the just-eased cameraAltitude so the world point under the
	// cursor stays put as zoom resolves (vs the prior single-shot shift that
	// panned). frac grows 0 -> final as altitude eases from h0 to the target.
	// Armed only by beginZoomAnchor (gated MC2_LOWCAM_ZOOM_ANCHOR in anchoredZoom).
	if (zoomAnchorActive)
	{
		float frac = 1.0f - (cameraAltitude / zoomAnchorH0);
		frac = std::clamp(frac, -1.0f, 1.0f);
		glm::vec3 Tnew = zoomAnchorPivot;
		Tnew.x = zoomAnchorPivot.x + (zoomAnchorWorld.x - zoomAnchorPivot.x) * frac;
		Tnew.y = zoomAnchorPivot.y + (zoomAnchorWorld.y - zoomAnchorPivot.y) * frac;
		setPosition(Tnew, false);   // instant, no swoop double-smoothing
		if (fabsf(cameraAltitude - cameraAltitudeDesired) < 0.1f)
			zoomAnchorActive = false;   // settled -> release
	}

	calculateProjectionConstants();

	globalScaleFactor = getScaleFactor();
	globalScaleFactor *= viewMulX / Environment.screenWidth;		//Scale Mechs to ScreenRES
	
	//-----------------------------------------------
	// Set Ambient for this pass of rendering	
	DWORD lightRGB = (ambientRed<<16)+(ambientGreen<<8)+ambientBlue;
		
	setLightColor(1,lightRGB);
	setLightIntensity(1,1.0);

    updateLights();

	{
		// F3 CPU projection cost-baseline: matrix_build site (b) â€”
		// Per-camera CPU projection values, copied by each draw producer.
		::mc2_cpu_proj_cost::Scope _f3_setcam_scope(
		    ::mc2_cpu_proj_cost::BucketId::BUCKET_MATRIX_BUILD);
		tglView.SetCameraMatrices(cameraOrigin,cameraToClip);
	}
	tglView.SetFog(fogColor,fogStart,fogFull);

	active = true;

	terrainLightCalc = false;

	// [CAMERA_MOTION v1] recon: measure stationary-camera fraction.
	// Gates Lifecycle Option C (camera-motion-gated fail-open).
	// Stationary thresholds:
	//   rotation < 0.5 deg/frame: smaller than typical operator-pan rate
	//     (RTS pans typically 30-60 deg/sec at 60fps = 0.5-1.0 deg/frame);
	//     0.5 deg/frame is the floor below which the operator is not
	//     actively rotating.
	//   position < 1.0 unit/frame: world units (terrain.x/y in MC2 worldspace);
	//     a stationary camera still drifts sub-pixel from float math but
	//     should not exceed ~1 unit between frames at any sane FPS.
	// Both thresholds must hold for "stationary" â€” translation OR rotation
	// counts as motion, since either invalidates lagged-visibility data.
	{
		ZoneScopedN("Camera.MotionTracking");
		static bool   s_motionInit       = false;
		static float  s_prevRotation     = 0.0f;
		static glm::vec3 s_prevPosition;  // default-ctor zeroed; actual prev seeded on first call
		static uint32_t s_motionFrames     = 0;
		static uint32_t s_motionStationary = 0;
		static uint32_t s_motionMoving     = 0;
		constexpr float STATIONARY_ROT_DEG = 0.5f;
		constexpr float STATIONARY_POS_UNI = 1.0f;
		constexpr uint32_t SUMMARY_PERIOD  = 600u;

		if (!s_motionInit) {
			s_prevRotation = cameraRotation;
			s_prevPosition = position;
			s_motionInit = true;
		} else {
			float dRot = cameraRotation - s_prevRotation;
			dRot = std::fabs(dRot);
			// wrap-around: rotation is in degrees, fold to [0,180]
			if (dRot > 180.0f) dRot = 360.0f - dRot;

			float dx = position.x - s_prevPosition.x;
			float dy = position.y - s_prevPosition.y;
			float dz = position.z - s_prevPosition.z;
			float dPos = sqrtf(dx*dx + dy*dy + dz*dz);

			s_motionFrames++;
			if (dRot < STATIONARY_ROT_DEG && dPos < STATIONARY_POS_UNI) {
				s_motionStationary++;
			} else {
				s_motionMoving++;
			}

			s_prevRotation = cameraRotation;
			s_prevPosition = position;

			// [CAMERA-SMOOTHING-1] evidence tooling (both default OFF):
			//   MC2_CAM_SMOOTH_TEST=1  â€” synthetic scroll driver: frames
			//     120..240 feed moveRight at 2000 wu/s + one rotateLeft
			//     11.25-deg snap at frame 150 (mimics the keyboard/edge
			//     scroll + rotation input path), then input stops. Proves
			//     ramp-in/tail-out in the delta log without a human.
			//   MC2_TEST_SPAWN=1 + MC2_DIAG_TAGS=CAMERA_ELEVATION appends
			//     every frame's pose and elevation goals to cam_smooth_trace.csv.
			{
				static int s_camTest = -1;
				static int s_camTrace = -1;
				static unsigned s_camTestFrame = 0;
				static FILE* s_camTraceFile = nullptr;
				if (s_camTest < 0) {
					s_camTest = mc2::gate::get<"MC2_CAM_SMOOTH_TEST">();
					s_camTrace = mc2::gate::get<"MC2_TEST_SPAWN">() && mc2_diag::tagRequested("CAMERA_ELEVATION");
				}
				if (s_camTest) {
					s_camTestFrame++;
					if (s_camTestFrame >= 120 && s_camTestFrame <= 240)
						moveRight(2000.0f * frameLength);
					if (s_camTestFrame == 150)
						rotateLeft(11.25f);
					// Held-rotate burst at the LEGACY quantizer cadence
					// (11.25-deg snap every 3rd frame ~= the missiongui
					// accumulator firing at 60-80fps). knob=0 -> raw snaps
					// in the trace; knob>0 -> exponential shaping.
					if (s_camTestFrame >= 300 && s_camTestFrame <= 420 &&
						(s_camTestFrame % 3) == 0)
						rotateLeft(11.25f);
				}
				if (s_camTrace) {
					if (!s_camTraceFile)
					{
						s_camTraceFile = fopen("cam_smooth_trace.csv", "w");
						if (s_camTraceFile)
							mc2::fmt::print(s_camTraceFile, "frame,dt_ms,px,py,rot,alt,dpos,drot,pitch,pitch_goal,alt_goal\n");
					}
					if (s_camTraceFile)
					{
						mc2::fmt::print(s_camTraceFile, "{:d},{:.3f},{:.2f},{:.2f},{:.3f},{:.4f},{:.4f},{:.4f},{:.6f},{:.6f},{:.4f}\n", mc2::fmt::as_u32(s_motionFrames), frameLength * 1000.0f, position.x, position.y, cameraRotation, cameraAltitude, dPos, dRot, projectionAngle, pitchGoal, cameraAltitudeDesired);
						fflush(s_camTraceFile);
					}
				}
			}

			if ((s_motionFrames % SUMMARY_PERIOD) == 0u) {
				MC2_DIAGF("[CAMERA_MOTION v1] event=summary frames=%u stationary=%u moving=%u "
				       "stationary_pct=%.1f threshold_deg=%.2f threshold_pos=%.2f\n",
				       s_motionFrames, s_motionStationary, s_motionMoving,
				       (s_motionFrames > 0u)
				           ? (100.0f * static_cast<float>(s_motionStationary) / static_cast<float>(s_motionFrames))
				           : 0.0f,
				       STATIONARY_ROT_DEG, STATIONARY_POS_UNI);
				fflush(stdout);
			}
		}
	}

	return NO_ERR;
}

float currentScaleFactor = 0.0;
bool lastZoom = FALSE;

//---------------------------------------------------------------------------
void Camera::render (void)
{
	//------------------------------------------------------
	// At present, these actually draw.  Later they will 
	// add elements to the draw list and sort and draw.
	// The later time has arrived.  We begin sorting immediately.
	// NO LONGER NEED TO SORT!
	// ZBuffer time has arrived.  Share and Enjoy!
	// Everything SIMPLY draws at the execution point into the zBuffer
	// at the correct depth.  Miracles occur at that point!
	// Big code change but it removes a WHOLE bunch of code and memory!
	
	//--------------------------------------------------------
	// Get new viewport values to scale stuff.  No longer uses
	// VFX stuff for this.  ALL GOS NOW!
	gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);
	//--------------------------------------------------------
	// Get new viewport values to scale stuff.  No longer uses
	// VFX stuff for this.  ALL GOS NOW!
	screenResolution.x = viewMulX;
	screenResolution.y = viewMulY;
	calculateProjectionConstants();

	tglView.SetViewport(viewMulX,viewMulY,viewAddX,viewAddY);

	userInput->setViewport(viewMulX,viewMulY,viewAddX,viewAddY);

	globalScaleFactor = getScaleFactor();
	globalScaleFactor *= viewMulX / Environment.screenWidth;		//Scale Mechs to ScreenRES
	
	//-----------------------------------------------
	// Set Ambient for this pass of rendering	
	DWORD lightRGB = (ambientRed<<16)+(ambientGreen<<8)+ambientBlue;
		
	eye->setLightColor(1,lightRGB);
	eye->setLightIntensity(1,1.0);

	if (active && turn > 1)
	{
		land->render();								//render the Terrain

		Footprints::render();

		//Only the GameCamera knows about this.  Heidi, override this function in EditorCamera
		//and have your objectManager draw.
//		ObjectManager->render(true, true, true);	//render all other objects
	}

	//-----------------------------------------------------
}
