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
std::unique_ptr<Stuff::MemoryStream>
	MLRLightMap::stream;
static std::unique_ptr<unsigned char[]> s_streamBuffer;
GOSVertexPool*
	MLRLightMap::vertexPool;

std::unique_ptr<DynamicArrayOf<Stuff::Vector4D>>
	transformedCoords, clipExtraCoords;
std::unique_ptr<DynamicArrayOf<RGBAColor>>
	clipExtraColors;
std::unique_ptr<DynamicArrayOf<Vector2DScalar>>
	clipExtraTexCoords;
std::unique_ptr<DynamicArrayOf<MLRClippingState>>
	clippingStates;
std::unique_ptr<ClipPolygon2[]>
	MLRLightMap::clipBuffer;

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

	s_streamBuffer = std::make_unique_for_overwrite<unsigned char[]>(Limits::Max_Size_Of_LightMap_MemoryStream);
	Register_Pointer(s_streamBuffer.get());

	stream = std::make_unique<MemoryStream>(s_streamBuffer.get(), Limits::Max_Size_Of_LightMap_MemoryStream);
	Register_Object(stream.get());

	transformedCoords = std::make_unique<DynamicArrayOf<Stuff::Vector4D>>(Limits::Max_Number_Vertices_Per_Polygon);
	Register_Object(transformedCoords.get());
	clipExtraCoords = std::make_unique<DynamicArrayOf<Stuff::Vector4D>>(2*Limits::Max_Number_Vertices_Per_Polygon);
	Register_Object(clipExtraCoords.get());
	clipExtraColors = std::make_unique<DynamicArrayOf<RGBAColor>>(2*Limits::Max_Number_Vertices_Per_Polygon);
	Register_Object(clipExtraColors.get());
	clipExtraTexCoords = std::make_unique<DynamicArrayOf<Vector2DScalar>>(2*Limits::Max_Number_Vertices_Per_Polygon);
	Register_Object(clipExtraTexCoords.get());
	clippingStates = std::make_unique<DynamicArrayOf<MLRClippingState>>(Limits::Max_Number_Vertices_Per_Polygon);
	Register_Object(clippingStates.get());

	clipBuffer = std::make_unique<ClipPolygon2[]>(2);
	Register_Pointer(clipBuffer.get());

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
	Unregister_Pointer(clipBuffer.get());
	clipBuffer.reset();

	Unregister_Object(transformedCoords.get());
	transformedCoords.reset();
	Unregister_Object(clipExtraCoords.get());
	clipExtraCoords.reset();
	Unregister_Object(clipExtraColors.get());
	clipExtraColors.reset();
	Unregister_Object(clipExtraTexCoords.get());
	clipExtraTexCoords.reset();
	Unregister_Object(clippingStates.get());
	clippingStates.reset();

	Unregister_Object(stream.get());
	stream.reset();

	Unregister_Pointer(s_streamBuffer.get());
	s_streamBuffer.reset();

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
	Check_Object(stream.get());

}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRLightMap::TestInstance()
{
	Verify(IsDerivedFrom(DefaultData));
	Check_Object(stream.get());
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
