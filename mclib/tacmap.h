#ifndef TACMAP_H
#define TACMAP_H
/*************************************************************************************************\
TacMap.h			: Interface for the TacMap component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

//*************************************************************************************************
#include"mclib.h"


inline constexpr unsigned int BUILDING_COLOR = 0xffff9f00;
/**************************************************************************************************
CLASS DESCRIPTION
TacMap:
**************************************************************************************************/
class TacMap
{
	public:

	TacMap();

	static void worldToTacMap( glm::vec3& world, int xOffset, int yOffset, int xSize, int ySize, gos_VERTEX& tac );
	static void tacMapToWorld( const Stuff::Vector2DOf<long>& screen, int xSize, int ySize,  glm::vec3& world );

};


//*************************************************************************************************
#endif  // end of file ( TacMap.h )
