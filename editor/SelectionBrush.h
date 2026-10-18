#ifndef SELECTIONBRUSH_H
#define SELECTIONBRUSH_H
/*************************************************************************************************\
SelectionBrush.h	: Interface for the SelectionBrush component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef BRUSH_H
#include "Brush.h"
#endif

class ActionPaintTile;
class ModifyBuildingAction;
class EditorObject;

class SelectionBrush: public Brush
{
	public:

		SelectionBrush( bool Area, int radius );
		virtual ~SelectionBrush();
		virtual bool beginPaint();
		virtual Action* endPaint();
		virtual bool paint( glm::vec3& worldPos, int screenX, int screenY );
		virtual bool canPaint( glm::vec3& worldPos, int screenX, int screenY, int flags ) { return true; } 
		virtual void render( int screenX, int screenY );
		bool    paintSmooth( glm::vec3& worldPos, int screenX, int screenY, int radius );

	private:

		// suppressed
		SelectionBrush( const SelectionBrush& electionBrush );
		SelectionBrush& operator=( const SelectionBrush& electionBrush );
		static float calcNewHeight( int vertexRow, int vertexCol, float screenDeltaY );
		bool   paintSmoothArea( glm::vec3& worldPos, int screenX, int screenY, float radiusX, float radiusY,
									   int j, int i);

		Stuff::Vector4D		lastPos;
		glm::vec3		firstWorldPos;
		glm::vec3		lastWorldPos;

		// Dedicated last-cursor-screen state for the hand-tool (pDragBuilding) move.
		// lastPos doubles as the height-drag delta tracker and is zeroed in beginPaint,
		// so it is unreliable as the per-frame screen delta base for the jacobian move.
		Stuff::Vector2DOf<float>	m_lastDragScreen;

		// BUG1 (zoomed-in marquee): track the rubber-band rectangle in SCREEN space
		// directly. The previous path built firstWorldPos/lastWorldPos via the broken
		// eye->inverseProject (documented X-collapse) and endPaint re-projected them to
		// screen — under zoom that produced an empty/garbage rect so nothing selected.
		Stuff::Vector4D		m_marqueeFirstScreen;
		Stuff::Vector4D		m_marqueeLastScreen;
		bool				m_marqueeStarted;

		bool bPainting;
		bool bArea;
		bool bDrag;
		EditorObject* pDragBuilding;
		bool bFirstClick;

		int lastRow;
		int lastCol;

		ActionPaintTile*	pCurAction;
		ModifyBuildingAction*	pCurModifyBuildingAction;

		int smoothRadius;


};


//*************************************************************************************************
#endif  // end of file ( SelectionBrush.h )
