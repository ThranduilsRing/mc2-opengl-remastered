//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLREFFECT_HPP

#if !defined(MLR_MLR_HPP)
#include<mlr/mlr.hpp>
#endif

namespace MidLevelRenderer {

	struct EffectClipPolygon
	{
		void Init();
		void Destroy();

		std::vector<Stuff::Vector4D> coords; //[Max_Number_Vertices_Per_Polygon];
		std::vector<Stuff::RGBAColor> colors; //[Max_Number_Vertices_Per_Polygon];
		std::vector<Vector2DScalar> texCoords; //[Max_Number_Vertices_Per_Polygon];
		std::vector<MLRClippingState> clipPerVertex; //[Max_Number_Vertices_Per_Polygon];
	};

	class DrawEffectInformation;

	//##########################################################################
	//#########################    MLREffect   #################################
	//##########################################################################

	class MLREffect :
		public Stuff::RegisteredClass
	{
	public:
		static void
			InitializeClass();
		static void
			TerminateClass();

		MLREffect(int, ClassData *class_data);
		~MLREffect();

		virtual void
			SetData(
				const int *count,
				const glm::vec3 *point_data,
				const Stuff::RGBAColor *color_data
			) = 0;

		virtual int
			GetType(int) { return 0; }

//	add another effect
		virtual void Draw (DrawEffectInformation*, GOSVertexPool*, MLRSorter*) = 0;

		virtual void Transform(int, int);

// switches single/all effects on or off
		void
			TurnAllOn();
		void
			TurnAllOff();

		void
			TurnOn(int nr)
				{ Check_Object(this); Verify(nr<maxNrOf); testList[nr] |= 2; }
		void
			TurnOff(int nr)
				{ Check_Object(this); Verify(nr<maxNrOf); testList[nr] &= ~2; }

		bool IsOn(int nr)
			{ Check_Object(this); Verify(nr<maxNrOf); return (testList[nr] & 2)? true : false; }

		virtual int	Clip(MLRClippingState, GOSVertexPool*) = 0;		

		void
			SetEffectToClipMatrix(
				const glm::mat4 *effectToWorld, 
				const glm::mat4 *worldToClipMatrix
			)
				{
					Check_Object(this);
					effectToClipMatrix = *effectToWorld * *worldToClipMatrix;
				}

		GOSVertex*
			GetGOSVertices()
				{ Check_Object(this); return gos_vertices; }

		int
			GetNumGOSVertices()
				{ Check_Object(this); return numGOSVertices; }

		int
			GetSortDataMode()
				{ Check_Object(this); return drawMode; }

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
			TestInstance() const
				{};

	protected:
		static std::unique_ptr<EffectClipPolygon[]> clipBuffer;

		void
			TurnAllVisible();
		void
			TurnAllInVisible();

		void
			TurnVisible(int nr)
				{ Check_Object(this); Verify(nr<maxNrOf); testList[nr] |= 1; }

		void
			TurnInVisible(int nr)
				{ Check_Object(this); Verify(nr<maxNrOf); testList[nr] &= ~1; }

		int visible;
		int maxNrOf;

		const glm::vec3 *points;

		const Stuff::RGBAColor *colors;

		static std::vector<Stuff::Vector4D> transformedCoords;

		std::vector<int>	testList;

		int drawMode;

		glm::mat4
			worldToEffect;

		glm::mat4
			effectToClipMatrix;

		GOSVertex *gos_vertices;
		int	numGOSVertices;
	};

	struct EffectClipData
	{
		Stuff::Vector4D *coords;
		Stuff::RGBAColor *colors;
		Vector2DScalar *texCoords;
		MLRClippingState *clipPerVertex;

		int flags;
		int length;
	};


}
