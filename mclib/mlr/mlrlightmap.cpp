//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"

#if !defined(MLR_MLRCLIPTRICK_HPP)
#include<mlr/mlrcliptrick.hpp>
#endif

//#############################################################################
//###########################    MLRLightMap    ###############################
//#############################################################################

MLRLightMap::ClassData*
	MLRLightMap::DefaultData = nullptr;
Stuff::MemoryStream
	*MLRLightMap::stream;
GOSVertexPool*
	MLRLightMap::vertexPool;

DynamicArrayOf<Stuff::Vector4D>
	*transformedCoords, *clipExtraCoords;
DynamicArrayOf<RGBAColor>
	*clipExtraColors;
DynamicArrayOf<Vector2DScalar>
	*clipExtraTexCoords;
DynamicArrayOf<MLRClippingState>
	*clippingStates;
ClipPolygon2
	*MLRLightMap::clipBuffer;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRLightMap::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLRLightMapClassID,
			"MidLevelRenderer::MLRLightMap",
			RegisteredClass::DefaultData
		);
	Register_Object(DefaultData);

	unsigned char *ptr = new unsigned char [Limits::Max_Size_Of_LightMap_MemoryStream];
	Register_Pointer(ptr);

	stream = new MemoryStream(ptr, Limits::Max_Size_Of_LightMap_MemoryStream);
	Register_Object(stream);

	transformedCoords = new DynamicArrayOf<Stuff::Vector4D> (Limits::Max_Number_Vertices_Per_Polygon);
	Register_Object(transformedCoords);
	clipExtraCoords = new DynamicArrayOf<Stuff::Vector4D> (2*Limits::Max_Number_Vertices_Per_Polygon);
	Register_Object(clipExtraCoords);
	clipExtraColors = new DynamicArrayOf<RGBAColor> (2*Limits::Max_Number_Vertices_Per_Polygon);
	Register_Object(clipExtraColors);
	clipExtraTexCoords = new DynamicArrayOf<Vector2DScalar> (2*Limits::Max_Number_Vertices_Per_Polygon);
	Register_Object(clipExtraTexCoords);
	clippingStates = new DynamicArrayOf<MLRClippingState> (Limits::Max_Number_Vertices_Per_Polygon);
	Register_Object(clippingStates);

	clipBuffer = new ClipPolygon2 [2];
	Register_Pointer(clipBuffer);

	clipBuffer[0].Init(Limits::Max_Number_Of_Multitextures);
	clipBuffer[1].Init(Limits::Max_Number_Of_Multitextures);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRLightMap::TerminateClass()
{
	clipBuffer[1].Destroy();
	clipBuffer[0].Destroy();
	Unregister_Pointer(clipBuffer);
	delete [] clipBuffer;

	Unregister_Object(transformedCoords);
	delete transformedCoords;
	Unregister_Object(clipExtraCoords);
	delete clipExtraCoords;
	Unregister_Object(clipExtraColors);
	delete clipExtraColors;
	Unregister_Object(clipExtraTexCoords);
	delete clipExtraTexCoords;
	Unregister_Object(clippingStates);
	delete clippingStates;

	stream->Rewind();
	unsigned char *ptr = static_cast<unsigned char *>(stream->GetPointer());

	Unregister_Object(stream);
	delete stream;
	stream = nullptr;

	Unregister_Pointer(ptr);
	delete [] ptr;

	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRLightMap::MLRLightMap(MLRTexture *tex) :
	RegisteredClass(DefaultData)
{
	Verify(gos_GetCurrentHeap() == Heap);
	state.SetTextureHandle(tex->GetTextureHandle());
	state.SetRenderDeltaMask(MLRState::TextureMask);

//	state.SetFogData(0xffffffff, 0.0f, 1.0f, 100.0f);
#ifdef OLDFOG
	state.SetFogMode(MLRState::DisableFogMode);
#else
	state.SetFogMode(0);
#endif

	state.SetZBufferCompareOn();
	state.SetZBufferWriteOff();
	state.SetBackFaceOn();
	state.SetAlphaMode(MLRState::OneOneMode);

	state.SetFilterMode(MLRState::BiLinearFilterMode);
	state.SetTextureWrapMode(MLRState::TextureClamp);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRLightMap::~MLRLightMap()
{
	Check_Object(this);
	Check_Object(stream);

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRLightMap::TestInstance()
{
	Verify(IsDerivedFrom(DefaultData));
	Check_Object(stream);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
