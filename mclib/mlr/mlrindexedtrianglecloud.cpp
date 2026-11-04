//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"

#if !defined(MLR_MLRCLIPTRICK_HPP)
#include<mlr/mlrcliptrick.hpp>
#endif

#if !defined(MLR_MLRINDEXEDTRIANGLECLOUD_HPP)
#include<mlr/mlrindexedtrianglecloud.hpp>
#endif
extern DWORD gShowClippedPolys;
extern unsigned short *indexOffset;	// [MidLevelRenderer::Max_Number_Vertices_Per_Mesh]

//#############################################################################
//####################    MLRIndexedTriangleCloud    ##########################
//#############################################################################

std::unique_ptr<DynamicArrayOf<unsigned char>>
	MLRIndexedTriangleCloud::visibleIndexedVertices;


MLRIndexedTriangleCloud::ClassData*
	MLRIndexedTriangleCloud::DefaultData = nullptr;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRIndexedTriangleCloud::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLRIndexedTriangleCloudClassID,
			"MidLevelRenderer::MLRIndexedTriangleCloud",
			MLRTriangleCloud::DefaultData
		);
	Register_Object(DefaultData);
	
	visibleIndexedVertices = std::make_unique<DynamicArrayOf<unsigned char>>(Limits::Max_Number_Vertices_Per_Mesh);
	Register_Pointer(visibleIndexedVertices.get());

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRIndexedTriangleCloud::TerminateClass()
{
	Unregister_Pointer(visibleIndexedVertices.get());
	visibleIndexedVertices.reset();

	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRIndexedTriangleCloud::MLRIndexedTriangleCloud(int nr) :
	MLRTriangleCloud(nr)
{
	Verify(gos_GetCurrentHeap() == Heap);
	usedNrOfPoints = nullptr;

	Check_Pointer(this);
	
	drawMode = SortData::TriIndexedList;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRIndexedTriangleCloud::~MLRIndexedTriangleCloud()
{
	Check_Object(this);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void 
	MLRIndexedTriangleCloud::SetData
	(
		const int *tri_count,
		const int *point_count,
		const unsigned short *index_data,
		const glm::vec3 *point_data,
		const Stuff::RGBAColor *color_data,
		const Vector2DScalar *uv_data
	)
{
	Check_Pointer(this);

	usedNrOfTriangles = tri_count;
	usedNrOfPoints = point_count;

	Verify(*usedNrOfTriangles <= maxNrOf);

	index = index_data;
	points = point_data;
	colors = color_data;
	texCoords = uv_data;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void 
	MLRIndexedTriangleCloud::Draw (DrawEffectInformation *dInfo, GOSVertexPool *allVerticesToDraw, MLRSorter *sorter)
{
	Check_Object(this);

	worldToEffect = glm::inverse((*dInfo->effectToWorld));

	Vector4D *v4 = transformedCoords->GetData();
	for(int k=0;k<*usedNrOfPoints;k++, v4++)
	{
		v4->MultiplyPoint(points[k], effectToClipMatrix);
		(*clipPerVertex)[k].Clip4dVertex(v4);
	}


#if 0
	Lighting(*shape->worldToShape, dInfo->activeLights, dInfo->nrOfActiveLights);
#endif

	if( Clip(dInfo->clippingFlags, allVerticesToDraw) )
	{
		sorter->AddEffect(this, dInfo->state);
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
int 
	MLRIndexedTriangleCloud::Clip(MLRClippingState clippingFlags, GOSVertexPool *vt)
{
	Verify(*usedNrOfTriangles > 0);
	
	//
	//------------------------
	// Handle the indexed case
	//------------------------
	//

	memset(visibleIndexedVertices->GetData(), 0, *usedNrOfPoints);

	//
	//-----------------------------------------------------------------
	// Step through each polygon, making sure that we don't try to clip
	// backfaced polygons
	//-----------------------------------------------------------------
	//
	int i, j;
	int index0, index1, index2;

	for(i=0,j=0;i<*usedNrOfTriangles;j+=3,++i)
	{
		index0 = index[j];
		index1 = index[j+1];
		index2 = index[j+2];
		// MC2 has always accepted these polygons without CPU clipping.
		{
			testList[i] = 1;

			(*visibleIndexedVertices)[index0] = 1;
			(*visibleIndexedVertices)[index1] = 1;
			(*visibleIndexedVertices)[index2] = 1;

#ifdef LAB_ONLY
			Set_Statistic(PolysClippedButInside, PolysClippedButInside+1);
#endif
		}
	}

	Check_Object(vt);
	gos_vertices = vt->GetActualVertexPool();
	numGOSVertices = 0;
	gos_indices = vt->GetActualIndexPool();
	numGOSIndices = 0;

	unsigned short strideIndex;
	for(j=0,strideIndex=0;j<*usedNrOfPoints;j++)
	{
		if((*visibleIndexedVertices)[j] == 0)
		{
			strideIndex++;
		}
		else
		{
			indexOffset[j] = static_cast<unsigned short>(j-strideIndex);

			GOSCopyData(
				&gos_vertices[numGOSVertices],
				transformedCoords->GetData(),
				colors,
				texCoords,
				j
			);

#ifdef LAB_ONLY
			if(gShowClippedPolys)
			{
				gos_vertices[numGOSVertices].argb = 0xff0000ff;
				gos_vertices[numGOSVertices].u = 0.0f;
				gos_vertices[numGOSVertices].v = 0.0f;
			}
#endif

			numGOSVertices++;
		}
	}

	for(i=0,j=0;i<*usedNrOfTriangles;j+=3,++i)
	{
		if(testList[i] == 0)
		{
			continue;
		}

		Verify((vt->GetLastIndex() + 3 + numGOSIndices) < vt->GetLength());

		Verify(indexOffset[index[j]] < numGOSVertices);
		gos_indices[numGOSIndices] = indexOffset[index[j]];

		Verify(indexOffset[index[j+2]] < numGOSVertices);
		gos_indices[numGOSIndices+1] = indexOffset[index[j+2]];

		Verify(indexOffset[index[j+1]] < numGOSVertices);
		gos_indices[numGOSIndices+2] = indexOffset[index[j+1]];

		numGOSIndices += 3;
	}


	vt->Increase(numGOSVertices);
	vt->IncreaseIndex(numGOSIndices);

	return numGOSIndices;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void 
	MLRIndexedTriangleCloud::TestInstance() const
{
	if (usedNrOfTriangles)
	{
		Check_Pointer(usedNrOfTriangles);
		Verify(*usedNrOfTriangles >= 0);
		Verify(*usedNrOfTriangles <= maxNrOf);
	}
}

