//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLR_I_L_DeT_PMESH_HPP

#if !defined(MLR_MLR_HPP)
#include<mlr/mlr.hpp>
#endif

#if !defined(MLR_MLR_I_C_DeT_PMESH_HPP)
#include<mlr/mlr_i_c_det_pmesh.hpp>
#endif

namespace MidLevelRenderer {

	//##########################################################################
	//###### MLRIndexedPolyMesh with color and lighting two texture layer  #####
	//##########################################################################


	class MLR_I_L_DeT_PMesh:
		public MLR_I_C_DeT_PMesh
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
		MLR_I_L_DeT_PMesh(
			ClassData *class_data,
			Stuff::MemoryStream *stream,
			int version
		);
		~MLR_I_L_DeT_PMesh();

	public:
		MLR_I_L_DeT_PMesh(ClassData *class_data=MLR_I_L_DeT_PMesh::DefaultData);

		static MLR_I_L_DeT_PMesh*
			Make(
				Stuff::MemoryStream *stream,
				int version
			);

		void
			Save(Stuff::MemoryStream *stream);

	public:
		virtual void
			SetNormalData(
				const glm::vec3 *array,
				int point_count
			);
		virtual void
			GetNormalData(
				glm::vec3 **array,
				int *point_count
			);

		virtual void
			SetColorData(
				const Stuff::RGBAColor *array,
				int point_count
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
			size_t ret = MLR_I_C_DeT_PMesh::GetSize();
			ret += normals.size() * sizeof(normals[0]);
			ret += litColors.size() * sizeof(litColors[0]);

			return ret;
		}

	protected:
		std::vector<glm::vec3> normals;		// Base address of normal list 

		std::vector<Stuff::RGBAColor> litColors;
		std::vector<Stuff::RGBAColor> *actualColors;

	};

//	MLR_I_L_DeT_PMesh*
//		CreateIndexedCube(Stuff::Scalar, Stuff::RGBAColor*, Stuff::Vector3D*, MLRState*);
	MLRShape*
		CreateIndexedIcosahedron_Color_Lit_DetTex(
			IcoInfo&,
			MLRState*,
			MLRState*
		);
}
