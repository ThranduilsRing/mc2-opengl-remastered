//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"

//#############################################################################
//###############    MLRInfiniteLightWithFalloff    ###########################
//#############################################################################

MLRInfiniteLightWithFalloff::ClassData*
	MLRInfiniteLightWithFalloff::DefaultData = nullptr;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRInfiniteLightWithFalloff::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLRInfiniteLightWithFalloffClassID,
			"MidLevelRenderer::MLRInfiniteLightWithFalloff",
			MLRLight::DefaultData
		);
	Register_Object(DefaultData);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRInfiniteLightWithFalloff::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRInfiniteLightWithFalloff::MLRInfiniteLightWithFalloff(ClassData *class_data) :
	MLRLight(class_data)
{
	Verify(gos_GetCurrentHeap() == Heap);
	lightMask = MLRState::FaceLightingMode|MLRState::VertexLightingMode;
	innerRadius = 0.0f;
	outerRadius = 0.0f;

	oneOverDistance = 100.0f;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRInfiniteLightWithFalloff::MLRInfiniteLightWithFalloff(
	ClassData *class_data,
	Stuff::MemoryStream *stream,
	int version
) :
	MLRLight(class_data, stream, version)
{
	Check_Object(stream);
	Verify(gos_GetCurrentHeap() == Heap);
	lightMask = MLRState::FaceLightingMode|MLRState::VertexLightingMode;
	Scalar inner, outer;
	*stream >> inner >> outer;
	SetFalloffDistance (inner, outer);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRInfiniteLightWithFalloff::~MLRInfiniteLightWithFalloff()
{
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRInfiniteLightWithFalloff::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);

	MLRLight::Save(stream);
	*stream << innerRadius << outerRadius;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRInfiniteLightWithFalloff::TestInstance()
{
	Verify(IsDerivedFrom(DefaultData));
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// sebi or -> oradius, because apparently "or" is a regstered token :-/
void
	MLRInfiniteLightWithFalloff::SetFalloffDistance (Scalar ir, Scalar oradius)
{
	Check_Object(this);

	innerRadius = ir;
	outerRadius = oradius;

	oneOverDistance = 1.0f/(outerRadius - innerRadius);
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
bool
	MLRInfiniteLightWithFalloff::GetFalloffDistance (Scalar& ir, Scalar& oradius)
{
	Check_Object(this);

	ir = innerRadius;
	oradius = outerRadius;

	return true;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRInfiniteLightWithFalloff::LightVertex(const MLRVertexData& vertexData)
{
	glm::vec3 light_z;

	GetInShapeDirection(light_z);

	//
	//-------------------------------------------------------------------
	// Now we reduce the light level falling on the vertex based upon the
	// cosine of the angle between light and normal
	//-------------------------------------------------------------------
	//
	Scalar cosine = -(glm::dot(light_z, (*vertexData.normal))) * intensity;

	RGBColor light_color(color);

	glm::vec3 vertex_to_light;

	Verify(GetFalloffDistance(vertex_to_light.x, vertex_to_light.y));

	GetInShapePosition(vertex_to_light);

	vertex_to_light -= *vertexData.point;

	//
	//--------------------------------------------------------------
	// If the distance to the vertex is zero, the light will not
	// contribute to the vertex coloration.  Otherwise, decrease the
	// light level as appropriate to the distance
	//--------------------------------------------------------------
	//
	Scalar length = glm::length(vertex_to_light);

	Scalar falloff = 1.0f;

	if(GetFalloff(length, falloff))
	{
		light_color.red *= falloff;
		light_color.green *= falloff;
		light_color.blue *= falloff;
	}
	else
	{
		return;
	}

	if (cosine > SMALL)
	{
		light_color.red *= cosine;
		light_color.green *= cosine;
		light_color.blue *= cosine;
		vertexData.color->red += light_color.red;
		vertexData.color->green += light_color.green;
		vertexData.color->blue += light_color.blue;
	}
}
