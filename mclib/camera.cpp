//---------------------------------------------------------------------------
//
// Camera.cpp -- File contains the camera class code
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include <algorithm>
#include "camera_internal.h"
#include "../GameOS/gameos/gos_cull_freeze.h"
#include "Gates/gate.h"
#include "../GameOS/gameos/utils/mc2log.h"   // LOCAL-LIGHTS-4: [MISSION_LIGHT v1]
#include "mission_light_profile.h"
#include "authored_sun.h"
#include "../GameOS/gameos/visual_tuning_profile.h"
#include "mc2fmt.h"


inline float agsqrt( float _a, float _b )
{
	return sqrt(_a*_a + _b*_b);
}

//---------------------------------------------------------------------------
// Static Globals

// Sun colour/ambient as authored, captured before the per-mission sun keys replace them (Camera::init).
static mc2::authored_sun::AuthoredSun s_authoredSun;

char WindowTitle[1024];		// Global window title (GetWindowText is VERY slow)

// Scroll-velocity momentum for adaptive zoom speed.
// Bumped by ZoomIn/ZoomOut each call, decayed every frame by Camera::update().
float s_scrollMomentum = 0.0f;

// [CAMERA-SMOOTHING-1] frame-rate-independent gameplay-camera smoothing.
// MC2_CAMERA_SMOOTH=<ms> — time constant of the exponential approach toward
// the input-accumulated goal pose. 0 = off (legacy verbatim: hard per-frame
// position steps, 11.25-degree rotation snaps, 20/s zoom lerp). Default 100ms:
// tight, not floaty. Clamped to [0, 2000].
float camSmoothTau (void)
{
	static const float s_tau = mc2::gate::get<"MC2_CAMERA_SMOOTH">() * 0.001f;
	return s_tau;
}

// Exponential-approach blend factor for a step of dt seconds: identical
// convergence fraction per unit time regardless of frame rate.
float camSmoothAlpha (float dt)
{
	float tau = camSmoothTau();
	if (tau <= 0.0f)
		return 1.0f;
	float a = 1.0f - expf(-dt / tau);
	return (a > 1.0f) ? 1.0f : a;
}

long topCtrlUpd = 0;

#ifdef _DEBUG
extern long currentHotSpot;
#endif

// RNA - moved this out of the #ifdef PROFILE so it's available in release
extern long displayProfileData;

long leaveSwoopyOff = 0;

bool drawCameraCircle = FALSE;
// WATER-ASPECT-CORRECT-2: true when the most recent perspective-projection
// build (below, in Camera::setOrthogonal's frustum block, despite the name
// it builds the perspective frustum too) used the REAL drawable aspect
// (UI-ASPECT-ANCHOR-1, default) rather than the legacy 4:3-locked frustum
// (MC2_CAMERA_ASPECT_NATIVE=0, or a mech-preview FBO pass). Consumed by
// gameos_graphics.cpp to keep the water MDI shader's dFdx(nz) aspect
// compensation in sync with whichever frustum this frame actually used,
// instead of assuming the frustum is always 4:3-locked.
bool g_cameraProjAspectNative = true;
MemoryPtr pauseShape = nullptr;
MemoryPtr askedShape = nullptr;
bool MaxObjectsDrawn = FALSE;

float elevationAdjustFactor = 50.0;

float Camera::MIN_PERSPECTIVE =			-20.0f;  // allow looking up to 20deg above horizontal
float Camera::MIN_ORTHO		  =			18.0f;
float Camera::MAX_PERSPECTIVE =			88.0f;
float Camera::MAX_ORTHO		  =			88.0f;

float zoomMax = 2.0;
float zoomMin = 0.1f;
float FOVMax = 75.0f;
float FOVMin = 20.0f;
float Camera::AltitudeMinimum = 10.0f;
float Camera::AltitudeMaximumLo = 6000.0f;
float Camera::AltitudeMaximumHi = 6400.0f;
float Camera::AltitudeDefault = 1200.0f;
float Camera::AltitudeTight = 800.0f;

float Camera::globalScaleFactor = 1.0;

float Camera::MaxClipDistance	= 50000.0f;
float Camera::MinHazeDistance	= 50000.0f;
float Camera::HazeFactor	 	= 0.0f;
float Camera::DistanceFactor	= mc2FogInvSpan(MinHazeDistance, MaxClipDistance);
float Camera::NearPlaneDistance = -400.0f;
float Camera::FarPlaneDistance  = 61555.0f;

float Camera::MinNearPlane		= -100.0f;
float Camera::MaxNearPlane		= -400.0f;

float Camera::MinFarPlane		= 61500.0f;
float Camera::MaxFarPlane		= 61555.0f;

float Camera::verticalSphereClipConstant = 0.0f;
float Camera::horizontalSphereClipConstant = 0.0f;

bool Camera::inMovieMode = false;
bool Camera::forceMovieEnd = false;
float Camera::MaxLetterBoxTime = 5.0f;		//Takes this many seconds to get into letter box mode
float Camera::MaxLetterBoxPos = 0.15f;		//This is the percentage of the screen covered by EACH black bar!

float Camera::cameraTilt[MAX_VIEWS] = 
	{
		NORM_PERSPECTIVE,
		NORM_PERSPECTIVE,
		NORM_PERSPECTIVE,
		NORM_PERSPECTIVE,
	};
	
float Camera::cameraZoom[MAX_VIEWS] = 
	{
		Camera::AltitudeDefault,
		Camera::AltitudeDefault,
		Camera::AltitudeDefault,
		Camera::AltitudeDefault
	};

frameOfRef		Camera::cameraFrame;

//---------------------------------------------------------------------------
// LOCAL-LIGHTS-4: per-mission night lighting. On night and evening missions the
// authored sun/moon and ambient were bright enough that the base read as lit grey
// and the local lamps (building searchlights, generators, mech headlights) could not
// carry the scene. The mission's sun and ambient are scaled down by how night-like
// its sun is (lightAltitude between NIGHT_START_PITCH and NIGHT_LIGHT_PITCH; exactly
// 1.0 on day missions, so day maps are byte-unchanged). A mission may name its own
// target; the rest of the night missions use the default.
// The name is the loading mission's authored file name (Mission::missionFileName, set
// before the camera is created, and restored from a save's MissionName), so campaign
// loads and saves ("mc2_10_saved_*") resolve the profile like a command-line launch.
namespace {
void applyMissionLightProfile(Camera& c, bool alreadyApplied)
{
    mc2_mission_light::apply(c, alreadyApplied);
    if (c.missionLightName.empty()) return;
    const float night = mc2_mission_light::nightFromSun(c.lightPitch);
    if (night <= 0.0f) return;
    const auto& p = mc2_mission_light::missionLightProfile(c.missionLightName);
    const float ls = alreadyApplied ? 1.0f : 1.0f + (p.light - 1.0f) * night;
    const float as = alreadyApplied ? 1.0f : 1.0f + (p.ambient - 1.0f) * night;
	if (mc2::gate::get<"MC2_DIAG_VERBOSE">()) {
		MC2_EVENT(Render, "MISSION_LIGHT", 1,
			"mission={} night={:.2f} light_scale={:.2f} ambient_scale={:.2f} ground_scale={:.2f} from_save={} light=({},{},{}) ambient=({},{},{})",
			c.missionLightName.c_str(), night, ls, as, 1.0f + (p.ground - 1.0f) * night, alreadyApplied ? 1 : 0,
			static_cast<unsigned>(c.lightRed), static_cast<unsigned>(c.lightGreen), static_cast<unsigned>(c.lightBlue),
			static_cast<unsigned>(c.ambientRed), static_cast<unsigned>(c.ambientGreen), static_cast<unsigned>(c.ambientBlue));
	}
}
} // namespace

void mc2SetMissionLightProfileName(std::string_view missionFileName)
{
	if (eye) eye->missionLightName = missionFileName;
}

// The mission's scale of the prelit ground (terrain / overlay / vegetation UBO sceneLightScale).
float mc2SceneLightScale()
{
	return eye ? mc2_mission_light::sceneScale(*eye) : 1.0f;
}

// How dark the mission's night is: 0 for day and evening suns (night <= 0.35, e.g. mc2_14's low
// orange sun keeps its soft shadows), easing to 1 at full night. Drives the sun-shadow fade and
// the water's night sky (GameOS receivers declare it extern, like mc2SceneLightScale).
float mc2NightLightFactor()
{
	return eye && !eye->missionLightName.empty() ? mc2_mission_light::darkness(eye->lightPitch) : 0.0f;
}

// Night fade of the mover (mech/prop) sun shadow from the mission's current moon and ambient strength
// (mission_light_profile.h moverNightFade); 0 by day. Also reports the inputs for the diag line.
float mc2MoverShadowNightFade(float* darknessOut, float* moonLumOut, float* ambientLumOut)
{
	const float d = mc2NightLightFactor();
	const float moon = eye ? 0.299f*eye->lightRed + 0.587f*eye->lightGreen + 0.114f*eye->lightBlue : 0.0f;
	const float amb = eye ? 0.299f*eye->ambientRed + 0.587f*eye->ambientGreen + 0.114f*eye->ambientBlue : 0.0f;
	if (darknessOut) *darknessOut = d;
	if (moonLumOut) *moonLumOut = moon;
	if (ambientLumOut) *ambientLumOut = amb;
	return mc2_mission_light::moverNightFade(d, moon, amb);
}

//---------------------------------------------------------------------------
// Camera Class
//---------------------------------------------------------------------------

	
//---------------------------------------------------------------------------
long Camera::init (FitIniFilePtr cameraFile )
{
	long result = cameraFile->seekBlock("Cameras");
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdFloat("ProjectionAngle",projectionAngle);
	gosASSERT(result == NO_ERR);

	result = cameraFile->readIdFloat("PositionX",position.x);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdFloat("PositionY",position.y);
	gosASSERT(result == NO_ERR);
		
	result = cameraFile->readIdFloat("PositionZ",position.z);
	gosASSERT(result == NO_ERR);
	cameraShiftZ = position.z;
	
	result = cameraFile->readIdBoolean("Ready",ready);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdUChar("LightRed",lightRed);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdUChar("LightGreen",lightGreen);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdUChar("LightBlue",lightBlue);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdUChar("AmbientRed",ambientRed);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdUChar("AmbientGreen",ambientGreen);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdUChar("AmbientBlue",ambientBlue);
	gosASSERT(result == NO_ERR);

	unsigned char tmpUChar;
	result = cameraFile->readIdUChar("TerrainShadowColorEnabled",tmpUChar);
	if ((result != NO_ERR) || (0 == tmpUChar)) {
		terrainShadowColorEnabled = false;
	}
	else
	{
		terrainShadowColorEnabled = true;
	}
	
	result = cameraFile->readIdUChar("TerrainShadowRed",terrainShadowRed);
	if (result != NO_ERR) { terrainShadowRed = ambientRed; }
	
	result = cameraFile->readIdUChar("TerrainShadowGreen",terrainShadowGreen);
	if (result != NO_ERR) { terrainShadowGreen = ambientGreen; }
	
	result = cameraFile->readIdUChar("TerrainShadowBlue",terrainShadowBlue);
	if (result != NO_ERR) { terrainShadowBlue = ambientBlue; }

	result = cameraFile->readIdUChar("SeenRed",seenRed);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdUChar("SeenGreen",seenGreen);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdUChar("SeenBlue",seenBlue);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdUChar("BaseRed",baseRed);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdUChar("BaseGreen",baseGreen);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdUChar("BaseBlue",baseBlue);
	gosASSERT(result == NO_ERR);
			
	result = cameraFile->readIdFloat("LightDirYaw",lightYaw);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdFloat("LightDirPitch",lightPitch);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdFloat("DayToNightTime",day2NightTransitionTime);
	if (result != NO_ERR)
	{
		day2NightTransitionTime = -1.0f;
		dayLightTime = 0.0f;
		gosASSERT((lightYaw >= -360.0) && (lightYaw <= 360.0));
		//gosASSERT((lightPitch >= 0.0) && (lightPitch <= 90.0));
	
		lightDirection.x = cos((lightYaw + 45.0f) * DEGREES_TO_RADS);
		lightDirection.y = sin((lightYaw + 45.0f) * DEGREES_TO_RADS);
		lightDirection.z = sin(lightPitch * DEGREES_TO_RADS);

		// initialize these so we can edit 'em
		dayAmbientRed = ambientRed;
		dayAmbientBlue = ambientBlue;
		dayAmbientGreen = ambientGreen;
		
		dayLightRed = lightRed;
		dayLightGreen = lightGreen;
		dayLightBlue = lightBlue;
		
		sunsetLightRed = 205/*arbitrary default*/;
		sunsetLightGreen = 84/*arbitrary default*/;
		sunsetLightBlue = 31/*arbitrary default*/;
		
		nightAmbientRed = 32/*arbitrary default*/;
		nightAmbientBlue = 32/*arbitrary default*/;
		nightAmbientGreen = 32/*arbitrary default*/;;
		
		nightLightRed = 64/*arbitrary default*/;
		nightLightGreen = 64/*arbitrary default*/;
		nightLightBlue = 64/*arbitrary default*/;

		dayLightPitch = lightPitch;
	}
	else		//They want the time of day to change!
	{
		result = cameraFile->readIdFloat("DayLightPitch",dayLightPitch);
		gosASSERT(result == NO_ERR);
		
		result = cameraFile->readIdUChar("DayLightRed",dayLightRed);
		gosASSERT(result == NO_ERR);
		result = cameraFile->readIdUChar("DayLightGreen",dayLightGreen);
		gosASSERT(result == NO_ERR);
		result = cameraFile->readIdUChar("DayLightBlue",dayLightBlue);
		gosASSERT(result == NO_ERR);
	
		result = cameraFile->readIdUChar("DayAmbientRed",dayAmbientRed);
		gosASSERT(result == NO_ERR);
		result = cameraFile->readIdUChar("DayAmbientGreen",dayAmbientGreen);
		gosASSERT(result == NO_ERR);
		result = cameraFile->readIdUChar("DayAmbientBlue",dayAmbientBlue);
		gosASSERT(result == NO_ERR);
		
		result = cameraFile->readIdUChar("SunsetLightRed",sunsetLightRed);
		if (NO_ERR != result)
		{
			sunsetLightRed = 254/*arbitrary default*/;
			sunsetLightGreen = 68/*arbitrary default*/;
			sunsetLightBlue = 5/*arbitrary default*/;
		}
		else
		{
			result = cameraFile->readIdUChar("SunsetLightGreen",sunsetLightGreen);
			gosASSERT(result == NO_ERR);
			result = cameraFile->readIdUChar("SunsetLightBlue",sunsetLightBlue);
			gosASSERT(result == NO_ERR);
		}
		
		result = cameraFile->readIdUChar("NightLightRed",nightLightRed);
		gosASSERT(result == NO_ERR);
		result = cameraFile->readIdUChar("NightLightGreen",nightLightGreen);
		gosASSERT(result == NO_ERR);
		result = cameraFile->readIdUChar("NightLightBlue",nightLightBlue);
		gosASSERT(result == NO_ERR);

		result = cameraFile->readIdUChar("NightAmbientRed",nightAmbientRed);
		gosASSERT(result == NO_ERR);
		result = cameraFile->readIdUChar("NightAmbientGreen",nightAmbientGreen);
		gosASSERT(result == NO_ERR);
		result = cameraFile->readIdUChar("NightAmbientBlue",nightAmbientBlue);
		gosASSERT(result == NO_ERR);
		
		dayLightTime = 0.0f;
		lightPitch = dayLightPitch;
		ambientRed = dayAmbientRed;
		ambientBlue = dayAmbientBlue;
		ambientGreen = dayAmbientGreen;
		lightRed = dayLightRed;
		lightBlue = dayLightBlue;
		lightGreen = dayLightGreen;
		
		gosASSERT((lightYaw >= -360.0) && (lightYaw <= 360.0));
		//gosASSERT((lightPitch >= 0.0) && (lightPitch <= 90.0));
	
		lightDirection.x = cos((lightYaw + 45.0f) * DEGREES_TO_RADS);
		lightDirection.y = sin((lightYaw + 45.0f) * DEGREES_TO_RADS);
		lightDirection.z = sin(lightPitch * DEGREES_TO_RADS);
	}

	{
		// A save's camera block already carries the scaled colours (Camera::save marks it).
		bool profileApplied = false;
		const bool fromSave = cameraFile->readIdBoolean("NightProfileApplied", profileApplied) == NO_ERR;
		if (!fromSave)
			profileApplied = false;
		applyMissionLightProfile(*this, profileApplied);
		// The day/night branch above reset the live colours to the day set; a save holds the colours in force when it was written.
		if (fromSave)
		{
			cameraFile->readIdUChar("LightRed", lightRed);
			cameraFile->readIdUChar("LightGreen", lightGreen);
			cameraFile->readIdUChar("LightBlue", lightBlue);
			cameraFile->readIdUChar("AmbientRed", ambientRed);
			cameraFile->readIdUChar("AmbientGreen", ambientGreen);
			cameraFile->readIdUChar("AmbientBlue", ambientBlue);
		}
		// Per-mission sun (visual_tuning.json sunPitch/sunYaw/sunColor*): day suns only, absolute values so a save re-applies the same.
		s_authoredSun = {};
		if (!missionLightName.empty() && mc2_mission_light::nightFromSun(dayLightPitch) == 0.0f)
		{
			// A keyed save already holds the keyed colours; it also carries the authored ones (Camera::save).
			bool savedHas = false;
			std::array<unsigned char, 3> savedSun{}, savedAmb{};
			if (fromSave && cameraFile->readIdBoolean("AuthoredSunValid", savedHas) == NO_ERR && savedHas)
			{
				cameraFile->readIdUChar("AuthoredLightRed", savedSun[0]); cameraFile->readIdUChar("AuthoredLightGreen", savedSun[1]);
				cameraFile->readIdUChar("AuthoredLightBlue", savedSun[2]); cameraFile->readIdUChar("AuthoredAmbientRed", savedAmb[0]);
				cameraFile->readIdUChar("AuthoredAmbientGreen", savedAmb[1]); cameraFile->readIdUChar("AuthoredAmbientBlue", savedAmb[2]);
			}
			s_authoredSun = mc2::authored_sun::resolve({lightRed, lightGreen, lightBlue}, {ambientRed, ambientGreen, ambientBlue}, savedHas, savedSun, savedAmb);
			s_authoredSun.pitch = lightPitch;   // the pitch before the keys; a keyed save carries it (a save without it: no pitch gain)
			if (savedHas) cameraFile->readIdFloat("AuthoredLightPitch", s_authoredSun.pitch);
			s_authoredSun.yaw = lightYaw; s_authoredSun.hasYaw = true;   // the yaw before the keys; a keyed save without it falls back to the live yaw (pitch-only rotation)
			if (savedHas) s_authoredSun.hasYaw = cameraFile->readIdFloat("AuthoredLightYaw", s_authoredSun.yaw) == NO_ERR;
			if (const auto v = visualTuning_sunValue("sunPitch")) lightPitch = dayLightPitch = std::max(*v, 26.0f);
			if (const auto v = visualTuning_sunValue("sunYaw")) lightYaw = *v;
			lightDirection.x = cos((lightYaw + 45.0f) * DEGREES_TO_RADS);
			lightDirection.y = sin((lightYaw + 45.0f) * DEGREES_TO_RADS);
			lightDirection.z = sin(lightPitch * DEGREES_TO_RADS);
			const auto r = visualTuning_sunValue("sunColorR"), g = visualTuning_sunValue("sunColorG"), b = visualTuning_sunValue("sunColorB");
			if (r && g && b)
			{
				lightRed = dayLightRed = static_cast<unsigned char>(*r + 0.5f);
				lightGreen = dayLightGreen = static_cast<unsigned char>(*g + 0.5f);
				lightBlue = dayLightBlue = static_cast<unsigned char>(*b + 0.5f);
			}
			// sunAmbientScale: ambient* is what every shadowed surface (terrain, props, mechs, grass) is lit with.
			// A save's ambient bytes are already scaled (Camera::save marks it): scaling again would compound on every load.
			bool scaledInSave = false;
			if (!fromSave || cameraFile->readIdBoolean("SunAmbientScaled", scaledInSave) != NO_ERR)
				scaledInSave = false;
			if (scaledInSave)
				sunAmbientScaled = true;
			if (const auto k = (scaledInSave ? std::optional<float>() : visualTuning_sunValue("sunAmbientScale")))
			{
				sunAmbientScaled = true;
				const auto sc = [k](unsigned char& v) { v = static_cast<unsigned char>(std::min(255.0f, v * *k + 0.5f)); };
				sc(ambientRed); sc(ambientGreen); sc(ambientBlue); sc(dayAmbientRed); sc(dayAmbientGreen); sc(dayAmbientBlue);
			}
			if (mc2::gate::get<"MC2_DIAG_VERBOSE">())
				MC2_EVENT(Render, "SUN_LIGHT", 1, "mission={} from_save={} dir=({:.3f},{:.3f},{:.3f}) sun=({},{},{}) ambient=({},{},{}) ambient_scaled={}",
					missionLightName.c_str(), fromSave ? 1 : 0, lightDirection.x, lightDirection.y, lightDirection.z,
					static_cast<unsigned>(lightRed), static_cast<unsigned>(lightGreen), static_cast<unsigned>(lightBlue),
					static_cast<unsigned>(ambientRed), static_cast<unsigned>(ambientGreen), static_cast<unsigned>(ambientBlue), sunAmbientScaled ? 1 : 0);
		}
	}

	result = cameraFile->readIdFloat("NewScale",newScaleFactor);
	gosASSERT(result == NO_ERR);

	float startRotation = 0.0;
	result = cameraFile->readIdFloat("StartRotation",startRotation);
	gosASSERT(result == NO_ERR);
	
	setCameraRotation(startRotation,startRotation);

	result = cameraFile->readIdFloatArray("LODScales",zoomLevelLODScale,MAX_LODS);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdFloat("ElevationAdjustFactor",elevationAdjustFactor);
	gosASSERT(result == NO_ERR);
	
	result = cameraFile->readIdFloat("ZoomMax",zoomMax);
	gosASSERT(result == NO_ERR);

	result = cameraFile->readIdFloat("ZoomMin",zoomMin);
	gosASSERT(result == NO_ERR);

	FOVMax = 90.0f;
	result = cameraFile->readIdFloat("FOVMax",FOVMax);
//	gosASSERT(result == NO_ERR);

	FOVMin = 20.0f;
	result = cameraFile->readIdFloat("FOVMin",FOVMin);
//	gosASSERT(result == NO_ERR);

	// Override config limits — full zoom out / no distance culling
	zoomMax = 2.0f;
	zoomMin = 0.1f;
	AltitudeMaximumLo = 6000.0f;
	AltitudeMaximumHi = 6400.0f;
	MaxClipDistance = 50000.0f;
	MinHazeDistance = 50000.0f;
	DistanceFactor = mc2FogInvSpan(MinHazeDistance, MaxClipDistance);

 	setClass(BASE_CAMERA);
	
	//Replace with TGL
	if ( !worldLights ) // only do the alloc once
	{
		worldLights = static_cast<TG_LightPtr *>(std::malloc(sizeof(TG_LightPtr) * MAX_LIGHTS_IN_WORLD));
		memset(worldLights,0,sizeof(TG_LightPtr) * MAX_LIGHTS_IN_WORLD);
		numLights = 2;

		//---------------------------------------------------------
		// First light is sun.
		worldLights[0] = static_cast<TG_LightPtr>(std::malloc(sizeof(TG_Light)));
		worldLights[0]->init(TG_LIGHT_INFINITE);
	}
	
	if ( !activeLights ) // only do the alloc once
	{
		activeLights = static_cast<TG_LightPtr *>(std::malloc(sizeof(TG_LightPtr) * MAX_LIGHTS_IN_WORLD));
		memset(activeLights,0,sizeof(TG_LightPtr) * MAX_LIGHTS_IN_WORLD);
		numActiveLights = 0;
	}
	
	if ( !terrainLights ) // only do the alloc once
	{
		terrainLights = static_cast<TG_LightPtr *>(std::malloc(sizeof(TG_LightPtr) * MAX_LIGHTS_IN_WORLD));
		memset(terrainLights,0,sizeof(TG_LightPtr) * MAX_LIGHTS_IN_WORLD);
		numTerrainLights = 0;
	}
	
  	glm::mat4 lightToWorldMatrix;
	
	mc2::math::buildTranslation(lightToWorldMatrix, glm::vec3(0.0,0.0,0.0));
	mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));

	worldLights[0]->SetLightToWorld(&lightToWorldMatrix);

	//---------------------------------------------------------
	// Second light is ambient.
	worldLights[1] = static_cast<TG_LightPtr>(std::malloc(sizeof(TG_Light)));
	worldLights[1]->init(TG_LIGHT_AMBIENT);

	//---------------------
	// Read in Fog Values.
	result = cameraFile->readIdFloat("FogStart",fogStart);
	gosASSERT(result == NO_ERR);

	result = cameraFile->readIdFloat("FogFull",fogFull);
	gosASSERT(result == NO_ERR);

    // sebi
    unsigned long tmp;
	result = cameraFile->readIdULong("FogColor", tmp);
    fogColor = static_cast<DWORD>(tmp);
	dayFogColor = fogColor;
	gosASSERT(result == NO_ERR);

	result = cameraFile->readIdFloat("FogTransparency",fogTransparency);

	long userMin, userMax, baseTerrain;
	cameraFile->readIdLong( "UserMin", userMin );
	cameraFile->readIdLong( "UserMax", userMax );
	cameraFile->readIdLong( "BaseTerrain", baseTerrain );
	land->setUserSettings( userMin, userMax, baseTerrain );

	//-------------------------------------------
	// To get it working again.
	usePerspective = true;
	camera_fov = 40.0f;
	cosHalfFOV = cos(camera_fov * 0.7071f * DEGREES_TO_RADS);				//Cosine of half the FOV for view cone.
	
	float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
	float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);

	newScaleFactor = 1.0f - ((cameraAltitude - AltitudeMinimum) / testMax);

	terrainLightNight = false;
	terrainLightCalc = true;
	
//	if (ready)
//	{
//		activate();
//		update();
//	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------
void Camera::destroy (void)
{
    mc2_spotlight_diag::reset(this);
	if (worldLights)
	{
		std::free(worldLights[1]);
		worldLights[1] = nullptr;

		std::free(worldLights[0]);
		worldLights[0] = nullptr;

		std::free(worldLights);
		worldLights = nullptr;
	}
	
	if (activeLights)
	{
		std::free(activeLights);
		activeLights = nullptr;
	}
	
	if (terrainLights)
	{
		std::free(terrainLights);
		terrainLights = nullptr;
	}
}

// getClosestVertex: screen-click -> terrain vertex (row,col).
// Reinstated 2026-05-24 for the EditRel Mission Editor (editor/TerrainBrush.h:56
// is the sole caller). This is a NEW modern implementation — NOT the old
// pr-36 body that read stale topVertex->px/py. Thin adapter over:
//   (1) Camera::inverseProject  (screen -> fresh world point, no stale read)
//   (2) Terrain::worldToTile    (world -> vertex row/col)
// See engine-standalone camera.cpp for full design rationale.
void Camera::getClosestVertex (Stuff::Vector2DOf<long>& screenPos,
                               long& row, long& col)
{
	glm::vec3 worldPoint;
	worldPoint.x = worldPoint.y = worldPoint.z = 0.0f;

	// Modern picking authority: screen -> fresh world point. Returns 0 when
	// the camera is not yet initialized (turn < 4); worldPoint stays at origin.
	inverseProject(screenPos, worldPoint);

	// Canonical world -> terrain vertex (row,col) map.
	int tileR = 0, tileC = 0;
	land->worldToTile(worldPoint, tileR, tileC);

	// Clamp into the valid vertex grid. The editor guard
	// (tileR < realVerticesMapSide && tileR > -1) treats out-of-range
	// as "no paint", so clamping here keeps row/col well-formed.
	const int maxVertex = static_cast<int>(Terrain::realVerticesMapSide) - 1;
	tileR = std::clamp(tileR, 0, maxVertex);
	tileC = std::clamp(tileC, 0, maxVertex);

	row = static_cast<long>(tileR);
	col = static_cast<long>(tileC);
}

//---------------------------------------------------------------------------
// VPL-retirement Step 3 (3a): shared CPU camera-frustum x quad-AABB primitive.
// Members of Camera; pure CPU, no GL, no GPU readback (all inputs are CPU
// camera matrices + map-stable vertex coords).
//
// UNIFIED PROJECTION (2026-06-08): planes are extracted by Gribb-Hartmann
// directly from worldToClipGL() -- the SAME world->GL-NDC matrix the GPU
// rasterizes terrain/props/mechs with (kAxisSwapMC2toGL * worldToCam *
// cameraToClip * kPixelHomogToGLNDC). This is the single source of truth for
// the view frustum: cull and render share one matrix and can no longer drift.
//
// History / why this was wrong before: the previous body extracted planes from
// the raw `worldToClip` member (= worldToCam * cameraToClip) and hand-folded
// ONLY the MC2->GL axis swap. But `worldToClip` is D3D-pixel-homogeneous
// (clip.xy/clip.w in [0,1], Y-down, clip.w<0 for in-front -- see
// makePixelHomogToGLNDC). It is NOT GL clip space. The kPixelHomogToGLNDC stage
// (the -2x/+w X map, Y-flip, and w-negate) was dropped, so the extracted
// left/right planes were MIRRORED vs the rendered image -> terrain LOD chunk
// cull rejected on-screen blocks on one screen half (orientation-dependent
// "zero-command" streaks). Extracting from worldToClipGL() carries every stage
// the GPU applies, so the planes are correct by construction. This is the same
// D3D<->GL split-brain that bit shadow (a365e6ad) and static props (09707cd8).
//
// Convention: row-vector multiply clip_col = sum_row world_row * M(row,col)
// with world = (wx, wy, wz, 1) (same as projectModernClipGL()). GL clip-Z is
// ZERO_TO_ONE (glClipControl in gameosmain.cpp), so the clip volume is
//   -w <= x <= w,  -w <= y <= w,  0 <= z <= w   (matches clipSpaceFrustumAdmitGL)
// giving the six half-spaces:
//   left=  x+w>=0   right= w-x>=0
//   bottom=y+w>=0   top=   w-y>=0
//   near=  z  >=0   far=   w-z>=0   (near is BARE z under ZERO_TO_ONE, not z+w)
// Each plane [a b c d] tests RAW world AABBs as a*wx + b*wy + c*wz + d >= 0,
// where a=M(0,*), b=M(1,*), c=M(2,*), d=M(3,*) combine column 3 (w) with the
// per-plane axis column. Planes are unnormalized: quadAabbInFrustum tests sign
// only, so scale is irrelevant.
void Camera::extractFrustumPlanes (float planes[6][4]) const
{
	const glm::mat4 M = worldToClipGL();

	// Per plane: axis clip column, sign on w (col 3), sign on the axis column.
	// plane = wSign * M(:,3) + aSign * M(:,axisCol).
	struct PlaneSel { int axisCol; float wSign; float aSign; };
	static const PlaneSel sel[6] = {
		{0, +1.0f, +1.0f},   // left   :  x + w
		{0, +1.0f, -1.0f},   // right  :  w - x
		{1, +1.0f, +1.0f},   // bottom :  y + w
		{1, +1.0f, -1.0f},   // top    :  w - y
		{2,  0.0f, +1.0f},   // near   :  z       (ZERO_TO_ONE: z >= 0)
		{2, +1.0f, -1.0f},   // far    :  w - z
	};

	for (int p = 0; p < 6; p++)
	{
		const int   c  = sel[p].axisCol;
		const float ws = sel[p].wSign;
		const float as = sel[p].aSign;
		planes[p][0] = ws * M[3][0] + as * M[c][0];   // world X coef
		planes[p][1] = ws * M[3][1] + as * M[c][1];   // world Y coef
		planes[p][2] = ws * M[3][2] + as * M[c][2];   // world Z coef
		planes[p][3] = ws * M[3][3] + as * M[c][3];   // constant
	}
}

//---------------------------------------------------------------------------
// Conservative p-vertex AABB-vs-frustum test. For each of 6 world-space planes
// pick the AABB corner most in the plane's positive direction; if that corner
// is strictly behind the plane the AABB is fully outside -> reject. Never
// false-negative; may false-positive slightly outside (acceptable - picking
// refines with an exact forward-projection screen test downstream).
bool Camera::quadAabbInFrustum (const float planes[6][4],
                                const glm::vec3& mn,
                                const glm::vec3& mx) const
{
	// CAMERA-FRUSTUM-HARNESS-1: delegate the pure test to camera_frustum_math.h
	// (same arithmetic), adapting Stuff::Vector3D -> float[3] at the boundary.
	const float fmn[3] = { mn.x, mn.y, mn.z };
	const float fmx[3] = { mx.x, mx.y, mx.z };
	return camera_frustum_math::aabbInFrustum(planes, fmn, fmx);
}

//---------------------------------------------------------------------------
// [LOW-CAMERA-TERRAIN-CULL-1 / FIX-2] Near-plane-relaxed variant. Same as
// quadAabbInFrustum but skips plane index 4 (near). Used by the terrain
// LOD-chunk cull (gated by MC2_LOWCAM_TERRAIN_NEAR) so terrain within the
// near-plane distance of a low-pitched eye is not spuriously culled. At a high
// camera angle near terrain already passes the near plane, so dropping that
// term is a no-op there.
bool Camera::quadAabbInFrustumSkipNear (const float planes[6][4],
                                        const glm::vec3& mn,
                                        const glm::vec3& mx) const
{
	const float fmn[3] = { mn.x, mn.y, mn.z };
	const float fmx[3] = { mx.x, mx.y, mx.z };
	return camera_frustum_math::aabbInFrustumSkipPlane(planes, fmn, fmx, 4);
}

//---------------------------------------------------------------------------
// F6 T2: per-frame frustum-planes cache. Terrain::geometry calls
// cacheFrustumPlanes() once per frame (outside both slimReduce and
// setupTextures loops); both then read via getCachedFrustumPlanes().
// Single extractFrustumPlanes call per frame shared across all sites.
void Camera::cacheFrustumPlanes()
{
	extractFrustumPlanes(cachedFrustumPlanes_);
	mc2cullfreeze::cullPlanes(cachedFrustumPlanes_);   // debug: frozen culling camera
}

const float (*Camera::getCachedFrustumPlanes() const)[4]
{
	return cachedFrustumPlanes_;
}

// NIGHT-LIGHT-POOLS-1 optional knob MC2_NIGHT_AMBIENT_FLOOR=<0..64>: minimum
// per-channel output of the getLightRed/Green/Blue family. These getters feed
// the colormap night burn-in (terrtxm2.cpp burnInShadows) plus every flat-lit
// terrain/cement consumer, so a floor here lifts the darkest night terrain off
// pitch-black without touching authored day lighting (values already above the
// floor pass through untouched). Default UNSET = 0 = stock data-faithful
// (max(x,0) is the identity on unsigned channels). Boot-cached (Debug
// Instrumentation Rule); clamped to [0,64] so it can only be a subtle floor,
// never a wash-out.
static long mc2NightAmbientFloor (void)
{
	static const long s_floor = static_cast<long>(mc2::gate::get<"MC2_NIGHT_AMBIENT_FLOOR">());
	return s_floor;
}

//---------------------------------------------------------------------------
void Camera::getPropLightContract (int S[3], int A[3], int &F, float D[3])
{
	S[0] = lightRed; S[1] = lightGreen; S[2] = lightBlue;
	A[0] = ambientRed; A[1] = ambientGreen; A[2] = ambientBlue;
	F = static_cast<int>(mc2NightAmbientFloor());
	D[0] = D[1] = D[2] = 0.0f;
	if (worldLights[0])
	{
		const glm::vec3 d = mc2::math::localForwardInWorld(worldLights[0]->lightToWorld);
		D[0] = d.x; D[1] = d.y; D[2] = d.z;
	}
}

//---------------------------------------------------------------------------
unsigned char Camera::getLightRed (float intensity)
{
	const long floorV = mc2NightAmbientFloor();
	//ASM and Inline
	if (intensity > 0.0)
	{
		long fResult = float2long(lightRed * intensity + ambientRed);
		fResult = std::max(fResult, floorV);
		if (fResult < 0xff)
			return fResult;
		else
			return 0xff;
	}

	return (ambientRed < floorV) ? static_cast<unsigned char>(floorV) : ambientRed;
}

//---------------------------------------------------------------------------
unsigned char Camera::getLightGreen (float intensity)
{
	const long floorV = mc2NightAmbientFloor();
	//ASM and Inline
	if (intensity > 0.0)
	{
		long fResult = float2long(lightGreen * intensity + ambientGreen);
		fResult = std::max(fResult, floorV);
		if (fResult < 0xff)
			return fResult;
		else
			return 0xff;
	}

	return (ambientGreen < floorV) ? static_cast<unsigned char>(floorV) : ambientGreen;
}

//---------------------------------------------------------------------------
unsigned char Camera::getLightBlue (float intensity)
{
	const long floorV = mc2NightAmbientFloor();
	//ASM and Inline
	if (intensity > 0.0)
	{
		long fResult = float2long(lightBlue * intensity + ambientBlue);
		fResult = std::max(fResult, floorV);
		if (fResult < 0xff)
			return fResult;
		else
			return 0xff;
	}

	return (ambientBlue < floorV) ? static_cast<unsigned char>(floorV) : ambientBlue;
}

// R = liveFlat / authoredFlat per channel at light z: how far the per-mission sun keys moved the flat light (sun*z +
// ambient) away from what the prelit colormap and other authored art were burned with. Exactly 1 with no keys, at night
// and with no mission. The ground, decals and grass multiply it in (EnvironmentUBO.sceneLight.yzw); cement already
// uses the live flat light, so ground and cement move together.
void mc2_FlatLightRatio(float z, const unsigned char liveSun[3], const unsigned char liveAmb[3], float out[3])
{
	const auto r = mc2::authored_sun::flatRatio(s_authoredSun, {liveSun[0], liveSun[1], liveSun[2]}, {liveAmb[0], liveAmb[1], liveAmb[2]}, z, mc2NightAmbientFloor());
	out[0] = r[0]; out[1] = r[1]; out[2] = r[2];}

// The authored sun next to the live one (authored_sun.h OneSun): rotation, pitch undo, ambient and sun-colour relights. Identity / 1 with no keys.
void mc2_OneSun(const float liveDir[3], float livePitchDeg, float liveYawDeg, const unsigned char liveSun[3], const unsigned char liveAmb[3],
                float zLive, const float R[3], float out[19])   // out: 9 rotation (column-major), 3 pitchUndo, 3 ambRelight, 3 sunRelight, 1 authored-valid
{
	const auto s = mc2::authored_sun::oneSun(s_authoredSun, {liveDir[0], liveDir[1], liveDir[2]}, livePitchDeg, liveYawDeg, {liveSun[0], liveSun[1], liveSun[2]},
		{liveAmb[0], liveAmb[1], liveAmb[2]}, zLive, {R[0], R[1], R[2]}, mc2NightAmbientFloor());
	for (int c = 0; c < 3; ++c) {
		for (int r = 0; r < 3; ++r) out[c * 3 + r] = s.authFromLive[c][r];
		out[9 + c] = s.pitchUndo[c]; out[12 + c] = s.ambRelight[c]; out[15 + c] = s.sunRelight[c];
	}
	out[18] = s.authoredValid ? 1.0f : 0.0f;
}

// Live/authored ambient luma: the sun keys' move of the sky fill (scales the prop SH).
float mc2_AmbientRatio(const unsigned char liveAmb[3])
{
	return mc2::authored_sun::ambientRatio(s_authoredSun, {liveAmb[0], liveAmb[1], liveAmb[2]});
}

bool Camera::save( FitIniFile* file )
{
	file->writeBlock("Cameras");
	file->writeIdFloat( "ProjectionAngle", projectionAngle );
	file->writeIdFloat( "PositionX", position.x );
	file->writeIdFloat( "PositionY", position.y );
	file->writeIdFloat( "PositionZ", 0.0 );
	file->writeIdBoolean( "Ready", true );
	// LOCAL-LIGHTS-4: the colours below already carry the mission night profile; a load must not scale them twice.
	file->writeIdBoolean( "NightProfileApplied", missionLightApplied );
	file->writeIdBoolean( "SunAmbientScaled", sunAmbientScaled );
	if (s_authoredSun.valid)   // the colours before the sun keys, so a load of a keyed save keeps the flat-light ratio
	{
		file->writeIdBoolean( "AuthoredSunValid", true );
		file->writeIdFloat( "AuthoredLightPitch", s_authoredSun.pitch );
		if (s_authoredSun.hasYaw) file->writeIdFloat( "AuthoredLightYaw", s_authoredSun.yaw );
		file->writeIdUChar( "AuthoredLightRed", s_authoredSun.sun[0] ); file->writeIdUChar( "AuthoredLightGreen", s_authoredSun.sun[1] );
		file->writeIdUChar( "AuthoredLightBlue", s_authoredSun.sun[2] ); file->writeIdUChar( "AuthoredAmbientRed", s_authoredSun.amb[0] );
		file->writeIdUChar( "AuthoredAmbientGreen", s_authoredSun.amb[1] ); file->writeIdUChar( "AuthoredAmbientBlue", s_authoredSun.amb[2] );
	}

	file->writeIdUChar( "LightRed", lightRed );
	file->writeIdUChar( "LightGreen", lightGreen );
	file->writeIdUChar( "LightBlue", lightBlue );
	file->writeIdUChar( "AmbientBlue", ambientBlue );
	file->writeIdUChar( "AmbientGreen", ambientGreen );
	file->writeIdUChar( "AmbientRed", ambientRed );
	if (terrainShadowColorEnabled)
	{
		file->writeIdUChar( "TerrainShadowColorEnabled", 1 );
	}
	else
	{
		file->writeIdUChar( "TerrainShadowColorEnabled", 0 );
	}
	file->writeIdUChar( "TerrainShadowBlue", terrainShadowBlue );
	file->writeIdUChar( "TerrainShadowGreen", terrainShadowGreen );
	file->writeIdUChar( "TerrainShadowRed", terrainShadowRed );

	
	file->writeIdUChar( "DayLightRed", dayLightRed );
	file->writeIdUChar( "DayLightGreen", dayLightGreen );
	file->writeIdUChar( "DayLightBlue", dayLightBlue );
	file->writeIdUChar( "DayAmbientBlue", dayAmbientBlue );
	file->writeIdUChar( "DayAmbientGreen", dayAmbientGreen );
	file->writeIdUChar( "DayAmbientRed", dayAmbientRed );
	file->writeIdUChar( "SunsetLightRed", sunsetLightRed );
	file->writeIdUChar( "SunsetLightGreen", sunsetLightGreen );
	file->writeIdUChar( "SunsetLightBlue", sunsetLightBlue );
	file->writeIdUChar( "NightLightRed", nightLightRed );
	file->writeIdUChar( "NightLightGreen", nightLightGreen );
	file->writeIdUChar( "NightLightBlue", nightLightBlue );
	file->writeIdUChar( "NightAmbientBlue", nightAmbientBlue );
	file->writeIdUChar( "NightAmbientGreen", nightAmbientGreen );
	file->writeIdUChar( "NightAmbientRed", nightAmbientRed );

	file->writeIdUChar( "SeenBlue", seenBlue );
	file->writeIdUChar( "SeenGreen", seenGreen );
	file->writeIdUChar( "SeenRed", seenRed );
	file->writeIdUChar( "BaseBlue", baseBlue );
	file->writeIdUChar( "BaseGreen", baseGreen );
	file->writeIdUChar( "BaseRed", baseRed );
	
	file->writeIdFloat( "LightDirPitch", lightPitch );
	file->writeIdFloat("DayLightPitch",dayLightPitch);

	file->writeIdFloat("DayToNightTime",day2NightTransitionTime);
	file->writeIdFloat( "LightDirYaw", lightYaw );
	
	file->writeIdFloat( "NewScale", newScaleFactor );
	file->writeIdFloat( "StartRotation", getCameraRotation() );
	file->writeIdFloatArray( "LODScales", zoomLevelLODScale, 3 );
	file->writeIdFloat( "ElevationAdjustFactor", elevationAdjustFactor );
	file->writeIdFloat( "ZoomMax", zoomMax );
	file->writeIdFloat( "ZoomMin", zoomMin );
	file->writeIdFloat( "FOVMax",FOVMax);
	file->writeIdFloat( "FOVMin",FOVMin);
	file->writeIdFloat( "FogStart", fogStart );
	file->writeIdFloat( "FogFull", fogFull );
	file->writeIdULong( "FogColor", dayFogColor );
	file->writeIdFloat( "FogTransparency", fogTransparency );
	long userMin, userMax;
	int baseTerrain;
	land->getUserSettings( userMin, userMax, baseTerrain );
	file->writeIdLong( "UserMin", userMin );
	file->writeIdLong( "UserMax", userMax );
	file->writeIdLong( "BaseTerrain", baseTerrain );

	return true;

}

float Camera::getFarClipDistance()
{
	return MaxClipDistance;
}

void Camera::setFarClipDistance(float farClipDistance)
{
	MaxClipDistance = farClipDistance;
	DistanceFactor	= mc2FogInvSpan(MinHazeDistance, MaxClipDistance);
}

float Camera::getNearClipDistance()
{
	return NearPlaneDistance;
}

void Camera::setNearClipDistance(float nearClipDistance)
{
	NearPlaneDistance = nearClipDistance;
}

float Camera::getMaximumCameraAltitude()
{
	float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
	float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);

 	return testMax;
}
void Camera::setMaximumCameraAltitude(float maxAltitude)
{
	AltitudeMaximumLo = maxAltitude;
}
