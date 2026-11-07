//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLR_I_L_DT_TMESH_HPP

#include<mlr/mlr.hpp>
#include<mlr/mlr_i_c_tmesh.hpp>
#include<mlr/mlr_i_l_pmesh.hpp>

namespace MidLevelRenderer {

	//##########################################################################
	//#### MLRIndexedTriMesh with no color no lighting one texture layer  #####
	//##########################################################################


	class MLR_I_L_DT_TMesh:
		public MLR_I_C_DT_TMesh
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
		MLR_I_L_DT_TMesh(
			ClassData *class_data,
			Stuff::MemoryStream *stream,
			int version
		);
		~MLR_I_L_DT_TMesh();

	public:
		MLR_I_L_DT_TMesh(ClassData *class_data=MLR_I_L_DT_TMesh::DefaultData);

		static MLR_I_L_DT_TMesh*
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



		bool
			Copy(MLR_I_L_DT_PMesh*);

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
			size_t ret = MLR_I_C_DT_TMesh::GetSize();
			ret += normals.size() * sizeof(normals[0]);
			ret += litColors.size() * sizeof(litColors[0]);

			return ret;
		}

	protected:
		std::vector<glm::vec3> normals;		// Base address of normal list 

		std::vector<Stuff::RGBAColor> litColors;
	};

//	MLR_I_L_DT_TMesh*
//		CreateIndexedTriCube_NoColor_NoLit(Stuff::Scalar, MLRState*);
	MLRShape*
		CreateIndexedTriIcosahedron_Color_Lit_2Tex(
			IcoInfo&,
			MLRState*,
			MLRState*
		);

}
