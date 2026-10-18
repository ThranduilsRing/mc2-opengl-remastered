#ifndef BUILDINGBRUSH_H
#define BUILDINGBRUSH_H
/*************************************************************************************************\
BuildingBrush.h		: Interface for the BuildingBrush component. The thing you use to paint 
						buildings
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

#ifndef EDITOROBJECTMGR_H
#include "EditorObjects.h"
#endif

//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
BuildingBrush:
**************************************************************************************************/
class BuildingBrush: public Brush
{
	public:

		BuildingBrush( int group, int indexInGroup, int Alignment );
		virtual ~BuildingBrush();

		virtual bool beginPaint();
		virtual Action* endPaint();
		virtual bool paint( glm::vec3& worldPos, int screenX, int screenY );
		virtual bool canPaint( glm::vec3& worldPos, int screenX, int screenY, int flags );
		virtual void render( int ScreenMouseX, int ScreenMouseY);
		virtual void update( int screenX, int screenY );

		// Clamp a world position to the valid terrain cell grid and snap it to
		// that cell's centre. Guards the unbounded worldToCell/terrainElevation
		// OOB read (0xC0000005). Static so the placement-OOB smoke can exercise
		// the exact production guard. terr may be null (no-op).
		static void snapToTerrainCell( class Terrain* terr, glm::vec3& pos );

		void rotateBrush( int direction );
		void addRotationDegrees( float deg );

		// Read-only accessors so the object companion panel can show/highlight
		// the active object and reselect siblings without re-walking the menu.
		int getGroup() const { return group; }
		int getIndexInGroup() const { return indexInGroup; }
		int getAlignment() const { return alignment; }

		
		class BuildingAction : public Action
		{
		public:
			
			virtual ~BuildingAction();
			virtual bool redo();
			virtual bool undo();
			virtual void addBuildingInfo(EditorObject& info);

			std::vector<EditorObject*> objInfoPtrList;
		};

		protected:

		// suppression
		BuildingBrush( const BuildingBrush& buildingBrush );
		BuildingBrush& operator=( const BuildingBrush& buildingBrush );
		BuildingBrush();

		int group;
		int indexInGroup;
		float curRotation;

		BuildingAction*		pAction;

		ObjectAppearance*	pCursor;

		int					alignment;
};


//*************************************************************************************************
#endif  // end of file ( BuildingBrush.h )
