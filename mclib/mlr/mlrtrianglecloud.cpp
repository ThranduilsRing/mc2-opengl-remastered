//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"

#if !defined(MLR_MLRCLIPTRICK_HPP)
#include<mlr/mlrcliptrick.hpp>
#endif
extern DWORD gShowClippedPolys;

//#############################################################################
//#######################    MLRTriangleCloud    ##############################
//#############################################################################

std::vector<MLRClippingState>
	MLRTriangleCloud::clipPerVertex;


MLRTriangleCloud::ClassData*
	MLRTriangleCloud::DefaultData = nullptr;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRTriangleCloud::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLRTriangleCloudClassID,
			"MidLevelRenderer::MLRTriangleCloud",
			MLREffect::DefaultData
		);
	Register_Object(DefaultData);
	
	clipPerVertex.resize(Limits::Max_Number_Vertices_Per_Mesh);
	
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRTriangleCloud::TerminateClass()
{
	std::vector<MLRClippingState>().swap(clipPerVertex);
	

	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRTriangleCloud::MLRTriangleCloud(int nr) :
	MLREffect(nr, DefaultData)
{
	Verify(gos_GetCurrentHeap() == Heap);
	usedNrOfTriangles = nullptr;

	Check_Pointer(this);
	
	drawMode = SortData::TriList;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRTriangleCloud::~MLRTriangleCloud()
{
	Check_Object(this);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void 
	MLRTriangleCloud::SetData
	(
		const int *count,
		const glm::vec3 *point_data,
		const Stuff::RGBAColor *color_data
	)
{
	Check_Pointer(this);

	usedNrOfTriangles = count;
	Verify(*usedNrOfTriangles <= maxNrOf);
	points = point_data;
	colors = color_data;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void 
	MLRTriangleCloud::TestInstance() const
{
	if (usedNrOfTriangles)
	{
		Check_Pointer(usedNrOfTriangles);
		Verify(*usedNrOfTriangles >= 0);
		Verify(*usedNrOfTriangles <= maxNrOf);
	}
}

