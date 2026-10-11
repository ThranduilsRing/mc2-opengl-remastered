// MLR_I_TMesh: LightMapLighting (the light-map vertex lighting pass).
// Declared in the class headers; shared names in mlr_i_tmesh_internal.h.
#include "mlr_i_tmesh_internal.h"


extern RGBAColor errorColor;
extern bool
	CheckForBigTriangles(DynamicArrayOf<Vector2DScalar> *lightMapUVs, int stride);

//---------------------------------------------------------------------------
//
void
	MLR_I_TMesh::LightMapLighting(MLRLight *light)
{
	int i, j, k, len = numOfTriangles;
	LinearMatrix4D matrix = LinearMatrix4D::Identity;
	Point3D lightPosInShape, hitPoint;
	UnitVector3D up, left, forward;
	bool lm;
	Scalar f, rhf, falloff = 1.0f, distance;

	MLRLightMap *lightMap = light->GetLightMap();

	if( (!lightMap) || (!gEnableLightMaps) )
	{
		return;
	}

	Scalar bigUV = MLRState::GetMaxUV();
	int tooBig = 0;

	switch(light->GetLightType())
	{
		case MLRLight::PointLight:
		{
			Check_Object(lightMap);

			lightMap->AddState(referenceState.GetPriority()+1);
			
			light->GetInShapePosition(lightPosInShape);

			for(i=0,j=0,k=0;i<len;i++,j += 3)
			{
				if(testList[i] == 0)
				{
					continue;
				}

				f = facePlanes[i].GetDistanceTo(lightPosInShape);
				
				lm = false;

				if(f>0.0f && Cast_Object(MLRInfiniteLightWithFalloff*, light)->GetFalloff(f, falloff) == true)
				{
					rhf = 1.0f/f;

					matrix = LinearMatrix4D::Identity;
					matrix.AlignLocalAxisToWorldVector(facePlanes[i].normal, Z_Axis, X_Axis, Y_Axis);
					matrix.GetLocalLeftInWorld(&left);
					matrix.GetLocalUpInWorld(&up);
					matrix.GetLocalForwardInWorld(&forward);
					Verify(Small_Enough(up*left));

		#if defined(_ARMOR)
					Scalar diff = forward*left;
					Verify(Small_Enough(diff));
					diff = up*forward;
					Verify(Small_Enough(diff));
		#endif

					Check_Object(&forward);
					hitPoint.Multiply(forward, -f);
					hitPoint+=lightPosInShape;

					tooBig = 0;
					for(k=0;k<3;k++)
					{
						Vector3D vec(coords[index[k+j]]);
						vec-=hitPoint;

						(*lightMapUVs)[k][0] = -(left*vec)*rhf;
						(*lightMapUVs)[k][1] = -(up*vec)*rhf;

						if(
							(*lightMapUVs)[k][0] >= -0.5f && (*lightMapUVs)[k][0] <= 0.5f &&
							(*lightMapUVs)[k][1] >= -0.5f && (*lightMapUVs)[k][1] <= 0.5f

						) 
						{
							lm = true;
						}

						if(
							(*lightMapUVs)[k][0] < -bigUV || (*lightMapUVs)[k][0] > bigUV ||
							(*lightMapUVs)[k][1] < -bigUV || (*lightMapUVs)[k][1] > bigUV

						) 
						{
							tooBig++;
						}
					}
				}
				else
				{
					continue;
				}

				if(tooBig==0 && (lm == true || CheckForBigTriangles(lightMapUVs, 3) == true))
				{
					lightMap->SetPolygonMarker(0);
					lightMap->AddUShort(3);

#if 0
					Vector3D vec(coords[index[j]]);
					SPEW(("micgaert", "\nvertex1 = %f,%f,%f", vec.x, vec.y, vec.z));
					vec = coords[index[j+1]];
					SPEW(("micgaert", "vertex2 = %f,%f,%f", vec.x, vec.y, vec.z));
					vec = coords[index[j+2]];
					SPEW(("micgaert", "vertex3 = %f,%f,%f", vec.x, vec.y, vec.z));
					vec = facePlanes[i].normal;
					SPEW(("micgaert", "normal = %f,%f,%f", vec.x, vec.y, vec.z));
					SPEW(("micgaert", "forward = %f,%f,%f", forward.x, forward.y, forward.z));
					SPEW(("micgaert", "distance = %f", f));
					SPEW(("micgaert", "light = %f,%f,%f", lightPosInShape.x, lightPosInShape.y, lightPosInShape.z));
					SPEW(("micgaert", "projection = %f,%f,%f", hitPoint.x, hitPoint.y, hitPoint.z));
#endif

					Scalar sq_falloff
						= falloff*falloff*light->GetIntensity();
					RGBAColor color(sq_falloff, sq_falloff, sq_falloff, 1.0f);

					lightMap->AddColor(color);
					for(k=0;k<3;k++)
					{
						lightMap->AddCoord(coords[index[k+j]]);
					}
					for(k=0;k<3;k++)
					{
						lightMap->AddUVs((*lightMapUVs)[k][0]+0.5f, (*lightMapUVs)[k][1]+0.5f);
		//				DEBUG_STREAM << k << " " << lightMapUVs[k][0] << " " << lightMapUVs[k][0] << "\n";
					}
				}
			}
		}
		break;
		case MLRLight::SpotLight:
		{
			int behindCount = 0, falloffCount = 0;

			Check_Object(lightMap);

			lightMap->AddState(referenceState.GetPriority()+1);
			
			light->GetInShapePosition(matrix);
			lightPosInShape = matrix;

			Scalar tanSpeadAngle = Cast_Object(MLRSpotLight*, light)->GetTanSpreadAngle();

#ifndef TOP_DOWN_ONLY
			matrix.GetLocalLeftInWorld(&left);
			matrix.GetLocalUpInWorld(&up);
			matrix.GetLocalForwardInWorld(&forward);
#else
			forward = UnitVector3D(0.0f, -1.0f, 0.0);
			up = UnitVector3D(1.0f, 0.0f, 0.0);
			left = UnitVector3D(0.0f, 0.0f, 1.0);
#endif

			Verify(Small_Enough(up*left));

			for(i=0,j=0,k=0;i<len;i++,j += 3)
			{
				behindCount = 0;
				falloffCount = 0;

				if(testList[i] == 0)
				{
					continue;
				}

				lm = false;

				if(!facePlanes[i].IsSeenBy(lightPosInShape))
				{
					continue;
				}

#if SPEW_AWAY
				Scalar maxX, maxZ;
				Scalar minX, minZ;

				minX = maxX = coords[index[j]].x;
				minZ = maxZ = coords[index[j]].z;

				if(minX>coords[index[j+1]].x)
				{
					minX = coords[index[j+1]].x;
				}
				if(minX>coords[index[j+2]].x)
				{
					minX = coords[index[j+2]].x;
				}
				if(minZ>coords[index[j+1]].z)
				{
					minZ = coords[index[j+1]].z;
				}
				if(minX>coords[index[j+2]].z)
				{
					minZ = coords[index[j+2]].z;
				}
				if(maxX<coords[index[j+1]].x)
				{
					maxX = coords[index[j+1]].x;
				}
				if(maxX<coords[index[j+2]].x)
				{
					maxX = coords[index[j+2]].x;
				}
				if(maxZ<coords[index[j+1]].z)
				{
					maxZ = coords[index[j+1]].z;
				}
				if(maxX<coords[index[j+2]].z)
				{
					maxZ = coords[index[j+2]].z;
				}

				if(lightPosInShape.x > minX && lightPosInShape.x < maxX && lightPosInShape.z > minZ && lightPosInShape.z < maxZ)
				{
					SPEW(("micgaert", "On Target !!"));
				}
#endif

				tooBig = 0;
				for(k=0;k<3;k++)
				{
					Vector3D vec;
					Scalar oneOver;

					vec.Subtract(coords[index[k+j]], lightPosInShape);

#ifndef TOP_DOWN_ONLY
					distance = (vec*forward);
#else
					distance = -vec.y;
#endif

#if SPEW_AWAY
					SPEW(("micgaert", "vertex%d = %f,%f,%f", k, coords[index[k+j]].x, coords[index[k+j]].y, coords[index[k+j]].z));
					SPEW(("micgaert", "distance = %f", distance));
#endif
					if(distance > SMALL)
					{
						if(Cast_Object(MLRInfiniteLightWithFalloff*, light)->GetFalloff(distance, falloff) == false)
						{
							falloffCount++;
						}
						(*lightMapSqFalloffs)[k] = falloff*falloff*light->GetIntensity();

						oneOver
#if 0
							= 1.0f/(2.0f*distance*tanSpeadAngle);
#else
							= OneOverApproximate(2.0f*distance*tanSpeadAngle);
#endif
					}
					else
					{
						behindCount++;
						oneOver = 1.0f/50.0f;
						(*lightMapSqFalloffs)[k] = 0.0f;
#if SPEW_AWAY
						SPEW(("micgaert", "Behind"));
#endif
					}

#ifndef TOP_DOWN_ONLY
					(*lightMapUVs)[k][0] = (left*vec) * oneOver;
					(*lightMapUVs)[k][1] = -(up*vec) * oneOver;
#else
					(*lightMapUVs)[k][0] = vec.x * oneOver;
					(*lightMapUVs)[k][1] = -vec.z * oneOver;
#endif

#if SPEW_AWAY
					SPEW(("micgaert", "uv%d = %f,%f", k, (*lightMapUVs)[k][0], (*lightMapUVs)[k][1]));
#endif
					if(
						(*lightMapUVs)[k][0] >= -0.5f && (*lightMapUVs)[k][0] <= 0.5f &&
						(*lightMapUVs)[k][1] >= -0.5f && (*lightMapUVs)[k][1] <= 0.5f

					) 
					{
						lm = true;
					}
					if(
						(*lightMapUVs)[k][0] < -bigUV || (*lightMapUVs)[k][0] > bigUV ||
						(*lightMapUVs)[k][1] < -bigUV || (*lightMapUVs)[k][1] > bigUV

					) 
					{
						tooBig++;
					}
				}
#if 1
				if(	
					tooBig == 0 
					&& behindCount < 3  
					&& falloffCount < 3 
 					&& ((lm == true)	|| CheckForBigTriangles(lightMapUVs, 3) == true)
				)
				{
					lightMap->SetPolygonMarker(1);
					lightMap->AddUShort(3);

					for(k=0;k<3;k++)
					{
						lightMap->AddCoord(coords[index[k+j]]);
					}
					for(k=0;k<3;k++)
					{
						lightMap->AddColor((*lightMapSqFalloffs)[k], (*lightMapSqFalloffs)[k], (*lightMapSqFalloffs)[k], 1.0f);
					}
					for(k=0;k<3;k++)
					{
						lightMap->AddUVs((*lightMapUVs)[k][0]+0.5f, (*lightMapUVs)[k][1]+0.5f);
		//				DEBUG_STREAM << k << " " << lightMapUVs[k][0] << " " << lightMapUVs[k][0] << "\n";
					}
	#if SPEW_AWAY
					SPEW(("micgaert", "See the Light !"));
	#endif
				}
	#if SPEW_AWAY
				Vector3D vec = facePlanes[i].normal;
				SPEW(("micgaert", "normal = %f,%f,%f", vec.x, vec.y, vec.z));
				SPEW(("micgaert", "forward = %f,%f,%f", forward.x, forward.y, forward.z));
				SPEW(("micgaert", "left = %f,%f,%f", left.x, left.y, left.z));
				SPEW(("micgaert", "up = %f,%f,%f", up.x, up.y, up.z));
				SPEW(("micgaert", "light = %f,%f,%f\n", lightPosInShape.x, lightPosInShape.y, lightPosInShape.z));
	#endif
#else
				if(tooBig != 0)
				{
					lightMap->SetPolygonMarker(1);
					lightMap->AddUShort(3);

					for(k=0;k<3;k++)
					{
						lightMap->AddCoord(coords[index[k+j]]);
					}
					for(k=0;k<3;k++)
					{
						lightMap->AddColor(RGBAColor(0.0f, 0.0f, 0.5f, 1.0f));
					}
					for(k=0;k<3;k++)
					{
						lightMap->AddUVs(0.5f, 0.5f);
		//				DEBUG_STREAM << k << " " << lightMapUVs[k][0] << " " << lightMapUVs[k][0] << "\n";
					}
				} 
				else if(behindCount != 0)
				{
					lightMap->SetPolygonMarker(1);
					lightMap->AddUShort(3);

					for(k=0;k<3;k++)
					{
						lightMap->AddCoord(coords[index[k+j]]);
					}
					for(k=0;k<3;k++)
					{
						lightMap->AddColor(RGBAColor(0.5f, 0.0f, 0.0f, 1.0f));
					}
					for(k=0;k<3;k++)
					{
						lightMap->AddUVs(0.5f, 0.5f);
		//				DEBUG_STREAM << k << " " << lightMapUVs[k][0] << " " << lightMapUVs[k][0] << "\n";
					}
				} 
				else	if(behindCount == 0 && (lm == true || CheckForBigTriangles(&lightMapUVs, 3) == true) )
				{
					lightMap->SetPolygonMarker(1);
					lightMap->AddUShort(3);

					for(k=0;k<3;k++)
					{
						lightMap->AddCoord(coords[index[k+j]]);
					}
					for(k=0;k<3;k++)
					{
						lightMap->AddColor(lightMapSqFalloffs[k], lightMapSqFalloffs[k], lightMapSqFalloffs[k], 1.0f);
					}
					for(k=0;k<3;k++)
					{
						lightMap->AddUVs(lightMapUVs[k][0]+0.5f, lightMapUVs[k][1]+0.5f);
		//				DEBUG_STREAM << k << " " << lightMapUVs[k][0] << " " << lightMapUVs[k][0] << "\n";
					}

				}
				else if(CheckForBigTriangles(&lightMapUVs, 3) == false)
				{
					lightMap->SetPolygonMarker(1);
					lightMap->AddUShort(3);

					for(k=0;k<3;k++)
					{
						lightMap->AddCoord(coords[index[k+j]]);
					}
					for(k=0;k<3;k++)
					{
						lightMap->AddColor(errorColor);
					}
					for(k=0;k<3;k++)
					{
						lightMap->AddUVs(0.5f, 0.5f);
		//				DEBUG_STREAM << k << " " << lightMapUVs[k][0] << " " << lightMapUVs[k][0] << "\n";
					}
				}
#endif
			}
		}
		break;
		default:
			STOP(("MLR_I_PMesh::LightMapLighting: What you want me to do ?"));
		break;
	}
}
