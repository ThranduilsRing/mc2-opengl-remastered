//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLRSORTBYORDER_HPP

#if !defined(MLR_MLR_HPP)
#include<mlr/mlr.hpp>
#endif

#if !defined(MLR_MLRSTATE_HPP)
#include<mlr/mlrstate.hpp>
#endif

namespace MidLevelRenderer {

	struct SortAlpha {
		Stuff::Scalar distance;
 		MLRState *state;
		GOSVertex triangle[3];
	};

	//##########################################################################
	//######################    MLRSortByOrder    ##############################
	//##########################################################################

	class MLRSortByOrder :
		public MLRSorter
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
		MLRSortByOrder(MLRTexturePool*);
		~MLRSortByOrder();

		void AddEffect(MLREffect*, const MLRState&);

		virtual void AddSortRawData(SortData*);

	//	starts the action
		void RenderNow ();

	//	resets the sorting
		void Reset ();

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Testing
	//
	public:
		void
			TestInstance() const;

	protected:
		int
			lastUsedInBucket[MLRState::PriorityCount];

		std::vector<SortData*>  //, Max_Number_Primitives_Per_Frame + Max_Number_ScreenQuads_Per_Frame
			priorityBuckets[MLRState::PriorityCount];

		std::vector<SortAlpha*>  //, Max_Number_Primitives_Per_Frame + Max_Number_ScreenQuads_Per_Frame
			alphaSort;
	};

}
