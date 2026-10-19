//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"

#if defined(TRACE_ENABLED) && defined(MLR_TRACE)
	BitTrace *MLR_I_PMesh_Clip;
#endif

#define UV_TEST 0

extern DWORD gEnableLightMaps;

//#############################################################################
//###### MLRIndexedPolyMesh with no color no lighting one texture layer  ######
//#############################################################################

MLR_I_PMesh::ClassData*
	MLR_I_PMesh::DefaultData = nullptr;

DynamicArrayOf<Vector2DScalar> *lightMapUVs;
DynamicArrayOf<Scalar> *lightMapSqFalloffs;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_PMesh::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLR_I_PMeshClassID,
			"MidLevelRenderer::MLR_I_PMesh",
			MLRIndexedPrimitiveBase::DefaultData,
			(MLRPrimitiveBase::Factory)&Make
		);
	Register_Object(DefaultData);

	#if defined(TRACE_ENABLED) && defined(MLR_TRACE)
		MLR_I_PMesh_Clip = new BitTrace("MLR_I_PMesh_Clip");
		Register_Object(MLR_I_PMesh_Clip);
	#endif

	lightMapUVs = new DynamicArrayOf<Vector2DScalar>(Limits::Max_Number_Vertices_Per_Polygon);
	Register_Object(lightMapUVs);

	lightMapSqFalloffs = new DynamicArrayOf<Scalar> (Limits::Max_Number_Vertices_Per_Polygon);
	Register_Object(lightMapSqFalloffs);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_PMesh::TerminateClass()
{
	Unregister_Object(lightMapSqFalloffs);
	delete lightMapSqFalloffs;

	Unregister_Object(lightMapUVs);
	delete lightMapUVs;

	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;

	#if defined(TRACE_ENABLED) && defined(MLR_TRACE)
		Unregister_Object(MLR_I_PMesh_Clip);
		delete MLR_I_PMesh_Clip;
	#endif
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_PMesh::MLR_I_PMesh(
	ClassData *class_data,
	MemoryStream *stream,
	int version
):
	MLRIndexedPrimitiveBase(class_data, stream, version)
{
	Check_Pointer(this);
	Check_Pointer(stream);
	Verify(gos_GetCurrentHeap() == Heap);

	facePlanes.SetLength(GetNumPrimitives());
	testList.SetLength(GetNumPrimitives());

	FindFacePlanes();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_PMesh::MLR_I_PMesh(ClassData *class_data):
	MLRIndexedPrimitiveBase(class_data)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);

	drawMode = SortData::TriIndexedList;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/*
void
	MLR_I_PMesh::Copy(MLRIndexedPolyMesh *polyMesh)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);

	int len;
	unsigned short *_index;
	glm::vec3 *_coords;
	Vector2DScalar *_texCoords;
	unsigned char *_lengths;

	polyMesh->GetCoordData(&_coords, &len);
	SetCoordData(_coords, len);

	polyMesh->GetIndexData(&_index, &len);
	SetIndexData(_index, len);

	polyMesh->GetSubprimitiveLengths(&_lengths);
	SetSubprimitiveLengths(_lengths, polyMesh->GetNumPrimitives());

	facePlanes.SetLength(GetNumPrimitives());
	testList.SetLength(GetNumPrimitives());

	FindFacePlanes();

	polyMesh->GetTexCoordData(&_texCoords, &len);
	SetTexCoordData(_texCoords, len);

	referenceState = polyMesh->GetReferenceState();

	visibleIndexedVerticesKey = false;
	visibleIndexedVertices.SetLength(coords.GetLength());
}
*/

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_PMesh::~MLR_I_PMesh()
{
	Check_Object(this);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_PMesh*
	MLR_I_PMesh::Make(
		MemoryStream *stream,
		int version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	MLR_I_PMesh *mesh = new MLR_I_PMesh(DefaultData, stream, version);
	gos_PopCurrentHeap();

	return mesh;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_PMesh::Save(MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);

	MLRIndexedPrimitiveBase::Save(stream);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_PMesh::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_PMesh::InitializeDrawPrimitive(unsigned char vis, int parameter)
{
	MLRIndexedPrimitiveBase::InitializeDrawPrimitive(vis, parameter);

	if(parameter & 1)
	{
		ResetTestList();
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_PMesh::SetSubprimitiveLengths (unsigned char *data, int numPrimitives)
{
	Check_Object(this); 
	Verify(gos_GetCurrentHeap() == Heap);

	lengths.AssignData(data, numPrimitives);

	facePlanes.SetLength(numPrimitives);

	testList.SetLength(numPrimitives);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_PMesh::GetSubprimitiveLengths (unsigned char **data, int *l)
{
	Check_Object(this);
	*l = static_cast<int>(lengths.GetLength());
	*data = lengths.GetData();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_PMesh::FindFacePlanes()
{
	Check_Object(this); 

	int i, j, stride, numPrimitives = GetNumPrimitives();
	glm::vec3 v;

	Verify(index.GetLength() > 0);

	for(i=0,j=0;i<numPrimitives;i++)
	{
		stride = lengths[i];

		facePlanes[i].buildPlane(
			coords[index[j]],
			(coords[index[j+1]]),
			(coords[index[j+2]])
		);

		j += stride;
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
int
	MLR_I_PMesh::FindBackFace(const glm::vec3& u)
{
	Check_Object(this);

	int i, numPrimitives = GetNumPrimitives();
	int ret = 0, len = static_cast<int>(lengths.GetLength());
	unsigned char *iPtr;
	const mc2::math::Plane *p;

	if(len <= 0)
	{
		visible = 0;

		return 0;
	}

	p = &facePlanes[0];
	iPtr = &testList[0];

	if(state.GetBackFaceMode() == MLRState::BackFaceOffMode)
	{
		ResetTestList();
		ret = 1;
	}
	else
	{
		for(i=0;i<numPrimitives;i++,p++,iPtr++)
		{
			*iPtr = (p->distanceTo(u) >= 0.0f) ? static_cast<unsigned char>(0xff) : static_cast<unsigned char>(0);
			
			ret += *iPtr;
		}

		visible = ret ? static_cast<unsigned char>(1) : static_cast<unsigned char>(0);
	}

	visible = ret ? static_cast<unsigned char>(1) : static_cast<unsigned char>(0);

	FindVisibleVertices();

	return ret;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_PMesh::ResetTestList()
{
	int i, numPrimitives = GetNumPrimitives();
	unsigned char *iPtr = &testList[0];

	for(i=0;i<numPrimitives;i++,iPtr++)
	{
		*iPtr = 1;
	}

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
int
	MLR_I_PMesh::FindVisibleVertices()
{
	Check_Object(this);
	Verify(index.GetLength() > 0);

	int ret, i, j, k, stride, len = static_cast<int>(lengths.GetLength());

	Verify(index.GetLength() > 0);
	
	for(i=0,j=0,ret=0;i<len;i++)
	{
		stride = lengths[i];

		Verify(stride >= 3);

		if(testList[i] == 0)
		{
			j += stride;
			continue;
		}

		for(k=j;k<j+stride;k++)
		{
			visibleIndexedVertices[index[k]] = 1;
			ret++;
		}

		j += stride;
	}
	visibleIndexedVerticesKey = true;

	return ret;
}

extern DWORD gEnableTextureSort, gEnableAlphaSort;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_PMesh::Transform(glm::mat4 *mat)
{
	Check_Object(this);

	Start_Timer(Transform_Time);

	Verify(index.GetLength() > 0);
	int i, len = static_cast<int>(coords.GetLength());

	if(visibleIndexedVerticesKey == false)
	{
		FindVisibleVertices();
	}

	Stuff::Vector4D *v4d = transformedCoords->GetData();
	glm::vec3 *p3d = coords.GetData();
	MLRClippingState *cs = clipPerVertex->GetData();
	unsigned char *viv = visibleIndexedVertices.GetData();

	for(i=0;i<len;i++,p3d++,v4d++,cs++,viv++)
	{
		if(*viv == 0)
		{
			continue;
		}
		
		v4d->MultiplyPoint(*p3d, *mat);
#ifdef LAB_ONLY
		Set_Statistic(TransformedVertices, TransformedVertices+1);
#endif

		cs->Clip4dVertex(v4d);
//
//--------------------------------------------------------
// I claims all vertices are in. lets check it. who knows
//--------------------------------------------------------
//
#ifdef LAB_ONLY
		if( (*cs)==0)
		{
#if defined(_ARMOR)
			if(ArmorLevel > 3)
			{
//
//--------------------------------------------------------
// I claims all vertices are in. lets check it. who knows
//--------------------------------------------------------
//
				Verify(v4d->x >= 0.0f && v4d->x <= v4d->w );
				Verify(v4d->y >= 0.0f && v4d->y <= v4d->w );
				Verify(v4d->z >= 0.0f && v4d->z <= v4d->w );
			}
#endif
			Set_Statistic(NonClippedVertices, NonClippedVertices+1);
		}
		else
		{
			Set_Statistic(ClippedVertices, ClippedVertices+1);
		}
#endif
	}
	Stop_Timer(Transform_Time);
}

#undef I_SAY_YES_TO_DUAL_TEXTURES
#undef I_SAY_YES_TO_COLOR
#undef I_SAY_YES_TO_LIGHTING

#define CLASSNAME MLR_I_PMesh

#if defined(TRACE_ENABLED) && defined(MLR_TRACE)
	#define SET_MLR_PMESH_CLIP() MLR_I_PMesh_Clip->Set()
	#define CLEAR_MLR_PMESH_CLIP() MLR_I_PMesh_Clip->Clear()
#else
	#define SET_MLR_PMESH_CLIP()
	#define CLEAR_MLR_PMESH_CLIP()
#endif

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	This include contains follwing functions:
//	void MLR_I_PMesh::TransformNoClip(Matrix4D*, GOSVertexPool*);
//	int MLR_I_PMesh::Clip(MLRClippingState, GOSVertexPool*);
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

#include<mlr/mlrprimitiveclipping.hpp>

#undef CLASSNAME

//---------------------------------------------------------------------------
//
void
	MLR_I_PMesh::Lighting(MLRLight* const* lights, int nrLights)
{
	int i;
	MLRLightMap *lightMap;

	for(i=0;i<nrLights;i++)
	{
		lightMap = lights[i]->GetLightMap();

		if(lightMap!=nullptr)
		{
			LightMapLighting(lights[i]);
		}
	}

}

RGBAColor errorColor;

//---------------------------------------------------------------------------
//
bool
	CheckForBigTriangles(DynamicArrayOf<Vector2DScalar> *lightMapUVs, int stride)
{
	int k1, k0;
	Scalar u1v0, u0v1;

	int up=0, down=0, right=0, left=0;

	for(k0=0;k0<stride;k0++)
	{
		if((*lightMapUVs)[k0][1] > 0.5f)
		{
			up++;
		}
		if((*lightMapUVs)[k0][1] < -0.5f)
		{
			down++;
		}
		if((*lightMapUVs)[k0][0] > 0.5f)
		{
			right++;
		}
		if((*lightMapUVs)[k0][0] < -0.5f)
		{
			left++;
		}
	}

	if(up==stride || down==stride || left==stride || right==stride)
	{
		errorColor = RGBAColor(0.5f, 0.5f, 0.0f, 1.0f);
		return false;
	}

#if 1
	Scalar sign = -1.0f;
	if( 
		((*lightMapUVs)[1][0]-(*lightMapUVs)[0][0]) * ((*lightMapUVs)[stride-1][1]-(*lightMapUVs)[0][1]) >
		((*lightMapUVs)[1][1]-(*lightMapUVs)[0][1]) * ((*lightMapUVs)[stride-1][0]-(*lightMapUVs)[0][0])
		)
	{
		sign = 1.0f;
	}
#endif

	for(k0=0;k0<stride;k0++)
	{
		k1 = (k0+1<stride) ? k0+1 : 0;

		u1v0 = 	(*lightMapUVs)[k1][0] * (*lightMapUVs)[k0][1];
		u0v1 = 	(*lightMapUVs)[k0][0] * (*lightMapUVs)[k1][1];

		if( sign*(u1v0 - u0v1) < 0.0f )
//		if( (u1v0 - u0v1) < 0.0f )
		{
			continue;
		}

		if(
			(
				((*lightMapUVs)[k1][0]*(*lightMapUVs)[k1][0] - 2.0f*(*lightMapUVs)[k1][0]*(*lightMapUVs)[k0][0] + (*lightMapUVs)[k0][0]*(*lightMapUVs)[k0][0]) +
				((*lightMapUVs)[k1][1]*(*lightMapUVs)[k1][1] - 2.0f*(*lightMapUVs)[k1][1]*(*lightMapUVs)[k0][1] + (*lightMapUVs)[k0][1]*(*lightMapUVs)[k0][1])
			) 	<	(2.0f*(u1v0*u1v0 - 2.0f*u1v0*u0v1 + u0v1*u0v1))
		)
		{
#if UV_TEST
			DEBUG_STREAM << k0 << endl;
			for(int i=0;i<stride;i++)
			{
				DEBUG_STREAM << (*lightMapUVs)[i][0] << " " << (*lightMapUVs)[i][1] << endl;
			}
			DEBUG_STREAM << endl;
#endif
			errorColor = RGBAColor(0.0f, 0.5f, 0.0f, 1.0f);
			return false;
		}
	}
	return true;
}

//---------------------------------------------------------------------------
//
void
	MLR_I_PMesh::LightMapLighting(MLRLight *light)
{
	if(!gEnableLightMaps)
	{
		return;
	}

	unsigned short stride;
	int i, j, k, len = static_cast<int>(lengths.GetLength());
	glm::mat4 matrix = glm::mat4(1.0f);
	glm::vec3 lightPosInShape, hitPoint;
	glm::vec3 up, left, forward;
	bool lm;
	Scalar f, rhf, falloff = 1.0f, distance;

	MLRLightMap *lightMap = light->GetLightMap();

	if( (!lightMap)  || (!gEnableLightMaps) )
	{
		return;
	}

	switch(light->GetLightType())
	{
		case MLRLight::PointLight:
		{
			lightMap->AddState(referenceState.GetPriority()+1);
			
			light->GetInShapePosition(lightPosInShape);

			for(i=0,j=0,k=0;i<len;i++,j += stride)
			{
				stride = lengths[i];

				if(testList[i] == 0)
				{
					continue;
				}

				f = facePlanes[i].distanceTo(lightPosInShape);
				
				lm = false;

				if(f>0.0f && Cast_Object(MLRInfiniteLightWithFalloff*, light)->GetFalloff(f, falloff) == true)
				{
					rhf = 1.0f/f;

					matrix = glm::mat4(1.0f);
					mc2::math::alignLocalAxisToWorldVector(matrix, facePlanes[i].normal, Z_Axis, Y_Axis, X_Axis);
					left = mc2::math::worldLeftInLocal(matrix);
					up = mc2::math::worldUpInLocal(matrix);
					forward = mc2::math::worldForwardInLocal(matrix);
					Verify(Small_Enough(up*left));

		#if defined(_ARMOR)
					Scalar diff = forward*left;
					Verify(Small_Enough(diff));
					diff = up*forward;
					Verify(Small_Enough(diff));
		#endif

					Check_Object(&forward);
					hitPoint = forward * -f;
					hitPoint+=lightPosInShape;

					for(k=0;k<stride;k++)
					{
						glm::vec3 vec(coords[index[k+j]]);
						vec-=hitPoint;

						(*lightMapUVs)[k][0] = -(glm::dot(left, vec))*rhf;
						(*lightMapUVs)[k][1] = -(glm::dot(up, vec))*rhf;

						if(
							(*lightMapUVs)[k][0] >= -0.5f && (*lightMapUVs)[k][0] <= 0.5f &&
							(*lightMapUVs)[k][1] >= -0.5f && (*lightMapUVs)[k][1] <= 0.5f

						) 
						{
							lm = true;
						}
					}
				}
				else
				{
					continue;
				}

				if(lm == true || CheckForBigTriangles(lightMapUVs, stride) == true)
				{
					lightMap->SetPolygonMarker(0);
					lightMap->AddUShort(stride);

					Scalar sq_falloff
						= falloff*falloff*light->GetIntensity();
					RGBAColor color(sq_falloff, sq_falloff, sq_falloff, 1.0f);

					lightMap->AddColor(color);
					for(k=0;k<stride;k++)
					{
						lightMap->AddCoord(coords[index[k+j]]);
					}
					for(k=0;k<stride;k++)
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

			lightMap->AddState(referenceState.GetPriority()+1);
			
			light->GetInShapePosition(matrix);
			lightPosInShape = mc2::math::getTranslation(matrix);

			Scalar tanSpeadAngle = Cast_Object(MLRSpotLight*, light)->GetTanSpreadAngle();

			left = mc2::math::localLeftInWorld(matrix);
			up = mc2::math::localUpInWorld(matrix);
			forward = mc2::math::localForwardInWorld(matrix);

			Verify(Small_Enough(up*left));

			for(i=0,j=0,k=0;i<len;i++,j += stride)
			{
				behindCount = 0;
				falloffCount = 0;
				stride = lengths[i];

				if(testList[i] == 0)
				{
					continue;
				}

				lm = false;

				if(!facePlanes[i].isSeenBy(lightPosInShape))
				{
					continue;
				}

				int tooBig = 0;
				for(k=0;k<stride;k++)
				{
					glm::vec3 vec;
					vec = coords[index[k+j]] - lightPosInShape;

					distance = (glm::dot(vec, forward));

					if(distance > SMALL)
					{
						if(Cast_Object(MLRInfiniteLightWithFalloff*, light)->GetFalloff(distance, falloff) == false)
						{
							falloffCount++;
						}
						(*lightMapSqFalloffs)[k] = falloff*falloff*light->GetIntensity();
					}
					else
					{
						behindCount++;
						break;
					}

					(*lightMapUVs)[k][0] = (glm::dot(left, vec))/(2.0f*distance*tanSpeadAngle);
					(*lightMapUVs)[k][1] = -(glm::dot(up, vec))/(2.0f*distance*tanSpeadAngle);

					if(
						(*lightMapUVs)[k][0] >= -0.5f && (*lightMapUVs)[k][0] <= 0.5f &&
						(*lightMapUVs)[k][1] >= -0.5f && (*lightMapUVs)[k][1] <= 0.5f

					) 
					{
						lm = true;
					}
                    //sebi:  what was the idea? why no ()?
					if(
						(*lightMapUVs)[k][0] < -1.5f || (*lightMapUVs)[k][0] > 1.5f ||
						(*lightMapUVs)[k][1] < -1.5f && (*lightMapUVs)[k][1] > 1.5f

					) 
					{
						tooBig++;
					}
				}

#if 1
				if(	
					tooBig == 0 &&
					behindCount == 0 && 
					falloffCount < stride &&
					(lm == true || CheckForBigTriangles(lightMapUVs, stride) == true)
					)
				{
					lightMap->SetPolygonMarker(1);
					lightMap->AddUShort(stride);

					for(k=0;k<stride;k++)
					{
						lightMap->AddCoord(coords[index[k+j]]);
					}
					for(k=0;k<stride;k++)
					{
						lightMap->AddColor((*lightMapSqFalloffs)[k], (*lightMapSqFalloffs)[k], (*lightMapSqFalloffs)[k], 1.0f);
					}
					for(k=0;k<stride;k++)
					{
						lightMap->AddUVs((*lightMapUVs)[k][0]+0.5f, (*lightMapUVs)[k][1]+0.5f);
		//				DEBUG_STREAM << k << " " << lightMapUVs[k][0] << " " << lightMapUVs[k][0] << "\n";
					}
				}
#else
				if(tooBig != 0)
				{
					lightMap->SetPolygonMarker(1);
					lightMap->AddUShort(stride);

					for(k=0;k<stride;k++)
					{
						lightMap->AddCoord(coords[index[k+j]]);
					}
					for(k=0;k<stride;k++)
					{
						lightMap->AddColor(RGBAColor(0.0f, 0.0f, 0.5f, 1.0f));
					}
					for(k=0;k<stride;k++)
					{
						lightMap->AddUVs(0.5f, 0.5f);
		//				DEBUG_STREAM << k << " " << lightMapUVs[k][0] << " " << lightMapUVs[k][0] << "\n";
					}
				} 
				else if(behindCount != 0)
				{
					lightMap->SetPolygonMarker(1);
					lightMap->AddUShort(stride);

					for(k=0;k<stride;k++)
					{
						lightMap->AddCoord(coords[index[k+j]]);
					}
					for(k=0;k<stride;k++)
					{
						lightMap->AddColor(RGBAColor(0.5f, 0.0f, 0.0f, 1.0f));
					}
					for(k=0;k<stride;k++)
					{
						lightMap->AddUVs(0.5f, 0.5f);
		//				DEBUG_STREAM << k << " " << lightMapUVs[k][0] << " " << lightMapUVs[k][0] << "\n";
					}
				} 
				else	if(behindCount == 0 && (lm == true || CheckForBigTriangles(&lightMapUVs, stride) == true) )
				{
					lightMap->SetPolygonMarker(1);
					lightMap->AddUShort(stride);

					for(k=0;k<stride;k++)
					{
						lightMap->AddCoord(coords[index[k+j]]);
					}
					for(k=0;k<stride;k++)
					{
						lightMap->AddColor(lightMapSqFalloffs[k], lightMapSqFalloffs[k], lightMapSqFalloffs[k], 1.0f);
					}
					for(k=0;k<stride;k++)
					{
						lightMap->AddUVs(lightMapUVs[k][0]+0.5f, lightMapUVs[k][1]+0.5f);
		//				DEBUG_STREAM << k << " " << lightMapUVs[k][0] << " " << lightMapUVs[k][0] << "\n";
					}

				}
				else if(CheckForBigTriangles(&lightMapUVs, stride) == false)
				{
					lightMap->SetPolygonMarker(1);
					lightMap->AddUShort(stride);

					for(k=0;k<stride;k++)
					{
						lightMap->AddCoord(coords[index[k+j]]);
					}
					for(k=0;k<stride;k++)
					{
						lightMap->AddColor(errorColor);
					}
					for(k=0;k<stride;k++)
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

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_PMesh*
	MidLevelRenderer::CreateIndexedCube_NoColor_NoLit(
		Scalar half, 
		MLRState *state
	)
{
	gos_PushCurrentHeap(Heap);
	MLR_I_PMesh *ret = new MLR_I_PMesh();
	Register_Object(ret);

	glm::vec3 *coords = new glm::vec3 [8];
	Register_Object(coords);

	coords[0] = glm::vec3( half, -half,  half);
	coords[1] = glm::vec3(-half, -half,  half);
	coords[2] = glm::vec3( half, -half, -half);
	coords[3] = glm::vec3(-half, -half, -half);
	coords[4] = glm::vec3(-half,  half,  half);
	coords[5] = glm::vec3( half,  half,  half);
	coords[6] = glm::vec3( half,  half, -half);
	coords[7] = glm::vec3(-half,  half, -half);

	unsigned char *lengths = new unsigned char [6];
	Register_Pointer(lengths);

	int i;

	for(i=0;i<6;i++)
	{
		lengths[i] = 4;
	}

	ret->SetSubprimitiveLengths(lengths, 6);

	ret->SetCoordData(coords, 8);

	unsigned short	*index = new unsigned short [6*4];
	Register_Pointer(index);

	index[0] = 0;
	index[1] = 2;
	index[2] = 6;
	index[3] = 5;

	index[4] = 0;
	index[5] = 5;
	index[6] = 4;
	index[7] = 1;

	index[8] = 5;
	index[9] = 6;
	index[10] = 7;
	index[11] = 4;

	index[12] = 2;
	index[13] = 3;
	index[14] = 7;
	index[15] = 6;

	index[16] = 1;
	index[17] = 4;
	index[18] = 7;
	index[19] = 3;

	index[20] = 0;
	index[21] = 1;
	index[22] = 3;
	index[23] = 2;

	ret->SetIndexData(index, 6*4);

	ret->FindFacePlanes();

	Vector2DScalar *texCoords = new Vector2DScalar[8];
	Register_Pointer(texCoords);

	texCoords[0] = Vector2DScalar(0.0f, 0.0f);
	texCoords[1] = Vector2DScalar(0.0f, 0.0f);
	texCoords[2] = Vector2DScalar(0.0f, 0.0f);
	texCoords[3] = Vector2DScalar(0.0f, 0.0f);

	texCoords[4] = Vector2DScalar(0.0f, 0.0f);
	texCoords[5] = Vector2DScalar(0.0f, 0.0f);
	texCoords[6] = Vector2DScalar(0.0f, 0.0f);
	texCoords[7] = Vector2DScalar(0.0f, 0.0f);

	if(state != nullptr)
	{
		ret->SetReferenceState(*state);
		if(state->GetTextureHandle() > 0)
		{
			texCoords[0] = Vector2DScalar(0.0f, 0.0f);
			texCoords[1] = Vector2DScalar(1.0f, 0.0f);
			texCoords[2] = Vector2DScalar(0.25f, 0.25f);
			texCoords[3] = Vector2DScalar(0.75f, 0.25f);

			texCoords[4] = Vector2DScalar(1.0f, 1.0f);
			texCoords[5] = Vector2DScalar(0.0f, 1.0f);
			texCoords[6] = Vector2DScalar(0.25f, 0.75f);
			texCoords[7] = Vector2DScalar(0.75f, 0.75f);
		}
	}
	ret->SetTexCoordData(texCoords, 8);

	Unregister_Object(texCoords);
	delete [] texCoords;

	Unregister_Pointer(index);
	delete [] index;

	Unregister_Pointer(lengths);
	delete [] lengths;

	Unregister_Object(coords);
	delete [] coords;

	gos_PopCurrentHeap();
	return ret;
}

long MidLevelRenderer::triDrawn = 0;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MidLevelRenderer::subdivide (
		glm::vec3 *coords,
		glm::vec3& v1,
		glm::vec3& v2,
		glm::vec3& v3,
		long depth,
		long tri2draw, 
        float rad
	)
{
	glm::vec3 v12(0.0f, 0.0f, 0.0f), v23(0.0f, 0.0f, 0.0f), v31(0.0f, 0.0f, 0.0f);
	int i;

	if (depth == 0)
	{
		if (tri2draw > triDrawn)
		{
			coords[3*triDrawn] = v1 * rad;
			coords[3*triDrawn+1] = v3 * rad;
			coords[3*triDrawn+2] = v2 * rad;
		}
		triDrawn++;
		return;
	}

	for (i=0;i<3;i++)
	{
		v12[i] = v1[i] + v2[i];
		v23[i] = v2[i] + v3[i];
		v31[i] = v3[i] + v1[i];
	}
	v12 = glm::normalize(v12);
	v23 = glm::normalize(v23);
	v31 = glm::normalize(v31);

	MidLevelRenderer::subdivide (coords, v1, v12, v31, depth-1, tri2draw, rad);
	MidLevelRenderer::subdivide (coords, v2, v23, v12, depth-1, tri2draw, rad);
	MidLevelRenderer::subdivide (coords, v3, v31, v23, depth-1, tri2draw, rad);
	MidLevelRenderer::subdivide (coords, v12, v23, v31, depth-1, tri2draw, rad);
}

float MidLevelRenderer::vdata[12][3] = {
  {-ICO_X, 0.0f, ICO_Z}, {ICO_X, 0.0f, ICO_Z}, {-ICO_X, 0.0f, -ICO_Z}, {ICO_X, 0.0f, -ICO_Z},
  {0.0f, ICO_Z, ICO_X}, {0.0f, ICO_Z, -ICO_X}, {0.0f, -ICO_Z, ICO_X}, {0.0f, -ICO_Z, -ICO_X},
  {ICO_Z, ICO_X, 0.0f}, {-ICO_Z, ICO_X, 0.0f}, {ICO_Z, -ICO_X, 0.0f}, {-ICO_Z, -ICO_X, 0.0f}
};

unsigned int MidLevelRenderer::tindices [20][3] = {
  {0, 4, 1}, {0, 9, 4}, {9, 5, 4}, {4, 5, 8}, {4, 8, 1},
  {8, 10, 1}, {8, 3, 10}, {5, 3, 8}, {5, 2, 3}, {2, 7, 3},
  {7, 10, 3}, {7, 6, 10}, {7, 11, 6}, {11, 0, 6}, {0, 1, 6},
  {6, 1, 10}, {9, 0, 11}, {9, 11, 2}, {9, 2, 5}, {7, 2, 11}
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRShape*
	MidLevelRenderer::CreateIndexedIcosahedron_NoColor_NoLit(
		IcoInfo& icoInfo,
		MLRState *state
	)
{
	gos_PushCurrentHeap(Heap);
	MLRShape *ret = new MLRShape(20);
	Register_Object(ret);

	int i, j, k;
	long    nrTri = static_cast<long>(ceil (icoInfo.all * pow (4.0f, icoInfo.depth)));
	glm::vec3 v[3];

	if(3*nrTri >= Limits::Max_Number_Vertices_Per_Mesh)
	{
		nrTri = Limits::Max_Number_Vertices_Per_Mesh/3;
	}

	unsigned char *lengths = new unsigned char [nrTri];
	Register_Pointer(lengths);

	for(i=0;i<nrTri;i++)
	{
		lengths[i] = 3;
	}

	glm::vec3 *coords = new glm::vec3 [nrTri*3];
	Register_Pointer(coords);
	
	glm::vec3 *collapsedCoords = nullptr;
	if(icoInfo.indexed==true)
	{
		collapsedCoords = new glm::vec3 [nrTri*3];
		Register_Pointer(collapsedCoords);
	}

	unsigned short	*index = new unsigned short [nrTri*3];
	Register_Pointer(index);
	Vector2DScalar *texCoords = new Vector2DScalar[nrTri*3];
	Register_Pointer(texCoords);

	int uniquePoints = 0;
	for (k=0;k<20;k++)
	{
		triDrawn = 0;
		MLR_I_PMesh *mesh = new MLR_I_PMesh();
		Register_Object(mesh);

// setup vertex position information
	    for (j=0;j<3;j++)
		{
			v[j].x = vdata[tindices[k][j]][0];
			v[j].y = vdata[tindices[k][j]][1];
			v[j].z = vdata[tindices[k][j]][2];
		}
		subdivide (coords, v[0], v[1], v[2], icoInfo.depth, nrTri, icoInfo.radius);

		mesh->SetSubprimitiveLengths(lengths, nrTri);


		if(icoInfo.indexed==true)
		{
			uniquePoints = 1;
			collapsedCoords[0] = coords[0];
			index[0] = 0;

			for(i=1;i<nrTri*3;i++)
			{
				for(j=0;j<uniquePoints;j++)
				{
					if(mc2::math::closeEnough(coords[i], collapsedCoords[j]))
					{
						break;
					}
				}
				if(j==uniquePoints)
				{
					collapsedCoords[uniquePoints++] = coords[i];
				}
				index[i] = static_cast<unsigned short>(j);
			}
			mesh->SetCoordData(collapsedCoords, uniquePoints);
		}
		else
		{
			uniquePoints = nrTri*3;
			for(i=0;i<nrTri*3;i++)
			{
				index[i] = static_cast<unsigned short>(i);
			}
			mesh->SetCoordData(coords, nrTri*3);
		}

		mesh->SetIndexData(index, nrTri*3);

		mesh->FindFacePlanes();

		if(state == nullptr)
		{
			for(i=0;i<uniquePoints;i++)
			{
				texCoords[i] = Vector2DScalar(0.0f, 0.0f);
			}
		}
		else
		{
			mesh->SetReferenceState(*state);
			if(state->GetTextureHandle() > 0)
			{
				if(icoInfo.indexed==true)
				{
					for(i=0;i<uniquePoints;i++)
					{
						texCoords[i] = 
							Vector2DScalar(
								(1.0f + collapsedCoords[i].x)/2.0f,
								(1.0f + collapsedCoords[i].y)/2.0f
							);
					}
				}
				else
				{
					for(i=0;i<nrTri;i++)
					{
						texCoords[3*i] = 
							Vector2DScalar(
								(1.0f + coords[3*i].x)/2.0f,
								(1.0f + coords[3*i].y)/2.0f
							);
						texCoords[3*i+1] = 
							Vector2DScalar(
								(1.0f + coords[3*i+1].x)/2.0f,
								(1.0f + coords[3*i+1].y)/2.0f
							);
						texCoords[3*i+2] = 
							Vector2DScalar(
								(1.0f + coords[3*i+2].x)/2.0f,
								(1.0f + coords[3*i+2].y)/2.0f
							);
					}
				}
			}
			else
			{
				for(i=0;i<uniquePoints;i++)
				{
					texCoords[i] = Vector2DScalar(0.0f, 0.0f);
				}
			}
		}
		mesh->SetTexCoordData(texCoords, uniquePoints);

		ret->Add(mesh);
		mesh->DetachReference();
	}

	Unregister_Pointer(texCoords);
	delete [] texCoords;

	Unregister_Pointer(index);
	delete [] index;

	if(icoInfo.indexed==true)
	{
		Unregister_Pointer(collapsedCoords);
		delete [] collapsedCoords;
	}
	
	Unregister_Pointer(coords);
	delete [] coords;
	
	Unregister_Pointer(lengths);
	delete [] lengths;

	gos_PopCurrentHeap();
	return ret;
}
