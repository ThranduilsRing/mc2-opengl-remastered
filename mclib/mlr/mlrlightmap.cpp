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

std::vector<Stuff::Vector4D>
	transformedCoords, clipExtraCoords;
std::vector<RGBAColor>
	clipExtraColors;
std::vector<Vector2DScalar>
	clipExtraTexCoords;
std::vector<MLRClippingState>
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

	transformedCoords.resize(Limits::Max_Number_Vertices_Per_Polygon);
	clipExtraCoords.resize(2*Limits::Max_Number_Vertices_Per_Polygon);
	clipExtraColors.resize(2*Limits::Max_Number_Vertices_Per_Polygon);
	clipExtraTexCoords.resize(2*Limits::Max_Number_Vertices_Per_Polygon);
	clippingStates.resize(Limits::Max_Number_Vertices_Per_Polygon);

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

	std::vector<Stuff::Vector4D>().swap(transformedCoords);
	std::vector<Stuff::Vector4D>().swap(clipExtraCoords);
	std::vector<RGBAColor>().swap(clipExtraColors);
	std::vector<Vector2DScalar>().swap(clipExtraTexCoords);
	std::vector<MLRClippingState>().swap(clippingStates);

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
