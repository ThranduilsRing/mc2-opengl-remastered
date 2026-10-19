//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLRLIGHT_HPP

#include"mlr.hpp"

namespace MidLevelRenderer {

	class GOSVertexPool;
	class MLRLightMap;

	struct MLRVertexData {
		glm::vec3 *point;
#if COLOR_AS_DWORD
		DWORD *color;
#else
		Stuff::RGBAColor *color;
#endif
		glm::vec3 *normal;

		int index;
	};

	//##########################################################################
	//#########################    MLRLight    #################################
	//##########################################################################

	class MLRLight:
		public Stuff::RegisteredClass
	{
	public:
		static void
			InitializeClass();
		static void
			TerminateClass();

		MLRLight(ClassData *class_data);
		MLRLight(
			ClassData *class_data,
			Stuff::MemoryStream *stream,
			int version
		);
		MLRLight(
			ClassData *class_data,
			Stuff::Page* page
		);
		~MLRLight();

		static MLRLight*
			Make(
				Stuff::MemoryStream *stream,
				int version
			);
		static MLRLight*
			Make(Stuff::Page *page);

		virtual void
			Save(Stuff::MemoryStream *stream);
		virtual void
			Write(Stuff::Page *page);

		enum LightType {
			AmbientLight = 0,
			InfiniteLight,
			InfiniteLightWithFallOff,
			PointLight,
			SpotLight,
			LookUpLight
		};

		virtual LightType
			GetLightType() = 0;

		virtual void
			LightVertex(const MLRVertexData&) = 0;

		void SetIntensity (Stuff::Scalar _int)
			{ Check_Object(this); intensity = _int; };
		Stuff::Scalar GetIntensity ()
			{ Check_Object(this); return intensity; };

		void SetColor(Stuff::RGBColor col)
			{ Check_Object(this); color = col; }
		void SetColor(Stuff::Scalar, Stuff::Scalar, Stuff::Scalar);

		void GetColor(Stuff::RGBColor& col)
			{ Check_Object(this); col = color; };
		void GetColor(Stuff::Scalar&, Stuff::Scalar&, Stuff::Scalar&);

		void GetInWorldPosition(glm::vec3& pos)
			{ Check_Object(this); pos = mc2::math::getTranslation(lightToWorld); };

		void GetInWorldDirection(glm::vec3& dir)
			{ Check_Object(this); dir = mc2::math::localForwardInWorld(lightToWorld); };

		void GetInShapePosition(glm::vec3& pos)
			{ Check_Object(this); pos = mc2::math::getTranslation(lightToShape); };

		void GetInShapePosition(glm::mat4& pos)
			{ Check_Object(this); pos = lightToShape; };

		void GetInShapeDirection(glm::vec3& dir)
			{ Check_Object(this); dir = mc2::math::localForwardInWorld(lightToShape); };

		void SetLightToWorldMatrix(const glm::mat4&);
		const glm::mat4&
			GetLightToWorldMatrix()
				{ Check_Object(this); return lightToWorld; };

		virtual
			void SetLightToShapeMatrix(const glm::mat4&);
		const glm::mat4&
			GetLightToShapeMatrix()
				{ Check_Object(this); return lightToShape; };

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Light Map Support
	//
		virtual MLRLightMap *
			GetLightMap()
				{Check_Object(this); return nullptr; }

		int
			GetLightMask()
				{Check_Object(this); return lightMask;}
		void
			SetDynamicLight()
				{Check_Object(this); lightMask |= MLRState::TerrainLightingMode;}
		void
			SetStaticLight()
				{Check_Object(this); lightMask &= ~MLRState::TerrainLightingMode;}

		void
			SetName(const char *name)
				{Check_Object(this); lightName = name;}

		const char*
			GetName()
				{Check_Object(this); return lightName;}

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Class Data Support
	//
	public:
		static ClassData
			*DefaultData;

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Testing
	//
	public:
		void
			TestInstance();

	protected:
		Stuff::Scalar
			intensity;

		Stuff::RGBColor
			color;

		glm::mat4
			lightToWorld, lightToShape;

		int
			lightMask;

		Stuff::MString
			lightName;
	};

}
