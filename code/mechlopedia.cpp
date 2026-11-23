#include <memory>
#include "mc2fmt.h"
#include "utils/mc2log.h"
#define MECHLOPEDIA_CPP
/*************************************************************************************************\
Mechlopedia.cpp			: Implementation of the Mechlopedia component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include <cstdio>
#include <algorithm>
#include"mechlopedia.h"
#include"mc2str.h"
#include"inifile.h"
#include"mclib.h"
#include"logisticsdata.h"
#include"unitprofile.h"   // EQUIP-ADD-UI-NAME-1 (S3): equipmentAddUiTextForStringId
#include"mission.h"   // ENCYCLO-LAZYLOAD-1: initTGLForLogistics
#include <cstdlib>
#include"platform_str.h"   // S_stricmp (encyclopedia_<tab> parse)
#include"../resource.h"
#include"resource_messages.h"
#include"prefs.h"
#include"cmponent.h"
#include"../GuiRuntime/GuiRuntime.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "unit_display_name.h"

constexpr int ENCYCLO_MECHS = 130;
constexpr int ENCYCLO_BUILD = 131;
constexpr int ENCYCLO_VEHIC = 132;
constexpr int ENCYCLO_WEAPONS = 133;
constexpr int ENCYCLO_PILOTS = 134;
constexpr int ENCYCLO_HISTORY = 135;
constexpr int ENCYCLO_MM = 136;

constexpr int PERSONALITY_COUNT = 11;

MechlopediaListItem* MechlopediaListItem::s_templateItem = nullptr;				




Mechlopedia::Mechlopedia(  )
{
	helpTextArrayID = 1;

	for (auto & subScreen : subScreens)
	{
		subScreen = nullptr;
	}

	currentScreen = 0;
}

//-------------------------------------------------------------------------------------------------

Mechlopedia::~Mechlopedia()
{
}



int Mechlopedia::init()
{
	FullPathFileName path;
	path.init( artPath, "mcl_en", ".fit" );

	FitIniFile file;
	if ( NO_ERR != file.open( path ) )
	{
		char errorStr[256];
		(*std::format_to_n( errorStr, sizeof(errorStr) - 1, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(path)) ).out = '\0');
		Assert( 0, 0, errorStr );
		return 0;
	}

	LogisticsScreen::init( file, "Static", "Text", "Rect", "Button" );
	defsHelpTextKey = "game.mcl_en.text.help_text";

	buttons[buttonCount-1].setMessageOnRelease();

	listBox.init( rects[0].left(), rects[0].top(), rects[0].width(), rects[0].height() );
	listBox.setOrange(true);

	Mechlopedia::MechScreen* pMechScreen = new Mechlopedia::MechScreen;
	pMechScreen->setListBox( &listBox );
	pMechScreen->setVehicle( false );
	pMechScreen->init();
	subScreens[0] = pMechScreen;
	
	Mechlopedia::BuildingScreen* pBuildingScreen = new Mechlopedia::BuildingScreen;
	pBuildingScreen->setListBox( &listBox );
	pBuildingScreen->init();
	subScreens[1] = pBuildingScreen;
	
	Mechlopedia::WeaponScreen* pWeaponScreen = new Mechlopedia::WeaponScreen;
	pWeaponScreen->setListBox( &listBox );
	pWeaponScreen->init();
	subScreens[3] = pWeaponScreen;

	pMechScreen = new Mechlopedia::MechScreen;
	pMechScreen->setVehicle( true );
	pMechScreen->setListBox( &listBox );
	pMechScreen->init();
	subScreens[2] = pMechScreen;

	Mechlopedia::PersonalityScreen* pPersScreen = new Mechlopedia::PersonalityScreen;
	pPersScreen->setListBox( &listBox );
	pPersScreen->init();
	pPersScreen->setIsHistory( false );
	subScreens[4] = pPersScreen;

	Mechlopedia::PersonalityScreen* pHistroyScreen = new Mechlopedia::PersonalityScreen;
	pHistroyScreen->setListBox( &listBox );
	pHistroyScreen->init();
	pHistroyScreen->setIsHistory( true );
	subScreens[5] = pHistroyScreen;

	
	MechlopediaListItem::init();

	for ( int i = ENCYCLO_MECHS; i < ENCYCLO_MM; i++ )
	{
		getButton( i )->setPressFX( LOG_VIDEOBUTTONS );
		getButton( i )->setHighlightFX( LOG_DIGITALHIGHLIGHT );
		getButton( i )->setDisabledFX( -1 );
	}

	return true;
}


// ENCYCLO-UNDEFINED-FILTER-1: data rows without a resolved display name come
// through as "undefined" (missing string ids / placeholder variants); hide
// them from every encyclopedia list.
static bool mc2IsUndefinedName( mc2::czstring n )
{
	return !mc2::names::usable( std::string_view( mc2::str::from_c( n ) ) );
}

// ENCYCLO-UNDEFINED-FILTER-2: mod content can carry a VALID display name but a
// placeholder encyclopedia entry — the description string resolves to
// "undefined". Hide those rows too (a list entry whose page is just
// "undefined" is noise).
static bool mc2IsUndefinedDescription( int descID )
{
	const std::string desc = loadResourceString( descID );
	return mc2IsUndefinedName( desc.c_str() );
}

int			Mechlopedia::handleMessage( unsigned long, unsigned long who)
{
	// unpress all the others
	for ( int i = ENCYCLO_MECHS; i < ENCYCLO_MM; i++ )
		getButton( i )->press( 0 );

	if ( getButton( who ) )
		getButton( who )->press( true );

	if ( who < ENCYCLO_MM )
	{
		if ( subScreens[currentScreen] )
			subScreens[currentScreen]->end();

		currentScreen = who - ENCYCLO_MECHS;
		if ( subScreens[currentScreen] )
			subScreens[currentScreen]->begin();

		syncEntityList();
	}

	else 
	{
		for (auto & subScreen : subScreens)
		{
			subScreen->end();
		}
		beginFadeOut( .5f );
		status = NEXT;
	}

	return 1;
}

void Mechlopedia::update()
{
	LogisticsScreen::update();

	if ( subScreens[currentScreen] )
		subScreens[currentScreen]->update();

	if ( hasDefsUiPage() )
	{
		int newSel = getDefsListSelection( "game.mcl_en.list.entity_selection" );
		if ( newSel >= 0 && newSel != listBox.GetSelectedItem() )
		{
			listBox.SelectItem( newSel );
			aTextListItem* pItem = static_cast<aTextListItem*>(listBox.GetItem( newSel ));
			if ( pItem && subScreens[currentScreen] )
				static_cast<SubScreen*>(subScreens[currentScreen])->select( pItem );
		}
	}
}

void Mechlopedia::render()
{
	GUI_RECT rect = { 0, 0, Environment.screenWidth, Environment.screenHeight };
	drawRect( rect, 0xff000000 );

	if ( subScreens[currentScreen] )
		subScreens[currentScreen]->render();
	
	LogisticsScreen::render();	
}


//////////////////////////////////////////////////////////


int Mechlopedia::SubScreen::init( FitIniFile& file )
{
	LogisticsScreen::init( file, "Static", "Text", "Rect", "Button" );

	for ( int i = 0; i < buttonCount; i++ )
		buttons[i].setMessageOnRelease();

	descriptionListBox.init( rects[0].left(), rects[0].top(), rects[0].width(), rects[0].height() );
	descriptionListBox.move( 285, 58 );
	descriptionListBox.setDisabledFX( -1 );
	descriptionListBox.setHighlightFX( -1 );
	descriptionListBox.setPressFX( -1 );

	return 0;

}

void Mechlopedia::SubScreen::end(  )
{
	camera.setMech( nullptr );
}


void Mechlopedia::SubScreen::update()
{
	if ( !hasDefsUiPage() )
	{
		int mouseX = userInput->getMouseX();
		int mouseY = userInput->getMouseY();

		// check for new selection....
		groupListBox->update();
		if ( groupListBox->pointInside( mouseX, mouseY) && userInput->isLeftClick() )
		{
			int index = groupListBox->GetSelectedItem();
			if ( index != -1 )
			{
			//	for ( int i = 0; i < groupListBox->GetItemCount(); i++ )
			//	{
			//		groupListBox->GetItem( i )->setColor( 0xff43311C );
			//	}

				aTextListItem* pItem = static_cast<aTextListItem*>(groupListBox->GetItem( index ));
			//	pItem->setColor( 0xff866234 );
				select( pItem );

			}
		}
	}

	if ( !hasDefsUiPage() )
		descriptionListBox.update();
	camera.update();

}

void Mechlopedia::syncEntityList()
{
	if ( !hasDefsUiPage() ) return;

	std::vector<std::string> items;
	items.reserve( listBox.GetItemCount() );
	for ( int i = 0; i < listBox.GetItemCount(); i++ )
	{
		aTextListItem* pItem = static_cast<aTextListItem*>(listBox.GetItem( i ));
		if ( pItem ) items.push_back( pItem->getText() );
	}
	const bool ok = setDefsListItems( "game.mcl_en.list.entity_selection", items );

	// ENCYCLO-LIST-1 diagnostic: the list can be empty for two very different
	// reasons — legacy listBox never populated (data side) vs the defs GuiList
	// key not found (mirror side). Log both counts to tell them apart.
	if ( mc2::gate::get<"MC2_LOG_PREVIEW">() )
	{
		mc2::fmt::print_out("[ENCYCLO] syncEntityList screen={:d} legacyItems={:d} mirrorOk={:d}\n",
			mc2::fmt::as_long(currentScreen), mc2::fmt::as_i32(static_cast<int>(items.size())), mc2::fmt::as_i32(static_cast<int>(ok)));
		fflush(stdout);
	}

	int sel = listBox.GetSelectedItem();
	setDefsListSelection( "game.mcl_en.list.entity_selection", sel >= 0 ? sel : 0 );
}

void Mechlopedia::begin()
{
	// ENCYCLO-LAZYLOAD-1: on a fresh boot the smart-load path defers the
	// mech/variant scan and TGL pool init until a campaign/mission needs
	// them — entering the Encyclopedia straight from the main menu then had
	// NO variants (empty lists) and no shape pools (pObject=null, blank 3D
	// preview; confirmed via [PREVIEW] log). Both calls are guarded/
	// idempotent: LogisticsData::init() early-returns after first run,
	// initTGLForLogistics is what every MissionBegin::begin already calls.
	LogisticsData::instance->init();
	Mission::initTGLForLogistics();

	beginFadeIn( 2.0f );
	status = RUNNING;

	// MC2_BOOT_TO_SCREEN=encyclopedia_<tab> harness sub-target: land on a
	// specific encyclopedia tab with no clicks (weapons/vehicles/buildings).
	long bootTab = 0;
	const std::string bootScrText = mc2::gate::str<"MC2_BOOT_TO_SCREEN">();   // "" when unset or empty
	const char* bootScr = bootScrText.c_str();
	if ( bootScr[0] )
	{
		if      ( !S_stricmp(bootScr, "encyclopedia_vehicles") )  bootTab = ENCYCLO_VEHIC;
		else if ( !S_stricmp(bootScr, "encyclopedia_buildings") ) bootTab = ENCYCLO_BUILD;
		else if ( !S_stricmp(bootScr, "encyclopedia_weapons") )   bootTab = ENCYCLO_WEAPONS;
		else if ( !S_stricmp(bootScr, "encyclopedia_pilots") )    bootTab = ENCYCLO_PILOTS;
	}

	if ( bootTab )
	{
		getButton( bootTab )->press( true );
		handleMessage( 0, bootTab );
		listBox.setScrollPos( 0 );
	}
	else if ( !currentScreen )
	{
		getButton( ENCYCLO_MECHS )->press( true );
		handleMessage( 0, ENCYCLO_MECHS );

		listBox.setScrollPos( 0 );
	}
}

//////////////////////////////////////////////////////////
void Mechlopedia::MechScreen::init()
{
	FullPathFileName path;
	path.init( artPath, "mcl_en_mechs", ".fit" );

	if ( tryInitDefsOnly( path ) )
	{
		// Camera bounds from mcl_en_mechs.fit Gui3DView:
		//   rect = "12,43,154,191"  mountOffset = "285,58"
		float sx = (Environment.screenWidth > 0) ? static_cast<float>(Environment.screenWidth) / 800.f : 1.f;
		float sy = (Environment.screenHeight > 0) ? static_cast<float>(Environment.screenHeight) / 600.f : 1.f;
		if ( mc2::gate::get<"MC2_LOG_PREVIEW">() )
		{
			bool f = mc2::log::enabled(mc2::log::Cat::Ui, mc2::log::Level::Info);
			if (f) { MC2_LOG(Ui, Info, "[PREVIEW] MechScreen::init DEFS-BRANCH screenW={:d} screenH={:d} sx={:.4f} sy={:.4f} bounds=[{:.1f},{:.1f},{:.1f},{:.1f}]", mc2::fmt::as_i32(Environment.screenWidth), mc2::fmt::as_i32(Environment.screenHeight), sx, sy, (12+285)*sx, (43+58)*sy, (12+154+285)*sx, (43+191+58)*sy);   }
		}
		camera.init( (12 + 285) * sx, (43 + 58) * sy,
		             (12 + 154 + 285) * sx, (43 + 191 + 58) * sy );
		return;
	}

	FitIniFile file;
	if ( NO_ERR != file.open( path ) )
	{
		char errorStr[256];
		(*std::format_to_n( errorStr, sizeof(errorStr) - 1, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(path)) ).out = '\0');
		Assert( 0, 0, errorStr );
		return;
	}

	SubScreen::init( file );

	compListBox.init( rects[2].left(), rects[2].top(), rects[2].width(), rects[2].height() );
	compListBox.move( 285, 58 );
	statsListBox.init( rects[1].left(), rects[1].top(), rects[1].width(), rects[1].height() );
	statsListBox.move( 285, 58 );

	statsListBox.setDisabledFX( -1 );
	statsListBox.setHighlightFX( -1 );
	statsListBox.setPressFX( -1 );

	// MERGE-CONFLICT-UI-PHASE1: THEIRS wraps the legacy camera.init() call with a
	// screen-scale factor (Environment.screenWidth/800, .../600) here, applied
	// consistently to every SubScreen::init() camera setup in this file. OURS has
	// no notion of this scaling in the legacy (non-defs) camera path elsewhere in
	// this file. Kept THEIRS' scaled form since it's applied uniformly across all
	// four screens' init() and is orthogonal to OURS' engine-side mech-scale fix
	// in MechScreen::setMech() below (that fix operates on camera.setScale(), a
	// different call than camera.init()'s pixel-rect bounds here).
	{
		float sx = (Environment.screenWidth > 0) ? static_cast<float>(Environment.screenWidth) / 800.f : 1.f;
		float sy = (Environment.screenHeight > 0) ? static_cast<float>(Environment.screenHeight) / 600.f : 1.f;
		camera.init( (statics[4].left() + 285) * sx, (statics[4].top() + 58) * sy,
		             (statics[4].right() + 285) * sx, (statics[4].bottom() + 58) * sy );
	}
	statics[4].setColor( 0 );
	textObjects[0].setText( "" );
}

void Mechlopedia::MechScreen::begin()
{
	// need to fill that list box
	if ( bIsVehicle )
	{
		if ( !hasDefsUiPage() )
			textObjects[1].setText( IDS_VEHICLE_STATS );
		int count = 256;
		const LogisticsVehicle* pVehicles[256];
		LogisticsData::instance->getVehicles( pVehicles, count );
		
		const LogisticsVariant* pCopters[256];
		int copterCount = 256;
		LogisticsData::instance->getHelicopters( pCopters, copterCount );

		for ( int i = 1; i < count; ++i )
		{
			const LogisticsVehicle* cur = pVehicles[i];
			for ( int j = 0; j < i; ++j )
			{
				if ( cur->getNameID() == pVehicles[j]->getNameID() && j != i )
				{
					pVehicles[i] = pVehicles[j];
					pVehicles[j] = cur;
					break;
				}
			}
		}

		

		

		groupListBox->removeAllItems( true );
		for (int i = 0; i < count; i++ )
		{
			auto pEntry = std::make_unique<MechlopediaListItem>();
			const std::string name = pVehicles[i]->getDisplayName();
			if ( mc2IsUndefinedName( name.c_str() ) ) continue;
			if ( mc2IsUndefinedDescription( pVehicles[i]->getEncyclopediaID() ) ) continue;   // ENCYCLO-UNDEFINED-FILTER-2
			pEntry->setText( mc2::str::ascii_upper( name ) );
			pEntry->resize( groupListBox->width() - groupListBox->getScrollBarWidth() - 18, pEntry->height() );
			bool bFound = 0;

			for ( int j = 0; j < groupListBox->GetItemCount(); j++ )
			{
				aTextListItem* pItem = static_cast<aTextListItem*>(groupListBox->GetItem( j ));
				if ( S_stricmp( name.c_str(), pItem->getText() ) < 0 )
				{
					groupListBox->InsertItem( pEntry.release(), j );
					bFound = true;
					break;
				}
			}
			if ( !bFound )
				groupListBox->AddItem( pEntry.release() );
		}
		for (int i = 0; i < copterCount; i++ )
		{
			if ( mc2IsUndefinedName( pCopters[i]->getName().c_str() ) ) continue;
			if ( mc2IsUndefinedDescription( pCopters[i]->getEncyclopediaID() ) ) continue;   // ENCYCLO-UNDEFINED-FILTER-2
			auto pEntry = std::make_unique<MechlopediaListItem>();
			pEntry->setText( mc2::str::ascii_upper( pCopters[i]->getName() ) );
			pEntry->resize( groupListBox->width() - groupListBox->getScrollBarWidth() - 18,
				pEntry->height() );

			bool bFound = 0;
			for ( int j = 0; j < groupListBox->GetItemCount(); j++ )
			{
				aTextListItem* pItem = static_cast<aTextListItem*>(groupListBox->GetItem( j ));
				if ( S_stricmp( pCopters[i]->getName().c_str(), pItem->getText() ) < 0 )
				{
					groupListBox->InsertItem( pEntry.release(), j );
					bFound = true;
					break;
				}
			}
			if ( !bFound )
				groupListBox->AddItem( pEntry.release() );

		}
	}
	else
	{
		if ( !hasDefsUiPage() )
			textObjects[1].setText( IDS_MECH_STATS );
		int count = 256;
		const LogisticsVariant* pChassis[256];
		LogisticsData::instance->getEncyclopediaMechs( pChassis, count );

		for ( int i = 1; i < count; ++i )
		{
			const LogisticsVariant* cur = pChassis[i];
			for ( int j = 0; j <= i; ++j )
			{
				if ( S_stricmp( cur->getName().c_str(), pChassis[j]->getName().c_str() ) < 0  )
				{
					for ( int l = i-1; l >= j; l-- )
					{
						pChassis[l+1] = pChassis[l];
					}
					pChassis[j] = cur;

					break;
				}
			}
		}

		groupListBox->removeAllItems( true );
		for (int i = 0; i < count; i++ )
		{
			// The chassis name is text once the shared resolver has picked it; filter on that.
			const std::string chassisName = pChassis[i]->getChassisDisplayName();
			if ( mc2IsUndefinedName( chassisName.c_str() ) ) continue;
			auto pEntry = std::make_unique<MechlopediaListItem>();
			pEntry->setText( chassisName.c_str() );
			pEntry->resize( groupListBox->width() - groupListBox->getScrollBarWidth() - 18,
				pEntry->height() );
			groupListBox->AddItem( pEntry.release() );
		}
	}

	aTextListItem* pEntry = static_cast<aTextListItem*>(groupListBox->GetItem( 0 ));
	if ( pEntry )
	{
		select( pEntry );
		groupListBox->SelectItem( 0 );
		pEntry->setColor( 0xff866234 );
	}

	groupListBox->setScrollPos( 0 );
} 

void Mechlopedia::MechScreen::update()
{
	SubScreen::update();

	if ( !hasDefsUiPage() )
	{
		compListBox.update();
		statsListBox.update();
	}
}

void Mechlopedia::MechScreen::select( aTextListItem* pItem )
{
	const char* pText = pItem->getText();


	if ( !bIsVehicle )
	{
		const std::string name = mc2::str::from_c( pText ) + " Prime";

		LogisticsVariant* pChassis  = LogisticsData::instance->getVariant( name.c_str() ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
		// ENCYCLO-3D-1 diagnostic: is the select→variant→camera chain intact?
		if ( mc2::gate::get<"MC2_LOG_PREVIEW">() )
		{
			if ( bool f = mc2::log::enabled(mc2::log::Cat::Ui, mc2::log::Level::Info) )
			{
				MC2_LOG(Ui, Info, "[ENCYCLO] select mech text='{}' lookup='{}' variant={:016X}", mc2::fmt::safe(pText), name.c_str(), mc2::fmt::as_ptr(static_cast<void*>(pChassis)));

			}
		}
		setMech( pChassis, 1 );
	}
	else
	{
		LogisticsVehicle* pVehicle = LogisticsData::instance->getVehicle( pText ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
		if ( pVehicle )
			setVehicle( pVehicle );
		else // copter
		{
			LogisticsVariant* pMech = LogisticsData::instance->getVariant( pText ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
			if ( pMech )
				setMech( pMech, 0 );
		}
	}
}
void Mechlopedia::MechScreen::render()
{
	// MERGE-CONFLICT-UI-PHASE1: OURS (PREVIEW-FIX) reordered this function so
	// camera.render() runs LAST, after LogisticsScreen::render(285,58) -- the 2D
	// GUI primitives draw with ZCompare/ZWrite off, so anything drawn after the
	// mech preview in an overlapping region overwrites its pixels, producing a
	// blank preview. THEIRS instead added a hasDefsUiPage() branch: in the
	// legacy (non-defs) path it renders camera.render() BEFORE
	// LogisticsScreen::render (the original, pre-PREVIEW-FIX order), and in the
	// defs/ImGui path it defers camera.render() to GuiRuntime::PostImGuiRender
	// so it draws after the ImGui frame. Resolution: keep OURS' legacy-path fix
	// (camera.render() last) since that's a confirmed blank-preview bugfix and
	// this is the legacy (non-defs) render path THEIRS' own comment describes
	// as already correct in WeaponScreen/BuildingScreen; keep THEIRS' defs-page
	// branch verbatim since it is an independent addition (ImGui page active)
	// that doesn't touch the legacy ordering at all. Also dropped THEIRS' stray
	// one-shot fprintf diagnostic block that was in this function -- it's debug
	// scaffolding, not feature code, and doesn't belong in the merged result.
	if ( !hasDefsUiPage() )
	{
		groupListBox->render();
		descriptionListBox.render();
		statsListBox.render();
		compListBox.render();

		LogisticsScreen::render(285, 58);

		camera.setPreviewOffscreen( false );
		camera.render();
	}
	else
	{
		// PREVIEW-FBO-FIXED-800x600-1: renders into its own fixed 800x600
		// offscreen texture (setPreviewOffscreen), so it no longer needs to be
		// deferred past the ImGui frame to avoid clobbering it -- draw +
		// composite inline, same as any other ImGui widget this frame.
		camera.setPreviewOffscreen( true );
		camera.render();
		float px = 0, py = 0, pw = 0, ph = 0;
		if ( getDefsElementScreenRect( "game.mcl_en_mechs.3dview.walking_mech_should_be_centered_here", px, py, pw, ph ) )
			camera.drawPreviewToPanel( px, py, pw, ph );
		LogisticsScreen::render(285, 58);
	}
}

namespace {

// Legacy font controls are invisible in the original renderer; defs pages need
// plain text with the original line breaks.
void cleanLegacyDescription( std::string& text )
{
	std::erase( text, '\\' );
	std::replace( text.begin(), text.end(), '/', '\n' );
}

} // namespace

void Mechlopedia::MechScreen::setVehicle( LogisticsVehicle* pVehicle )
{
	if ( !pVehicle )
		return;

	int descID = pVehicle->getEncyclopediaID();
	char text[256];
	cLoadString( pVehicle->getNameID(), text, 255 );
	const std::string tmpStr = mc2::str::ascii_upper( text );
	std::unique_ptr<aTextListItem> pItem;
	if ( !hasDefsUiPage() )
	{
		descriptionListBox.removeAllItems( true );

		pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );
		pItem->forceToTop( true );
		pItem->resize( descriptionListBox.width() - descriptionListBox.getScrollBarWidth() - 16, pItem->height() );
		pItem->setText( loadResourceString( descID ) );
		pItem->sizeToText( );
		pItem->setColor( 0xff005392 );

		descriptionListBox.AddItem( pItem.release() );

		compListBox.setVehicle( pVehicle );
		textObjects[0].setText( tmpStr );
	}

	camera.setVehicle( pVehicle->getFileName().c_str(), prefs.baseColor, prefs.highlightColor, prefs.highlightColor );
	camera.setScale( pVehicle->getScale() );

	statsListBox.removeAllItems( true );

	std::string formatText;

	long color = (textCount > 0) ? textObjects[0].getColor() : 0xffffffff;
	//	pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );
	// add house stats NO HOUSE FOR VEHICLES
//	long houseID = pVehicle->getHouseID();
//	cLoadString( IDS_HOUSE0 + houseID, tmp, 255 );
//	cLoadString( IDS_EN_HOUSE, text, 255 );
//	sprintf( formatText, text, tmp );

//	pItem->setText( formatText );
//	pItem->setColor( color );
//	statsListBox.AddItem( pItem );

	pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );
	// add weight stats
	formatText = mc2msg::message<IDS_EN_WEIGHT>( pVehicle->getMaxWeight() );
	
	pItem->setText( formatText );
	pItem->setColor( color );
	statsListBox.AddItem( pItem.release() );

	// add weight class stats
	pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );

	formatText = mc2msg::message<IDS_EN_CLASS>( pVehicle->getMechClass() );
	
	pItem->setText( formatText );
	pItem->setColor( color );
	statsListBox.AddItem( pItem.release() );

	// now armor
	pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );

	formatText = mc2msg::message<IDS_EN_ARMOR>( loadResourceString( pVehicle->getArmorClass() ), static_cast<long>(pVehicle->getArmor()) );
	
	pItem->setText( formatText );
	pItem->setColor( color );
	statsListBox.AddItem( pItem.release() );


	// now speed
	pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );

	formatText = mc2msg::message<IDS_EN_SPEED>( static_cast<long>(pVehicle->getDisplaySpeed().value_or(-1)) );

	pItem->setText( formatText );
	pItem->setColor( color );
	statsListBox.AddItem( pItem.release() );

	if ( hasDefsUiPage() )
	{
		std::string descText = loadResourceString( descID );
		cleanLegacyDescription( descText );
		setDefsElementText( "game.mcl_en_mechs.text.description", descText );

		std::vector<std::string> statsItems;
		for ( int i = 0; i < statsListBox.GetItemCount(); i++ )
		{
			aTextListItem* pStat = static_cast<aTextListItem*>(statsListBox.GetItem( i ));
			if ( pStat ) statsItems.push_back( pStat->getText() );
		}
		setDefsListItems( "game.mcl_en_mechs.list.stats", statsItems );

		compListBox.setVehicle( pVehicle );
		syncWeaponLoadout( this, compListBox, "game.mcl_en_mechs.list.weapons" );
	}

}

void Mechlopedia::MechScreen::setMech( LogisticsVariant* pChassis, bool bShowJump )
{
	if ( !pChassis )
		return;

	int descID = pChassis->getEncyclopediaID();

	const std::string name = pChassis->getChassisDisplayName();
	const std::string upper = mc2::str::ascii_upper( name );

	std::unique_ptr<aTextListItem> pItem;
	if ( !hasDefsUiPage() )
	{
		descriptionListBox.removeAllItems( true );

		pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );
		pItem->resize( descriptionListBox.width()  - descriptionListBox.getScrollBarWidth()- 16, pItem->height() );
		pItem->setText( loadResourceString( descID ) );
		pItem->sizeToText( );
		pItem->setColor( 0xff005392 );

		pItem->forceToTop( true );
		descriptionListBox.AddItem( pItem.release() );

		compListBox.setMech( pChassis );
		textObjects[0].setText( upper  );
	}

	camera.setMech( pChassis->getFileName().c_str(), prefs.baseColor, prefs.highlightColor, prefs.highlightColor );
	// PREVIEW-FRAMING-FIX: the Mech Bay (mechbayscreen.cpp:777) follows setMech()
	// with setScale(chassis scale). The encyclopedia omitted it for mechs, so
	// shapeScale stayed 0.0f -> SimpleCamera::update() called pObject->scale(0)
	// -> TG node shapeScalar=0 -> the per-vertex transform (tgl.cpp:1810,
	// `if (shapeScalar > 0.0f) pos *= shapeScalar;`) left the model at its raw
	// native size instead of the ~1.0 chassis scale that AltitudeTight=650 frames
	// for. The mech drew (363 mech3d draws confirmed) but unframed/oversized in
	// the tiny preview rect -> "blank". Match the bay so the model is framed.
	// MERGE-CONFLICT-UI-PHASE1: this fix (OURS, engine-side) is unconditional --
	// applies regardless of hasDefsUiPage() -- since the 3D camera/model framing
	// is shared by both the legacy listbox UI and the ImGui defs UI (both read
	// from the same SimpleCamera). THEIRS did not touch this call at all; no
	// actual collision, kept as-is.
	camera.setScale( pChassis->getChassis()->getScale() );
	// ENCYCLO-3D-2 diagnostic: is the chassis scale real here on fresh boot?
	if ( mc2::gate::get<"MC2_LOG_PREVIEW">() )
	{
		if ( bool f = mc2::log::enabled(mc2::log::Cat::Ui, mc2::log::Level::Info) )
		{
			MC2_LOG(Ui, Info, "[ENCYCLO] setMech '{}' chassisScale={:.3f} file={}", mc2::fmt::safe(name.c_str()), pChassis->getChassis()->getScale(), pChassis->getFileName().c_str());

		}
	}

	statsListBox.removeAllItems( true );

	std::string formatText;

	pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );

	long color = (textCount > 0) ? textObjects[0].getColor() : 0xffffffff;

	// add house stats
	if ( !bIsVehicle )
	{
		long houseID = pChassis->getHouseID();
		formatText = mc2msg::message<IDS_EN_HOUSE>( loadResourceString( IDS_HOUSE0 + houseID ) );

		pItem->setText( formatText );
		pItem->setColor( color );
		statsListBox.AddItem( pItem.release() );
	}

	pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );
	// add weight stats
	formatText = mc2msg::message<IDS_EN_WEIGHT>( pChassis->getMaxWeight() );
	
	pItem->setText( formatText );
	pItem->setColor( color );
	statsListBox.AddItem( pItem.release() );

	// add weight class stats
	pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );

	formatText = mc2msg::message<IDS_EN_CLASS>( pChassis->getMechClass() );
	
	pItem->setText( formatText );
	pItem->setColor( color );
	statsListBox.AddItem( pItem.release() );

	// now armor
	pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );

	formatText = mc2msg::message<IDS_EN_ARMOR>( loadResourceString( pChassis->getArmorClass() ), static_cast<long>(pChassis->getArmor()) );
	
	pItem->setText( formatText );
	pItem->setColor( color );
	statsListBox.AddItem( pItem.release() );


	// now speed
	pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );

	formatText = mc2msg::message<IDS_EN_SPEED>( static_cast<long>(pChassis->getDisplaySpeed().value_or(-1)) );
	
	pItem->setText( formatText );
	pItem->setColor( color );
	statsListBox.AddItem( pItem.release() );

	// now jump range
	if ( bShowJump )
	{
		pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );

		formatText = mc2msg::message<IDS_EN_JUMP>( static_cast<long>(pChassis->getJumpRange()) * 25 );
		
		pItem->setText( formatText );
		pItem->setColor( color );
		statsListBox.AddItem( pItem.release() );
	}

	if ( hasDefsUiPage() )
	{
		std::string descText = loadResourceString( descID );
		cleanLegacyDescription( descText );
		setDefsElementText( "game.mcl_en_mechs.text.description", descText );

		std::vector<std::string> statsItems;
		for ( int i = 0; i < statsListBox.GetItemCount(); i++ )
		{
			aTextListItem* pStat = static_cast<aTextListItem*>(statsListBox.GetItem( i ));
			if ( pStat ) statsItems.push_back( pStat->getText() );
		}
		setDefsListItems( "game.mcl_en_mechs.list.stats", statsItems );

		compListBox.setMech( pChassis );
		syncWeaponLoadout( this, compListBox, "game.mcl_en_mechs.list.weapons" );
	}

}


//////////////////////////////////////////////////////////
void Mechlopedia::WeaponScreen::init()
{
	FullPathFileName path;
	path.init( artPath, "mcl_en_wep", ".fit" );

	if ( tryInitDefsOnly( path ) )
	{
		// Camera bounds from mcl_en_wep.fit Static4 (GuiImage):
		//   rect = "12,43,260,191"  mountOffset = "285,58"
		float sx = (Environment.screenWidth > 0) ? static_cast<float>(Environment.screenWidth) / 800.f : 1.f;
		float sy = (Environment.screenHeight > 0) ? static_cast<float>(Environment.screenHeight) / 600.f : 1.f;
		camera.init( (12 + 285) * sx, (43 + 58) * sy,
		             (12 + 260 + 285) * sx, (43 + 191 + 58) * sy );
		return;
	}

	FitIniFile file;
	if ( NO_ERR != file.open( path ) )
	{
		char errorStr[256];
		(*std::format_to_n( errorStr, sizeof(errorStr) - 1, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(path)) ).out = '\0');
		Assert( 0, 0, errorStr );
	}

	SubScreen::init( file );

	statsListBox.init( rects[2].left(), rects[2].top(), rects[2].right(), rects[2].bottom() );
	statsListBox.move( 285, 58 );

	{
		float sx = (Environment.screenWidth > 0) ? static_cast<float>(Environment.screenWidth) / 800.f : 1.f;
		float sy = (Environment.screenHeight > 0) ? static_cast<float>(Environment.screenHeight) / 600.f : 1.f;
		camera.init( (statics[4].left() + 285) * sx, (statics[4].top() + 58) * sy,
		             (statics[4].right() + 285) * sx, (statics[4].bottom() + 58) * sy );
	}
	statics[4].setColor( 0 );
	textObjects[0].setText( "" );
}
void Mechlopedia::WeaponScreen::update()
{
	SubScreen::update();

	if ( !hasDefsUiPage() )
		statsListBox.update();
}

void Mechlopedia::WeaponScreen::render()
{
	if ( !hasDefsUiPage() )
	{
		descriptionListBox.render();
		statsListBox.render();
		groupListBox->render();
		camera.setPreviewOffscreen( false );
		camera.render();
	}
	else
	{
		// ENCYCLO-3D-2: the defs page DOES carry the weapon preview slot — as a
		// GuiImage ("...image.rotating_view_of_weapon_is_centered_here"), not a
		// Gui3DView, which is why the old "no 3D panel, draw directly" branch
		// never showed anything (the direct 800x600-space draw lands wrong at
		// real resolutions). Same fixed-800x600-FBO + composite as MechScreen/
		// BuildingScreen; getElementScreenRect returns the true screen rect at
		// any resolution.
		camera.setPreviewOffscreen( true );
		camera.render();
		float px = 0, py = 0, pw = 0, ph = 0;
		if ( getDefsElementScreenRect( "game.mcl_en_wep.image.rotating_view_of_weapon_is_centered_here", px, py, pw, ph ) )
			camera.drawPreviewToPanel( px, py, pw, ph );
	}
	LogisticsScreen::render( 285, 58 );
}

void Mechlopedia::WeaponScreen::select( aTextListItem* pEntry )
{
	LogisticsComponent* pComponent = LogisticsData::instance->getComponent( pEntry->getID() ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
	setWeapon( pComponent );

	
}

int __cdecl sortWeapon( const void* pW1, const void* pW2 )
{
	LogisticsComponent* p1 = *static_cast<LogisticsComponent**>(const_cast<void*>(pW1));
	LogisticsComponent* p2 = *static_cast<LogisticsComponent**>(const_cast<void*>(pW2));

	return S_stricmp( p1->getName(), p2->getName() );

}

void Mechlopedia::WeaponScreen::begin()
{
	groupListBox->removeAllItems( true );

	LogisticsComponent* comps[256];
	int count = 256;
	LogisticsData::instance->getAllComponents( comps, count );

	qsort( comps, count, sizeof( LogisticsComponent* ), sortWeapon );

	for ( int i = 0; i < count; i++ )
	{
		if ( mc2IsUndefinedName( comps[i]->getName() ) ) continue;
		auto pItem = std::make_unique<MechlopediaListItem>();
		pItem->setText( mc2::str::ascii_upper( mc2::str::from_c( comps[i]->getName() ) ) );
		pItem->setID( comps[i]->getID() );
		pItem->resize( groupListBox->width() - groupListBox->getScrollBarWidth() - 18, pItem->height() );

		groupListBox->AddItem( pItem.release() );
	}

	aTextListItem* pEntry = static_cast<aTextListItem*>(groupListBox->GetItem( 0 ));
	if ( pEntry )
	{
		select( pEntry );
		groupListBox->SelectItem( 0 );
		pEntry->setColor( 0xff866234 );
	}

	groupListBox->setScrollPos( 0 );
}
void Mechlopedia::WeaponScreen::setWeapon ( LogisticsComponent* pComponent )
{
	if ( !pComponent )
		return;

	statsListBox.removeAllItems( true );

	//set header
	const std::string name = mc2::str::ascii_upper( mc2::str::from_c( pComponent->getName() ) );

	// set description
	std::unique_ptr<aTextListItem> pEntry;
	if ( !hasDefsUiPage() )
	{
		descriptionListBox.removeAllItems( true );
		textObjects[0].setText( name );

		pEntry = std::make_unique<aTextListItem>( IDS_EN_WEAPON_FONT );
		pEntry->setColor( 0xff005392 );
		// EQUIP-ADD-UI-NAME-1 (S3): ADD-filled component descriptions come
		// from the def, not the string table. Miss -> stock path.
		const char* addDesc = equipmentAddUiTextForStringId( pComponent->getHelpID() );
		if ( addDesc )
			pEntry->setText( addDesc );
		else
			pEntry->setText( loadResourceString( pComponent->getHelpID() ) );
		pEntry->resize( descriptionListBox.width() - descriptionListBox.getScrollBarWidth() - 16, pEntry->height() );
		pEntry->sizeToText();
		pEntry->forceToTop( true );
		descriptionListBox.AddItem( pEntry.release() );
	}

	std::string final;
	long color = (textCount > 0) ? textObjects[0].getColor() : 0xffffffff;

	/*
	RATE OF FIRE
	HEAT
	AMMO (if the Ammo Tracking option is set to on in the Options Screen)
	COST*/

	// set stats
/*	pEntry = new aTextListItem( IDS_EN_WEAPON_FONT );

	cLoadString( IDS_EN_WEAPON_WEIGHT, buffer, 255 );
	sprintf( final, buffer, pComponent->getWeight() );
	pEntry->setText( final );
	pEntry->setColor( color );
	statsListBox.AddItem( pEntry );*/

	// RANGE
	if ( pComponent->isWeapon() )
	{
		pEntry = std::make_unique<aTextListItem>( IDS_EN_WEAPON_FONT );

		final = mc2msg::message<IDS_EN_WEAPON_RANGE>( loadResourceString( IDS_HOTKEY1 + pComponent->getRangeType() ) );
		pEntry->setText( final );
		pEntry->setColor( color );
		statsListBox.AddItem( pEntry.release() );

		//	DAMAGE
		pEntry = std::make_unique<aTextListItem>( IDS_EN_WEAPON_FONT );

		final = mc2msg::message<IDS_EN_WEAPON_DAMAGE>( static_cast<long>(pComponent->getDamage()) );
		pEntry->setText( final );
		pEntry->setColor( color );
		statsListBox.AddItem( pEntry.release() );

	}
	else if ( pComponent->getType() == COMPONENT_FORM_BULK )
	{
		pEntry = std::make_unique<aTextListItem>( IDS_EN_WEAPON_FONT );

		final = mc2msg::message<IDS_EN_WEAPON_ARMOR>( 32 );
		pEntry->setText( final );
		pEntry->setColor( color );
//		pEntry->setHelpID( IDS_EN_WEAPON_ARMOR_HELP );
		statsListBox.AddItem( pEntry.release() );


	}


	// RATE OF FIRE
	if ( pComponent->getRecycleTime() )
	{
		pEntry = std::make_unique<aTextListItem>( IDS_EN_WEAPON_FONT );

		final = mc2msg::message<IDS_EN_WEAPON_RATEOFFIRE>( 10.f/pComponent->getRecycleTime() );
		pEntry->setText( final );
		pEntry->setColor( color );
		statsListBox.AddItem( pEntry.release() );
	}


	// heat
	pEntry = std::make_unique<aTextListItem>( IDS_EN_WEAPON_FONT );

	final = mc2msg::message<IDS_EN_WEAPON_HEAT>( static_cast<long>(pComponent->getHeat()) );
	pEntry->setText( final );
	pEntry->setColor( color );
	statsListBox.AddItem( pEntry.release() );

	// AMMO
	if ( !prefs.useUnlimitedAmmo && pComponent->getAmmo() )
	{
		pEntry = std::make_unique<aTextListItem>( IDS_EN_WEAPON_FONT );

		final = mc2msg::message<IDS_EN_WEAPON_AMMO>( static_cast<long>(pComponent->getAmmo()) );
		pEntry->setText( final );
		pEntry->setColor( color );
		statsListBox.AddItem( pEntry.release() );
	}

	// COST
	pEntry = std::make_unique<aTextListItem>( IDS_EN_WEAPON_FONT );

	final = mc2msg::message<IDS_EN_WEAPON_COST>( static_cast<long>(pComponent->getCost()) );
	pEntry->setText( final );
	pEntry->setColor( color );
	statsListBox.AddItem( pEntry.release() );



	camera.setComponent( pComponent->getPictureFileName() );
	camera.setScale( 1.5 );

	if ( !hasDefsUiPage() )
	{
		FullPathFileName path;
		path.init( artPath, pComponent->getIconFileName(), ".tga" );
		int sizeX = pComponent->getComponentWidth();
		int sizeY = pComponent->getComponentHeight();
		float oldMidX = (rects[1].right() + rects[1].left())/2.f;
		float oldMidY = (rects[1].bottom() + rects[1].top())/2.f;
		statics[9].setTexture( path);
		statics[9].resize( sizeX * LogisticsComponent::XICON_FACTOR, sizeY * LogisticsComponent::YICON_FACTOR);
		statics[9].setUVs( 0.f, 0.f, sizeX * 48.f, sizeY * 32.f );
		statics[9].moveTo( oldMidX - .5 * statics[9].width(), oldMidY - .5 * statics[9].height() );
	}

	if ( hasDefsUiPage() )
	{
		// ADD rows own their description in the equipment definition.
		const char* addDesc = equipmentAddUiTextForStringId( pComponent->getHelpID() );
		std::string descText = addDesc ? std::string( addDesc )
			: loadResourceString( pComponent->getHelpID() );
		cleanLegacyDescription( descText );
		setDefsElementText( "game.mcl_en_wep.text.description", descText );

		std::vector<std::string> statsItems;
		for ( int i = 0; i < statsListBox.GetItemCount(); i++ )
		{
			aTextListItem* pStat = static_cast<aTextListItem*>(statsListBox.GetItem( i ));
			if ( pStat ) statsItems.push_back( pStat->getText() );
		}
		setDefsListItems( "game.mcl_en_wep.list.stats", statsItems );

		// Weapon icon: take the cellW*48 x cellH*32 texel region of the icon
		// sheet, draw it at that pixel size centered in the icon-outline box
		// (mcl_en_wep Rect1 local rect = 10,272,117,180), matching the legacy
		// statics[9] placement.
		FullPathFileName iconPath;
		iconPath.init( artPath, pComponent->getIconFileName(), ".tga" );
		const int cellX = pComponent->getComponentWidth();
		const int cellY = pComponent->getComponentHeight();
		const int iconW = cellX * LogisticsComponent::XICON_FACTOR;
		const int iconH = cellY * LogisticsComponent::YICON_FACTOR;
		const int boxX = 10, boxY = 272, boxW = 117, boxH = 180;
		setDefsElementImageRegion(
			"game.mcl_en_wep.image.static_icon_of_weapon_gets_centered_here",
			static_cast<const char*>(iconPath),
			0, 0, cellX * 48, cellY * 32,
			boxX + ( boxW - iconW ) / 2, boxY + ( boxH - iconH ) / 2, iconW, iconH );
	}

}

//*************************************************************************************************

void Mechlopedia::PersonalityScreen::init()
{
	FullPathFileName path;
	path.init( artPath, "mcl_en_person", ".fit" );

	if ( tryInitDefsOnly( path ) )
		return;

	FitIniFile file;
	if ( NO_ERR != file.open( path ) )
	{
		char errorStr[256];
		(*std::format_to_n( errorStr, sizeof(errorStr) - 1, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(path)) ).out = '\0');
		Assert( 0, 0, errorStr );
		return;
	}

	SubScreen::init( file );
}
void Mechlopedia::PersonalityScreen::update()
{
	if ( !hasDefsUiPage() )
		groupListBox->update();
	SubScreen::update();
}
void Mechlopedia::PersonalityScreen::render()
{
	if ( !hasDefsUiPage() )
	{
		descriptionListBox.render();
		groupListBox->render();
	}
	LogisticsScreen::render(285, 58);
}
void Mechlopedia::PersonalityScreen::begin()
{
	groupListBox->removeAllItems(true);

	int FirstID = bIsHistory ? IDS_HISTORY_0 : IDS_PERSONALITY_0;

	int count = bIsHistory ? 5 : PERSONALITY_COUNT;

	for ( int i = 0; i < count; i++ )
	{
		auto pItem = std::make_unique<MechlopediaListItem>();
		char text[256];
		cLoadString( FirstID + i, text, 255 );
		const std::string upper = mc2::str::ascii_upper( text );
		pItem->setText( upper );
		pItem->setID( i );
		pItem->resize( groupListBox->width() - groupListBox->getScrollBarWidth() - 18, pItem->height() );


		bool bAdded = 0;
		if ( !bIsHistory ) // turns out we need to sort 'em
		{
			for ( int j = 0; j < groupListBox->GetItemCount(); j++ )
			{
				MechlopediaListItem* pTmpItem = static_cast<MechlopediaListItem*>(groupListBox->GetItem( j ));
				// The list is ordered by name, ignoring ASCII case; the stock personality names keep the order of the former locale collation.
				if ( mc2::str::icompare( upper, mc2::str::from_c( pTmpItem->getText() ) ) < 0 )
				{
					groupListBox->InsertItem( pItem.release(), j );
					bAdded = 1;
					break;
				}
			}

			if ( !bAdded )
				groupListBox->AddItem( pItem.release() );
		}
		else
			groupListBox->AddItem( pItem.release() );

	}

	aTextListItem* pItem = static_cast<aTextListItem*>(groupListBox->GetItem( 0 ));
	if ( pItem )
	{
		select( pItem );
		groupListBox->SelectItem( 0 );
	}

	groupListBox->setScrollPos( 0 );
}

void Mechlopedia::PersonalityScreen::select( aTextListItem* pEntry )
{
	int ID = pEntry->getID();

	int PictureID = bIsHistory ? IDS_HISTORY_PICTURE0 : IDS_PERSONALITY_PICTURE0;
	int DescriptionID = bIsHistory ? IDS_HISTORY_DESCRIPTION_0 : IDS_PERONSALITY_DESCRIPTION0;

	if ( !hasDefsUiPage() )
	{
		descriptionListBox.removeAllItems( true );

		auto pItem = std::make_unique<aTextListItem>( IDS_EN_WEAPON_FONT );
		pItem->setColor( 0xff005392);
		pItem->resize( descriptionListBox.width() - descriptionListBox.getScrollBarWidth() - 16, pEntry->height() );
		pItem->setText( loadResourceString( DescriptionID + ID ) );
		pItem->sizeToText();
		pItem->forceToTop( true );
		descriptionListBox.AddItem( pItem.release() );

		textObjects[0].setText( pEntry->getText() );
	}

	if ( hasDefsUiPage() )
	{
		std::string descText = loadResourceString( DescriptionID + ID );
		cleanLegacyDescription( descText );
		setDefsElementText( "game.mcl_en_person.text.description", descText );
	}

	char fileName[256];
	cLoadString( PictureID + ID, fileName, 255 );
	FullPathFileName path;
	path.init( artPath, fileName, ".tga" );

	if ( !hasDefsUiPage() )
	{
		statics[4].setTexture( path );
		statics[4].setUVs( 0, 0, statics[4].width(), statics[4].height() );
	}

	if ( hasDefsUiPage() )
		setDefsElementTexture( "game.mcl_en_person.image.personality_picture_is_placed_here", std::string(static_cast<const char*>(path)) );
}

////////////////////////////////////////////////////////
void MechlopediaListItem::render()
{
	bmpAnim.setState( (aAnimGroup::STATE)state );
	bmpAnim.update();
	long color = bmpAnim.getCurrentColor( (aAnimGroup::STATE)state );
	bmp.setColor( color );
	bmp.render();

	aAnimTextListItem::render();

}

void MechlopediaListItem::init( )
{
	FitIniFile file;
	FullPathFileName path;
	path.init( artPath, "mcl_en_sub", ".fit" );
	if ( NO_ERR != file.open( path ) )
	{
		char errorStr[256];
		(*std::format_to_n( errorStr, sizeof(errorStr) - 1, "couldn' open file {}", mc2::fmt::safe(static_cast<const char*>(path)) ).out = '\0');
		Assert( 0, 0, errorStr );
		return;
	}

	if ( !s_templateItem )
	{
		s_templateItem = new MechlopediaListItem();
		s_templateItem->bmp.init( &file, "Static0" );
		s_templateItem->bmpAnim.init( &file, "Animation1" );
		(static_cast<aAnimTextListItem*>(s_templateItem))->init( file );

	}
	
	
}


MechlopediaListItem::MechlopediaListItem() 
: aAnimTextListItem(IDS_EN_LISTBOX_FONT)
{
	if ( s_templateItem&& this != s_templateItem )
	{
		operator=( *s_templateItem );

		bmp = s_templateItem->bmp;
		bmpAnim = s_templateItem->bmpAnim;
		
		resize( bmp.width(), bmp.height()+2 );
	}

	addChild( &bmp );
}

///////////////////////////////////////////////////////////////////////////////////

void Mechlopedia::BuildingScreen::init()
{
	FullPathFileName path;
	path.init( artPath, "mcl_en_bldg", ".fit" );

	if ( tryInitDefsOnly( path ) )
	{
		// Camera bounds from mcl_en_bldg.fit Gui3DView:
		//   rect = "12,43,230,232"  mountOffset = "285,58"
		float sx = (Environment.screenWidth > 0) ? static_cast<float>(Environment.screenWidth) / 800.f : 1.f;
		float sy = (Environment.screenHeight > 0) ? static_cast<float>(Environment.screenHeight) / 600.f : 1.f;
		camera.init( (12 + 285) * sx, (43 + 58) * sy,
		             (12 + 230 + 285) * sx, (43 + 232 + 58) * sy );
		return;
	}

	FitIniFile file;
	if ( NO_ERR != file.open( path ) )
	{
		char errorStr[256];
		(*std::format_to_n( errorStr, sizeof(errorStr) - 1, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(path)) ).out = '\0');
		Assert( 0, 0, errorStr );
		return;
	}

	SubScreen::init( file );

	compListBox.init( rects[2].left(), rects[2].top(), rects[2].width(), rects[2].height() );
	compListBox.move( 285, 58 );

	{
		float sx = (Environment.screenWidth > 0) ? static_cast<float>(Environment.screenWidth) / 800.f : 1.f;
		float sy = (Environment.screenHeight > 0) ? static_cast<float>(Environment.screenHeight) / 600.f : 1.f;
		camera.init( (statics[4].left() + 285) * sx, (statics[4].top() + 58) * sy,
		             (statics[4].right() + 285) * sx, (statics[4].bottom() + 58) * sy );
	}
	statics[4].setColor( 0 );
	textObjects[0].setText( "" );
	descriptionListBox.init( rects[1].left() + 285, rects[1].top() + 58, rects[1].width(), rects[1].height() );
}
void Mechlopedia::BuildingScreen::update()
{
	SubScreen::update();

	if ( !hasDefsUiPage() )
		compListBox.update();
}
void Mechlopedia::BuildingScreen::render()
{
	if ( !hasDefsUiPage() )
	{
		compListBox.render();
		descriptionListBox.render();
		groupListBox->render();
		camera.setPreviewOffscreen( false );
		camera.render();
	}
	else
	{
		// PREVIEW-FBO-FIXED-800x600-1: same fixed-800x600-FBO + composite
		// approach as MechScreen::render() above.
		camera.setPreviewOffscreen( true );
		camera.render();
		float px = 0, py = 0, pw = 0, ph = 0;
		if ( getDefsElementScreenRect( "game.mcl_en_bldg.3dview.rotating_picture_of_building_goes_here_davion_version", px, py, pw, ph ) )
			camera.drawPreviewToPanel( px, py, pw, ph );
	}
	LogisticsScreen::render(285, 58);
}
void Mechlopedia::BuildingScreen::begin()
{
	groupListBox->removeAllItems( true );

	LogisticsData::Building* pBldgs[256];
	int count = 255;
	LogisticsData::instance->getBuildings( pBldgs, count );
	for (  int i = 0; i < count; i++ )
	{
		char tmp[256];
		cLoadString( pBldgs[i]->nameID, tmp, 255 );
		// The faction prefix is cut from the upper-cased name (a mixed-case "Liao " can no longer be present at that point).
		const std::string str = mc2::str::erase_first( mc2::str::ascii_upper( tmp ), "LIAO " );

		bool bFound = 0;
		for ( int j = 0; j < groupListBox->GetItemCount(); j++ )
		{
			if ( S_stricmp(  str.c_str(), (static_cast<aTextListItem*>(groupListBox->GetItem( j )))->getText() ) < 0 )
			{
				auto pItem = std::make_unique<MechlopediaListItem>();
				pItem->setText( str );
				pItem->setID( pBldgs[i]->nameID );
				pItem->resize( groupListBox->width() - groupListBox->getScrollBarWidth() - 18, pItem->height() );

				groupListBox->InsertItem( pItem.release(), j );
				bFound = true;
				break;
			}
		}

		if ( !bFound )
		{
			auto pItem = std::make_unique<MechlopediaListItem>();
			pItem->setText( str );
			pItem->setID( pBldgs[i]->nameID );
			pItem->resize( groupListBox->width() - groupListBox->getScrollBarWidth() - 18, pItem->height() );

			groupListBox->AddItem( pItem.release() );
		}
		
	}

	aTextListItem* pItem = static_cast<aTextListItem*>(groupListBox->GetItem( 0 ));
	select( pItem );
}
void Mechlopedia::BuildingScreen::select( aTextListItem* pEntry )
{
	if ( !pEntry )
		return;

	LogisticsData::Building* pBldg = LogisticsData::instance->getBuilding( pEntry->getID() ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);

	if ( pBldg )
	{
		char name[256];
		cLoadString( pBldg->nameID, name, 255 );
		const std::string tmpStr = mc2::str::erase_first( mc2::str::ascii_upper( name ), "LIAO " );

		camera.setBuilding( pBldg->fileName );
		camera.setScale( pBldg->scale );

		if ( !hasDefsUiPage() )
		{
			textObjects[0].setText( tmpStr );

			const std::string formatted = mc2msg::message<IDS_EN_BUILDING_WEIGHT>( pBldg->weight );
			textObjects[1].setText( formatted );

			descriptionListBox.removeAllItems( true );

			auto pItem = std::make_unique<aTextListItem>( IDS_EN_LISTBOX_FONT );
			pItem->resize( descriptionListBox.width() - descriptionListBox.getScrollBarWidth() - 16, pItem->height() );
			pItem->setText( loadResourceString( pBldg->encycloID ) );
			pItem->sizeToText( );
			pItem->setColor( 0xff005392 );
			pItem->forceToTop( true );
			descriptionListBox.AddItem( pItem.release() );
		}

		LogisticsComponent* pComps[4];
		int count = 0;
		for (int ID : pBldg->componentIDs)
		{
				if ( ID )
			{
				LogisticsComponent* pComp = LogisticsData::instance->getComponent( ID ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
				if ( pComp )
				{
					pComps[count++] = pComp;
				}
			}
		}

		if ( !hasDefsUiPage() )
		{
			compListBox.removeAllItems( true );
			if ( count )
				compListBox.setComponents( count, pComps );
		}

		if ( hasDefsUiPage() )
		{
			std::string descText = loadResourceString( pBldg->encycloID );
			cleanLegacyDescription( descText );
			setDefsElementText( "game.mcl_en_bldg.text.description", descText );

			// Structure points readout (legacy textObjects[1]).
			const std::string structText = mc2msg::message<IDS_EN_BUILDING_WEIGHT>( pBldg->weight );
			setDefsElementText( "game.mcl_en_bldg.text.structure_readout", structText );

			// Weapons loadout (per-range colored), populated from the building's
			// component IDs exactly as the legacy compListBox is.
			compListBox.removeAllItems( true );
			if ( count )
				compListBox.setComponents( count, pComps );
			syncWeaponLoadout( this, compListBox, "game.mcl_en_bldg.list.weapons" );
		}
	}


}

//*************************************************************************************************
// end of file ( Mechlopedia.cpp )
