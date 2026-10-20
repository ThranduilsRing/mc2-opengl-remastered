// TG_Shape: the per-vertex and per-face lighting phases of MultiTransformShape -- hot-colour and base colour,
// vertex lights, fog and highlight, face lights and the triangle queue. The orchestrator is in tgl_transform.cpp.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "tgl_internal.h"
#include "tgl_transform_frame.h"
#include "mc2fmt.h"


// Seed one vertex colour from the hot-colour markers and the base vertex colour.
void TG_Shape::xformVertexBaseColor(bool isHudElement, bool isNight, float nightFactor, TG_TypeShapePtr theShape, long j, TgXformFrame& f) {
	auto& redFinal = f.redFinal; auto& greenFinal = f.greenFinal; auto& blueFinal = f.blueFinal;
	auto& redAmb = f.redAmb; auto& greenAmb = f.greenAmb; auto& blueAmb = f.blueAmb;
	auto& redSpec = f.redSpec; auto& greenSpec = f.greenSpec; auto& blueSpec = f.blueSpec;
	auto& startVLight = f.startVLight; auto& doHotGreenTrace = f.doHotGreenTrace;
	redFinal=0, greenFinal=0, blueFinal=0;
	redAmb = 0, greenAmb = 0, blueAmb = 0;
	redSpec=0, greenSpec=0, blueSpec=0;
	
	if (lighteningLevel > 0)
	{
		redSpec = blueSpec = greenSpec = lighteningLevel;
	}

	startVLight = theShape->listOfTypeVertices[j].aRGBLight;
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
	// [PARITY_DIAG v2] is gated by MC2_OBJECT_PARITY_TRACE. Zero output in
	// production logs is expected and correct.
	doHotGreenTrace = (s_hotGreenLightTraceEnabled && !s_hotGreenLightTraceDone
	                        && (startVLight == 0xff00ff00) && !isNight
	                        && (nightFactor <= Stuff::SMALL));
	if (doHotGreenTrace) {
		s_hotGreenLightTraceDone = true;
		mc2::fmt::print_err("[PARITY_DIAG v2] CPU hot-green vertex={:d} normalShape=({:.4f},{:.4f},{:.4f}) s_numLights={:d}\n", mc2::fmt::as_i32(j), theShape->listOfTypeVertices[j].normal.x, theShape->listOfTypeVertices[j].normal.y, theShape->listOfTypeVertices[j].normal.z, mc2::fmt::as_i32(static_cast<int>(s_numLights)));
		std::fflush(stderr);
	}
}

// Accumulate the world lights into one vertex colour.
void TG_Shape::xformVertexLights(TG_TypeShapePtr theShape, long j, TgXformFrame& f) {
	auto& redFinal = f.redFinal; auto& greenFinal = f.greenFinal; auto& blueFinal = f.blueFinal;
	auto& redAmb = f.redAmb; auto& greenAmb = f.greenAmb; auto& blueAmb = f.blueAmb;
	auto& redSpec = f.redSpec; auto& greenSpec = f.greenSpec; auto& blueSpec = f.blueSpec;
	auto& doHotGreenTrace = f.doHotGreenTrace;
	if (useVertexLighting && (Environment.Renderer != 3))
	{
		if (!isSpotlight && !isWindow)
		{
			for (long i=0;i<s_numLights;i++)
			{
				if ((s_listOfLights[i] != nullptr) && (s_listOfLights[i]->active))
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
								mc2::fmt::print_err("[PARITY_DIAG v2] CPU light[{:d}] AMBIENT aRGB=0x{:08X} ambR={:d} ambG={:d} ambB={:d}\n", mc2::fmt::as_i32(i), mc2::fmt::as_u32(static_cast<unsigned>(startLight)), mc2::fmt::as_u32(redAmb), mc2::fmt::as_u32(greenAmb), mc2::fmt::as_u32(blueAmb));
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
								mc2::fmt::print_err("[PARITY_DIAG v2] CPU light[{:d}] INFINITE aRGB=0x{:08X} lightDir=({:.4f},{:.4f},{:.4f}) cosine={:.4f}\n", mc2::fmt::as_i32(i), mc2::fmt::as_u32(static_cast<unsigned>(startLight)), s_lightDir[i].x, s_lightDir[i].y, s_lightDir[i].z, cosine);
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
								glm::vec3 MirrorVector(listOfVertices[i].normal);
								MirrorVector *= mirrorScalar;
								MirrorVector.Subtract(MirrorVector,s_lightDir[i]);
								MirrorVector.Normalize(MirrorVector);

								glm::vec3 SpecPoint;
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
							glm::vec3 vertexToLight;
							vertexToLight = mc2::math::getTranslation(s_lightToShape[i]);
							vertexToLight -= theShape->listOfTypeVertices[j].position;

							float length = glm::length(vertexToLight);

							float falloff = 1.0f;

							float red,green,blue;

							if (s_listOfLights[i]->GetFalloff(length, falloff))
							{
								float cosine = -(glm::dot(s_lightDir[i], (theShape->listOfTypeVertices[j].normal)));

								red = float((startLight>>16) & 0x000000ff) * falloff;
								green = float((startLight>>8) & 0x000000ff) * falloff;
								blue = float((startLight) & 0x000000ff) * falloff;

								red *= cosine;
								green *= cosine;
								blue *= cosine;

								redFinal += static_cast<DWORD>(red);
								greenFinal += static_cast<DWORD>(green);
								blueFinal += static_cast<DWORD>(blue);
							}
						}
						break;

						case TG_LIGHT_POINT:
						{
							glm::vec3 vertexToLight;
							vertexToLight = s_lightDir[i];
							float length = glm::length(vertexToLight);

							if (length > Stuff::SMALL)
							{	
								vertexToLight = glm::normalize(vertexToLight);
	
								float falloff = 1.0f;
	
								if (s_listOfLights[i]->GetFalloff(length, falloff))
								{
									float cosine = glm::dot(vertexToLight, (theShape->listOfTypeVertices[j].normal));
	
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
	
										redSpec += static_cast<DWORD>(red);
										greenSpec += static_cast<DWORD>(green);
										blueSpec += static_cast<DWORD>(blue);
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

									redSpec += static_cast<DWORD>(red);
									greenSpec += static_cast<DWORD>(green);
									blueSpec += static_cast<DWORD>(blue);
								}
							}
						}
						break;
							  
						case TG_LIGHT_TERRAIN:
						{
							if (useShadows)
							{
								glm::vec3 vertexToLight;
								glm::vec3 pos = theShape->listOfTypeVertices[j].position;
								RotateLight(pos,yawRotation);
								vertexToLight = s_lightDir[i] + pos;
								float length = glm::length(vertexToLight);
	
								if (length > Stuff::SMALL)
								{	
									float falloff = 1.0f;
									if (s_listOfLights[i]->GetFalloff(length, falloff))
									{
										float red,green,blue;
		
										red = float((startLight>>16) & 0x000000ff) * falloff;
										green = float((startLight>>8) & 0x000000ff) * falloff;
										blue = float((startLight) & 0x000000ff) * falloff;
		
										listOfColors[j].redSpec = static_cast<DWORD>(red);
										listOfColors[j].greenSpec = static_cast<DWORD>(green);
										listOfColors[j].blueSpec = static_cast<DWORD>(blue);
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
							glm::vec3 vertexToLight;
							vertexToLight = s_lightDir[i];
							
							//-------------------------------------------------
							// Defines the actual spot of light on the ground
							float length = glm::length(vertexToLight);

							//-------------------------------------------------
							// Defines the REAL direction of the spot light.
							vertexToLight = s_spotDir[i];
							if (glm::length(vertexToLight) > Stuff::SMALL)
								vertexToLight = glm::normalize(vertexToLight);
							else
								length = 99999999999999.0f;

							float falloff = 1.0f;

							if (s_listOfLights[i]->GetFalloff(length, falloff))
							{
								float cosine = glm::dot(vertexToLight, (theShape->listOfTypeVertices[j].normal));

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

									redSpec += static_cast<DWORD>(red);
									greenSpec += static_cast<DWORD>(green);
									blueSpec += static_cast<DWORD>(blue);
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
			mc2::fmt::print_err("[PARITY_DIAG v2] CPU hot-green final: redFinal={:d} greenFinal={:d} blueFinal={:d} (before argb write: 0x{:08X})\n", mc2::fmt::as_u32(redFinal), mc2::fmt::as_u32(greenFinal), mc2::fmt::as_u32(blueFinal), mc2::fmt::as_u32(static_cast<unsigned>(((0xff << 24) + (redFinal << 16) + (greenFinal << 8) + blueFinal))));
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
}

// Write one vertex fog colour and apply the highlight.
void TG_Shape::xformVertexFog(TG_ShapeRecPtr parentNode, TG_TypeShapePtr theShape, long j, TgXformFrame& f) {
	auto& redSpec = f.redSpec; auto& greenSpec = f.greenSpec; auto& blueSpec = f.blueSpec;
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

// Accumulate the world lights into one visible face colour.
void TG_Shape::xformFaceLight(TG_TypeShapePtr theShape, long j, TgXformFrame& f) {
	auto& redFinal = f.redFinal; auto& greenFinal = f.greenFinal; auto& blueFinal = f.blueFinal;
	auto& redAmb = f.redAmb; auto& greenAmb = f.greenAmb; auto& blueAmb = f.blueAmb;
	auto& redSpec = f.redSpec; auto& greenSpec = f.greenSpec; auto& blueSpec = f.blueSpec;
	auto& startVLight = f.startVLight;
	redFinal = 0, greenFinal = 0, blueFinal = 0;
	redSpec=0, greenSpec=0, blueSpec=0;
	redAmb=0, greenAmb=0, blueAmb=0;
	
	if (useFaceLighting)
	{
		for (long i=0;i<s_numLights;i++)
		{
			if (s_listOfLights[i] != nullptr)
			{
				DWORD startVLight = s_listOfLights[i]->GetaRGB();
				switch (s_listOfLights[i]->lightType)
				{
					case TG_LIGHT_INFINITE:
					{
						if (glm::length(s_lightDir[i]) > Stuff::SMALL)
							s_lightDir[i] = glm::normalize(s_lightDir[i]);

						float cosine = -(glm::dot(s_lightDir[i], (theShape->listOfTypeTriangles[j].faceNormal)));

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
						if (glm::length(s_lightDir[i]) > Stuff::SMALL)
							s_lightDir[i] = glm::normalize(s_lightDir[i]);

						float cosine = -(glm::dot(s_lightDir[i], (theShape->listOfTypeTriangles[j].faceNormal)));

						glm::vec3 vertexToLight;
						vertexToLight = mc2::math::getTranslation(s_lightToShape[i]);
						vertexToLight -= theShape->listOfTypeVertices[theShape->listOfTypeTriangles[j].Vertices[0]].position;

						float length = glm::length(vertexToLight);

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

							redFinal += static_cast<DWORD>(red);
							greenFinal += static_cast<DWORD>(green);
							blueFinal += static_cast<DWORD>(blue);
						}
					}
					break;

					case TG_LIGHT_POINT:
					{
						glm::vec3 vertexToLight;
						vertexToLight = mc2::math::getTranslation(s_lightToShape[i]);
						vertexToLight -= theShape->listOfTypeVertices[theShape->listOfTypeTriangles[j].Vertices[0]].position;

						float length = glm::length(vertexToLight);

						vertexToLight = glm::normalize(vertexToLight);
						float falloff = 1.0f;

						if (s_listOfLights[i]->GetFalloff(length, falloff))
						{
							float cosine = -(glm::dot(vertexToLight, (theShape->listOfTypeTriangles[j].faceNormal)));

							float red,green,blue;

							if (cosine > 0.0f)
							{
								red = float((startVLight>>16) & 0x000000ff) * falloff;
								green = float((startVLight>>8) & 0x000000ff) * falloff;
								blue = float((startVLight) & 0x000000ff) * falloff;

								red *= cosine;
								green *= cosine;
								blue *= cosine;

								redSpec += static_cast<DWORD>(red);
								greenSpec += static_cast<DWORD>(green);
								blueSpec += static_cast<DWORD>(blue);
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
}

// Add the face colour to each corner vertex colour.
void TG_Shape::xformFaceCorners(BYTE alphaValue, TG_TypeShapePtr theShape, long j, TgXformFrame& f) {
	auto& redFinal = f.redFinal; auto& greenFinal = f.greenFinal; auto& blueFinal = f.blueFinal;
	auto& redSpec = f.redSpec; auto& greenSpec = f.greenSpec; auto& blueSpec = f.blueSpec;
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
}

// Queue the face triangle with its texture and render flags.
void TG_Shape::xformFaceQueue(bool isHudElement, BYTE alphaValue, bool isClamped, TG_TypeShapePtr theShape, long j) {
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
		mc2::renderSubmissions().addTriangle(0xffffffff,MC2_ISSPOTLGT);
	}
	else if (isWindow)
	{
		mc2::renderSubmissions().addTriangle(0xffffffff, MC2_DRAWALPHA);
	}
	else
	{
		//--------------------------------------------------
		// Any sorting we need to do occurs in here as well!
		// So, find out what texture this face is using and mark that texture as needing 3 more vertices.
		if (theShape->listOfTextures[theShape->listOfTypeTriangles[j].localTextureHandle].textureAlpha || (alphaValue != 0xff))
		{
			mc2::renderSubmissions().addTriangle(theShape->listOfTextures[theShape->listOfTypeTriangles[j].localTextureHandle].mcTextureNodeIndex,MC2_DRAWALPHA | addFlags);
		}
		else
		{
			mc2::renderSubmissions().addTriangle(theShape->listOfTextures[theShape->listOfTypeTriangles[j].localTextureHandle].mcTextureNodeIndex,MC2_DRAWSOLID | addFlags);
		}

	}
}
