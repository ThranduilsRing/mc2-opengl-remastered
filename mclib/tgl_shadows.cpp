// TG shapes: shadow transform and shadow rendering.
// Declared in the class headers; shared names in tgl_internal.h.
#include "tgl_internal.h"

//
void TG_Shape::MultiTransformShadows (Stuff::Point3D *pos, Stuff::LinearMatrix4D *s2w, float rotation)
{
	if (!numVertices)		//WE are the root Shape which may have no shape or a helper shape which defintely has no shape!
		return;
		
	if (noShadow)
		return;
		
	if (!listOfVertices ||
		!listOfColors ||
		!listOfShadowTVertices ||
		!listOfTriangles ||
		!listOfVisibleFaces ||
		!listOfVisibleShadows)
		return;

	Stuff::Matrix4D identityMatrix;
	identityMatrix.BuildIdentity();
	
	TG_TypeShapePtr theShape = (TG_TypeShapePtr)myType;
	//
	// 
	//
	// Again, note that after first iteration of infinite light we only need to transform infinite shadows!
	Stuff::Matrix4D shapeToLocal;
	Stuff::Matrix4D shapeToClip;
	
	long shadowNum = 0;

	memset(shadowsVisible,0,sizeof(bool) * MAX_SHADOWS);

		//-------------------------------------------------------
		// Now, for each light IN RANGE
		// Use formula form Blinn-Trip Down the Graphics Pipeline-Chapter 6
		for (long i=0;i<s_numLights;i++)
		{
			if ((s_listOfLights[i] != nullptr) && (s_listOfLights[i]->active) && (shadowNum < MAX_SHADOWS))
			{
				switch (s_listOfLights[i]->lightType)
				{
					case TG_LIGHT_AMBIENT:
					{
						//Ambient light casts no shadows!
					}
					break;

					case TG_LIGHT_INFINITE:
					{
						//----------------------------------------------
						// The Sun casts no shadows at night!!!
						if (!eye->getIsNight())
						{
							Stuff::Vector3D s_lightDir = s_rootLightDir[i];
							RotateLight(s_lightDir,rotation);
							
							for (long j=0;j<numVertices;j++)
							{
								long index = j + (shadowNum * numVertices);

								{
									if (listOfShadowVertices[index].bDataIsNotValid)
									{
										recalcShadows = true;
										listOfShadowVertices[index].bDataIsNotValid = false;
									}
								}

								if (recalcShadows)	//Otherwise, just project last known DATA!!
													//This should almost never be true for any non-moving object.
													//Movers will recalc every frame.  Non-Movers will recalc 
													//when the sun moves enough!
								{
									Stuff::Point3D post = theShape->listOfTypeVertices[j].position;
									Stuff::Vector4D up;
									Stuff::Point3D s_position;
									shapeToLocal.Multiply(*s2w,identityMatrix);
									up.Multiply(post,shapeToLocal);

									//Everything is in WORLD coords now.
									//But this assumes terrain is FLAT and at zero elevation.
									// Thus, we must translate up to tangent plane at terrain position
									up.y -= pos->y;
									if (up.y <= 0.0f)
									{
										s_position.x = up.x;
										s_position.z = up.z;
									}
									else
									{
										float zFactor = up.y / s_lightDir.y;
										s_position.x = up.x - (zFactor * s_lightDir.x);
										s_position.z = up.z - (zFactor * s_lightDir.z);
									}
									
									s_position.y = 0.0f;
			
									
									Stuff::Vector3D rPos;
									rPos.x = -s_position.x;
									rPos.y = s_position.z;
									rPos.z = land->getTerrainElevation(rPos);
									
									listOfShadowVertices[index].position = rPos;
								}
								
								//The transformed position and fog data DO NOT NEED TO EXIST all of time.
								// ONLY the calced position data is invariant.  The other two could come
								// from a common pool and save us SCADS of RAM!!
								// Do this!!!!  I DID!
								// -fs
								// [PROJECTZ:ScreenXYOracle id=tgl_shadow_vertex_a]
								eye->projectForScreenXY(listOfShadowVertices[index].position,listOfShadowTVertices[index].transformedPosition);
								shadowsVisible[shadowNum] = true;

		
								DWORD fogRGB = 0xff000000;
								if (useFog)
								{
									if (pos->y < fogStart)
									{
										float fogFactor = fogStart - pos->y;
										if (fogFactor < 0.0)
											fogRGB = (0xff<<24);
										else
										{
											fogFactor /= (fogStart - fogFull);
											if (fogFactor <= 1.0)
											{
												fogFactor *= fogFactor;
												fogFactor = 1.0 - fogFactor;
												fogFactor *= 256.0;
											}
											else
											{
												fogFactor = 256.0;
											}
		
											unsigned char fogResult = float2long(fogFactor);
											fogRGB = (fogResult << 24);
										}
									}
									else
									{
										fogRGB = (0xff<<24);
									}
								}
								
								if (useFog && Camera::HazeFactor != 0.0f)
								{
									float fogFactor = 1.0 - Camera::HazeFactor;
									DWORD distFog = float2long(fogFactor * 255.0f);
									distFog <<= 24;
									
									if (distFog < fogRGB)
										fogRGB = distFog;
								}

								listOfShadowTVertices[index].fRGBFog = fogRGB;
				   			}

							shadowNum++;
						}
					}
					break;

					case TG_LIGHT_INFINITEWITHFALLOFF:
					{
						//Not planning to use this type of light anymore.  No shadows for now.
					}
					break;

					case TG_LIGHT_POINT:
					case TG_LIGHT_SPOT:
					{
						if (useLocalShadows)
						{
							//------------------------------------------------------------------------------------------
							// Point light does cast a shadow.  LightDir Will change every frame relative to the point.
							// I.e. we DO need to recalc every frame.
							
							Stuff::Point3D vertexToLight;
							vertexToLight = s_lightDir[i];
							float length = vertexToLight.GetApproximateLength();
							
							vertexToLight = s_rootLightDir[i];
							float spotLength = s_listOfLights[i]->maxSpotLength - vertexToLight.GetApproximateLength();
							if (spotLength < 50.0f)
								spotLength = 50.0f;
									
							float falloff = 1.0f;
	
							//Lights do not cast a shadow unless they are intense enough!!
							if (s_listOfLights[i]->GetFalloff(length, falloff) && (falloff > 0.5f))
							{
								for (long j=0;j<numVertices;j++)
								{
									long index = j + (shadowNum * numVertices);
									
									Stuff::Point3D post = theShape->listOfTypeVertices[j].position;
									Stuff::Vector4D up;
									Stuff::Point3D s_position;
									shapeToLocal.Multiply(*s2w,identityMatrix);
									up.Multiply(post,shapeToLocal);
	
									//Everything is in WORLD coords now.
									//But this assumes terrain is FLAT and at zero elevation.
									// Thus, we must translate up to tangent plane at terrain position
									up.y -= pos->y;
	
									shadowsVisible[shadowNum] = true;
									if (up.y <= 0.0f)
									{
										s_position.x = up.x;
										s_position.z = up.z;
									}
									else
									{
										float zFactor = up.y / spotLength;
										s_position.x = up.x - (zFactor * -s_rootLightDir[i].x);
										s_position.z = up.z - (zFactor * -s_rootLightDir[i].z);
									}
									
									s_position.y = 0.0f;
		
	
									Stuff::Vector3D rPos;
									rPos.x = -s_position.x;
									rPos.y = s_position.z;
									rPos.z = land->getTerrainElevation(rPos);
	
									listOfShadowVertices[index].position = rPos;
	
									//The transformed position and fog data DO NOT NEED TO EXIST all of time.
									// ONLY the calced position data is invariant.  The other two could come
									// from a common pool and save us SCADS of RAM!!
									// Do this!!!!  I DID!
									// -fs
									// [PROJECTZ:ScreenXYOracle id=tgl_shadow_vertex_b]
									eye->projectForScreenXY(listOfShadowVertices[index].position,listOfShadowTVertices[index].transformedPosition);
									shadowsVisible[shadowNum] = true;
	
									DWORD fogRGB = 0xff000000;
									if (useFog)
									{
										if (pos->y < fogStart)
										{
											float fogFactor = fogStart - pos->y;
											if (fogFactor < 0.0)
												fogRGB = (0xff<<24);
											else
											{
												fogFactor /= (fogStart - fogFull);
												if (fogFactor <= 1.0)
												{
													fogFactor *= fogFactor;
													fogFactor = 1.0 - fogFactor;
													fogFactor *= 256.0;
												}
												else
												{
													fogFactor = 256.0;
												}
			
												unsigned char fogResult = float2long(fogFactor);
												fogRGB = (fogResult << 24);
											}
										}
										else
										{
											fogRGB = (0xff<<24);
										}
									}
									
									if (useFog && Camera::HazeFactor != 0.0f)
									{
										float fogFactor = 1.0 - Camera::HazeFactor;
										DWORD distFog = float2long(fogFactor * 255.0f);
										distFog <<= 24;
										
										if (distFog < fogRGB)
											fogRGB = distFog;
									}
	
									listOfShadowTVertices[index].fRGBFog = fogRGB;
								}
	
								shadowNum++;
							}
							else
							{
								shadowsVisible[shadowNum] = false;
							}
						}
					}
					break;
				}
			}
		}

	numVisibleShadows = 0;			//Reset Visible Shadows

	long totalShadows = 0;
	long j=0;
	for (j=0;j<MAX_SHADOWS;j++)
		if (shadowsVisible[j])
			totalShadows++;
		
	if (totalShadows)
	{
		for (j=0;j<numTriangles;j++)
		{
			listOfVisibleShadows[numVisibleShadows] = j;
			numVisibleShadows++;
			
			TG_TypeTriangle triType = theShape->listOfTypeTriangles[j];
			
			//--------------------------------------------------
			// Any sorting we need to do occurs in here as well!
			// So, find out what texture this face is using and mark that texture as needing 3 more vertices.
			if (theShape->listOfTextures[triType.localTextureHandle + (theShape->numTextures>>1)].gosTextureHandle != 0xffffffff)
			{
				for (long k=0;k<totalShadows;k++)
					mcTextureManager->addTriangle(theShape->listOfTextures[triType.localTextureHandle + (theShape->numTextures>>1)].mcTextureNodeIndex,MC2_DRAWALPHA | MC2_ISSHADOWS);
			}
			else
			{
				for (long k=0;k<totalShadows;k++)
					mcTextureManager->addTriangle(theShape->listOfTextures[triType.localTextureHandle].mcTextureNodeIndex,MC2_DRAWALPHA | MC2_ISSHADOWS);
			}
		}
	}
}	

//------------------------------------------------------------------------------------------------------------------------------------
// This functions draws the shadows.  May want zCompare and zWrite off at this point, hence separate render to facilitate draw order.
// So shadows fall across all 3d objects on terrain.
long TG_Shape::RenderShadows (long startFace)
{
	if (!renderTGLShapes)
		return(startFace);
		
 	if (!numVertices)		//WE are the root Shape which may have no shape or a helper shape which defintely has no shape!
		return(startFace);
		
	if (!numVisibleShadows)
		return startFace;
		
	if (S_strnicmp(getNodeName(),"SpotLight_",10) == 0)
		return startFace;
		
	if (!listOfVertices ||
		!listOfColors ||
		!listOfShadowTVertices ||
		!listOfTriangles ||
		!listOfVisibleFaces ||
		!listOfVisibleShadows ||
		(/*(lastTurnTransformed != (turn-1)) &&*/ (lastTurnTransformed != turn)))
		return startFace;

 	if (fogColor != 0xffffffff)
	{
		gos_SetRenderState( gos_State_Fog, fogColor); //sebi
	}
	else
	{
		gos_SetRenderState( gos_State_Fog, 0);
	}

	TG_TypeShapePtr theShape = (TG_TypeShapePtr)myType;
	for (long i=0;i<MAX_SHADOWS;i++)
	{
		if (shadowsVisible[i])
		{
			for (long j=0;j<numVisibleShadows && listOfShadowTVertices;j++)
			{
				DWORD realStart = (j + startFace) % 64;
				if (listOfVisibleShadows[j] < numTriangles)
				{

					TG_TypeTriangle triType = theShape->listOfTypeTriangles[listOfVisibleShadows[j]];
	
					TG_ShadowVertexTemp vertex0 = listOfShadowTVertices[triType.Vertices[0] + (i * numVertices)];
					TG_ShadowVertexTemp vertex1 = listOfShadowTVertices[triType.Vertices[1] + (i * numVertices)];
					TG_ShadowVertexTemp vertex2 = listOfShadowTVertices[triType.Vertices[2] + (i * numVertices)];
	
					gos_VERTEX gVertex[3];
	
					gVertex[0].x		= vertex0.transformedPosition.x;
					gVertex[0].y		= vertex0.transformedPosition.y;
					gVertex[0].z		= vertex0.transformedPosition.z + (1.0f/60000.0f)*(realStart);
					gVertex[0].rhw		= vertex0.transformedPosition.w;
					gVertex[0].u		= triType.uvdata.u0;
					gVertex[0].v		= triType.uvdata.v0;
					gVertex[0].argb		= 0x3f000000;
					gVertex[0].frgb		= vertex0.fRGBFog;
	
					gVertex[1].x		= vertex1.transformedPosition.x;
					gVertex[1].y		= vertex1.transformedPosition.y;
					gVertex[1].z		= vertex1.transformedPosition.z + (1.0f/60000.0f)*(realStart);
					gVertex[1].rhw		= vertex1.transformedPosition.w;
					gVertex[1].u		= triType.uvdata.u1;
					gVertex[1].v		= triType.uvdata.v1;
					gVertex[1].argb		= 0x3f000000;
					gVertex[1].frgb		= vertex1.fRGBFog;
	
					gVertex[2].x		= vertex2.transformedPosition.x;
					gVertex[2].y		= vertex2.transformedPosition.y;
					gVertex[2].z		= vertex2.transformedPosition.z + (1.0f/60000.0f)*(realStart);
					gVertex[2].rhw		= vertex2.transformedPosition.w;
					gVertex[2].u		= triType.uvdata.u2;
					gVertex[2].v		= triType.uvdata.v2;
					gVertex[2].argb		= 0x3f000000;
					gVertex[2].frgb		= vertex2.fRGBFog;
	
					if (drawOldWay)
					{
						if (theShape->listOfTextures[triType.localTextureHandle + (theShape->numTextures>>1)].gosTextureHandle != 0xffffffff)
							gos_SetRenderState( gos_State_Texture, theShape->listOfTextures[triType.localTextureHandle + (theShape->numTextures>>1)].gosTextureHandle );
						else if (theShape->listOfTextures[triType.localTextureHandle].gosTextureHandle != 0xffffffff)
							gos_SetRenderState( gos_State_Texture, theShape->listOfTextures[triType.localTextureHandle].gosTextureHandle );
						else
							gos_SetRenderState( gos_State_Texture, 0 );
							
						if ((gVertex[0].z >= 0.0f) &&
							(gVertex[0].z < 1.0f) &&
							(gVertex[1].z >= 0.0f) &&  
							(gVertex[1].z < 1.0f) && 
							(gVertex[2].z >= 0.0f) &&  
							(gVertex[2].z < 1.0f))
						{
							gos_DrawTriangles(gVertex, 3);
						}
					}
					else
					{
						if ((gVertex[0].z >= 0.0f) &&
							(gVertex[0].z < 1.0f) &&
							(gVertex[1].z >= 0.0f) &&  
							(gVertex[1].z < 1.0f) && 
							(gVertex[2].z >= 0.0f) &&  
							(gVertex[2].z < 1.0f))
						{
							if (theShape->listOfTextures[triType.localTextureHandle + (theShape->numTextures>>1)].gosTextureHandle != 0xffffffff)
							{
								mcTextureManager->addVertices(theShape->listOfTextures[triType.localTextureHandle + (theShape->numTextures>>1)].mcTextureNodeIndex,gVertex,MC2_ISSHADOWS | MC2_DRAWALPHA);
							}
							else
							{
								mcTextureManager->addVertices(theShape->listOfTextures[triType.localTextureHandle].mcTextureNodeIndex,gVertex,MC2_ISSHADOWS | MC2_DRAWALPHA);
							}
						}
					}
				}
			}
		}
	}

	gos_SetRenderState( gos_State_Fog, 0);

	return(startFace + numVisibleShadows);
}
