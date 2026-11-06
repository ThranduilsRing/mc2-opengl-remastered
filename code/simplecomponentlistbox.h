#ifndef SIMPLECOMPONENTLISTBOX_H
#define SIMPLECOMPONENTLISTBOX_H
#include "mc2_czstring.h"
/*************************************************************************************************\
SimpleComponentListBox.h			: Interface for the SimpleComponentListBox component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef ALISTBOX_H
#include"alistbox.h"
#endif

class LogisticsVariant;
class LogisticsVehicle;
class LogisticsComponent;
class LogisticsScreen;
//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
SimpleComponentListBox:
**************************************************************************************************/
class ComponentListBox: public aListBox
{
	public:

		ComponentListBox();
		~ComponentListBox();
		void setMech( LogisticsVariant* pMech );
		void setVehicle( LogisticsVehicle* pVehicle );
		void setComponents( long componentCount, LogisticsComponent** components );

	private:

		ComponentListBox( const ComponentListBox& src );
		ComponentListBox& operator=( const ComponentListBox& omponentListBox );
		// HELPER FUNCTIONS

};



//*************************************************************************************************
// Mirror text and per-item range colors into the defs-page weapon loadout list.
void syncWeaponLoadout( LogisticsScreen* screen, ComponentListBox& box, mc2::czstring listKey );

#endif  // end of file ( SimpleComponentListBox.h )
