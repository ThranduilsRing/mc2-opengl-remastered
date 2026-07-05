//---------------------------------------------------------------------------
//
// Camera.cpp -- File contains the camera class code
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef CAMERA_H
#include"camera.h"
#include"camera_frustum_math.h"
#endif

#ifndef TERRAIN_H
#include"terrain.h"
#endif

#ifndef VERTEX_H
#include"vertex.h"
#endif

#ifndef CRATER_H
#include"crater.h"
#endif

#ifndef TERRTXM_H
#include"terrtxm.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef USERINPUT_H
#include"userinput.h"
#endif

// [CAMERA_MOTION v1] recon-only motion-fraction tracking — gates whether
// Lifecycle Option C (camera-motion-gated fail-open) is shippable. See
// docs/superpowers/explorations/2026-05-07-lifecycle-normal-zoom-design.md.
#include"../GameOS/gameos/gos_profiler.h"
#include "cpu_proj_cost_split.h"  // F3 CPU projection cost-baseline (RAII scope)
#include <math.h>
#include <cstdlib>  // std::getenv for [VPL_PICK v1] env-gated trace (Step 3 3a)
#include <cstdio>   // std::printf for [VPL_PICK v1] lifecycle print
#include <vector>   // T1.15 [SPOT_DIAG v1] camera-overwrite probe state
#include "spotlight_diag.h"  // T1.16 — (E)-owned per-slot probe registry
#include "xform_conventions.h"  // XFORM-CONVENTION-HARNESS-1: lifted clip-space convention matrices

extern void AG_ellipse_draw(PANE *pane, LONG xc, LONG yc, LONG width, LONG height, LONG color);
extern void AG_ellipse_fill(PANE *pane, LONG xc, LONG yc, LONG width, LONG height, LONG color);
extern void AG_StatusBar( PANE *pane, int X0, int Y0, int X1, int Y1, int Color, int Width );
extern void AG_shape_draw (PANE *pane, void *shape_table,LONG shape_number, LONG hotX, LONG hotY);
extern void AG_shape_translate_draw (PANE *pane, void *shape_table,LONG shape_number, LONG hotX, LONG hotY);
extern void AG_shape_lookaside(MemoryPtr palette);

#pragma warning(disable:4305/*double to float truncation*/)

namespace {
// XFORM-CONVENTION-HARNESS-1: the two canonical clip-space convention
// matrices used to live here as anonymous-namespace free functions. They were
// lifted verbatim into the GL-free leaf TU mclib/xform_conventions.cpp so a
// host unit test (tests/unit/test_xform_convention.cpp) can link them without
// pulling GL-coupled camera.cpp. The full rationale for the sign/handedness
// choices is preserved at the new TU's function definitions. camera.cpp keeps
// its two file-scope constants and simply initializes them from the lifted
// functions; the hot paths below are byte-identical to before.
const Stuff::Matrix4D kAxisSwapMC2toGL   = mc2xform::makeAxisSwapMC2toGL();
const Stuff::Matrix4D kPixelHomogToGLNDC = mc2xform::makePixelHomogToGLNDC();
} // namespace

inline float agsqrt( float _a, float _b )
{
	return sqrt(_a*_a + _b*_b);
}

//---------------------------------------------------------------------------
// Static Globals

char WindowTitle[1024];		// Global window title (GetWindowText is VERY slow)

// Scroll-velocity momentum for adaptive zoom speed.
// Bumped by ZoomIn/ZoomOut each call, decayed every frame by Camera::update().
static float s_scrollMomentum = 0.0f;

long topCtrlUpd = 0;

extern long scenarioEndTurn;
extern float actualTime;


#define TABLE_ENTRY_SIZE		256
#define SCALE_LEVELS			7
#define	RAMPED_RED				239
#define RAMPED_GREEN			248
#define NO_RAM_FOR_SCALE_TABLE	0x12120001

#define NIGHT_START_PITCH		(25.0f)
#define NIGHT_LIGHT_PITCH		(10.0f)
#define TERRAIN_LIGHTS_ON		(40.0f)

#ifdef _DEBUG
extern long currentHotSpot;
#endif

// RNA - moved this out of the #ifdef PROFILE so it's available in release
extern long displayProfileData;

extern bool drawTerrainGrid;
extern long mechElements;

long leaveSwoopyOff = 0;

extern long tileCacheReqs;
extern long tileCacheHits;
extern long tileCacheMiss;

bool drawCameraCircle = FALSE;
extern bool gamePaused;
extern bool gameAsked;
MemoryPtr pauseShape = NULL;
MemoryPtr askedShape = NULL;
extern bool gRestartRender;
bool MaxObjectsDrawn = FALSE;

float elevationAdjustFactor = 50.0;

float Camera::MIN_PERSPECTIVE =			-20.0f;  // allow looking up to 20deg above horizontal
float Camera::MIN_ORTHO		  =			18.0f;
float Camera::MAX_PERSPECTIVE =			88.0f;
float Camera::MAX_ORTHO		  =			88.0f;

#define NORM_PERSPECTIVE		35.0f

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
float Camera::DistanceFactor	= 1.0f / (MaxClipDistance - MinHazeDistance);
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

extern bool recalcLight;
 
const float CAM_THRESHOLD = 150.0f;

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
	DistanceFactor = 1.0f / (MaxClipDistance - MinHazeDistance + 1.0f);

 	setClass(BASE_CAMERA);
	
	//Replace with TGL
	if ( !worldLights ) // only do the alloc once
	{
		worldLights = (TG_LightPtr *)systemHeap->Malloc(sizeof(TG_LightPtr) * MAX_LIGHTS_IN_WORLD);
		memset(worldLights,0,sizeof(TG_LightPtr) * MAX_LIGHTS_IN_WORLD);
		numLights = 2;

		//---------------------------------------------------------
		// First light is sun.
		worldLights[0] = (TG_LightPtr)systemHeap->Malloc(sizeof(TG_Light));
		worldLights[0]->init(TG_LIGHT_INFINITE);
	}
	
	if ( !activeLights ) // only do the alloc once
	{
		activeLights = (TG_LightPtr *)systemHeap->Malloc(sizeof(TG_LightPtr) * MAX_LIGHTS_IN_WORLD);
		memset(activeLights,0,sizeof(TG_LightPtr) * MAX_LIGHTS_IN_WORLD);
		numActiveLights = 0;
	}
	
	if ( !terrainLights ) // only do the alloc once
	{
		terrainLights = (TG_LightPtr *)systemHeap->Malloc(sizeof(TG_LightPtr) * MAX_LIGHTS_IN_WORLD);
		memset(terrainLights,0,sizeof(TG_LightPtr) * MAX_LIGHTS_IN_WORLD);
		numTerrainLights = 0;
	}
	
  	Stuff::LinearMatrix4D lightToWorldMatrix;
	
	lightToWorldMatrix.BuildTranslation(Stuff::Point3D(0.0,0.0,0.0));
	lightToWorldMatrix.BuildRotation(Stuff::EulerAngles(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));

	worldLights[0]->SetLightToWorld(&lightToWorldMatrix);

	//---------------------------------------------------------
	// Second light is ambient.
	worldLights[1] = (TG_LightPtr)systemHeap->Malloc(sizeof(TG_Light));
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
    fogColor = (DWORD)tmp;
	dayFogColor = fogColor;
	gosASSERT(result == NO_ERR);

	result = cameraFile->readIdFloat("FogTransparency",fogTransparency);
#if 0
	if (result != NO_ERR)
	{
		/* This is an old file where FogColor includes the color of the sky showing through. */

		/* arbitrary assumed daytime sky color */
		static const float fDaySkyRed = 100.0f;
		static const float fDaySkyGreen = 162.0f;
		static const float fDaySkyBlue = 255.0f;

		const float fDayFogRed = (float)((dayFogColor >> 16) & 0xff);
		const float fDayFogGreen = (float)((dayFogColor >> 8) & 0xff);
		const float fDayFogBlue = (float)((dayFogColor) & 0xff);

		/* We assume that the fog color specified by the user is a combination of the diffuse
		light scattered by the fog and the light from the sky showing through the fog. There's
		no way to know how transparent the fog is meant to be without the user specifying it, 
		but for now we'll assume the maximum transparency possible given the daytime fog
		color and an assumed daytime sky color. */

		float fFogTransparency = 1.0f;
		if ((0.0f < fDaySkyRed) && (fDayFogRed / fDaySkyRed < fFogTransparency))
		{
			fFogTransparency = fDayFogRed / fDaySkyRed;
		}
		if ((0.0f < fDaySkyGreen) && (fDayFogGreen / fDaySkyGreen < fFogTransparency))
		{
			fFogTransparency = fDayFogGreen / fDaySkyGreen;
		}
		if ((0.0f < fDaySkyBlue) && (fDayFogBlue / fDaySkyBlue < fFogTransparency))
		{
			fFogTransparency = fDayFogBlue / fDaySkyBlue;
		}

		/* Here we subtract out the sky color component to get the actual color of the fog. */
		int opaqueDayFogRed = fDayFogRed - fFogTransparency * fDaySkyRed;
		int opaqueDayFogGreen = fDayFogGreen - fFogTransparency * fDaySkyGreen;
		int opaqueDayFogBlue = fDayFogBlue - fFogTransparency * fDaySkyBlue;
		gosASSERT(0 == ((~0xff) & opaqueDayFogRed));
		gosASSERT(0 == ((~0xff) & opaqueDayFogGreen));
		gosASSERT(0 == ((~0xff) & opaqueDayFogBlue));

		dayFogColor = (opaqueDayFogRed << 16) + (opaqueDayFogGreen << 8) + opaqueDayFogBlue;
		fogTransparency = fFogTransparency;
	}
	gosASSERT(result == NO_ERR);
#endif

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
	if (worldLights)
	{
		systemHeap->Free(worldLights[1]);
		worldLights[1] = NULL;

		systemHeap->Free(worldLights[0]);
		worldLights[0] = NULL;

		systemHeap->Free(worldLights);
		worldLights = NULL;
	}
	
	if (activeLights)
	{
		systemHeap->Free(activeLights);
		activeLights = NULL;
	}
	
	if (terrainLights)
	{
		systemHeap->Free(terrainLights);
		terrainLights = NULL;
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
	Stuff::Vector3D worldPoint;
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
	if (tileR < 0)
		tileR = 0;
	else if (tileR >= Terrain::realVerticesMapSide)
		tileR = Terrain::realVerticesMapSide - 1;
	if (tileC < 0)
		tileC = 0;
	else if (tileC >= Terrain::realVerticesMapSide)
		tileC = Terrain::realVerticesMapSide - 1;

	row = (long)tileR;
	col = (long)tileC;
}

//---------------------------------------------------------------------------

inline void mapTileCellToWorldPos (long tileR, long tileC, long cellR, long cellC, Stuff::Vector3D& worldPos) {

	worldPos.x = Terrain::tileColToWorldCoord[tileC] + Terrain::cellToWorldCoord[cellC] + Terrain::halfWorldUnitsPerCell;
	worldPos.y = Terrain::tileRowToWorldCoord[tileR] - Terrain::cellToWorldCoord[cellR] - Terrain::halfWorldUnitsPerCell;
	worldPos.z = (float)0.0;
}

//---------------------------------------------------------------------------

inline void mapCellToWorldPos (long cellR, long cellC, Stuff::Vector3D& worldPos) {

	worldPos.x = Terrain::cellColToWorldCoord[cellC] + Terrain::halfWorldUnitsPerCell;
	worldPos.y = Terrain::cellRowToWorldCoord[cellR] - Terrain::halfWorldUnitsPerCell;
	worldPos.z = (float)0.0;
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
	const Stuff::Matrix4D M = worldToClipGL();

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
		planes[p][0] = ws * M(0, 3) + as * M(0, c);   // world X coef
		planes[p][1] = ws * M(1, 3) + as * M(1, c);   // world Y coef
		planes[p][2] = ws * M(2, 3) + as * M(2, c);   // world Z coef
		planes[p][3] = ws * M(3, 3) + as * M(3, c);   // constant
	}
}

//---------------------------------------------------------------------------
// Conservative p-vertex AABB-vs-frustum test. For each of 6 world-space planes
// pick the AABB corner most in the plane's positive direction; if that corner
// is strictly behind the plane the AABB is fully outside -> reject. Never
// false-negative; may false-positive slightly outside (acceptable - picking
// refines with an exact forward-projection screen test downstream).
bool Camera::quadAabbInFrustum (const float planes[6][4],
                                const Stuff::Vector3D& mn,
                                const Stuff::Vector3D& mx) const
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
                                        const Stuff::Vector3D& mn,
                                        const Stuff::Vector3D& mx) const
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
}

const float (*Camera::getCachedFrustumPlanes() const)[4]
{
	return cachedFrustumPlanes_;
}

//---------------------------------------------------------------------------
// Ported screen-triangle containment (the body of the deleted overThisTile),
// rewritten to operate on FRESHLY forward-projected screen corners instead of
// stale per-frame vertices[]->px/py. corners[4] are screen XY from
// projectForSelectionPicking; valid[4] flags whether each projected inside the
// viewport rect. A tile is "over" the cursor if it contains it in either of
// its two triangles (0,1,2) / (0,2,3) - same theorem as the original.
static inline bool s_pointInScreenTri (const Stuff::Point3D& v0,
                                       const Stuff::Point3D& v1,
                                       const Stuff::Point3D& v2,
                                       long mouseX, long mouseY)
{
	// CAMERA-FRUSTUM-HARNESS-1: delegate to camera_frustum_math.h (same arithmetic).
	return camera_frustum_math::pointInScreenTri(v0.x, v0.y, v1.x, v1.y,
	                                             v2.x, v2.y, mouseX, mouseY);
}

static inline bool s_overThisTileProjected (const Stuff::Point3D corners[4],
                                             const bool valid[4],
                                             long mouseX, long mouseY)
{
	// Triangle one: corners 0,1,2.
	if (valid[0] && valid[1] && valid[2] &&
	    s_pointInScreenTri(corners[0], corners[1], corners[2], mouseX, mouseY))
		return true;

	// Triangle two: corners 0,2,3 (tiles are two polys).
	if (valid[0] && valid[2] && valid[3] &&
	    s_pointInScreenTri(corners[0], corners[2], corners[3], mouseX, mouseY))
		return true;

	return false;
}
		
//---------------------------------------------------------------------------
// PARITY DIAGNOSTIC helper (MC2_TERRAIN_PICK_PARITY). Logs the raycast vs
// legacy world results for the same screen coords, then yields the value the
// active flag-on mode (raycast-if-flag-on-else-legacy) would have returned:
//   - raycast hit  -> overwrite point with the raycast hit, return 0.
//   - raycast miss -> leave the legacy point/return unchanged.
static unsigned long parityReport (Stuff::Vector2DOf<long> &screenPos,
                                   bool raycastHit,
                                   const Stuff::Vector3D &raycastPt,
                                   Stuff::Vector3D &point,
                                   unsigned long legacyRet)
{
	const long verticesMapSideDivTwo = Terrain::realVerticesMapSide / 2;
	const long metersMapSideDivTwo =
		verticesMapSideDivTwo * float2long(Terrain::worldUnitsPerVertex);

	// Tile cell index for the legacy world point.
	long legacyTileC =
		float2long(point.x * Terrain::oneOverWorldUnitsPerVertex + verticesMapSideDivTwo);
	long legacyTileR =
		float2long((metersMapSideDivTwo - point.y) * Terrain::oneOverWorldUnitsPerVertex);

	long rcTileC = -1, rcTileR = -1;
	float dWorld = -1.0f;
	if (raycastHit)
	{
		rcTileC = float2long(raycastPt.x * Terrain::oneOverWorldUnitsPerVertex + verticesMapSideDivTwo);
		rcTileR = float2long((metersMapSideDivTwo - raycastPt.y) * Terrain::oneOverWorldUnitsPerVertex);
		const float ddx = raycastPt.x - point.x;
		const float ddy = raycastPt.y - point.y;
		const float ddz = raycastPt.z - point.z;
		const float d2 = ddx*ddx + ddy*ddy + ddz*ddz;
		dWorld = (d2 > 0.0f) ? sqrtf(d2) : 0.0f;
	}

	std::printf("[PICK_PARITY] screen=(%ld,%ld)  "
	            "raycast=(%.3f,%.3f,%.3f) cell=(%ld,%ld)  "
	            "legacy=(%.3f,%.3f,%.3f) cell=(%ld,%ld)  "
	            "dWorld=%.4f  raycastHit=%d\n",
	            (long)screenPos.x, (long)screenPos.y,
	            raycastPt.x, raycastPt.y, raycastPt.z, rcTileC, rcTileR,
	            point.x, point.y, point.z, legacyTileC, legacyTileR,
	            dWorld, raycastHit ? 1 : 0);
	std::fflush(stdout);

	// Preserve flag-on return behavior: raycast wins when it hit.
	if (raycastHit)
	{
		point = raycastPt;
		return 0;
	}
	return legacyRet;
}

//---------------------------------------------------------------------------
// [LOW-CAMERA] Guard-free general 4x4 inverse (Gauss-Jordan, partial pivot).
// Stuff::Matrix4D::Invert flags any det < SMALL(1e-4) singular -> ×1e30 garbage,
// which is EVERY reverse-Z projective matrix (the documented "Invert(
// worldToClipGL()) UNRELIABLE in the game" trap, camera.h). Used ONLY by the
// inverse-VP cursor->world helpers below (preview + mouse-anchored zoom); the
// global Matrix4D::Invert is left byte-identical, and terrain PICKING uses the
// forward-projection path, not this — so no working path is affected.
static bool lowCamInvert4x4 (const Stuff::Matrix4D& M, Stuff::Matrix4D& out)
{
	double a[4][8];
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
		{ a[r][c] = (double)M(r,c); a[r][c+4] = (r==c)?1.0:0.0; }
	for (int col = 0; col < 4; ++col)
	{
		int piv = col; double best = fabs(a[col][col]);
		for (int r = col+1; r < 4; ++r)
			if (fabs(a[r][col]) > best) { best = fabs(a[r][col]); piv = r; }
		if (best < 1e-20) return false;
		if (piv != col) for (int k = 0; k < 8; ++k) { double t=a[col][k]; a[col][k]=a[piv][k]; a[piv][k]=t; }
		double inv = 1.0 / a[col][col];
		for (int k = 0; k < 8; ++k) a[col][k] *= inv;
		for (int r = 0; r < 4; ++r) if (r != col)
		{ double f=a[r][col]; for (int k=0;k<8;++k) a[r][k]-=f*a[col][k]; }
	}
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			out(r,c) = (float)a[r][c+4];
	return true;
}

//---------------------------------------------------------------------------
// O(1) screen -> z=0 ground-plane unproject. No quad scan, no terrain pick.
// See header. Mirrors the matrix-inverse unproject used by the LOD-chunk pick
// path, then intersects the camera ray with the world z=0 plane.
bool Camera::screenToGroundPlaneApprox (long screenX, long screenY, Stuff::Vector3D &outWorld, float z_plane)
{
	outWorld.x = outWorld.y = outWorld.z = 0.0f;
	if (turn < 4)
		return false;

	Stuff::Matrix4D M = worldToClipGL();
	Stuff::Matrix4D Minv;
	if (!lowCamInvert4x4(M, Minv)) return false;

	const float w = screenResolution.x;
	const float h = screenResolution.y;
	if (w <= 0.0f || h <= 0.0f)
		return false;

	const float ndcX =  2.0f * (float(screenX) / w) - 1.0f;
	const float ndcY =  1.0f - 2.0f * (float(screenY) / h);

	// clip_row * Minv (row-vector convention), perspective divide.
	auto unprojectNDC = [&](float nz) -> Stuff::Vector3D {
		float wx = ndcX*Minv(0,0) + ndcY*Minv(1,0) + nz*Minv(2,0) + Minv(3,0);
		float wy = ndcX*Minv(0,1) + ndcY*Minv(1,1) + nz*Minv(2,1) + Minv(3,1);
		float wz = ndcX*Minv(0,2) + ndcY*Minv(1,2) + nz*Minv(2,2) + Minv(3,2);
		float ww = ndcX*Minv(0,3) + ndcY*Minv(1,3) + nz*Minv(2,3) + Minv(3,3);
		const float invW = (fabsf(ww) > 1e-8f) ? (1.0f / ww) : 0.0f;
		Stuff::Vector3D v; v.x = wx*invW; v.y = wy*invW; v.z = wz*invW;
		return v;
	};
	// GL-swapped (x'=-x, y'=z, z'=y) -> MC2 world (x=-glX, y=glZ, z=glY).
	auto glToMC2 = [](const Stuff::Vector3D& g) -> Stuff::Vector3D {
		Stuff::Vector3D r; r.x = -g.x; r.y = g.z; r.z = g.y; return r;
	};

	// REVERSE-Z, ZERO_TO_ONE: near plane is NDC z=1, far is z=0 (see inverseProject).
	Stuff::Vector3D ro = glToMC2(unprojectNDC(1.0f));    // near
	Stuff::Vector3D rf = glToMC2(unprojectNDC( 0.0f));   // far

	const float dz = rf.z - ro.z;
	if (fabsf(dz) < 1e-6f) { outWorld = ro; outWorld.z = 0.0f; return false; }

	const float t = (z_plane - ro.z) / dz;
	outWorld.x = ro.x + t * (rf.x - ro.x);
	outWorld.y = ro.y + t * (rf.y - ro.y);
	outWorld.z = z_plane;
	return true;
}

//---------------------------------------------------------------------------
bool Camera::screenToTerrainApprox (long screenX, long screenY, Stuff::Vector3D &outWorld)
{
	outWorld.x = outWorld.y = outWorld.z = 0.0f;
	if (turn < 4)
		return false;

	Stuff::Matrix4D M = worldToClipGL();
	Stuff::Matrix4D Minv;
	if (!lowCamInvert4x4(M, Minv)) return false;

	const float w = screenResolution.x;
	const float h = screenResolution.y;
	if (w <= 0.0f || h <= 0.0f)
		return false;

	const float ndcX =  2.0f * (float(screenX) / w) - 1.0f;
	const float ndcY =  1.0f - 2.0f * (float(screenY) / h);

	auto unprojectNDC = [&](float nz) -> Stuff::Vector3D {
		float wx = ndcX*Minv(0,0) + ndcY*Minv(1,0) + nz*Minv(2,0) + Minv(3,0);
		float wy = ndcX*Minv(0,1) + ndcY*Minv(1,1) + nz*Minv(2,1) + Minv(3,1);
		float wz = ndcX*Minv(0,2) + ndcY*Minv(1,2) + nz*Minv(2,2) + Minv(3,2);
		float ww = ndcX*Minv(0,3) + ndcY*Minv(1,3) + nz*Minv(2,3) + Minv(3,3);
		const float invW = (fabsf(ww) > 1e-8f) ? (1.0f / ww) : 0.0f;
		Stuff::Vector3D v; v.x = wx*invW; v.y = wy*invW; v.z = wz*invW;
		return v;
	};
	auto glToMC2 = [](const Stuff::Vector3D& g) -> Stuff::Vector3D {
		Stuff::Vector3D r; r.x = -g.x; r.y = g.z; r.z = g.y; return r;
	};

	// REVERSE-Z, ZERO_TO_ONE: near plane is NDC z=1, far is z=0.
	Stuff::Vector3D ro = glToMC2(unprojectNDC(1.0f));    // near
	Stuff::Vector3D rf = glToMC2(unprojectNDC( 0.0f));   // far

	const float dz = rf.z - ro.z;
	if (fabsf(dz) < 1e-6f) { outWorld = ro; return false; }

	// Fixed-point iteration: intersect the ray with the horizontal plane at
	// the terrain height of the previous hit. Converges in 2-3 steps on any
	// terrain a camera ray can see (slope bounded by the tilt).
	float planeZ = 0.0f;
	Stuff::Vector3D hit;
	hit.x = hit.y = hit.z = 0.0f;
	for (int iter = 0; iter < 3; ++iter)
	{
		const float t = (planeZ - ro.z) / dz;
		hit.x = ro.x + t * (rf.x - ro.x);
		hit.y = ro.y + t * (rf.y - ro.y);
		hit.z = planeZ;
		if (!land)
			break;
		const float e = land->getTerrainElevation(hit);
		if (fabsf(e - planeZ) < 1.0f)
		{
			hit.z = e;
			break;
		}
		planeZ = e;
	}
	outWorld = hit;
	return true;
}

//---------------------------------------------------------------------------
unsigned long Camera::inverseProject (Stuff::Vector2DOf<long> &screenPos, Stuff::Vector3D &point)
{
	if (turn < 4)
	{
		point.x = point.y = point.z = 0.0f;
		return 0;
	}

	//-----------------------------------------------------------
	// Phase 7B: heightfield raycast picker — replaces the quadList AABB/
	// screen-triangle scan when MC2_TERRAIN_LOD_CHUNK=1.
	// The quadList may be empty or stale under the LOD chunk path, so we
	// build a world-space ray from the screen pixel via worldToClipGL()^-1
	// and fire it against the full-resolution PostcompVertex heightfield.
	// Object picking (findObjectByMouse) is UNAFFECTED — this replaces only
	// the terrain-surface portion of inverseProject.
	// DECOUPLED FROM CHUNK RENDER: the chunk heightfield-raycast picker's
	// screen->world ray is broken (getPosition() returns the camera GROUND-FOCUS
	// point not the eye, and the worldToClipGL inverse collapses the X response),
	// so every click hit a near-constant cell -> move orders teleported across the
	// map. The legacy forward-projection quadList picker below is correct
	// (projectForSelectionPicking uses the same matrix the GPU renders with), so
	// picking uses IT even when the chunk RENDER path is on (MC2_TERRAIN_LOD_CHUNK).
	// Opt back into the raycast picker with MC2_TERRAIN_RAYCAST_PICK to debug it.
	static const bool s_lodChunkPick = (std::getenv("MC2_TERRAIN_RAYCAST_PICK") != nullptr);
	// PARITY DIAGNOSTIC: when MC2_TERRAIN_PICK_PARITY is set we compute BOTH
	// the heightfield raycast hit AND the legacy quadList result for the same
	// screen coords, log the delta, and still return exactly what the active
	// mode (raycast-if-flag-on-else-legacy) would have returned.
	static const bool s_pickParity = (std::getenv("MC2_TERRAIN_PICK_PARITY") != nullptr);
	bool   parityRaycastHit = false;
	Stuff::Vector3D parityRaycastPt;
	parityRaycastPt.x = parityRaycastPt.y = parityRaycastPt.z = 0.0f;
	if (s_lodChunkPick && land)
	{
		// Build the GL clip-to-world inverse matrix.
		Stuff::Matrix4D M = worldToClipGL();
		Stuff::Matrix4D Minv;
		Minv.Invert(M);

		// Convert screen pixel to GL NDC.
		// screenPos: Y-down from top-left (same convention as screenResolution).
		// GL NDC: x in [-1,+1] left-to-right, y in [-1,+1] bottom-to-top.
		const float w = screenResolution.x;
		const float h = screenResolution.y;
		const float ndcX =  2.0f * (float(screenPos.x) / w) - 1.0f;
		const float ndcY =  1.0f - 2.0f * (float(screenPos.y) / h);

		// Unproject near and far NDC points to world space.
		// Row-vector convention: world_row = clip_row * Minv.
		// Clip near: (ndcX, ndcY, -1, 1); Clip far: (ndcX, ndcY, +1, 1).
		auto unprojectNDC = [&](float nx, float ny, float nz) -> Stuff::Vector3D {
			// clip_row * Minv; w=1
			float wx = nx * Minv(0,0) + ny * Minv(1,0) + nz * Minv(2,0) + Minv(3,0);
			float wy = nx * Minv(0,1) + ny * Minv(1,1) + nz * Minv(2,1) + Minv(3,1);
			float wz = nx * Minv(0,2) + ny * Minv(1,2) + nz * Minv(2,2) + Minv(3,2);
			float ww = nx * Minv(0,3) + ny * Minv(1,3) + nz * Minv(2,3) + Minv(3,3);
			Stuff::Vector3D v;
			const float invW = (fabsf(ww) > 1e-8f) ? (1.0f / ww) : 0.0f;
			v.x = wx * invW;
			v.y = wy * invW;
			v.z = wz * invW;
			return v;
		};

		// REVERSE-Z, ZERO_TO_ONE clip depth (glClipControl + glClearDepth(0)):
		// the NEAR plane is NDC z=1. The FAR plane (z=0) unproject is unreliable
		// here (it came out ABOVE the camera, inverting the ray), so we do NOT use
		// it. Instead build the ray from the known camera eye position (MC2 world)
		// toward the well-conditioned near-plane point.
		Stuff::Vector3D nearPt = unprojectNDC(ndcX, ndcY, 1.0f);

		// worldToClipGL uses the GL axis swap (x'=-x, y'=z, z'=y); invert it:
		//   mc2X = -glX,  mc2Y = glZ,  mc2Z = glY
		auto glToMC2 = [](const Stuff::Vector3D& g) -> Stuff::Vector3D {
			Stuff::Vector3D r;
			r.x = -g.x;
			r.y =  g.z;
			r.z =  g.y;
			return r;
		};
		Stuff::Vector3D nearMc = glToMC2(nearPt);

		// Origin = camera eye (the matrix the GPU renders with is built from this);
		// direction = eye -> near-plane point (on the pixel ray, in front of eye).
		Stuff::Vector3D ro = getPosition();
		float rdx = nearMc.x - ro.x;
		float rdy = nearMc.y - ro.y;
		float rdz = nearMc.z - ro.z;
		const float rlen = sqrtf(rdx*rdx + rdy*rdy + rdz*rdz);
		if (rlen > 1e-6f)
		{
			const float inv = 1.0f / rlen;
			rdx *= inv; rdy *= inv; rdz *= inv;
		}

		float hitX, hitY, hitZ;
		if (Terrain::raycastTerrain(ro.x, ro.y, ro.z, rdx, rdy, rdz,
		                            &hitX, &hitY, &hitZ))
		{
			if (s_pickParity)
			{
				// Record the raycast hit but DO NOT return yet: let the legacy
				// quadList path below run too so we can log both and the delta.
				parityRaycastHit = true;
				parityRaycastPt.x = hitX;
				parityRaycastPt.y = hitY;
				parityRaycastPt.z = hitZ;
			}
			else
			{
				point.x = hitX;
				point.y = hitY;
				point.z = hitZ;
				return 0;
			}
		}
		// Miss — fall through to the quadList path (which may also miss and
		// return 1). On an empty quadList the old path will iterate zero tiles
		// and hit the off-map fallback, which returns 1 (acceptable miss).
	}

	//-----------------------------------------------------------
	// VPL-retirement Step 3 (3a): recursion-free tile selection.
	// Replaces the old per-frame VPL tile walk (clipInfo admission +
	// overThisTile screen test reading stale vertices[]->px/py + closest-pz
	// selection). Two stages, both pure CPU, no GPU readback:
	//   (1) frustum-AABB admission over all quads via the shared helper
	//       (extractFrustumPlanes + quadAabbInFrustum on RAW world AABBs),
	//   (2) exact forward-projection screen-containment + nearest-screen
	//       refinement (ported overThisTile screen-triangle test on FRESHLY
	//       projected corners, NOT stale per-frame px/py).
	// This path never calls inverseProjectForPicking / inverseProjectZ, so
	// the !usePerspective recursion (inverseProjectZ) is structurally
	// unreachable from picking regardless of CTRL+ALT+P toggle state.
	static const bool s_vplPickTrace =
		(std::getenv("MC2_VPL_PICK") != nullptr);
	{
		static bool s_announced = false;
		if (s_vplPickTrace && !s_announced)
		{
			s_announced = true;
			std::printf("[VPL_PICK v1] event=repoint_active "
			            "path=frustum_aabb+forward_proj_refine\n");
		}
	}

	TerrainQuadPtr currentTile = land->getQuadList();
	unsigned long numTiles = land->getNumQuads();
	TerrainQuadPtr closestTiles[100];
	memset(closestTiles,0,sizeof(TerrainQuadPtr)*100);

	TerrainQuadPtr closestTile = NULL;
	long currentClosest = 0;
	VertexPtr closestVertex = NULL;

	float planes[6][4];
	extractFrustumPlanes(planes);

	// Optional env-gated build-free self-test of the matrix/swizzle math:
	// for sampled quads, any RAW world point whose forward projection lands
	// strictly inside the viewport rect with screen.z in [0,1) MUST pass the
	// degenerate-AABB frustum test (helper may be true when projection is
	// just outside - acceptable; helper must NEVER be false when projection
	// is strictly inside).
	if (s_vplPickTrace && numTiles > 0)
	{
		const long sampleStride = (numTiles > 64) ? (long)(numTiles / 64) : 1;
		TerrainQuadPtr st = land->getQuadList();
		long checked = 0;
		for (long i = 0; i < (long)numTiles; i += sampleStride)
		{
			TerrainQuadPtr q = st + i;
			Stuff::Vector3D wp;
			wp.x = q->vertices[0]->vx;
			wp.y = q->vertices[0]->vy;
			wp.z = q->vertices[0]->pVertex->elevation;

			Stuff::Vector4D sc;
			eye->projectForSelectionPicking(wp, sc);
			const bool strictlyInside =
				(sc.x > 0.0f) && (sc.y > 0.0f) &&
				(sc.x < screenResolution.x) && (sc.y < screenResolution.y) &&
				(sc.z >= 0.0f) && (sc.z < 1.0f);
			if (strictlyInside)
			{
				const bool inFr = quadAabbInFrustum(planes, wp, wp);
				gosASSERT(inFr); // conservative helper: never false when proj inside
			}
			if (++checked >= 64)
				break;
		}
	}

	//-----------------------------------------------------------
	// [LOW-CAMERA-PICK-RAY-1 v2] Two root causes make the legacy picker mis-land
	// at LOW/grazing pitch (greybeard + adversarial recon, both in the EXISTING
	// forward-projection picker — NOT a raycast):
	//   (A) the 100-tile cap (closestTiles[100]) fills in MAP ORDER, so at low
	//       pitch the frustum admits >>100 tiles and the true tile under the
	//       cursor is dropped before it is ever projected; and
	//   (B) the Stage-2 tiebreak is DEPTH-BLIND (corners[].z=0; picks the tile
	//       whose nearest projected corner is closest in SCREEN space), so among
	//       the many tiles a grazing ray pierces it readily picks a FAR one.
	// Fix = single pass (no cap) + pick the FRONTMOST tile that CONTAINS the
	// cursor (min eye-distance), reusing the same projection + containment test.
	// Gated MC2_LOWCAM_PICK (default ON; =0 -> exact legacy two-stage below).
	static const bool s_lowCamPick =
		[]{ const char* v = std::getenv("MC2_LOWCAM_PICK"); return !(v && v[0]=='0'); }();
	static const bool s_capTrace = (std::getenv("MC2_PICK_CAP_TRACE") != nullptr);

	if (s_lowCamPick)
	{
		long admitted = 0, contained = 0;
		double bestEyeDistSq = 1.0e30;
		TerrainQuadPtr t = land->getQuadList();
		for (long i = 0; i < (long)numTiles; i++, t++)
		{
			Stuff::Vector3D mn, mx;
			mn.x = mx.x = t->vertices[0]->vx;
			mn.y = mx.y = t->vertices[0]->vy;
			mn.z = mx.z = t->vertices[0]->pVertex->elevation;
			for (int c = 1; c < 4; c++)
			{
				const float vx = t->vertices[c]->vx;
				const float vy = t->vertices[c]->vy;
				const float vz = t->vertices[c]->pVertex->elevation;
				if (vx < mn.x) mn.x = vx; if (vx > mx.x) mx.x = vx;
				if (vy < mn.y) mn.y = vy; if (vy > mx.y) mx.y = vy;
				if (vz < mn.z) mn.z = vz; if (vz > mx.z) mx.z = vz;
			}
			if (!quadAabbInFrustum(planes, mn, mx))
				continue;
			admitted++;

			Stuff::Point3D corners[4];
			bool valid[4];
			float cx = 0.0f, cy = 0.0f, cz = 0.0f;   // world tile center
			for (int c = 0; c < 4; c++)
			{
				Stuff::Vector3D wp;
				wp.x = t->vertices[c]->vx;
				wp.y = t->vertices[c]->vy;
				wp.z = t->vertices[c]->pVertex->elevation;
				cx += wp.x; cy += wp.y; cz += wp.z;
				Stuff::Vector4D sc;
				eye->projectForSelectionPicking(wp, sc);
				corners[c].x = sc.x;
				corners[c].y = sc.y;
				corners[c].z = 0.0f;
				valid[c] = (sc.x >= 0.0f) && (sc.y >= 0.0f) &&
				           (sc.x <= screenResolution.x) &&
				           (sc.y <= screenResolution.y) &&
				           (sc.z >= 0.0f) && (sc.z < 1.0f);
			}
			if (!s_overThisTileProjected(corners, valid, screenPos.x, screenPos.y))
				continue;
			contained++;
			// Frontmost = smallest eye distance among containing tiles. physicalPos
			// is the true camera eye (MC2 world); getPosition() is the ground-focus.
			cx *= 0.25f; cy *= 0.25f; cz *= 0.25f;
			const double ex = (double)cx - (double)physicalPos.x;
			const double ey = (double)cy - (double)physicalPos.y;
			const double ez = (double)cz - (double)physicalPos.z;
			const double eyeDistSq = ex*ex + ey*ey + ez*ez;
			if (eyeDistSq < bestEyeDistSq)
			{
				bestEyeDistSq = eyeDistSq;
				closestTile = t;
			}
		}
		if (s_capTrace)
			fprintf(stderr, "[PICK_CAP] mode=lowcam numTiles=%lu admitted=%ld "
				"contained=%ld cursor=(%ld,%ld) hit=%d\n",
				numTiles, admitted, contained,
				(long)screenPos.x, (long)screenPos.y, closestTile ? 1 : 0);
	}
	else
	{
	//-----------------------------------------------------------
	// Stage 1: frustum-AABB admission over all quads. RAW world AABB built
	// from vertices[0..3]->vx/.vy/->pVertex->elevation (unrotated world
	// coords per vertex.h:75). Keep the existing cap-100 + guard.
	for (long i=0;i<(long)numTiles;i++)
	{
		Stuff::Vector3D mn, mx;
		mn.x = mx.x = currentTile->vertices[0]->vx;
		mn.y = mx.y = currentTile->vertices[0]->vy;
		mn.z = mx.z = currentTile->vertices[0]->pVertex->elevation;
		for (int c = 1; c < 4; c++)
		{
			const float vx = currentTile->vertices[c]->vx;
			const float vy = currentTile->vertices[c]->vy;
			const float vz = currentTile->vertices[c]->pVertex->elevation;
			if (vx < mn.x) mn.x = vx; if (vx > mx.x) mx.x = vx;
			if (vy < mn.y) mn.y = vy; if (vy > mx.y) mx.y = vy;
			if (vz < mn.z) mn.z = vz; if (vz > mx.z) mx.z = vz;
		}

		if (quadAabbInFrustum(planes, mn, mx) && (currentClosest < 100))
		{
			closestTiles[currentClosest] = currentTile;
			currentClosest++;
		}

		currentTile++;
	}

	//-----------------------------------------------------------
	// Stage 2: exact forward-projection screen-containment + nearest-screen
	// refinement. Forward-project each admitted tile's 4 corners FRESH via
	// projectForSelectionPicking, run the ported screen-triangle test, and
	// keep the tile whose nearest projected corner is closest to the cursor
	// in screen space (same nearest-screen-distance tiebreak semantics as
	// the old pz tiebreak, computed from live projection). On cap-100
	// saturation this ordering keeps the nearest-screen tile, not first-100.
	{
		double bestDistSq = 1.0e30;
		for (long i = 0; i < currentClosest; i++)
		{
			TerrainQuadPtr t = closestTiles[i];
			Stuff::Point3D corners[4];
			bool valid[4];
			double tileNearestSq = 1.0e30;
			for (int c = 0; c < 4; c++)
			{
				Stuff::Vector3D wp;
				wp.x = t->vertices[c]->vx;
				wp.y = t->vertices[c]->vy;
				wp.z = t->vertices[c]->pVertex->elevation;

				Stuff::Vector4D sc;
				eye->projectForSelectionPicking(wp, sc);
				corners[c].x = sc.x;
				corners[c].y = sc.y;
				corners[c].z = 0.0f;
				valid[c] = (sc.x >= 0.0f) && (sc.y >= 0.0f) &&
				           (sc.x <= screenResolution.x) &&
				           (sc.y <= screenResolution.y) &&
				           (sc.z >= 0.0f) && (sc.z < 1.0f);

				const double dx = (double)sc.x - (double)screenPos.x;
				const double dy = (double)sc.y - (double)screenPos.y;
				const double dsq = dx * dx + dy * dy;
				if (dsq < tileNearestSq)
					tileNearestSq = dsq;
			}

			if (s_overThisTileProjected(corners, valid, screenPos.x, screenPos.y))
			{
				if (tileNearestSq < bestDistSq)
				{
					bestDistSq = tileNearestSq;
					closestTile = t;
				}
			}
		}
	}
	}

	if (closestTile)
	{
#ifdef _DEBUG
		closestTile->selected = TRUE;
		closestTile->vertices[0]->selected = TRUE;
#endif

		//-----------------------------------------------------------------------------
		// Once we have the tile, use the vertex[0] to find out which cell we are in.
		closestVertex = closestTile->vertices[0];

		//-------------------------------------------------------------------
		// Really new method.  Snap to center of closest Cell.  This is ideal
		// because movement should always be to this location and Heidi and Mike need
		// cell resolution in the Editor!
		Stuff::Vector4D cellCenter;

		long cellCenterC = -1;
		long cellCenterR = -1;
		float cellWidth = Terrain::worldUnitsPerCell;
		float halfCellWidth = cellWidth / 2.0f;
		long tileC = -1, tileR = -1;
		long VerticesMapSideDivTwo = Terrain::realVerticesMapSide / 2;
		long MetersMapSideDivTwo = VerticesMapSideDivTwo * float2long(Terrain::worldUnitsPerVertex);
		
		long dx,dy,cd=1<<30,dist,tvx=screenPos.x,tvy=screenPos.y;
		for (long cellC=0;cellC<3;cellC++)
		{
			for (long cellR=0;cellR<3;cellR++)
			{
				point.x = closestVertex->vx;
				point.y = closestVertex->vy;
				point.z = closestVertex->pVertex->elevation;

				point.x += (cellC) * cellWidth + halfCellWidth;
				point.y -= (cellR) * cellWidth + halfCellWidth;
				point.z = land->getTerrainElevation(point);
				// [PROJECTZ:SelectionPicking id=picking_closest_cell_center]
				PROJECTZ_SITE("picking_closest_cell_center", "SelectionPicking");
				eye->projectForSelectionPicking(point,cellCenter);

				dx = (tvx - float2long(cellCenter.x));
				dy = (tvy - float2long(cellCenter.y));
				dist = dx * dx + dy * dy;
				if (dist < cd)
				{
					cd = dist;
					cellCenterC = cellC;
					cellCenterR = cellR;

					tileC = float2long(point.x * Terrain::oneOverWorldUnitsPerVertex + VerticesMapSideDivTwo);
					tileR = float2long((MetersMapSideDivTwo - point.y) * Terrain::oneOverWorldUnitsPerVertex);
				}
			}
		}

		if (tileR < 0)
			tileR = 0;

		if (tileR >= Terrain::realVerticesMapSide)
			tileR = Terrain::realVerticesMapSide-1;

		if (tileC < 0)
			tileC = 0;

		if (tileC >= Terrain::realVerticesMapSide)
			tileC = Terrain::realVerticesMapSide-1;

		mapTileCellToWorldPos (tileR,tileC,cellCenterR,cellCenterC,point);
		point.z = land->getTerrainElevation(point);
	}
	else
	{
	
		// not in any tile, must be off map.  We're going to return the coord of the
		// closest vertex
		float dis = 999999999.f;
		int closeRow = 0, closeCol = 0;
		Stuff::Vector3D tmpWorld;
		Stuff::Vector4D tmpScreen;
		
		/*for ( int column = 0; column < land->realVerticesMapSide; column += land->realVerticesMapSide - 1 )
		{
			for ( int row = 0; row < land->realVerticesMapSide; ++row )
			{
				tmpWorld.x = land->tileColToWorldCoord[column];
				tmpWorld.y = land->tileRowToWorldCoord[row];
				tmpWorld.z = land->getTerrainElevation( row, column );

				projectZ( tmpWorld, tmpScreen );
				
				float tmpDis = (tmpScreen.x - screenPos.x) * (tmpScreen.x - screenPos.x ) + (tmpScreen.y - screenPos.y) * (tmpScreen.y - screenPos.y );
				if ( tmpDis < dis )
				{
					closeRow = row;
					closeCol = column;
					dis = tmpDis;
				}
			}
		}*/

		// MISSION-INTERFACE-PERF-1: this "off map" fallback is the PRODUCTION
		// ground picker whenever the quadList is empty (LOD-chunk terrain path:
		// numTiles=0 -> closestTile never set -> every cache-miss cursor
		// unproject lands here). The legacy loop below brute-force-projects ALL
		// realVerticesMapSide^2 map vertices (getTerrainElevation +
		// projectForSelectionPicking each) = ~890us/frame on mc2_24, 85-95% of
		// the GameLogic.Mission.Interface zone ([IFACE_PERF v1] 2026-07-01).
		//
		// MC2_PICK_FALLBACK_COARSE=1 (default OFF): coarse-to-fine argmin over
		// the same screen-distance field -- stride-8 coarse pass, then full-res
		// refine in a +/-9 window around the coarse winner. Same
		// nearest-projected-vertex result in practice (the screen-distance
		// basin spans many vertices at any playable zoom); the fallback is
		// already vertex-resolution approximate (return 1 below). With
		// MC2_PICK_CAP_TRACE=1 also set, every 32nd walk re-runs the full
		// brute force and logs a [PICK_FALLBACK] parity line.
		static const bool s_fallbackCoarse =
			(std::getenv("MC2_PICK_FALLBACK_COARSE") != nullptr);

		const long side = land->realVerticesMapSide;
		auto sampleVertex = [&](long row, long col, float& bestDis,
		                        int& bestRow, int& bestCol) {
			tmpWorld.x = land->tileColToWorldCoord[col];
			tmpWorld.y = land->tileRowToWorldCoord[row];
			tmpWorld.z = land->getTerrainElevation( row, col );

			// [PROJECTZ:SelectionPicking id=picking_closest_vertex_fallback]
			PROJECTZ_SITE("picking_closest_vertex_fallback", "SelectionPicking");
			projectForSelectionPicking( tmpWorld, tmpScreen );

			float tmpDis = (tmpScreen.x - screenPos.x) * (tmpScreen.x - screenPos.x ) + (tmpScreen.y - screenPos.y) * (tmpScreen.y - screenPos.y );
			if ( tmpDis < bestDis )
			{
				bestRow = (int)row;
				bestCol = (int)col;
				bestDis = tmpDis;
			}
		};

		if (s_fallbackCoarse)
		{
			// Coarse pass: stride 8, plus the last row/col so map edges are
			// always sampled regardless of side % 8.
			const long stride = 8;
			for ( long column = 0; column < side; column += stride )
				for ( long row = 0; row < side; row += stride )
					sampleVertex(row, column, dis, closeRow, closeCol);
			if ((side - 1) % stride != 0)
			{
				for ( long row = 0; row < side; row += stride )
					sampleVertex(row, side - 1, dis, closeRow, closeCol);
				for ( long column = 0; column < side; column += stride )
					sampleVertex(side - 1, column, dis, closeRow, closeCol);
				sampleVertex(side - 1, side - 1, dis, closeRow, closeCol);
			}
			// Full-res refine around the coarse winner.
			const long r0 = (closeRow - 9 > 0) ? (closeRow - 9) : 0;
			const long r1 = (closeRow + 9 < side - 1) ? (closeRow + 9) : (side - 1);
			const long c0 = (closeCol - 9 > 0) ? (closeCol - 9) : 0;
			const long c1 = (closeCol + 9 < side - 1) ? (closeCol + 9) : (side - 1);
			for ( long column = c0; column <= c1; column++ )
				for ( long row = r0; row <= r1; row++ )
					sampleVertex(row, column, dis, closeRow, closeCol);

			// Sampled parity vs the legacy brute force (every 32nd walk,
			// only when the pre-existing pick trace is also enabled).
			if (s_capTrace)
			{
				static unsigned long s_fbWalk = 0;
				if ((++s_fbWalk & 31) == 0)
				{
					float bfDis = 999999999.f;
					int bfRow = 0, bfCol = 0;
					for ( long column = 0; column < side; column++ )
						for ( long row = 0; row < side; row++ )
							sampleVertex(row, column, bfDis, bfRow, bfCol);
					fprintf(stderr, "[PICK_FALLBACK] parity=%s coarse=(%d,%d) brute=(%d,%d) "
						"coarse_dsq=%.1f brute_dsq=%.1f cursor=(%ld,%ld)\n",
						(bfRow == closeRow && bfCol == closeCol) ? "ok" : "DIFF",
						closeRow, closeCol, bfRow, bfCol, dis, bfDis,
						(long)screenPos.x, (long)screenPos.y);
					// Keep the brute-force answer on divergence (trace mode only).
					if (bfDis < dis) { dis = bfDis; closeRow = bfRow; closeCol = bfCol; }
				}
			}
		}
		else
		{
			for ( long column = 0; column < side; column ++ )
			{
				for ( long row = 0; row < side; row++/*row += land->realVerticesMapSide - 1*/ )
				{
					sampleVertex(row, column, dis, closeRow, closeCol);
				}
			}
		}

		point.x = land->tileColToWorldCoord[closeCol];
		point.y = land->tileRowToWorldCoord[closeRow];
		point.z = land->getTerrainElevation( closeRow, closeCol );

		if (s_pickParity && s_lodChunkPick)
			return parityReport(screenPos, parityRaycastHit, parityRaycastPt, point, 1);

		return 1; // don't return success for bogus value
	}

	if (s_pickParity && s_lodChunkPick)
		return parityReport(screenPos, parityRaycastHit, parityRaycastPt, point, 0);

	return(0);
}


//---------------------------------------------------------------------------
void Camera::updateGoalVelocity (void)
{
	if (goalVelTime > 0.0)
	{
		Stuff::Vector3D velDiff;
		velDiff.Subtract(goalVelocity,velocity);
		velDiff /= goalVelTime;
		velDiff *= frameLength;
		goalVelTime -= frameLength;

		if (goalVelTime < 0.0)
			velocity = goalVelocity;
		else
			velocity.Add(velocity,velDiff);
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
void Camera::updateGoalPosition (Stuff::Vector3D &pos)
{
	if (goalPosTime > 0.0)
	{
		Stuff::Vector3D posDiff;
		posDiff.Subtract(goalPosition,pos);
		//sebi
		//posDiff /= goalPosTime;
		//posDiff *= frameLength;
		posDiff *= frameLength / goalPosTime;
		goalPosTime -= frameLength;

		if (goalPosTime < 0.0f)
			pos = goalPosition;
		else
			pos.Add(position,posDiff);
	}
	else
	{
		goalPosTime = 0.0;
		goalPosition = pos;
	}
}

//---------------------------------------------------------------------------
static const float rhatx = 1.0; //cos(0/*degrees*/ * DEGREES_TO_RADS);
static const float rhaty = 0.0; //sin(0/*degrees*/ * DEGREES_TO_RADS);
static const float ghatx = -0.5; //cos(120/*degrees*/ * DEGREES_TO_RADS);
static const float ghaty = 0.86602540378443864676372317075294f; //sin(120/*degrees*/ * DEGREES_TO_RADS);
static const float bhatx = -0.5; //cos(240/*degrees*/ * DEGREES_TO_RADS);
static const float bhaty = -0.86602540378443864676372317075294f; //sin(240/*degrees*/ * DEGREES_TO_RADS);
static const float two_pi = 6.283185307179586476925286766559f;

/* all params range from 0.0 to 1.0 */
static void rgb2hsi(float r, float g, float b, float &hue, float &saturation, float &intensity)
{
	intensity = (r + g + b) / 3.0;

	if (0.0 >= intensity) {
		saturation = 0.0;
		hue = 0.0;
		return;
	}

	float min = r;
	if (g < min) { min = g; }
	if (b < min) { min = b; }

	saturation = 1.0 - (min / intensity);

	float cx = r * rhatx + g * ghatx + b * bhatx;
	float cy = r * rhaty + g * ghaty + b * bhaty;

	hue = atan2(cy, cx);
	if (0.0 > hue) { hue += two_pi; }
	hue /= two_pi;
}

static void hsi2rgb(float hue, float saturation, float intensity, float &r, float &g, float &b)
{
	float thue = hue;

	if ((1.0f / 3.0f) > hue) {
	} else if ((2.0f / 3.0f) > hue) {
		thue -= 1.0f / 3.0f;
	} else {
		thue -= 2.0f / 3.0f;
	}

	float chatx = cos(two_pi * thue);
	float chaty = sin(two_pi * thue);
	float ti = 3.0f * intensity;

	{
		b = (1.0f - saturation) * intensity;
		float tib = ti - b;
		float denominator = (chaty*(ghatx - rhatx) - chatx*(ghaty - rhaty));
		if (0.0f == denominator) {
			/* I suspect it's mathematically impossible to get here, but I'm too lazy to make sure. */
			denominator = 0.000001f;
		}
		g = ((b*bhaty + tib*rhaty)*chatx - (b*bhatx + tib*rhatx)*chaty) / denominator;
		r = tib - g;
	}

	if ((1.0f / 3.0f) > hue) {
	} else if ((2.0f / 3.0f) > hue) {
		float swap = b;
		b = g;
		g = r;
		r = swap;
	} else {
		float swap = b;
		b = r;
		r = g;
		g = swap;
	}
}

/* interpolation with dy/dx == 0 at x = 0 and x = 1 */
float smoothConnectingInterpolation(float y0, float y1, float x)
{
	float a = 0.5 * (y0 - y1);
	float b = 0.5 * (y0 + y1);
	float retval = a * cos(x * (0.5 * two_pi)) + b;
	return retval;
}

void Camera::updateDaylight (bool bInitialize)
{
	float day2NightTransitionTime = this->day2NightTransitionTime;
	if (bInitialize) {
		if (day2NightTransitionTime <= 0.0f) {
			day2NightTransitionTime = 3600.0f/*seconds*/;
		}
		dayLightTime = dayLightPitch / 360.0f/*degrees*/ * day2NightTransitionTime;
	}

	if (day2NightTransitionTime > 0.0f)
	{
		while (dayLightTime > day2NightTransitionTime) {
			dayLightTime -= day2NightTransitionTime;
		}
		
		float oneOverDay2NightTransitionTime = 1.0f / day2NightTransitionTime;

		lightPitch = dayLightTime * oneOverDay2NightTransitionTime * 360.0/*degrees*/;

		/* arbitrary assumed daytime sky color */
		static const float fDaySkyRed = 193.0f;
		static const float fDaySkyGreen = 231.0f;
		static const float fDaySkyBlue = 238.0f;

		float fSkyRed;
		float fSkyGreen;
		float fSkyBlue;

		float fAmbientRed;
		float fAmbientGreen;
		float fAmbientBlue;

		float fLightRed;
		float fLightGreen;
		float fLightBlue;

		if (180.0f/*degrees*/ >= lightPitch) 
		{
		 	//Set flags to help renderer with light sources.
			float lightAltitude = lightPitch;
			if (lightPitch > 90.0f)
				lightAltitude = 180.0f - lightPitch;
				
			isNight = false;
			if (lightAltitude < NIGHT_LIGHT_PITCH)
				isNight = true;
				
			if (lightAltitude > NIGHT_START_PITCH)
			{
				nightFactor = 0.0f;
			}
			else
			{
				nightFactor = (NIGHT_START_PITCH - lightAltitude) / (NIGHT_START_PITCH - NIGHT_LIGHT_PITCH);
				if (nightFactor > 1.0f)
					nightFactor = 1.0f;
			}
		
			/* daytime */
			float fSineOfPitch = sin(lightPitch * DEGREES_TO_RADS);

			/* these calculations are sort of based on optics */
			//Do not let day ambient go darker then nightAmbient for smooth transitions.
			fAmbientRed = (dayAmbientRed - nightAmbientRed) * fSineOfPitch + nightAmbientRed;
			fAmbientGreen = (dayAmbientGreen - nightAmbientGreen) * fSineOfPitch + nightAmbientGreen;
			fAmbientBlue = (dayAmbientBlue - nightAmbientBlue) * fSineOfPitch + nightAmbientBlue;

			/* these sunset color calculations are not based on optics*/
			float fAngleFromTheHorizon = 90.0/*degrees*/ - fabs(90.0/*degrees*/ - lightPitch);
			/*static*/ const float fSunSetStartAngle = 35.0/*degrees*/;
			if (fSunSetStartAngle > fAngleFromTheHorizon) 
			{
				const float fSunSetRed = sunsetLightRed;
				const float fSunSetGreen = sunsetLightGreen;
				const float fSunSetBlue = sunsetLightBlue;
				float fSunSetHue;
				float fSunSetSaturation;
				float fSunSetIntensity;
				rgb2hsi(fSunSetRed / 255.0f, fSunSetGreen / 255.0f, fSunSetBlue / 255.0f
					, fSunSetHue, fSunSetSaturation, fSunSetIntensity);

				float fDayLightHue;
				float fDayLightSaturation;
				float fDayLightIntensity;
				rgb2hsi(dayLightRed / 255.0f, dayLightGreen / 255.0f, dayLightBlue / 255.0f
					, fDayLightHue, fDayLightSaturation, fDayLightIntensity);

				float fSunSetProportion = 0.75/*arbitrary*/ * sin((fSunSetStartAngle - fAngleFromTheHorizon) * (90.0f/*degrees*/ / fSunSetStartAngle) * DEGREES_TO_RADS);
				float fOneMinusSunSetProportion = 1.0f - fSunSetProportion;

				/* for sunlight color, linear interpolation in HSI color space is a better
				model than linear interpolation in RGB color space */
				float H = fSunSetHue * fSunSetProportion + fDayLightHue * fOneMinusSunSetProportion;
				float S = fSunSetSaturation * fSunSetProportion + fDayLightSaturation * fOneMinusSunSetProportion;
				float I = fSunSetIntensity * fSunSetProportion + fDayLightIntensity * fOneMinusSunSetProportion;
				float R, G, B;
				hsi2rgb(H, S, I, R, G, B);
				if (R > 0.99f) { R = 0.99f; }
				if (G > 0.99f) { G = 0.99f; }
				if (B > 0.99f) { B = 0.99f; }
				if (R < 0.0f) { R = 0.0f; }
				if (G < 0.0f) { G = 0.0f; }
				if (B < 0.0f) { B = 0.0f; }
				fLightRed = R * 255.0f;
				fLightGreen = G * 255.0f;
				fLightBlue = B * 255.0f;


				/* Basically I'm using an arbitrary harcoded formula (which may need some
				tweaking) to approximate the sky color as it progresses to sunset. For more
				accurate results it would probably be better to store a table of colors obtained
				from photographs. */

				fSunSetProportion = sin((fSunSetStartAngle - fAngleFromTheHorizon) * (90.0f/*degrees*/ / fSunSetStartAngle) * DEGREES_TO_RADS);
				float fSunAzimuth = lightYaw;
				if (90.0/*degrees*/ < lightPitch) {
					fSunAzimuth += 180.0/*degress*/;
				}
				float fAzimuthCameraAngleRelativeToTheSun = fmod(fabs(fSunAzimuth - (getCameraRotation() - 135.0)), 360.0/*degrees*/);
				if (180.0/*degress*/ < fAzimuthCameraAngleRelativeToTheSun) {
					fAzimuthCameraAngleRelativeToTheSun = 360.0/*degrees*/ - fAzimuthCameraAngleRelativeToTheSun;
				}
				fSunSetProportion *= (1.0 - pow(fAzimuthCameraAngleRelativeToTheSun / 180.0/*degress*/, 2.0/*arbitrary*/));
				fOneMinusSunSetProportion = 1.0 - fSunSetProportion;

				float fDaySkyHue;
				float fDaySkySaturation;
				float fDaySkyIntensity;
				rgb2hsi(fDaySkyRed / 255.0f, fDaySkyGreen / 255.0f, fDaySkyBlue / 255.0f
					, fDaySkyHue, fDaySkySaturation, fDaySkyIntensity);
				H = fSunSetHue * fSunSetProportion + fDaySkyHue * fOneMinusSunSetProportion;
				/*static*/ const float fGreenHue = 1.0f / 3.0f;
				float za;
				if (fGreenHue > H) {
					/*static*/ const float fGreenHueMargin = 1.0 / 6.0 + 0.12/*arbitrary*/;
					if ((fGreenHue - fGreenHueMargin) > H) {
						za = 0.0;
					} else {
						za = smoothConnectingInterpolation(0, 1, (H - (fGreenHue - fGreenHueMargin)) / fGreenHueMargin);
						za = pow(fabs(za), 0.15f/*arbitrary*/);
					}
				} else {
					/*static*/ const float fGreenHueMargin = 1.0 / 6.0 + 0.00/*arbitrary*/;
					if ((fGreenHue + fGreenHueMargin) < H) {
						za = 0.0;
					} else {
						za = smoothConnectingInterpolation(0, 1, ((fGreenHue + fGreenHueMargin) - H) / fGreenHueMargin);
						za = pow(fabs(za), 0.15f/*arbitrary*/);
					}
				}
				S = fSunSetSaturation * fSunSetProportion + fDaySkySaturation * fOneMinusSunSetProportion;
				S = 0.0f/*arbitrary*/ * za + S * (1.0f - za);
				/*static*/ const float brightHue = 0.25 * 1.0 / 3.0; /*arbitrary*/
				float zb;
				if (brightHue > H) {
					if (brightHue != fSunSetHue) {
						zb = sin((H - fSunSetHue) / (brightHue - fSunSetHue) * 0.35f * two_pi);
					} else {
						zb = 0.0;
					}
				} else {
					zb = sin((fDaySkyHue - H) / (fDaySkyHue - brightHue) * 0.35f * two_pi);
				}
				zb = pow(fabs(zb), 1.0f);
				I = fSunSetIntensity * fSunSetProportion + fDaySkyIntensity * fOneMinusSunSetProportion;
				I = 0.85f/*arbitrary*/ * zb + I * (1.0f - zb);
				I *= (1.0 - (fAzimuthCameraAngleRelativeToTheSun / 180.0/*degress*/) * (1.0 - (fAngleFromTheHorizon / fSunSetStartAngle)));
				hsi2rgb(H, S, I, R, G, B);
				if (R > 0.99f) { R = 0.99f; }
				if (G > 0.99f) { G = 0.99f; }
				if (B > 0.99f) { B = 0.99f; }
				if (R < 0.0f) { R = 0.0f; }
				if (G < 0.0f) { G = 0.0f; }
				if (B < 0.0f) { B = 0.0f; }
				fSkyRed = R * 255.0f;
				fSkyGreen = G * 255.0f;
				fSkyBlue = B * 255.0f;

				/*static*/ const float fFadeToDarknessStartAngle  = 5.0/*degrees*/;
				if (fFadeToDarknessStartAngle > fAngleFromTheHorizon) 
				{
					float fOneMinusDarknessProportion = sin((fAngleFromTheHorizon) * (90.0f/*degrees*/ / fFadeToDarknessStartAngle) * DEGREES_TO_RADS);
					float fDarknessProportion = 1.0f - fOneMinusDarknessProportion;

					fLightRed = fLightRed * fOneMinusDarknessProportion + nightLightRed * fDarknessProportion;
					fLightGreen = fLightGreen * fOneMinusDarknessProportion + nightLightGreen * fDarknessProportion;
					fLightBlue = fLightBlue * fOneMinusDarknessProportion + nightLightBlue * fDarknessProportion;

					fSkyRed = fSkyRed * fOneMinusDarknessProportion;
					fSkyGreen = fSkyGreen * fOneMinusDarknessProportion;
					fSkyBlue = fSkyBlue * fOneMinusDarknessProportion;
				}
			}
			else 
			{
				fLightRed = dayLightRed;
				fLightGreen = dayLightGreen;
				fLightBlue = dayLightBlue;

				fSkyRed = fDaySkyRed;
				fSkyGreen = fDaySkyGreen;
				fSkyBlue = fDaySkyBlue;
			}
		} 
		else 
		{
		 	//Set flags to help renderer with light sources.
			isNight = true;
			nightFactor = 1.0f;	
			
			/* nighttime */
			float moonPitch = 180.0f/*degrees*/ - fabs(180.0f/*degrees*/ - lightPitch);
			lightPitch = moonPitch;

			//float fSineOfPitch = sin(moonPitch * DEGREES_TO_RADS);

			/* these calculations are sort of based on optics */
			// DO NOT ever get darker then the night Ambient.  Change hue with moon.
			// Not realistic but very unplayable in the pitch dark.
			fAmbientRed = nightAmbientRed;// * fSineOfPitch;
			fAmbientGreen = nightAmbientGreen;// * fSineOfPitch;
			fAmbientBlue = nightAmbientBlue;// * fSineOfPitch;

			fLightRed = nightLightRed;
			fLightGreen = nightLightGreen;
			fLightBlue = nightLightBlue;

			fSkyRed = 0.0;
			fSkyGreen = 0.0;
			fSkyBlue = 0.0;
		}

		ambientRed = fAmbientRed;
		ambientBlue = fAmbientBlue;
		ambientGreen = fAmbientGreen;
		
		lightRed = fLightRed;
		lightGreen = fLightGreen;
		lightBlue = fLightBlue;

		Stuff::LinearMatrix4D lightToWorldMatrix;
		lightToWorldMatrix.BuildTranslation(Stuff::Point3D(0.0f,0.0f,0.0f));
		lightToWorldMatrix.BuildRotation(Stuff::EulerAngles(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));
		worldLights[0]->SetLightToWorld(&lightToWorldMatrix);

		/* In the real world, fog color is generally varies with the ambient color, but in our
		game fog color more reflects "sky color near the horizon" */

		float fFogTransparency = fogTransparency;
		float fOpaqueDayFogRed = (float)((dayFogColor >> 16) & 0xff);
		float fOpaqueDayFogGreen = (float)((dayFogColor >> 8) & 0xff);
		float fOpaqueDayFogBlue = (float)((dayFogColor) & 0xff);

		static const float fBlackSkyLevel = 32.0f; /* If the light is below this level, the sky is effectively black. */

		//float fFogRed = fLightRed - fBlackSkyLevel;
		//float fFogGreen = fLightGreen - fBlackSkyLevel;
		//float fFogBlue = fLightBlue - fBlackSkyLevel;
		float fFogRed = fAmbientRed - fBlackSkyLevel;
		float fFogGreen = fAmbientGreen - fBlackSkyLevel;
		float fFogBlue = fAmbientBlue - fBlackSkyLevel;
		if (0.0f > fFogRed) { fFogRed = 0.0f; }
		if (0.0f > fFogGreen) { fFogGreen = 0.0f; }
		if (0.0f > fFogBlue) { fFogBlue = 0.0f; }

		//float fTmpRed = (float)(dayLightRed) - fBlackSkyLevel;
		//float fTmpGreen = (float)(dayLightGreen) - fBlackSkyLevel;
		//float fTmpBlue = (float)(dayLightBlue) - fBlackSkyLevel;
		float fTmpRed = (float)(dayAmbientRed) - fBlackSkyLevel;
		float fTmpGreen = (float)(dayAmbientGreen) - fBlackSkyLevel;
		float fTmpBlue = (float)(dayAmbientBlue) - fBlackSkyLevel;
		float epsilon = 1.0;
		if (epsilon > fTmpRed) { fTmpRed = epsilon; }
		if (epsilon > fTmpGreen) { fTmpGreen = epsilon; }
		if (epsilon > fTmpBlue) { fTmpBlue = epsilon; }

		float fOneMinusFogTransparency = 1.0 - fFogTransparency;
		fFogRed = fFogRed / fTmpRed * fOpaqueDayFogRed * fOneMinusFogTransparency;
		fFogGreen = fFogGreen / fTmpGreen * fOpaqueDayFogGreen * fOneMinusFogTransparency;
		fFogBlue = fFogBlue / fTmpBlue * fOpaqueDayFogBlue * fOneMinusFogTransparency;

		/* Here we add in the sky color component. */
		fFogRed = fFogRed + fFogTransparency * fSkyRed;
		fFogGreen = fFogGreen + fFogTransparency * fSkyGreen;
		fFogBlue = fFogBlue + fFogTransparency * fSkyBlue;

		unsigned int fogRed = fFogRed;
		unsigned int fogGreen = fFogGreen;
		unsigned int fogBlue = fFogBlue;
		if (0xff < fogRed) { fogRed = 0xff; }
		if (0xff < fogGreen) { fogGreen = 0xff; }
		if (0xff < fogBlue) { fogBlue = 0xff; }
		fogColor = (fogRed << 16) + (fogGreen << 8) + fogBlue;

		dayLightTime += frameLength;
	}
}

//---------------------------------------------------------------------------
void Camera::updateLetterboxAndFade (void)
{
	//OK, if we are inMovieMode and we haven't finished letterboxing, finish it!
	if (!startEnding && inMovieMode && (letterBoxTime != MaxLetterBoxTime))
	{
		letterBoxTime += frameLength;
		float letterBoxPercent = letterBoxTime / MaxLetterBoxTime;
		if (letterBoxTime > MaxLetterBoxTime)
		{
			letterBoxPercent = 1.0f;
			letterBoxTime = MaxLetterBoxTime;
		}
		
		letterBoxPos = letterBoxPercent * MaxLetterBoxPos;
		letterBoxAlpha = (letterBoxPercent * 255.0f);
	}
	
	//Now, if we are in startEnding and we haven't finished DE-Letterboxing, finish it.
	if (startEnding && inMovieMode && (letterBoxPos != 0.0f))
	{
		letterBoxTime -= frameLength;
		float letterBoxPercent = letterBoxTime / MaxLetterBoxTime;
		//sebi: if letterBoxTime = 0 then letterBoxPercent also = 0 and then letterBoxPos = 0
		// see (*) and that means that on next iteration this condition will be false
		// but inMovieMode and startEnding will not be updated, causeing infinit stay in movie mode 
		if (letterBoxTime <= 0.0f)
		//if (letterBoxTime < 0.0f)
		{
			letterBoxPercent = 0.0f;
			inMovieMode = false;
			startEnding = false;
		}
		
		letterBoxPos = letterBoxPercent * MaxLetterBoxPos; // (*)
		letterBoxAlpha = (letterBoxPercent * 255.0f);
	}
	
	//If we are fading toward a color, continue the fade.
	// We only fades during movie Mode at present!!
	if (inMovieMode && inFadeMode)
	{
		timeLeftToFade += frameLength;
		float fadePercent = timeLeftToFade / startFadeTime;
		if (timeLeftToFade > startFadeTime)
		{
			fadePercent = 1.0f;
			inFadeMode = false;
		}
		
		fadeAlpha = (fadeStart >> 24) + (fadePercent * (float(fadeColor >> 24) - float(fadeStart >> 24)));
	}
}

//---------------------------------------------------------------------------
long Camera::update (void)
{
	//---------------------------------------------------------
	// This is the guts.  This routine will be used to
	// move the camera, cut to different shots, etc.

	switch (cameraClass)
	{
		case BASE_CAMERA:
			break;
			
		default:
			return INVALID_CAMERA;
	}

	float oldNightFactor = nightFactor;
	
	//updateDaylight();
	
	//----------------------------------------------------
	// Check if light moved.
	// Should not be necessary every frame.
	// TEST FOR NOW!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	lightDirection.x = cos((lightYaw + 45.0f) * DEGREES_TO_RADS);
	lightDirection.y = sin((lightYaw + 45.0f) * DEGREES_TO_RADS);
	lightDirection.z = sin(lightPitch * DEGREES_TO_RADS);

	lightDirection.Normalize(lightDirection);

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
	Stuff::Vector3D newPosition = position;
	
	//------------------
	// Velocity Update
	updateGoalVelocity();
	
	if ((velocity.x != 0.0) &&
		(velocity.y != 0.0) &&
		(velocity.z != 0.0))
	{
		Stuff::Vector3D posOffset;
		posOffset = velocity;
		posOffset *= frameLength;
		
		newPosition += posOffset;
	}
	
	updateGoalPosition(newPosition);
	updateGoalRotation();
	updateGoalFOV();
	
	setPosition(newPosition);		//THIS ALWAYS sets z position for us now!!!!!

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

	// Smooth zoom: lerp cameraAltitude toward cameraAltitudeDesired each frame.
	// Also decay scroll momentum (used for adaptive scroll-speed zoom).
	{
		s_scrollMomentum -= frameLength * 4.0f;
		if (s_scrollMomentum < 0.0f) s_scrollMomentum = 0.0f;

		float diff = cameraAltitudeDesired - cameraAltitude;
		if (fabsf(diff) > 0.1f)
			cameraAltitude += diff * (frameLength * 20.0f < 1.0f ? frameLength * 20.0f : 1.0f);
		else
			cameraAltitude = cameraAltitudeDesired;
		if (cameraAltitude < AltitudeMinimum)
			cameraAltitude = AltitudeMinimum;
		float _ap = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
		float _tm = AltitudeMaximumLo + ((AltitudeMaximumHi - AltitudeMaximumLo) * _ap);
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
		if (frac > 1.0f) frac = 1.0f;
		if (frac < -1.0f) frac = -1.0f;
		Stuff::Vector3D Tnew = zoomAnchorPivot;
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
		// F3 CPU projection cost-baseline: matrix_build site (b) —
		// per-frame TG_Shape::SetCameraMatrices composes s_worldToClip.
		::mc2_cpu_proj_cost::Scope _f3_setcam_scope(
		    ::mc2_cpu_proj_cost::BUCKET_MATRIX_BUILD);
		TG_Shape::SetCameraMatrices(&cameraOrigin,&cameraToClip);
	}
	TG_Shape::SetFog(fogColor,fogStart,fogFull);

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
	// Both thresholds must hold for "stationary" — translation OR rotation
	// counts as motion, since either invalidates lagged-visibility data.
	{
		ZoneScopedN("Camera.MotionTracking");
		static bool   s_motionInit       = false;
		static float  s_prevRotation     = 0.0f;
		static Stuff::Vector3D s_prevPosition;  // default-ctor zeroed; actual prev seeded on first call
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
			if (dRot < 0.0f) dRot = -dRot;
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

			if ((s_motionFrames % SUMMARY_PERIOD) == 0u) {
				printf("[CAMERA_MOTION v1] event=summary frames=%u stationary=%u moving=%u "
				       "stationary_pct=%.1f threshold_deg=%.2f threshold_pos=%.2f\n",
				       s_motionFrames, s_motionStationary, s_motionMoving,
				       (s_motionFrames > 0u)
				           ? (100.0f * (float)s_motionStationary / (float)s_motionFrames)
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

	TG_Shape::SetViewport(viewMulX,viewMulY,viewAddX,viewAddY);

	userInput->setViewport(viewMulX,viewMulY,viewAddX,viewAddY);
	
	//--------------------------------------------------------
	// Get new viewport values to scale stuff.  No longer uses
	// VFX stuff for this.  ALL GOS NOW!
	screenResolution.x = viewMulX;
	screenResolution.y = viewMulY;
	calculateProjectionConstants();

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

		craterManager->render();					//render the craters and footprints

		//Only the GameCamera knows about this.  Heidi, override this function in EditorCamera
		//and have your objectManager draw.
//		ObjectManager->render(true, true, true);	//render all other objects
	}

	//-----------------------------------------------------
}

//---------------------------------------------------------------------------
// T1.15 [SPOT_DIAG v1] file-scope state for the camera-overwrite probe.
// First-seen-per-slot is always-on (one stderr line per unique slot index seen
// here); per-summary is env-gated every 600 frames.
static const bool s_spotDiagCamEnabled = (getenv("MC2_SPOT_DIAG") != nullptr);
static std::vector<bool> s_spotDiagCamSeenSlot;  // sized to MAX_LIGHTS_IN_WORLD lazily
static unsigned long s_spotDiagCamFrames        = 0;
static unsigned long s_spotDiagCamPointSeen     = 0;  // window: distinct POINT slots seen
static unsigned long s_spotDiagCamActiveTrue    = 0;  // window: active_after=true counts
static unsigned long s_spotDiagCamActiveFalse   = 0;  // window: active_after=false counts

// T1.16 — per-slot active-state tracker for (E)-owned slots only.
// Sentinel 0xFFu == "never seen yet" (no transition emitted on first sample).
static std::vector<unsigned char> s_spotDiagT16LastActive;

void Camera::updateLights()
{
	//---------------------------------------------------------------------------------
	// Check which lights are on screen and deactivate those which are NOT on screen!
	numActiveLights = numTerrainLights = 0;
	++s_spotDiagCamFrames;  // T1.15 frame tick (updateLights is once-per-frame)
	if (s_spotDiagCamSeenSlot.empty()) s_spotDiagCamSeenSlot.assign(MAX_LIGHTS_IN_WORLD, false);
	for (long i=0;i<MAX_LIGHTS_IN_WORLD;i++)
	{
		if (worldLights[i])
		{
			TG_LightPtr light = worldLights[i];
			
			// IF we are a terrain Light, we are ONLY active when day changes to Night or vice/versa.
			// On screen matters not.
			if (light->lightType == TG_LIGHT_TERRAIN)
			{
				Stuff::Vector4D dummy;
				// [PROJECTZ:LightingShadow id=light_terrain_active_test]
				PROJECTZ_SITE("light_terrain_active_test", "LightingShadow");
				light->active = projectForLightingShadow(light->position,dummy);
				if (light->active)
				{
					if (terrainLightCalc)
						activeLights[numActiveLights++] = light;
					terrainLights[numTerrainLights++] = light;
				}
				continue;			
			}
				
			//If we are infinite or Ambient, we MUST be active
			if ((light->lightType == TG_LIGHT_AMBIENT)  ||
				(light->lightType == TG_LIGHT_INFINITE))
			{
				light->active = true;
				activeLights[numActiveLights++] = light;
				terrainLights[numTerrainLights++] = light;
				continue;			   
			}
			
			// IF we are a Spot or Point light, we must know if we are onscreen.
			// This is complicated because these lights have a radius!
			// For now, simple position check.  Make complicated later!
			if (light->lightType >= TG_LIGHT_POINT && light->lightType < TG_LIGHT_TERRAIN)
			{
				Stuff::Vector4D dummy;
				// [PROJECTZ:LightingShadow id=light_spot_point_active_test]
				PROJECTZ_SITE("light_spot_point_active_test", "LightingShadow");
				light->active = projectForLightingShadow(light->position,dummy);
				// T1.16 [SPOT_DIAG v1] per-slot transition probe — emit ONLY for
				// (E)-owned slots, state-change-on-transition. Per-summary
				// snapshot below at end of updateLights() every 600 frames.
				if (mc2_spotlight_diag::is_enabled()) {
					mc2_spotlight_diag::SourceClass src;
					if (mc2_spotlight_diag::is_e_slot(i, &src)) {
						if (s_spotDiagT16LastActive.size() < (size_t)MAX_LIGHTS_IN_WORLD)
							s_spotDiagT16LastActive.assign(MAX_LIGHTS_IN_WORLD, 0xFFu);
						const unsigned char curActive = light->active ? 1u : 0u;
						const unsigned char prev = s_spotDiagT16LastActive[i];
						if (prev != 0xFFu && prev != curActive) {
							const char* srcName =
								(src == mc2_spotlight_diag::Bldg ? "bldg" :
								 src == mc2_spotlight_diag::Mech ? "mech" : "gv");
							std::fprintf(stderr,
								"[SPOT_DIAG v1] event=our_slot_transition slot=%ld src=%s "
								"frame=%lu light_pos=(%.1f, %.1f, %.1f) active=%u prev=%u\n",
								i, srcName, s_spotDiagCamFrames,
								(float)light->position.x, (float)light->position.y, (float)light->position.z,
								(unsigned)curActive, (unsigned)prev);
							std::fflush(stderr);
						}
						s_spotDiagT16LastActive[i] = curActive;
					}
				}
				// T1.15 [SPOT_DIAG v1] camera-overwrite probe.
				// First-hit per slot index — always-on, one stderr line per
				// distinct slot ever seen here. Window counters tally
				// active_after true/false for periodic summary.
				if ((size_t)i < s_spotDiagCamSeenSlot.size() && !s_spotDiagCamSeenSlot[i]) {
					s_spotDiagCamSeenSlot[i] = true;
					const char* tname = (light->lightType == TG_LIGHT_POINT) ? "POINT" : "SPOT";
					std::fprintf(stderr,
						"[SPOT_DIAG v1] event=overwrite_first_seen slot=%ld type=%s "
						"pos=(%.2f,%.2f,%.2f) active_after=%d\n",
						i, tname,
						(float)light->position.x, (float)light->position.y, (float)light->position.z,
						light->active ? 1 : 0);
					std::fflush(stderr);
					++s_spotDiagCamPointSeen;
				}
				if (light->active) ++s_spotDiagCamActiveTrue;
				else               ++s_spotDiagCamActiveFalse;
				activeLights[numActiveLights++] = light;
				terrainLights[numTerrainLights++] = light;
			}
		}
	}
	// T1.15 [SPOT_DIAG v1] per-summary overwrite aggregate (env-gated).
	if (s_spotDiagCamEnabled && (s_spotDiagCamFrames % 600) == 0) {
		unsigned long total = s_spotDiagCamActiveTrue + s_spotDiagCamActiveFalse;
		double truePct  = (total > 0) ? (100.0 * (double)s_spotDiagCamActiveTrue  / (double)total) : 0.0;
		double falsePct = (total > 0) ? (100.0 * (double)s_spotDiagCamActiveFalse / (double)total) : 0.0;
		std::fprintf(stderr,
			"[SPOT_DIAG v1] event=overwrite_summary frames=%lu point_slots_seen=%lu "
			"active_true_pct=%.2f active_false_pct=%.2f samples=%lu\n",
			s_spotDiagCamFrames, s_spotDiagCamPointSeen, truePct, falsePct, total);
		std::fflush(stderr);
		// Reset window counters (per-summary cadence per task spec)
		s_spotDiagCamActiveTrue  = 0;
		s_spotDiagCamActiveFalse = 0;
		// T1.16 — per-slot summary: one stderr line per tagged (E)-owned slot.
		// Periodic ground-truth snapshot (every 600 frames) without per-frame
		// line volume. Walks MAX_LIGHTS_IN_WORLD; emits only for slots that
		// is_e_slot() recognizes as tagged.
		for (long si = 0; si < MAX_LIGHTS_IN_WORLD; ++si) {
			mc2_spotlight_diag::SourceClass src;
			if (mc2_spotlight_diag::is_e_slot(si, &src)) {
				TG_LightPtr l = worldLights[si];
				const char* srcName =
					(src == mc2_spotlight_diag::Bldg ? "bldg" :
					 src == mc2_spotlight_diag::Mech ? "mech" : "gv");
				std::fprintf(stderr,
					"[SPOT_DIAG v1] event=our_slot_summary frame=%lu slot=%ld src=%s "
					"active=%u pos=(%.1f, %.1f, %.1f)\n",
					s_spotDiagCamFrames, si, srcName,
					l ? (unsigned)(l->active ? 1u : 0u) : 0xFFu,
					l ? (float)l->position.x : 0.0f,
					l ? (float)l->position.y : 0.0f,
					l ? (float)l->position.z : 0.0f);
				std::fflush(stderr);
			}
		}
	}
}

//---------------------------------------------------------------------------
long Camera::activate (void)
{
	//------------------------------------------
	// If camera is already active, just return
	if (ready && active)
	{
		updateDaylight(true);
		return(NO_ERR);
	}
	
 	return NO_ERR;
}

float zero = 0.0f;
float one = 1.0f;
float point1 = 0.000001f;

//---------------------------------------------------------------------------
void Camera::projectCamera (Stuff::Vector3D &point)
{
	//--------------------------------------------------------------------
	// This is already in -x,z,y reference frame.
	point *= worldToCameraMatrix;
}
 
//------------------------------------------------------------------------------
void Camera::setOrthogonal(void)
{
	if (!usePerspective)
	{
		//--------------------
		//Parallel Projection
		float invCamScale = 1.0 / newScaleFactor;
		
		float left_clip, right_clip,top_clip,bottom_clip,far_clip,near_clip;
		
		left_clip = invCamScale * 300.0;
		top_clip = left_clip * ((float)Environment.screenHeight / (float)Environment.screenWidth);
		right_clip = -invCamScale * 300.0;
		bottom_clip = right_clip * ((float)Environment.screenHeight / (float)Environment.screenWidth);
		near_clip = -2000.0;
		far_clip = 8000.0;
		// EDITOR-FAR-CLIP-1 (parallel/ortho mirror): the editor zooms the eye far
		// outside the map, so terrain crosses the FAR side of this orthographic depth
		// slab (stock far = 8000) and gets clipped in the GPU-indirect pzOk gate
		// (u_worldToClipGL). Extend ONLY the far plane in the editor; leave near at
		// -2000 (widening near broke the reverse-Z depth window). Game byte-identical.
		{
			extern bool InEditor;
			if (InEditor) far_clip = 400000.0;
		}

		//
		//------------------------------------------------------------------------
		// Set up the camera to clip matrix.  This matrix takes camera space
		// coordinates and maps them into a homogeneous culling space where valid
		// X, Y, and Z axis values (when divided by W) will all be between 0 and 1
		//------------------------------------------------------------------------
		//
		cameraToClip(LEFT_AXIS, LEFT_AXIS) = 1.0f / (left_clip-right_clip);
		cameraToClip(LEFT_AXIS, UP_AXIS) = 0.0f;
		cameraToClip(LEFT_AXIS, FORWARD_AXIS) = 0.0f;
		cameraToClip(LEFT_AXIS, 3) = 0.0f;

		cameraToClip(UP_AXIS, LEFT_AXIS) = 0.0f;
		cameraToClip(UP_AXIS, UP_AXIS) = 1.0f / (top_clip-bottom_clip);
		cameraToClip(UP_AXIS, FORWARD_AXIS) = 0.0f;
		cameraToClip(UP_AXIS, 3) = 0.0f;

		cameraToClip(FORWARD_AXIS, LEFT_AXIS) = 0.0;
		cameraToClip(FORWARD_AXIS, UP_AXIS) = 0.0;
		// Reverse-Z (U1): parallel/ortho z:[near,far]->NDC[1,0] (was [0,1]).
		cameraToClip(FORWARD_AXIS, FORWARD_AXIS) = -1.0f / (far_clip-near_clip);
		cameraToClip(FORWARD_AXIS, 3) = 0.0;

		cameraToClip(3, LEFT_AXIS) = -right_clip / (left_clip-right_clip);
		cameraToClip(3, UP_AXIS) = -bottom_clip / (top_clip-bottom_clip);
		// Reverse-Z (U1): translate term flips sign and uses far_clip.
		cameraToClip(3, FORWARD_AXIS) = far_clip / (far_clip-near_clip);
		cameraToClip(3, 3) = 1.0f;
		// F2 unified-projection: keep GL-native product in sync with cameraToClip.
		cameraToClipGL.Multiply(cameraToClip, kPixelHomogToGLNDC);
	}
	else
	{
		//-----------------------
		//Perspective Projection
		//

		float far_clip,near_clip;
		near_clip = Camera::NearPlaneDistance;
		far_clip = Camera::FarPlaneDistance;

		// EDITOR-FAR-CLIP-1: the editor zooms the eye far outside the map, so the
		// terrain crosses the stock far plane (61555) and gets depth-clipped in the
		// GPU-indirect path (u_worldToClipGL) -> "can't see past some distance".
		// Extend the far plane in the EDITOR by default (InEditor) so the whole map
		// draws from any zoom. Reverse-Z keeps depth precision fine across the larger
		// range. MC2_EDITOR_FAR_CLIP=<value> overrides; the game is unaffected.
		// LIVE InEditor check (NOT static const): setOrthogonal runs during early
		// camera init, BEFORE InitializeGameEngine() sets InEditor=true, so a
		// static-const capture froze this at false and the override never applied
		// (measured: far map corners stayed beyond the small scaled far plane).
		extern bool InEditor;
		float editorFar = 0.0f;
		{
			const char* v = getenv("MC2_EDITOR_FAR_CLIP");
			if (v && *v) { float f = (float)atof(v); editorFar = (f > 1000.0f) ? f : 400000.0f; }
			else if (InEditor) editorFar = 400000.0f;
		}
		if (editorFar > far_clip)
			far_clip = editorFar;

		float horizontal_fov = camera_fov * DEGREES_TO_RADS;
		// UI-ASPECT-ANCHOR-1 (camera): build the projection with the REAL
		// display aspect, not the HUD-clamped 800x600 (4:3). The clamped ratio
		// baked a 4:3 frustum that the full-window stretch then distorted at
		// any other aspect (16:9 = fat mechs; 25:9 = squashed scene). Pick math
		// shares this matrix (project/inverse through the same constants), so
		// screen<->world stays consistent. MC2_CAMERA_ASPECT_NATIVE=0 restores
		// the legacy stretched look.
		float height2width = ((float)Environment.screenHeight / (float)Environment.screenWidth);
		{
			static const bool s_nativeAspect =
				[]{ const char* e = getenv("MC2_CAMERA_ASPECT_NATIVE"); return !(e && e[0] == '0'); }();
			// Panel previews (SimpleCamera -> fixed 800x600 FBO) must keep the
			// clamped ratio: their render target IS 4:3, not the window.
			extern int g_mechPreviewRenderDepth;
			if (s_nativeAspect && g_mechPreviewRenderDepth == 0
				&& Environment.drawableWidth > 0 && Environment.drawableHeight > 0)
				height2width = (float)Environment.drawableHeight / (float)Environment.drawableWidth;
		}

		//
		//-------------------------------------------------------
		// Calculate the horizontal, vertical, and forward ranges
		//-------------------------------------------------------
		//
		float left_clip = -(float)(near_clip * tan(horizontal_fov*0.5f));
		float top_clip = -left_clip * height2width;
		float bottom_clip = -top_clip;
		float right_clip = -left_clip;
		
		float horizontal_range = APPLY_LEFT_SIGN(1.0f) / (left_clip - right_clip);
		float vertical_range = APPLY_UP_SIGN(1.0f) / (top_clip - bottom_clip);
		float depth_range = APPLY_FORWARD_SIGN(1.0f) / (far_clip - near_clip);
		
 		//
		//------------------------------------------------------------------------
		// Set up the camera to clip matrix.  This matrix takes camera space
		// coordinates and maps them into a homogeneous culling space where valid
		// X, Y, and Z axis values (when divided by W) will all be between 0 and 1
		//------------------------------------------------------------------------
		//
		cameraToClip(LEFT_AXIS, LEFT_AXIS) = near_clip * horizontal_range;
		cameraToClip(LEFT_AXIS, UP_AXIS) = 0.0f;
		cameraToClip(LEFT_AXIS, FORWARD_AXIS) = 0.0f;
		cameraToClip(LEFT_AXIS, 3) = 0.0f;
	
		cameraToClip(UP_AXIS, LEFT_AXIS) = 0.0f;
		cameraToClip(UP_AXIS, UP_AXIS) = near_clip * vertical_range;
		cameraToClip(UP_AXIS, FORWARD_AXIS) = 0.0f;
		cameraToClip(UP_AXIS, 3) = 0.0f;
	
		cameraToClip(FORWARD_AXIS, LEFT_AXIS) = -right_clip * horizontal_range;
		cameraToClip(FORWARD_AXIS, UP_AXIS) = -bottom_clip * vertical_range;
		// Reverse-Z (U1): for NDC=(A*z+B)/z, near->1/far->0 gives
		// A=-near/(far-near), B=+near*far/(far-near). depth_range already
		// carries 1/(far-near) and the axis sign, so: negate-and-near-swap
		// the FF term, sign-flip the 3F term.
		cameraToClip(FORWARD_AXIS, FORWARD_AXIS) = -near_clip * depth_range;
		cameraToClip(FORWARD_AXIS, 3) = 1.0f;

		cameraToClip(3, LEFT_AXIS) = 0.0f;
		cameraToClip(3, UP_AXIS) = 0.0f;
		cameraToClip(3, FORWARD_AXIS) = far_clip * near_clip * depth_range;
		cameraToClip(3, 3) = 0.0f;
		// F2 unified-projection: keep GL-native product in sync with cameraToClip.
		cameraToClipGL.Multiply(cameraToClip, kPixelHomogToGLNDC);

		// [REVERSE_Z v1] env-gated lifecycle print (MC2_REVERSE_Z_TRACE=1),
		// silent by default; one-shot via a static latch. Matches the
		// MC2_DEBUG_SHADOW_* pattern (CLAUDE.md Debug instrumentation rule;
		// structural depth change). Samples the perspective z-row at the
		// near and far planes: under reverse-Z near must map to NDC z=1 and
		// far to NDC z=0. NDC z = (FF*z_cam + B) / z_cam, with FF and B the
		// just-written cameraToClip terms.
		static const bool s_rzTrace = (getenv("MC2_REVERSE_Z_TRACE") != nullptr);
		if (s_rzTrace) {
			static int s_rzBuild = 0;
			++s_rzBuild;
			const float FF = (float)cameraToClip(FORWARD_AXIS, FORWARD_AXIS);
			const float B  = (float)cameraToClip(3, FORWARD_AXIS);
			const float ndcNear = (FF * near_clip + B) / near_clip;
			const float ndcFar  = (FF * far_clip  + B) / far_clip;
			fprintf(stderr,
				"[REVERSE_Z v1] event=proj_build n=%d mode=perspective "
				"near=%.4f far=%.4f FF=%.8f B=%.8f "
				"ndcZ_near=%.6f(expect~1) ndcZ_far=%.6f(expect~0)\n",
				s_rzBuild, near_clip, far_clip, FF, B, ndcNear, ndcFar);
		}

		// [WATER_ASPECT_DIAG v1] env-gated one-shot (MC2_WATER_ASPECT_DIAG=1),
		// silent by default. Confirms the projection-vs-viewport aspect mismatch
		// suspected behind the "water dark/blown/noisy at odd desktop resolution"
		// issue. The perspective frustum aspect here comes ONLY from
		// Environment.screenWidth/Height (force-clamped to 800x600 by the
		// HUD-RES-CLAMP, gameos_graphics.cpp), while the scene FBO + glViewport
		// rasterize at Environment.drawableWidth/Height (native FULLSCREEN_DESKTOP
		// drawable). When those aspects differ, geometry is stretched in X by
		// (vpAspect/projAspect) and the water FS screen-space derivatives
		// (dFdx/dFdy) go anisotropic. stretchX != 1.0 => mismatch present.
		static const bool s_waAspect = (getenv("MC2_WATER_ASPECT_DIAG") != nullptr);
		if (s_waAspect) {
			const float projAspect = (float)Environment.screenWidth /
			                         (float)(Environment.screenHeight ? Environment.screenHeight : 1);
			const float vpAspect   = (float)Environment.drawableWidth /
			                         (float)(Environment.drawableHeight ? Environment.drawableHeight : 1);
			const float stretchX   = projAspect != 0.0f ? (vpAspect / projAspect) : 0.0f;
			fprintf(stderr,
				"[WATER_ASPECT_DIAG v1] proj=%dx%d (aspect=%.4f, 4:3=1.333) "
				"viewport=%dx%d (aspect=%.4f) stretchX=%.4f (1.0=match) "
				"dFdx/dFdy_aniso~=%.4f\n",
				Environment.screenWidth, Environment.screenHeight, projAspect,
				Environment.drawableWidth, Environment.drawableHeight, vpAspect,
				stretchX, stretchX != 0.0f ? (1.0f / stretchX) : 0.0f);
		}
	}
}

extern bool useLOSAngle;
extern float	worldUnitsPerMeter;
Stuff::Vector3D actualPosition;

#define MOVE_IN_INC			50.0f
#define ELEVATION_BUFFER	125.0f
#define ANGLE_UP_INC		0.25f

//-----------------------------------------------------------------------------------------------
bool CameraLineOfSight (Stuff::Vector3D position, Stuff::Vector3D targetPosition) 
{
	int posCellR, posCellC;
	int tarCellR, tarCellC;
	land->worldToCell(position, posCellR, posCellC);
	land->worldToCell(targetPosition, tarCellR, tarCellC);
	
	//-----------------------------------------------------
	// Once we allow teams to have alliances (for contacts,
	// etc.), simply set all nec. team bits in this mask...

	//------------------------------------------------------------------------------------------
	// Within magic radius.  Check REAL LOS now.
	// Check is really simple.
	// Find deltaCellRow and deltaCellCol and iterate over them from source to dest.
	// If the magic line ever goes BELOW the terrainElevation PLUS localElevation return false.
	Stuff::Vector3D startPos, endPos;
	startPos.Zero();
	endPos.Zero();
	
	int tCellRow = tarCellR, tCellCol = tarCellC;
	int mCellRow = posCellR, mCellCol = posCellC;
	
	land->getCellPos(tCellRow,tCellCol,endPos);
	startPos = position;
	
//	land->getCellPos(mCellRow,mCellCol,startPos);
//	startPos.z = position.z;

	Stuff::Vector3D deltaCellVec;
	deltaCellVec.y = tCellRow - mCellRow;
	deltaCellVec.x = tCellCol - mCellCol;
	deltaCellVec.z = 0.0f;
	float startHeight = startPos.z;
	
	float length = deltaCellVec.GetApproximateLength();
	
	if (length > Stuff::SMALL)
	{
		float colLength = deltaCellVec.x / length;
		float rowLength = deltaCellVec.y / length;
		float heightLen = (endPos.z - startPos.z) / length;
		
		float lastCol = fabs(colLength * 5.0);
		float lastRow = fabs(rowLength * 5.0);
		
		float startCellRow = mCellRow;
		float startCellCol = mCellCol;
		
		float endCellRow = tCellRow;
		float endCellCol = tCellCol;
		
		Stuff::Vector3D currentPos = startPos;
		Stuff::Vector3D dist;
		dist.Subtract(endPos,currentPos);
		
		bool colDone = false, rowDone = false;
		while (!colDone || !rowDone)
		{
			if (fabs(startCellRow - endCellRow) > lastRow)	//DO NOT INCLUDE LAST CELL!!!!!
			{
				startCellRow += rowLength;
			}
			else
			{
				rowDone = true;
			}
				
			if (fabs(startCellCol - endCellCol) > lastCol)	//DO NOT INCLUDE LAST CELL!!!!!
			{
				startCellCol += colLength;
			}
			else
			{
				colDone = true;
			}

			startHeight += heightLen;
			
			int startCellC = startCellCol;
			int startCellR = startCellRow;

			land->getCellPos(startCellR,startCellC,currentPos);
			//float localElev = (worldUnitsPerMeter * 4.0f * (float)GameMap->getLocalHeight(startCellR,startCellC));
			//currentPos.z += localElev;
			
			if (startHeight < currentPos.z)
			{
				return false;
			}
		}
	}
	
	return true;
}

//------------------------------------------------------------
void Camera::setCameraOrigin (void)
{
	if (!usePerspective)
	{
		//--------------------
		//Parallel Projection
		cameraDirection.range = -3000.0f;
		//cameraDirection.pitch = cameraDirection.pitch;
		cameraOrigin.BuildRotation(Stuff::EulerAngles(cameraDirection.pitch, cameraDirection.yaw, 0.0f));

		Stuff::Point3D translation(cameraDirection);

		translation.x -= cameraShift.x;
		translation.z += cameraShift.y;
		translation.y += cameraShiftZ;

		cameraOrigin.BuildTranslation(translation);
		
		lookVector.Normalize(translation);
	}
	else
	{
		//--------------------------------
		// Perspective projection
		bool isOKView = false;
		float localAltitude = cameraAltitude;
		float localAngle = projectionAngle;
		while (!isOKView)
		{
			cameraDirection.range = localAltitude;
			cameraDirection.pitch = -localAngle * DEGREES_TO_RADS;
			cameraOrigin.BuildRotation(Stuff::EulerAngles(cameraDirection.pitch, cameraDirection.yaw, 0.0f));
	
			Stuff::Point3D translation(cameraDirection);
			lookVector = translation;
			lookVector.Normalize(lookVector);
			lookVector.Negate(lookVector);
			
			translation.x += -cameraShift.x;
			translation.z += cameraShift.y;
			translation.y += cameraShiftZ;
			
			actualPosition.x = -translation.x;
			actualPosition.y = translation.z;
			actualPosition.z = translation.y - ELEVATION_BUFFER;

			// [HZB_CAM v1] one-shot diagnostic (env-gated, at most once per second)
			if (getenv("MC2_HZB_FORCE_HORIZON"))
			{
				static double s_hzbCamLastPrint = -999.0;
				double nowSec = (double)clock() / CLOCKS_PER_SEC;
				if (nowSec - s_hzbCamLastPrint >= 1.0)
				{
					s_hzbCamLastPrint = nowSec;
					fprintf(stderr,
						"[HZB_CAM v1] eyeX=%f eyeY=%f lookX=%f lookY=%f rot=%f angle=%f\n",
						actualPosition.x, actualPosition.y,
						lookVector.x, lookVector.y,
						cameraRotation, projectionAngle);
					fflush(stderr);
				}
			}

			// Clamp camera to terrain floor via position, not angle.
			// When clamped, recompute the rotation to look FROM the lifted
			// position TOWARD the focus — prevents the lifted camera from
			// aiming into the ground with the pre-lift pitch.
			if (land)
			{
				float terrainAtCam = land->getTerrainElevation(actualPosition);
				if (terrainAtCam < Terrain::waterElevation)
					terrainAtCam = Terrain::waterElevation;
				if (translation.y < terrainAtCam + AltitudeMinimum)
				{
					translation.y = terrainAtCam + AltitudeMinimum;
					actualPosition.z = translation.y - ELEVATION_BUFFER;

					// Repoint at focus from clamped position.
					float dx = position.x - actualPosition.x;
					float dy = position.y - actualPosition.y;
					float dz = cameraShiftZ - translation.y;
					float horiz = sqrtf(dx*dx + dy*dy);
					float newPitch = (horiz > 0.1f) ? atan2f(dz, horiz) : (dz < 0.0f ? -1.5707f : 0.0f);
					cameraOrigin.BuildRotation(Stuff::EulerAngles(newPitch, cameraDirection.yaw, 0.0f));
				}
			}

			isOKView = true;
			cameraOrigin.BuildTranslation(translation);
			physicalPos = translation;
		}
	}

	//------------------------------------------------------------
	// DO Exactly what MLR does to create its Internal Matrices.
	Stuff::LinearMatrix4D cameraToWorldMatrix(cameraOrigin);
																  
	worldToCameraMatrix.Invert(cameraToWorldMatrix);

	worldToClip.Multiply(worldToCameraMatrix, cameraToClip);
	
	if (usePerspective)
		clipToWorld.Invert(worldToClip);
}

//---------------------------------------------------------------------------
Stuff::Matrix4D Camera::worldToClipGL() const
{
    // F2 unified-projection: GPU-path projection product.
    //   = kAxisSwapMC2toGL * worldToCameraMatrix * cameraToClipGL
    // cameraToClipGL was precomputed at camera-update time (see
    // cameraToClipGL.Multiply invocations after every cameraToClip write
    // in calculateProjectionConstants). Replaces the prior per-call
    // kPixelHomogToGLNDC Multiply.
    Stuff::Matrix4D viewClipGL;
    viewClipGL.Multiply(worldToCameraMatrix, cameraToClipGL);
    Stuff::Matrix4D out;
    out.Multiply(kAxisSwapMC2toGL, viewClipGL);
    return out;
}

//---------------------------------------------------------------------------
Stuff::Matrix4D Camera::worldToViewGL() const
{
    // F1-3A ViewUniforms: view matrix = kAxisSwapMC2toGL * worldToCameraMatrix.
    // Mirrors worldToClipGL() but stops before cameraToClipGL multiply.
    // Uses Matrix4D::Multiply(const AffineMatrix4D&, const Matrix4D&) overload,
    // valid because LinearMatrix4D inherits AffineMatrix4D.
    Stuff::Matrix4D out;
    // Reuse worldToClipGL() intermediate: out = kAxisSwapMC2toGL * worldToCameraMatrix
    // There is no Multiply(Matrix4D, LinearMatrix4D) directly, but LinearMatrix4D
    // IS-A AffineMatrix4D, so we produce a temporary Matrix4D first.
    Stuff::Matrix4D viewM(worldToCameraMatrix); // AffineMatrix4D -> Matrix4D promotion
    out.Multiply(kAxisSwapMC2toGL, viewM);
    return out;
}

//---------------------------------------------------------------------------
Stuff::Vector3D Camera::cameraOriginGL() const
{
    // F1-3A ViewUniforms: camera world position transformed to GL coord space.
    // kAxisSwapMC2toGL: x' = -x, y' = z, z' = y.
    // physicalPos holds the camera world position in MC2 space (set in updateCameraInfo
    // as physicalPos = translation).
    return Stuff::Vector3D(-physicalPos.x, physicalPos.z, physicalPos.y);
}

//---------------------------------------------------------------------------
ModernClipResult Camera::projectModernClipGL(const Stuff::Vector3D& world) const
{
    // F4 projectZ-bypass helper. Row-vector convention:
    //   clip_row = world_row * M
    // world.w = 1.
    // M = worldToClipGL() = kAxisSwapMC2toGL * worldToCameraMatrix * cameraToClipGL.
    Stuff::Matrix4D M = worldToClipGL();
    ModernClipResult r;
    r.clip.x = world.x * M(0,0) + world.y * M(1,0) + world.z * M(2,0) + M(3,0);
    r.clip.y = world.x * M(0,1) + world.y * M(1,1) + world.z * M(2,1) + M(3,1);
    r.clip.z = world.x * M(0,2) + world.y * M(1,2) + world.z * M(2,2) + M(3,2);
    r.clip.w = world.x * M(0,3) + world.y * M(1,3) + world.z * M(2,3) + M(3,3);
    r.admit  = clipSpaceFrustumAdmitGL(r.clip);
    return r;
}

//---------------------------------------------------------------------------
void Camera::calculateProjectionConstants (void)
{
	cameraDirection.yaw = cameraRotation * DEGREES_TO_RADS;
	cameraDirection.pitch = projectionAngle * DEGREES_TO_RADS;
	
	screenCenter.x = screenResolution.x / 2.0;
	screenCenter.y = screenResolution.y / 2.0;
	screenCenter.z = screenResolution.z = 0.0f;
	
	if (usePerspective)
	{
		float oneOverResMag = screenResolution.GetLength();
		if (oneOverResMag > Stuff::SMALL)
			oneOverResMag = 1.0f / oneOverResMag;

		verticalSphereClipConstant = tan(screenResolution.y * oneOverResMag * camera_fov * 0.7071f * DEGREES_TO_RADS);
		horizontalSphereClipConstant = tan(screenResolution.x * oneOverResMag * camera_fov * 0.7071f * DEGREES_TO_RADS);
		
		cameraFrame.reset_to_world_frame();
		
		Stuff::Vector3D rotationVector(-projectionAngle,0.0f,cameraRotation);
		cameraFrame.rotate(rotationVector);
	}

	setOrthogonal();		//Setup the camera Clip matrix.

	setCameraOrigin();

	// Invalidate pick screen-rect cache when camera inputs change visually.
	// Coarse grid: position snapped to 0.25 WU, rotation to 0.05 deg, fov to 0.1 deg.
	// Stable under per-frame altitude/zoom lerp micro-drift that would otherwise
	// cause the revision to increment every frame even on a "stationary" camera.
	{
		auto snapI = [](float v, float step) -> uint32_t {
			return (uint32_t)(int32_t)(v / step);
		};
		uint32_t h = snapI(position.x,      0.25f) * 2654435761u
		           ^ snapI(position.y,      0.25f) * 2246822519u
		           ^ snapI(cameraShiftZ,    0.25f) * 3266489917u
		           ^ snapI(cameraRotation,  0.05f) * 374761393u
		           ^ snapI(projectionAngle, 0.05f) * 668265263u
		           ^ snapI(camera_fov,      0.1f)  * 2166136261u
		           ^ (uint32_t)(int32_t)screenResolution.x
		           ^ (uint32_t)(int32_t)screenResolution.y * 40503u;
		if (h != lastCameraInputHash_) {
			lastCameraInputHash_ = h;
			++viewProjectionRevision_;
			if (viewProjectionRevision_ == 0) viewProjectionRevision_ = 1;
		}
	}
}

//---------------------------------------------------------------------------
void Camera::calculateTopViewConstants (void)
{
	//-----------------------------------------------------------
	// Make camera look down on terrain.
	cameraDirection.yaw = cameraRotation * DEGREES_TO_RADS;
	cameraDirection.pitch = -90.0f * DEGREES_TO_RADS;
	
	screenCenter.x = screenResolution.x / 2.0;
	screenCenter.y = screenResolution.y / 2.0;
	
	setOrthogonal();		//Setup the camera Clip matrix.
	
	setCameraOrigin();
}

//---------------------------------------------------------------------------
void Camera::deactivate (void)
{
	//--------------------------------------------------------
	// Anything a camera needs to do on shutdown goes in here.
	active = FALSE;
}

//---------------------------------------------------------------------------
void Camera::setPosition(Stuff::Vector3D newPosition, bool swoopy)
{
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
	
	Stuff::Vector3D cornerPos1(maxVisual,0,0), cornerPos2(0,maxVisual,0), cornerPos3(-maxVisual,0,0), cornerPos4(0,-maxVisual,0);
	Stuff::Vector3D distance1, distance2, distance3, distance4;

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
void Camera::movePosLeft(float amount, Stuff::Vector3D &pos)
{
	Stuff::Vector3D direction;
	direction.x = -0.7071f;
	direction.y = -0.7071f;
	direction.z = 0.0;

	OppRotate(direction,cameraRotation);
	direction *= amount;

	pos.x += direction.x;
	pos.y += direction.y;
}

//---------------------------------------------------------------------------
void Camera::movePosRight(float amount, Stuff::Vector3D &pos)
{
	Stuff::Vector3D direction;
	direction.x = 0.7071f;
	direction.y = 0.7071f;
	direction.z = 0.0;

	OppRotate(direction,cameraRotation);
	direction *= amount;

	pos.x += direction.x;
	pos.y += direction.y;
}

//---------------------------------------------------------------------------
void Camera::movePosUp(float amount, Stuff::Vector3D &pos)
{
	Stuff::Vector3D direction;
	direction.x = -0.7071f;
	direction.y = 0.7071f;
	direction.z = 0.0;

	OppRotate(direction,cameraRotation);
	direction *= amount;

	pos.x += direction.x;
	pos.y += direction.y;
}

//---------------------------------------------------------------------------
void Camera::movePosDown(float amount, Stuff::Vector3D &pos)
{
	Stuff::Vector3D direction;
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
	Stuff::Vector3D direction;
	
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

	Stuff::Vector3D newPosition = position;
	newPosition.x += direction.x;
	newPosition.y += direction.y;

	setPosition(newPosition);
}

//---------------------------------------------------------------------------
void Camera::moveRight(float amount)
{
	Stuff::Vector3D direction;
	
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

	Stuff::Vector3D newPosition = position;
	newPosition.x += direction.x;
	newPosition.y += direction.y;

	setPosition(newPosition);
}

//---------------------------------------------------------------------------
void Camera::moveUp(float amount)
{
	Stuff::Vector3D direction;
	
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

	Stuff::Vector3D newPosition = position;
	newPosition.x += direction.x;
	newPosition.y += direction.y;
	
	setPosition(newPosition);
}

//---------------------------------------------------------------------------
void Camera::moveDown(float amount)
{
	Stuff::Vector3D direction;
	
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

	Stuff::Vector3D newPosition = position;
	newPosition.x += direction.x;
	newPosition.y += direction.y;

	setPosition(newPosition);
}

//---------------------------------------------------------------------------
void Camera::rotateLightLeft(float amount)
{
	lightYaw += amount;
	if (lightYaw > 360.0)
		lightYaw -= 360;

	//Replace with TGL Lights
	Stuff::LinearMatrix4D lightToWorldMatrix;

	lightToWorldMatrix.BuildTranslation(Stuff::Point3D(0.0,0.0,0.0));
	lightToWorldMatrix.BuildRotation(Stuff::EulerAngles(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));

	worldLights[0]->SetLightToWorld(&lightToWorldMatrix);
}	

//---------------------------------------------------------------------------
void Camera::rotateLightRight(float amount)
{
	lightYaw -= amount;
	if (lightYaw < -360.0)
		lightYaw += 360;

	//Replace with TGL Lights
	Stuff::LinearMatrix4D lightToWorldMatrix;
	lightToWorldMatrix.BuildTranslation(Stuff::Point3D(0.0,0.0,0.0));
	lightToWorldMatrix.BuildRotation(Stuff::EulerAngles(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));

	worldLights[0]->SetLightToWorld(&lightToWorldMatrix);
}	

//---------------------------------------------------------------------------
void Camera::rotateLightUp(float amount)
{
	lightPitch += amount;
	if (lightPitch > 90.0)
		lightPitch = 90.0;

	//Replace with TGL Lights
	Stuff::LinearMatrix4D lightToWorldMatrix;
	
	lightToWorldMatrix.BuildTranslation(Stuff::Point3D(0.0,0.0,0.0));
	lightToWorldMatrix.BuildRotation(Stuff::EulerAngles(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));

	worldLights[0]->SetLightToWorld(&lightToWorldMatrix);
}	

//---------------------------------------------------------------------------
void Camera::rotateLightDown(float amount)
{
	lightPitch -= amount;
	if (lightPitch < 5.0f)
		lightPitch = 5.0f;

	//Replace with TGL Lights
	Stuff::LinearMatrix4D lightToWorldMatrix;
	
	lightToWorldMatrix.BuildTranslation(Stuff::Point3D(0.0,0.0,0.0));
	lightToWorldMatrix.BuildRotation(Stuff::EulerAngles(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));

	worldLights[0]->SetLightToWorld(&lightToWorldMatrix);
}	

//---------------------------------------------------------------------------
void Camera::rotateLeft(float amount)
{
	float newCamAngle = cameraRotation+amount;
	float newCamWorld = worldCameraRotation+amount;
	setCameraRotation(newCamAngle,newCamWorld);
	
	cameraShiftZ = position.z;
}

//---------------------------------------------------------------------------
void Camera::rotateRight(float amount)
{
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
//---------------------------------------------------------------------------
unsigned char Camera::getLightRed (float intensity)
{
	//ASM and Inline
	if (intensity > 0.0)
	{
		long fResult = float2long(lightRed * intensity + ambientRed);
		if (fResult < 0xff)
			return fResult;
		else
			return 0xff;
	}
	
	return ambientRed;
}

//---------------------------------------------------------------------------
unsigned char Camera::getLightGreen (float intensity)
{
	//ASM and Inline
	if (intensity > 0.0)
	{
		long fResult = float2long(lightGreen * intensity + ambientGreen);
		if (fResult < 0xff)
			return fResult;
		else
			return 0xff;
	}

	return ambientGreen;
}

//---------------------------------------------------------------------------
unsigned char Camera::getLightBlue (float intensity)
{
	//ASM and Inline
	if (intensity > 0.0)
	{
		long fResult = float2long(lightBlue * intensity + ambientBlue);
		if (fResult < 0xff)
			return fResult;
		else
			return 0xff;
	}

	return ambientBlue;
}

bool Camera::save( FitIniFile* file )
{
	file->writeBlock("Cameras");
	file->writeIdFloat( "ProjectionAngle", projectionAngle );
	file->writeIdFloat( "PositionX", position.x );
	file->writeIdFloat( "PositionY", position.y );
	file->writeIdFloat( "PositionZ", 0.0 );
	file->writeIdBoolean( "Ready", true );

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
	DistanceFactor	= 1.0f / (MaxClipDistance - MinHazeDistance);
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

//---------------------------------------------------------------------------
