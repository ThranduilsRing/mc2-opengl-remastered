#ifndef DAMAGEBRUSH_H
#define DAMAGEBRUSH_H
/*************************************************************************************************\
DamageBrush.h			: Interface for the DamageBrush component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef BRUSH_H
#include "Brush.h"
#endif

#include "Action.h"
#include "EditorObjects.h"



//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
DamageBrush:
**************************************************************************************************/
class DamageBrush: public Brush
{
	public:
	
	DamageBrush(bool bDamage ){ damage = bDamage; pAction = nullptr; }
	~DamageBrush() override{}
	bool beginPaint() override;
	Action* endPaint() override;
	bool paint( Stuff::Vector3D& worldPos, int screenX, int screenY  ) override;
	bool canPaint( Stuff::Vector3D& worldPos, int screenX, int screenY, int flags ) override; 
	bool canPaintSelection( ) override;
	Action* applyToSelection() override;
	bool	damage;

	private:
		ModifyBuildingAction* pAction;
};


//*************************************************************************************************
#endif  // end of file ( DamageBrush.h )
