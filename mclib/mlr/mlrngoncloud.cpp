//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"

#if !defined(MLR_MLRCLIPTRICK_HPP)
#include<mlr/mlrcliptrick.hpp>
#endif
extern DWORD gShowClippedPolys;

//#############################################################################
//#######################    MLRNGonCloud    ##################################
//#############################################################################

DynamicArrayOf<MLRClippingState>
	*MLRNGonCloud::clipPerVertex;



MLRNGonCloud::ClassData*
	MLRNGonCloud::DefaultData = nullptr;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRNGonCloud::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLRNGonCloudClassID,
			"MidLevelRenderer::MLRNGonCloud",
			MLREffect::DefaultData
		);
	Register_Object(DefaultData);

	clipPerVertex = new DynamicArrayOf<MLRClippingState> (Limits::Max_Number_Vertices_Per_Mesh);
	Register_Object(clipPerVertex);
	
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRNGonCloud::TerminateClass()
{
	Unregister_Object(clipPerVertex);
	delete clipPerVertex;
	

	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRNGonCloud::MLRNGonCloud(int vertices, int nr) :
	MLREffect(nr, DefaultData)
{
	Verify(gos_GetCurrentHeap() == Heap);
	usedNrOfNGons = nullptr;

	numOfVertices = vertices;

	Check_Pointer(this);
	
	Verify(vertices*nr >= 0 && vertices*nr<=Limits::Max_Number_Vertices_Per_Mesh);
	specialClipColors.SetLength(vertices);
	drawMode = SortData::TriList;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRNGonCloud::~MLRNGonCloud()
{
	Check_Object(this);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void 
	MLRNGonCloud::SetData
	(
		const int *count,
		const glm::vec3 *point_data,
		const Stuff::RGBAColor *color_data
	)
{
	Check_Pointer(this);

	usedNrOfNGons = count;
	Verify(*usedNrOfNGons <= maxNrOf);
	points = point_data;
	colors = color_data;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void 
	MLRNGonCloud::Draw (DrawEffectInformation *dInfo, GOSVertexPool *allVerticesToDraw, MLRSorter *sorter)
{
	Check_Object(this);

	worldToEffect = glm::inverse((*dInfo->effectToWorld));

	Transform(*usedNrOfNGons, numOfVertices);

	if( Clip(dInfo->clippingFlags, allVerticesToDraw) )
	{
		sorter->AddEffect(this, dInfo->state);
	}
}

static MLRClippingState theAnd, theOr, theTest;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
int 
	MLRNGonCloud::Clip(MLRClippingState clippingFlags, GOSVertexPool *vt)
{
	Check_Object(this);

	int i, j, k;
	int end, len = *usedNrOfNGons, ret = 0;

#if	EFECT_CLIPPED				// this effect gets not clipped
	int l, mask, k1, ct=0;
	Scalar a=0.0f, bc0, bc1;
#endif

	
	Check_Object(vt);
	gos_vertices = vt->GetActualVertexPool();
	Check_Pointer(gos_vertices);
	numGOSVertices = 0;

	//
	//--------------------------------------
	// See if we don't have to draw anything
	//--------------------------------------
	//
	if(clippingFlags.GetClippingState() == 0 || len <= 0)
	{
		if(len <= 0)
		{
			visible = 0;
		}
		else
		{
			//
			//-------------------------------
			// Handle the non-indexed version
			//-------------------------------
			//
			for(i=0,j=0;i<len;i++,j += numOfVertices)
			{
//				if(IsOn(i) == false)
//				{
//					continue;
//				}

				DWORD inColor = GOSCopyColor(&colors[2*i]);
				DWORD outColor = GOSCopyColor(&colors[2*i+1]);
				for(int z=1; z < numOfVertices-1; z++)
				{
					GOSCopyTriangleData(
						&gos_vertices[numGOSVertices],
						transformedCoords->GetData(),
						j, z+j+1, z+j,
						true
					);
					gos_vertices[numGOSVertices].argb = inColor;
					gos_vertices[numGOSVertices+1].argb = outColor;
					gos_vertices[numGOSVertices+2].argb = outColor;

					numGOSVertices += 3;
				}
			}

			Check_Object(vt);
			vt->Increase(numGOSVertices);
			
			visible = numGOSVertices ? 1 : 0;
		}

		return visible;
	}

	//
	//-------------------------------
	// Handle the non-indexed version
	//-------------------------------
	//
	//
	//-----------------------------------------------------------------
	// Step through each polygon, making sure that we don't try to clip
	// backfaced polygons
	//-----------------------------------------------------------------
	//
	for(i=0,j=0;i<len;i++,j+=numOfVertices)
	{
//		if(IsOn(i) == false)
//		{
//			continue;
//		}

		TurnVisible(i);

		//
		//---------------------------------------------------------------
		// Test each vertex of the polygon against the allowed clipping
		// planes, and accumulate status for which planes always clip and
		// which planes clipped at least once
		//---------------------------------------------------------------
		//
		theAnd.SetClippingState(0x3f);
		theOr.SetClippingState(0);
		end = j+numOfVertices;

		Stuff::Vector4D *v4d = transformedCoords->GetData()+j;
		MLRClippingState *cs = clipPerVertex->GetData()+j;

		for(k=j;k<end;k++,v4d++,cs++)
		{
			cs->Clip4dVertex(v4d);

			theAnd &= (*clipPerVertex)[k];

			theOr |= (*clipPerVertex)[k];

#ifdef LAB_ONLY
			if(*cs==0)
			{
				Set_Statistic(NonClippedVertices, NonClippedVertices+1);
			}
			else
			{
				Set_Statistic(ClippedVertices, ClippedVertices+1);
			}
#endif
		}

		theAnd = theOr = 0;		//ASSUME NO CLIPPING NEEDED FOR MC2.  Its just not done here!

		//
		//-------------------------------------------------------------------
		// If any bit is set for all vertices, then the polygon is completely
		// outside the viewing space and we don't have to draw it.  On the
		// other hand, if no bits at all were ever set, we can do a trivial
		// accept of the polygon
		//-------------------------------------------------------------------
		//
		// MC2 has always accepted these polygons without CPU clipping.
		{
			TurnVisible(i);

			DWORD inColor = GOSCopyColor(&colors[2*i]);
			DWORD outColor = GOSCopyColor(&colors[2*i+1]);

			for(int z=1; z < numOfVertices-1; z++)
			{
				GOSCopyTriangleData(
					&gos_vertices[numGOSVertices],
					transformedCoords->GetData(),
					j, z+j+1, z+j,
					true
				);
				gos_vertices[numGOSVertices].argb = inColor;
				gos_vertices[numGOSVertices+1].argb = outColor;
				gos_vertices[numGOSVertices+2].argb = outColor;

#ifdef LAB_ONLY
				if(gShowClippedPolys)
				{
					gos_vertices[numGOSVertices].argb = 0xff0000ff;
					gos_vertices[numGOSVertices].u = 0.0f;
					gos_vertices[numGOSVertices].v = 0.0f;

					gos_vertices[numGOSVertices+1].argb = 0xff0000ff;
					gos_vertices[numGOSVertices+1].u = 0.0f;
					gos_vertices[numGOSVertices+1].v = 0.0f;

					gos_vertices[numGOSVertices+2].argb = 0xff0000ff;
					gos_vertices[numGOSVertices+2].u = 0.0f;
					gos_vertices[numGOSVertices+2].v = 0.0f;
				}
#endif

				numGOSVertices += 3;
			}

			ret++;
		}
	}


	vt->Increase(numGOSVertices);

	visible = numGOSVertices ? 1 : 0;

	if(visible)
	{
	}
	else
	{
	}

	return visible;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void 
	MLRNGonCloud::TestInstance() const
{
	if (usedNrOfNGons)
	{
		Check_Pointer(usedNrOfNGons);
		Verify(*usedNrOfNGons >= 0);
		Verify(*usedNrOfNGons <= maxNrOf);
	}
}

