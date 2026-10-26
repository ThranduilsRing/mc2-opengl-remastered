//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"
#include <memory>


//#############################################################################
//######## MLRIndexedPolyMesh with color and lighting one texture layer  ######
//#############################################################################

MLR_I_L_PMesh::ClassData*
	MLR_I_L_PMesh::DefaultData = nullptr;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_L_PMesh::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLR_I_L_PMeshClassID,
			"MidLevelRenderer::MLR_I_L_PMesh",
			MLR_I_C_PMesh::DefaultData,
			(MLRPrimitiveBase::Factory)&Make
		);
	Register_Object(DefaultData);

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_L_PMesh::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_L_PMesh::MLR_I_L_PMesh(
	ClassData *class_data,
	MemoryStream *stream,
	int version
):
	MLR_I_C_PMesh(DefaultData, stream, version)
{
	Check_Pointer(this);
	Check_Pointer(stream);
	Verify(gos_GetCurrentHeap() == Heap);

	switch(version)
	{
		case 1:
		case 2:
		{
			STOP(("This class got created only after version 2 !"));
		}
		break;
		default:
		{
			MemoryStreamIO_Read(stream, &normals);
		}
		break;
	}

	litColors.SetLength(colors.GetLength());
	actualColors = &colors;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_L_PMesh::MLR_I_L_PMesh(ClassData *class_data):
	MLR_I_C_PMesh(class_data), normals(0)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);

	actualColors = &colors;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/*
void
	MLR_I_L_PMesh::Copy(MLRIndexedPolyMesh *polyMesh)
{
	Check_Pointer(this);

	int len;
	glm::vec3 *_normals;

	MLR_I_C_PMesh::Copy(polyMesh);

	polyMesh->GetNormalData(&_normals, &len);
	SetNormalData(_normals, len);
}
*/

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_L_PMesh::~MLR_I_L_PMesh()
{
	Check_Object(this);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_L_PMesh*
	MLR_I_L_PMesh::Make(
		MemoryStream *stream,
		int version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	MLR_I_L_PMesh *mesh = new MLR_I_L_PMesh(DefaultData, stream, version);
	gos_PopCurrentHeap();

	return mesh;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_L_PMesh::Save(MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);

	MLR_I_C_PMesh::Save(stream);

	MemoryStreamIO_Write(stream, &normals);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_L_PMesh::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_L_PMesh::SetNormalData(
		const glm::vec3 *data,
		int dataSize
	)
{
	Check_Object(this); 
	Check_Pointer(data);

	Verify(coords.GetLength() == 0 || dataSize == coords.GetLength());
	Verify(colors.GetLength() == 0 || dataSize == colors.GetLength());
	Verify(texCoords.GetLength() == 0 || dataSize == texCoords.GetLength());

	normals.AssignData(data, dataSize);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_L_PMesh::GetNormalData(
		glm::vec3 **data,
		int *dataSize
	)
{
	Check_Object(this); 
	*data = normals.GetData();
	*dataSize = static_cast<int>(normals.GetLength());
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_L_PMesh::SetColorData(
#if COLOR_AS_DWORD
		const DWORD *data,
#else
		const RGBAColor *data,
#endif
		int dataSize
	)
{
	Check_Object(this); 
	Check_Pointer(data);
	Verify(gos_GetCurrentHeap() == Heap);

	Verify(coords.GetLength() == 0 || dataSize == coords.GetLength());
	Verify(texCoords.GetLength() == 0 || dataSize == texCoords.GetLength());

	litColors.SetLength(dataSize);
	colors.AssignData(data, dataSize);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_L_PMesh::PaintMe(
#if COLOR_AS_DWORD
		const DWORD *paintMe
#else
		const RGBAColor *paintMe
#endif
		
	)
{
	Check_Object(this);
	Verify(gos_GetCurrentHeap() == Heap);

#if 0
	Verify(colors.GetLength() == litColors.GetLength());
#else
	if(colors.GetLength() == litColors.GetLength())
	{
		litColors.SetLength(colors.GetLength());
	}
#endif

	int k, len = static_cast<int>(litColors.GetLength());

#if COLOR_AS_DWORD
	DWORD argb = GOSCopyColor(paintMe);

	for(k=0;k<len;k++)
	{
		litColors[k] = argb;
	}
#else
	for(k=0;k<len;k++)
	{
		litColors[k] = *paintMe;
	}
#endif
	// set the to use colors to the original colors ...
	// only lighting could overwrite this;
	actualColors = &litColors;
}



//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRShape*
	MidLevelRenderer::CreateIndexedIcosahedron_Color_Lit(
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

	auto lengthsOwner = std::make_unique_for_overwrite<unsigned char[]>(nrTri);
	unsigned char* lengths = lengthsOwner.get();
	Register_Pointer(lengths);

	for(i=0;i<nrTri;i++)
	{
		lengths[i] = 3;
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
	auto colorsOwner = std::make_unique_for_overwrite<RGBAColor[]>(nrTri*3);
	RGBAColor* colors = colorsOwner.get();
	Register_Pointer(colors);
	auto normalsOwner = std::make_unique_for_overwrite<glm::vec3[]>(nrTri*3);
	glm::vec3* normals = normalsOwner.get();
	Register_Pointer(normals);

	int uniquePoints = 0;
	for (k=0;k<20;k++)
	{
		MidLevelRenderer::triDrawn = 0;
		MLR_I_L_PMesh *mesh = new MLR_I_L_PMesh();
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

		if(icoInfo.indexed==true)
		{
			for(i=0;i<uniquePoints;i++)
			{
				colors[i] = 
					RGBAColor(
						(1.0f + collapsedCoords[i].x)/2.0f,
						(1.0f + collapsedCoords[i].y)/2.0f,
						(1.0f + collapsedCoords[i].z)/2.0f,
						1.0f
					);
				normals[i] = glm::normalize(collapsedCoords[i]);

			}
		}
		else
		{
			for(i=0;i<uniquePoints;i++)
			{
				colors[i] = 
					RGBAColor(
						(1.0f + coords[i].x)/2.0f,
						(1.0f + coords[i].y)/2.0f,
						(1.0f + coords[i].z)/2.0f,
						1.0f
					);
				normals[i] = glm::normalize(coords[i]);
			}
		}
		mesh->SetColorData(colors, uniquePoints);
		mesh->SetNormalData(normals, uniquePoints);

		ret->Add(mesh);
		mesh->DetachReference();
	}

	Unregister_Pointer(normals);
	normalsOwner.reset();

	Unregister_Pointer(colors);
	colorsOwner.reset();

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
	
	Unregister_Pointer(lengths);
	lengthsOwner.reset();

	gos_PopCurrentHeap();
	return ret;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/*
MLR_I_L_PMesh*
	MidLevelRenderer::CreateIndexedCube(
		Scalar half, 
		RGBAColor *eightColors, 
		glm::vec3 *eightNormals,
		MLRState *state
	)
{
	gos_PushCurrentHeap(Heap);
	MLR_I_L_PMesh *ret = new MLR_I_L_PMesh;
	Register_Object(ret);

	auto coordsOwner = std::make_unique_for_overwrite<glm::vec3[]>(8);
	glm::vec3* coords = coordsOwner.get();
	Register_Object(coords);

	coords[0] = glm::vec3( half, -half,  half);
	coords[1] = glm::vec3(-half, -half,  half);
	coords[2] = glm::vec3( half, -half, -half);
	coords[3] = glm::vec3(-half, -half, -half);
	coords[4] = glm::vec3(-half,  half,  half);
	coords[5] = glm::vec3( half,  half,  half);
	coords[6] = glm::vec3( half,  half, -half);
	coords[7] = glm::vec3(-half,  half, -half);

	auto lengthsOwner = std::make_unique_for_overwrite<unsigned char[]>(6);
	unsigned char* lengths = lengthsOwner.get();
	Register_Pointer(lengths);

	int i;

	for(i=0;i<6;i++)
	{
		lengths[i] = 4;
	}

	ret->SetPrimitiveLength(lengths, 6);

	ret->SetCoordData(coords, 8);

	auto indexOwner = std::make_unique_for_overwrite<unsigned short[]>(6*4);
	unsigned short* index = indexOwner.get();
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

	if(eightColors!=NULL)
	{
#if COLOR_AS_DWORD
		auto dwColorOwner = std::make_unique_for_overwrite<DWORD[]>(8);
		DWORD* dwColor = dwColorOwner.get();
		Register_Pointer(dwColor);

		for(i=0;i<8;i++)
		{
			dwColor[i] = GOSCopyColor(eightColors+i);
		}

		ret->SetColorData(dwColor, 8);

		Unregister_Pointer(dwColor);
		dwColorOwner.reset();
#else
		ret->SetColorData(eightColors, 8);
#endif
	}

	if(eightNormals!=NULL)
	{
		ret->SetNormalData(eightNormals, 8);
	}

	auto texCoordsOwner = std::make_unique_for_overwrite<Vector2DScalar[]>(8);
	Vector2DScalar* texCoords = texCoordsOwner.get();
		Register_Object(texCoords);

	texCoords[0] = Vector2DScalar(0.0f, 0.0f);
	texCoords[1] = Vector2DScalar(0.0f, 0.0f);
	texCoords[2] = Vector2DScalar(0.0f, 0.0f);
	texCoords[3] = Vector2DScalar(0.0f, 0.0f);

	texCoords[4] = Vector2DScalar(0.0f, 0.0f);
	texCoords[5] = Vector2DScalar(0.0f, 0.0f);
	texCoords[6] = Vector2DScalar(0.0f, 0.0f);
	texCoords[7] = Vector2DScalar(0.0f, 0.0f);

	if(state != NULL)
	{
		ret->SetReferenceState(*state);
		if(state->GetTexture() > 0)
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
	texCoordsOwner.reset();

	Unregister_Pointer(index);
	indexOwner.reset();

	Unregister_Pointer(lengths);
	lengthsOwner.reset();

	Unregister_Object(coords);
	coordsOwner.reset();

	gos_PopCurrentHeap();
	return ret;
}
*/
