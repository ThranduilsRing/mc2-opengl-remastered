#ifndef MECHPURCHASESCREEN_H
#define MECHPURCHASESCREEN_H
/*************************************************************************************************\
MechPurchaseScreen.h			: Interface for the MechPurchaseScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef LOGISTICSSCREEN_H
#include"logisticsscreen.h"
#endif

#include"mechlistbox.h"
#include"logisticsmech.h"
#include"elist.h"
#include"logisticsmechdisplay.h"

class LogisticsMech;

//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
MechPurchaseScreen:
**************************************************************************************************/
class MechPurchaseScreen: public LogisticsScreen
{
	public:

		static MechPurchaseScreen* instance() { return s_instance; }

		MechPurchaseScreen();
		~MechPurchaseScreen() override;

		int					init( FitIniFile& file );

		void		begin() override;
		void		end() override;
		void		update() override;
		void		render(int xOffset, int yOffset) override;

		void				setMech( LogisticsMech* pMech, bool bFromLB = 0 );
		void				beginDrag( LogisticsMech* pMech );
		int			handleMessage( unsigned long, unsigned long ) override;

	private:
	
		MechPurchaseScreen( const MechPurchaseScreen& src );
		MechPurchaseScreen& operator=( const MechPurchaseScreen& echPurchaseScreen );

		MechListBox			inventoryListBox;
		MechListBox			variantListBox;
		LogisticsMechDisplay	mechDisplay;

		LogisticsMech*		pDragMech;
		aObject				dragIcon;

		bool				acceptPressed;


		static MechPurchaseScreen* s_instance;


		bool				dragStartLeft;

		
		using MECH_LIST = EList<LogisticsMech, LogisticsMech >;	
		MECH_LIST		prevInventory;

		float				countDownTime;
		float				curCount;
		float				previousAmount;
		float				oldCBillsAmount;

		float				flashTime;


	// HELPER FUNCTIONS

		void addSelectedMech();
		void removeSelectedMech();
		void addMech( LogisticsMech* pMech );
		void removeMech( LogisticsMech* pMech );
		void endDrag();
		bool selectFirstBuyableMech();



};


//*************************************************************************************************
#endif  // end of file ( MechPurchaseScreen.h )
