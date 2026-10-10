// TG shapes: TG_Shape::MultiTransformShape and its positions-only variant, light gathering and billboard handling.
// Declared in the class headers; shared names in tgl_internal.h.
#include "tgl_internal.h"
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

void *TG_Shape::operator new (size_t mySize)
{
	void *result = TG_Shape::tglHeap->Malloc(mySize);
	return result;
}
 
 void TG_Shape::operator delete (void *us)
{
	TG_Shape::tglHeap->Free(us);
}

//-------------------------------------------------------------------------------
//This function sets up the camera Matrices for this TG_Shape to transform
//itself with.  These matrices are static and only need to be set once per
//render pass if the camera does not change for that pass.
void TG_Shape::SetCameraMatrices (Stuff::LinearMatrix4D *camOrigin, Stuff::Matrix4D *camToClip)
{
	s_cameraOrigin = camOrigin;
	s_cameraToClip = camToClip;

	s_worldToCamera.Invert(*s_cameraOrigin);

	s_worldToClip.Multiply(s_worldToCamera, *s_cameraToClip);
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

long TG_Shape::MultiTransformShape (Stuff::Matrix4D *shapeToClip, Stuff::Point3D *backFacePoint, TG_ShapeRecPtr parentNode, bool isHudElement, BYTE alphaValue, bool isClamped)
{
	// [OBJECT_RECON v1] outer scope. No-op when MC2_OBJECT_RECON_TRACY unset.
	::mc2_object_recon::Scope _recon_total_(
		&::mc2_object_recon::g_per_frame.shape_total_ns,
		&::mc2_object_recon::g_per_frame.shape_calls);

	if (!numVertices)		//WE are the root Shape which may have no shape or a helper shape which defintely has no shape!
	{
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
		Stuff::Point3D pos = theShape->listOfTypeVertices[j].position;
		if (shapeScalar > 0.0f)
			pos *= shapeScalar;
			
		Stuff::Vector4D xformCoords;
		Stuff::Vector4D screen;

		xformCoords.Multiply(pos,*shapeToClip);

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

		DWORD redFinal=0, greenFinal=0, blueFinal=0;
		DWORD redAmb = 0, greenAmb = 0, blueAmb = 0;
		DWORD redSpec=0, greenSpec=0, blueSpec=0;
		
		if (lighteningLevel > 0)
		{
			redSpec = blueSpec = greenSpec = lighteningLevel;
		}

		DWORD startVLight = theShape->listOfTypeVertices[j].aRGBLight;
   		if (startVLight == 0xffff00ff)			//Hot Pink -- Lit Windows -- ONLY at NIGHT
   		{
   			if (isNight)
   			{
   				redFinal = (theShape->hotPinkRGB>>16) & 0x000000ff;
   				greenFinal = (theShape->hotPinkRGB>>8) & 0x000000ff;
   				blueFinal = (theShape->hotPinkRGB) & 0x000000ff;
   			}
   			else if (nightFactor > Stuff::SMALL)
   			{
   				redFinal = (theShape->hotPinkRGB>>16) & 0x000000ff;
   				greenFinal = (theShape->hotPinkRGB>>8) & 0x000000ff;
   				blueFinal = (theShape->hotPinkRGB) & 0x000000ff;
   				
   				redFinal *= nightFactor;
   				blueFinal *= nightFactor;
   				greenFinal *= nightFactor;
   			}
			else		//Its not night, paint windows dark grey
			{
				redFinal = 0x2f;
				greenFinal = 0x2f;
				blueFinal = 0x2f;
			}
   		}
   		else if (startVLight == 0xffffff00)		//Hot Yellow -- Outside Building Lights -- ONLY at NIGHT
   		{
   			if (nightFactor >= 0.75f)
   			{
   				if (isNight)
   				{
   					redFinal = (theShape->hotYellowRGB>>16) & 0x000000ff;
   					greenFinal = (theShape->hotYellowRGB>>8) & 0x000000ff;
   					blueFinal = (theShape->hotYellowRGB) & 0x000000ff;
   				}
   				else
   				{
   					redFinal = (theShape->hotYellowRGB>>16) & 0x000000ff;
   					greenFinal = (theShape->hotYellowRGB>>8) & 0x000000ff;
   					blueFinal = (theShape->hotYellowRGB) & 0x000000ff;
   					
   					redFinal *= nightFactor;
   					blueFinal *= nightFactor;
   					greenFinal *= nightFactor;
   				}
   			}
   		}
   		else if (startVLight == 0xff00ff00)		//Hot Green -- Building Base Lights -- ONLY at NIGHT
   		{
   			if (isNight)
   			{
   				redFinal = (theShape->hotGreenRGB>>16) & 0x000000ff;
   				greenFinal = (theShape->hotGreenRGB>>8) & 0x000000ff;
   				blueFinal = (theShape->hotGreenRGB) & 0x000000ff;
   			}
   			else if (nightFactor > Stuff::SMALL)
   			{
   				redFinal = (theShape->hotGreenRGB>>16) & 0x000000ff;
   				greenFinal = (theShape->hotGreenRGB>>8) & 0x000000ff;
   				blueFinal = (theShape->hotGreenRGB) & 0x000000ff;

   				redFinal *= nightFactor;
   				blueFinal *= nightFactor;
   				greenFinal *= nightFactor;
   			}
   		}
   		else if (startVLight == 0xffff0000)		//Hot Red -- Blink this light.
   		{
   		
   		}
   		else if (startVLight == 0xff0000ff)		//Hot Blue -- Blink this light.
   		{
   		
   		}
   		else if (startVLight & 0x00ffffff)		//Some other kind of light, just add it in.
   		{
			if (!lightsOut)
			{
				redFinal = (startVLight>>16) & 0x000000ff;
				greenFinal = (startVLight>>8) & 0x000000ff;
				blueFinal = (startVLight) & 0x000000ff;
			}
   		}
		else if (isHudElement)
		{
			redFinal = blueFinal = greenFinal = 0xff;	//Just max out its light.
		}

		if (BaseVertexColor)
		{
			redFinal += ((BaseVertexColor>>16) & 0x000000ff);
			if (redFinal > 0xff)
				redFinal = 0xff;

			greenFinal += ((BaseVertexColor>>8) & 0x000000ff);
			if (greenFinal > 0xff)
				greenFinal = 0xff;

			blueFinal += (BaseVertexColor & 0x000000ff);
			if (blueFinal > 0xff)
				blueFinal = 0xff;
		}

		// Stage 2.D.2: one-shot hot-green trace flag.
		// Set on the first daytime hot-green vertex when MC2_OBJECT_PARITY_TRACE=1.
		// Traces s_lightDir, normal, cosine, and contributions — the CPU side
		// that should match GatherLightsParameters and the GPU dot product.
		//
		// Stage 2.D.2.1 (m2): [PARITY_DIAG v2] is a SEPARATE diagnostic stream
		// from [OBJECT_PARITY v1]. The "v2" label is NOT a schema bump of the
		// parity system — it distinguishes this subsystem's trace stream. It is
		// gated by MC2_OBJECT_PARITY_TRACE (see gameosmain.cpp), not by
		// MC2_OBJECT_PARITY_CHECK. Zero [PARITY_DIAG v2] output in production
		// logs is expected and correct.
		bool doHotGreenTrace = (s_hotGreenLightTraceEnabled && !s_hotGreenLightTraceDone
		                        && (startVLight == 0xff00ff00) && !isNight
		                        && (nightFactor <= Stuff::SMALL));
		if (doHotGreenTrace) {
			s_hotGreenLightTraceDone = true;
			std::fprintf(stderr,
				"[PARITY_DIAG v2] CPU hot-green vertex=%ld "
				"normalShape=(%.4f,%.4f,%.4f) s_numLights=%d\n",
				j,
				theShape->listOfTypeVertices[j].normal.x,
				theShape->listOfTypeVertices[j].normal.y,
				theShape->listOfTypeVertices[j].normal.z,
				(int)s_numLights);
			std::fflush(stderr);
		}

		if (useVertexLighting && (Environment.Renderer != 3))
		{
			if (!isSpotlight && !isWindow)
			{
				for (long i=0;i<s_numLights;i++)
				{
					if ((s_listOfLights[i] != NULL) && (s_listOfLights[i]->active))
					{
						DWORD startLight = s_listOfLights[i]->GetaRGB();
						switch (s_listOfLights[i]->lightType)
						{
							case TG_LIGHT_AMBIENT:
							{
								redAmb = ((startLight>>16) & 0x000000ff);
								greenAmb = ((startLight>>8) & 0x000000ff);
								blueAmb = ((startLight) & 0x000000ff);
								if (doHotGreenTrace) {
									std::fprintf(stderr,
										"[PARITY_DIAG v2] CPU light[%ld] AMBIENT "
										"aRGB=0x%08X ambR=%u ambG=%u ambB=%u\n",
										i, (unsigned)startLight,
										redAmb, greenAmb, blueAmb);
									std::fflush(stderr);
								}
							}
							break;

							case TG_LIGHT_INFINITE:
							{
								float cosine = s_lightDir[i].x * theShape->listOfTypeVertices[j].normal.x;
								cosine += s_lightDir[i].y * theShape->listOfTypeVertices[j].normal.y;
								cosine += s_lightDir[i].z * theShape->listOfTypeVertices[j].normal.z;

								if (doHotGreenTrace) {
									std::fprintf(stderr,
										"[PARITY_DIAG v2] CPU light[%ld] INFINITE "
										"aRGB=0x%08X lightDir=(%.4f,%.4f,%.4f) cosine=%.4f\n",
										i, (unsigned)startLight,
										s_lightDir[i].x, s_lightDir[i].y, s_lightDir[i].z,
										cosine);
									std::fflush(stderr);
								}

								if (cosine < 0.0f)
								{
									float cos = fabs(cosine);
									float red = float((startLight>>16) & 0x000000ff) * cos;
									float green = float((startLight>>8) & 0x000000ff) * cos;
									float blue = float((startLight) & 0x000000ff) * cos;

									redFinal += float2long(red);
									greenFinal += float2long(green);
									blueFinal += float2long(blue);

									/*
									//-----------------------
									// Run Specular next.
									// Mirror the Light vector.
									// Then DOT with Camera vector.
									// Mul Result by itself 4 times.
									// Mul that result by shinyness.
									// Multiply THAT through color and apply!
									float mirrorScalar = cosine * 2.0f;
									Stuff::Vector3D MirrorVector(listOfVertices[i].normal);
									MirrorVector *= mirrorScalar;
									MirrorVector.Subtract(MirrorVector,s_lightDir[i]);
									MirrorVector.Normalize(MirrorVector);

									Stuff::Vector3D SpecPoint;
									if (turn > 3)
										SpecPoint.Normalize(*backFacePoint);
									float specular = SpecPoint.x * MirrorVector.x;
									specular += SpecPoint.y * MirrorVector.y;
									specular += SpecPoint.z * MirrorVector.z;

									if (specular < 0.0f)
									{
										//specular = fabs(specular);
										specular *= specular;
										specular *= specular;
										specular *= specular;
										specular *= specular;

										specular *= 1.0f;	
										// For now, a shinyness of 1.0f.  Perfect reflection!
										// Multiplies will make specular positive!
										float red = float((startLight>>16) & 0x000000ff) * specular;
										float green = float((startLight>>8) & 0x000000ff) * specular;
										float blue = float((startLight) & 0x000000ff) * specular;

										redSpec += (DWORD)red;
										greenSpec += (DWORD)green;
										blueSpec += (DWORD)blue;
									}
									*/
								}
							}
							break;
	
							case TG_LIGHT_INFINITEWITHFALLOFF:
							{
								Stuff::Point3D vertexToLight;
								vertexToLight = s_lightToShape[i];
								vertexToLight -= theShape->listOfTypeVertices[j].position;
	
								float length = vertexToLight.GetApproximateLength();
	
								float falloff = 1.0f;
	
								float red,green,blue;
	
								if (s_listOfLights[i]->GetFalloff(length, falloff))
								{
									float cosine = -(s_lightDir[i] * (theShape->listOfTypeVertices[j].normal));
	
									red = float((startLight>>16) & 0x000000ff) * falloff;
									green = float((startLight>>8) & 0x000000ff) * falloff;
									blue = float((startLight) & 0x000000ff) * falloff;
	
									red *= cosine;
									green *= cosine;
									blue *= cosine;
	
									redFinal += (DWORD)red;
									greenFinal += (DWORD)green;
									blueFinal += (DWORD)blue;
								}
							}
							break;
	
							case TG_LIGHT_POINT:
							{
								Stuff::Point3D vertexToLight;
								vertexToLight = s_lightDir[i];
								float length = vertexToLight.GetApproximateLength();
	
								if (length > Stuff::SMALL)
								{	
									vertexToLight.Normalize(vertexToLight);
		
									float falloff = 1.0f;
		
									if (s_listOfLights[i]->GetFalloff(length, falloff))
									{
										float cosine = vertexToLight * (theShape->listOfTypeVertices[j].normal);
		
										float red,green,blue;
		
										if (cosine < 0.0f)
										{
											cosine = fabs(cosine);
											red = float((startLight>>16) & 0x000000ff) * falloff;
											green = float((startLight>>8) & 0x000000ff) * falloff;
											blue = float((startLight) & 0x000000ff) * falloff;
		
											red *= cosine;
											green *= cosine;
											blue *= cosine;
		
											redSpec += (DWORD)red;
											greenSpec += (DWORD)green;
											blueSpec += (DWORD)blue;
										}
									}
								}
								else
								{
									//Object is in center of light.  BLOW IT OUT!
									float cosine = 1.0f;
	
									float red,green,blue;
	
									if (cosine < 0.0f)
									{
										cosine = fabs(cosine);
										red = float((startLight>>16) & 0x000000ff);
										green = float((startLight>>8) & 0x000000ff);
										blue = float((startLight) & 0x000000ff);
	
										red *= cosine;
										green *= cosine;
										blue *= cosine;
	
										redSpec += (DWORD)red;
										greenSpec += (DWORD)green;
										blueSpec += (DWORD)blue;
									}
								}
							}
							break;
								  
							case TG_LIGHT_TERRAIN:
							{
								if (useShadows)
								{
									Stuff::Point3D vertexToLight;
									Stuff::Vector3D pos = theShape->listOfTypeVertices[j].position;
									RotateLight(pos,yawRotation);
									vertexToLight.Add(s_lightDir[i],pos);
									float length = vertexToLight.GetApproximateLength();
		
									if (length > Stuff::SMALL)
									{	
										float falloff = 1.0f;
										if (s_listOfLights[i]->GetFalloff(length, falloff))
										{
											float red,green,blue;
			
											red = float((startLight>>16) & 0x000000ff) * falloff;
											green = float((startLight>>8) & 0x000000ff) * falloff;
											blue = float((startLight) & 0x000000ff) * falloff;
			
											listOfColors[j].redSpec = (DWORD)red;
											listOfColors[j].greenSpec = (DWORD)green;
											listOfColors[j].blueSpec = (DWORD)blue;
										}
									}
									else
									{
										//Object is in center of light.  NOTHING HAPPENS WITH THIS KIND!!!
										//Light is already burned in!
									}
								}
							}
							break;
							
 							case TG_LIGHT_SPOT:
							{
								Stuff::Point3D vertexToLight;
								vertexToLight = s_lightDir[i];
								
								//-------------------------------------------------
								// Defines the actual spot of light on the ground
								float length = vertexToLight.GetApproximateLength();
	
								//-------------------------------------------------
								// Defines the REAL direction of the spot light.
								vertexToLight = s_spotDir[i];
								if (vertexToLight.GetApproximateLength() > Stuff::SMALL)
									vertexToLight.Normalize(vertexToLight);
								else
									length = 99999999999999.0f;
	
								float falloff = 1.0f;
	
								if (s_listOfLights[i]->GetFalloff(length, falloff))
								{
									float cosine = vertexToLight * (theShape->listOfTypeVertices[j].normal);
	
									float red,green,blue;
	
									if (cosine < 0.0f)
									{
										cosine = fabs(cosine);
										red = float((startLight>>16) & 0x000000ff) * falloff;
										green = float((startLight>>8) & 0x000000ff) * falloff;
										blue = float((startLight) & 0x000000ff) * falloff;
	
										red *= cosine;
										green *= cosine;
										blue *= cosine;
	
										redSpec += (DWORD)red;
										greenSpec += (DWORD)green;
										blueSpec += (DWORD)blue;
									}
								}
							}
							break;
						}
					}
				}
				
				redFinal += redAmb;
				blueFinal += blueAmb;
				greenFinal += greenAmb;
			}

			if (redFinal > 255)
				redFinal = 255;

			if (greenFinal > 255)
				greenFinal = 255;

			if (blueFinal > 255)
				blueFinal = 255;

			if (doHotGreenTrace) {
				std::fprintf(stderr,
					"[PARITY_DIAG v2] CPU hot-green final: "
					"redFinal=%u greenFinal=%u blueFinal=%u "
					"(before argb write: 0x%08X)\n",
					redFinal, greenFinal, blueFinal,
					(unsigned)((0xff << 24) + (redFinal << 16) + (greenFinal << 8) + blueFinal));
				std::fflush(stderr);
			}

			listOfVertices[j].argb = (0xff << 24) + (redFinal << 16) + (greenFinal << 8) + (blueFinal);

			// NEW
			// Add in the pre-calced or calced this frame terrain lights.
			// Terrain lights will only be made ACTIVE when day/night changes.
			// One frame of active is enough to store new values!
			redSpec += listOfColors[j].redSpec;
			greenSpec += listOfColors[j].greenSpec;
			blueSpec += listOfColors[j].blueSpec;
			
			if (redSpec > 255)
				redSpec = 255;

			if (greenSpec > 255)
				greenSpec = 255;

			if (blueSpec > 255)
				blueSpec = 255;
		}
		else
		{
			listOfVertices[j].argb = (0xff << 24) + (redAmb << 16) + (greenAmb << 8) + (blueAmb);
		}

		DWORD fogValue = (listOfVertices[j].frgb >> 24) & 0x000000ff;
		if (Environment.Renderer != 3)
		{
			if (useFog)
			{
				float relElevation = theShape->listOfTypeVertices[j].position.y;
				relElevation += theShape->relativeNodeCenter.y;

				TG_ShapeRecPtr curNode = parentNode;
				while (curNode && curNode->node)
				{
					relElevation += curNode->node->myType->GetRelativeNodeCenter().y;
					curNode = curNode->parentNode;
				}

				if (relElevation < MAX_FOG_ELEVATION)
				{
					float fogFactor = MAX_FOG_ELEVATION - relElevation;
					if (fogFactor >= 0.0)
					{
						fogFactor /= (MAX_FOG_ELEVATION);
						if (fogFactor <= 1.0)
						{
							fogFactor *= fogFactor;
							fogFactor = 1.0 - fogFactor;
						}
						else
						{
							fogFactor = 0.0;
						}

						fogValue += float2long((0x000000ff - fogValue) * fogFactor);
					}
				}
				else
				{
					fogValue = 0x000000ff;
				}

				listOfVertices[j].frgb = (fogValue << 24) + (redSpec << 16) + (greenSpec << 8) + (blueSpec);
			}
			else
			{
				listOfVertices[j].frgb = (0xff << 24) + (redSpec << 16) + (greenSpec << 8) + (blueSpec);
			}
		}
		else
		{
			listOfVertices[j].frgb = (0xff << 24) + (redSpec << 16) + (greenSpec << 8) + (blueSpec);
		}

		if (useFog && Camera::HazeFactor != 0.0f)
		{
			float fogFactor = 1.0 - Camera::HazeFactor;
			DWORD distFog = float2long(fogFactor * 255.0f);
			
			if (distFog < fogValue)
				fogValue = distFog;
				
			listOfVertices[j].frgb = (fogValue << 24) + (redSpec << 16) + (greenSpec << 8) + (blueSpec);
		}
		
		if (aRGBHighlight)
		{
			DWORD argb = listOfVertices[j].argb;

			DWORD fFinal = (argb>>24) & 0x000000ff;
			DWORD rFinal = (argb>>16) & 0x000000ff;
			DWORD gFinal = (argb>>8) & 0x000000ff;
			DWORD bFinal = (argb) & 0x000000ff;

			rFinal += (aRGBHighlight>>16) & 0x000000ff;
			if (rFinal > 255)
				rFinal = 255;

			gFinal += (aRGBHighlight>>8) & 0x000000ff;
			if (gFinal > 255)
				gFinal = 255;

			bFinal += (aRGBHighlight) & 0x000000ff;;
			if (bFinal > 255)
				bFinal = 255;

			argb = (fFinal << 24) + (rFinal << 16) + (gFinal << 8) + (bFinal);
			listOfVertices[j].argb = argb;
		}
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
		cosine = (*backFacePoint * (tri->faceNormal));
		// EDGE-LIGHT-BILLBOARD-CULL-1 (master-FAIL #8): edge-light glow cards are
		// two-sided so they stay lit as the camera orbits toward top-down instead
		// of backface-culling. MC2_LIGHT_BILLBOARD_TWOSIDED=0 restores legacy cull.
		if ((cosine > 0.0f) || isSpotlight ||
			(isLightBillboard && tgLightBillboardTwoSided()))
		{
			listOfVisibleFaces[numVisibleFaces] = j;
			numVisibleFaces++;

			DWORD redFinal = 0, greenFinal = 0, blueFinal = 0;
			DWORD redSpec=0, greenSpec=0, blueSpec=0;
			DWORD redAmb=0, greenAmb=0, blueAmb=0;
	
			if (useFaceLighting)
			{
				for (long i=0;i<s_numLights;i++)
				{
					if (s_listOfLights[i] != NULL)
					{
						DWORD startVLight = s_listOfLights[i]->GetaRGB();
						switch (s_listOfLights[i]->lightType)
						{
							case TG_LIGHT_INFINITE:
							{
								if (s_lightDir[i].GetLength() > Stuff::SMALL)
									s_lightDir[i].Normalize(s_lightDir[i]);
		
								float cosine = -(s_lightDir[i] * (theShape->listOfTypeTriangles[j].faceNormal));
		
								if (cosine > 0.0f)
								{
									float red = float((startVLight>>16) & 0x000000ff) * cosine;
									float green = float((startVLight>>8) & 0x000000ff) * cosine;
									float blue = float((startVLight) & 0x000000ff) * cosine;
		
									redFinal += float2long(red);
									greenFinal += float2long(green);
									blueFinal += float2long(blue);
								}
							}
							break;
		
							case TG_LIGHT_INFINITEWITHFALLOFF:
							{
								if (s_lightDir[i].GetLength() > Stuff::SMALL)
									s_lightDir[i].Normalize(s_lightDir[i]);
		
								float cosine = -(s_lightDir[i] * (theShape->listOfTypeTriangles[j].faceNormal));
		
								Stuff::Point3D vertexToLight;
								vertexToLight = s_lightToShape[i];
								vertexToLight -= theShape->listOfTypeVertices[theShape->listOfTypeTriangles[j].Vertices[0]].position;
		
								float length = vertexToLight.GetApproximateLength();
		
								float falloff = 1.0f;
		
								float red,green,blue;
		
								if ((cosine > 0.0f) && s_listOfLights[i]->GetFalloff(length, falloff))
								{
									red = float((startVLight>>16) & 0x000000ff) * falloff;
									green = float((startVLight>>8) & 0x000000ff) * falloff;
									blue = float((startVLight) & 0x000000ff) * falloff;
		
									red *= cosine;
									green *= cosine;
									blue *= cosine;
		
									redFinal += (DWORD)red;
									greenFinal += (DWORD)green;
									blueFinal += (DWORD)blue;
								}
							}
							break;
		
							case TG_LIGHT_POINT:
							{
								Stuff::Point3D vertexToLight;
								vertexToLight = s_lightToShape[i];
								vertexToLight -= theShape->listOfTypeVertices[theShape->listOfTypeTriangles[j].Vertices[0]].position;
		
								float length = vertexToLight.GetApproximateLength();
		
								vertexToLight.Normalize(vertexToLight);
								float falloff = 1.0f;
		
								if (s_listOfLights[i]->GetFalloff(length, falloff))
								{
									float cosine = -(vertexToLight * (theShape->listOfTypeTriangles[j].faceNormal));
		
									float red,green,blue;
		
									if (cosine > 0.0f)
									{
										red = float((startVLight>>16) & 0x000000ff) * falloff;
										green = float((startVLight>>8) & 0x000000ff) * falloff;
										blue = float((startVLight) & 0x000000ff) * falloff;
		
										red *= cosine;
										green *= cosine;
										blue *= cosine;
		
										redSpec += (DWORD)red;
										greenSpec += (DWORD)green;
										blueSpec += (DWORD)blue;
									}
								}
							}
							break;
		
							case TG_LIGHT_AMBIENT:
							{
								redAmb = ((startVLight>>16) & 0x000000ff);
								greenAmb = ((startVLight>>8) & 0x000000ff);
								blueAmb = ((startVLight) & 0x000000ff);
							}
							break;
						}
					}
				}
		
				redFinal += redAmb;
				blueFinal += blueAmb;
				greenFinal += greenAmb;
				
				if (redFinal > 255)
					redFinal = 255;
	
				if (greenFinal > 255)
					greenFinal = 255;
	
				if (blueFinal > 255)
					blueFinal = 255;
					
				if (redSpec > 255)
					redSpec = 255;
	
				if (greenSpec > 255)
					greenSpec = 255;
	
				if (blueSpec > 255)
					blueSpec = 255;
			}
	
			for (long i=0;i<3;i++ )
			{
				DWORD argb = listOfVertices[theShape->listOfTypeTriangles[j].Vertices[i]].argb;
				DWORD rFinal = (argb>>16) & 0x000000ff;
				DWORD gFinal = (argb>>8) & 0x000000ff; 
				DWORD bFinal = (argb) & 0x000000ff;    
	
				rFinal += redFinal;
				if (rFinal > 255)
					rFinal = 255;
	
				gFinal += greenFinal;
				if (gFinal > 255)
					gFinal = 255;
	
				bFinal += blueFinal;
				if (bFinal > 255)
					bFinal = 255;
	
				listOfTriangles[j].aRGBLight[i] = (alphaValue << 24) + (rFinal << 16) + (gFinal << 8) + (bFinal);
	
				DWORD frgb = listOfVertices[theShape->listOfTypeTriangles[j].Vertices[i]].frgb;
	
				DWORD fFinal = (frgb>>24) & 0x000000ff;
				rFinal = (frgb>>16) & 0x000000ff;
				gFinal = (frgb>>8) & 0x000000ff;
				bFinal = (frgb) & 0x000000ff;
	
				rFinal += (redSpec>>16) & 0x000000ff;
				if (rFinal > 255)
					rFinal = 255;
	
				gFinal += (greenSpec>>8) & 0x000000ff;
				if (gFinal > 255)
					gFinal = 255;
	
				bFinal += (blueSpec) & 0x000000ff;
				if (bFinal > 255)
					bFinal = 255;
	
				listOfTriangles[j].fRGBLight[i] = (fFinal << 24) + (rFinal << 16) + (gFinal << 8) + (bFinal);
			}
			
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
			
			if (isSpotlight)
			{
				mcTextureManager->addTriangle(0xffffffff,MC2_ISSPOTLGT);
			}
			else if (isWindow)
			{
				mcTextureManager->addTriangle(0xffffffff, MC2_DRAWALPHA);
			}
			else
			{
				//--------------------------------------------------
				// Any sorting we need to do occurs in here as well!
				// So, find out what texture this face is using and mark that texture as needing 3 more vertices.
				if (theShape->listOfTextures[theShape->listOfTypeTriangles[j].localTextureHandle].textureAlpha || (alphaValue != 0xff))
				{
					mcTextureManager->addTriangle(theShape->listOfTextures[theShape->listOfTypeTriangles[j].localTextureHandle].mcTextureNodeIndex,MC2_DRAWALPHA | addFlags);
				}
				else
				{
					mcTextureManager->addTriangle(theShape->listOfTextures[theShape->listOfTypeTriangles[j].localTextureHandle].mcTextureNodeIndex,MC2_DRAWSOLID | addFlags);
				}

			}
		}
	}
	} // [OBJECT_RECON v1] end flight scope

	// FIXME: this (listOfTypeTriangles[0]) is not correct if model has more than 1 texture!
	// [OBJECT_RECON v1] emit scope: GatherLightsParameters + addLightDataStructure
	// + addRenderShape — the modern shader path queue cost.
	::mc2_object_recon::Scope _recon_emit_(
		&::mc2_object_recon::g_per_frame.shape_emit_ns);
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
long TG_Shape::MultiTransformShape_PositionsOnly (Stuff::Matrix4D *shapeToClip, Stuff::Point3D *backFacePoint, TG_ShapeRecPtr parentNode, bool isHudElement, BYTE alphaValue, bool isClamped)
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
		Stuff::Point3D pos = theShape->listOfTypeVertices[j].position;
		if (shapeScalar > 0.0f)
			pos *= shapeScalar;

		Stuff::Vector4D xformCoords;
		Stuff::Vector4D screen;

		xformCoords.Multiply(pos,*shapeToClip);

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
		cosine = (*backFacePoint * (tri->faceNormal));
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
