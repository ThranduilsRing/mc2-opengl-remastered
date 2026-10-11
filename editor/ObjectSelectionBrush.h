#ifndef OBJECTSELECTIONBRUSH_H
#define OBJECTSELECTIONBRUSH_H
/*************************************************************************************************\
ObjectSelectionBrush.h	: Interface for the ObjectSelectionBrush component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef BRUSH_H
#include "Brush.h"
#endif

#include "elist.h"

class ActionPaintTile;
class EditorObject;

using EditorObjectPointerList = EList <EditorObject *, EditorObject *>;

class ObjectSelectionBrush: public Brush
{
	public:

		ObjectSelectionBrush();
		~ObjectSelectionBrush() override;
		bool beginPaint() override;
		Action* endPaint() override;
		bool paint( Stuff::Vector3D& worldPos, int screenX, int screenY ) override;
		bool canPaint( Stuff::Vector3D& worldPos, int screenX, int screenY, int flags ) override { return true; } 
		void render( int screenX, int screenY ) override;
		EditorObjectPointerList selectedObjectPointerList();

	private:

		// suppressed
		ObjectSelectionBrush( const ObjectSelectionBrush& electionBrush );
		ObjectSelectionBrush& operator=( const ObjectSelectionBrush& electionBrush );

		Stuff::Vector4D		lastPos;
		Stuff::Vector3D		lastWorldPos;

		bool bPainting;
		bool bFirstClick;

		ActionPaintTile*	pCurAction;
};


//*************************************************************************************************
#endif  // end of file ( ObjectSelectionBrush.h )
