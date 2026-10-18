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

#include <vector>

class ActionPaintTile;
class EditorObject;

using EditorObjectPointerList = std::vector<EditorObject*>;

class ObjectSelectionBrush: public Brush
{
	public:

		ObjectSelectionBrush();
		~ObjectSelectionBrush() override;
		bool beginPaint() override;
		Action* endPaint() override;
		bool paint( glm::vec3& worldPos, int screenX, int screenY ) override;
		bool canPaint( glm::vec3& worldPos, int screenX, int screenY, int flags ) override { return true; } 
		void render( int screenX, int screenY ) override;
		EditorObjectPointerList selectedObjectPointerList();

	private:

		// suppressed
		ObjectSelectionBrush( const ObjectSelectionBrush& electionBrush );
		ObjectSelectionBrush& operator=( const ObjectSelectionBrush& electionBrush );

		Stuff::Vector4D		lastPos;
		glm::vec3		lastWorldPos;

		bool bPainting;
		bool bFirstClick;

		ActionPaintTile*	pCurAction;
};


//*************************************************************************************************
#endif  // end of file ( ObjectSelectionBrush.h )
