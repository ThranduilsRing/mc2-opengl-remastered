//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLR_I_C_TMESH_HPP

#include<mlr/mlr.hpp>
#include<mlr/mlr_i_tmesh.hpp>
#include<mlr/mlr_i_c_pmesh.hpp>

namespace MidLevelRenderer {

	//##########################################################################
	//#### MLRIndexedTriMesh with no color no lighting one texture layer  #####
	//##########################################################################


	class MLR_I_C_TMesh:
		public MLR_I_TMesh
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
		MLR_I_C_TMesh(
			ClassData *class_data,
			Stuff::MemoryStream *stream,
			int version
		);
		~MLR_I_C_TMesh();

	public:
		MLR_I_C_TMesh(ClassData *class_data=MLR_I_C_TMesh::DefaultData);

		static MLR_I_C_TMesh*
			Make(
				Stuff::MemoryStream *stream,
				int version
			);

		void
			Save(Stuff::MemoryStream *stream);

	public:
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



		bool
			Copy(MLR_I_C_PMesh*);

		void
			HurtMe(const glm::mat4& pain, Stuff::Scalar radius);

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// This functions using the static buffers
	//
		void
			SetClipColor(Stuff::RGBAColor &color, int index)
				{
					Check_Object(this); Verify(clipExtraColors.size() > index);
					clipExtraColors[index] = color;
				}

		void
			FlashClipColors(int num)
				{
					Check_Object(this); Verify(clipExtraTexCoords.size() > num);
					colors.SetLength(num);
					visibleIndexedVertices.SetLength(num);
					Mem_Copy(colors.GetData(), clipExtraColors.data(), sizeof(Stuff::RGBAColor)*num, sizeof(Stuff::RGBAColor)*num);
				}
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
			size_t ret = MLR_I_TMesh::GetSize();
			ret += colors.GetSize();

			return ret;
		}

	protected:
		Stuff::DynamicArrayOf<Stuff::RGBAColor> colors;	// Base address of color list 
		Stuff::DynamicArrayOf<Stuff::RGBAColor> *actualColors;
	};

	MLR_I_C_TMesh*
		CreateIndexedTriCube_Color_NoLit(Stuff::Scalar, MLRState*);
	MLRShape*
		CreateIndexedTriIcosahedron_Color_NoLit(
			IcoInfo&,
			MLRState*
		);

}
