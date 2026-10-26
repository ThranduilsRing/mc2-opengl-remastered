//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLRSORTER_HPP

#include<mlr/mlr.hpp>
#include<mlr/mlrlight.hpp>
#include<mlr/gosvertex.hpp>
#include<mlr/gosvertex2uv.hpp>
#include<mlr/gosvertexpool.hpp>

namespace MidLevelRenderer {

	class MLRPrimitive;
	class MLREffect;
	class GOSVertex;

	struct SortAlpha;

	class SortData {
	public:
		SortData () 
			{ vertices = nullptr; numVertices = 0; indices = nullptr;
			  numIndices = 0; type = TriList; texture2 = 0; }

		void DrawTriList();
		void DrawTriIndexedList();
		void DrawPointCloud();

		int LoadAlphaFromTriList(SortAlpha**);
		int LoadAlphaFromTriIndexedList(SortAlpha**);
		int LoadAlphaFromPointCloud(SortAlpha**);

		static constexpr int TriList = 0;
		static constexpr int TriIndexedList = 1;
		static constexpr int PointCloud = 2;
		static constexpr int LastMode = 3;

		using DrawFunc = void (SortData::*)();
		using LoadSortAlphaFunc = int (SortData::*)(SortAlpha**);

		static DrawFunc Draw[LastMode];
		static LoadSortAlphaFunc LoadSortAlpha[LastMode];

		MLRState state;
		void *vertices;
		int	numVertices;
		unsigned short *indices;
		int numIndices;
		int type;
		int texture2;
	};


	class MLRPrimitiveBase;


#define CalDraw	


#ifdef CalDraw
	struct ToBeDrawnPrimitive {
		ToBeDrawnPrimitive();

		MLRPrimitiveBase *primitive;

		MLRState state;
		glm::vec3 cameraPosition;
		MLRClippingState clippingFlags;
		glm::mat4 shapeToClipMatrix;
		glm::mat4 worldToShape;

		static GOSVertexPool *allVerticesToDraw;

		MLRLight* activeLights[Limits::Max_Number_Of_Lights_Per_Primitive];

		int	nrOfActiveLights;
	};
#endif

	//##########################################################################
	//#########################    MLRSorter    ################################
	//##########################################################################

	class _declspec(novtable) MLRSorter :
		public Stuff::RegisteredClass
	{
	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Initialization
	//
	public:
		static void
			InitializeClass();
		static void
			TerminateClass();
		static ClassData
			*DefaultData;

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Constructors/Destructors
	//
	public:
		MLRSorter(ClassData *class_data, MLRTexturePool*);
		~MLRSorter();

		virtual void AddPrimitive(MLRPrimitiveBase*, int=0) = 0;
		virtual void AddEffect(MLREffect*, const MLRState&) = 0;

		virtual void AddSortRawData(SortData*) = 0;

		virtual void
			DrawPrimitive(MLRPrimitiveBase*, int=0);

		void
			SetTexturePool(MLRTexturePool *tp)
				{ Check_Object(this); Check_Object(tp); texturePool = tp; }

		bool
			SetDifferences(const MLRState& original, const MLRState& newer);

	//	starts the action
		virtual void RenderNow () = 0;

	//	resets the sorting
		virtual void Reset ();

	//	lets begin the dance
		virtual void StartDraw(const MLRState &default_state);

	//	enter raw data
		SortData*
			SetRawData
				(	void *vertices, 
					int numVertices,
					const MLRState& state,
					const int& mode,
					int tex2 = 0
				);

		SortData*
			SetRawIndexedData
				(	void* vertices, 
					int numVertices, 
					unsigned short *indices,
					int numIndices,
					const MLRState& state, 
					const int& mode,
					int tex2 = 0
				);

		SortData*
			SetRawData(MLRPrimitiveBase*, int=0 );

//	Just scaling down a bit to keep z under 1.0f	
		void
			SetFarClipReciprocal(Stuff::Scalar fcr)
				{ Check_Object(this); farClipReciprocal = fcr*(1.0f-Stuff::SMALL); }

#ifdef CalDraw
		ToBeDrawnPrimitive*
			GetCurrentTBDP()
				{ Check_Object(this); return &drawData[lastUsedDraw]; }
		ToBeDrawnPrimitive*
			GetCurrentTBDP(int index)
				{ Check_Object(this); Verify(index<lastUsedDraw); return &drawData[index]; }
		void
			IncreaseTBDPCounter();
#endif

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Testing
	//
	public:
		void
			TestInstance() const;

	protected:
		MLRState theCurrentState;
		MLRTexturePool *texturePool;

		int lastUsedRaw;
		Stuff::DynamicArrayOf<SortData> rawDrawData; // Max_Number_Primitives_Per_Frame

		int
			lastUsedInBucketNotDrawn[MLRState::PriorityCount];

#ifdef CalDraw
		int lastUsedDraw;
		Stuff::DynamicArrayOf<ToBeDrawnPrimitive> drawData; // Max_Number_Primitives_Per_Frame
		Stuff::DynamicArrayOf<ToBeDrawnPrimitive*>  //, Max_Number_Primitives_Per_Frame + Max_Number_ScreenQuads_Per_Frame
			priorityBucketsNotDrawn[MLRState::PriorityCount];
#endif

		Stuff::Scalar
			farClipReciprocal;
	};

}
