//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"


//#############################################################################
//############################   MLREffect    #################################
//#############################################################################

void EffectClipPolygon::Init()
{
	Verify(gos_GetCurrentHeap() == StaticHeap);
	coords.resize(Limits::Max_Number_Vertices_Per_Polygon);
	colors.resize(Limits::Max_Number_Vertices_Per_Polygon);
	texCoords.resize(Limits::Max_Number_Vertices_Per_Polygon);
	clipPerVertex.resize(Limits::Max_Number_Vertices_Per_Polygon);
}

void EffectClipPolygon::Destroy()
{
	decltype(coords)().swap(coords);
	decltype(colors)().swap(colors);
	decltype(texCoords)().swap(texCoords);
	decltype(clipPerVertex)().swap(clipPerVertex);
}

//#############################################################################
//############################   MLREffect    #################################
//#############################################################################


MLREffect::ClassData*
	MLREffect::DefaultData = nullptr;
std::unique_ptr<EffectClipPolygon[]>
	MLREffect::clipBuffer;

std::vector<Vector4D>
	MLREffect::transformedCoords;


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLREffect::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLREffectClassID,
			"MidLevelRenderer::MLREffect",
			RegisteredClass::DefaultData
		);
	Register_Object(DefaultData);
	
	transformedCoords.resize(Limits::Max_Number_Vertices_Per_Mesh);

	clipBuffer = std::make_unique<EffectClipPolygon[]>(2);
	Register_Pointer(clipBuffer.get());

	clipBuffer[0].Init();
	clipBuffer[1].Init();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLREffect::TerminateClass()
{
	clipBuffer[1].Destroy();
	clipBuffer[0].Destroy();
	Unregister_Pointer(clipBuffer.get());
	clipBuffer.reset();

	std::vector<Vector4D>().swap(transformedCoords);

	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLREffect::MLREffect(int nr, ClassData *class_data):
	RegisteredClass(class_data)
{
	Verify(gos_GetCurrentHeap() == Heap);
	visible = 0;
	maxNrOf = nr;

	testList.resize(maxNrOf);
	for(int i=0; i < maxNrOf; i++)
	{
		testList[i] = 0;
	}
	
	TurnAllOff();
	TurnAllVisible();

	worldToEffect = glm::mat4(1.0f);

	gos_vertices = nullptr;
	numGOSVertices = 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLREffect::~MLREffect()
{
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void 
	MLREffect::Transform(int nrOfUsedEffects, int nrOfVertices)
{
	Check_Object(this);

	Start_Timer(Transform_Time);
	int i, j, k;

	for(i=0,j=0;i<nrOfUsedEffects;i++,j+=nrOfVertices)
	{
		if(IsOn(i) == false)
		{
			continue;
		}

		for(k=j;k<j+nrOfVertices;k++)
		{
			transformedCoords[k].MultiplyPoint(points[k], effectToClipMatrix);
		}
	}
	Stop_Timer(Transform_Time);
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLREffect::TurnAllOn()
{
	Check_Object(this);
	int i;

	for(i=0;i<maxNrOf;i++)
	{
		testList[i] |= 2;
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLREffect::TurnAllOff()
{
	Check_Object(this);
	int i;

	for(i=0;i<maxNrOf;i++)
	{
		testList[i] &= ~2;
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLREffect::TurnAllVisible()
{
	Check_Object(this);
	int i;

	for(i=0;i<maxNrOf;i++)
	{
		testList[i] |= 1;
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLREffect::TurnAllInVisible()
{
	Check_Object(this);
	int i;

	for(i=0;i<maxNrOf;i++)
	{
		testList[i] &= ~1;
	}
}
