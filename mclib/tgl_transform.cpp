// TG shapes: TG_Shape::MultiTransformShape and its positions-only variant, light gathering and billboard handling.
// Declared in the class headers; shared names in tgl_internal.h.
#include "tgl_internal.h"
#include "tgl_transform_frame.h"
#include "Gates/gate.h"
// MC2_LIGHT_BILLBOARD_TWOSIDED=1 suppresses legacy backface culling of edge-light
// cards, so a card survives when the camera orbits behind it. Parsed once.
//
// AUDIT-TRUTH-1: RETIRED to default-OFF (was default-ON). It shipped enabled
// despite being non-stock, never user-approved, and declared falsified and
// near-inert by its own successor lane. Default OFF == the stock/legacy cull,
// which is what this predicate did before the gate existed; the gate only ever
// ADDS faces to listOfVisibleFaces, so OFF cannot remove anything stock drew.
// Kept as an opt-in lever rather than deleted: two call sites still consume it
// and it is the only way to A/B the two-sided hypothesis. Note this is the CPU
// transform path only -- the GPU static-prop equivalent is the separate
// MC2_LIGHT_BILLBOARD_FACING (also default-OFF).
static bool tgLightBillboardTwoSided()
{
	static const bool s = mc2::gate::get<"MC2_LIGHT_BILLBOARD_TWOSIDED">();
	return s;
}

// Stage 2.D.2 diagnostic: one-shot trace for hot-green CPU lighting.
// Fires once per session when MC2_OBJECT_PARITY_TRACE=1.
// Prints s_lightDir direction, normal, cosine, contribution per vertex.
bool s_hotGreenLightTraceDone = false;
bool s_hotGreenLightTraceEnabled = mc2::gate::get<"MC2_OBJECT_PARITY_TRACE">();

//-------------------------------------------------------------------------------
//This function sets up the camera Matrices for this TG_Shape to transform
//itself with.  These matrices are static and only need to be set once per
//render pass if the camera does not change for that pass.
void TG_Shape::SetCameraMatrices (glm::mat4 *camOrigin, glm::mat4 *camToClip)
{
	s_cameraOrigin = camOrigin;
	s_cameraToClip = camToClip;

	s_worldToCamera = glm::inverse((*s_cameraOrigin));

	s_worldToClip = s_worldToCamera * *s_cameraToClip;
}	

void TG_Shape::SetFog (DWORD fRGB, float fStart, float fFull)
{
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
	viewMulX = mulX;
	viewMulY = mulY;
	viewAddX = addX;
	viewAddY = addY;
}	

//-------------------------------------------------------------------------------
//This function sets the list of lights used by the TransformShape function
//to light the shape.
//Function returns 0 if lightList entries are all OK.  -1 otherwise.
//
long TG_Shape::SetLightList (TG_LightPtr *lightList, DWORD nLights)
{
	if (lightList)
	{
		s_listOfLights = lightList;
		s_numLights = nLights;
	}
	else
	{
		s_listOfLights = NULL;
		s_numLights = 0;
	}

	return 0;
}	

//-------------------------------------------------------------------------------
//This function sets the fog values for the shape.  Straight fog right now.
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


//-------------------------------------------------------------------------------
//This function does exactly what TranformShape does EXCEPT that the shapeToClip,
//Lighting and backface matrices have been calculated in the step above this one.
//This saves enormous processor cycles when matrices are the same and transforms
//Can just be run from the same matrices without recalcing them!
//Function returns -1 if all vertex screen positions are off screen.
//Function returns 0 if any one vertex screen position is off screen.
//Function returns 1 is all vertex screen positions are on screen.
// NOTE:  THIS IS NOT A RIGOROUS CLIP!!!!!!!!!


// Claim the hardware vertex slot of a GPU-only shape that has no software vertices.
void TG_Shape::xformGpuOnlyShape(glm::mat4* shapeToClip, bool isHudElement, BYTE alphaValue, bool isClamped) {
		// [UI-PHASE1] carried from mc2r_ui_phase1 (base->theirs hunk @tgl.cpp:1722+).
		// GPU-only shape: Assimp-imported mesh with ib_/vb_ but no SW vertex pipeline.
		// Claim the hardware vertex node slot now so TG_Shape::Render can fill it via
		// the 3-arg path.  Also set lastTurnTransformed so Render's staleness guard passes.
		// Guard: only TG_TypeShape (SHAPE_NODE) carries ib_/vb_/listOfTextures — casting
		// a bare TG_TypeNode (root/helper) reads garbage at those offsets and crashes.
		TG_TypeShapePtr hwShape = (myType && myType->GetNodeType() == SHAPE_NODE)
			? (TG_TypeShapePtr)myType : nullptr;
		if (bShadersDrawPathEnabled && hwShape && hwShape->ib_ && hwShape->vb_
			&& hwShape->listOfTypeTriangles && hwShape->listOfTextures
			&& shapeToClip
			&& !eligibleForGpuObjects(this) && !isSpotlight && !isWindow
			&& !hwShape->listOfTextures[hwShape->listOfTypeTriangles[0].localTextureHandle].textureAlpha
			&& (alphaValue == 0xff))
		{
			DWORD addFlags = 0;
			if (isHudElement) addFlags |= MC2_ISCOMPASS;
			if (isClamped)    addFlags |= MC2_ISTERRAIN;
			if (hwShape->alphaTestOn) addFlags |= MC2_ALPHATEST;

			size_t numLights = MAX_HW_LIGHTS_IN_WORLD;
			GatherLightsParameters(&lightData_);
			gosASSERT(numLights > 0);

			cur_viewport[0] = viewMulX;
			cur_viewport[1] = viewMulY;
			cur_viewport[2] = viewAddX;
			cur_viewport[3] = viewAddY;
			cur_shape2clip = *shapeToClip;

			mcTextureManager->addRenderShape(
				hwShape->listOfTextures[hwShape->listOfTypeTriangles[0].localTextureHandle].mcTextureNodeIndex,
				MC2_DRAWSOLID | addFlags);

			lastTurnTransformed = turn;
		}
}

// Project one vertex to screen space and track the on-screen flags and preview bounds.
void TG_Shape::xformVertexProject(glm::mat4* shapeToClip, bool& oneOff, bool& oneOn, TG_TypeShapePtr theShape, long j) {
	glm::vec3 pos = theShape->listOfTypeVertices[j].position;
	if (shapeScalar > 0.0f)
		pos *= shapeScalar;
		
	Stuff::Vector4D xformCoords;
	Stuff::Vector4D screen;

	xformCoords.MultiplyPoint(pos, *shapeToClip);

	if (eye->usePerspective)
	{
		float rhw = 1.0f;
		if (xformCoords.w != 0.0f)
			rhw = 1.0f / xformCoords.w;
		
		screen.x = (xformCoords.x * rhw) * viewMulX + viewAddX;
		screen.y = (xformCoords.y * rhw) * viewMulY + viewAddY;
		screen.z = (xformCoords.z * rhw);
		screen.w = fabs(rhw);
	}
	else
	{
		screen.x = (1.0f - xformCoords.x) * viewMulX + viewAddX;
		screen.y = (1.0f - xformCoords.y) * viewMulY + viewAddY;
		screen.z = xformCoords.z;
		screen.w = 0.000001f;
	}

	if ((screen.x < 0) || (screen.y < 0) || (screen.x >= viewMulX) || (screen.y >= viewMulY))
		oneOff = TRUE;

	if ((screen.x >= 0) && (screen.y >= 0) && (screen.x < viewMulX) && (screen.y <= viewMulY))
		oneOn = TRUE;

	listOfVertices[j].x = screen.x;
	listOfVertices[j].y = screen.y;
	listOfVertices[j].z = screen.z;
	listOfVertices[j].rhw = screen.w;
	listOfVertices[j].frgb = fogRGB;
	memset(&listOfColors[j],0,sizeof(listOfColors[j]));

	// [UI-PHASE1] carried from mc2r_ui_phase1: Encyclopedia preview accumulates
	// the real projected screen bounds when armed (see g_previewCaptureBounds).
	if (g_previewCaptureBounds)
	{
		if (screen.x < g_previewBoundsMinX) g_previewBoundsMinX = screen.x;
		if (screen.x > g_previewBoundsMaxX) g_previewBoundsMaxX = screen.x;
		if (screen.y < g_previewBoundsMinY) g_previewBoundsMinY = screen.y;
		if (screen.y > g_previewBoundsMaxY) g_previewBoundsMaxY = screen.y;
	}
}

// Queue the whole shape for the modern shader path.
void TG_Shape::xformEmitShape(glm::mat4* shapeToClip, bool isHudElement, BYTE alphaValue, bool isClamped, TG_TypeShapePtr theShape) {
	if (bShadersDrawPathEnabled && !eligibleForGpuObjects(this) && !isSpotlight && !isWindow && !theShape->listOfTextures[theShape->listOfTypeTriangles[0].localTextureHandle].textureAlpha && (alphaValue == 0xff))
	{
		DWORD addFlags = 0;
		if (isHudElement)
		{
			addFlags |= MC2_ISCOMPASS;
		}

		if (isClamped)
		{
			addFlags |= MC2_ISTERRAIN;
		}

		if(theShape->alphaTestOn)
		{
			addFlags |= MC2_ALPHATEST;
		}

		if (theShape->ib_ && theShape->vb_) {

            size_t numLights = MAX_HW_LIGHTS_IN_WORLD;
            GatherLightsParameters(&lightData_);
            gosASSERT(numLights > 0); // ensure at least one light was filled

			cur_viewport[0] = viewMulX;
			cur_viewport[1] = viewMulY;
			cur_viewport[2] = viewAddX;
			cur_viewport[3] = viewAddY;
			cur_shape2clip = *shapeToClip;

			// FIXME: this (listOfTypeTriangles[0]) is not correct if model has more than 1 texture! 
			mcTextureManager->addRenderShape(theShape->listOfTextures[theShape->listOfTypeTriangles[0].localTextureHandle].mcTextureNodeIndex, MC2_DRAWSOLID | addFlags);
		}
	}
}

// Transforms, lights and queues one shape. The timing scopes, the pool allocation, the per-vertex and per-face loop
// headers with their colour frame, the face cull test and the result stay here; each phase body is a member
// helper declared in tgl.h.
long TG_Shape::MultiTransformShape (glm::mat4 *shapeToClip, glm::vec3 *backFacePoint, TG_ShapeRecPtr parentNode, bool isHudElement, BYTE alphaValue, bool isClamped)
{
	// [OBJECT_RECON v1] outer scope. No-op when MC2_OBJECT_RECON_TRACY unset.
	::mc2_object_recon::Scope _recon_total_(
		&::mc2_object_recon::g_per_frame.shape_total_ns,
		&::mc2_object_recon::g_per_frame.shape_calls);

	if (!numVertices)		//WE are the root Shape which may have no shape or a helper shape which defintely has no shape!
	{
		xformGpuOnlyShape(shapeToClip, isHudElement, alphaValue, isClamped);
		return(1);
	}

	//-------------------------------------------------
	// Transform entire list of vertices.
	// shapeOrigin is ShapeToWorld.
	// invShapeOrigin is WorldToShape.
	bool oneOff = false;
	bool oneOn = false;

	bool isNight = eye->getIsNight();
	float nightFactor = eye->getNightFactor();

	if (lightsOut)
	{
		isNight = false;
		nightFactor = 0.0f;
	}
	
	// GHOST-CONE-VERIFY-4: a blip-only enemy is hidden by fading alphaValue to
	// 0, not by the `visible` flag — and the isSpotlight branch below emits at
	// hardcoded full opacity, ignoring alphaValue entirely. Bail here so a
	// fully-faded (invisible) mover draws no searchlight wedge. Static lamps
	// sit at alphaValue 0xff and are untouched. MC2_LIGHT_CONE_VIS_GATE=0
	// restores the legacy leak.
	if (isSpotlight && (!isNight
		|| mc2_cone_glow::cone_geom_hidden_by_alpha(alphaValue)))
	{
		listOfVertices = NULL;		//Mark this as untransformed!!
		return(1);
	}
		
	TG_TypeShapePtr theShape = (TG_TypeShapePtr)myType;

	{
		// [OBJECT_RECON v1] alloc sub-stage.
		::mc2_object_recon::Scope _recon_alloc_(
			&::mc2_object_recon::g_per_frame.shape_alloc_ns);
		//At this point, we know we are going to process this shape,
		// Get memory for its components from the pools!
		listOfVertices       = MC2_TGL_GET_VERTS_FOR_SHAPE(vertexPool, numVertices, this);
		listOfColors         = MC2_TGL_GET_COLORS(colorPool, numVertices);

		listOfShadowTVertices = MC2_TGL_GET_SHADOW(shadowPool, numVertices);

		listOfTriangles      = MC2_TGL_GET_TRIANGLES(trianglePool, numTriangles);

		listOfVisibleFaces   = MC2_TGL_GET_FACES(facePool, numTriangles);
		listOfVisibleShadows = MC2_TGL_GET_FACES(facePool, numTriangles);
	}

	if (!listOfVertices ||
		!listOfColors ||
		!listOfShadowTVertices ||
		!listOfTriangles ||
		!listOfVisibleFaces ||
		!listOfVisibleShadows)
		return(1);

	lastTurnTransformed = turn;

	// [OBJECT_RECON v1] per-vertex loop scope (transform + lighting bake combined).
	// Splitting xform from vlight per-iteration would add ~100 ns × N_vertices
	// of timer overhead per shape — comparable to the data we're measuring.
	// Per recon prompt's Tracy-zone-overhead rule: wrap the whole loop only.
	// Output field is `vlight` (lighting is the dominant chunk of this loop;
	// `xform` field is left zero on this build — surface in slice-2 spec if
	// finer-grained timing becomes necessary).
	{
	::mc2_object_recon::Scope _recon_vlight_(
		&::mc2_object_recon::g_per_frame.shape_vlight_ns);
	for (long j=0;j<numVertices;j++)
	{
		xformVertexProject(shapeToClip, oneOff, oneOn, theShape, j);
		TgXformFrame f;
		xformVertexBaseColor(isHudElement, isNight, nightFactor, theShape, j, f);
		xformVertexLights(theShape, j, f);
		xformVertexFog(parentNode, theShape, j, f);
	}
	} // [OBJECT_RECON v1] end vlight scope

	numVisibleFaces = 0;			//Reset Visible Faces

	TG_TypeTrianglePtr tri = &(theShape->listOfTypeTriangles[0]);
	// [OBJECT_RECON v1] per-face loop scope (face lighting + listOfTriangles
	// writes + addTriangle queue calls combined).
	{
	::mc2_object_recon::Scope _recon_flight_(
		&::mc2_object_recon::g_per_frame.shape_flight_ns);
	for (unsigned int j=0;j<numTriangles;j++,tri++)
	{
		float cosine = 0.0;
		cosine = (glm::dot(*backFacePoint, (tri->faceNormal)));
		// EDGE-LIGHT-BILLBOARD-CULL-1 (master-FAIL #8): edge-light glow cards are
		// two-sided so they stay lit as the camera orbits toward top-down instead
		// of backface-culling. MC2_LIGHT_BILLBOARD_TWOSIDED=0 restores legacy cull.
		if ((cosine > 0.0f) || isSpotlight ||
			(isLightBillboard && tgLightBillboardTwoSided()))
		{
			listOfVisibleFaces[numVisibleFaces] = j;
			numVisibleFaces++;

			TgXformFrame f;
			xformFaceLight(theShape, j, f);
			xformFaceCorners(alphaValue, theShape, j, f);
			xformFaceQueue(isHudElement, alphaValue, isClamped, theShape, j);
		}
	}
	} // [OBJECT_RECON v1] end flight scope

	// FIXME: this (listOfTypeTriangles[0]) is not correct if model has more than 1 texture!
	// [OBJECT_RECON v1] emit scope: GatherLightsParameters + addLightDataStructure
	// + addRenderShape — the modern shader path queue cost.
	::mc2_object_recon::Scope _recon_emit_(
		&::mc2_object_recon::g_per_frame.shape_emit_ns);
	xformEmitShape(shapeToClip, isHudElement, alphaValue, isClamped, theShape);
	gosASSERT(oneOff || oneOn);

	if (oneOff && !oneOn)
	{
		return(-1);
	}
	else if (oneOff && oneOn)
	{
		return(0);
	}		
	else if (!oneOff && oneOn)
	{
		return(1);
	}

	return(-1);
}

//-------------------------------------------------------------------------------
// Slice 2 (object-offload) — Stage 2.A
// MultiTransformShape_PositionsOnly: copy-and-strip variant of MultiTransformShape.
// KEEPS: pre-loop setup, all 6 pool allocations (PerPolySelect at tglpp.cpp:14-21
// requires ALL pointers non-null), screen-space transform, lastTurnTransformed,
// per-vertex loop's transform code (writes to listOfVertices[j].x/y/z/rhw/frgb),
// memset of listOfColors[j] to keep contents defined, per-face backface cull
// populating listOfVisibleFaces, oneOff/oneOn return logic.
// STRIPS: per-vertex lighting kernel (hot-color tags, redFinal/greenFinal/
// blueFinal accumulation, per-light loop, .argb write, fog-elevation lighting,
// frgb lighting recomputation, distance-fog), aRGBHighlight additive block,
// per-face lighting (useFaceLighting=false in stock), listOfTriangles aRGBLight
// /fRGBLight writes, addTriangle queue calls (dead on Renderer==3), addRenderShape
// emit block (slice 2's batcher draws the actor instead).
// Stage 2.A: declared and defined; NO call sites wired (dead code under flag-off).
// Stage 2.B wires the call sites in BldgAppearance/TreeAppearance/GenericAppearance
// ::update inside their existing cull gates, behind g_useGpuObjects.
long TG_Shape::MultiTransformShape_PositionsOnly (glm::mat4 *shapeToClip, glm::vec3 *backFacePoint, TG_ShapeRecPtr parentNode, bool isHudElement, BYTE alphaValue, bool isClamped)
{
	// [OBJECT_RECON v1] outer scope. No-op when MC2_OBJECT_RECON_TRACY unset.
	::mc2_object_recon::Scope _recon_total_(
		&::mc2_object_recon::g_per_frame.shape_total_ns,
		&::mc2_object_recon::g_per_frame.shape_calls);

	if (!numVertices)		//WE are the root Shape which may have no shape or a helper shape which defintely has no shape!
		return(1);

	//-------------------------------------------------
	// Transform entire list of vertices.
	// shapeOrigin is ShapeToWorld.
	// invShapeOrigin is WorldToShape.
	bool oneOff = false;
	bool oneOn = false;

	bool isNight = eye->getIsNight();
	float nightFactor = eye->getNightFactor();

	if (lightsOut)
	{
		isNight = false;
		nightFactor = 0.0f;
	}

	// GHOST-CONE-VERIFY-4: a blip-only enemy is hidden by fading alphaValue to
	// 0, not by the `visible` flag — and the isSpotlight branch below emits at
	// hardcoded full opacity, ignoring alphaValue entirely. Bail here so a
	// fully-faded (invisible) mover draws no searchlight wedge. Static lamps
	// sit at alphaValue 0xff and are untouched. MC2_LIGHT_CONE_VIS_GATE=0
	// restores the legacy leak.
	if (isSpotlight && (!isNight
		|| mc2_cone_glow::cone_geom_hidden_by_alpha(alphaValue)))
	{
		listOfVertices = NULL;		//Mark this as untransformed!!
		return(1);
	}

	TG_TypeShapePtr theShape = (TG_TypeShapePtr)myType;

	{
		// [OBJECT_RECON v1] alloc sub-stage.
		::mc2_object_recon::Scope _recon_alloc_(
			&::mc2_object_recon::g_per_frame.shape_alloc_ns);
		// Pool allocations preserved verbatim — PerPolySelect at tglpp.cpp:14-21
		// requires ALL of these pointers to be non-null.
		listOfVertices       = MC2_TGL_GET_VERTS_FOR_SHAPE(vertexPool, numVertices, this);
		listOfColors         = MC2_TGL_GET_COLORS(colorPool, numVertices);

		listOfShadowTVertices = MC2_TGL_GET_SHADOW(shadowPool, numVertices);

		listOfTriangles      = MC2_TGL_GET_TRIANGLES(trianglePool, numTriangles);

		listOfVisibleFaces   = MC2_TGL_GET_FACES(facePool, numTriangles);
		listOfVisibleShadows = MC2_TGL_GET_FACES(facePool, numTriangles);
	}

	if (!listOfVertices ||
		!listOfColors ||
		!listOfShadowTVertices ||
		!listOfTriangles ||
		!listOfVisibleFaces ||
		!listOfVisibleShadows)
		return(1);

	lastTurnTransformed = turn;

	// [OBJECT_RECON v1] per-vertex loop scope.
	// _PositionsOnly: transform only; lighting kernel stripped.
	{
	::mc2_object_recon::Scope _recon_vlight_(
		&::mc2_object_recon::g_per_frame.shape_vlight_ns);
	for (long j=0;j<numVertices;j++)
	{
		glm::vec3 pos = theShape->listOfTypeVertices[j].position;
		if (shapeScalar > 0.0f)
			pos *= shapeScalar;

		Stuff::Vector4D xformCoords;
		Stuff::Vector4D screen;

		xformCoords.MultiplyPoint(pos, *shapeToClip);

		if (eye->usePerspective)
		{
			float rhw = 1.0f;
			if (xformCoords.w != 0.0f)
				rhw = 1.0f / xformCoords.w;

			screen.x = (xformCoords.x * rhw) * viewMulX + viewAddX;
			screen.y = (xformCoords.y * rhw) * viewMulY + viewAddY;
			screen.z = (xformCoords.z * rhw);
			screen.w = fabs(rhw);
		}
		else
		{
			screen.x = (1.0f - xformCoords.x) * viewMulX + viewAddX;
			screen.y = (1.0f - xformCoords.y) * viewMulY + viewAddY;
			screen.z = xformCoords.z;
			screen.w = 0.000001f;
		}

		if ((screen.x < 0) || (screen.y < 0) || (screen.x >= viewMulX) || (screen.y >= viewMulY))
			oneOff = TRUE;

		if ((screen.x >= 0) && (screen.y >= 0) && (screen.x < viewMulX) && (screen.y <= viewMulY))
			oneOn = TRUE;

		listOfVertices[j].x = screen.x;
		listOfVertices[j].y = screen.y;
		listOfVertices[j].z = screen.z;
		listOfVertices[j].rhw = screen.w;
		listOfVertices[j].frgb = fogRGB;
		memset(&listOfColors[j],0,sizeof(listOfColors[j]));

		// [UI-PHASE1] carried from mc2r_ui_phase1: Encyclopedia preview accumulates
		// the real projected screen bounds when armed (see g_previewCaptureBounds).
		if (g_previewCaptureBounds)
		{
			if (screen.x < g_previewBoundsMinX) g_previewBoundsMinX = screen.x;
			if (screen.x > g_previewBoundsMaxX) g_previewBoundsMaxX = screen.x;
			if (screen.y < g_previewBoundsMinY) g_previewBoundsMinY = screen.y;
			if (screen.y > g_previewBoundsMaxY) g_previewBoundsMaxY = screen.y;
		}

		// Per-vertex lighting kernel STRIPPED (slice 2 _PositionsOnly):
		// redFinal/greenFinal/blueFinal accumulation, hot-color tag dispatch,
		// per-light loop, .argb write, fog-elevation recomputation, frgb
		// lighting update, distance-fog update, aRGBHighlight additive
		// — all omitted. listOfVertices[j].argb is left "stale or zero" per
		// spec Stage 2.B note; GPU path lights it via VS in Stage 2.C.
	}
	} // [OBJECT_RECON v1] end vlight scope

	numVisibleFaces = 0;			//Reset Visible Faces

	TG_TypeTrianglePtr tri = &(theShape->listOfTypeTriangles[0]);
	// [OBJECT_RECON v1] per-face loop scope.
	// _PositionsOnly: backface cull only; per-face lighting, listOfTriangles
	// writes, and addTriangle queue calls all stripped.
	{
	::mc2_object_recon::Scope _recon_flight_(
		&::mc2_object_recon::g_per_frame.shape_flight_ns);
	for (unsigned int j=0;j<numTriangles;j++,tri++)
	{
		float cosine = 0.0;
		cosine = (glm::dot(*backFacePoint, (tri->faceNormal)));
		// EDGE-LIGHT-BILLBOARD-CULL-1 (master-FAIL #8): edge-light glow cards are
		// two-sided so they stay lit as the camera orbits toward top-down instead
		// of backface-culling. MC2_LIGHT_BILLBOARD_TWOSIDED=0 restores legacy cull.
		if ((cosine > 0.0f) || isSpotlight ||
			(isLightBillboard && tgLightBillboardTwoSided()))
		{
			listOfVisibleFaces[numVisibleFaces] = j;
			numVisibleFaces++;

			// Per-face lighting (useFaceLighting=false in stock per Recon
			// Section 9 Correction B), listOfTriangles[j].aRGBLight[i] /
			// .fRGBLight[i] writes, addFlags / addTriangle queue calls
			// all STRIPPED. addTriangle is dead on Renderer==3 anyway.
		}
	}
	} // [OBJECT_RECON v1] end flight scope

	// addRenderShape block (the bShadersDrawPathEnabled emit branch in
	// MultiTransformShape) entirely STRIPPED. GPU-eligible populations are
	// submitted by the slice 1 batcher; this path must not double-draw them.
	// Note: the addRenderShape gate in MultiTransformShape is also defended
	// by the !eligibleForGpuObjects(this) term added above; this strip is
	// belt-and-suspenders for the _PositionsOnly variant.

	gosASSERT(oneOff || oneOn);

	if (oneOff && !oneOn)
	{
		return(-1);
	}
	else if (oneOff && oneOn)
	{
		return(0);
	}
	else if (!oneOff && oneOn)
	{
		return(1);
	}

	return(-1);
}

// Slice 2 (object-offload) — Stage 2.A
// Per-actor light-data gather without the per-vertex bake side-effects of
// MultiTransformShape. Uses the texture manager's per-frame scene template
// cache, then patches the current actor's terrain-scaled light color and
// submits through addLightDataStructure. Returned index is broadcast into per-leaf
// GpuStaticPropInstance.lightDataIndex by Stage 2.C wiring in
// GpuStaticPropBatcher::submitMultiShape (between the registration-check
// loop and the per-leaf submit loop). Stage 2.A: declared and defined; no
// callers wired.
uint32_t TG_Shape::GatherGpuObjectLightDataOnly()
{
	LcsScope<LcsBucket::C5> _lcs;
	return mcTextureManager->addLightDataStructureWithPerActorColor(&lightData_);
}

uint32_t TG_Shape::ResubmitCachedLightData()
{
	LcsScope<LcsBucket::C6> _lcs;
	return mcTextureManager->addLightDataStructure(&lightData_);
}
