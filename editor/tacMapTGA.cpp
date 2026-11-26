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
		glm::vec3 world{};
		POINT pts[5];


		// Use the scene dimensions and terrain hits from the rendered camera rays.
		// A corner above the horizon or outside the map cannot close a footprint.
		bool hits[4];
		screen.x = 1;
		screen.y = 1;
		hits[0] = eye->pickTerrainRay(screen.x, screen.y, world);
		TacMap::worldToTacMap( world, 0, 0, EDITOR_TACMAP_DISPLAY_SIZE, EDITOR_TACMAP_DISPLAY_SIZE,  tmp );
		pts[0].x = tmp.x;
		pts[0].y = tmp.y;

		screen.y = Environment.drawableHeight - 1;
		hits[1] = eye->pickTerrainRay(screen.x, screen.y, world);
		TacMap::worldToTacMap( world, 0, 0, EDITOR_TACMAP_DISPLAY_SIZE, EDITOR_TACMAP_DISPLAY_SIZE,  tmp );
		pts[1].x = tmp.x;
		pts[1].y = tmp.y;

		screen.x = Environment.drawableWidth - 1;
		hits[2] = eye->pickTerrainRay(screen.x, screen.y, world);
		TacMap::worldToTacMap( world, 0, 0,EDITOR_TACMAP_DISPLAY_SIZE, EDITOR_TACMAP_DISPLAY_SIZE, tmp );
		pts[2].x = tmp.x;
		pts[2].y = tmp.y;

		screen.y = 1;
		hits[3] = eye->pickTerrainRay(screen.x, screen.y, world);
		TacMap::worldToTacMap( world, 0, 0, EDITOR_TACMAP_DISPLAY_SIZE, EDITOR_TACMAP_DISPLAY_SIZE, tmp );
		pts[3].x = tmp.x;
		pts[3].y = tmp.y;

		pts[4] = pts[0];

		CPen ourPen( PS_SOLID, 1,  0x00ffffff );
		CPen *pReplacedPen = dc.SelectObject(&ourPen);

		for ( int i = 0; i < 4; ++i )
		{
			if (!hits[i] || !hits[(i + 1) % 4]) continue;
			dc.MoveTo(pts[i]);
			dc.LineTo(pts[i + 1]);
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
