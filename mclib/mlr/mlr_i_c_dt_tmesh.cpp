//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"
#include <memory>


//#############################################################################
//###### MLRIndexedTriMesh with color but no lighting one texture layer  ######
//#############################################################################

MLR_I_C_DT_TMesh::ClassData*
	MLR_I_C_DT_TMesh::DefaultData = nullptr;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_DT_TMesh::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLR_I_C_DT_TMeshClassID,
			"MidLevelRenderer::MLR_I_C_DT_TMesh",
			MLR_I_DT_TMesh::DefaultData,
			(MLRPrimitiveBase::Factory)&Make
		);
	Register_Object(DefaultData);

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_DT_TMesh::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_C_DT_TMesh::MLR_I_C_DT_TMesh(
	ClassData *class_data,
	MemoryStream *stream,
	int version
):
	MLR_I_DT_TMesh(class_data, stream, version)
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
#if COLOR_AS_DWORD
			MemoryStreamIO_Read(stream, &colors);
#else
			Stuff::DynamicArrayOf<DWORD> smallColors;

			MemoryStreamIO_Read(stream, &smallColors);
		
			int i, len = static_cast<int>(smallColors.GetLength());

			colors.SetLength(len);

			DWORD theColor;

			for(i=0;i<len;i++)
			{
				theColor = smallColors[i];

				colors[i].blue = (theColor & 0xff) * One_Over_256;

				theColor = theColor>>8;

				colors[i].green = (theColor & 0xff) * One_Over_256;

				theColor = theColor>>8;

				colors[i].red = (theColor & 0xff) * One_Over_256;

				theColor = theColor>>8;

				colors[i].alpha = (theColor & 0xff) * One_Over_256;
			}
#endif
		}
		break;
	}

	actualColors = &colors;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_C_DT_TMesh::MLR_I_C_DT_TMesh(ClassData *class_data):
	MLR_I_DT_TMesh(class_data), colors(0)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);

	actualColors = &colors;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_C_DT_TMesh::~MLR_I_C_DT_TMesh()
{
	Check_Object(this);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_C_DT_TMesh*
	MLR_I_C_DT_TMesh::Make(
		MemoryStream *stream,
		int version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	MLR_I_C_DT_TMesh *mesh = new MLR_I_C_DT_TMesh(DefaultData, stream, version);
	gos_PopCurrentHeap();

	return mesh;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_DT_TMesh::Save(MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);

	MLR_I_TMesh::Save(stream);

#if COLOR_AS_DWORD
	MemoryStreamIO_Write(stream, &colors);
#else
	Stuff::DynamicArrayOf<DWORD> smallColors;
	int i, len = static_cast<int>(colors.GetLength());

	const Stuff::RGBAColor *data = colors.GetData();

	smallColors.SetLength(len);

	for(i=0;i<len;i++)
	{
		smallColors[i] = GOSCopyColor(data+i);
	}

	MemoryStreamIO_Write(stream, &smallColors);
#endif
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
bool
	MLR_I_C_DT_TMesh::Copy(MLR_I_C_DT_PMesh *pMesh)
{
	Check_Object(this);
	Check_Object(pMesh);

	int len;
#if COLOR_AS_DWORD
	DWORD *_colors;
#else
	RGBAColor *_colors;
#endif

	MLR_I_DT_TMesh::Copy(pMesh);

	pMesh->GetColorData(&_colors, &len);
	SetColorData(_colors, len);

	return true;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_DT_TMesh::SetColorData(
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

	Verify(coords.GetLength() == 0 || dataSize == coords.GetLength());
	Verify(texCoords.GetLength() == 0 || 2*dataSize == texCoords.GetLength());

	colors.AssignData(data, dataSize);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_DT_TMesh::GetColorData(
#if COLOR_AS_DWORD
		DWORD **data,
#else
		RGBAColor **data,
#endif
		int *dataSize
	)
{
	Check_Object(this); 
	*data = colors.GetData();
	*dataSize = static_cast<int>(colors.GetLength());
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_DT_TMesh::PaintMe(
#if COLOR_AS_DWORD
		const DWORD *paintMe
#else
		const RGBAColor *paintMe
#endif
		
	)
{
	Check_Object(this);
	// original color is lost !!!;

	int k, len = static_cast<int>(colors.GetLength());

#if COLOR_AS_DWORD
	DWORD argb = GOSCopyColor(paintMe);

	for(k=0;k<len;k++)
	{
		colors[k] = argb;
	}
#else
	for(k=0;k<len;k++)
	{
		colors[k] = *paintMe;
	}
#endif
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_DT_TMesh::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

extern DWORD gEnableTextureSort, gEnableAlphaSort;



//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_C_DT_TMesh*
	MidLevelRenderer::CreateIndexedTriCube_Color_NoLit_2Tex(
		Scalar half, 
		MLRState *state
	)
{
	return nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRShape*
	MidLevelRenderer::CreateIndexedTriIcosahedron_Color_NoLit_2Tex(
		IcoInfo& icoInfo,
		MLRState *state,
		MLRState *state2
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
	auto texCoordsOwner = std::make_unique_for_overwrite<Vector2DScalar[]>(2*nrTri*3);
	Vector2DScalar* texCoords = texCoordsOwner.get();
	Register_Pointer(texCoords);
	auto colorsOwner = std::make_unique_for_overwrite<RGBAColor[]>(nrTri*3);
	RGBAColor* colors = colorsOwner.get();
	Register_Pointer(colors);

	int uniquePoints = 0;
	for (k=0;k<20;k++)
	{
		triDrawn = 0;
		MLR_I_C_DT_TMesh *mesh = new MLR_I_C_DT_TMesh();
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
			for(i=0;i<2*uniquePoints;i++)
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
						texCoords[i+uniquePoints] = 
							Vector2DScalar(
								(1.0f + collapsedCoords[i].x)/2.0f,
								(1.0f + collapsedCoords[i].z)/2.0f
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
						texCoords[3*i+nrTri] = 
							Vector2DScalar(
								(1.0f + coords[3*i].x)/2.0f,
								(1.0f + coords[3*i].z)/2.0f
							);
						texCoords[3*i+1+nrTri] = 
							Vector2DScalar(
								(1.0f + coords[3*i+1].x)/2.0f,
								(1.0f + coords[3*i+1].z)/2.0f
							);
						texCoords[3*i+2+nrTri] = 
							Vector2DScalar(
								(1.0f + coords[3*i+2].x)/2.0f,
								(1.0f + coords[3*i+2].z)/2.0f
							);
					}
				}
			}
			else
			{
				for(i=0;i<2*uniquePoints;i++)
				{
					texCoords[i] = Vector2DScalar(0.0f, 0.0f);
				}
			}
		}
		mesh->SetTexCoordData(texCoords, 2*uniquePoints);

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
			}
		}
		mesh->SetColorData(colors, uniquePoints);

		mesh->SetReferenceState(*state2, 1);

		ret->Add(mesh);
		mesh->DetachReference();
	}

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
	
	gos_PopCurrentHeap();
	return ret;
}
