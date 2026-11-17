#include "Gates/preferences_legacy.h"
#define MECHLABSCREEN_CPP
/*************************************************************************************************\
MechLabScreen.cpp			: Implementation of the MechLabScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"mechlabscreen.h"
#include <array>
#include <string>
#include <string_view>
#include "Gates/gate.h"
#include"inifile.h"
#include"logisticsdata.h"
#include"mechbayscreen.h"
#include"cmponent.h"
#include"unitprofile.h"	// EQUIP-ADD-UI-NAME-1 (S3): equipmentAddUiTextForStringId
#include"platform_windows.h"
#include "../resource.h"
#include"logisticsdialog.h"
#include "logisticserrors.h"
#include<malloc.h>
#include"gamesound.h"
#include "../GameOS/gameos/vulkan_legacy_shape_island.h"

// Interactive cheat gate
// The launcher writes these into launcher_env.json, which main() applies after static initialization, so they
// are read at first use rather than in a namespace-scope initializer.
static bool cheatInfiniteMoney() { static const bool on = mc2::gate::get<"MC2_CHEAT_INFINITE_MONEY">(); return on; }
#include"chatwindow.h"
#include"multplyr.h"
#include"prefs.h"
#include"../GuiRuntime/GuiRuntime.h"
#include "mc2fmt.h"
#include "mc2str.h"
#include "component_rules.h"
#include "mechlab_loc_layout.h"
#include "package_chassis.h"
#include "../Gates/render_options.h"
#include "../GameOS/gameos/input_capture_policy.h"

namespace ml = mc2::mechlab_loc;
static bool isPackageChassis(const LogisticsVariant* v) { return v && v->getChassis() && v->getChassis()->getPackage(); }
static std::string dropRefusal;
static float dropRefusalTime = 0.f;
static bool dragCancelled = false;
static void showDropRefusal(std::string_view reason)
{
	dropRefusal = reason;
	dropRefusalTime = 3.5f;
}

MechLabScreen* MechLabScreen::s_instance = nullptr;
namespace {
bool previewPaused = false;
int previewHeld = -1;
float previewHoldTime = 0.f;
// Legacy canvas coordinates, shared by drawing and input; below the payload grid.
int previewButton(const SimpleCamera& camera)
{
	const float x = userInput->getMouseX() - (camera.bounds[2] - 92.f);
	const float y = userInput->getMouseY() - (camera.bounds[3] - 28.f);
	const int column = static_cast<int>(x);
	return x >= 0.f && x < 86.f && y >= 0.f && y < 22.f && column % 30 < 26 ? column / 30 : -1;
}
void drawPreviewControls(const SimpleCamera& camera, bool enabled)
{
	float sx, sy, ox, oy;
	aObject::getCanvasTransform(sx, sy, ox, oy);
	for (int i = 0; i < 3; ++i)
	{
		const float x = (camera.bounds[2] - 92.f + i * 30.f) * sx + ox;
		const float y = (camera.bounds[3] - 28.f) * sy + oy;
		const unsigned color = enabled ? 0xffffb04a : 0xff805830;
		GuiRuntime::DrawUiRect(x, y, 26.f * sx, 22.f * sy,
			enabled && previewButton(camera) == i ? 0xff56321a : 0xff17120d, true);
		GuiRuntime::DrawUiRect(x, y, 26.f * sx, 22.f * sy, color, false);
		if (i == 1 && !previewPaused)
		{
			GuiRuntime::DrawUiRect(x + 9.f * sx, y + 6.f * sy, 3.f * sx, 10.f * sy, color, true);
			GuiRuntime::DrawUiRect(x + 15.f * sx, y + 6.f * sy, 3.f * sx, 10.f * sy, color, true);
		}
		else
		{
			const float tip = i == 0 ? 8.f : 18.f, base = i == 0 ? 17.f : 9.f;
			GuiRuntime::DrawUiTriangleVerts(x + tip * sx, y + 11.f * sy,
				x + base * sx, y + 6.f * sy, x + base * sx, y + 16.f * sy, color);
		}
	}
}
}

std::unique_ptr<LogisticsVariantDialog> MechLabScreen::saveDlg = nullptr;
std::unique_ptr<LogisticsAcceptVariantDialog> MechLabScreen::acceptDlg = nullptr;

GUI_RECT MechLabScreen::sensorRects[4] = { 35, 126, 35 + 64, 126 + 64,
											35, 126, 35 + 79, 126 + 64,
											35, 126, 35 + 94, 126 + 64,
											249, 126, 249 + 79, 126 + 64 };

long	MechLabScreen::sensorHelpIDs[4] = { IDS_HELP_COMP14, IDS_HELP_COMP15, IDS_HELP_COMP17, IDS_HELP_COMP38 };


MechLabScreen::MechLabScreen(  ) 
{
	status = RUNNING;
	pVariant = nullptr;
	helpTextArrayID = 14;
	pCurComponent = nullptr;
	gosASSERT( !s_instance );
	s_instance = this;
	pDragComponent = nullptr;
	pSelectedComponent = nullptr;
	selI = -1;
	selJ = -1;

	countDownTime = .5;
	curCount = 0.0;
	previousAmount = 0;

	oldCBillsAmount  =0;

	oldHeat = 0;
	heatTime = 0;

	oldArmor = 0;
	newArmor = 0;
	armorTime = 0;
	bDragLeft = 0;

	selRect = nullptr;

}

//-------------------------------------------------------------------------------------------------

MechLabScreen::~MechLabScreen()
{
	s_instance = nullptr;

	if (saveDlg)
	{
		saveDlg.reset();
		saveDlg = nullptr;
	}

	if (acceptDlg)
	{
		acceptDlg.reset();
		acceptDlg = nullptr;
	}

	variantList.destroy();

	componentListBox.destroy();
}

int MechLabScreen::init( FitIniFile& file )
{
	if ( saveDlg )// already initialized
		return -1;

	ComponentListItem::init( file );

	setLayoutNeeds( { "mcl_mc.fit", 56, 23, 9, 0, 17, false } ); // stock element counts; see LayoutNeeds
	LogisticsScreen::init( file, "Static", "Text", "Rect", "Button" );
	defsHelpTextKey = "game.mcl_mc.text.help_text";

	componentListBox.init( rects[2].left(), rects[2].top(),
							rects[2].width(), rects[2].height() );

	componentListBox.setHighlightFX( -1 ); // want to do this by hand.
	
	saveDlg = std::make_unique_for_overwrite<LogisticsVariantDialog>();
	acceptDlg = std::make_unique_for_overwrite<LogisticsAcceptVariantDialog>();

	FitIniFile saveFile;
	FullPathFileName path;
	path.init( artPath, "MCL_SV", ".fit" );

	if ( NO_ERR != saveFile.open( path ) )
	{
		Assert( 0, 0, "couldn't open MCL_SV.fit" );
		return -1;
	}

	saveDlg->init( saveFile );
	saveFile.close();

	path.init( artPath, "mcl_sv_acceptchanges", ".fit" );

	if ( NO_ERR != saveFile.open( path ) )
	{
		Assert( 0, 0, "couldn't open MCL_SV.fit" );
		return -1;
	}

	acceptDlg->init( saveFile );


	payloadIcon.init( &file, "PayloadUnit" );	  

	char blockName[64];
	for ( int i = 0; i < MECH_LAB_ATTRIBUTE_METER_COUNT; i++ )
	{
		mc2::fmt::to_buf(blockName, "AttributeMeter{:d}", mc2::fmt::as_i32(i));
		attributeMeters[i].init( &file, blockName );
	}

	for (int i= 0; i < buttonCount; i++ )
	{
		switch( buttons[i].getID() )
		{
			case	COMPONENT_FORM_WEAPON_ENERGY:
			case	COMPONENT_FORM_WEAPON_BALLISTIC:
			case	COMPONENT_FORM_WEAPON_MISSILE:
			case	COMPONENT_FORM_JUMPJET:
				break;

			default:
				{
				buttons[i].setMessageOnRelease();
				buttons[i].setPressFX(LOG_CLICKONBUTTON );
				}

				
		}	
	}

	file.seekBlock( "ComboBox0" );
	long xLoc, yLoc;
	file.readIdLong( "XLocation", xLoc );
	file.readIdLong( "YLocation", yLoc );
	char fileName[256];
	file.readIdString( "FileName", fileName, 255 );

	path.init( artPath, fileName, ".fit" );

	FitIniFile comboFile;

	if ( NO_ERR != comboFile.open( path ) )
	{
		char errorStr[255];
		mc2::fmt::to_buf(errorStr, "couldn't open file {}", mc2::fmt::safe(static_cast<const char*>(path)));
		Assert( 0, 0, errorStr );
	}


	variantList.init( &comboFile, "VariantComboBox" );
	variantList.setReadOnly( true );
	variantList.ListBox().setOrange(true);
	variantList.setParent( this );
	variantList.setHelpID( IDS_HELP_MC_VARIANT_DROP_DOWN_LIST_BOX );

	// Grid drawing, selection and drag conversion consume these same rects.
	for (int i = 7; i <= 11; ++i) {
		rects[i].moveTo(ml::kAreaX + (i - 7) * ml::kCellW / 2, 364);
		rects[i].resize(ml::kCellW, LogisticsComponent::YICON_FACTOR);
	}
	rects[6].moveTo(321, 409);
	rects[6].resize(2 * ml::kCellW, 64);
	rects[13].moveTo(ml::kAreaX, 130);
	rects[13].resize(316, 344);
	rects[12].moveTo(27, 495);
	rects[12].resize(182, 30);
	payloadIcon.resize(ml::kCellW, LogisticsComponent::YICON_FACTOR);
	mechCamera.init(ml::kPreview.x, ml::kPreview.y,
		ml::kPreview.x + ml::kPreview.w, ml::kPreview.y + ml::kPreview.h);
	textObjects[11].moveTo(240, 488);
	textObjects[12].moveTo(240, 506);
	textObjects[4].moveTo(27, 499);
	textObjects[5].moveTo(130, 497);
	for (int i : {20, 32}) {
		statics[i].moveTo(217, i == 20 ? 490 : 508);
		statics[i].resize(20, 15);
	}

	getButton( COMPONENT_FORM_WEAPON_ENERGY )->setPressFX( LOG_VIDEOBUTTONS );
	getButton( COMPONENT_FORM_WEAPON_BALLISTIC )->setPressFX( LOG_VIDEOBUTTONS );
	getButton( COMPONENT_FORM_WEAPON_MISSILE )->setPressFX( LOG_VIDEOBUTTONS );
	getButton( COMPONENT_FORM_JUMPJET )->setPressFX( LOG_VIDEOBUTTONS );

	getButton( COMPONENT_FORM_WEAPON_ENERGY )->setHighlightFX( LOG_DIGITALHIGHLIGHT );
	getButton( COMPONENT_FORM_WEAPON_BALLISTIC )->setHighlightFX( LOG_DIGITALHIGHLIGHT );
	getButton( COMPONENT_FORM_WEAPON_MISSILE )->setHighlightFX( LOG_DIGITALHIGHLIGHT );
	getButton( COMPONENT_FORM_JUMPJET )->setHighlightFX( LOG_DIGITALHIGHLIGHT );


	
	// initialize the selection rectangles
	for (int i = 0; i < 5; i++ )
	{
		for ( int j = 0; j < 2; j++ )
		{
			mc2::fmt::to_buf(fileName, "mcl_mc_{:d}by{:d}_selection.tga", mc2::fmt::as_i32(j+1), mc2::fmt::as_i32(i+1));
			path.init( artPath, fileName, ".tga" );

			if ( fileExists( path ) )
			{
				selRects[j][i].setTexture( path );
				selRects[j][i].resize( (j+1) * ml::kCellW, (i+1) * LogisticsComponent::YICON_FACTOR );
				// now need to set the UV's
				selRects[j][i].setUVs( 0, 0, (j+1) * 48, (i+1) * 32 );
			}

		}
	}

	path.init( artPath, "mcl_mc_jumpjets_selection", ".tga" );
	selJumpJetRect.setTexture( path );
	selJumpJetRect.resize( 2 * ml::kCellW, 2 * LogisticsComponent::YICON_FACTOR );
	// now need to set the UV's
	selJumpJetRect.setUVs( 0, 0, 96, 64 );

	return true;
}

void MechLabScreen::begin()
{
	pInspectedComponent = nullptr;
	dropRefusalTime = 0.f;
	dragCancelled = false;
	previewPaused = false;
	previewHeld = -1;
	componentCount = 0;
	bSaveDlg = 0;
	status = RUNNING;
	bDragLeft= 0;
	bErrorDlg = 0;

	if ( !pVariant ) // if we are coming directly from the main menu
	{
		// getMechToModify() can return null when no mech is selected (e.g. forced
		// begin() from the soak check-screens harness before logistics is staged).
		// Bail out early rather than null-deref â€” caller should have guarded first.
		LogisticsMech* mechToMod = LogisticsData::instance
		                           ? LogisticsData::instance->getMechToModify()
		                           : nullptr;
		if ( !mechToMod )
		{
			status = DONE;
			return;
		}
		pVariant = mechToMod->getVariant();

		variantList.ListBox().removeAllItems(true);

		int maxCount = 0;
		LogisticsVariant** pVar = nullptr;
		int addedCount = 0;

		LogisticsData::instance->getChassisVariants( pVariant->getChassis(),pVar, maxCount );
		if ( maxCount )
		{
			maxCount ++;
			pVar = static_cast<LogisticsVariant**>(_alloca( maxCount* sizeof (LogisticsVariant*) ));
			LogisticsData::instance->getChassisVariants( pVariant->getChassis(),pVar, maxCount );

			for ( int i = 0; i < maxCount; i++ )
			{
				if ( pVar[i]->allComponentsAvailable() )
				{
					variantList.AddItem( pVar[i]->getName().c_str(), rects[1].getColor() );
					if ( mc2::str::iequals( pVar[i]->getName(), pVariant->getName() ) )
					{
						variantList.SelectItem( i );
						variantList.EditBox().setEntry( pVar[i]->getName() );
					}

					addedCount++;
				}
			}

		}
		if ( !addedCount ) // this is a mech we don't have access to yet
		{
			variantList.AddItem( pVariant->getName().c_str(), rects[1].getColor() );
			variantList.SelectItem( 0 );
			variantList.EditBox().setEntry(pVariant->getName() );

		}
		


		

		// no changes, set cost to 0
		textObjects[5].setText( "0" );
	}
	

	if ( pVariant )
	{
		// MC2_ENGINE_XL: size the XL-engine components' footprint + heat to this
		// mech's weight class (runs for every bay entry incl. Modify Mech, not just
		// the main-menu path). Base XL: Light 2x1/h8, Medium 3x1/h12, Heavy 2x2/h16,
		// Assault 3x2/h20. Clan XL: 1x1/2x1/2x1/3x1. All placement/render sites read
		// getComponentWidth/Height, so this one hook drives the whole UI.
		if ( pVariant->getChassis() && LogisticsData::instance )
		{
			float tons = pVariant->getChassis()->getMaxWeight();
			int cls = ( tons <= 35 ) ? 0 : ( tons <= 55 ) ? 1 : ( tons <= 75 ) ? 2 : 3;
			// Footprints stay <=2 wide: the mech grid + selRects[2][5] only support
			// 2-wide components (a 3-wide index reads past selRects into
			// selJumpJetRect -> engine shows the JJ ghost). Cells: Light2 Med3
			// Heavy4 Assault6 (base); 1/2/2/3 (clan).
			static const std::array<int, 4> baseW = { 2, 1, 2, 2 };  // L  M  H  A
			static const std::array<int, 4> baseH = { 1, 3, 2, 3 };  // 2  3  4  6 cells
			static const std::array<int, 4> clanW = { 1, 2, 2, 1 };
			static const std::array<int, 4> clanH = { 1, 1, 1, 3 };  // 1  2  2  3 cells
			static const std::array<float, 4> heatByCls = { 8.0f, 12.0f, 16.0f, 20.0f };
			LogisticsComponent* xl  = LogisticsData::instance->getComponent( 2 ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
			LogisticsComponent* cxl = LogisticsData::instance->getComponent( 3 ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
			if ( xl )  xl->setEngineProfile( baseW[cls], baseH[cls], heatByCls[cls] );
			if ( cxl ) cxl->setEngineProfile( clanW[cls], clanH[cls], heatByCls[cls] );
		}

		textObjects[3].setText( pVariant->getChassisName() );
		originalCost = pVariant->getCost();


		char path[256];
		strcpy( path, artPath );
		strcat( path, "MCL_MC_" );
		char mechName[64];
		_splitpath( pVariant->getFileName().c_str(), nullptr, nullptr, mechName, nullptr );
		strcat( path, mechName );
		strcat( path, "_B.tga" );
		CharLower( path );

		for ( int i = 51; i < 54; i++ )
			statics[i].setTexture( path );

		strcpy( path, artPath );
		strcat( path, "MCL_MC_" );
		strcat( path, mechName );
		strcat( path, "_A.tga" );
		CharLower( path );

		statics[50].setTexture( path );

		// MECHLAB-3D-1: pose the real model; a chassis with no model keeps the silhouette statics.
		mechCamera.setMech( mc2::str::file_stem( pVariant->getFileName() ).c_str(), prefs.baseColor, prefs.highlightColor, prefs.highlightColor );
		if ( mechCamera.hasObject() && pVariant->getChassis() )
			mechCamera.setScale( pVariant->getChassis()->getScale() );
		if ( mechCamera.hasObject() )
			mechCamera.setFramedPose( 0.88f, -35.f, 30.f );	// front three-quarter, 88% of the panel height


		for (int i = COMPONENT_FORM_WEAPON_ENERGY; i < COMPONENT_FORM_JUMPJET + 1; i++ )
		{
			aButton* pButton = getButton( i );
			if ( pButton )
			{
				pButton->press( 0 );
			}
		}
		componentListBox.removeAllItems( 0 );

		componentListBox.setType( COMPONENT_FORM_WEAPON_BALLISTIC, -1, -1 );
		getButton( COMPONENT_FORM_WEAPON_BALLISTIC )->press( true );

		updateDiagram();

		if ( -1 == selectFirstDiagramComponent() )
			selectFirstLBComponent();

		oldCBillsAmount = 0;
	}

	// init CBills
	char text[32];
	mc2::fmt::to_buf(text, "{:d} ", mc2::fmt::as_i32(LogisticsData::instance->getCBills()));
	textObjects[1].setText( text );
	setDefsElementText( "game.mcl_mc.text.cbills_text", text );


	varName = variantList.EditBox().entry();

	if ( pVariant->getChassis()->jumpJetsAllowed() )
	{
		statics[54].showGUIWindow( true );
	}
	else
		statics[54].showGUIWindow( false );



}
void MechLabScreen::end()
{
	mechCamera.setMech( nullptr );
	pVariant = nullptr;
}
void MechLabScreen::update()
{
	dropRefusalTime = std::max(0.f, dropRefusalTime - frameLength);
	if (userInput->getMouseLeftButtonState() != MC2_MOUSE_DOWN) dragCancelled = false;
	// Headless proof hook (MC2_SOAK_CHECK_SCREENS walk): open the SAVE VARIANT
	// dialog once on the first mech lab update so the -5 capture shows it.
	{
		static const bool s_soakCheck = mc2::gate::get<"MC2_SOAK_CHECK_SCREENS">();
		static bool s_dlgFired = false;
		if ( s_soakCheck && !s_dlgFired && !bSaveDlg && saveDlg )
		{
			s_dlgFired = true;
			pCurDialog = saveDlg.get();
			pCurDialog->begin();
			pCurDialog->getEnterAnim().end();	// settled pose: the -5 capture only sees 5 frames
			bSaveDlg = true;
		}
	}
	// Mirror the legacy weapon-info / mech-name / cost / heat+armor-limit text
	// objects into the defs page each frame (they're set across many branches of
	// setComponent/update*, and the defs page suppresses the legacy textObjects).
	// textObjects[N] <-> MechLabTextEntryN <-> the defs key below.
	static const struct { int idx; const char* key; } s_textDefsMap[] = {
		{  3, "game.mcl_mc.text.mech_name" },
		{  5, "game.mcl_mc.text.cost_of_changes_readout" },
		{  6, "game.mcl_mc.text.weapon_info_header" },
		{  7, "game.mcl_mc.text.weapon_info_range_readout" },
		{  8, "game.mcl_mc.text.weapon_info_damage_readout" },
		{  9, "game.mcl_mc.text.weapon_info_recycle_readout" },
		{ 10, "game.mcl_mc.text.weapon_info_heat_readout" },
		{ 11, "game.mcl_mc.text.armor_limit_readout" },
		{ 12, "game.mcl_mc.text.heat_limit_readout" },
		{ 13, "game.mcl_mc.text.weapon_info_ammo_text_readout" },
		{ 15, "game.mcl_mc.text.jump_jet_heat_readout" },
	};
	for ( const auto& m : s_textDefsMap )
		if ( m.idx < textCount )
			setDefsElementText( m.key, textObjects[m.idx].textString() );

	// Swap the weapon-info ICON set to match the selected component type, the way
	// the legacy code toggled statics[] in setComponent()/showJumpJetItems(). The
	// defs page suppresses those legacy statics and renders its own icon elements
	// on top, so drive their visibility here. One icon set per type (no overlap):
	//   weapon  -> range / damage / recycle / heat (+ ammo unless unlimited)
	//   jump jet-> jump-range / jump-jet-heat
	//   heat sink-> heat-sink
	//   armor (bulk)-> armor
	{
		const int  t        = pCurComponent ? pCurComponent->getType() : -1;
		const bool isJump   = ( t == COMPONENT_FORM_JUMPJET );
		const bool isHeat   = ( t == COMPONENT_FORM_HEATSINK );
		const bool isArmor  = ( t == COMPONENT_FORM_BULK );
		const bool isWeapon = ( pCurComponent && !isJump && !isHeat && !isArmor );

		setDefsElementVisible( "game.mcl_mc.image.icon_for_weapon_range", isWeapon );
		setDefsElementVisible( "game.mcl_mc.image.icon_for_damage", isWeapon );
		setDefsElementVisible( "game.mcl_mc.image.icon_for_recycle_rate", isWeapon );
		setDefsElementVisible( "game.mcl_mc.image.icon_for_weapon_heat", isWeapon );
		setDefsElementVisible(
			"game.mcl_mc.image.icon_for_ammo_this_one_won_t_show_up_if_infinite_ammo_is_on_in_options",
			isWeapon && !useUnlimitedAmmo );
		setDefsElementVisible(
			"game.mcl_mc.image.icon_for_jump_range_use_only_when_jump_jets_are_selected", isJump );
		setDefsElementVisible(
			"game.mcl_mc.image.icon_for_jump_jet_heat_use_only_when_jump_jets_are_selected", isJump );
		setDefsElementVisible(
			"game.mcl_mc.image.icon_for_heat_sink_heat_use_only_when_heat_sink_is_selected", isHeat );
		setDefsElementVisible(
			"game.mcl_mc.image.icon_for_armor_use_only_when_armor_is_selected", isArmor );
	}

	if ( bSaveDlg )
	{
		pCurDialog->update();

		if ( pCurDialog->isDone() )
		{
			if ( pCurDialog->getStatus() == YES )
			{
				LogisticsData::instance->acceptMechModifications( pCurDialog->getFileName().c_str() );
				pCurDialog->end();
				status = UP;
				bSaveDlg = 0;
			}
			else if ( pCurDialog->getStatus() == NO )
			{
				bSaveDlg = 0;
				pCurDialog->end();
			}
		}

		return;
	}
	else if ( bErrorDlg )
	{
		LogisticsOneButtonDialog::instance()->update();
		if ( LogisticsOneButtonDialog::instance()->isDone() )
			bErrorDlg = 0;

		return;
	}

	if ( !pVariant || status != RUNNING )
		return;

	
	const int previewHit = mechCamera.hasObject() && !pDragComponent ? previewButton(mechCamera) : -1;
	const bool previewDown = userInput->getMouseLeftButtonState() == MC2_MOUSE_DOWN;
	static bool previewWasDown = false;
	if (previewDown && !previewWasDown && previewHit >= 0)
	{
		previewHeld = previewHit;
		previewHoldTime = 0.f;
		if (previewHeld == 1) previewPaused = !previewPaused;
		else mechCamera.rotateBy(previewHeld == 0 ? -15.f : 15.f);
	}
	const bool previewOwnsInput = previewHeld >= 0;
	if (previewDown && previewHeld != 1 && previewHeld >= 0 && previewHit == previewHeld)
	{
		previewHoldTime += std::clamp(frameLength, 0.f, 0.1f);
		if (previewHoldTime > 0.3f)
			mechCamera.rotateBy((previewHeld == 0 ? -60.f : 60.f) * std::clamp(frameLength, 0.f, 0.1f));
	}
	if (!previewDown || pDragComponent) previewHeld = -1;
	previewWasDown = previewDown;
	mechCamera.setTurntablePaused(previewPaused || previewOwnsInput || pDragComponent != nullptr);
	mechCamera.update();

	int oldSel = componentListBox.GetSelectedItem(); // need to check this before we update the list box

	for (auto & attributeMeter : attributeMeters)
		attributeMeter.update();


	if ( !MPlayer || !ChatWindow::instance()->pointInside(userInput->getMouseX(), userInput->getMouseY()) )
		LogisticsScreen::update();

	if (!previewOwnsInput && !variantList.pointInside(userInput->getMouseX(), userInput->getMouseY()) && !pDragComponent && isPackageChassis(pVariant) && userInput->isLeftClick() &&
		mc2::mechlab_loc::toggleRect().contains(static_cast<float>(userInput->getMouseX()), static_cast<float>(userInput->getMouseY())))
		setLocationView(userInput->getMouseX() < mc2::mechlab_loc::toggleRect().cx());
	if (!previewOwnsInput && (pDragComponent || !variantList.pointInside(userInput->getMouseX(), userInput->getMouseY())))
	{
		if (locationViewActive()) updateLocationInput();
		else {
			if ( userInput->isLeftClick() && rects[13].pointInside(userInput->getMouseX(), userInput->getMouseY()) )
				pInspectedComponent = nullptr;
			updateDiagramInput();
		}
	}


	variantList.update();

	bool vals[4];
	if ( pVariant && status == RUNNING ) // sensor help texts
	{
		int sensorID = pVariant->getSensorID().value_or(-1);
		for ( int i = 0; i < 3; i++ )
		{
			if ( sensorID == sensorHelpIDs[i] - IDS_HELP_COMP0 )
				vals[i] = true;
			else
				vals[i] = false;
		}
		vals[3] = pVariant->getECM().value_or(-1) > 0;

		for (int i = 0; i < 4; i++ )
		{
			if ( vals[i] )
			{
				if ( sensorRects[i].left <= userInput->getMouseX() &&
					sensorRects[i].right >= userInput->getMouseX() &&
					sensorRects[i].top <= userInput->getMouseY() && 
					sensorRects[i].bottom >= userInput->getMouseY()
					&& !helpTextID )
				{
					helpTextID = sensorHelpIDs[i];
					break;
				}
			}
		}
	}

	// hack, in case the variant list sets the helpID
	if ( ::helpTextID )
	{
		// EQUIP-ADD-UI-NAME-1 (S3): ADD-filled component help ids have no
		// string-table entry; resolve from the def. Miss -> stock path.
		const char* addHelpText = equipmentAddUiTextForStringId( ::helpTextID );
		if ( addHelpText )
			textObjects[helpTextArrayID].setText( addHelpText );
		else
			textObjects[helpTextArrayID].setText( helpTextID );
	}

	// see if the variant changed
	const std::string tmp = variantList.EditBox().entry();
	if ( !mc2::str::iequals( varName, tmp ) )
	{
		pInspectedComponent = nullptr;
		swapVariant();
	}

	

	if ( status != RUNNING ) // can change after button updates
		return;

	// update the list box
	if ( userInput->isLeftClick() && componentListBox.pointInside(userInput->getMouseX(), userInput->getMouseY()) )
		pInspectedComponent = nullptr;
	componentListBox.update();
	int newSel = componentListBox.GetSelectedItem();

	if ( oldSel != newSel && !pDragComponent )
	{
		if ( newSel != -1 )
		{
			pSelectedComponent = nullptr;
			selI = selJ = -1;
		}
		else
		{
			bool bFound = 0;
			for ( int i = 0; i < componentListBox.GetItemCount(); i++ )
			{
				if ( componentListBox.GetItem( i )->getState() != aListItem::DISABLED
					&& !canAddComponent( (static_cast<ComponentListItem*>(componentListBox.GetItem( i )))->getComponent() ) )
				{
					bFound = 1;
					break;
				}
			
			}
			if ( !bFound )
				selectFirstDiagramComponent();
		}
	}

	if ( pInspectedComponent ) { pSelectedComponent = nullptr; selI = selJ = -1; }
	bool bNoDelete = !canRemoveComponent( pSelectedComponent );


	if ( pSelectedComponent  )
	{
		setComponent( pSelectedComponent );
		if ( bNoDelete || (selI == -1 && selJ == -1) )
			getButton( MB_MSG_REMOVE )->disable( 1 );
		else
			getButton( MB_MSG_REMOVE )->disable( 0 );
		getButton( MB_MSG_ADD )->disable( 1 );
	}
	else
	{
		LogisticsComponent* pComp = pInspectedComponent ? pInspectedComponent : componentListBox.getComponent();
		if ( pComp )
		{
			setComponent( pComp );
			getButton( MB_MSG_ADD )->disable( canAddComponent( pComp ) != 0 );
			getButton( MB_MSG_REMOVE )->disable( 1 );
		}
		else
			getButton( MB_MSG_REMOVE )->disable( 1 );

	}

	long color = 0xffffffff;
	
	float newCost = pVariant->getCost();

	// figure out change
	float costChange = newCost - originalCost;

	if ( curCount && curCount + frameLength < countDownTime  )
	{
		curCount += frameLength;
		float curAmount = previousAmount - (curCount/countDownTime * previousAmount); 
		costChange += curAmount;
		color = 0xffa21600;
		if ( curAmount > 0 )
			color = 0xffc8e100;
	}

	updateHeatMeter();
	updateArmorMeter();
	
	char text[64];
	mc2::fmt::to_buf(text, "{:.0f} ", costChange);
	textObjects[5].setText( text );
	textObjects[5].setColor( color );

	if ( MPlayer && ChatWindow::instance() )
		ChatWindow::instance()->update();




}

void MechLabScreen::updateDiagramInput()
{
	auto refuseRemoval = [this]() {
		showDropRefusal("Not enough cooling: remove weapons first.");
		soundSystem->playDigitalSample( LOG_WRONGBUTTON );
	};
	// Focus cancellation is not a drop; return a removed component to its slot.
	if (pDragComponent && (gos_GetKeyStatus(KEY_LMOUSE) == KEY_FREE ||
		userInput->isRightClick() || userInput->getKeyDown(KEY_ESCAPE))) {
		if (mc2::components::restoreGridDrag(bDragLeft, false, false, true)) {
			addComponent(pDragComponent, selI, selJ);
			pSelectedComponent = pDragComponent;
		}
		pDragComponent = nullptr;
		bDragLeft = false;
		dragCancelled = true;
		updateDiagram();
		return;
	}
	if (dragCancelled) return;
	long mouseX = userInput->getMouseX();
	long mouseY = userInput->getMouseY();


	if ( rects[13].pointInside(mouseX, mouseY) ||
		input::capture::startsWidgetDrag(userInput->isLeftDrag(),
			rects[13].pointInside(userInput->getMouseDragX(), userInput->getMouseDragY())) )
	{
		long x, y, x2, y2;
		getMouseDiagramCoords( x, y );	
		
		if ( x != -2 )
		{
			// now offset by size of the component
			if ( pDragComponent )
			{
				long screenX, screenY;
				diagramToScreen( x, y, screenX, screenY );

				x -= pDragComponent->getComponentWidth()/2;
				y -= pDragComponent->getComponentHeight()/2;

				if ( mouseX - screenX > ml::kCellW/2
					&& pDragComponent->getComponentWidth()/2)
				{
					x += 1;
				}	
			}

			x = std::max<decltype(x)>(x, 0);

			if ( x >= pVariant->getComponentAreaWidth() - 1 ) 
				x = pVariant->getComponentAreaWidth() - 1;

			y = std::max<decltype(y)>(y, 0);

			if ( y >= pVariant->getComponentAreaHeight() - 1 )
				y = pVariant->getComponentAreaHeight() - 1;

			
			if ( pDragComponent )
			{
				x2 = x + pDragComponent->getComponentWidth();
				y2 = y + pDragComponent->getComponentHeight();	

				if ( x2 > pVariant->getComponentAreaWidth() )
				{
					x = pVariant->getComponentAreaWidth() - pDragComponent->getComponentWidth();
					x2 = pVariant->getComponentAreaWidth();
				}

				if ( y2 > pVariant->getComponentAreaHeight() )
				{
					y = pVariant->getComponentAreaHeight() - pDragComponent->getComponentHeight();
					y2 = pVariant->getComponentAreaHeight();
				}
			}
			else
			{
				x2 = x + 1;
				y2 = y + 1;
			}
		
			long tmpX, tmpY;

			diagramToScreen( x, y, tmpX, tmpY );			

			// update outline rect
			if ( selRect )
				selRect->moveTo( tmpX, tmpY );			


			// highlight text if appropriate
			LogisticsComponent* pComp = pVariant->getCompAtLocation(x, y, tmpX, tmpY ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
			if ( pComp )
			{
				long compX, compY, compX2, compY2;
				diagramToScreen( tmpX, tmpY, compX, compY );
				diagramToScreen( tmpX + pComp->getComponentWidth(), tmpY + pComp->getComponentHeight(), compX2, compY2 );
				if ( (compX <= userInput->getMouseX() && compX2 >= userInput->getMouseX()
					&& compY <= userInput->getMouseY() && compY2 >= userInput->getMouseY())
					|| tmpX == -2 )
				{
					::helpTextID = IDS_HELP_COMP0 + pComp->getID();
				}
			}
		}
		else if ( selRect )
		{
			selRect->moveTo( rects[6].left() + rects[6].width()/2 - selRect->width()/2,
							rects[6].top() + rects[6].height()/2 - selRect->height()/2);
		}
		// check for jump jet hot text
		if ( x == -2 && y == -2 )
		{
			long tmpX, tmpY;
			LogisticsComponent* pComp = pVariant->getCompAtLocation(x, y, tmpX, tmpY ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
			if ( pComp )
			{
				long compX, compY, compX2, compY2;
				diagramToScreen( tmpX, tmpY, compX, compY );
				diagramToScreen( tmpX + pComp->getComponentWidth(), tmpY + pComp->getComponentHeight(), compX2, compY2 );
				if ( (compX <= userInput->getMouseX() && compX2 >= userInput->getMouseX()
					&& compY <= userInput->getMouseY() && compY2 >= userInput->getMouseY())
					|| tmpX == -2 )
				{
					::helpTextID = IDS_HELP_COMP0 + pComp->getID();
				}
			}
		}
		

		if ( pDragComponent )
		{
			if (NO_ERR == pVariant->canAddComponent( pDragComponent, x, y )
				&& x != -1 && y != -1 )
			{
					selRect->setColor( 0xff40ff80 );
			}
			else
				selRect->setColor( 0xffff0000 );			
		}			
		else if ( input::capture::startsWidgetDrag(userInput->isLeftDrag(),
			rects[13].pointInside(userInput->getMouseDragX(), userInput->getMouseDragY())) )
		{
			long i, j;
			getMouseDiagramCoords(userInput->getMouseDragX(), userInput->getMouseDragY(), i, j);

			if ( i != -1 && j != -1 )
			{
				LogisticsComponent* pComp = pVariant->getCompAtLocation(i, j, selI, selJ ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
				if ( pComp )
				{
					if ( canRemoveComponent( pComp ) )
					{
						componentListBox.SelectItem( -1 );
						beginDrag( pComp );
						pSelectedComponent = pComp;
						bDragLeft = true;
						removeComponent( i, j );
						setComponent( pComp );
						updateDiagram();
					}
					else
						refuseRemoval();
				}
				else
					pSelectedComponent = nullptr;
			}

		}
		else if ( userInput->isLeftDoubleClick() )
		{
			long tmpI, tmpJ;
			getMouseDiagramCoords( tmpI, tmpJ );

			if ( tmpI != -1 && tmpJ != -1 )
			{
				selI = tmpI;
				selJ = tmpJ;
				LogisticsComponent* pComp = pVariant->getCompAtLocation(tmpI, tmpJ, selI, selJ ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
				if ( pComp )
				{
					if ( canRemoveComponent( pComp ) )
					{
						removeComponent( selI, selJ );
						componentListBox.SelectItem( -1 );
						if ( -1 == selectFirstDiagramComponent() )
							selectFirstLBComponent();				
					}
					else
						refuseRemoval();
				}
			}
		
		}
		else if ( userInput->isLeftClick() )
		{
			long tmpI, tmpJ;
			getMouseDiagramCoords( tmpI, tmpJ );

			if ( tmpI != -1 && tmpJ != -1 )
			{
				selI = tmpI;
				selJ = tmpJ;
				LogisticsComponent* pComp = pVariant->getCompAtLocation(tmpI, tmpJ, selI, selJ ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
				if ( pComp )
				{
					pSelectedComponent = pComp;
					setComponent( pComp );
					soundSystem->playDigitalSample( LOG_SELECT );
					componentListBox.SelectItem( -1 );
				}
			}
		}		
	}

	if ( userInput->leftMouseReleased() && pDragComponent )
			endDrag();


}
void MechLabScreen::render(int xOffset, int yOffset)
{

	if ( !xOffset && !yOffset )
	{
		if ( !MPlayer && !LogisticsData::instance->isSingleMission() && LogisticsData::instance->newWeaponsAvailable() )
		{
			soundSystem->playBettySample( BETTY_NEW_WEAPONS );
			LogisticsData::instance->setNewWeaponsAcknowledged();
		}
	}


	
	// Legacy->display scale for the text bridge (crisp TTF on the component list +
	// variant dropdown, matching the data/defs UI layer).
	// UI-ASPECT-ANCHOR-1: canvas-aware transform (scale + pad origin).
	float tbSx = 1.f, tbSy = 1.f, tbOx = 0.f, tbOy = 0.f;
	aObject::getCanvasTransform( tbSx, tbSy, tbOx, tbOy );

	// DEFS-LIST-ICONS-1: full GUI bridge (not just text) so each component row's
	// icon quad reaches the ImGui layer instead of being buried under the defs
	// page (text-only AVAILABLE COMPONENTS rows with icon-sized gaps). Bridged
	// edge rows also clip to the list box (DEFS-LIST-CLIP-1).
	aObject::beginGuiBridge( tbSx, tbSy, tbOx, tbOy );
	aObject::beginTextBridge( tbSx, tbSy, 1.3f, tbOx, tbOy );
	componentListBox.move( xOffset, yOffset );
	componentListBox.render();
	componentListBox.move( -xOffset, -yOffset );
	aObject::endTextBridge();
	aObject::endGuiBridge();




	// DEFS-METER-BRIDGE-1: stat bars on the ImGui layer (raw gos draws are
	// buried under the defs page).
	aObject::beginGuiBridge( tbSx, tbSy, tbOx, tbOy );
	attributeMeters[5].render(xOffset + 176, yOffset + 11);
	attributeMeters[6].render(xOffset + 176, yOffset + 22);
	aObject::endGuiBridge();

	if ( !xOffset && !yOffset )
	{
		if ( mechCamera.hasObject() )
		{
			mechCamera.setPreviewOffscreen( true );
			mechCamera.render();
			mechCamera.drawPreviewToPanel(
				mechCamera.bounds[0] * tbSx + tbOx, mechCamera.bounds[1] * tbSy + tbOy,
				(mechCamera.bounds[2] - mechCamera.bounds[0]) * tbSx,
				(mechCamera.bounds[3] - mechCamera.bounds[1]) * tbSy );
		}
	}

	for (int i : {6, 7, 8, 9, 10, 13, 15, 16}) textObjects[i].showGUIWindow(false);
	for (int i : {16, 22, 23, 24, 26, 29, 30, 31, 34, 47, 48, 49, 50, 51, 52, 53, 54, 55})
		statics[i].showGUIWindow(false);
	LogisticsScreen::render(xOffset, yOffset );
	if (pCurComponent) {
		auto line = [&](float y, std::string_view value, std::uint32_t color) {
			GuiRuntime::DrawUiText((195 + xOffset) * tbSx + tbOx, (y + yOffset) * tbSy + tbOy,
				150 * tbSx, 12 * tbSy, value.data(), color, static_cast<int>(10 * tbSy), 0, "");
		};
		char value[96];
		line(60, pCurComponent->getName(), 0xffe8eef4);
		mc2::fmt::to_buf(value, "Cost {}", pCurComponent->getCost());
		line(73, value, 0xffffd060);
		if (pCurComponent->getType() == COMPONENT_FORM_HEATSINK)
			mc2::fmt::to_buf(value, "Cooling +{:.1f}", pCurComponent->getDamage());
		else
			mc2::fmt::to_buf(value, "Heat {:.1f}  Damage {:.1f}", pCurComponent->getHeat(), pCurComponent->getDamage());
		line(86, value, 0xffe8eef4);
		mc2::fmt::to_buf(value, "Range {}  Cycle {:.1f}s", pCurComponent->getRange(), pCurComponent->getRecycleTime());
		line(99, pCurComponent->getType() == COMPONENT_FORM_HEATSINK ? "" : value, 0xffa0c8e0);
		auto meter = [&](float x, float y, float fraction, std::uint32_t color) {
			GuiRuntime::DrawUiRect((x + xOffset) * tbSx + tbOx, (y + yOffset) * tbSy + tbOy,
				70 * tbSx, 2 * tbSy, 0xff25323e, true);
			GuiRuntime::DrawUiRect((x + xOffset) * tbSx + tbOx, (y + yOffset) * tbSy + tbOy,
				70 * tbSx * std::clamp(fraction, 0.f, 1.f), 2 * tbSy, color, true);
		};
		meter(195, 97, pCurComponent->getHeat() / LogisticsComponent::MAX_HEAT, 0xffff6040);
		meter(270, 97, pCurComponent->getDamage() / LogisticsComponent::MAX_DAMAGE, 0xff80c8ff);
		if (pCurComponent->getType() != COMPONENT_FORM_HEATSINK)
			meter(195, 110, (pCurComponent->getRange() + 1) / 3.f, 0xff80c8ff);
	}

	if ( MPlayer && ChatWindow::instance() )
		ChatWindow::instance()->render(xOffset, yOffset);

	const bool locOn = locationViewActive();
	const bool gridBridge = mechCamera.hasObject() || locOn || isPackageChassis(pVariant);
	if ( gridBridge )
		aObject::beginGuiBridge( tbSx, tbSy, tbOx, tbOy );

	// figure where components go
	if ( pVariant && !locOn )
	{
		int width = pVariant->getComponentAreaWidth();
		long minX = rects[7 + 6-width].left();
		long maxY = rects[7].bottom();
		long minY = maxY - pVariant->getComponentAreaHeight() * LogisticsComponent::YICON_FACTOR;
		// draw little component slots
		
		long x = minX;
		long y = minY;
		for ( int j = 0; j < pVariant->getComponentAreaHeight(); j++ )
		{
			x = minX;
			for ( int i = 0; i < pVariant->getComponentAreaWidth(); i++ )
			{
				// 3D mech behind: the cell is only an outline so the mech reads through the grid.
				if ( gridBridge )
				{
					const GUI_RECT cell = { x + xOffset, y + yOffset, static_cast<long>(x + xOffset + payloadIcon.width() - 1), static_cast<long>(y + yOffset + payloadIcon.height() - 1) };
					drawEmptyRect( cell, 0xb0d0d8e0, 0xb0d0d8e0 );
				}
				else
					payloadIcon.render( x + xOffset, y + yOffset );
				x += payloadIcon.width();	
			}

			y += payloadIcon.height();
		}
		
	}
	
	



	for (int i = 0; i < componentCount && !locOn; i++ )
		componentIcons[i].render(xOffset, yOffset);

	// Selection and drag outlines share storage; only the drag owns it while held.
	if ( pSelectedComponent && pVariant && !pDragComponent && !locOn )
	{
		long i = selI;
		long j = selJ;
		pVariant->getComponentLocation( pSelectedComponent, i, j );

		if ( i != -1 && j != -1 )
		{
			aObject* tmpRect = nullptr;

			if ( pSelectedComponent->getType() == COMPONENT_FORM_JUMPJET )
			{
				tmpRect = &selJumpJetRect;
				selJumpJetRect.moveTo( rects[6].left() + xOffset, rects[6].top() + yOffset );
			}

			else
			{
				long left, right;
				diagramToScreen( i, j, left, right );
				{
					int sw = std::clamp(pSelectedComponent->getComponentWidth(), 1, 2);
					int sh = std::clamp(pSelectedComponent->getComponentHeight(), 1, 5);
					tmpRect = &selRects[sw-1][sh-1];   // clamp: selRects is [2][5]
				}
				tmpRect->moveTo( left + xOffset, right + yOffset );
			}
			
		
			if ( !pDragComponent && tmpRect )
			{
				tmpRect->setColor( 0xffffffff );
				tmpRect->render();
			}


		}
		else
			pSelectedComponent = nullptr;
	}

	if ( isPackageChassis(pVariant) )
		drawLocationView( tbSx, tbSy, tbOx, tbOy, xOffset, yOffset );
	if ( gridBridge )
		aObject::endGuiBridge();
	if (!xOffset && !yOffset && mechCamera.hasObject())
		drawPreviewControls(mechCamera, !bSaveDlg && !bErrorDlg && !pDragComponent && status == RUNNING);
    // The F9 animation window is an ImGui window and would sit above the legacy save/error dialogs.
    if (!xOffset && !yOffset && !bSaveDlg && !bErrorDlg) mechCamera.drawAnimationPanel();

	aObject::beginTextBridge( tbSx, tbSy, 1.3f, tbOx, tbOy );
	variantList.move( xOffset, yOffset );
	variantList.render();
	variantList.move( -xOffset, -yOffset );
	aObject::endTextBridge();

	if ( pDragComponent )
	{
		// Always composite above the defs page, including without a 3D preview.
		aObject::beginGuiBridge( tbSx, tbSy, tbOx, tbOy );
		dragIcon.moveTo( userInput->getMouseX() - dragIcon.width() / 2, 
			userInput->getMouseY() - dragIcon.height() / 2 );
		dragIcon.render();

		const GUI_RECT footprint = {static_cast<long>(dragIcon.left()), static_cast<long>(dragIcon.top()),
			static_cast<long>(dragIcon.right()), static_cast<long>(dragIcon.bottom())};
		drawEmptyRect(footprint, 0xffffffff, 0xffffffff);
		if (rects[13].pointInside(userInput->getMouseX(), userInput->getMouseY()))
		{
			const GUI_RECT target = {static_cast<long>(selRect->left()), static_cast<long>(selRect->top()),
				static_cast<long>(selRect->right()), static_cast<long>(selRect->bottom())};
			drawEmptyRect(target, selRect->getColor(), selRect->getColor());
		}
		aObject::endGuiBridge();
	}
	if (dropRefusalTime > 0.f)
	{
		GuiRuntime::DrawUiRect(145.f * tbSx + tbOx, 540.f * tbSy + tbOy,
			510.f * tbSx, 48.f * tbSy, 0xff15212c, true);
		GuiRuntime::DrawUiText(150.f * tbSx + tbOx, 546.f * tbSy + tbOy,
			500.f * tbSx, 40.f * tbSy, dropRefusal.c_str(), 0xffffc060,
			static_cast<int>(16.f * tbSy), 0, "");
	}


	if ( bSaveDlg )
	{
		pCurDialog->render( );
		return;
	}

	else if ( bErrorDlg )
	{
		LogisticsOneButtonDialog::instance()->render();
	}








}

void MechLabScreen::updateHeatMeter()
{
	long maxHeat = pVariant->getMaxHeat();
	long curHeat = pVariant->getHeat();
	float fCurHeat = curHeat;

	bool bShowDenom = true;
	
	if ( heatTime )
	{
		if ( heatTime < .6 )
		{
			if ( oldHeat >= curHeat )
			{
				curHeat = oldHeat - (oldHeat - fCurHeat)*heatTime/.6;
				fCurHeat = curHeat;
			}
			else
			{
				curHeat = oldHeat * heatTime/.6;	
				fCurHeat = curHeat; 
			}

			if ( flashHeatRange )
				curHeat = pVariant->getHeat();

			heatTime += frameLength;

		}
		else if ( heatTime > 1.6 || oldHeat >= curHeat )
		{
			heatTime = 0; // done animating;
		}
		else
		{
			float flashTime = heatTime - .6;
			if ( flashTime < .25
				|| ( flashTime > .5 && flashTime < .75 ) )
			{
				
			}
			else
			{
				if ( flashHeatRange )
					bShowDenom = false;
				else
					fCurHeat = oldHeat;		
			}
		
			heatTime += frameLength;
		}
	}

	char text[64];
	if ( bShowDenom )
		mc2::fmt::to_buf(text, "{:d}/{:d}", mc2::fmt::as_i32(curHeat), mc2::fmt::as_i32(maxHeat));
	else
		mc2::fmt::to_buf(text, "{:d}/", mc2::fmt::as_i32(curHeat));

	textObjects[12].setText( text );
	attributeMeters[MECH_LAB_ATTRIBUTE_METER_COUNT-4].setValue( fCurHeat/static_cast<float>(maxHeat) );

}

void MechLabScreen::updateArmorMeter()
{
	long maxarmor = pVariant->getMaxArmor();
	long curarmor = pVariant->getArmor();
	float fCurarmor = curarmor;
	
	if ( armorTime )
	{
		if ( armorTime < .6 )
		{
			if ( oldArmor >= curarmor )
			{
				curarmor = oldArmor - (oldArmor - fCurarmor)*armorTime/.6;
				fCurarmor = curarmor;
			}
			else
			{
				curarmor = oldArmor * armorTime/.6;	
				fCurarmor = curarmor; 
			}
			armorTime += frameLength;

		}
		else if ( armorTime > 1.6 || oldArmor >= curarmor )
		{
			armorTime = 0; // done animating;
		}
		else
		{
			float flashTime = armorTime - .6;
			if ( flashTime < .25
				|| ( flashTime > .5 && flashTime < .75 ) )
			{
				
			}
			else
			{
				fCurarmor = oldArmor;		
			}
		
			armorTime += frameLength;
		}
	}

	char text[64];
	mc2::fmt::to_buf(text, "{:d}/{:d}", mc2::fmt::as_i32(curarmor), mc2::fmt::as_i32(maxarmor));
	textObjects[11].setText( text );
	attributeMeters[MECH_LAB_ATTRIBUTE_METER_COUNT-3].setValue( fCurarmor/static_cast<float>(maxarmor) );

}

void MechLabScreen::swapVariant()
{
	// variant changed
	varName = variantList.EditBox().entry();

	LogisticsVariant* pNewVariant = LogisticsData::instance->getVariant( varName.c_str() ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
	if ( pVariant && pNewVariant)
	{
		std::string oldName = pVariant->getName();
		*pVariant = *pNewVariant;
		pVariant->setName( oldName );
	}
	updateDiagram();
	selectFirstDiagramComponent();
}

int	MechLabScreen::handleMessage( unsigned long msg, unsigned long who)
{
	int i;

	if ( msg == aMSG_SELCHANGED )
	{
		pInspectedComponent = nullptr;
		swapVariant(); 
		return 1;
	}
	switch( who )
	{
		case MB_MSG_NEXT: // actuall accept
			{
				// make sure we have the money...
				float newCost = pVariant->getCost();
				// figure out change
				float costChange = newCost - originalCost;

				if ( !cheatInfiniteMoney() && costChange > LogisticsData::instance->getCBills() )
				{
					LogisticsOneButtonDialog::instance()->setText( IDS_MC_INSUFFICIENT_CBILLS, IDS_DIALOG_OK,
						IDS_DIALOG_OK );
					LogisticsOneButtonDialog::instance()->begin();
					bErrorDlg = true;
				}
				else
				{				
					// see if it's the same as the current variant...
					LogisticsVariant* pCurVar = LogisticsData::instance->getVariant( varName.c_str() ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
					if ( pCurVar && *pCurVar == *pVariant )
					{
						LogisticsData::instance->acceptMechModificationsUseOldVariant( pCurVar->getName().c_str() );
						status = UP;
					}
					else
					{	
						if ( pCurVar && !pCurVar->allComponentsAvailable() )
							pCurDialog = acceptDlg.get();
						else if ( pVariant && !pVariant->allComponentsAvailable() )
							pCurDialog = acceptDlg.get();
						else
							pCurDialog = saveDlg.get();
						
						pCurDialog->begin();
						bSaveDlg = true;
					}
				}
			}
			break;
		case MB_MSG_PREV: // actually cancel
			LogisticsData::instance->cancelMechModfications();
			pVariant = nullptr;
			status = UP;
			break;

		case	COMPONENT_FORM_WEAPON_ENERGY:
		case	COMPONENT_FORM_WEAPON_BALLISTIC:
		case	COMPONENT_FORM_WEAPON_MISSILE:
			for ( i = COMPONENT_FORM_WEAPON_ENERGY; i < COMPONENT_FORM_JUMPJET + 1; i++ )
			{
				aButton* pButton = getButton( i );
				if ( pButton )
				{
					pButton->press( 0 );
				}
			}
			getButton( who )->press( 1 );
			componentListBox.setType( who, -1, -1);
			break;
		case	COMPONENT_FORM_JUMPJET:
			for ( i = COMPONENT_FORM_WEAPON_ENERGY; i < COMPONENT_FORM_JUMPJET + 1; i++ )
			{
				aButton* pButton = getButton( i );
				if ( pButton )
				{
					pButton->press( 0 );
				}
			}
			getButton( who )->press( 1 );
			{
				// MC2_ENGINE_XL: surface XL engine components in the equipment tab
				// so they can be installed (they bump speed at mission load).
				componentListBox.setType( COMPONENT_FORM_JUMPJET, COMPONENT_FORM_BULK,
					COMPONENT_FORM_HEATSINK, COMPONENT_FORM_ENGINE );
			}
			break;

		case MB_MSG_ADD:
			{
			long x = -1; 
			long y = -1;
			addComponent( componentListBox.getComponent(), x, y );
			}
			break;

		case MB_MSG_REMOVE:
			if ( pSelectedComponent && (selI != -1 && selJ != -1) )
			{
				removeComponent( selI, selJ );
				pSelectedComponent = nullptr;
				if ( -1 == selectFirstDiagramComponent() )
					selectFirstLBComponent();
			}

			updateDiagram();

			break;

		case MB_MSG_MAINMENU:
			status = MAINMENU;
			break;



	
	}

	return 0;
}

void MechLabScreen::inspectComponent( LogisticsComponent* c )
{
	pInspectedComponent = c;
	if ( c ) showDropRefusal( mc2::components::placementRefusal( canAddComponent(c) ) );
}

void	MechLabScreen::setComponent( LogisticsComponent* pComponent, bool bMessageFromLB )
{
	if ( bMessageFromLB && canAddComponent( pComponent ) ) // if they clicked on a disabled item
	{
		if ( !locationViewActive() ) // location view keeps it selected: every panel shows why it refuses
		{
			componentListBox.SelectItem( -1 );
			selI = selJ = -1;
			pSelectedComponent = nullptr;
		}

		getButton( MB_MSG_ADD )->disable( true );
	}

	if ( pComponent && pComponent == pCurComponent )
		return;

	pCurComponent = pComponent;

	// ammo doesn't deserve a meter
	attributeMeters[3].showGUIWindow( 0 );

	char text[64];
	if ( pCurComponent )
	{
		textObjects[6].setText( pComponent->getName() );


		// first figure out if this is a jump jet or not
		if ( pCurComponent->getType() == COMPONENT_FORM_JUMPJET )
		{
			showJumpJetItems( 1 );


			// set up jump range
			attributeMeters[4].setValue((static_cast<float>(pVariant->getMaxJumpRange()) * 25.0f) / MAX_JUMP_RANGE );
			mc2::fmt::to_buf(text, "{:d}", mc2::fmt::as_i32(pVariant->getMaxJumpRange() * 25));
			textObjects[7].setText( text );
			textObjects[7].setColor( 0xff005392 );
			rects[16].setHelpID( IDS_HELP_PM_JUMP );

	
			// set up heat
			attributeMeters[7].setValue(pCurComponent->getHeat()/LogisticsComponent::MAX_HEAT);
			attributeMeters[7].showGUIWindow( true );
			
			mc2::fmt::to_buf(text, "{:.1f}", pCurComponent->getHeat());
			textObjects[15].setText( text );
			
			textObjects[8].setText( "" );
			textObjects[9].setText( "" );
			textObjects[10].setText( "" );
			textObjects[13].setText( "" );
			rects[17].setHelpID( IDS_HELP_MC_HEAT_GENERATED );


			attributeMeters[0].showGUIWindow( false );

			rects[18].setHelpID( 0 );
			rects[19].setHelpID( 0 );
			rects[20].setHelpID( 0 );


			

		}
		else if ( pCurComponent->getType() == COMPONENT_FORM_HEATSINK )
		{
			showJumpJetItems( 1 );


			// hide jump range stuff
			attributeMeters[4].showGUIWindow( 0 );
			textObjects[7].showGUIWindow( 0 );
			statics[47].showGUIWindow( 0 );

			textObjects[15].setText( "" );

			// jump range
			statics[48].showGUIWindow( 0 );
			// armor
			statics[49].showGUIWindow( 0 );
			// damage
			statics[30].showGUIWindow( 0 );
			// heat
			statics[55].showGUIWindow( 1 );	


			attributeMeters[7].showGUIWindow( 0 );
	
			// set up heat, negative heat is stored in the damage area of heat sinks, god knows why
			attributeMeters[8].setValue(pCurComponent->getDamage()/LogisticsComponent::MAX_HEAT);
			attributeMeters[8].showGUIWindow( true );
			
			mc2::fmt::to_buf(text, "+{:.1f}", pCurComponent->getDamage());
			textObjects[16].setText( text );
			textObjects[16].showGUIWindow( true );
			
			textObjects[8].setText( "" );
			textObjects[9].setText( "" );
			textObjects[10].setText( "" );
			textObjects[13].setText( "" );

			attributeMeters[0].showGUIWindow( false );

			rects[16].setHelpID( IDS_HELP_MC_HEAT_GENERATED );
			rects[17].setHelpID( 0 );
			rects[18].setHelpID( 0 );
			rects[19].setHelpID( 0 );
			rects[20].setHelpID( 0 );
		}
		else if ( pCurComponent->getType() == COMPONENT_FORM_BULK )
		{	
			showJumpJetItems( 1 );
			
			// jump range
			statics[48].showGUIWindow( 0 );
			// armor
			statics[49].showGUIWindow( 1 );
			// damage
			statics[30].showGUIWindow( 0 );
			
			mc2::fmt::to_buf(text, "{:.1f}", pCurComponent->getDamage());
			textObjects[7].setText( text );
			textObjects[7].setColor( 0xff005392 );
			textObjects[8].setText( "" );
			textObjects[9].setText( "" );
			textObjects[10].setText( "" );
			textObjects[15].setText( "" );

			textObjects[13].setText( "" );
			attributeMeters[7].showGUIWindow( false );

			for( int i = 0; i < 4; i++ )
			{
				attributeMeters[i].showGUIWindow( 0 );
			}

			rects[16].setHelpID( IDS_HELP_MC_ARMOR );
			rects[17].setHelpID( 0 );
			rects[18].setHelpID( 0 );
			rects[19].setHelpID( 0 );
			rects[20].setHelpID( 0 );

		}
		else if ( pCurComponent->getType() == COMPONENT_FORM_ENGINE )
		{
			// XL engine upgrade: not a weapon — show a minimal safe panel and do
			// NOT fall into the weapon branch (which indexes rangeColors[] by
			// getRangeType(), == NO_RANGE(3) for an engine => out of bounds).
			showJumpJetItems( 1 );
			attributeMeters[4].showGUIWindow( 0 );
			textObjects[7].setText( "" );
			textObjects[8].setText( "" );
			textObjects[9].setText( "" );
			textObjects[10].setText( "" );
			textObjects[13].setText( "" );
			textObjects[15].setText( "" );
			for ( int i = 0; i < 4; i++ )
				attributeMeters[i].showGUIWindow( 0 );
			attributeMeters[7].showGUIWindow( false );
			rects[16].setHelpID( 0 );
			rects[17].setHelpID( 0 );
			rects[18].setHelpID( 0 );
			rects[19].setHelpID( 0 );
			rects[20].setHelpID( 0 );
		}
		else
		{
			std::array<uint32_t, 3> rangeColors = {0xff6e7c00, 0xff005392,0xffa21600  };

			showJumpJetItems( 0 );
			textObjects[7].setText( pCurComponent->getRangeType() + IDS_SHORT_RANGE );
			textObjects[7].setColor( rangeColors[pCurComponent->getRangeType()] );


			attributeMeters[0].showGUIWindow( true );
			attributeMeters[0].setValue(pCurComponent->getDamage()/LogisticsComponent::MAX_DAMAGE);
			mc2::fmt::to_buf(text, "{:.1f}", pCurComponent->getDamage());
			textObjects[8].setText( text );
			
			if ( pCurComponent->getRecycleTime() )
			{
				attributeMeters[1].setValue((10.f/pCurComponent->getRecycleTime())/LogisticsComponent::MAX_RECYCLE);
				mc2::fmt::to_buf(text, "{:.1f}", 10.f/pCurComponent->getRecycleTime());
				textObjects[9].setText( text );
			}
			else
			{
				attributeMeters[1].setValue( 0.f );
				mc2::fmt::to_buf(text, "{:.1f}", 0.f);
				textObjects[9].setText( text );
			}
			

			attributeMeters[2].setValue(pCurComponent->getHeat()/LogisticsComponent::MAX_HEAT);
			mc2::fmt::to_buf(text, "{:.1f}", pCurComponent->getHeat());
			textObjects[10].setText( text );

			attributeMeters[7].showGUIWindow( false );

			if ( !useUnlimitedAmmo )
			{
				mc2::fmt::to_buf(text, "{:d}", mc2::fmt::as_i32(pCurComponent->getAmmo()));
				textObjects[13].setText( text );
				attributeMeters[3].setValue( (static_cast<float>(pCurComponent->getAmmo()))/164.f );
			}
			else
				textObjects[13].setText( "" );

			textObjects[15].setText( "" );

			rects[16].setHelpID( IDS_HELP_MC_RANGE );
			rects[17].setHelpID( IDS_HELP_MC_DAMAGE );
			rects[18].setHelpID( IDS_HELP_MC_FIRE_RATE );
			rects[19].setHelpID( IDS_HELP_MC_HEAT_GENERATED );
			if ( !prefs.useUnlimitedAmmo )
				rects[20].setHelpID( IDS_HELP_MC_AMMO );
			else
				rects[20].setHelpID( 0 );
			
		}
	}
	else
	{
		for ( int i = 7; i < 11; i++ )
		{
			textObjects[i].setText( "" );
		}

		textObjects[13].setText( "" );

		for (int i = 0; i < 5; i++ )
		{
			attributeMeters[i].setValue( 0 );
			attributeMeters[i].showGUIWindow( 0 );
		}
	}
}

void MechLabScreen::showJumpJetItems( bool bShow )
{
		// show jump jet stuff
		statics[47].showGUIWindow( bShow );
		statics[48].showGUIWindow( bShow );
		statics[49].showGUIWindow( 0 );
		attributeMeters[4].showGUIWindow( bShow );
		attributeMeters[0].showGUIWindow( 1 );
		statics[30].showGUIWindow( !bShow ); // damage

		textObjects[7].showGUIWindow( 1 );

		// hide all heat sink stuff, regardless of who
		attributeMeters[8].showGUIWindow( 0 );
		textObjects[16].showGUIWindow( 0 );
		statics[55].showGUIWindow( 0 );
		
		// turn off weapon stuff
		for ( int i = 31; i < 35; i++ )
		{
			statics[i].showGUIWindow( !bShow );
		}

		// but not the heat one!
		statics[33].showGUIWindow( true );

		// turn off ammo thing if appropriate
		if (!useUnlimitedAmmo && !bShow)
		{
			statics[34].showGUIWindow( 1 );
			attributeMeters[3].showGUIWindow( 1 );
		}
		else
		{
			statics[34].showGUIWindow( 0 );
			attributeMeters[3].showGUIWindow( 0 );
		}

		// weapon stuff
		for (int i = 1; i < 3; i++ )
		{
			attributeMeters[i].showGUIWindow( !bShow );
		}
}

int MechLabScreen::canAddComponent( LogisticsComponent* pComponent )
{
	if ( !pVariant )
		return 0;

	long i = -1;
	long j = -1;
	int retVal = pVariant->canAddComponent( pComponent, i, j );

	return retVal;
}


int MechLabScreen::addComponent( LogisticsComponent* pComponent, long& x, long& y )
{
	if ( !pComponent )
		return -1;

	int retVal = -1;

	if ( pComponent->getHeat() || pComponent->getType() == COMPONENT_FORM_HEATSINK )
	{
		oldHeat = pVariant->getHeat();
		if ( pComponent->getType() == COMPONENT_FORM_HEATSINK )
		{
			oldHeat -= 1;
			flashHeatRange = true;
		}
		else
			flashHeatRange = false;
		heatTime = .0001f;
	}
	else if ( pComponent->getType() == COMPONENT_FORM_BULK )
	{
		oldArmor = pVariant->getArmor();
		armorTime = .0001f;
	}
	
	if ( pComponent )
		retVal = pVariant->addComponent( pComponent, x, y );

	// Every entry (drop, double-click, Add button) says why it was refused.
	if ( retVal != NO_ERR )
		showDropRefusal( mc2::components::placementRefusal( retVal ) );

	updateDiagram();

	return retVal;

}

void MechLabScreen::beginDrag( LogisticsComponent* pComponent )
{
	pInspectedComponent = nullptr;
	if ( !pDragComponent && !dragCancelled )
	{
		pDragComponent = pComponent;

		if ( pDragComponent )
		{
			//initialize the drag object
			int sizeX = pComponent->getComponentWidth();
			int sizeY = pComponent->getComponentHeight();
			
			const char* pFile = pComponent->getIconFileName();

			FullPathFileName path;
			path.init( artPath, pFile, "tga" );

			dragIcon.setTexture( path );
			dragIcon.setComponent(*pComponent);
			dragIcon.resize( sizeX * ml::kCellW, sizeY * LogisticsComponent::YICON_FACTOR);
			dragIcon.setUVs( 0.f, 0.f, sizeX * 48.f, sizeY * 32.f );
			dragIcon.setColor( 0xffffffff );

			{
				int cw = std::clamp(sizeX, 1, 2);
				int ch = std::clamp(sizeY, 1, 5);
				selRect = &selRects[cw-1][ch-1];   // clamp: selRects is [2][5]
			}

			if ( pComponent->getType() == COMPONENT_FORM_JUMPJET )
			{
				selRect = &selJumpJetRect;
			}

		}
	}
}

void MechLabScreen::endDrag( )
{
	if ( pDragComponent && locationViewActive() )
	{
		endDragLocation();
		return;
	}

	float mouseX = userInput->getMouseX();
	float mouseY = userInput->getMouseY();
	if ( pDragComponent && rects[13].pointInside( mouseX, mouseY ) )
	{
		long x, y;

		// use selRect, it already has proper coords
		getMouseDiagramCoords( selRect->left(), selRect->top(), x, y );
		
		if ( mc2::components::finishGridDrop(x, y, bDragLeft, selI, selJ,
			[this](long& i, long& j) {
				return addComponent(pDragComponent, i, j);
			}) )
		{
			componentListBox.SelectItem( -1 );
			selI = x;
			selJ = y;
			pSelectedComponent = pDragComponent;
		}

		updateDiagram();
 	
	}
	else
	{
		if (mc2::components::restoreGridDrag(bDragLeft, false,
			componentListBox.pointInside(mouseX, mouseY), false))
		{
			addComponent(pDragComponent, selI, selJ);
			pSelectedComponent = pDragComponent;
		}
		if ( -1 == componentListBox.GetSelectedItem() )
		{
			if ( -1 == selI )
				selectFirstLBComponent();
		}
	}

	pDragComponent = nullptr;
	bDragLeft = 0;

	
}

void MechLabScreen::getMouseDiagramCoords( long& x, long& y )
{

	long mouseX = userInput->getMouseX();
	long mouseY = userInput->getMouseY();

	getMouseDiagramCoords( mouseX, mouseY, x, y);

}

void MechLabScreen::getMouseDiagramCoords( long screenX, long screenY, long& x, long& y )
{
	x = -1; 
	y = -1;
	
	// get position under cursor
	int width = pVariant->getComponentAreaWidth();
	int height = pVariant->getComponentAreaHeight();
	
	long minX = rects[7 + 6-width].left();
	long maxY = rects[7].bottom();
	long minY = maxY - height * LogisticsComponent::YICON_FACTOR;
	long maxX = minX + width * ml::kCellW;

	long mouseX = screenX;
	long mouseY = screenY;

	// are we in the right area?
	if ( mouseX >= rects[6].left() 
				&& mouseX <= rects[6].right()
				&& mouseY >= rects[6].top() 
				&& mouseY <= rects[6].bottom() 
				&& pVariant->getChassis()->jumpJetsAllowed() )
	{
		x = -2;
		y = -2;
	}
	else
//		if ( mouseX >= minX && mouseX <= maxX
//		&& mouseY >= minY && mouseY <= maxY )
	{
		// specificly, which square are we in
		x = (mouseX - minX)/ml::kCellW;
		y = (mouseY - minY)/LogisticsComponent::YICON_FACTOR;
	}
	
}

void MechLabScreen::diagramToScreen( long i, long j, long& x, long& y  )
{
	if ( i == -2 && j == -2 )
	{
		x = rects[6].left();
		y = rects[6].top();
		return;
	}

	if ( !pVariant )
	{
		x = 0;
		y = 0;
		return;
	}

	int height = pVariant->getComponentAreaHeight();
	int width = pVariant->getComponentAreaWidth();
	
	long minX = rects[7 + 6-width].left();
	long maxY = rects[7].bottom();
	long minY = maxY - height * LogisticsComponent::YICON_FACTOR;

	x = minX + i * ml::kCellW;
	y = minY + j * LogisticsComponent::YICON_FACTOR;


}


void MechLabScreen::updateDiagram()
{
	if ( !pVariant || status != RUNNING )
		return;

	int width = pVariant->getComponentAreaWidth();
	long minX = rects[7 + 6-width].left();
	long maxY = rects[7].bottom();

	long minY = maxY - pVariant->getComponentAreaHeight() * LogisticsComponent::YICON_FACTOR;

	long IDArray[128];
	long xLocs[128];
	long yLocs[128];
	componentCount = 128;


	pVariant->getComponentsWithLocation(  componentCount, IDArray, xLocs, yLocs );
	for ( int i = 0; i < componentCount; i++ )
	{
		LogisticsComponent* pComponent = LogisticsData::instance->getComponent( IDArray[i] ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);

		long xLoc = minX + xLocs[i] * ml::kCellW;
		long yLoc = minY + yLocs[i] * LogisticsComponent::YICON_FACTOR;

		if ( pComponent->getType() == COMPONENT_FORM_JUMPJET )
		{
			xLoc =	rects[6].left();
			yLoc = rects[6].top();
			 
		}

		int sizeX = pComponent->getComponentWidth();
		int sizeY = pComponent->getComponentHeight();
	

		const char* pFile = pComponent->getIconFileName();

		FullPathFileName path;
		path.init( artPath, pFile, "tga" );

		componentIcons[i].setTexture( path );
		componentIcons[i].setComponent(*pComponent);
		componentIcons[i].resize( sizeX * ml::kCellW, sizeY * LogisticsComponent::YICON_FACTOR );
		componentIcons[i].setUVs( 0.f, 0.f, sizeX * 48.f, sizeY * 32.f );
		componentIcons[i].moveTo( xLoc, yLoc );
		componentIcons[i].setColor( 0xffffffff );
	}

	// update heat
	updateHeatMeter();
	updateArmorMeter();

	// update cost
	float newCost = pVariant->getCost();

	// figure out change
	float costChange = newCost - originalCost;

	if ( costChange != oldCBillsAmount )
	{
		previousAmount = oldCBillsAmount - costChange;
		curCount = .00001f;
		oldCBillsAmount = costChange;
		if ( previousAmount < 0 )
			soundSystem->playDigitalSample( WINDOW_OPEN );
		else
			soundSystem->playDigitalSample( WINDOW_CLOSE );
	}




}

int MechLabScreen::selectFirstDiagramComponent()
{
	if ( !pVariant )
		return -1;
	int count = 0;
	bool bFound = 0;
	componentListBox.SelectItem( -1 );
	for ( int j = 0; j < pVariant->getComponentAreaHeight(); j++ )
	{
		for ( int i = 0; i < pVariant->getComponentAreaWidth(); i++ )
		{
			LogisticsComponent* pComp = pVariant->getCompAtLocation(i, j, selI, selJ ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
			if ( pComp )
			{
				count++;
				bFound = true;
				pSelectedComponent = pComp;

				break;
			}
		}
		if ( bFound )
			break;
	}

	if ( !bFound )
	{
		selI = -1;
		selJ = -1;
		return -1;
	}

	return count;
}
int MechLabScreen::selectFirstLBComponent()
{
	selI = selJ = -1;
	pSelectedComponent = nullptr;
	int index =  componentListBox.selectFirstAvailableComponent();
	return index;
}
void MechLabScreen::removeComponent( long i, long j )
{

	long tmpX, tmpY;
	LogisticsComponent* pComp = pVariant->getCompAtLocation(i, j, tmpX, tmpY ).transform([](auto ref) { return &ref.get(); }).value_or(nullptr);
	if ( pComp )
	{
		if ( !pComp->isAvailable() )
		{
			LogisticsOneButtonDialog::instance()->setText(IDS_MC_UNAVAILABLE_TECH, IDS_DIALOG_OK, IDS_DIALOG_OK);
			LogisticsOneButtonDialog::instance()->begin();
			bErrorDlg = true;
		}
	}
	long tmpHeat = pVariant->getHeat();
	long tmpArmor = pVariant->getArmor();
	pVariant->removeComponent( i, j );

	if ( tmpHeat != pVariant->getHeat() )
	{
		oldHeat = tmpHeat;
		heatTime = .0001f;
	}
	else if ( tmpArmor != pVariant->getArmor() )
	{
		oldArmor = tmpArmor;
		armorTime = .0001f;
	}
	updateDiagram();
			
}
bool MechLabScreen::canRemoveComponent( LogisticsComponent* pComp )
{
	if ( pComp && pComp->getType() == COMPONENT_FORM_HEATSINK )
	{
		if ( !mc2::components::coolingAllowsRemoval(pVariant->getMaxHeat(),
			pComp->getDamage(), pVariant->getHeat()) )
			return 0;
	}

	return 1;
}


//*************************************************************************************************
// end of file ( MechLabScreen.cpp )

//*************************************************************************************************
// MECHLAB-LOC-1: location view (package / BT2018 chassis). Layout, hit rects and refusal reasons
// are the pure code/mechlab_loc_layout.h; every rect below is drawn AND hit-tested from it.
//*************************************************************************************************
namespace {
struct LocItem { LogisticsComponent* comp; long x, y; };
int s_locHover = -1;
bool s_locLegal[ml::kPanelCount] = {};
std::string s_locReason[ml::kPanelCount];

const mc2pkg::PackageChassis* locPackage(const LogisticsVariant* v)
{
	return (v && v->getChassis()) ? v->getChassis()->getPackage() : nullptr;
}
std::optional<int> regionOfPanel(const mc2pkg::PackageChassis* pc, int panel)
{
	for (size_t k = 0; pc && k < pc->regions.size(); ++k)
		if (pc->regions[k].location == ml::panelName(panel)) return static_cast<int>(k);
	return std::nullopt;
}
void gatherPlaced(LogisticsVariant* v, std::vector<mc2pkg::Placed>& placed, std::vector<LogisticsComponent*>& comps)
{
	placed.clear();
	comps.clear();
	long ids[128], xs[128], ys[128];
	LogisticsComponent* arr[128];
	long n = 128, n2 = 128;
	if (v->getComponentsWithLocation(n, ids, xs, ys) != 0 || v->getComponents(n2, arr) != 0) return;
	for (long i = 0; i < n && i < n2; ++i)
	{
		mc2pkg::Placed p;
		p.x = static_cast<int>(xs[i]); p.y = static_cast<int>(ys[i]);
		p.w = arr[i]->getComponentWidth(); p.h = arr[i]->getComponentHeight();
		switch (arr[i]->getType())
		{
			case COMPONENT_FORM_WEAPON_ENERGY: case COMPONENT_FORM_WEAPON: p.category = mc2pkg::Mount::Energy; break;
			case COMPONENT_FORM_WEAPON_BALLISTIC: p.category = mc2pkg::Mount::Ballistic; break;
			case COMPONENT_FORM_WEAPON_MISSILE: p.category = mc2pkg::Mount::Missile; break;
			default: break;
		}
		p.ordinal = arr[i]->getID();
		p.weight = arr[i]->getWeight();
		placed.push_back(p);
		comps.push_back(arr[i]);
	}
}
void gatherPanelItems(LogisticsVariant* v, int panel, std::vector<LocItem>& out)
{
	out.clear();
	const mc2pkg::PackageChassis* pc = locPackage(v);
	const int ri = regionOfPanel(pc, panel).value_or(-1);
	if (ri < 0) return;
	std::vector<mc2pkg::Placed> placed;
	std::vector<LogisticsComponent*> comps;
	gatherPlaced(v, placed, comps);
	for (size_t i = 0; i < placed.size(); ++i)
		if (placed[i].x >= 0 && pc->regions[ri].contains(placed[i].x, placed[i].y))
			out.push_back({comps[i], placed[i].x, placed[i].y});
}
void refreshLocationVerdicts(LogisticsVariant* v, LogisticsComponent* cand)
{
	const mc2pkg::PackageChassis* pc = locPackage(v);
	for (int p = 0; p < ml::kPanelCount; ++p) {
		s_locLegal[p] = false;
		s_locReason[p].clear();
		if (!cand || !pc || p >= ml::kLocationCount) continue;
		const int ri = regionOfPanel(pc, p).value_or(-1);
		if (cand->getType() == COMPONENT_FORM_JUMPJET) {
			long x = -1, y = -1;
			const int refusal = v->canAddComponent(cand, x, y);
			s_locReason[p] = refusal == JUMPJETS_NOT_ALLOWED ? "No jump jets" :
				refusal == ONLY_ONE_JUMPJET_ALLOWED ? "Jets installed" :
				refusal ? mc2::components::placementRefusal(refusal) : "ADD special slot";
		}
		else if (ri < 0) s_locReason[p] = "no slots here";
		else {
			auto place = v->packageLocationPlace(cand, ri);
			s_locLegal[p] = place.legal;
			s_locReason[p] = std::move(place.reason);
		}
	}
}
const char* mountLetter(mc2pkg::Mount m)
{
	switch (m) {
		case mc2pkg::Mount::Energy: return "E";
		case mc2pkg::Mount::Ballistic: return "B";
		case mc2pkg::Mount::Missile: return "M";
		case mc2pkg::Mount::AntiPersonnel: return "A";
		default: return "?";
	}
}
}

bool MechLabScreen::locationViewActive() const
{
	return locPackage(pVariant) && mc2::options::current().mechLabLocations;
}

void MechLabScreen::setLocationView( bool on )
{
	if ( mc2::options::current().mechLabLocations == on || pDragComponent )
		return;
	const auto previous = prefs.render;
	prefs.render.mechLabLocations = on;
	if ( !prefs.save() )
		prefs.render = previous;
	auto options = mc2::options::current();
	options.mechLabLocations = on;
	mc2::options::publish( options );
	updateDiagram();
}

bool MechLabScreen::placeInPanel( LogisticsComponent* pComponent, int panel, long& x, long& y )
{
	const mc2pkg::PackageChassis* pc = locPackage( pVariant );
	const int ri = regionOfPanel( pc, panel ).value_or( -1 );
	std::string reason;
	LogisticsVariant::LocationPlace place;
	if ( ri >= 0 )
	{
		place = pVariant->packageLocationPlace( pComponent, ri );
		reason = place.reason;
	}
	x = place.x; y = place.y;
	if ( ri < 0 || !place.legal )
	{
		showDropRefusal( std::string( ml::panelTitle( panel ) ) + ": " + ( ri < 0 ? "no equipment slots here." : reason ) );
		soundSystem->playDigitalSample( LOG_WRONGBUTTON );
		return false;
	}
	return 0 == addComponent( pComponent, x, y );
}

void MechLabScreen::endDragLocation()
{
	const int panel = ml::panelAt( static_cast<float>( userInput->getMouseX() ), static_cast<float>( userInput->getMouseY() ) ).value_or( -1 );
	long x = -1, y = -1;
	bool placed = false;
	if ( panel >= 0 && panel < ml::kLocationCount )
		placed = placeInPanel( pDragComponent, panel, x, y );
	if ( placed )
	{
		componentListBox.SelectItem( -1 );
		selI = x;
		selJ = y;
		pSelectedComponent = pDragComponent;
	}
	else if ( mc2::components::restoreGridDrag( bDragLeft, false,
		componentListBox.pointInside( userInput->getMouseX(), userInput->getMouseY() ), false ) )
	{
		addComponent( pDragComponent, selI, selJ );
		pSelectedComponent = pDragComponent;
	}
	else if ( -1 == componentListBox.GetSelectedItem() && -1 == selI )
		selectFirstLBComponent();
	updateDiagram();
	pDragComponent = nullptr;
	bDragLeft = 0;
}

void MechLabScreen::updateLocationInput()
{
	auto refuseRemoval = [this]() {
		showDropRefusal("Not enough cooling: remove weapons first.");
		soundSystem->playDigitalSample( LOG_WRONGBUTTON );
	};
	if ( pDragComponent && ( gos_GetKeyStatus( KEY_LMOUSE ) == KEY_FREE ||
		userInput->isRightClick() || userInput->getKeyDown( KEY_ESCAPE ) ) )
	{
		if ( mc2::components::restoreGridDrag( bDragLeft, false, false, true ) )
		{
			addComponent( pDragComponent, selI, selJ );
			pSelectedComponent = pDragComponent;
		}
		pDragComponent = nullptr;
		bDragLeft = false;
		dragCancelled = true;
		updateDiagram();
		return;
	}
	if ( dragCancelled )
		return;
	const float mx = static_cast<float>( userInput->getMouseX() );
	const float my = static_cast<float>( userInput->getMouseY() );
	LogisticsComponent* cand = pDragComponent ? pDragComponent
		: ( pInspectedComponent ? pInspectedComponent : ( -1 != componentListBox.GetSelectedItem() ? componentListBox.getComponent() : nullptr ) );
	refreshLocationVerdicts( pVariant, cand );
	s_locHover = ml::panelAt( mx, my ).value_or( -1 );

	std::vector<LocItem> items;
	const int dragPanel = ml::panelAt( static_cast<float>( userInput->getMouseDragX() ), static_cast<float>( userInput->getMouseDragY() ) ).value_or( -1 );
	if ( !pDragComponent && dragPanel >= 0 && dragPanel < ml::kLocationCount &&
		input::capture::startsWidgetDrag( userInput->isLeftDrag(), true ) )
	{
		std::vector<LocItem> origin;
		gatherPanelItems( pVariant, dragPanel, origin );
		const int orow = ml::itemRowAt( dragPanel, static_cast<float>( userInput->getMouseDragX() ), static_cast<float>( userInput->getMouseDragY() ), static_cast<int>( origin.size() ) ).value_or( -1 );
		if ( orow >= 0 && orow < static_cast<int>( origin.size() ) )
		{
			const LocItem it = origin[orow];
			if ( canRemoveComponent( it.comp ) )
			{
				selI = it.x;
				selJ = it.y;
				componentListBox.SelectItem( -1 );
				beginDrag( it.comp );
				pSelectedComponent = it.comp;
				bDragLeft = true;
				removeComponent( it.x, it.y );
				setComponent( it.comp );
				updateDiagram();
			}
			else
				refuseRemoval();
		}
	}
	if ( s_locHover >= 0 && s_locHover < ml::kLocationCount )
	{
		gatherPanelItems( pVariant, s_locHover, items );
		const int row = ml::itemRowAt( s_locHover, mx, my, static_cast<int>( items.size() ) ).value_or( -1 );
		const LocItem* item = ( row >= 0 && row < static_cast<int>( items.size() ) ) ? &items[row] : nullptr;
		if ( item )
			::helpTextID = IDS_HELP_COMP0 + item->comp->getID();
		if ( userInput->isLeftDoubleClick() )
		{
			if ( item )
			{
				if ( canRemoveComponent( item->comp ) )
				{
					removeComponent( item->x, item->y );
					componentListBox.SelectItem( -1 );
					pSelectedComponent = nullptr;
					selI = selJ = -1;
					if ( -1 == selectFirstDiagramComponent() )
						selectFirstLBComponent();
				}
				else
					refuseRemoval();
			}
		}
		else if ( userInput->isLeftClick() )
		{
			if ( item )
			{
				selI = item->x;
				selJ = item->y;
				pInspectedComponent = nullptr;
				pSelectedComponent = item->comp;
				setComponent( item->comp );
				soundSystem->playDigitalSample( LOG_SELECT );
				componentListBox.SelectItem( -1 );
			}
			else if ( cand )
			{
				long x = -1, y = -1;
				if ( placeInPanel( cand, s_locHover, x, y ) )
				{
					selI = x; selJ = y;
					pSelectedComponent = cand;
					pInspectedComponent = nullptr;
					componentListBox.SelectItem( -1 );
				}
			}
		}
	}
	if ( userInput->leftMouseReleased() && pDragComponent )
		endDrag();
}

void MechLabScreen::drawLocationView( float tbSx, float tbSy, float tbOx, float tbOy, int xOffset, int yOffset )
{
	const ml::Canvas cv{ tbSx, tbSy, tbOx, tbOy };
	const mc2pkg::PackageChassis* pc = locPackage( pVariant );
	if ( !pc )
		return;
	auto rectOf = [&]( ml::Rect r ) { r.x += xOffset; r.y += yOffset; return ml::toScreen( r, cv ); };
	auto text = [&]( ml::Rect r, std::string s, std::uint32_t color, float px, int align ) {
		const ml::Rect sr = rectOf( r );
		GuiRuntime::DrawUiText( sr.x, sr.y, sr.w, sr.h, s.c_str(), color, static_cast<int>( px * tbSy ), align, "" );
	};
	auto fill = [&]( ml::Rect r, std::uint32_t color, bool filled ) {
		const ml::Rect sr = rectOf( r );
		GuiRuntime::DrawUiRect( sr.x, sr.y, sr.w, sr.h, color, filled );
	};
	LogisticsComponent* cand = pDragComponent ? pDragComponent
		: ( pInspectedComponent ? pInspectedComponent : ( -1 != componentListBox.GetSelectedItem() ? componentListBox.getComponent() : nullptr ) );

	std::vector<mc2pkg::Placed> placed;
	std::vector<LogisticsComponent*> comps;
	gatherPlaced( pVariant, placed, comps );
	const float mass = mc2pkg::massUsed( placed );
	// mounts in use per hardpoint: the same deterministic assignment deployment uses.
	const std::vector<int> mountOf = mc2pkg::assignMounts( *pc, placed );
	std::vector<bool> hpUsed( pc->hardpoints.size(), false );
	for ( int m : mountOf )
		if ( m >= 0 ) hpUsed[m] = true;

	// toggle (package chassis only; stock chassis keep the grid and show none)
	{
		const ml::Rect t = ml::toggleRect();
		const bool loc = mc2::options::current().mechLabLocations;
		fill( t, 0xff15212c, true );
		fill( { t.x, t.y, t.w * 0.5f, t.h }, loc ? 0xff2d6a8f : 0xff15212c, true );
		fill( { t.x + t.w * 0.5f, t.y, t.w * 0.5f, t.h }, loc ? 0xff15212c : 0xff2d6a8f, true );
		fill( t, 0xffd0d8e0, false );
		text( { t.x, t.y + 2, t.w * 0.5f, t.h }, "LOCATIONS", 0xffffffff, 11.f, 2 );
		text( { t.x + t.w * 0.5f, t.y + 2, t.w * 0.5f, t.h }, "CLASSIC", 0xffffffff, 11.f, 2 );
	}
	if ( !mc2::options::current().mechLabLocations )
		return;

	for ( int p = 0; p < ml::kPanelCount; ++p )
	{
		const ml::Rect r = ml::panelRect( p );
		const bool doll = p == ml::kDoll;
		const int ri = doll ? -1 : regionOfPanel( pc, p ).value_or( -1 );
		const bool hasSlots = ri >= 0;
		fill( r, 0xe8101820, true );
		std::uint32_t edge = 0xff5a6a78;
		if ( cand && !doll )
			edge = s_locLegal[p] ? 0xff40ff80 : 0xffff4040;
		else if ( p == s_locHover && !doll )
			edge = 0xffd0d8e0;
		fill( r, edge, false );
		if ( doll )
		{
			int special = 0;
			for ( const auto& it : placed ) if ( it.x < 0 || it.y < 0 ) ++special;
			if ( special && r.h > 40 ) {
				text( { r.x + 3, r.y + 4, r.w - 6, 14 }, "+ special (CLASSIC)", 0xffe8eef4, 10.f, 0 );
				text( { r.x + 3, r.y + 20, r.w - 6, 28 }, "Jump jets / special slots", 0xffa0b0c0, 10.f, 0 );
			}
			char buf[64];
			std::snprintf( buf, sizeof buf, "MASS %.1f / %.1f t", static_cast<double>( mass ), static_cast<double>( pc->payload ) );
			text( { r.x, r.y + r.h - 14, r.w, 14 }, buf, mass > pc->payload + 1e-3f ? 0xffff6060 : 0xffffd060, 11.f, 2 );
			continue;
		}
		int cells = 0, used = 0;
		if ( hasSlots ) {
			cells = pc->regions[ri].w * pc->regions[ri].h;
			for ( const mc2pkg::Placed& it : placed )
				if ( it.x >= 0 && pc->regions[ri].contains( it.x, it.y ) ) used += it.w * it.h;
		}
		char head[48];
		if ( hasSlots )
			std::snprintf( head, sizeof head, "%s %d/%d", ml::panelTitle( p ), used, cells );
		else
			std::snprintf( head, sizeof head, "%s", ml::panelTitle( p ) );
		text( { r.x + 3, r.y + 2, r.w - 6, 13 }, head, hasSlots ? 0xffffffff : 0xff808890, 11.f, 0 );
		float bx = r.x + r.w - 3.f;
		for ( size_t k = pc->hardpoints.size(); k-- > 0; )
			if ( pc->hardpoints[k].location == ml::panelName( p ) )
			{
				bx -= 11.f;
				const ml::Rect b{ bx, r.y + 15, 10, 11 };
				fill( b, hpUsed[k] ? 0xffb07020 : 0xff2a7a4a, true );
				text( { b.x, b.y - 1, b.w, b.h }, mountLetter( pc->hardpoints[k].mount ), 0xffffffff, 10.f, 2 );
			}
		if ( !hasSlots )
		{
			text( { r.x + 3, r.y + 30, r.w - 6, 13 }, "no equipment slots", 0xff808890, 10.f, 0 );
			continue;
		}
		std::vector<LocItem> items;
		gatherPanelItems( pVariant, p, items );
		for ( size_t k = 0; k < items.size(); ++k )
		{
			ml::Rect row = ml::itemRowRect( p, static_cast<int>( k ), static_cast<int>( items.size() ) );
			const bool sel = items[k].x == selI && items[k].y == selJ && pSelectedComponent && !pDragComponent;
			if ( sel ) fill( row, 0x802d6a8f, true );
			char line[64];
			const bool showWeight = items.size() <= ml::kRowsPerColumn;
			std::string name = items[k].comp->getName();
			const ml::Rect nameRect = rectOf( { row.x + 1, row.y, row.w - ( showWeight ? 28 : 2 ), row.h } );
			const int nameSize = static_cast<int>( 10.f * tbSy );
			std::vector<std::string> nameLines;
			bool truncated = false;
			while ( !name.empty() && GuiRuntime::CalcUiTextWrapLines( ( name + ( truncated ? "..." : "" ) ).c_str(), nameRect.w, nameSize, "", nameLines ) && nameLines.size() > 1 ) {
				size_t cut = name.size() - 1;
				while ( cut > 0 && ( static_cast<unsigned char>( name[cut] ) & 0xc0 ) == 0x80 ) --cut;
				name.resize( cut );
				truncated = true;
			}
			if ( truncated ) name += "...";
			GuiRuntime::DrawUiText( nameRect.x, nameRect.y, nameRect.w, nameRect.h,
				name.c_str(), 0xffe8eef4, nameSize, 0, "" );
			if ( showWeight ) {
				std::snprintf( line, sizeof line, "%.1ft", static_cast<double>( items[k].comp->getWeight() ) );
				text( { row.x + row.w - 28, row.y, 27, row.h }, line, 0xffa0b0c0, 10.f, 1 );
			}
		}
		if ( cand )
			text( { r.x + 2, r.y + r.h - ml::kRefusalH, r.w - 4, ml::kRefusalH }, s_locReason[p].c_str(), s_locLegal[p] ? 0xff60ff90 : 0xffff7070, 10.f, 0 );
	}
	// The combo text is composited later; cover every panel beneath its expanded list.
	// left()/top() are parent-relative; the list draws at its global location.
	if ( variantList.ListBox().isShowing() ) {
		auto& list = variantList.ListBox();
		fill( { static_cast<float>( list.globalX() ), static_cast<float>( list.globalY() ), list.width(), list.height() }, 0xff101820, true );
	}
}
