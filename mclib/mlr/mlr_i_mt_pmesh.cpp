//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"
#include <memory>


//#############################################################################
//###### MLRIndexedPolyMesh with no color no lighting two texture layer  ######
//#############################################################################

MLR_I_MT_PMesh::ClassData*
	MLR_I_MT_PMesh::DefaultData = nullptr;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_MT_PMesh::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLR_I_MT_PMeshClassID,
			"MidLevelRenderer::MLR_I_MT_PMesh",
			MLR_I_PMesh::DefaultData,
			(MLRPrimitiveBase::Factory)&Make
		);
	Register_Object(DefaultData);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_MT_PMesh::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_MT_PMesh::MLR_I_MT_PMesh(
	ClassData *class_data,
	MemoryStream *stream,
	int version
):
	MLR_I_PMesh(class_data, stream, version)
{
	Check_Pointer(this);
	Check_Pointer(stream);
	Verify(gos_GetCurrentHeap() == Heap);

	multiTexCoords.SetLength(Limits::Max_Number_Of_Multitextures);
	multiTexCoordsPointers.SetLength(Limits::Max_Number_Of_Multitextures);
	multiState.SetLength(Limits::Max_Number_Of_Multitextures);
	multiReferenceState.SetLength(Limits::Max_Number_Of_Multitextures);

	multiTexCoords[0] = &texCoords;
	multiTexCoordsPointers[0] = texCoords.data();

	multiReferenceState[0] = referenceState;

	*stream >> passes;

	for(int i=1;i<passes;i++)
	{
		multiReferenceState[i].Load(stream, version);

		multiTexCoords[i] = new std::vector<Vector2DScalar>;
		Register_Pointer(multiTexCoords[i]);

		MemoryStreamIO_Read(stream, multiTexCoords[i]);

		multiTexCoordsPointers[i] = multiTexCoords[i]->data();
	}

	currentNrOfPasses = passes;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_MT_PMesh::MLR_I_MT_PMesh(ClassData *class_data):
	MLR_I_PMesh(class_data)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);

	multiTexCoords.SetLength(Limits::Max_Number_Of_Multitextures);
	multiTexCoordsPointers.SetLength(Limits::Max_Number_Of_Multitextures);

	multiTexCoords[0] = &texCoords;
	multiTexCoordsPointers[0] = texCoords.data();

	multiState.SetLength(Limits::Max_Number_Of_Multitextures);
	multiReferenceState.SetLength(Limits::Max_Number_Of_Multitextures);

	passes = 1;

	currentNrOfPasses = passes;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_MT_PMesh::Copy(MLR_I_PMesh *pMesh)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);

	int num;
	glm::vec3 *points;

	referenceState = pMesh->GetReferenceState();
	multiReferenceState[0] = referenceState;

	pMesh->GetCoordData(&points, &num);
	SetCoordData(points, num);

	Vector2DScalar *tex;
	pMesh->GetTexCoordData(&tex, &num);
	SetTexCoordData(tex, num);

	multiTexCoords[0] = &texCoords;
	multiTexCoordsPointers[0] = texCoords.data();

	unsigned char *length_array;
	pMesh->GetSubprimitiveLengths(&length_array, &num);
	SetSubprimitiveLengths(length_array, num);

	drawMode = pMesh->GetSortDataMode();

	unsigned short *index_array;
	pMesh->GetIndexData(&index_array, &num);
	SetIndexData(index_array, num);

	facePlanes.resize(GetNumPrimitives());
	testList.resize(GetNumPrimitives());

	FindFacePlanes();

	// Release the owned pass slots [1,passes) before collapsing to one pass;
	// slot 0 borrows texCoords and is not owned.
	for(int i=1;i<passes;i++)
	{
		Unregister_Pointer(multiTexCoords[i]);
		std::unique_ptr<std::vector<Vector2DScalar>> owned(multiTexCoords[i]);
		multiTexCoords[i] = nullptr;
		multiTexCoordsPointers[i] = nullptr;
	}

	passes = 1;

	currentNrOfPasses = passes;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_MT_PMesh::~MLR_I_MT_PMesh()
{
	Check_Object(this);

	for(int i=1;i<passes;i++)
	{
		Unregister_Pointer(multiTexCoords[i]);
		delete multiTexCoords[i];
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_MT_PMesh*
	MLR_I_MT_PMesh::Make(
		MemoryStream *stream,
		int version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	MLR_I_MT_PMesh *mesh = new MLR_I_MT_PMesh(DefaultData, stream, version);
	gos_PopCurrentHeap();

	return mesh;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_MT_PMesh::Save(MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);

	MLR_I_PMesh::Save(stream);

	*stream << passes;

	for(int i=1;i<passes;i++)
	{
		multiReferenceState[i].Save(stream);

		MemoryStreamIO_Write(stream, multiTexCoords[i]);
	}
}
 
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_MT_PMesh::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_MT_PMesh::InitializeDrawPrimitive(unsigned char vis, int parameter)
{
	MLR_I_PMesh::InitializeDrawPrimitive(vis, parameter);

	currentNrOfPasses = passes;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_MT_PMesh::SetTexCoordData(
		const Vector2DScalar *data,
		int dataSize,
		int pass
	)
{
	Check_Object(this); 
	Check_Pointer(data);

	Verify(pass>=0 && pass<Limits::Max_Number_Of_Multitextures);
	Verify(coords.size() == 0 || dataSize == coords.size());

	if(pass == 0)
	{
		texCoords.assign(data, data + dataSize);

		multiTexCoordsPointers[0] = texCoords.data();
	}
	else
	{
		if(pass == passes)
		{
			Verify(dataSize == multiTexCoords[pass-1]->size());

			gos_PushCurrentHeap(Heap);
			multiTexCoords[pass] = new std::vector<Vector2DScalar>;
			Register_Pointer(multiTexCoords[pass]);
			multiTexCoords[pass]->assign(data, data + dataSize);
			gos_PopCurrentHeap();

			multiTexCoordsPointers[pass] = multiTexCoords[pass]->data();
			
			passes++;
			currentNrOfPasses++;
		}
		else
		{
			STOP(("Add texture layers only one by one !!!"));
		}
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_MT_PMesh::GetTexCoordData(
		const Vector2DScalar **data,
		int *dataSize,
		int pass
	)
{
	Check_Object(this); 
	Check_Pointer(data);

	Verify(pass>=0 && pass<passes);

	*data = multiTexCoords[pass]->data();
	*dataSize = static_cast<int>(multiTexCoords[pass]->size());
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_MT_PMesh::SetTexCoordDataPointer(const Vector2DScalar *data)
{
	Check_Pointer(data);

	Verify(currentNrOfPasses>=passes);

	multiTexCoordsPointers[currentNrOfPasses++] = data;
}



//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRShape*
	MidLevelRenderer::CreateIndexedIcosahedron_NoColor_NoLit_MultiTexture(
		IcoInfo& icoInfo,
		DynamicArrayOf<MLRState> *states
	)
{
	gos_PushCurrentHeap(Heap);
	MLRShape *ret = new MLRShape(20);
	Register_Object(ret);

	int i, j, k;
	int nrOfPasses = static_cast<int>(states->GetLength());
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

	auto texCoords = std::make_unique<std::unique_ptr<Vector2DScalar[]>[]>(nrOfPasses);
	for(i=0;i<nrOfPasses;i++)
	{
		texCoords[i] = std::make_unique_for_overwrite<Vector2DScalar[]>(nrTri*3);
		Register_Pointer(texCoords[i].get());
	}

	auto indexOwner = std::make_unique_for_overwrite<unsigned short[]>(nrTri*3);
	unsigned short* index = indexOwner.get();
	Register_Pointer(index);

	int uniquePoints = 0;
	for (k=0;k<20;k++)
	{
		triDrawn = 0;
		MLR_I_MT_PMesh *mesh = new MLR_I_MT_PMesh();
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

		for(j=0;j<nrOfPasses;j++)
		{
			if(icoInfo.indexed==true)
			{
				for(i=0;i<uniquePoints;i++)
				{
					texCoords[j][i] = 
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
					texCoords[j][3*i] = 
						Vector2DScalar(
							(1.0f + coords[3*i].x)/2.0f,
							(1.0f + coords[3*i].y)/2.0f
						);
					texCoords[j][3*i+1] = 
						Vector2DScalar(
							(1.0f + coords[3*i+1].x)/2.0f,
							(1.0f + coords[3*i+1].y)/2.0f
						);
					texCoords[j][3*i+2] = 
						Vector2DScalar(
							(1.0f + coords[3*i+2].x)/2.0f,
							(1.0f + coords[3*i+2].y)/2.0f
						);
				}
			}
			mesh->SetTexCoordData(texCoords[j].get(), uniquePoints, j);
		}

		for(i=0;i<nrOfPasses;i++)
		{
			mesh->SetReferenceState((*states)[i], i);
		}

		ret->Add(mesh);
		mesh->DetachReference();
	}

	for(i=0;i<nrOfPasses;i++)
	{
		Unregister_Pointer(texCoords[i].get());
		texCoords[i].reset();
	}

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
