#ifndef MECHPURCHASESCREEN_H
#define MECHPURCHASESCREEN_H
/*************************************************************************************************\
MechPurchaseScreen.h			: Interface for the MechPurchaseScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef LOGISTICSSCREEN_H
#include <vector>
#include"logisticsscreen.h"
#endif

#include"mechlistbox.h"
#include"logisticsmech.h"
#include"logisticsmechdisplay.h"
#include "purchase_catalog.h"

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
		std::vector<std::unique_ptr<LogisticsMech>> purchaseMechs;
		std::vector<mc2purchase::Group> purchaseGroups;
		std::vector<std::string> purchaseGroupNames;
		LogisticsMech* purchaseSelection = nullptr;
		LogisticsMechDisplay	mechDisplay;

		LogisticsMech*		pDragMech;
		aObject				dragIcon;

		bool				acceptPressed;


		static MechPurchaseScreen* s_instance;


		bool				dragStartLeft;

		
		// Copies of the inventory taken in begin() and put back in end(); iterated only, no element address is kept.
		using MECH_LIST = std::vector<LogisticsMech>;
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
		void renderPurchaseGroups(int xOffset, int yOffset);



};


//*************************************************************************************************
#endif  // end of file ( MechPurchaseScreen.h )
