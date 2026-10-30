#include "Gates/preferences_legacy.h"
//-------------------------------------------------------------------------------
//
// Tiny Geometry Layer
//
// For MechCommander 2 -- Copyright (c) 1999 Microsoft
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "tgl_internal.h"
#include "sim_overlap.h"
#include <atomic>
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "vk_object_lights.h"

//-------------------------------------------------------------------------------
// Statics
glm::mat4 	*TG_Shape::s_cameraOrigin = nullptr;
glm::mat4			*TG_Shape::s_cameraToClip = nullptr;
glm::mat4	TG_Shape::s_worldToCamera;
glm::mat4			TG_Shape::s_worldToClip;

float					TG_Shape::viewMulX = 0.0;
float					TG_Shape::viewAddX = 0.0;
float					TG_Shape::viewMulY = 0.0;
float					TG_Shape::viewAddY = 0.0;

DWORD					TG_Shape::fogColor = 0xffffffff;
float					TG_Shape::fogStart = 0.0f;
float					TG_Shape::fogFull = 0.0f;

TG_LightPtr				*TG_Shape::s_listOfLights = nullptr;
DWORD					TG_Shape::s_numLights = 0;


DWORD					TG_Shape::lighteningLevel = 0;

// Encyclopedia 3D-preview auto-framing: when armed, the software vertex
// transform accumulates the projected screen-space bounding box of the
// geometry it draws. This is the *real* rendered extent (rotation- and
// geometry-correct), used by SimpleCamera to fit the object to the panel.
// Disarmed by default; zero cost on the normal in-mission path.
// [UI-PHASE1] carried from mc2r_ui_phase1 (base->theirs hunk @tgl.cpp:90-105);
// consumed by code/simplecamera.cpp + code/mechlopedia.cpp (Mechlopedia/Mechbay preview).
bool  g_previewCaptureBounds = false;
float g_previewBoundsMinX =  1e9f;
float g_previewBoundsMaxX = -1e9f;
float g_previewBoundsMinY =  1e9f;
float g_previewBoundsMaxY = -1e9f;
// Diagnostic: triangles actually emitted to the draw list.
// Reset and read by SimpleCamera::render to tell "transformed but not drawn"
// (render gate / cull) apart from "not transformed".
int   g_previewDrawTris = 0;
bool drawOldWay = false;

bool renderTGLShapes = true;

bool silentMode = false;		//Used for automated builds to keep errors from popping up.

// Tier-1 instrumentation canonical frame counter. Defined here (in mclib)
// rather than GameOS/gameos/gameosmain.cpp so data-tool targets (aseconv,
// pak, makefst, makersp) that link mclib but NOT gameosmain still resolve
// the symbol. The per-frame increment still lives in gameosmain.cpp's frame
// loop; data tools never increment it and see a harmless zero.
//
// CONTRACT. g_mc2FrameCounter is the engine LOGIC-PASS epoch: monotonic, advanced once per rendered frame and, under
// MC2_SIM_SPEED>=2 with the fixed-step harness, once more between the extra DoGameLogic substeps (gameosmain_frame.cpp), so every
// per-frame-keyed sim path (brain tick accumulator, light-row / cachedFrame_ freshness stamps) sees one logic pass per value. Sim
// code keys on it and must not change. g_mc2RenderFrame is the RENDER-frame clock: advanced exactly once per presented frame by
// gos::host::advanceFrameCounter() (game and editor); the present path, readback tags and frame-pinned captures key on it.
// At one logic pass per frame the two are equal.
// The overlap worker advances extra logic passes while the host presents. Relaxed
// accesses protect the epoch; the overlap join synchronizes the sim state itself.
std::atomic<uint32_t> g_mc2FrameCounter = 0;
uint32_t g_mc2RenderFrame = 0;

// VK-FIELD-VFX-5: fire-event present-capture arm. 0 = unarmed; the
// MC2_FX_FORCE_SPAWN=loop fixture (code/warrior.cpp) sets it to the frame
// ~0.5s after its first volley when MC2_VK_PRESENT_DUMP_ON_FIRE is set, and
// the VK present dump (engine_present_vk.cpp verifyReadbackContent +
// gameosmain verify window) treats it as a dynamic MC2_VK_PRESENT_DUMP_FRAME
// pin. Defined next to the frame counter for the same data-tool-link reason.
uint32_t g_mc2FxFireCaptureFrame = 0;

void TG_Shape::Touch() {
	lastTurnTransformed = turn;
}

//-------------------------------------------------------------------------------
//This function sets up the camera Matrices for this TG_Shape to transform
//itself with.  These matrices are static and only need to be set once per
//render pass if the camera does not change for that pass.
void TG_Shape::SetCameraMatrices (glm::mat4 *camOrigin, glm::mat4 *camToClip)
{
	mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "TG_Shape::SetCameraMatrices");
	s_cameraOrigin = camOrigin;
	s_cameraToClip = camToClip;

	s_worldToCamera = glm::inverse((*s_cameraOrigin));

	s_worldToClip = s_worldToCamera * *s_cameraToClip;
}

void TG_Shape::SetFog (DWORD fRGB, float fStart, float fFull)
{
	mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "TG_Shape::SetFog");
	fogColor = fRGB;
	fogStart = fStart;
	fogFull = fFull;

	if (fogStart == fogFull)
	{
		fogStart = -100000.0f;
		fogFull = -99999.0f;
	}
}

void TG_Shape::SetViewport (float mulX, float mulY, float addX, float addY)
{
	mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "TG_Shape::SetViewport");
	viewMulX = mulX;
	viewMulY = mulY;
	viewAddX = addX;
	viewAddY = addY;
}

//This function sets the list of lights used to light the shape (the CPU fallback draw reads them).
long TG_Shape::SetLightList (TG_LightPtr *lightList, DWORD nLights)
{
	mc2::overlap::assertMainThread("TG_Shape::SetLightList");
	if (lightList)
	{
		s_listOfLights = lightList;
		s_numLights = nLights;
	}
	else
	{
		s_listOfLights = nullptr;
		s_numLights = 0;
	}

	return 0;
}

void TG_Shape::SetFogRGB (DWORD fRGB)
{
	fogRGB = fRGB;
}

void TG_TypeShape::movePosRelativeCenterNode (void)
{
	for (long i=0;i<numTypeVertices;i++)
	{
		listOfTypeVertices[i].position -= nodeCenter;
	}
}

// Per-actor light-data gather for the GPU batchers: uses the texture manager's per-frame scene template cache, then
// patches the current actor's terrain-scaled light color and submits through addLightDataStructure.
uint32_t TG_Shape::GatherGpuObjectLightDataOnly()
{
	LcsScope<LcsBucket::C5> _lcs;
	return mc2::vkLights::addLightDataStructureWithPerActorColor(&lightData_);
}

// Re-submit the already-populated lightData_ to get a fresh UBO slot index without re-gathering from s_listOfLights.
uint32_t TG_Shape::ResubmitCachedLightData()
{
	LcsScope<LcsBucket::C6> _lcs;
	return mc2::vkLights::addLightDataStructure(&lightData_);
}
