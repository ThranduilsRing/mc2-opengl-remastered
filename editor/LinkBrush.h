#ifndef LINKBRUSH_H
#define LINKBRUSH_H
/*************************************************************************************************\
LinkBrush.h			: Interface for the LinkBrush component.
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

#ifndef BUILDINGLINK_H
#include "BuildingLink.h"
#endif

#include "mclib.h"
#include <vector>

class EditorObject;

//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
LinkBrush:
**************************************************************************************************/
class LinkBrush: public Brush
{
	public:

	LinkBrush( bool bLink );
	virtual ~LinkBrush();
	virtual bool beginPaint();
	virtual Action* endPaint();
	virtual bool paint( glm::vec3& worldPos, int screenX, int screenY  );
	virtual bool canPaint( glm::vec3& worldPos, int screenX, int screenY, int flags );
	virtual bool canPaintSelection( ){ return false; }
	virtual void render( int screenX, int screenY );

	const EditorObject*		parent;
	glm::vec3			parentPos;
	bool					bLink;


	bool unPaint( glm::vec3& worldPos, int screenX, int screenY  );
	bool canUnPaint( glm::vec3& worldPos, int screenX, int screenY, int flags );

	struct LinkInfo
	{
		
		enum TYPE
		{
			ADD = 0,
			REMOVE,
			EDIT
		};

		LinkInfo( BuildingLink* m_pOriginal, TYPE type = EDIT  );
		
		BuildingLink m_LinkCopy;  // copy of link
		int  type;

	};
	
	class LinkAction : public Action
	{
	
	public:
		
		
		LinkAction();
		~LinkAction( ){}
		
		virtual bool redo();
		virtual bool undo();

		std::vector<LinkInfo> changedLinks;
		void AddToListOnce( const LinkBrush::LinkInfo& info );
	};

	LinkAction* pAction;

};


//*************************************************************************************************
#endif  // end of file ( LinkBrush.h )
