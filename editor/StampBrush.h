#ifndef STAMPBRUSH_H
#define STAMPBRUSH_H
/*************************************************************************************************\
StampBrush.h : one-shot terrain "stamp" — applies a parametric height profile over a disc on a
               single click (pad / crater / hill). Reusable designer-controllable terrain edits.
               Reuses the sculpt undo action (ActionPaintTile) and recomputes normals after.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef BRUSH_H
#include "Brush.h"
#endif
#ifndef ACTION_H
#include "Action.h"
#endif

#include <set>

class StampBrush : public Brush
{
public:
	enum Type { PAD = 0, CRATER = 1, HILL = 2 };

	StampBrush( Type type, float radius, float strength );
	virtual ~StampBrush();

	virtual bool    beginPaint();
	virtual Action* endPaint();
	virtual bool    paint( glm::vec3& worldPos, int screenX, int screenY );
	virtual void    render( int screenX, int screenY );

	void  setRadius( float r )   { m_radius = r; }
	void  setStrength( float s ) { m_strength = s; }
	void  setType( Type t )      { m_type = t; }
	Type  getType() const        { return m_type; }

private:
	StampBrush();

	Type             m_type;
	float            m_radius;     // world units
	float            m_strength;   // crater depth / hill height (wu)
	bool             m_applied;    // one-shot guard: stamp once per click-stroke
	ActionPaintTile* m_pAction;
};

#endif // STAMPBRUSH_H
