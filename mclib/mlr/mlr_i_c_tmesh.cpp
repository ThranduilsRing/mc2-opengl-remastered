//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"
#include <memory>


//#############################################################################
//###### MLRIndexedTriMesh with color but no lighting one texture layer  ######
//#############################################################################

MLR_I_C_TMesh::ClassData*
	MLR_I_C_TMesh::DefaultData = nullptr;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_TMesh::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLR_I_C_TMeshClassID,
			"MidLevelRenderer::MLR_I_C_TMesh",
			MLR_I_TMesh::DefaultData,
			(MLRPrimitiveBase::Factory)&Make
		);
	Register_Object(DefaultData);

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_TMesh::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_C_TMesh::MLR_I_C_TMesh(
	ClassData *class_data,
	MemoryStream *stream,
	int version
):
	MLR_I_TMesh(class_data, stream, version)
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
		}
		break;
	}

	actualColors = &colors;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_C_TMesh::MLR_I_C_TMesh(ClassData *class_data):
	MLR_I_TMesh(class_data), colors(0)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);

	actualColors = &colors;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_C_TMesh::~MLR_I_C_TMesh()
{
	Check_Object(this);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_C_TMesh*
	MLR_I_C_TMesh::Make(
		MemoryStream *stream,
		int version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	MLR_I_C_TMesh *mesh = new MLR_I_C_TMesh(DefaultData, stream, version);
	gos_PopCurrentHeap();

	return mesh;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_TMesh::Save(MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);

	MLR_I_TMesh::Save(stream);

	Stuff::DynamicArrayOf<DWORD> smallColors;
	int i, len = static_cast<int>(colors.GetLength());

	const Stuff::RGBAColor *data = colors.GetData();

	smallColors.SetLength(len);

	for(i=0;i<len;i++)
	{
		smallColors[i] = GOSCopyColor(data+i);
	}

	MemoryStreamIO_Write(stream, &smallColors);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
bool
	MLR_I_C_TMesh::Copy(MLR_I_C_PMesh *pMesh)
{
	Check_Object(this);
	Check_Object(pMesh);

	int len;
	RGBAColor *_colors;

	MLR_I_TMesh::Copy(pMesh);

	pMesh->GetColorData(&_colors, &len);
	SetColorData(_colors, len);

	return true;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_TMesh::SetColorData(
		const RGBAColor *data,
		int dataSize
	)
{
	Check_Object(this); 
	Check_Pointer(data);

	Verify(coords.GetLength() == 0 || dataSize == coords.GetLength());
	Verify(texCoords.GetLength() == 0 || dataSize == texCoords.GetLength());

	colors.AssignData(data, dataSize);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_TMesh::GetColorData(
		RGBAColor **data,
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
	MLR_I_C_TMesh::PaintMe(
		const RGBAColor *paintMe
		
	)
{
	Check_Object(this);
	// original color is lost !!!;

	int k, len = static_cast<int>(colors.GetLength());

	for(k=0;k<len;k++)
	{
		colors[k] = *paintMe;
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_TMesh::HurtMe(const glm::mat4& pain, Stuff::Scalar radius)
{
	Check_Object(this); 

	Verify(colors.GetLength() == coords.GetLength());

	int i, len = static_cast<int>(colors.GetLength());

	glm::vec3 painpoint;
	glm::vec3 diff;
	glm::vec3 up, left, forward;
	Scalar x, y, sqRadius = radius*radius;

	painpoint = mc2::math::getTranslation(pain);
	left = mc2::math::localLeftInWorld(pain);
	up = mc2::math::localUpInWorld(pain);
	forward = mc2::math::localForwardInWorld(pain);

	if(FindBackFace(painpoint) && FindVisibleVertices())
	{
		for(i=0;i<len;i++)
		{
			if(visibleIndexedVertices[i]>0)
			{
				diff = coords[i] - painpoint;

				x = glm::dot(diff, left);
				y = glm::dot(diff, up);

				if((x*x + y*y) < sqRadius)
				{
					colors[i].red = 0.0f;
					colors[i].green = 0.0f;
					colors[i].blue = 0.0f;
					colors[i].alpha = 1.0f;
				}
			}
		}
	}
	visibleIndexedVerticesKey = false;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_C_TMesh::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

extern DWORD gEnableTextureSort, gEnableAlphaSort;



//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_C_TMesh*
	MidLevelRenderer::CreateIndexedTriCube_Color_NoLit(
		Scalar half, 
		MLRState *state
	)
{
	return nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRShape*
	MidLevelRenderer::CreateIndexedTriIcosahedron_Color_NoLit(
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

	int uniquePoints = 0;
	for (k=0;k<20;k++)
	{
		triDrawn = 0;
		MLR_I_C_TMesh *mesh = new MLR_I_C_TMesh();
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
