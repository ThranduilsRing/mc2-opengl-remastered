//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLRTRIANGLECLOUD_HPP

#if !defined(MLR_MLR_HPP)
#include<mlr/mlr.hpp>
#endif

#if !defined(MLR_MLREFFECT_HPP)
#include<mlr/mlreffect.hpp>
#endif

namespace MidLevelRenderer {

	//##########################################################################
	//#####################    MLRTriangleCloud    #############################
	//##########################################################################


	class MLRTriangleCloud:
		public MLREffect
	{
	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Initialization
	//
	public:
		static void
			InitializeClass();
		static void
			TerminateClass();

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Constructors/Destructors
	//
	public:
		MLRTriangleCloud(int);
		~MLRTriangleCloud();

		void
			SetData(
				const int *count,
				const glm::vec3 *point_data,
				const Stuff::RGBAColor *color_data
			);

		void Draw (DrawEffectInformation*, GOSVertexPool*, MLRSorter*) = 0;

		int	Clip(MLRClippingState, GOSVertexPool*) = 0;

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
			TestInstance() const;

	protected:
		const int *usedNrOfTriangles;


		static std::vector<MLRClippingState> clipPerVertex; // , Max_Number_Vertices_Per_Mesh


	};
}
