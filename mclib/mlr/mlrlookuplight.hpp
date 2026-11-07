//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once
#define MLR_MLRLOOKUPLIGHT_HPP

#include"mlr.hpp"
#include"mlrinfinitelight.hpp"

namespace MidLevelRenderer {

	//##########################################################################
	//########################    MLRLookUpLight    ############################
	//##########################################################################

	class MLRLookUpLight:
		public MLRInfiniteLight
	{
	public:
		static void
			InitializeClass();
		static void
			TerminateClass();

		MLRLookUpLight();
		MLRLookUpLight(
			Stuff::MemoryStream *stream,
			int version
		);
		~MLRLookUpLight();

		void
			Save(Stuff::MemoryStream *stream);

		virtual LightType
			GetLightType() 
				{ Check_Object(this); return LookUpLight; }


		virtual void
			LightVertex(const MLRVertexData&);

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// LookUp Light Specific
	//
	void
		SetMapOrigin(Scalar x, Scalar y, Scalar z)
			{ Check_Object(this); mapOrigin.x = x; mapOrigin.y = y; mapOrigin.z = z; }

	glm::vec3
		GetMapOrigin()
			{ Check_Object(this); return mapOrigin; }

	void
		SetMapSizeAndName(int x, int z, const char *name);

	int
		GetMapZoneCountX()
			{ Check_Object(this); return mapZoneCountX; }
	int
		GetMapZoneCountZ()
			{ Check_Object(this); return mapZoneCountZ; }

	const char*
		GetMapName()
			{ Check_Object(this); return mapName; }

	void
		SetMapZoneSizeX(Scalar x)
			{ Check_Object(this); zoneSizeX = x; Verify(x>SMALL); one_Over_zoneSizeX = 1.0f/x; }
	void
		SetMapZoneSizeZ(Scalar z)
			{ Check_Object(this); zoneSizeZ = z; Verify(z>SMALL); one_Over_zoneSizeZ = 1.0f/z;  }

	Scalar
		GetMapZoneSizeX()
			{ Check_Object(this); return zoneSizeX; }
	Scalar
		GetMapZoneSizeZ()
			{ Check_Object(this); return zoneSizeZ; }

	void SetLightToShapeMatrix(const glm::mat4&);

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
			TestInstance();

	protected:
		bool
			LoadMap();

		glm::vec3 mapOrigin;
		Scalar zoneSizeX, zoneSizeZ;
		Scalar one_Over_zoneSizeX, one_Over_zoneSizeZ;

		int mapZoneCountX, mapZoneCountZ;
		MString mapName;

		unsigned char **maps;

		glm::mat4
			shapeToWorld;
	};

}
