/***************************************************************
* FILENAME: ObjectSelectionBrush.cpp
* DESCRIPTION: Implements object-selection brush behavior for the Editor.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Added screen-space single-click object picking while preserving drag-select behavior.
****************************************************************/

#define OBJECTSELECTIONBRUSH_CPP

#include "stdafx.h"
#include "ObjectSelectionBrush.h"

#ifndef CAMERA_H
#include "Camera.h"
#endif

#ifndef EDITOROBJECTMGR_H
#include "EditorObjectMgr.h"
#endif

#ifndef ACTION_H
#include "Action.h"
#endif

#include "utilities.h"

#include "EditorMessages.h"
#include "resource.h"
#include "EditorInterface.h"


ObjectSelectionBrush::ObjectSelectionBrush()
{ 
	bPainting = false; 
	pCurAction = NULL; 

	lastPos.x = lastPos.y = lastPos.z = lastPos.w = 0.0f;		//Keep the FPU exception from going off!
	bFirstClick = false;
}

ObjectSelectionBrush::~ObjectSelectionBrush()
{
	if ( EditorObjectMgr::instance() )
		EditorObjectMgr::instance()->unselectAll();
	if ( land )
		land->unselectAll();
}

bool ObjectSelectionBrush::beginPaint()
{
	lastPos.x = lastPos.y = 0.0;
	bPainting = true;
	bFirstClick = !bFirstClick;
	return true;
}

Action* ObjectSelectionBrush::endPaint()
{
	bPainting = false;
	Action* pRetAction = NULL;
	if ( pCurAction )
	{
		if ( pCurAction->vertexInfoList.Count() )
		{
			pRetAction = pCurAction;
			pCurAction = NULL;
			land->recalcWater();
			land->reCalcLight();	
		}

		else
		{
			delete pCurAction;
			pCurAction = NULL;
		}
	}
	
	return pRetAction;
}

bool ObjectSelectionBrush::paint( Stuff::Vector3D& worldPos, int screenX, int screenY )
{
	Stuff::Vector4D endPos;
	endPos.x = screenX;
	endPos.y = screenY;

	//if ( bFirstClick ) // otherwise, do a new area select
	{
		bool bShift = GetAsyncKeyState( KEY_LSHIFT );
		
		// select the objects
		if ( lastPos.x != 0.0 && lastPos.y != 0.0 )
		{
			if ( !bShift )
			{
				land->unselectAll();
				EditorObjectMgr::instance()->unselectAll();
			}
			eye->projectZ( lastWorldPos, lastPos );
			EditorObjectMgr::instance()->select( lastPos, endPos );
			land->selectVerticesInRect( lastPos, endPos, false );
		}
		else
		{
			if ( lastPos != endPos )
			{
				land->unselectAll();
				EditorObjectMgr::instance()->unselectAll();			
			}
			lastPos = endPos;
			Stuff::Vector2DOf<long> screenPos;
			screenPos.x = screenX;
			screenPos.y = screenY;

			eye->inverseProject( screenPos, lastWorldPos );

			// by Methuselas: drag-select already works from projected object centers;
			// single-click needs to test the actual mouse screen point so remastered
			// mech appearances do not miss through terrain inverse-project drift.
			const EditorObject* pInfo = EditorObjectMgr::instance()->getObjectAtScreenPosition( screenX, screenY );
			if ( pInfo )
			{
				if ( !bShift || (bShift && pInfo->isSelected() == false ) )
					(const_cast<EditorObject*>(pInfo))->select( true );
				else
					(const_cast<EditorObject*>(pInfo))->select( false );
			}
			else
			{
				 int tileR, tileC;
				land->worldToTile( worldPos, tileR, tileC );
				if ( tileR > -1 && tileR < land->realVerticesMapSide
					&& tileC > -1 && tileC < land->realVerticesMapSide )
				{
					// figure out which vertex is closest
					if ( fabs(worldPos.x - land->tileColToWorldCoord[tileC]) >= land->worldUnitsPerVertex/2 )
						tileC++;

					if ( fabs(worldPos.y - land->tileRowToWorldCoord[tileR]) >= land->worldUnitsPerVertex/2 )
						tileR++;

					// by Methuselas: edge clicks can round to one-past-the-end after
					// the first bounds check.  Re-check before terrain selection array
					// access so object selection does not crash near map borders.
					if ( tileR > -1 && tileR < land->realVerticesMapSide
						&& tileC > -1 && tileC < land->realVerticesMapSide )
					{
						if (!bShift || (bShift && !land->isVertexSelected( tileR, tileC ) ) )
							land->selectVertex( tileR, tileC );

						else // shift key, object is selected
							land->selectVertex( tileR, tileC, false );
					}
				}
			}
		}
	}


	return true;
}

void ObjectSelectionBrush::render( int screenX, int screenY )
{
	
	if ( bPainting )
	{
		//------------------------------------------
		Stuff::Vector4D Screen;
		eye->projectZ( lastWorldPos, Screen );
		
		GUI_RECT rect = { screenX, screenY, (long)Screen.x, (long)Screen.y };
		drawRect( rect, 0x30ffffff );
		drawEmptyRect( rect, 0xff000000, 0xff000000 );
	}

	if ( !bPainting )
	{
		EditorInterface::instance()->ChangeCursor( IDC_TARGET );
	}
}

EditorObjectPointerList ObjectSelectionBrush::selectedObjectPointerList()
{
	return EditorObjectMgr::instance()->getSelectedObjectList();
}


//*************************************************************************************************
// end of file ( ObjectSelectionBrush.cpp )
 