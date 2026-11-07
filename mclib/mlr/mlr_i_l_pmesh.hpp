//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLR_I_L_PMESH_HPP

#if !defined(MLR_MLR_HPP)
#include<mlr/mlr.hpp>
#endif

#if !defined(MLR_MLR_I_C_PMESH_HPP)
#include<mlr/mlr_i_c_pmesh.hpp>
#endif

namespace MidLevelRenderer {

	//##########################################################################
	//###### MLRIndexedPolyMesh with color and lighting one texture layer  #####
	//##########################################################################


	class MLR_I_L_PMesh:
		public MLR_I_C_PMesh
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
		MLR_I_L_PMesh(
			ClassData *class_data,
			Stuff::MemoryStream *stream,
			int version
		);
		~MLR_I_L_PMesh();

	public:
		MLR_I_L_PMesh(ClassData *class_data=MLR_I_L_PMesh::DefaultData);

		static MLR_I_L_PMesh*
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

//		void Copy(MLRIndexedPolyMesh*);


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
			size_t ret = MLR_I_C_PMesh::GetSize();
			ret += normals.size() * sizeof(normals[0]);
			ret += litColors.GetSize();

			return ret;
		}

	protected:
		std::vector<glm::vec3> normals;		// Base address of normal list 

		Stuff::DynamicArrayOf<Stuff::RGBAColor> litColors;
		Stuff::DynamicArrayOf<Stuff::RGBAColor> *actualColors;

	};

//	MLR_I_L_PMesh*
//		CreateIndexedCube(Stuff::Scalar, Stuff::RGBAColor*, Stuff::Vector3D*, MLRState*);
	MLRShape*
		CreateIndexedIcosahedron_Color_Lit(
			IcoInfo&,
			MLRState*
		);

}
