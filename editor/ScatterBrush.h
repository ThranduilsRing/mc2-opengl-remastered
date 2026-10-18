#ifndef SCATTERBRUSH_H
#define SCATTERBRUSH_H
/*************************************************************************************************\
ScatterBrush.h : radius brush that scatters object instances (the generalised forest brush).
                 Paints copies of a chosen object (tree, rock, ruin, etc.) within a disc with
                 density / spacing / rotation+scale jitter / slope filtering. Forest painting is
                 just this brush with a tree object selected.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef BRUSH_H
#include "Brush.h"
#endif

class Action;

class ScatterBrush : public Brush
{
public:
	ScatterBrush( int group, int indexInGroup, int alignment );
	virtual ~ScatterBrush();

	virtual bool    beginPaint();
	virtual Action* endPaint();
	virtual bool    paint( glm::vec3& worldPos, int screenX, int screenY );
	virtual void    render( int screenX, int screenY );
	virtual void    update( int screenX, int screenY ) {}

	int getGroup() const { return m_group; }
	int getIndexInGroup() const { return m_indexInGroup; }

	// Shared scatter controls (driven by the ImGui panel). Static so the panel can
	// edit them without holding the live brush; one setting set for all scattering.
	static float    s_radius;        // wu
	static int      s_density;       // placement attempts per click
	static float    s_minSpacing;    // wu; skip if an object is closer than this
	static float    s_maxSlopeRise;  // wu rise over one vertex spacing; >this => skip (0 = no filter)
	static bool     s_randomRotation;
	static float    s_scaleJitter;   // 0..1 (+/- fraction)

private:
	ScatterBrush();
	ScatterBrush( const ScatterBrush& );
	ScatterBrush& operator=( const ScatterBrush& );

	int     m_group;
	int     m_indexInGroup;
	int     m_alignment;
	void*   m_pAction;        // BuildingBrush::BuildingAction* (opaque here to avoid header coupling)
	unsigned m_rng;
	bool    m_haveLast;       // throttle: only re-scatter after the cursor moves
	float   m_lastX, m_lastY;
};

#endif // SCATTERBRUSH_H
