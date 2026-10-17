#ifndef MINEBRUSH_H
#define MINEBRUSH_H
/*************************************************************************************************\
MineBrush.h			: Interface for the MineBrush component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef BRUSH_H
#include "Brush.h"
#endif

#include "Action.h"
#include "EditorObjects.h"
#include <vector>



//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
MineBrush:
**************************************************************************************************/
class MineBrush: public Brush
{
	public:
	
	MineBrush(){ pAction = nullptr; }
	virtual ~MineBrush(){}
	virtual bool beginPaint();
	virtual Action* endPaint();
	virtual bool paint( Stuff::Vector3D& worldPos, int screenX, int screenY  );
	virtual bool canPaint( Stuff::Vector3D& worldPos, int screenX, int screenY, int flags ); 
	virtual bool canPaintSelection( );
	virtual Action* applyToSelection();

	private:


		class MineAction : public Action
		{
		public:


			virtual ~MineAction(){}
			virtual bool redo();
			virtual bool undo();

			struct CTileMineInfo {
				int row;
				int column;
				unsigned long mineState;
			};
			std::vector<CTileMineInfo> mineInfoList;

			void AddChangedTileMineInfo( CTileMineInfo& info );
		};

		MineAction* pAction;
};


//*************************************************************************************************
#endif  // end of file ( MineBrush.h )
