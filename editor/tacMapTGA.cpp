#define TGAMAPTGA_CPP
/***************************************************************
* FILENAME: tacMapTGA.cpp
* DESCRIPTION: Paints and refreshes the Editor tactical map TGA control.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Doubled the Editor tacmap viewport overlay while preserving the 128x128 backing bitmap refresh.
****************************************************************/

#include "stdafx.h"
#include "TacMapTGA.h"
#include "tacMap.h"
#include "EditorData.h"


BEGIN_MESSAGE_MAP(TacMapTGA, TGAWnd)
	//{{AFX_MSG_MAP(TGAWnd)
	ON_WM_PAINT()
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

// THIS NEEDS TO BE MOVED TO A DERIVED CLASS!
void TacMapTGA::OnPaint()
{
	TGAWnd::OnPaint();

	CDC* pDC =GetDC();
	CDC& dc = *pDC;
	// device context for painting

	
	if ( eye && land && land->realVerticesMapSide )
	{
 		Stuff::Vector2DOf< long > screen;
		gos_VERTEX tmp;
		Stuff::Vector3D world;
		POINT pts[5];


		// Draw the view rectangle. Use the cheap O(1) ground-plane unproject —
		// NOT eye->inverseProject (the legacy terrain picker, which scans ~40k
		// quads per call and froze the editor for seconds when OnMouseWheel
		// forced a synchronous tacmap repaint per notch). The rectangle is an
		// approximate overlay; exact terrain intersection is unnecessary.
		screen.x = 1;
		screen.y = 1;
		eye->screenToGroundPlaneApprox( screen.x, screen.y, world );
		TacMap::worldToTacMap( world, 0, 0, EDITOR_TACMAP_DISPLAY_SIZE, EDITOR_TACMAP_DISPLAY_SIZE,  tmp );
		pts[0].x = tmp.x;
		pts[0].y = tmp.y;

		screen.y = Environment.screenHeight - 1;
		eye->screenToGroundPlaneApprox( screen.x, screen.y, world );
		TacMap::worldToTacMap( world, 0, 0, EDITOR_TACMAP_DISPLAY_SIZE, EDITOR_TACMAP_DISPLAY_SIZE,  tmp );
		pts[1].x = tmp.x;
		pts[1].y = tmp.y;

		screen.x = Environment.screenWidth - 1;
		eye->screenToGroundPlaneApprox( screen.x, screen.y, world );
		TacMap::worldToTacMap( world, 0, 0,EDITOR_TACMAP_DISPLAY_SIZE, EDITOR_TACMAP_DISPLAY_SIZE, tmp );
		pts[2].x = tmp.x;
		pts[2].y = tmp.y;

		screen.y = 1;
		eye->screenToGroundPlaneApprox( screen.x, screen.y, world );
		TacMap::worldToTacMap( world, 0, 0, EDITOR_TACMAP_DISPLAY_SIZE, EDITOR_TACMAP_DISPLAY_SIZE, tmp );
		pts[3].x = tmp.x;
		pts[3].y = tmp.y;

		pts[4] = pts[0];

		CPen ourPen( PS_SOLID, 1,  0x00ffffff );
		CPen *pReplacedPen = dc.SelectObject(&ourPen);

		dc.MoveTo( pts[0].x, pts[0].y );
		for ( int i = 1; i < 5; ++i )
		{
			dc.LineTo( pts[i] );
		}

		dc.SelectObject(pReplacedPen);
	}

	ReleaseDC( pDC );
}

void TacMapTGA::refreshBmp()
{
	// by Methuselas: refreshes the backing bitmap at 128x128 for mission
	// packet compatibility.  The floating Editor tacmap window itself is
	// enlarged to 2x by EDITOR_TACMAP_DISPLAY_SIZE and stretched by TGAWnd.
	// The DIB backing store exists only after the window's first paint; a headless or minimized
	// editor (playtest smoke) saves with no tac map window painted, and has no bitmap to refresh.
	if (!m_pBits)
		return;
	EditorData::instance->drawTacMap( (BYTE*)m_pBits, EDITOR_TACMAP_BITMAP_SIZE * EDITOR_TACMAP_BITMAP_SIZE * 4, EDITOR_TACMAP_BITMAP_SIZE );
	RedrawWindow( );
}
	
	
//*************************************************************************************************
// end of file ( TgaMapTGA.cpp )
