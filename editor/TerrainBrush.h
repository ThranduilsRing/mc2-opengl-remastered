#ifndef TERRAINBRUSH_H
#define TERRAINBRUSH_H
/*************************************************************************************************\
TerrainBrush.h		: Interface for the TerrainBrush component. used to paint textures
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef BRUSH_H
#include "Brush.h"
#endif
#include "Action.h"

#include <set>

class TerrainBrush: public Brush
{
	public:

		inline TerrainBrush( int Type )
		{
			if ( Type == -1 )
				Type = s_lastType;

			terrainType = Type;
			s_lastType = Type;
			pAction = nullptr;
			radius  = s_lastRadius;
		}
		virtual ~TerrainBrush(){}

		bool beginPaint();
		Action* endPaint();
		virtual bool paint( Stuff::Vector3D& worldPos, int screenX, int screenY );
		virtual bool canPaint( Stuff::Vector3D& worldPos, int screenX, int screenY, int flags ) { return true; }
		virtual void render( int screenX, int screenY );

		virtual Action* applyToSelection();

		void  setRadius( float r ) { radius = r; s_lastRadius = r; }
		float getRadius() const    { return radius; }



	private:

		// SUPPRESS THESE!
		TerrainBrush( const TerrainBrush& TerrainBrush );
		TerrainBrush& operator=( const TerrainBrush& TerrainBrush );

		int terrainType;
		float radius;                       // world units; 0 => single closest vertex
		std::set<int> touchedThisStroke;    // one apply per vertex per stroke

		static int s_lastType;
		static float s_lastRadius;

		ActionPaintTile* pAction;
};


//*************************************************************************************************
#endif  // end of file ( TerrainBrush.h )
