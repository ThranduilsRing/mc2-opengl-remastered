//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLRLIGHTMAP_HPP

#include"mlr.hpp"

namespace MidLevelRenderer {

	//##########################################################################
	//#########################    MLRLightMap    ##############################
	//##########################################################################

	class MLRLightMap:
		public Stuff::RegisteredClass
	{
	public:
		static void
			InitializeClass();
		static void
			TerminateClass();

		MLRLightMap(MLRTexture*);
		~MLRLightMap();

		enum MemoryStreamData {
			Matrix4D=0,
			ClippingState,
			MasterRenderState,
			LightMapRenderState,
			Polygon,
			PolygonWithColor
		};

		static void
			DrawLightMaps(MLRSorter*);

		static void
			 SetDrawData 
				(
					GOSVertexPool*,
					glm::mat4*,
					MLRClippingState&,
					MLRState&
				);

		static MLRShape*
			CreateLightMapShape();

		void
			SetState(MLRState new_state)
				{ Check_Object(this); state = new_state; }
		MLRState
			GetState()
				{ Check_Object(this); return state; }

		inline void
			SetPolygonMarker (int type)
				{ Check_Object(this); Check_Object(stream); *stream << (type ? (static_cast<int>((static_cast<MemoryStreamData>(PolygonWithColor)))) : (static_cast<int>((static_cast<MemoryStreamData>(Polygon)))) ); }

		inline void
			AddColor(Stuff::RGBAColor color)
				{ Check_Object(this); Check_Object(stream); *stream << color; }

		inline void
			AddColor(Stuff::Scalar red, Stuff::Scalar green, Stuff::Scalar blue, Stuff::Scalar alpha)
				{ Check_Object(this); Check_Object(stream); *stream << red << green << blue << alpha; }

		inline void
			AddCoord(glm::vec3 coord)
				{ Check_Object(this); Check_Object(stream); *stream << coord; }

		inline void
			AddUVs(Stuff::Scalar u, Stuff::Scalar v)
				{ Check_Object(this); Check_Object(stream); *stream << u << v; }

		inline void
			AddInt(int i)
				{ Check_Object(this); Check_Object(stream); *stream << i; }

		inline void
			AddUShort(unsigned short i)
				{ Check_Object(this); Check_Object(stream); *stream << i; }

		inline void
			AddState(int priority)
				{
					Check_Object(this);
					Check_Object(stream);  
					*stream << static_cast<int>(LightMapRenderState);
					state.SetPriority(priority);
					state.Save(stream);
				}

		const Vector2DScalar*
			GetCurrentUVPointer()
				{ Check_Object(this); Check_Object(stream); return Cast_Pointer(Vector2DScalar*, stream->GetPointer()); }

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
		// DrawLightMaps phases (mlrlightmap.cpp).
		static void flushForLightMapState(MLRSorter* sorter, GOSVertex*& gos_vertices, int& numGOSVertices, MLRState& currentState, MLRState& masterState);
		static void polygonFastPath(int& i, unsigned short& stride, DWORD& argb, Vector2DScalar*& texCoords, GOSVertex*& gos_vertices, int& numGOSVertices);
		static void polygonClipOnePlane(unsigned short& stride, Vector2DScalar*& texCoords, int& k, int& k0, int& k1, int& l, int& mask, int& ct, Stuff::Scalar& a, int& numberVerticesPerPolygon, bool& firstIsIn, MLRClippingState& theTest);
		static void polygonClipManyPlanes(unsigned short& stride, glm::vec3*& coords, Vector2DScalar*& texCoords, MLRClippingState& theOr, int& k, int& k1, int& l, int& mask, Stuff::Scalar& a, int& numberVerticesPerPolygon, bool& firstIsIn, MLRClippingState& theTest);
		static void polygonTriangulateClipped(int& i, DWORD& argb, GOSVertex*& gos_vertices, int& numGOSVertices, int& numberVerticesPerPolygon);
		static void colorPolygonFastPath(int& i, unsigned short& stride, Stuff::RGBAColor*& colors, Vector2DScalar*& texCoords, GOSVertex*& gos_vertices, int& numGOSVertices);
		static void colorPolygonClipOnePlane(unsigned short& stride, Stuff::RGBAColor*& colors, Vector2DScalar*& texCoords, int& k, int& k0, int& k1, int& l, int& mask, int& ct, Stuff::Scalar& a, int& numberVerticesPerPolygon, bool& firstIsIn, MLRClippingState& theTest);
		static void colorPolygonClipManyPlanes(unsigned short& stride, glm::vec3*& coords, Stuff::RGBAColor*& colors, Vector2DScalar*& texCoords, MLRClippingState& theOr, int& k, int& k1, int& l, int& mask, Stuff::Scalar& a, int& numberVerticesPerPolygon, bool& firstIsIn, MLRClippingState& theTest);
		static void colorPolygonTriangulateClipped(int& i, GOSVertex*& gos_vertices, int& numGOSVertices, int& numberVerticesPerPolygon);
		static ClipPolygon2
			*clipBuffer;

		MLRState
			state;

		static Stuff::MemoryStream
			*stream;
		static GOSVertexPool*
			vertexPool;
	};

}
