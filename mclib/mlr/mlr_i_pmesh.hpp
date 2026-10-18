//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLR_I_PMESH_HPP

#if !defined(MLR_MLR_HPP)
#include<mlr/mlr.hpp>
#endif
#if !defined(MLR_MLRINDEXEDPRIMITIVEBASE_HPP)
#include<mlr/mlrindexedprimitivebase.hpp>
#endif

namespace MidLevelRenderer {

	//##########################################################################
	//#### MLRIndexedPolyMesh with no color no lighting one texture layer  #####
	//##########################################################################


	class MLR_I_PMesh:
		public MLRIndexedPrimitiveBase
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
		MLR_I_PMesh(
			ClassData *class_data,
			Stuff::MemoryStream *stream,
			int version
		);
		~MLR_I_PMesh();

	public:
		MLR_I_PMesh(ClassData *class_data=MLR_I_PMesh::DefaultData);

		static MLR_I_PMesh*
			Make(
				Stuff::MemoryStream *stream,
				int version
			);

		void
			Save(Stuff::MemoryStream *stream);

	public:
//		void Copy(MLRIndexedPolyMesh*);

		virtual	void	InitializeDrawPrimitive(unsigned char, int=0);

		virtual void	SetSubprimitiveLengths(unsigned char *, int);
		virtual void	GetSubprimitiveLengths(unsigned char **, int*);

		void	FindFacePlanes();

		virtual int	FindBackFace(const glm::vec3&);

		const mc2::math::Plane *GetPolygonPlane(int i)
			{
				Check_Object(this);
				Verify(i<facePlanes.GetLength());

				return &facePlanes[i];
			}

		virtual void	Lighting(MLRLight* const*, int nrLights);

		virtual void LightMapLighting(MLRLight*);

		virtual void
#if COLOR_AS_DWORD
			PaintMe(const DWORD *paintMe) {};
#else
			PaintMe(const Stuff::RGBAColor *paintMe) {};
#endif

		virtual int	TransformAndClip(glm::mat4*, MLRClippingState, GOSVertexPool*,bool=false);

		virtual void
			TransformNoClip(glm::mat4*, GOSVertexPool*,bool=false);

	//	Initializes the visibility test list
		void
			ResetTestList();

	//	find which vertices are visible which not - returns nr of visible vertices
	//	the result is stored in the visibleIndexedVertices array
		int
			FindVisibleVertices();

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
			size_t ret = MLRIndexedPrimitiveBase::GetSize();
			ret += testList.GetSize();
			ret += facePlanes.GetSize();

			return ret;
		}

	protected:
		void
			Transform(glm::mat4*);

		Stuff::DynamicArrayOf<unsigned char>	testList;
		
		Stuff::DynamicArrayOf<mc2::math::Plane> facePlanes;
		
	};

	#define ICO_X 0.525731112119133606f
	#define ICO_Z 0.850650808352039932f

	extern float vdata[12][3];
	extern unsigned int tindices [20][3];

	extern long triDrawn;

	void
		subdivide (
			glm::vec3 *coords,
			glm::vec3& v1,
			glm::vec3& v2,
			glm::vec3& v3,
			long depth,
			long tri2draw, 
			float rad = 1.0f
		);

	MLR_I_PMesh*
		CreateIndexedCube_NoColor_NoLit(Stuff::Scalar, MLRState*);
	MLRShape*
		CreateIndexedIcosahedron_NoColor_NoLit(
			IcoInfo&,
			MLRState*
		);

}
