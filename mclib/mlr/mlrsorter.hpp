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

		int LoadAlphaFromTriList(SortAlpha**);
		int LoadAlphaFromTriIndexedList(SortAlpha**);

		static constexpr int TriList = 0;
		static constexpr int TriIndexedList = 1;
		static constexpr int LastMode = 2;

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

		virtual void AddEffect(MLREffect*, const MLRState&) = 0;

		virtual void AddSortRawData(SortData*) = 0;


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


//	Just scaling down a bit to keep z under 1.0f	
		void
			SetFarClipReciprocal(Stuff::Scalar fcr)
				{ Check_Object(this); farClipReciprocal = fcr*(1.0f-Stuff::SMALL); }


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



		Stuff::Scalar
			farClipReciprocal;
	};

}
