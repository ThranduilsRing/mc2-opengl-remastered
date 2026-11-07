//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLRINDEXEDPRIMITIVEBASE_HPP

#if !defined(MLR_MLR_HPP)
#include<mlr/mlr.hpp>
#endif

namespace MidLevelRenderer {

	//##########################################################################
	//###################    MLRIndexedPrimitiveBase    ########################
	//##########################################################################

	class MLRIndexedPrimitiveBase:
		public MLRPrimitiveBase
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
	protected:
		MLRIndexedPrimitiveBase(
			ClassData *class_data,
			Stuff::MemoryStream *stream,
			int version
		);
		~MLRIndexedPrimitiveBase();

	public:
		MLRIndexedPrimitiveBase(ClassData *class_data);

		static MLRIndexedPrimitiveBase*
			Make(
				Stuff::MemoryStream *stream,
				int version
			);

		virtual void
			Save(Stuff::MemoryStream *stream);

		virtual	void	InitializeDrawPrimitive(unsigned char, int=0);


		virtual void
			SetCoordData(
				const glm::vec3 *array,
				int point_count
			);

		virtual void
			SetIndexData(
				unsigned short *index_array,
				int index_count
			);

		virtual void
			GetIndexData(
				unsigned short **index_array,
				int *index_count
			);

		virtual unsigned short*
			GetGOSIndices(int=0)
				{ Check_Object(this); return gos_indices; }

		int
			GetNumGOSIndices()
				{ Check_Object(this); return numGOSIndices; }

		virtual void
			Transform(glm::mat4*);


		void
			TheIndexer(int num)
				{
					Check_Object(this);
					index.resize(num);
					for(unsigned short i=0;i<num;i++)
					{
						index[i] = i;
					}
				}

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
			size_t ret = MLRPrimitiveBase::GetSize();
			ret += visibleIndexedVertices.size() * sizeof(unsigned char);
			ret += index.size() * sizeof(index[0]);

			return ret;
		}

		bool
			CheckIndicies();

	protected:
		bool visibleIndexedVerticesKey;

		std::vector<unsigned char>	visibleIndexedVertices;

		std::vector<unsigned short>	index;	// List of color indexes 

		static std::vector<unsigned short> clipExtraIndex; // , Max_Number_Vertices_Per_Mesh

		unsigned short *gos_indices;
		unsigned short	numGOSIndices;
	};

}
