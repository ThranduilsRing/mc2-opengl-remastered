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







		void
			SetState(MLRState new_state)
				{ Check_Object(this); state = new_state; }
		MLRState
			GetState()
				{ Check_Object(this); return state; }

		inline void
			SetPolygonMarker (int type)
				{ Check_Object(this); Check_Object(stream.get()); *stream << (type ? (static_cast<int>((static_cast<MemoryStreamData>(PolygonWithColor)))) : (static_cast<int>((static_cast<MemoryStreamData>(Polygon)))) ); }

		inline void
			AddColor(Stuff::RGBAColor color)
				{ Check_Object(this); Check_Object(stream.get()); *stream << color; }

		inline void
			AddColor(Stuff::Scalar red, Stuff::Scalar green, Stuff::Scalar blue, Stuff::Scalar alpha)
				{ Check_Object(this); Check_Object(stream.get()); *stream << red << green << blue << alpha; }

		inline void
			AddCoord(glm::vec3 coord)
				{ Check_Object(this); Check_Object(stream.get()); *stream << coord; }

		inline void
			AddUVs(Stuff::Scalar u, Stuff::Scalar v)
				{ Check_Object(this); Check_Object(stream.get()); *stream << u << v; }

		inline void
			AddInt(int i)
				{ Check_Object(this); Check_Object(stream.get()); *stream << i; }

		inline void
			AddUShort(unsigned short i)
				{ Check_Object(this); Check_Object(stream.get()); *stream << i; }

		inline void
			AddState(int priority)
				{
					Check_Object(this);
					Check_Object(stream.get());  
					*stream << static_cast<int>(LightMapRenderState);
					state.SetPriority(priority);
					state.Save(stream.get());
				}

		const Vector2DScalar*
			GetCurrentUVPointer()
				{ Check_Object(this); Check_Object(stream.get()); return Cast_Pointer(Vector2DScalar*, stream->GetPointer()); }

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
		static std::unique_ptr<ClipPolygon2[]>
			clipBuffer;

		MLRState
			state;

		static std::unique_ptr<Stuff::MemoryStream>
			stream;
		static GOSVertexPool*
			vertexPool;
	};

}
