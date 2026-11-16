#define MECHPURCHASESCREEN_CPP
/*************************************************************************************************\
MechPurchaseScreen.cpp			: Implementation of the MechPurchaseScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"mechpurchasescreen.h"
#include "Gates/gate.h"
#include"mclib.h"
#include"logisticsdata.h"
#include"mechbayscreen.h"
#include"gamesound.h"
#include "../resource.h"
#include"simplecamera.h"
#include"attributemeter.h"
#include"chatwindow.h"
#include"multplyr.h"
#include"../GuiRuntime/GuiRuntime.h"
#include "mc2fmt.h"
#include "imgui.h"
#include "../GameOS/gameos/gos_input.h"

MechPurchaseScreen* MechPurchaseScreen::s_instance = nullptr;

// Interactive cheat gate
// The launcher writes these into launcher_env.json, which main() applies after static initialization, so they
// are read at first use rather than in a namespace-scope initializer.
static bool cheatInfiniteMoney() { static const bool on = mc2::gate::get<"MC2_CHEAT_INFINITE_MONEY">(); return on; }

constexpr int INVENTORY_ID = 88;


//-------------------------------------------------------------------------------------------------

MechPurchaseScreen::MechPurchaseScreen(  ) 
: inventoryListBox( 1, 0 )
{
	status = RUNNING;
	gosASSERT( !s_instance );
	s_instance = this;
	acceptPressed = 0;
	pDragMech = nullptr;
	dragStartLeft = 0;
	helpTextArrayID = 4; 
	inventoryListBox.setID( INVENTORY_ID );

	countDownTime = .5;
	curCount = 0.0;
	previousAmount = 0;

	oldCBillsAmount  =0;

	flashTime = 0.f;
}

//-------------------------------------------------------------------------------------------------

MechPurchaseScreen::~MechPurchaseScreen()
{
	s_instance = nullptr;

	inventoryListBox.destroy();
}


//-------------------------------------------------------------------------------------------------

int MechPurchaseScreen::init( FitIniFile& file )
{
	MechListBox::init();

	mechDisplay.init();

	setLayoutNeeds( { "mcl_m$.fit", 39, 4, 5, 0, 5, false } ); // stock element counts; see LayoutNeeds
	LogisticsScreen::init( file, "Static", "Text", "Rect", "Button" );

	(static_cast<aObject*>(&inventoryListBox))->init( rects[0].left(), rects[0].top(),
		rects[0].width(), rects[0].height() );

	inventoryListBox.setScrollBarOrange();



	for ( int i= 0; i < buttonCount; i++ )
		buttons[i].setMessageOnRelease();


	return NO_ERR;
}
//-------------------------------------------------------------------------------------------------

void MechPurchaseScreen::update()
{

	
	if ( !MPlayer || !ChatWindow::instance()->pointInside(userInput->getMouseX(), userInput->getMouseY()) )
		LogisticsScreen::update();
	

	// update CBills

	int amount = LogisticsData::instance->getCBills();
	long color = 0xff005392;
	if ( amount != oldCBillsAmount )
	{
		previousAmount = oldCBillsAmount - amount;
		curCount = .00001f;
		oldCBillsAmount = amount;
		if ( previousAmount < 0 )
			soundSystem->playDigitalSample( WINDOW_OPEN );
		else
			soundSystem->playDigitalSample( WINDOW_CLOSE );
	}
	if ( curCount && curCount + frameLength < countDownTime  )
	{
		curCount += frameLength;
		float curAmount = previousAmount - (curCount/countDownTime * previousAmount); 
		amount += curAmount;

		color = 0xffc8e100;
		if ( curAmount > 0 )
			color = 0xffa21600;
	}
	else if ( flashTime )
	{
		flashTime += frameLength;
		if ( flashTime < .125
			|| ( flashTime > .25 && flashTime < .3875 )
			|| ( flashTime > .5 && flashTime < .6625 ) )
		{
			color = 0xff000000;
		}
		else if ( flashTime > .75 )
		{
			flashTime = 0;
		}
		else
		{
			color = 0xffff0000;			
		}
	}
	

	char tmp[64];
	(*std::format_to_n( tmp, sizeof(tmp) - 1, "{:d} ", mc2::fmt::as_i32(amount)).out = '\0');
	textObjects[1].setText( tmp );
	textObjects[1].setColor( color );
	// CBILLS-MIRROR-1: the defs page suppresses the legacy readout — mirror the
	// live amount into the defs text element (same pattern as mechbayscreen's
	// cbills_text). Without this the panel showed the static fit label
	// ("C-Bills") instead of the number.
	setDefsElementText( "game.mcl_mdollar.text.cbills", tmp );

	int oldSell = inventoryListBox.GetSelectedItem();
	inventoryListBox.update();
	int newSell = inventoryListBox.GetSelectedItem();
	if ( oldSell != newSell && newSell != -1 )
	{
		purchaseSelection = nullptr;
	}

	LogisticsMech* pCurSell = inventoryListBox.getCurrentMech();
	LogisticsMech* pCurBuy = purchaseSelection;

	// disable the sell button
	buttons[2].disable( pCurSell ? 0 : 1);

	if ( pCurBuy )
	{
		if ( !cheatInfiniteMoney() && pCurBuy->getCost() > LogisticsData::instance->getCBills() )
		{
			pCurBuy = nullptr;
		}

		setMech( pCurBuy );
		
	}
	else if ( pCurSell )
		setMech( pCurSell );

	buttons[1].disable( pCurBuy ? 0 : 1 );
	if ( !pCurBuy &&
		userInput->isLeftClick() && buttons[1].pointInside( userInput->getMouseX(), userInput->getMouseY() ) )
	{
		if ( !flashTime )
			flashTime = .0001f;
	}

	// drag and drop
	// update drag and drop
	if ( pDragMech )
	{
		dragIcon.moveTo( userInput->getMouseX() - dragIcon.width() / 2, 
			userInput->getMouseY() - dragIcon.height() / 2 );

		if ( userInput->leftMouseReleased(  ) )
		{
			endDrag();	
		}

	}

	mechDisplay.update();

	inventoryListBox.enableAllItems();

	if ( MPlayer && ChatWindow::instance() )
		ChatWindow::instance()->update();

}

//-------------------------------------------------------------------------------------------------

void MechPurchaseScreen::render( int xOffset, int yOffset )
{

	if ( !xOffset && !yOffset )
	{
		if ( !MPlayer && !LogisticsData::instance->isSingleMission() && LogisticsData::instance->newMechsAvailable() )
		{
			soundSystem->playBettySample( BETTY_NEW_MECHS );
			LogisticsData::instance->setNewMechsAcknowledged();
		}
	}

	// Legacy->display scale for the text bridge (crisp TTF labels on the two mech
	// lists + the mech-info display, matching the data/defs UI layer).
	// UI-ASPECT-ANCHOR-1: canvas-aware transform (scale + pad origin).
	float tbSx = 1.f, tbSy = 1.f, tbOx = 0.f, tbOy = 0.f;
	aObject::getCanvasTransform( tbSx, tbSy, tbOx, tbOy );

	// Only the two mech lists need the text bridge (mech names).  mechDisplay routes
	// its info text into the mcl_mechinfo defs page and its loadout into a defs GuiList
	// (matching Mech Bay), so it must NOT be bridged -- otherwise the legacy loadout
	// list would TTF-draw on top of the GuiList and double up.
	// DEFS-LIST-ICONS-1: full GUI bridge (not just text) so the MechListBoxItem
	// quads — mech silhouette icons, cost icons, row outlines, scrollbars — reach
	// the ImGui layer instead of being buried under the defs page (text-only
	// rows). aListBox::render also clips bridged edge rows to the box instead of
	// letting them spill below it (DEFS-LIST-CLIP-1).
	aObject::beginGuiBridge( tbSx, tbSy, tbOx, tbOy );
	aObject::beginTextBridge( tbSx, tbSy, 1.0f, tbOx, tbOy );
	inventoryListBox.move( xOffset, yOffset);
	inventoryListBox.render();
	inventoryListBox.move( -xOffset, -yOffset );

	aObject::endTextBridge();
	aObject::endGuiBridge();

	mechDisplay.render( xOffset, yOffset );

	LogisticsScreen::render( xOffset, yOffset );
	renderPurchaseGroups(xOffset, yOffset);

	if ( pDragMech )
		dragIcon.render();

	
	if ( MPlayer && ChatWindow::instance() )
		ChatWindow::instance()->render(xOffset, yOffset);



}

//-------------------------------------------------------------------------------------------------

void MechPurchaseScreen::begin()
{

	inventoryListBox.removeAllItems(true);
	purchaseSelection = nullptr;
	purchaseMechs.clear();
	// initialize both the inventory and icon lists
	auto mechList = LogisticsData::instance->getInventory();
	prevInventory.clear();

	LogisticsMech* pSelMech = nullptr;

	oldCBillsAmount = LogisticsData::instance->getCBills();


	for (auto & mechElem : mechList)
		{
			if ( mechElem->getForceGroup() )
				continue;
		
			prevInventory.push_back( *mechElem );
			
	
			bool bFound = 0;

			for ( int i = 0; i < inventoryListBox.GetItemCount(); i++ )
			{
				if ( (static_cast<MechListBoxItem*>(inventoryListBox.GetItem(i)))->getMech()->getVariant()
					== mechElem->getVariant() )
					bFound = true;
			}

			if ( !bFound )
			{
				MechListBoxItem* item = new MechListBoxItem( mechElem, 1 );
				inventoryListBox.AddItem( item );	

			}

		}

	MechListBoxItem* item = static_cast<MechListBoxItem*>(inventoryListBox.GetItem( 0 ));
	if ( item )
	{
		inventoryListBox.SelectItem( 0 );
		pSelMech = item->getMech();
	}
	

	int count = 0;
	LogisticsData::instance->getPurchasableMechs(nullptr, count);
	std::vector<LogisticsVariant*> pVariants(count);
	LogisticsData::instance->getPurchasableMechs(pVariants.data(), count);
	std::vector<mc2purchase::Item> catalog;

	for ( int i =0; i < count; i++ )
	{
		if ( !MPlayer || MPlayer->missionSettings.variants || pVariants[i]->isDesignerMech() )
		{
			purchaseMechs.push_back(std::make_unique<LogisticsMech>(pVariants[i], -1));
			catalog.push_back({mc2purchase::chassisSource(pVariants[i]->getFileName()),
				pVariants[i]->getName(), pVariants[i]->getMaxWeight()});
		}
	}
	const char* campaignOwner = LookupModOwner(LogisticsData::instance->getCampaignName().c_str());
	purchaseGroups = mc2purchase::group(catalog, campaignOwner ? campaignOwner : "");
	purchaseKeyboardGroup = 0;
	purchaseGroupNames.clear();
	for (const auto& group : purchaseGroups)
		purchaseGroupNames.push_back(ModDisplayName(group.source));

	if ( !pSelMech )
	{
		selectFirstBuyableMech();
		pSelMech = purchaseSelection;
	}

	status = RUNNING;
	acceptPressed = 0;

	inventoryListBox.drawCBills(1);
	inventoryListBox.setOrange( 1 );
	
	
	setMech( pSelMech );

	pDragMech = nullptr;

}
//-------------------------------------------------------------------------------------------------

void MechPurchaseScreen::end()
{
	if ( !acceptPressed && status != MAINMENU )
	{ 
		// sell all current stuff
		auto list = LogisticsData::instance->getInventory();
		for ( auto iter = list.rbegin(); iter != list.rend(); ++iter )
		{
			LogisticsData::instance->sellMech( (*iter) );
		}


		unsigned long base, color1, color2;
		for (auto & prevInventoryElem : prevInventory)
		{
			prevInventoryElem.getColors(base, color1, color2);
			LogisticsData::instance->addMechToInventory( prevInventoryElem.getVariant(), prevInventoryElem.getPilot(),
				prevInventoryElem.getForceGroup(), true );
		}

	}

	inventoryListBox.removeAllItems(true);

	mechDisplay.setMech( nullptr );
	purchaseSelection = nullptr;
	purchaseMechs.clear();
	purchaseGroups.clear();
	purchaseGroupNames.clear();
	purchaseKeyboardGroup = 0;
	pDragMech = nullptr;
}

//-------------------------------------------------------------------------------------------------

void MechPurchaseScreen::setMech( LogisticsMech* pMech, bool FromLB )
{
	if ( status == RUNNING )
	{
		if ( FromLB ) // clicked on a disabled item
		{
			if ( !flashTime )
				flashTime = .000001f;
		}
		
		if ( FromLB ) // clicked on a disabled item
		{
			inventoryListBox.SelectItem( -1 );
			purchaseSelection = nullptr;

		}
		mechDisplay.setMech( pMech );		

	}
}

//-------------------------------------------------------------------------------------------------

void MechPurchaseScreen::beginDrag( LogisticsMech* pMech )
{
	if ( status != RUNNING )
		return;
	
	if ( !pDragMech )
	{
		pDragMech = pMech;
	}

	MechListBox::initIcon( pMech, dragIcon );

	if ( inventoryListBox.pointInside( userInput->getMouseDragX(),
		userInput->getMouseDragY() ) )
	{
		dragStartLeft = true;
		inventoryListBox.dimItem( pMech, true );
	}
	else 
	{
		dragStartLeft = false;
	}
}

void MechPurchaseScreen::endDrag( )
{
	if ( pDragMech )
	{
	
		if ( inventoryListBox.pointInside( userInput->getMouseX(),
			userInput->getMouseY() ) )
		{
			if ( !dragStartLeft )
			{
				addMech( pDragMech );
			}
		}
		else
		{
			if ( dragStartLeft )
				removeMech( pDragMech );
		}
	}

	inventoryListBox.undimAll();

	pDragMech = nullptr;
}


//-------------------------------------------------------------------------------------------------

int	MechPurchaseScreen::handleMessage( unsigned long what, unsigned long who )
{
	if ( status == RUNNING )
	{
		switch (who)
		{
			case MB_MSG_ADD:
				if ( INVENTORY_ID == what )
				{
					int index = inventoryListBox.GetSelectedItem();
					if ( index != -1 )
					{
						aListItem* pItem = inventoryListBox.GetItem( index );
						if ( pItem )
						{
							LogisticsMech* pMech = (static_cast<MechListBoxItem*>(pItem))->getMech();
							int oldCount = LogisticsData::instance->getVariantsInInventory( pMech->getVariant(), true );
							if ( LogisticsData::instance->sellMech( pMech ) && ( oldCount < 2 ) )
								(static_cast<MechListBoxItem*>(pItem))->resetMech( );
							soundSystem->playDigitalSample( LOG_SELECT );		
						}                                    
					}
				}

				else
					addSelectedMech();
					soundSystem->playDigitalSample( LOG_SELECT );		
				break;
			case MB_MSG_REMOVE:
				removeSelectedMech();
				break;
			case MB_MSG_NEXT:
				acceptPressed = true;
				status = DOWN;
				break;
			case MB_MSG_PREV:
				status = DOWN;
				break;
			case MB_MSG_MAINMENU:
				status = MAINMENU;
				break;
		}
	}

	return 0;

}

//-------------------------------------------------------------------------------------------------

void MechPurchaseScreen::addSelectedMech()
{
	LogisticsMech* pMech = purchaseSelection;
	if ( pMech )
	{
		addMech( pMech );
	}


}

void MechPurchaseScreen::addMech( LogisticsMech* pMech )
{
	if (!LogisticsData::instance->purchaseMech(pMech->getVariant()))
	{
		flashTime = .0001f;
		return;
	}

		for ( int i = 0; i < inventoryListBox.GetItemCount(); i++ )
		{
			if ( (static_cast<MechListBoxItem*>(inventoryListBox.GetItem(i)))->getMech()->getVariant()
				== pMech->getVariant() )
			{
				return;
			}
		}

		MechListBoxItem* pItem = new MechListBoxItem( pMech, 1 );
		inventoryListBox.AddItem( pItem );
}

//-------------------------------------------------------------------------------------------------

void MechPurchaseScreen::removeSelectedMech()
{
	LogisticsMech* pMech = inventoryListBox.getCurrentMech();
	if ( pMech )
	{
		removeMech( pMech );
	}
}

void MechPurchaseScreen::removeMech( LogisticsMech* pMech )
{
	int index = inventoryListBox.GetSelectedItem();
	aListItem* pItem = inventoryListBox.GetItem( index );

	int oldCount = LogisticsData::instance->getVariantsInInventory( pMech->getVariant(), true );
	if ( LogisticsData::instance->sellMech( pMech ) && ( oldCount < 2 ) )
	{
		inventoryListBox.RemoveItem( pItem, true );
	}

	if ( !inventoryListBox.GetItemCount() )
	{
		selectFirstBuyableMech();
	}
	else if ( -1 == inventoryListBox.GetSelectedItem() )
		inventoryListBox.SelectItem( 0 );

}

bool MechPurchaseScreen::selectFirstBuyableMech()
{
	for ( const auto& group : purchaseGroups )
	{
		if (!group.open) continue;
		for (const auto i : mc2purchase::validItems(group, purchaseMechs.size()))
		{
			if (LogisticsData::instance->canPurchaseMech(purchaseMechs[i]->getVariant()))
			{
				purchaseSelection = purchaseMechs[i].get();
				return true;
			}
		}
	}

	return false;
}

void MechPurchaseScreen::renderPurchaseGroups(int xOffset, int yOffset)
{
	if (!ImGui::GetCurrentContext()) return;
	float sx, sy, ox, oy;
	aObject::getCanvasTransform(sx, sy, ox, oy);
	ImGui::SetNextWindowPos(ImVec2((rects[1].left() + xOffset) * sx + ox, (rects[1].top() + yOffset) * sy + oy));
	ImGui::SetNextWindowSize(ImVec2(rects[1].width() * sx, rects[1].height() * sy));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.025f, 0.04f, 0.06f, 1));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.9f, 0.94f, 1));
	ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.18f, 0.32f, 1));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0f, 0.3f, 0.5f, 1));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.4f, 0.6f, 1));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3 * sx, 3 * sy));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	if (ImGui::Begin("##purchase_sources", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse))
	{
		// The font atlas already follows display DPI; scale only relative to the 1080p canvas.
		ImGui::SetWindowFontScale(sy * .75f * 1080.f / ImGui::GetIO().DisplaySize.y);
		ImGui::TextUnformatted("F6: source  Space: open/close\nF7: next mech  F8: buy");
		const auto* keys = input::getKeyboardInfo();
		if (!purchaseGroups.empty()) {
			if (keys->last_state_[SDL_SCANCODE_F6] == static_cast<int>(input::KS_PRESSED))
				purchaseKeyboardGroup = (purchaseKeyboardGroup + 1) % purchaseGroups.size();
			if (keys->last_state_[SDL_SCANCODE_SPACE] == static_cast<int>(input::KS_PRESSED))
				purchaseGroups[purchaseKeyboardGroup].open = !purchaseGroups[purchaseKeyboardGroup].open;
			auto& group = purchaseGroups[purchaseKeyboardGroup];
			const auto valid = mc2purchase::validItems(group, purchaseMechs.size());
			if (keys->last_state_[SDL_SCANCODE_F7] == static_cast<int>(input::KS_PRESSED) && !valid.empty()) {
				auto selected = std::find_if(valid.begin(), valid.end(), [&](auto i) { return purchaseMechs[i].get() == purchaseSelection; });
				if (selected == valid.end() || ++selected == valid.end()) selected = valid.begin();
				group.open = true;
				purchaseSelection = purchaseMechs[*selected].get();
				inventoryListBox.SelectItem(-1); setMech(purchaseSelection);
			}
			if (keys->last_state_[SDL_SCANCODE_F8] == static_cast<int>(input::KS_PRESSED) && purchaseSelection)
				addMech(purchaseSelection);
		}
		for (std::size_t g = 0; g < purchaseGroups.size(); ++g)
		{
			auto& group = purchaseGroups[g];
			ImGui::PushID(group.source.c_str());
			ImGui::SetNextItemOpen(group.open, ImGuiCond_Always);
			const std::string title = std::format("{}{} ({})", g == purchaseKeyboardGroup ? "> " : "", purchaseGroupNames[g], group.items.size());
			group.open = ImGui::CollapsingHeader(title.c_str());
			if (!group.open && purchaseSelection)
				for (const auto i : mc2purchase::validItems(group, purchaseMechs.size()))
					if (purchaseSelection == purchaseMechs[i].get()) { purchaseSelection = nullptr; setMech(nullptr); }
			if (group.open) for (const auto i : mc2purchase::validItems(group, purchaseMechs.size()))
			{
				auto* mech = purchaseMechs[i].get();
				ImGui::PushID(static_cast<int>(i));
				const bool buyable = LogisticsData::instance->canPurchaseMech(mech->getVariant()).has_value();
				const std::string label = std::format("{}\n{:.0f} t   {} C-Bills", mech->getName(), mech->getMaxWeight(), mech->getCost());
				if (!buyable) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1));
				if (ImGui::Selectable(label.c_str(), purchaseSelection == mech, ImGuiSelectableFlags_AllowDoubleClick))
				{
					purchaseSelection = mech;
					inventoryListBox.SelectItem(-1);
					setMech(mech);
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) addMech(mech);
				}
				if (!buyable) ImGui::PopStyleColor();
				if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter)) addMech(mech);
				if (buyable && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) beginDrag(mech);
				ImGui::PopID();
			}
			ImGui::PopID();
		}
	}
	ImGui::End();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(5);
}




//*************************************************************************************************
// end of file ( MechPurchaseScreen.cpp )
