//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"

//#############################################################################
//###########################    MLRLight    ##################################
//#############################################################################

MLRLight::ClassData*
	MLRLight::DefaultData = nullptr;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRLight::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLRLightClassID,
			"MidLevelRenderer::MLRLight",
			RegisteredClass::DefaultData
		);
	Register_Object(DefaultData);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRLight::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRLight::MLRLight(ClassData *class_data) :
	RegisteredClass(class_data)
{
	Verify(gos_GetCurrentHeap() == Heap);
	intensity = 1.0f;

	lightToWorld = glm::mat4(1.0f);
	lightToShape = glm::mat4(1.0f);

	color = RGBColor(0.0f, 0.0f, 0.0f);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRLight::MLRLight(
	ClassData *class_data,
	Stuff::MemoryStream *stream,
	int version
) :
	RegisteredClass(class_data)
{
	Check_Object(stream);
	Verify(gos_GetCurrentHeap() == Heap);

	glm::mat4 matrix;
	*stream >> intensity >> color >> matrix;

	if(version>=9)
	{
		*stream >> lightName;
	}
	else
	{
		lightName = "Light";
	}

	SetLightToWorldMatrix(matrix);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRLight::~MLRLight()
{
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRLight*
	MLRLight::Make(
		Stuff::MemoryStream *stream,
		int version
	)
{
	gos_PushCurrentHeap(Heap);
	int type;
	MLRLight *light = nullptr;
	*stream >> type;
	switch (type)
	{
	case AmbientLight:
		light = new MLRAmbientLight(stream, version);
		break;
	case InfiniteLight:
		light = new MLRInfiniteLight(MLRInfiniteLight::DefaultData, stream, version);
		break;
	case PointLight:
		light = new MLRPointLight(stream, version);
		break;
	case SpotLight:
		light = new MLRSpotLight(stream, version);
		break;
	case LookUpLight:
		light = new MLRLookUpLight(stream, version);
		break;
	default:
		STOP(("Bad light type"));
	}
	gos_PopCurrentHeap();
	return light;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRLight::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);

	*stream << static_cast<int>(GetLightType());
	*stream << intensity << color << lightToWorld;
	*stream << lightName;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRLight::TestInstance()
{
	Verify(IsDerivedFrom(DefaultData));
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRLight::SetLightToShapeMatrix(const glm::mat4& worldToShape)
{
	Check_Object(this);

	lightToShape = lightToWorld * worldToShape;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRLight::SetLightToWorldMatrix(const glm::mat4& matrix)
{
	Check_Object(this);

	lightToWorld = matrix;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRLight::SetColor (Scalar r, Scalar g, Scalar b)
{
	SetColor(RGBColor(r, b, b));
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRLight::GetColor (Scalar& r, Scalar& g, Scalar& b)
{
	Check_Object(this);

	r = color.red;
	g = color.green;
	b = color.blue;
}
