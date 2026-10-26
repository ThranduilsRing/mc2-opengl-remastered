//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"
#include <memory>

extern DWORD gEnableDetailTexture;


//#############################################################################
//###### MLRIndexedPolyMesh with no color no lighting two texture layer  ######
//#############################################################################

MLR_I_DeT_TMesh::ClassData*
	MLR_I_DeT_TMesh::DefaultData = nullptr;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_DeT_TMesh::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLR_I_DeT_TMeshClassID,
			"MidLevelRenderer::MLR_I_DeT_TMesh",
			MLR_I_TMesh::DefaultData,
			(MLRPrimitiveBase::Factory)&Make
		);
	Register_Object(DefaultData);
	
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_DeT_TMesh::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_DeT_TMesh::MLR_I_DeT_TMesh(
	ClassData *class_data,
	MemoryStream *stream,
	int version
):
	MLR_I_TMesh(class_data, stream, version)
{
	Check_Pointer(this);
	Check_Pointer(stream);
	Verify(gos_GetCurrentHeap() == Heap);

	referenceState2.Load(stream, version);

	*stream >> xOffset >> yOffset >> xScale >> yScale;
	*stream >> fadeDetailStart >> fadeDetailEnd;

	fadeMultiplicator = 255.9f/(fadeDetailStart-fadeDetailEnd);

	referenceState2.SetZBufferWriteOff();

#if 0
	referenceState.SetPriority(MLRState::DefaultPriority);
	referenceState2.SetPriority(MLRState::DefaultPriority);

	referenceState.SetPriority(MLRState::DefaultPriority+1);

	referenceState2.SetAlphaMode(MLRState::OneZeroMode);
	referenceState.SetAlphaMode(MLRState::AlphaInvAlphaMode);
#endif

	detTextureVisible = true;

	passes = 2;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_DeT_TMesh::MLR_I_DeT_TMesh(ClassData *class_data):
	MLR_I_TMesh(class_data)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);

	fadeDetailStart = 0.0f;
	fadeDetailEnd = 1.0f;
	fadeMultiplicator = 0.0f;

	detTextureVisible = true;
	passes = 2;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_DeT_TMesh::Copy(
		MLR_I_TMesh *tMesh,
		MLRState detailState,
		Stuff::Scalar xOff,
		Stuff::Scalar yOff,
		Stuff::Scalar xFac,
		Stuff::Scalar yFac
	)
{
	Check_Object(this);
	Check_Object(tMesh);
	Verify(gos_GetCurrentHeap() == Heap);

	int len;
	unsigned short *_index;
	glm::vec3 *_coords;
	Vector2DScalar *_texCoords;

	tMesh->GetCoordData(&_coords, &len);
	SetCoordData(_coords, len);

	tMesh->GetIndexData(&_index, &len);
	SetIndexData(_index, len);

	SetSubprimitiveLengths(nullptr, tMesh->GetNumPrimitives());

	facePlanes.SetLength(GetNumPrimitives());
	testList.SetLength(GetNumPrimitives());

	FindFacePlanes();

	tMesh->GetTexCoordData(&_texCoords, &len);
	SetTexCoordData(_texCoords, len);

	referenceState = tMesh->GetReferenceState();

	visibleIndexedVerticesKey = false;
	visibleIndexedVertices.SetLength(coords.GetLength());

	referenceState2 = detailState;

	SetDetailData(xOff, xFac, xFac, yFac, 80.0f, 100.0f);

	detTextureVisible = true;

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_DeT_TMesh::~MLR_I_DeT_TMesh()
{
	Check_Object(this);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_DeT_TMesh*
	MLR_I_DeT_TMesh::Make(
		MemoryStream *stream,
		int version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	MLR_I_DeT_TMesh *mesh = new MLR_I_DeT_TMesh(DefaultData, stream, version);
	gos_PopCurrentHeap();

	return mesh;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_DeT_TMesh::Save(MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);

	MLR_I_TMesh::Save(stream);

	referenceState2.Save(stream);

	*stream << xOffset << yOffset << xScale << yScale;
	*stream << fadeDetailStart << fadeDetailEnd;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_DeT_TMesh::SetDetailData(
		Stuff::Scalar xOff,
		Stuff::Scalar yOff,
		Stuff::Scalar xFac,
		Stuff::Scalar yFac,
		Stuff::Scalar detailStart,
		Stuff::Scalar detailEnd

	)
{
	Check_Object(this);

	xOffset = xOff;
	yOffset = yOff;
	xScale = xFac;
	yScale = yFac;

	fadeDetailStart = 1.0f/detailStart;
	Verify(detailEnd>0.0f);
	fadeDetailEnd = 1.0f/detailEnd;

	Verify(detailStart!=detailEnd);
	fadeMultiplicator = 255.9f/(fadeDetailStart-fadeDetailEnd);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
int
	MLR_I_DeT_TMesh::GetNumPasses()
{
	Check_Object(this);

	Check_Object(this);

	if(gEnableDetailTexture==0 || detTextureVisible==false)
	{
		return 1;
	}
	
	if(MLRState::GetMultitextureLightMap() == true && state.GetMultiTextureMode()!=MLRState::MultiTextureOffMode)
	{
		return 1;
	}

	return 2;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_DeT_TMesh::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}



//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_DeT_TMesh*
	MidLevelRenderer::CreateIndexedTriCube_NoColor_NoLit_DetTex(
		Scalar half, 
		MLRState *state,
		MLRState *state1
	)
{
	return nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRShape*
	MidLevelRenderer::CreateIndexedTriIcosahedron_NoColor_NoLit_DetTex(
		IcoInfo& icoInfo,
		MLRState *state,
		MLRState *stateDet
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

	auto coordsOwner = std::make_unique_for_overwrite<glm::vec3[]>(nrTri*3);
	glm::vec3* coords = coordsOwner.get();
	Register_Pointer(coords);
	
	std::unique_ptr<glm::vec3[]> collapsedCoordsOwner;
	glm::vec3* collapsedCoords = nullptr;
	if(icoInfo.indexed==true)
	{
		collapsedCoordsOwner = std::make_unique_for_overwrite<glm::vec3[]>(nrTri*3);
		collapsedCoords = collapsedCoordsOwner.get();
		Register_Pointer(collapsedCoords);
	}

	auto indexOwner = std::make_unique_for_overwrite<unsigned short[]>(nrTri*3);
	unsigned short* index = indexOwner.get();
	Register_Pointer(index);
	auto texCoordsOwner = std::make_unique_for_overwrite<Vector2DScalar[]>(nrTri*3);
	Vector2DScalar* texCoords = texCoordsOwner.get();
	Register_Pointer(texCoords);

	int uniquePoints = 0;
	for (k=0;k<20;k++)
	{
		triDrawn = 0;
		MLR_I_DeT_TMesh *mesh = new MLR_I_DeT_TMesh();
		Register_Object(mesh);

// setup vertex position information
	    for (j=0;j<3;j++)
		{
			v[j].x = vdata[tindices[k][j]][0];
			v[j].y = vdata[tindices[k][j]][1];
			v[j].z = vdata[tindices[k][j]][2];
		}
		subdivide (coords, v[0], v[1], v[2], icoInfo.depth, nrTri, icoInfo.radius);

		mesh->SetSubprimitiveLengths(nullptr, nrTri);

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
			state->SetMultiTextureMode(MLRState::MultiTextureLightmapMode);
			mesh->SetReferenceState(*state);
			state->SetMultiTextureMode(MLRState::MultiTextureOffMode);

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

		mesh->SetDetailData(0.0f, 0.0f, 16.0f, 16.0f, 50.0f, 70.0f);
		mesh->SetReferenceState(*stateDet, 1);

		ret->Add(mesh);
		mesh->DetachReference();
	}

	Unregister_Pointer(texCoords);
	texCoordsOwner.reset();

	Unregister_Pointer(index);
	indexOwner.reset();

	if(icoInfo.indexed==true)
	{
		Unregister_Pointer(collapsedCoords);
		collapsedCoordsOwner.reset();
	}
	
	Unregister_Pointer(coords);
	coordsOwner.reset();

	gos_PopCurrentHeap();
	return ret;
}
