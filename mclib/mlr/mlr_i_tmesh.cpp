//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "mlr_i_tmesh_internal.h"



//#define TOP_DOWN_ONLY

//#############################################################################
//###### MLRIndexedTriMesh with no color no lighting one texture layer  ######
//#############################################################################

MLR_I_TMesh::ClassData*
	MLR_I_TMesh::DefaultData = nullptr;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_TMesh::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLR_I_TMeshClassID,
			"MidLevelRenderer::MLR_I_TMesh",
			MLRIndexedPrimitiveBase::DefaultData,
			(MLRPrimitiveBase::Factory)&Make
		);
	Register_Object(DefaultData);

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_TMesh::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_TMesh::MLR_I_TMesh(
	ClassData *class_data,
	MemoryStream *stream,
	int version
):
	MLRIndexedPrimitiveBase(class_data, stream, version)
{
	Check_Pointer(this);
	Check_Pointer(stream);
	Verify(gos_GetCurrentHeap() == Heap);

	numOfTriangles = static_cast<int>(index.size())/3;

	facePlanes.resize(numOfTriangles);
	testList.resize(numOfTriangles);

	FindFacePlanes();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_TMesh::MLR_I_TMesh(ClassData *class_data):
	MLRIndexedPrimitiveBase(class_data)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);

	drawMode = SortData::TriIndexedList;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_TMesh::~MLR_I_TMesh()
{
	Check_Object(this);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_TMesh*
	MLR_I_TMesh::Make(
		MemoryStream *stream,
		int version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	MLR_I_TMesh *mesh = new MLR_I_TMesh(DefaultData, stream, version);
	gos_PopCurrentHeap();

	return mesh;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_TMesh::Save(MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);

	MLRIndexedPrimitiveBase::Save(stream);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_TMesh::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
bool
	MLR_I_TMesh::Copy(MLR_I_PMesh *pMesh)
{
	Check_Object(this);
	Check_Object(pMesh);
	Verify(gos_GetCurrentHeap() == Heap);

	int len;
	unsigned short *_index;
	glm::vec3 *_coords;
	Vector2DScalar *_texCoords;

	pMesh->GetCoordData(&_coords, &len);
	SetCoordData(_coords, len);

	pMesh->GetIndexData(&_index, &len);
	SetIndexData(_index, len);

	SetSubprimitiveLengths(nullptr, pMesh->GetNumPrimitives());

	facePlanes.resize(GetNumPrimitives());
	testList.resize(GetNumPrimitives());

	FindFacePlanes();

	pMesh->GetTexCoordData(&_texCoords, &len);
	SetTexCoordData(_texCoords, len);

	referenceState = pMesh->GetReferenceState();

	visibleIndexedVerticesKey = false;
	visibleIndexedVertices.SetLength(coords.size());

	return true;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_TMesh::InitializeDrawPrimitive(unsigned char vis, int parameter)
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
	MLR_I_TMesh::FindFacePlanes()
{
	Check_Object(this); 

	int i, j, numPrimitives = GetNumPrimitives();
	glm::vec3 v;

	Verify(index.size() > 0);

	for(i=0,j=0;i<numPrimitives;++i,j+=3)
	{
		facePlanes[i].buildPlane(
			coords[index[j]],
			(coords[index[j+1]]),
			(coords[index[j+2]])
		);

		;
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
int
	MLR_I_TMesh::FindBackFace(const glm::vec3& u)
{
	Check_Object(this);

	int ret = 0;
	unsigned char *iPtr;
	const mc2::math::Plane *p;

	if(numOfTriangles <= 0)
	{
		visible = 0;

		return 0;
	}

	p = &facePlanes[numOfTriangles-1];
	iPtr = &testList[0];

	if(state.GetBackFaceMode() == MLRState::BackFaceOffMode)
	{
		ResetTestList();
		ret = 1;
	}
	else
	{
		memset(iPtr, 0, numOfTriangles);
		for(int i=numOfTriangles-1;i>=0;p--,i--)
		{
			if(p->distanceTo(u)>= 0.0f)
			{
				iPtr[i] = static_cast<unsigned char>((ret = 1));
			}
		}
	}

	visible = static_cast<unsigned char>(ret);

	FindVisibleVertices();

	return ret;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLR_I_TMesh::ResetTestList()
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
	MLR_I_TMesh::FindVisibleVertices()
{
	Check_Object(this);
	Verify(index.size() > 0);

	int ret, i, j;
	DWORD *indices = reinterpret_cast<DWORD *>(index.data());

	for(i=0,j=0,ret=0;i<(numOfTriangles>>1);++i)
	{
		if(testList[i<<1] != 0)
		{
			visibleIndexedVertices[(*(indices+j))&0xffff] = 1;
			visibleIndexedVertices[(*(indices+j))>>16] = 1;
			visibleIndexedVertices[(*(indices+j+1))&0xffff] = 1;
			ret = 1;
		}
		if(testList[(i<<1)+1] != 0)
		{
			visibleIndexedVertices[(*(indices+j+1))>>16] = 1;
			visibleIndexedVertices[(*(indices+j+2))&0xffff] = 1;
			visibleIndexedVertices[(*(indices+j+2))>>16] = 1;
			ret = 1;
		}
		j+=3;
	}

	if(numOfTriangles&1)
	{
		if(testList[numOfTriangles-1] != 0)
		{
			j <<= 1;
			visibleIndexedVertices[index[j++]] = 1;
			visibleIndexedVertices[index[j++]] = 1;
			visibleIndexedVertices[index[j++]] = 1;
			ret = 1;
		}
	}

	visibleIndexedVerticesKey = true;

	return ret;
}

extern DWORD gEnableTextureSort, gEnableAlphaSort;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/*
void
	MLR_I_TMesh::Transform(glm::mat4 *mat)
{
	Check_Object(this);

	Verify(index.size() > 0);
	int i, len = coords.size();

	if(visibleIndexedVerticesKey == false)
	{
		FindVisibleVertices();
	}

	Stuff::Vector4D *v4d = transformedCoords.GetData();
	glm::vec3 *p3d = coords.data();
	MLRClippingState *cs = clipPerVertex.GetData();
	unsigned char *viv = visibleIndexedVertices.GetData();

	for(i=0;i<len;i++,p3d++,v4d++,cs++,viv++)
	{
		if(*viv == 0)
		{
			continue;
		}
		
		v4d->Multiply(*p3d, *mat);
#ifdef LAB_ONLY
		Statistics::MLR_TransformedVertices++;
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
			Statistics::MLR_NonClippedVertices++;
		}
		else
		{
			Statistics::MLR_ClippedVertices++;
		}
#endif
	}
}
*/



//---------------------------------------------------------------------------
//
