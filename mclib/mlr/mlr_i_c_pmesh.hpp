//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLR_I_C_PMESH_HPP

#if !defined(MLR_MLR_HPP)
#include<mlr/mlr.hpp>
#endif

#if !defined(MLR_MLR_I_PMESH_HPP)
#include<mlr/mlr_i_pmesh.hpp>
#endif

namespace MidLevelRenderer {

	//##########################################################################
	//#### MLRIndexedPolyMesh with color but no lighting one texture layer  ####
	//##########################################################################


	class MLR_I_C_PMesh:
		public MLR_I_PMesh
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
	protected:
		MLR_I_C_PMesh(
			ClassData *class_data,
			Stuff::MemoryStream *stream,
			int version
		);
		~MLR_I_C_PMesh();

	public:
		MLR_I_C_PMesh(ClassData *class_data=MLR_I_C_PMesh::DefaultData);

		static MLR_I_C_PMesh*
			Make(
				Stuff::MemoryStream *stream,
				int version
			);

		void
			Save(Stuff::MemoryStream *stream);

	public:
//		void Copy(MLRIndexedPolyMesh*);

		virtual void
			SetColorData(
				const Stuff::RGBAColor *array,
				int point_count
			);
		virtual void
			GetColorData(
				Stuff::RGBAColor **array,
				int *point_count
			);

		virtual void
			PaintMe(const Stuff::RGBAColor *paintMe);



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

	virtual size_t
			GetSize()
		{ 
			Check_Object(this);
			size_t ret = MLR_I_PMesh::GetSize();
			ret += colors.size() * sizeof(colors[0]);

			return ret;
		}

	protected:
		std::vector<Stuff::RGBAColor> colors;	// Base address of color list 
	};

	MLR_I_C_PMesh*
		CreateIndexedCube_Color_NoLit(Stuff::Scalar, Stuff::RGBAColor*, MLRState*);

	MLR_I_PMesh*
		CreateIndexedViewFrustrum_Color_NoLit(
		Stuff::Scalar near_clip,
		Stuff::Scalar far_clip,
		Stuff::Scalar left_clip,
		Stuff::Scalar right_clip,
		Stuff::Scalar top_clip,
		Stuff::Scalar bottom_clip,
		Stuff::RGBAColor&, 
		MLRState*
	);

	MLRShape*
		CreateIndexedIcosahedron_Color_NoLit(
			IcoInfo&,
			MLRState*
		);
}
