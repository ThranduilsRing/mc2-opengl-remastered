#include "render_view.h"
#include <algorithm>
#include "Gates/preferences_legacy.h"
#include <atomic>
#include "../GameOS/gameos/ui_texture.h"
#include "../mclib/texture_assets.h"
#define INFOWINDOW_CPP
/*************************************************************************************************\
InfoWindow.cpp			: Implementation of the InfoWindow component.
//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"infowindow.h"
#include"mclib.h"
#include "warrior.h"
#include "../resource.h"
#include"mover.h"
#include"mechicon.h"
#include"team.h"
#include"render_submissions.h"
#include"gameos.hpp"        // INFO-IMGUI-SLICE-1: gos_ComputeHudCanvasBox + Environment
#include"imgui.h"           // INFO-IMGUI-SLICE-1: ImGui info panel
#include"hud_imgui_font.h"  // FONT-IMGUI-SLICE-1: legacy bitmap-font text
#include<cstdlib>
#include<cstdint>           // INFO-IMGUI-SLICE-2: intptr_t (ImTextureID cast)

// IN-MISSION-HUD-IMGUI-SCOPE-1 slice D (no such gate)
#include"hudimguipanel.h"
#include"infowindowmodel.h"
#include "imgui.h"
#include"unitprofile.h"	// EQUIP-ADD-UI-NAME-1 (S3): equipmentAddUiTextForStringId
#include "Gates/gate.h"
#include "mc2fmt.h"

long InfoWindow::SCROLLLEFT = 0;
long InfoWindow::SCROLLRIGHT = 0;
long InfoWindow::SCROLLTOP = 0;
long InfoWindow::SCROLLBOTTOM = 0;
long InfoWindow::SECTIONSKIP = 0;
long InfoWindow::NAMELEFT = 0;
long InfoWindow::NAMERIGHT = 0;
long InfoWindow::NAMETOP = 0;
long InfoWindow::NAMEBOTTOM = 0;
long InfoWindow::HEALTHLEFT = 0;
long InfoWindow::HEALTHRIGHT = 0;
long InfoWindow::HEALTHTOP = 0;
long InfoWindow::HEALTHBOTTOM = 0;
long InfoWindow::DIVIDERCOLOR = 0;
long InfoWindow::DIVIDERLEFT = 0;
long InfoWindow::DIVIDERRIGHT = 0;
long	InfoWindow::PILOTLEFT = 0;
long InfoWindow::PILOTRIGHT = 0;
long InfoWindow::PILOTHEIGHT = 0;
long InfoWindow::MECHLEFT = 0;
long InfoWindow::MECHRIGHT = 0;
long InfoWindow::MECHHEIGHT = 0;
long InfoWindow::MECHBACKLEFT = 0;
long InfoWindow::MECHBACKRIGHT = 0;
long InfoWindow::MECHBACKHEIGHT = 0;
long InfoWindow::SCROLLBUTTONV = 0;
std::unique_ptr<StaticInfo[]> InfoWindow::skillInfos = nullptr;
long InfoWindow::SCROLLBUTTONU = 0;
long InfoWindow::SCROLLBUTTONHEIGHT = 0;
long InfoWindow::SCROLLBUTTONWIDTH = 0;
long InfoWindow::SCROLLBUTTONX = 0;
long InfoWindow::SCROLLMAX = 0;
long InfoWindow::SCROLLMIN = 0;
long InfoWindow::NUMBERSKILLBARS = 0;
long InfoWindow::SKILLUNITWIDTH = 0;
long InfoWindow::SKILLSKIP = 0;
long InfoWindow::SKILLHEIGHT = 0;
long InfoWindow::SKILLRIGHT = 0;
long InfoWindow::SKILLLEFT = 0;
long InfoWindow::SCROLLCOLOR = -1;
long InfoWindow::INFOHEIGHT = 0;
long InfoWindow::INFOWIDTH = 0;
long InfoWindow::INFOTOP = 0;
long InfoWindow::INFOLEFT = 0;
long InfoWindow::COMPONENTLEFT = 0;

long InfoWindow::SCROLLBOXLEFT= 0;
long InfoWindow::SCROLLBOXRIGHT= 0;
long InfoWindow::SCROLLBOXTOP= 0;
long InfoWindow::SCROLLBOXBOTTOM= 0;
long InfoWindow::PILOTNAMELEFT = 0;

GUI_RECT InfoWindow::NameRect = { 0 };

ControlButton InfoWindow::buttons[2] = {0};

InfoWindow* InfoWindow::s_instance = nullptr;


static float scrollAmount() { return 3.f * Environment.screenWidth/640.f; }
extern float WeaponRanges[NUM_WEAPON_RANGE_TYPES][2];

// initialize statics
ButtonData InfoWindow::buttonData[2];;


InfoWindow::InfoWindow()
{
	pUnit = nullptr;
	scrollLength = 0;
	scrollPos = 0;
	backgroundTexture = 0;
	icon = nullptr;
	infoLength = 0;

	skillInfos = std::make_unique_for_overwrite<StaticInfo[]>(7);
	memset( skillInfos.get(), 0, sizeof( StaticInfo ) *  7);
	memset( buttonData, 0, sizeof( buttonData ) );

	lastYClick = -1.f;
	s_instance = this;
}

void InfoWindow::init( FitIniFile& file )
{
	
	if ( NO_ERR != file.seekBlock( "Fonts" ) )
		Assert( 0, 0, "couldn't find the font block" );

	for ( int i = 0; i < 7; i++ )
	{
		if ( skillInfos[i].textureHandle )
		{
			mc2::textures::release( skillInfos[i].textureHandle );
		}
	}

    // sebi: WTF??? thank you clang
	//memset( skillInfos.get(), 0, sizeof( skillInfos ));
	memset( skillInfos.get(), 0, sizeof( StaticInfo ) * 7);

	//sebi: ORIG BUG FIX what a reason to clear this and then iterate over it?
	// obviously clear shoul dbe done after!
	//memset( buttonData, 0, sizeof( buttonData ) );


	for (auto & buttonDataElem : buttonData)
	{
		if ( buttonDataElem.textureHandle )
		{
			mc2::textures::release( buttonDataElem.textureHandle );
		}
	}

	memset( buttonData, 0, sizeof( buttonData )); // sebi


	long fontID;
	if ( NO_ERR != file.readIdLong( "InfoWndFont", fontID ) )
	{
		Assert( 0, 0,"couldn't find infoWndFont in button layout file" );
	}

	s_instance->nameFont.init( fontID );
	

	if ( NO_ERR != file.readIdLong( "ComponentFont", fontID ) )
	{
		Assert( 0, 0, "couldn't find componentFont in button layout file" );
	}

	s_instance->componentFont.init( fontID );



	if ( NO_ERR != file.seekBlock( "InfoWindow" ) )
	{
		Assert( 0, 0, "couldn't find Info block in button layout file" );
	}

	file.readIdLong( "XLocation", 	InfoWindow::INFOLEFT );
	file.readIdLong( "YLocation", 	InfoWindow::INFOTOP );
	file.readIdLong( "Width", 		InfoWindow::INFOWIDTH );
	file.readIdLong( "Height", 		InfoWindow::INFOHEIGHT );
	
	InfoWindow::INFOLEFT 	+= ControlGui::hiResOffsetX;
	InfoWindow::INFOTOP 	+= ControlGui::hiResOffsetY;
	
	file.readIdLong( "SkipBetweenSections", InfoWindow::SECTIONSKIP );

	file.readIdLong( "InfoHeaderBoxLeft", 	NameRect.left );
	file.readIdLong( "InfoHeaderBoxRight", 	NameRect.right );
	file.readIdLong( "InfoHeaderBoxTop", 	NameRect.top );
	file.readIdLong( "InfoHeaderBoxBottom", NameRect.bottom );

	NameRect.left 	+= ControlGui::hiResOffsetX; 
	NameRect.right 	+= ControlGui::hiResOffsetX; 
	NameRect.top 	+= ControlGui::hiResOffsetY;
	NameRect.bottom += ControlGui::hiResOffsetY; 
	
	file.readIdLong( "ScrollBarToP", 	InfoWindow::SCROLLTOP );
	file.readIdLong( "ScrollBarBottom", InfoWindow::SCROLLBOTTOM );
	file.readIdLong( "ScrollBarLeft", 	InfoWindow::SCROLLLEFT );
	file.readIdLong( "ScrollBarRight", 	InfoWindow::SCROLLRIGHT );
	file.readIdLong( "ScrollBarColor", 	InfoWindow::SCROLLCOLOR );
	
	InfoWindow::SCROLLTOP 		+= ControlGui::hiResOffsetY;
	InfoWindow::SCROLLBOTTOM	+= ControlGui::hiResOffsetY; 
	InfoWindow::SCROLLLEFT		+= ControlGui::hiResOffsetX; 
	InfoWindow::SCROLLRIGHT     += ControlGui::hiResOffsetX; 

	file.readIdLong( "ScrollContentBoxLeft", 		InfoWindow::SCROLLBOXLEFT );
	file.readIdLong( "ScrollContentBoxRight", 		InfoWindow::SCROLLBOXRIGHT );
	file.readIdLong( "ScrollContentBoxBarTop", 		InfoWindow::SCROLLBOXTOP );
	file.readIdLong( "ScrollContentBoxBarBottom", 	InfoWindow::SCROLLBOXBOTTOM );
	
	InfoWindow::SCROLLBOXLEFT  	+= ControlGui::hiResOffsetX;  
	InfoWindow::SCROLLBOXRIGHT  += ControlGui::hiResOffsetX;  
	InfoWindow::SCROLLBOXTOP    += ControlGui::hiResOffsetY;  
	InfoWindow::SCROLLBOXBOTTOM += ControlGui::hiResOffsetY;  

	file.readIdLong( "NameLocationLeft", 	InfoWindow::NAMELEFT );
	file.readIdLong( "NameLocationRight", 	InfoWindow::NAMERIGHT );
	file.readIdLong( "NameLocationTop", 	InfoWindow::NAMETOP );
	file.readIdLong( "NameLocationBottom", 	InfoWindow::NAMEBOTTOM );
	
	InfoWindow::NAMELEFT  	+= ControlGui::hiResOffsetX;
	InfoWindow::NAMERIGHT   += ControlGui::hiResOffsetX;
	InfoWindow::NAMETOP     += ControlGui::hiResOffsetY;
	InfoWindow::NAMEBOTTOM  += ControlGui::hiResOffsetY;

	file.readIdLong( "HealthBarLeft", 	InfoWindow::HEALTHLEFT );
	file.readIdLong( "HealthBarRight", 	InfoWindow::HEALTHRIGHT );
	file.readIdLong( "HealthBarTop", 	InfoWindow::HEALTHTOP );
	file.readIdLong( "HealthBarBottom", InfoWindow::HEALTHBOTTOM );
	
	InfoWindow::HEALTHLEFT		+= ControlGui::hiResOffsetX; 
	InfoWindow::HEALTHRIGHT     += ControlGui::hiResOffsetX; 
	InfoWindow::HEALTHTOP       += ControlGui::hiResOffsetY; 
	InfoWindow::HEALTHBOTTOM    += ControlGui::hiResOffsetY; 

	file.readIdLong( "DividerLeft", 	InfoWindow::DIVIDERLEFT );
	file.readIdLong( "DividerRight", 	InfoWindow::DIVIDERRIGHT );
	file.readIdLong( "DividerColor", InfoWindow::DIVIDERCOLOR );
	
	InfoWindow::DIVIDERLEFT 	+= ControlGui::hiResOffsetX;  
	InfoWindow::DIVIDERRIGHT    += ControlGui::hiResOffsetX;  

	file.readIdLong( "PilotLeft", 		InfoWindow::PILOTLEFT );
	file.readIdLong( "PilotRight", 		InfoWindow::PILOTRIGHT );
	
	InfoWindow::PILOTLEFT 	+= ControlGui::hiResOffsetX;  
	InfoWindow::PILOTRIGHT	+= ControlGui::hiResOffsetX;  
	
	file.readIdLong( "PilotHeight", 	InfoWindow::PILOTHEIGHT );
	file.readIdLong( "PilotNameLeft", 	PILOTNAMELEFT );
	
	PILOTNAMELEFT += ControlGui::hiResOffsetX;   

	file.readIdLong( "MechIconLeft", 		InfoWindow::MECHLEFT );
	file.readIdLong( "MechIconRight", 		InfoWindow::MECHRIGHT );
	file.readIdLong( "MechIconHeight", 		InfoWindow::MECHHEIGHT );
	file.readIdLong( "MechIconBackLeft", 	InfoWindow::MECHBACKLEFT );
	file.readIdLong( "MechIconBackRight", 	InfoWindow::MECHBACKRIGHT );
	file.readIdLong( "MechIconBackHeight", 	InfoWindow::MECHBACKHEIGHT );
	
	InfoWindow::MECHLEFT		+= ControlGui::hiResOffsetX;   
	InfoWindow::MECHRIGHT       += ControlGui::hiResOffsetX;   
	InfoWindow::MECHBACKLEFT    += ControlGui::hiResOffsetX;   
	InfoWindow::MECHBACKRIGHT   += ControlGui::hiResOffsetX;   
	
	file.seekBlock( "SkillMeter" );
	file.readIdLong( "Left", 		InfoWindow::SKILLLEFT );
	file.readIdLong( "Right", 		InfoWindow::SKILLRIGHT );
	
	InfoWindow::SKILLLEFT 	+= ControlGui::hiResOffsetX;  
	InfoWindow::SKILLRIGHT 	+= ControlGui::hiResOffsetX;  
	
	file.readIdLong( "Height", 		InfoWindow::SKILLHEIGHT );
	file.readIdLong( "Skip", 		InfoWindow::SKILLSKIP );
	file.readIdLong( "UnitWidth", 	InfoWindow::SKILLUNITWIDTH );
	file.readIdLong( "NumberUnits", InfoWindow::NUMBERSKILLBARS );
	
	file.seekBlock( "ScrollButton" );
	file.readIdLong( "Min", 		SCROLLMIN );
	file.readIdLong( "Max", 		SCROLLMAX );
	file.readIdLong( "XLocation", 	SCROLLBUTTONX );

	SCROLLMIN		+= ControlGui::hiResOffsetY; 
	SCROLLMAX	    += ControlGui::hiResOffsetY; 
	SCROLLBUTTONX 	+= ControlGui::hiResOffsetX;
	
	file.readIdLong( "Width", 		SCROLLBUTTONWIDTH );
	file.readIdLong( "Height", 		SCROLLBUTTONHEIGHT );
	
	file.readIdLong( "UNormal", SCROLLBUTTONU );
	file.readIdLong( "VNormal", SCROLLBUTTONV );



	ControlButton::initButtons( file, 2, buttons, buttonData, "InfoButton" );

	char SkillText[32];
	for (int i = 0; i < 7; i++ )
	{
		mc2::fmt::to_buf(SkillText, "Skill{:d}", mc2::fmt::as_i32(i));
		skillInfos[i].init( file, SkillText ,ControlGui::hiResOffsetX, ControlGui::hiResOffsetY);
	
	}

	InfoWindow::COMPONENTLEFT = InfoWindow::PILOTLEFT;
	

}

InfoWindow::~InfoWindow()
{
	skillInfos.reset();
}

void InfoWindow::setUnit(Mover* pNewMover)
{

	if (pNewMover && !pNewMover->contactView.model)
	{
		return;
	}

	if ( pUnit != pNewMover )
	{
		pUnit = pNewMover;
		bUnitChanged = true;

		if ( icon )
		{
			delete icon;
			icon = nullptr;
		}

		icon = nullptr;

		scrollPos = 0;
		infoLength = 0;

		if ( pNewMover )
		{

			if ( pNewMover->getObjectType()->getObjectTypeClass() == BATTLEMECH_TYPE )
			{
				MechIcon* pIcon = new MechIcon;
				pIcon->setLocationIndex( 16 );
				pIcon->init( pNewMover );
				pIcon->setDrawBack( true );
				icon = pIcon;
			}
			else
			{
				VehicleIcon* pIcon = new VehicleIcon;
				pIcon->setLocationIndex( 16 );
				pIcon->setDrawBack( 1 );
				pIcon->init( pNewMover );
				icon = pIcon;
			}
		}
	}
}

void InfoWindow::drawName( mc2::czstring name )
{
	GUI_RECT rect = { NAMELEFT, NAMETOP, NAMERIGHT, SCROLLTOP};
	drawRect( rect, 0xff000000 );
	drawEmptyRect( NameRect, 0xff002f55, 0xff002f55 );
	nameFont.render( name, NAMELEFT, NAMETOP, NAMERIGHT - NAMELEFT, NAMEBOTTOM - NAMETOP, 0xff5c96c2, 0, 3 );
}

// INFO-IMGUI-SLICE-1: gate. Default OFF -> legacy procedural panel, byte-identical.
bool InfoWindow::imguiInfoEnabled()
{
	static const bool s_on = false;
	return s_on;
}

// INFO-IMGUI-SLICE-1: ImGui unit-info panel. Reproduces the procedural legacy
// draw (header box + unit name + health bar + loadout list) bound to the live
// pUnit read this frame. The mech paperdoll icon is DEFERRED to slice-2 (its
// area is left blank). Loadout content comes from the shared gatherLoadoutRows
// helper so it cannot drift from the legacy enumeration.
void InfoWindow::extractHudSnapshot()
{
    hudInfo = HudInfo{};
    if (pUnit && !pUnit->contactView.model) setUnit(nullptr);
    if (!pUnit) return;
    hudInfo.name = pUnit->getIfaceName();
    hudInfo.health = mc2::render_view::hud().mover(pUnit).health;
    hudInfo.weaponCount = extractWeaponRows(hudInfo.names, hudInfo.disabled, hudInfo.ammo, hudInfo.ranges, hudInfo.draw);
    extractLoadoutRows(hudInfo.loadout);
    hudInfo.pilotVisible = extractPilotRows(hudInfo.pilot);
    hudInfo.equipment[0] = pUnit->ecm; hudInfo.equipment[1] = pUnit->probe;
    hudInfo.equipment[2] = pUnit->jumpJets; hudInfo.equipment[3] = pUnit->isMech() ? pUnit->sensor : 255;
    for (int i = 0; i < 4; ++i)
        if (hudInfo.equipment[i] != 255) hudInfo.equipmentMaster[i] = pUnit->inventory[hudInfo.equipment[i]].masterID;
}

int InfoWindow::gatherWeaponRows(unsigned char names[60], char disabled[60][2], long ammo[60], char ranges[60], bool draw[4]) const
{
    std::copy_n(hudInfo.names, 60, names); std::copy_n(hudInfo.ammo, 60, ammo);
    std::copy_n(hudInfo.ranges, 60, ranges); std::copy_n(hudInfo.draw, 4, draw);
    for (int i = 0; i < 60; ++i) { disabled[i][0] = hudInfo.disabled[i][0]; disabled[i][1] = hudInfo.disabled[i][1]; }
    return hudInfo.weaponCount;
}
void InfoWindow::gatherLoadoutRows(std::vector<InfoLoadoutRow>& out) const { out = hudInfo.loadout; }

void InfoWindow::renderImGui()
{
	// INFO-IMGUI-SLICE-3: legacy placement transform. Legacy HUD verts go through
	// TWO vertex transforms in gosRenderer::flushHUDBatch (gameos_graphics.cpp,
	// "HUD scale" + "UI-ASPECT-ANCHOR-1" blocks):
	//   (1) the in-mission bottom-band shrink — every draw call whose centroid
	//       sits below 60% of the logical height scales by s_hud_scale (default
	//       0.85) toward the bottom-center anchor (lw/2, lh) in logical
	//       Environment.screenWidth/Height space;
	//   (2) the 16:9 HUD canvas remap (gos_ComputeHudCanvasBox).
	// The info panel (authored y 434..597) sits entirely inside the bottom band,
	// so the WHOLE panel shrinks. Slices 1-2 applied only (2) — the raw canvas
	// box — which left the panel hugging the screen left edge ~13% oversized.
	// (PauseWindow can keep the raw box: its rect is in the TOP band, unshrunk.)
	int bx = 0, by = 0, bw = 0, bh = 0;
	if ( !gos_ComputeHudCanvasBox( Environment.drawableWidth, Environment.drawableHeight,
			&bx, &by, &bw, &bh ) )
	{ bx = 0; by = 0; bw = Environment.drawableWidth; bh = Environment.drawableHeight; }
	const float lw = ( Environment.screenWidth  > 0 ) ? static_cast<float>(Environment.screenWidth)  : 800.0f;
	const float lh = ( Environment.screenHeight > 0 ) ? static_cast<float>(Environment.screenHeight) : 600.0f;
	// same activation condition as flushHUDBatch (scale < 0.999 gate included)
	const float hud = ( gos_GetHudScaleActive() && gos_GetHudScale() < 0.999f )
		? gos_GetHudScale() : 1.0f;
	const float cx = static_cast<float>(bw) / lw;   // canvas remap scale (transform 2)
	const float cy = static_cast<float>(bh) / lh;
	const float sx = cx * hud;         // effective authored-px -> device-px scale
	const float sy = cy * hud;
	const float px = bx + ( lw * 0.5f + (   3.0f - lw * 0.5f ) * hud ) * cx;
	const float py = by + ( lh        + ( 434.0f - lh        ) * hud ) * cy;
	const float pw = 162.0f * sx;
	const float ph = 163.0f * sy;

	ImGui::SetNextWindowPos( ImVec2( px, py ) );
	ImGui::SetNextWindowSize( ImVec2( pw, ph ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
	ImGui::Begin( "##mc2_info", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav );

	ImDrawList* dl = ImGui::GetWindowDrawList();

	// authored (800x600 logical) -> device-pixel mapping: shrink (1) then canvas
	// remap (2), the same order flushHUDBatch applies them.
	auto AX = [&]( float ax ){ return bx + ( lw * 0.5f + ( ax - lw * 0.5f ) * hud ) * cx; };
	auto AY = [&]( float ay ){ return by + ( lh        + ( ay - lh        ) * hud ) * cy; };
	auto ARGB = []( unsigned long a ){
		return IM_COL32( (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff, (a >> 24) & 0xff );
	};
	const ImU32 chromeBlue = ARGB( static_cast<unsigned long>(SCROLLCOLOR) );   // fit ScrollBarColor 0xff002f55

	// full-panel black backing (legacy fills the info area black)
	dl->AddRectFilled( ImVec2( px, py ), ImVec2( px + pw, py + ph ), IM_COL32( 0, 0, 0, 255 ) );

	// header box. INFO-IMGUI-SLICE-3: legacy drawName draws the fit-loaded
	// InfoHeaderBox rect (NameRect, authored (8,439)..(159,471)) as an EMPTY
	// rect (drawEmptyRect = 1px outline, interior stays the black backing) —
	// slices 1-2 filled it, which is not what legacy shows.
	dl->AddRect(
		ImVec2( AX( static_cast<float>(NameRect.left) ),  AY( static_cast<float>(NameRect.top) ) ),
		ImVec2( AX( static_cast<float>(NameRect.right) ), AY( static_cast<float>(NameRect.bottom) ) ),
		IM_COL32( 0x00, 0x2F, 0x55, 255 ) );

	// FONT-IMGUI-SLICE-1: text draws with the SAME legacy bitmap fonts
	// (nameFont for the header, componentFont for the rows) through HudFont —
	// glyph quads from the gos font atlas. Strings pass through unmodified,
	// exactly like the legacy path (the slice-2 ToUpper approximation for the
	// ImGui default font is retired).
	HGOSFONT3D hdrFont = nameFont.getTempHandle();
	HGOSFONT3D rowFont = componentFont.getTempHandle();

	// unit name (legacy drawName: nameFont.render(..., 0xff5c96c2, 0, 3) —
	// WrapType 3 centers X and Y in the NAME rect). Mirror that centering in
	// the fit-loaded authored NAME rect, mapped through the canvas anchor.
	char nameBuf[256];
	const char* name;
	if ( pUnit )
		name = hudInfo.name.c_str();
	else
	{
		cLoadString( IDS_NOUNIT, nameBuf, 255 );
		name = nameBuf;
	}
	if ( name )
	{
		HudFont::DrawWrappedCentered( dl, hdrFont,
			AX( static_cast<float>(NAMELEFT) ), AY( static_cast<float>(NAMETOP) ),
			AX( static_cast<float>(NAMERIGHT) ), AY( static_cast<float>(NAMEBOTTOM) ),
			IM_COL32( 0x5C, 0x96, 0xC2, 255 ), name, sx, sy );
	}

	// health bar. FONT-IMGUI-SLICE-1: use the fit-loaded legacy HEALTH rect
	// (authored (14,461)..(153,465)) — sits inside the InfoHeaderBox under
	// the two-line name, like the legacy panel. Color by barStatus:
	// >=0.5 green, >0.2 yellow, else red. Dim bg (argb & 0xff7f7f7f) full width,
	// then filled fg to width*barStatus.
	const float hbL = AX( static_cast<float>(HEALTHLEFT) );
	const float hbR = AX( static_cast<float>(HEALTHRIGHT) );
	const float hbT = AY( static_cast<float>(HEALTHTOP) );
	const float hbB = AY( static_cast<float>(HEALTHBOTTOM) );
	if ( pUnit )
	{
		float barStatus = hudInfo.health;
		barStatus = std::clamp(barStatus, 0.0f, 1.0f);

		DWORD color;
		if ( barStatus >= 0.5 )      color = SB_GREEN;
		else if ( barStatus > 0.2 )  color = SB_YELLOW;
		else                         color = SB_RED;

		// SB_* pack as ARGB; force full alpha for display (legacy blends 0x6f but
		// over the black backing the fill reads solid like the reference).
		auto argb2im = []( uint32_t argb ){
			return IM_COL32( (argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff, (argb >> 24) & 0xff );
		};
		const uint32_t dim  = ( static_cast<uint32_t>(color) & 0xff7f7f7f ) | 0xff000000u;
		const uint32_t full = ( static_cast<uint32_t>(color) )              | 0xff000000u;

		dl->AddRectFilled( ImVec2( hbL, hbT ), ImVec2( hbR, hbB ), argb2im( dim ) );
		dl->AddRectFilled( ImVec2( hbL, hbT ),
			ImVec2( hbL + ( hbR - hbL ) * barStatus, hbB ), argb2im( full ) );
	}

	// INFO-IMGUI-SLICE-3: legacy chrome. Legacy render() outlines the scroll
	// content box (fit ScrollContentBox, authored (8,473)..(159,592)) and the
	// scrollbar track (fit ScrollBar, authored (142,475)..(157,590)), both in
	// SCROLLCOLOR (0xff002f55), interiors staying the black backing.
	dl->AddRect(
		ImVec2( AX( static_cast<float>(SCROLLBOXLEFT) ),  AY( static_cast<float>(SCROLLBOXTOP) ) ),
		ImVec2( AX( static_cast<float>(SCROLLBOXRIGHT) ), AY( static_cast<float>(SCROLLBOXBOTTOM) ) ),
		chromeBlue );
	dl->AddRect(
		ImVec2( AX( static_cast<float>(SCROLLLEFT) ),  AY( static_cast<float>(SCROLLTOP) ) ),
		ImVec2( AX( static_cast<float>(SCROLLRIGHT) ), AY( static_cast<float>(SCROLLBOTTOM) ) ),
		chromeBlue );

	// Scrolling content child covering the legacy content-box interior. The
	// child's right edge lands on the legacy thumb right (SCROLLBUTTONX +
	// SCROLLBUTTONWIDTH, authored 156) and ImGui's scrollbar width is set to
	// the legacy thumb width, so the ImGui scrollbar occupies exactly the
	// legacy track. Thumb styled the legacy blue 0xff5c96c2. POLISH-IMGUI-
	// SLICE-1: the up/down arrow-cap sprites (fit InfoButton0/1, MCUI_Med1
	// atlas) are drawn at the track ends below (static art — wheel + thumb
	// drag cover the interaction; click-to-scroll not wired this slice).
	const float childL = AX( static_cast<float>(SCROLLBOXLEFT) + 1.0f );
	const float childT = AY( static_cast<float>(SCROLLTOP) );
	const float childR = AX( static_cast<float>(( SCROLLBUTTONX + SCROLLBUTTONWIDTH )) );
	const float childB = AY( static_cast<float>(SCROLLBOTTOM) );
	ImGui::PushStyleColor( ImGuiCol_ScrollbarBg, IM_COL32( 0, 0, 0, 0 ) );
	ImGui::PushStyleColor( ImGuiCol_ScrollbarGrab,        IM_COL32( 0x5C, 0x96, 0xC2, 255 ) );
	ImGui::PushStyleColor( ImGuiCol_ScrollbarGrabHovered, IM_COL32( 0x7C, 0xB2, 0xDA, 255 ) );
	ImGui::PushStyleColor( ImGuiCol_ScrollbarGrabActive,  IM_COL32( 0x9C, 0xC8, 0xE8, 255 ) );
	ImGui::PushStyleVar( ImGuiStyleVar_ScrollbarSize, static_cast<float>(SCROLLBUTTONWIDTH) * sx );
	ImGui::PushStyleVar( ImGuiStyleVar_ScrollbarRounding, 0.0f );
	// legacy drawScrollingStuff advances curY by exact row heights/SECTIONSKIPs;
	// ImGui's default ItemSpacing between the Dummies would add ~4px per row and
	// push everything below the doll progressively lower than legacy.
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0.0f, 0.0f ) );
	ImGui::SetCursorScreenPos( ImVec2( childL, childT ) );
	float loadoutScrollMax = 0.0f;   // POLISH-IMGUI-SLICE-1: arrow-cap gate
	// POLISH-IMGUI-SLICE-2: scroll delta accumulated by the arrow caps AFTER
	// EndChild each frame, applied here at the next BeginChild (one-frame
	// latency, matching the legacy per-frame held-button repeat).
	static float s_capScrollDelta = 0.0f;
	if ( ImGui::BeginChild( "##mc2_info_loadout",
			ImVec2( childR - childL, childB - childT ),
			false, ImGuiWindowFlags_NoBackground ) )
	{
		loadoutScrollMax = ImGui::GetScrollMaxY();
		if ( s_capScrollDelta != 0.0f )
		{
			ImGui::SetScrollY( ImGui::GetScrollY() + s_capScrollDelta );
			s_capScrollDelta = 0.0f;
		}
		if ( pUnit )
		{
			// FONT-IMGUI-SLICE-1: rows draw with the legacy componentFont via
			// HudFont::DrawWrapped (legacy findTextBreak word-wrap); an
			// ImGui::Dummy per row keeps the child's cursor/scroll extent in
			// sync with what was drawn.
			ImDrawList* cdl = ImGui::GetWindowDrawList();
			// legacy row wrap width: COMPONENTLEFT .. SCROLLLEFT
			const float wrapW = static_cast<float>(( SCROLLLEFT - COMPONENTLEFT )) * sx;
			auto BitmapRow = [&]( mc2::czstring text, ImU32 col ){
				const ImVec2 cur = ImGui::GetCursorScreenPos();
				const float h = HudFont::DrawWrapped( cdl, rowFont,
					cur.x, cur.y, wrapW, col, text, sx, sy );
				ImGui::Dummy( ImVec2( wrapW, h ) );
			};
			// legacy drawScrollingStuff row x position (COMPONENTLEFT, authored 15)
			auto RowX = [&](){
				ImGui::SetCursorScreenPos(
					ImVec2( AX( static_cast<float>(COMPONENTLEFT) ), ImGui::GetCursorScreenPos().y ) );
			};
			// legacy drawDivider: 1px line DIVIDERLEFT..DIVIDERRIGHT in
			// DIVIDERCOLOR, bracketed by SECTIONSKIP gaps (drawScrollingStuff
			// inserts curY += SECTIONSKIP around each divider).
			auto Divider = [&](){
				ImGui::Dummy( ImVec2( 1.0f, static_cast<float>(SECTIONSKIP) * sy ) );
				const float dy = ImGui::GetCursorScreenPos().y;
				cdl->AddLine( ImVec2( AX( static_cast<float>(DIVIDERLEFT) ),  dy ),
				              ImVec2( AX( static_cast<float>(DIVIDERRIGHT) ), dy ),
				              ARGB( static_cast<unsigned long>(DIVIDERCOLOR) ) );
				ImGui::Dummy( ImVec2( 1.0f, static_cast<float>(SECTIONSKIP) * sy ) );
			};

			// top pad: legacy content starts at SCROLLBOXTOP + SECTIONSKIP
			// (authored 478); the child top sits at SCROLLTOP (475).
			ImGui::Dummy( ImVec2( 1.0f,
				AY( static_cast<float>(( SCROLLBOXTOP + SECTIONSKIP )) ) - childT ) );

			// INFO-IMGUI-SLICE-2 paperdoll (front + back schematic), SLICE-3:
			// moved INSIDE the scrolling child at the fit-loaded MechIcon rects
			// (authored front 14..45, back 51..82, 38 tall) — legacy draws it as
			// the first scrolling section, not pinned chrome.
			if ( icon )
			{
				uint64_t iconTex = 0;
				float u0, v0, u1, v1;
				const float dollTop = ImGui::GetCursorScreenPos().y;
				const float dollBot = dollTop + static_cast<float>(MECHHEIGHT) * sy;
				if ( icon->getUnitIconTexUV( iconTex, u0, v0, u1, v1 ) )
				{
					cdl->AddImage( static_cast<ImTextureID>(static_cast<intptr_t>(iconTex)),
						ImVec2( AX( static_cast<float>(MECHLEFT) ),  dollTop ),
						ImVec2( AX( static_cast<float>(MECHRIGHT) ), dollBot ),
						ImVec2( u0, v0 ), ImVec2( u1, v1 ) );
				}
				if ( icon->getUnitIconBackTexUV( iconTex, u0, v0, u1, v1 ) )
				{
					cdl->AddImage( static_cast<ImTextureID>(static_cast<intptr_t>(iconTex)),
						ImVec2( AX( static_cast<float>(MECHBACKLEFT) ),  dollTop ),
						ImVec2( AX( static_cast<float>(MECHBACKRIGHT) ), dollBot ),
						ImVec2( u0, v0 ), ImVec2( u1, v1 ) );
				}
				ImGui::Dummy( ImVec2( 1.0f, static_cast<float>(MECHHEIGHT) * sy ) );
				Divider();
			}

			std::vector<InfoLoadoutRow> rows;
			gatherLoadoutRows( rows );
			// legacy inserts SECTIONSKIP + divider + SECTIONSKIP after each
			// weapon-range group; groups are color-coded in the shared gather,
			// so a color change marks a group boundary. Equipment rows (gold
			// 0xffc29b00) get a SECTIONSKIP after each row like legacy.
			uint32_t prevArgb = 0;
			bool firstRow = true;
			for ( const InfoLoadoutRow& r : rows )
			{
				if ( !firstRow && r.argb != prevArgb )
					Divider();
				RowX();
				BitmapRow( r.text.c_str(), ARGB( r.argb ) );
				if ( r.argb == 0xffc29b00u )
					ImGui::Dummy( ImVec2( 1.0f, static_cast<float>(SECTIONSKIP) * sy ) );
				prevArgb = r.argb;
				firstRow = false;
			}

			// INFO-IMGUI-SLICE-2: pilot section (photo + name + rank + skills +
			// specialty skills). Content from the shared gatherPilotRows helper
			// (the legacy drawScrollingStuff consumes the SAME gather). Legacy
			// pilot-section blue is 0xff005392.
			// INFO-IMGUI-SLICE-4: the slice-2 "GUNNERY/PILOTING <value>" text
			// simplification is retired — the rank medallion + GUNNERY/PILOTING
			// label arts (fit Skill0..6 StaticInfos, MCUI_Med2 atlas) and the
			// segmented skill meter now draw exactly like the legacy
			// drawScrollingStuff/drawSkillBar path (infowindow.cpp legacy refs
			// below), reusing the fit-loaded StaticInfo rect + UVs read-only —
			// the same pattern as the OBJ status tiles / info arrow caps. Atlas
			// GL names are re-resolved per frame (OBJ-IMGUI-SLICE-1 lesson).
			const auto& pilot = hudInfo.pilot;
			if (hudInfo.pilotVisible)
			{
				Divider();

				const ImU32 pilotBlue = IM_COL32( 0x00, 0x53, 0x92, 255 );

				// --- photo row (legacy drawScrollingStuff): the SCROLLCOLOR
				// outline rect {PILOTLEFT, y, PILOTRIGHT+1, y+PILOTHEIGHT+1}
				// draws whenever the icon exists; the photo itself only when
				// the pilot is active; the name is vertically centered on the
				// photo row (legacy top = curY + PILOTHEIGHT/2 - fontHeight/2)
				// at the fit PilotNameLeft x.
				if ( icon )
				{
					const float rowTop = ImGui::GetCursorScreenPos().y;
					uint64_t pTex = 0;
					float pu0, pv0, pu1, pv1;
					if ( pilot.active && icon->getPilotIconTexUV( pTex, pu0, pv0, pu1, pv1 ) )
					{
						cdl->AddImage( static_cast<ImTextureID>(static_cast<intptr_t>(pTex)),
							ImVec2( AX( static_cast<float>(PILOTLEFT) ), rowTop ),
							ImVec2( AX( static_cast<float>(PILOTRIGHT) ), rowTop + static_cast<float>(PILOTHEIGHT) * sy ),
							ImVec2( pu0, pv0 ), ImVec2( pu1, pv1 ) );
					}
					cdl->AddRect(
						ImVec2( AX( static_cast<float>(PILOTLEFT) ), rowTop ),
						ImVec2( AX( static_cast<float>(( PILOTRIGHT + 1 )) ), rowTop + static_cast<float>(( PILOTHEIGHT + 1 )) * sy ),
						chromeBlue );
					// FONT-IMGUI-SLICE-1: legacy componentFont via HudFont
					const float nameH = HudFont::Measure( rowFont, pilot.name.c_str(), sx, sy ).y;
					HudFont::Draw( cdl, rowFont, AX( static_cast<float>(PILOTNAMELEFT) ),
						rowTop + static_cast<float>(PILOTHEIGHT) * sy * 0.5f - nameH * 0.5f,
						pilotBlue, pilot.name.c_str(), sx, sy );
					ImGui::Dummy( ImVec2( 1.0f, static_cast<float>(PILOTHEIGHT) * sy ) );
					ImGui::Dummy( ImVec2( 1.0f, static_cast<float>(SECTIONSKIP) * sy ) );
				}
				else
				{
					RowX();
					BitmapRow( pilot.name.c_str(), pilotBlue );
				}
				// legacy adds a second SECTIONSKIP before the rank row
				ImGui::Dummy( ImVec2( 1.0f, static_cast<float>(SECTIONSKIP) * sy ) );

				// --- rank + skill rows (legacy drawScrollingStuff j=0..2 loop):
				// j=0 rank medallion (skillInfos[rank]) + rank text at SKILLLEFT;
				// j=1 GUNNERY label art (skillInfos[5]) + meter + value;
				// j=2 PILOTING label art (skillInfos[6]) + meter + value.
				// Each art quad carries the fit-loaded x extent + atlas UVs in
				// skillInfos[i].location; y follows the row like the legacy
				// v[i].y = curY override. Outline = legacy drawEmptyRect
				// {v0.x-.5, y-.5, v2.x+1.5, y+h+1.5} in 0xff002f55.
				int currentSkill = pilot.rank;
				for ( int j = 0; j < 3; j++ )
				{
					const float rowTop = ImGui::GetCursorScreenPos().y;
					const gos_VERTEX* loc = skillInfos[currentSkill].location;
					const float rowH = loc[1].y - loc[0].y;   // authored 15
					{
						DWORD gosID = mc2::textures::sourceId(
							skillInfos[currentSkill].textureHandle );
						uint64_t glTex = ( gosID && gosID != 0xffffffff )
							? gos_GetUiTextureId( gosID ) : 0;
						if ( glTex )
							cdl->AddImage( static_cast<ImTextureID>(static_cast<intptr_t>(glTex)),
								ImVec2( AX( loc[0].x ), rowTop ),
								ImVec2( AX( loc[2].x ), rowTop + rowH * sy ),
								ImVec2( loc[0].u, loc[0].v ),
								ImVec2( loc[2].u, loc[2].v ) );
						cdl->AddRect(
							ImVec2( AX( loc[0].x - 0.5f ), rowTop - 0.5f * sy ),
							ImVec2( AX( loc[2].x + 1.5f ), rowTop + ( rowH + 1.5f ) * sy ),
							IM_COL32( 0x00, 0x2F, 0x55, 255 ) );
					}
					if ( j == 0 )
					{
						// rank text beside the medallion (legacy x = SKILLLEFT)
						HudFont::Draw( cdl, rowFont, AX( static_cast<float>(SKILLLEFT) ), rowTop,
							pilotBlue, pilot.rankText.c_str(), sx, sy );
					}
					else
					{
						// segmented meter — exact legacy drawSkillBar math:
						// black backing + 0xff002f55 outline on
						// {SKILLLEFT-SKILLSKIP+.5, y-.5, SKILLRIGHT+SKILLSKIP+.5,
						// y+h+1.5}; barCount = skill/NUMBERSKILLBARS units of
						// SKILLUNITWIDTH (+1 gap), color starting 0xff005392
						// stepped by the legacy packed-channel increments;
						// numeric value at SKILLRIGHT+2.
						const int skill = ( j == 1 ) ? pilot.gunnery : pilot.piloting;
						const int barCount = skill / static_cast<int>(NUMBERSKILLBARS);
						unsigned long color = 0xff005392;
						unsigned long redInc = 0, greenInc = 0, blueInc = 0;
						if ( barCount )
						{
							redInc   = static_cast<unsigned long>(( 205 / barCount )) << 16;
							greenInc = static_cast<unsigned long>(( ( 234 - 83 ) / barCount )) << 8;
							blueInc  = static_cast<unsigned long>(( ( 255 - 146 ) / barCount ));
						}
						const ImVec2 osMin( AX( static_cast<float>(( SKILLLEFT - SKILLSKIP )) + 0.5f ),
							rowTop - 0.5f * sy );
						const ImVec2 osMax( AX( static_cast<float>(( SKILLRIGHT + SKILLSKIP )) + 0.5f ),
							rowTop + ( rowH + 1.5f ) * sy );
						cdl->AddRectFilled( osMin, osMax, IM_COL32( 0, 0, 0, 255 ) );
						cdl->AddRect( osMin, osMax, IM_COL32( 0x00, 0x2F, 0x55, 255 ) );
						float uLeft = static_cast<float>(( SKILLLEFT + SKILLSKIP ));
						const float uTop = rowTop + ( static_cast<float>(SKILLSKIP) + 0.5f ) * sy;
						const float uBot = rowTop + ( rowH - static_cast<float>(SKILLSKIP) + 0.5f ) * sy;
						for ( int b = 0; b < barCount; b++ )
						{
							cdl->AddRectFilled(
								ImVec2( AX( uLeft ), uTop ),
								ImVec2( AX( uLeft + static_cast<float>(SKILLUNITWIDTH) ), uBot ),
								ARGB( color ) );
							color += redInc;
							color += greenInc;
							color += blueInc;
							uLeft += static_cast<float>(( SKILLUNITWIDTH + 1 ));
						}
						char numBuf[16];
						mc2::fmt::to_buf(numBuf, "{:d}", mc2::fmt::as_i32(skill));
						HudFont::Draw( cdl, rowFont, AX( static_cast<float>(SKILLRIGHT) + 2.0f ), rowTop,
							pilotBlue, numBuf, sx, sy );
					}
					ImGui::Dummy( ImVec2( 1.0f, rowH * sy ) );
					ImGui::Dummy( ImVec2( 1.0f, static_cast<float>(SECTIONSKIP) * sy ) );
					currentSkill = j + 5;
				}

				// specialty skills (legacy x = NAMELEFT, font-height advance)
				for ( const std::string& s : pilot.specialties )
				{
					ImGui::SetCursorScreenPos( ImVec2( AX( static_cast<float>(NAMELEFT) ),
						ImGui::GetCursorScreenPos().y ) );
					BitmapRow( s.c_str(), pilotBlue );
				}
			}

			// INFO-IMGUI-SLICE-2 capture-fixture assist: when the MC2_BOOT_INFO
			// capture fixture is driving AND MC2_BOOT_INFO_SCROLL=1 (both
			// registered Trace), pin the list scrolled to bottom from
			// bootFrame+3000 on so a late screenshot (recipe: boot 700 / shot
			// 4200) can pixel-verify the pilot rows. INFO-IMGUI-SLICE-4: the
			// pin is now opt-in via MC2_BOOT_INFO_SCROLL so the standard
			// fixture recipe keeps capturing the top-of-panel state (matching
			// the banked slice-3 baselines); the legacy path mirrors the same
			// pin (render()) so a legacy scrolled reference exists too. No
			// effect when either env is unset (normal play).
			{
				static const long s_bootInfoScroll = static_cast<long>(mc2::gate::get<"MC2_BOOT_INFO">());
				static const bool s_scrollPin = false;
				if ( s_bootInfoScroll > 0 && s_scrollPin )
				{
					extern std::atomic<uint32_t> g_mc2FrameCounter;
					if ( static_cast<long>(g_mc2FrameCounter.load(std::memory_order_relaxed)) >= s_bootInfoScroll + 3000 )
						ImGui::SetScrollY( ImGui::GetScrollMaxY() );
				}
			}
		}
	}
	ImGui::EndChild();
	ImGui::PopStyleVar( 3 );     // ScrollbarSize + ScrollbarRounding + ItemSpacing
	ImGui::PopStyleColor( 4 );   // ScrollbarBg + Grab/Hovered/Active

	// POLISH-IMGUI-SLICE-1 (INFO-SLICE-3 deferral): the legacy scroll arrow-cap
	// sprites at the track ends. Legacy render() draws buttons[0/1] (fit
	// InfoButton0 up-arrow authored (144,477,12,6) UV (201,0), InfoButton1
	// down-arrow (144,583,12,6) UV (201,16), MCUI_Med1 atlas — all four state
	// UVs identical, so this is static art) only when the list actually
	// scrolls (infowindow.cpp legacy: disabled when infoLength <
	// scrollBarLength || !pUnit, and disabled buttons are not rendered). The
	// ImGui equivalent of "content exceeds the view" is scrollMaxY > 0. The
	// quads carry the fit-loaded rect + makeUVs-resolved UVs (incl. the legacy
	// .1/fileWidth nudge) in buttons[i].location — reuse them verbatim. The
	// atlas GL name is re-resolved per frame (OBJ-IMGUI-SLICE-1 lesson).
	if ( pUnit && loadoutScrollMax > 0.0f )
	{
		// POLISH-IMGUI-SLICE-2 (POLISH-SLICE-1 deferral): cap CLICK-to-scroll.
		// Legacy scrolls while the cursor sits inside a cap rect with LMB held
		// (InfoWindow::update: getMouseLeftButtonState()==MC2_MOUSE_DOWN fires
		// handleClick every frame — code/infowindow.cpp:901-914; MC2_MOUSE_DOWN
		// is the held state, mclib/userinput.h:32), each frame stepping
		// scrollPos by scrollAmount() = 3.f*Environment.screenWidth/640.f thumb
		// px (code/infowindow.cpp:82, handleClick :1444-1457, setScrollPos
		// clamp :1529-1537). Thumb px convert to content px via increment =
		// infoLength/(SCROLLMAX-SCROLLMIN) (drawScrollingStuff :1192) where
		// SCROLLMAX = buttons[1].location[0].y - buttonHeight - 2 and
		// buttonHeight = physRange^2/(physRange+infoLength) clamped to
		// SCROLLBUTTONHEIGHT (render() :796-806). ImGui equivalent with the
		// same constants: step_dev = scrollAmount() * scrollMaxY_dev /
		// (SCROLLMAX_dyn - SCROLLMIN).
		const float physRange = buttons[1].location[0].y - buttons[0].location[2].y;
		const float infoLen = ( sy > 0.0f ) ? loadoutScrollMax / sy : 0.0f;
		float thumbH = physRange * physRange / ( physRange + infoLen );
		if ( thumbH < static_cast<float>(SCROLLBUTTONHEIGHT) )
			thumbH = static_cast<float>(SCROLLBUTTONHEIGHT);
		const float scrollMaxDyn = buttons[1].location[0].y - thumbH - 2.0f;
		const float trackRange = scrollMaxDyn - static_cast<float>(SCROLLMIN);
		const float stepDev = ( trackRange > 0.0f )
			? scrollAmount() * loadoutScrollMax / trackRange : 0.0f;

		ImGui::PushStyleVar( ImGuiStyleVar_WindowMinSize, ImVec2( 1.0f, 1.0f ) );
		ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
		for ( int i = 0; i < 2; i++ )
		{
			if ( !buttonData[i].textureHandle )
				continue;
			DWORD gosID = mc2::textures::sourceId( buttonData[i].textureHandle );
			if ( !gosID || gosID == 0xffffffff )
				continue;
			uint64_t glTex = gos_GetUiTextureId( gosID );
			if ( !glTex )
				continue;
			const gos_VERTEX* loc = buttons[i].location;
			const ImVec2 capMin( AX( loc[0].x ), AY( loc[0].y ) );
			const ImVec2 capMax( AX( loc[2].x ), AY( loc[2].y ) );
			dl->AddImage( static_cast<ImTextureID>(static_cast<intptr_t>(glTex)),
				capMin, capMax,
				ImVec2( loc[0].u, loc[0].v ),
				ImVec2( loc[2].u, loc[2].v ) );

			// A tiny top-level window over the cap rect owns the hit-test —
			// submitted after the loadout child it sits ABOVE it in ImGui's
			// hover order, so a cap press cannot fall through to the child
			// scrollbar's track-seek underneath. IsItemActive+IsItemHovered =
			// pressed on the cap and still over it, the closest ImGui match to
			// the legacy inside-rect+held check.
			ImGui::SetNextWindowPos( capMin );
			ImGui::SetNextWindowSize( ImVec2( capMax.x - capMin.x, capMax.y - capMin.y ) );
			ImGui::Begin( i == 0 ? "##mc2_info_cap0" : "##mc2_info_cap1", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav );
			ImGui::InvisibleButton( "##cap",
				ImVec2( capMax.x - capMin.x, capMax.y - capMin.y ) );
			if ( ImGui::IsItemActive() && ImGui::IsItemHovered() )
				s_capScrollDelta += ( i == 0 ) ? -stepDev : stepDev;
			ImGui::End();
		}
		ImGui::PopStyleVar( 2 );   // WindowMinSize + WindowBorderSize
	}

	ImGui::End();
	ImGui::PopStyleVar( 1 );   // WindowPadding
}

void InfoWindow::render()
{
	// INFO-IMGUI-SLICE-1: ImGui panel when gated on. Gate OFF -> legacy below,
	// byte-identical.
	if ( imguiInfoEnabled() )
	{
		renderImGui();
		return;
	}

	// INFO-IMGUI-SLICE-4 capture-fixture assist (legacy mirror of the ImGui
	// pin above): with the MC2_BOOT_INFO fixture driving AND
	// MC2_BOOT_INFO_SCROLL=1, pin the legacy list scrolled to bottom from
	// bootFrame+3000 on so a legacy scrolled reference of the pilot/skill
	// section can be captured headless (setScrollPos clamps to the live
	// SCROLLMAX-SCROLLMIN exactly like a user scroll). No effect when either
	// env is unset (normal play).
	{
		static const long s_bootInfoScroll = static_cast<long>(mc2::gate::get<"MC2_BOOT_INFO">());
		static const bool s_scrollPin = false;
		if ( s_bootInfoScroll > 0 && s_scrollPin )
		{
			extern std::atomic<uint32_t> g_mc2FrameCounter;
			if ( static_cast<long>(g_mc2FrameCounter.load(std::memory_order_relaxed)) >= s_bootInfoScroll + 3000 )
				setScrollPos( 1000000 );
		}
	}

	drawScrollingStuff();

	GUI_RECT tmpRect = { SCROLLBOXLEFT, SCROLLBOXBOTTOM, SCROLLBOXRIGHT, INFOTOP + INFOHEIGHT };
	drawRect( tmpRect, 0xff000000 );

	for (auto & button : buttons)
	{
		if ( button.isEnabled() )
			button.render();
	}
	
	long scrollBarLength = buttons[1].location[0].y - buttons[0].location[2].y - 4 - SCROLLBUTTONHEIGHT;

	gos_VERTEX v[4];
	for (auto & i : v)
	{
		i.argb = 0xff5c96c2;
		i.frgb = 0;
		i.rhw = .5;
		i.x = SCROLLBUTTONX;
		i.y = SCROLLMIN + scrollPos;
		i.z = 0.f;
		i.u = i.v = 0.f;
//		v[i].u = ((float)SCROLLBUTTONU)/256.f + .1/256.f;
//		v[i].v = ((float)SCROLLBUTTONV)/256.f + .1/256.f ;
	}
	if ( infoLength < scrollBarLength  || !pUnit )
	{
		buttons[0].disable( 1 );
		buttons[1].disable( 1 );
		SCROLLMAX = 0;
	}
	else
	{
		buttons[0].disable( 0 );
		buttons[1].disable( 0 );

		float physicalRange = buttons[1].location[0].y - buttons[0].location[2].y;
		float buttonHeight = SCROLLBUTTONHEIGHT;

		float RealRange = infoLength;

		buttonHeight =  physicalRange * physicalRange/(physicalRange + RealRange);

		buttonHeight = (std::max)( buttonHeight, static_cast<float>(SCROLLBUTTONHEIGHT) );

		SCROLLMAX = buttons[1].location[0].y - buttonHeight - 2;
		



		v[2].x = v[3].x = SCROLLBUTTONX + SCROLLBUTTONWIDTH;
		v[1].y = v[2].y = SCROLLBOTTOM;	
		v[1].y = v[2].y = v[0].y + buttonHeight;

	//	v[2].u = v[3].u = .1/256.f + (float)(SCROLLBUTTONU + ((float)SCROLLBUTTONWIDTH))/256.f;
	//	v[1].v = v[2].v = .1/256.f + (float)(SCROLLBUTTONV + ((float)SCROLLBUTTONHEIGHT))/256.f;

		gos_SetRenderState( gos_State_Texture, 0 );
		gos_DrawQuads( v, 4 );
	}



	// draw the name of the unit
	if ( pUnit )
	{
		drawName( hudInfo.name.c_str() );
	
		gos_SetRenderState( gos_State_Texture, 0 );

		// draw the health bar
		DWORD					color;
		
		float barStatus = hudInfo.health;
			
		barStatus = (std::min)( barStatus, 1.0f );

		if (barStatus >= 0.5)
			color = SB_GREEN;
		else if (barStatus > 0.2)
			color = SB_YELLOW;
		else 
			color = SB_RED;
			
		v[0].x = v[1].x = HEALTHLEFT;
		v[2].x = v[3].x = HEALTHRIGHT;
		v[0].y = v[3].y = HEALTHTOP;
		v[1].y = v[2].y = HEALTHBOTTOM;

		unsigned long dimColor = ( color & 0xff7f7f7f );
		for (auto & vElem : v)
			vElem.argb = dimColor;

		gos_DrawQuads( v, 4 );

		v[2].x = v[3].x = HEALTHLEFT + (HEALTHRIGHT - HEALTHLEFT)* barStatus;

		for (auto & vElem : v)
			vElem.argb = color | 0xff000000;

		gos_DrawQuads( v, 4 );
		
	}
	else
	{
		char noUnit[256];
		cLoadString( IDS_NOUNIT, noUnit, 255 );
		drawName( noUnit );
	}

	GUI_RECT border = { SCROLLBOXLEFT, SCROLLBOXTOP, SCROLLBOXRIGHT, SCROLLBOXBOTTOM  };
	drawEmptyRect( border, SCROLLCOLOR, SCROLLCOLOR );
	GUI_RECT rect = { SCROLLLEFT, SCROLLTOP, SCROLLRIGHT, SCROLLBOTTOM };
	drawEmptyRect( rect, SCROLLCOLOR, SCROLLCOLOR );


}

void InfoWindow::update()
{
	if (pUnit && !pUnit->contactView.model)
	{
		setUnit( nullptr );
		return;
	}

	// HUD-corrected: the unit-info panel + scroll bar draw shrunk by s_hud_scale,
	// so hit-test in HUD-inverse space to match the drawn (shrunk) widgets. No-op
	// when the HUD scale is off / outside the rendered bottom band.
	long mouseX = userInput->getMouseHudX();
	long mouseY = userInput->getMouseHudY();



	if ( icon )
		icon->update();

	for (auto & button : buttons)
	{	
		if ( button.location[0].x <= mouseX && mouseX <= button.location[2].x
				&& mouseY >= button.location[0].y && mouseY <= button.location[1].y  )
		{
			if ( userInput->getMouseLeftButtonState() == MC2_MOUSE_DOWN && button.isEnabled() )
			{
				{
					handleClick( button.ID );
					return;
				}
			}
		}
	}

	if ( userInput->isLeftDrag() )
	{
		if ( lastYClick != -1.f )
		{
			int tmpLastY = mouseY;
			tmpLastY -= userInput->getMouseDragHudY();
			setScrollPos( lastYClick + tmpLastY );
			return;
		}

	}	
	// see if its in the scroll bar area
	else if ( mouseX > SCROLLLEFT && mouseX < SCROLLRIGHT
				&& mouseY > SCROLLTOP && mouseY < SCROLLBOTTOM 
				&&  buttons[0].isEnabled() 
				&& userInput->getMouseDragHudX() > SCROLLLEFT && userInput->getMouseDragHudX() < SCROLLRIGHT
				&& userInput->getMouseDragHudY() > SCROLLTOP && userInput->getMouseDragHudY() < SCROLLBOTTOM
				)
		{


		// if its in the thumbdrag thingie, save the y
			float physicalRange = buttons[1].location[0].y - buttons[0].location[2].y;
			float buttonHeight = SCROLLBUTTONHEIGHT;

			float RealRange = infoLength;

			buttonHeight =  physicalRange * physicalRange/(physicalRange + RealRange);

			buttonHeight = (std::max)( buttonHeight, static_cast<float>(SCROLLBUTTONHEIGHT) );

			if ( mouseY > scrollPos + SCROLLMIN && mouseY < scrollPos + SCROLLMIN + buttonHeight )
			{
				lastYClick = scrollPos;
			}
			else if ( ( userInput->leftMouseReleased() || userInput->getMouseLeftHeld() > .5 ) )
			{
				lastYClick = -1;
				if ( mouseY > buttons[0].location[2].y 
					&& mouseY < buttons[1].location[0].y )
				{
			
					float newScrollPos = scrollPos;
					// if above the thumb, page up, otherwise page down
					if ( mouseY < SCROLLMIN + scrollPos )
					{
							newScrollPos = scrollPos - buttonHeight;
					}
					else if ( mouseY > SCROLLMIN + scrollPos + buttonHeight )
					{
							newScrollPos = scrollPos + buttonHeight;
					}

					newScrollPos = (std::max)( newScrollPos, 0.0f );
					newScrollPos = (std::min)( newScrollPos, infoLength );

					
					setScrollPos( newScrollPos );

				}

			}			
		}
		
		

		if ( userInput->leftMouseReleased() )
			lastYClick = -1;
		
}
	

// INFO-IMGUI-SLICE-1: shared weapon dedup/enumeration. Verbatim port of the
// former inline loop in drawScrollingStuff (same zero-inits, same walk over
// pUnit->inventory[numOther .. numOther+numWeapons]). Returns the distinct entry
// count. Used by BOTH the legacy draw and gatherLoadoutRows so they cannot drift.
int InfoWindow::extractWeaponRows( unsigned char names[60], char disabledCount[60][2],
	long ammo[60], char ranges[60], bool bDraw[4] ) const
{
	memset( disabledCount, 0, sizeof( char ) * 60 * 2);

	// sebi: ORIG BUG FIX memory owerwrite fix fow win64 build: sizeof(long) < sizeof(char*)!
	//memset( names, 0, sizeof( char* ) * 60 );
	// NOTE: names is a decayed pointer parameter here (not a fixed array), so the
	// MemSet(array&) template can't deduce N; explicit memset zeroes all 60 bytes.
	memset( names, 0, sizeof( unsigned char ) * 60 );

	memset( ranges, 0, sizeof( char ) * 60 );
	memset( ammo, 0, sizeof( long ) * 60 );

	memset( bDraw, 0, sizeof( bool ) * 4 );

	int i = 0;

	for (long curWeapon = pUnit->numOther; curWeapon < (pUnit->numOther + pUnit->numWeapons); curWeapon++)
	{
			unsigned char nName = pUnit->inventory[curWeapon].masterID;
			bool bFound = 0;
			for ( int j = 0; j < i; j++ )
			{
				if ( nName == names[j] )
				{
					disabledCount[j][1] ++;
					if (!pUnit->inventory[curWeapon].disabled && (pUnit->getWeaponShots(curWeapon) > 0)
						&& pUnit->inventory[curWeapon].health > 0 )
					{
						disabledCount[j][0]++;
					}

					bFound = true;
				}
			}
			if ( bFound )
				continue;

			names[i] = nName;

			//ONly need to add in the AMMO once!!
			// Glenn has taken the liberty of combining all shots of this type
			// into one master ammo list!!
			if ( MasterComponent::masterList[pUnit->inventory[curWeapon].masterID].getWeaponAmmoType() == WEAPON_AMMO_NONE
				|| useUnlimitedAmmo )
				ammo[i] = -1;
			else
				ammo[i] += pUnit->getWeaponShots( curWeapon );

			int  range = MasterComponent::masterList[pUnit->inventory[curWeapon].masterID].getWeaponRange();
			ranges[i] = range;
			bDraw[range] = true;

			if (!pUnit->inventory[curWeapon].disabled && (pUnit->getWeaponShots(curWeapon) > 0)
				&& pUnit->inventory[curWeapon].health > 0 )
			{
				disabledCount[i][0]++;
			}
			disabledCount[i][1]++;
			i++;
	}

	return i;
}

// INFO-IMGUI-SLICE-1: build the formatted loadout list for the ImGui panel.
// Mirrors drawScrollingStuff's text: weapon rows "N/M  NAME" grouped by range
// (textColors[]), each optional "shots left" ammo row, then the ecm/probe/jump/
// sensor equipment rows (0xffc29b00). Weapon enumeration is the shared helper.
void InfoWindow::extractLoadoutRows( std::vector<InfoLoadoutRow>& out ) const
{
	out.clear();
	if ( !pUnit )
		return;

	uint32_t textColors[4] = { 0xff6E7C00, 0xff005392, 0xffA21600 };

	char disabledCount[60][2];
	long ammo[60];
	char ranges[60];
	unsigned char names[60];
	bool bDraw[4];

	int count = extractWeaponRows( names, disabledCount, ammo, ranges, bDraw );

	for ( int j = 0; j < 3; j++ )
	{
		if ( !bDraw[j] )
			continue;

		for ( int k = 0; k < count; k++ )
		{
			if ( !names[k] )
				break;
			if ( ranges[k] != j )
				continue;

			char tmpName[256];
			cLoadString( IDS_COMP_ABBR0 + names[k], tmpName, 255 );
			out.push_back( { std::format( "{:d}/{:d}  {}", mc2::fmt::as_i32( disabledCount[k][0] ), mc2::fmt::as_i32( disabledCount[k][1] ), mc2::fmt::safe( tmpName ) ), textColors[j] } );

			if ( ammo[k] != -1 )
			{
				char tmpNumber[64];
				char tmpNumber2[64];
				cLoadString( IDS_MISSION_SHOTSLEFT, tmpNumber, 63 );
				sprintf( tmpNumber2, tmpNumber, ammo[k] );
				out.push_back( { std::string( tmpNumber2 ), textColors[j] } );
			}
		}
	}

	// equipment: ecm / probe / jumpJets / sensor (same enumeration as legacy).
	char eq[4];
	eq[0] = pUnit->ecm;
	eq[1] = pUnit->probe;
	eq[2] = pUnit->jumpJets;
	eq[3] = pUnit->isMech() ? pUnit->sensor : 255;

	for (const auto eqElem : eq)
	{
		if ( eqElem != 255 )
		{
			char tmpName[256];
			cLoadString( IDS_COMP_ABBR0 + pUnit->inventory[eqElem].masterID, tmpName, 255 );
			out.push_back( { std::string( tmpName ), 0xffc29b00u } );
		}
	}
}

// INFO-IMGUI-SLICE-2: pilot section content shared by BOTH the legacy draw and
// the ImGui panel. Verbatim lift of the former inline reads in
// drawScrollingStuff (same visibility condition, same dead-pilot fallback, same
// ACE rank special-case, same specialty enumeration) so the two paths cannot
// drift on WHAT the pilot section says.
bool InfoWindow::extractPilotRows( InfoPilotRows& out ) const
{
	out = InfoPilotRows();

	if ( !pUnit )
		return false;

	// DON'T DO PILOT INFO if ENEMY OR mech is destroyed or disabled.
	if ( !( pUnit->getTeam() && !Team::home->isEnemy(pUnit->getTeam()) && pUnit->isMech()
			&& !pUnit->isDisabled() && !pUnit->isDestroyed() ) )
		return false;

	MechWarrior* pWarrior = pUnit->getPilot();
	if ( !pWarrior )
		return false;

	out.active = pWarrior->active();

	char buffer[256];
	if ( out.active )
		out.name = pWarrior->getName();
	else
	{
		cLoadString( IDS_NOPILOT, buffer, 255 );
		out.name = buffer;
	}

	out.rank = pWarrior->getRank();
	//ACE not continguous with other ranks.  Added too late!
	if ( out.rank != 4 )
		cLoadString( IDS_GREEN + out.rank, buffer, 256 );
	else
		cLoadString( IDS_ACE, buffer, 256 );
	out.rankText = buffer;

	out.gunnery  = pWarrior->getSkill( MWS_GUNNERY );
	out.piloting = pWarrior->getSkill( MWS_PILOTING );

	for ( int i = 0; i < NUM_SPECIALTY_SKILLS; i++ )
	{
		if ( pWarrior->specialtySkills[i] )
		{
			cLoadString( IDS_SPECIALTY + i, buffer, 256 );
			out.specialties.push_back( buffer );
		}
	}

	return true;
}

void InfoWindow::drawScrollingStuff()
{
	if ( !pUnit )
		return;

	float offset = 0.f;
	if ( infoLength )
	{
		float increment = infoLength/(SCROLLMAX - SCROLLMIN );
		offset = -increment * scrollPos;
	}

	float curY = SCROLLBOXTOP + SECTIONSKIP  + offset;

	if ( icon )
	{
		if ( SCROLLTOP + offset > NAMETOP ) // draw icons if visible
		{
			icon->renderUnitIcon( MECHLEFT, curY, MECHRIGHT, curY + MECHHEIGHT);
			icon->renderUnitIconBack( MECHBACKLEFT, curY, MECHBACKRIGHT, curY + MECHHEIGHT );
		}

		curY += MECHHEIGHT + SECTIONSKIP;

		if( curY > NAMEBOTTOM ) // draw divider if visible
			drawDivider( curY );

	}

	uint32_t textColors[4] = { 0xff6E7C00, 0xff005392, 0xffA21600 };
	

	char disabledCount[60][2];
	long ammo[60];
	char ranges[60];
	unsigned char names[60];// sebi: changed long to char; MUST be unsigned — masterID (CompBas#) can be >=128 for Clan components, signed char would underflow IDS_COMP_ABBR0+names[i]
	bool bDraw[4];

	int curComponentCount = 60;

	// INFO-IMGUI-SLICE-1: dedup/enumeration factored into the shared helper so the
	// ImGui panel (gatherLoadoutRows) lists identical hardware. Behaviour is
	// byte-identical to the former inline loop (same zero-inits + same walk).
	int i = gatherWeaponRows( names, disabledCount, ammo, ranges, bDraw );

	curY += SECTIONSKIP;


	unsigned long height = componentFont.height();
	

	// removing headers for now
//	long stringIDs[4] = { IDS_SHORT, IDS_MEDIUM, IDS_LONG, IDS_COMPONENT};
//	long headerColors[4] = { 0xFFC8E100, 0xff0091FF, 0xFFFF0000, 0xffFF8A00 };
	std::string capHeader;



	for ( int j = 0; j < 3; j++ )
	{
		if ( !bDraw[j] ) // make sure we have one
			continue;

//		char header[64];
		if ( curY > NAMETOP )
		{
//			cLoadString( stringIDs[j], header, 63 );
//			capHeader = header;
			//capHeader.MakeUpper();
//			componentFont.render( capHeader, COMPONENTLEFT, curY, SCROLLLEFT - COMPONENTLEFT, height, headerColors[j], 0, 0 );
		}
//		curY += height;
		
		for ( i = 0; i < curComponentCount; i++ )
		{
			if ( !names[i] )
				break;
			char tmpName[256];
			if ( ranges[i] == j )
			{
				if ( curY > NAMETOP )
				{
					// EQUIP-ADD-UI-NAME-1 (S3): ADD-filled ids have no string-
					// table abbrev; use the def DisplayName. Miss -> stock path.
					const char* addName =
						equipmentAddUiTextForStringId( IDS_COMP_ABBR0 + names[i] );
					if ( addName )
					{
						strncpy( tmpName, addName, 255 );
						tmpName[255] = 0;
					}
					else
						cLoadString( IDS_COMP_ABBR0 + names[i], tmpName, 255 );
					capHeader = std::format( "{:d}/{:d}  {}", mc2::fmt::as_i32( disabledCount[i][0] ), mc2::fmt::as_i32( disabledCount[i][1] ), mc2::fmt::safe( tmpName ) );
					componentFont.render( capHeader.c_str(), COMPONENTLEFT, curY, SCROLLLEFT - COMPONENTLEFT, height, textColors[j], 0, 0 );
				}
				curY += height;	

				if ( ammo[i] != -1 )
				{
					if ( curY > NAMETOP )
					{
						// make the ammo number
						char tmpNumber[64];
						char tmpNumber2[64];
						cLoadString( IDS_MISSION_SHOTSLEFT, tmpNumber, 63 );
						sprintf( tmpNumber2, tmpNumber, ammo[i] );
						componentFont.render( tmpNumber2, COMPONENTLEFT, curY, SCROLLLEFT - COMPONENTLEFT, height,  textColors[j], 0, 0 );	
				
					}
					curY += height;
				}
			}
	
		}

		curY += SECTIONSKIP;

		if ( curY > NAMEBOTTOM )
			drawDivider( curY );

		curY += SECTIONSKIP;

	}

	//sebi: ORIG BUG FIX memory owerwrite fix
	//memset( names, 0, sizeof( char* ) * 60 );
	MemSet(names, 0);

	char count[4];
	count[0] = hudInfo.equipment[0];
	count[1] = hudInfo.equipment[1];
	count[2] = hudInfo.equipment[2];
	count[3] = hudInfo.equipment[3];

	if ((count[0] || count[1] || count[2] || count[3] ) )
	{
		if ( curY > NAMETOP )
		{
//			cLoadString( stringIDs[j], header, 63 );
//			componentFont.render( header, COMPONENTLEFT, curY, SCROLLLEFT - COMPONENTLEFT, height, headerColors[j], 0, 0 );
		}

//		curY += height;
	}
	
	

	for (int curWeapon = 0; curWeapon < 4; curWeapon++ )
	{
		if ( count[curWeapon] != 255)
		{
			uint32_t color = 0xffc29b00;
			//Neither the ecm, probe, sensors or JumpJets can ever REALLY be disabled. No matter what the setting is!!
//			if (pUnit->inventory[count[curWeapon]].disabled) 
//				color = 0xff7f7f7f;

			char tmpName[256];
			// EQUIP-ADD-UI-NAME-1 (S3): same DisplayName fallback as above.
			const char* addName = equipmentAddUiTextForStringId(
				IDS_COMP_ABBR0 + hudInfo.equipmentMaster[curWeapon] );
			if ( addName )
			{
				strncpy( tmpName, addName, 255 );
				tmpName[255] = 0;
			}
			else
				cLoadString( IDS_COMP_ABBR0 + hudInfo.equipmentMaster[curWeapon], tmpName, 255 );

			if ( curY > NAMETOP )
			{
				componentFont.render( tmpName, COMPONENTLEFT, curY, SCROLLLEFT - COMPONENTLEFT, height, color, 0, 0 );
			}
	
			curY += height;
			curY += InfoWindow::SECTIONSKIP;

		}
	}

	if ( curY > NAMEBOTTOM )
		drawDivider( curY );

	curY += SECTIONSKIP;

	// DON'T DO PILOT INFO if ENEMY OR mech is destroyed or disabled.
	// INFO-IMGUI-SLICE-2: visibility condition + name/rank/skills/specialty
	// content factored into the shared gatherPilotRows helper (ImGui panel uses
	// the same one). Draw calls below are byte-identical to the former inline
	// reads — same strings, same values, same order.
	const auto& pilotRows = hudInfo.pilot;
	if (hudInfo.pilotVisible)
	{
		if ( icon )
		{
			if ( curY > NAMETOP )
			{
				if ( pilotRows.active )
					icon->renderPilotIcon( PILOTLEFT, curY, PILOTRIGHT, curY + PILOTHEIGHT );
				GUI_RECT tmpRect = { PILOTLEFT, static_cast<long>(curY), (PILOTRIGHT + 1), static_cast<long>((curY + PILOTHEIGHT + 1)) };
				drawEmptyRect( tmpRect, SCROLLCOLOR, SCROLLCOLOR  );

				float right = SCROLLLEFT;
				float top = curY + PILOTHEIGHT/2 - height/2;
				float bottom = top + height;
				// draw the name of the pilot
				capHeader = pilotRows.name;
				componentFont.render( capHeader.c_str(), PILOTNAMELEFT, top, right - PILOTNAMELEFT, bottom - top, 0xff005392, 0, 0 );

			}

			curY += PILOTHEIGHT;
			curY += SECTIONSKIP;
		}

		curY += SECTIONSKIP;

		if ( curY > NAMETOP )
		{
			componentFont.render( pilotRows.rankText.c_str(), SKILLLEFT, curY, SCROLLLEFT - SKILLLEFT, height, 0xff005392, 0, 0 );
		}

		int currentSkill = pilotRows.rank;
		for (int j = 0; j < 3; j ++ )
		{
			gos_VERTEX v[4];

			float height = skillInfos[currentSkill].location[1].y - skillInfos[currentSkill].location[0].y;
			for ( i = 0; i < 4; i++ )
			{
				v[i] = skillInfos[currentSkill].location[i];
				v[i].y = curY;
				v[i].rhw = .5;
			}

			v[1].y = v[2].y = curY + height;

			if ( curY > NAMETOP )
			{
				GUI_RECT tmpRect = { static_cast<long>((v[0].x - .5)), static_cast<long>((v[0].y - .5)), static_cast<long>((v[2].x + 1.5)), static_cast<long>((v[2].y + 1.5)) };
				drawEmptyRect( tmpRect, 0xff002f55, 0xff002f55  );


				unsigned long gosID = mc2::textures::sourceId( skillInfos[currentSkill].textureHandle );
				gos_SetRenderState( gos_State_Texture, gosID );
				gos_DrawQuads( v, 4 );

				if ( j != 0 )
				{
					int skill = ( j == 1 ) ? pilotRows.gunnery : pilotRows.piloting;
					drawSkillBar( skill, curY, height );
				}
			}

			curY += height;
			curY += SECTIONSKIP;
			currentSkill = j + 5;

		}



		for (const auto & specialty : pilotRows.specialties)
		{
			if ( curY > NAMETOP )
			{
				componentFont.render( specialty.c_str(), NAMELEFT, curY, NAMERIGHT - NAMELEFT, height, 0xff005392, 0, 0 );
			}
			curY += height;
		}
	}
	else
		curY += 10 * SECTIONSKIP;


	if ( infoLength == 0 )
		infoLength = curY - SCROLLTOP - ( SCROLLBOTTOM - SCROLLTOP );
}

void InfoWindow::handleClick( int ID )
{
	switch( ID )
	{
	case SCROLLUP:
		setScrollPos( scrollPos - scrollAmount() );
		break;

	case SCROLLDOWN:
		setScrollPos( scrollPos + scrollAmount() );
		break;

	}
}

void InfoWindow::drawDivider( float yVal )
{
	gos_VERTEX v[2];

//	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha );

	gos_SetRenderState( gos_State_Specular,FALSE );

	gos_SetRenderState( gos_State_AlphaTest, 0 );
		
	gos_SetRenderState( gos_State_Texture, 0 );

	gos_SetRenderState( gos_State_Filter, gos_FilterNone );


	memset( v, 0, sizeof( gos_VERTEX ) * 2 );
	for (auto & vElem : v)
		vElem.rhw = .5f;

	v[0].x = DIVIDERLEFT;
	v[0].y = v[1].y = yVal;
	v[1].x = DIVIDERRIGHT;
	v[0].argb = v[1].argb = DIVIDERCOLOR;

 	gos_DrawLines( v, 2 );

}

void InfoWindow::drawSkillBar( int skill, float yVal, float height )
{
	float left = InfoWindow::SKILLLEFT;
	float right = InfoWindow::SKILLRIGHT;

	int barCount = skill/InfoWindow::NUMBERSKILLBARS;

	int redIncrement = 0;
	int greenIncrement = 0;
	int blueIncrement = 0;
	if ( barCount )
	{
		redIncrement =  (205/barCount) << 16;
		greenIncrement = ((234-83)/barCount) << 8;
		blueIncrement = (255 - 146)/barCount;
	}

	unsigned long color = 0xff005392;

	GUI_RECT outSideRect = { static_cast<long>((left - SKILLSKIP + .5f)), static_cast<long>((yVal - .5f)), static_cast<long>((right + SKILLSKIP + .5f)), static_cast<long>((yVal + height + 1.5f))};
	drawRect( outSideRect, 0xff000000 );
	GUI_RECT rect = { static_cast<long>((left  + InfoWindow::SKILLSKIP)), static_cast<long>((yVal + InfoWindow::SKILLSKIP + .5f)),
		static_cast<long>((left  + InfoWindow::SKILLSKIP + SKILLUNITWIDTH)), static_cast<long>((yVal + height - InfoWindow::SKILLSKIP + .5f)) };

	drawEmptyRect( outSideRect, 0xff002f55, 0xff002f55 );

	for ( int i = 0; i < barCount; i++ )
	{
		drawRect( rect, color );
		color += redIncrement;
		color += greenIncrement;
		color += blueIncrement;
		
		rect.left += SKILLUNITWIDTH + 1;
		rect.right = rect.left + SKILLUNITWIDTH;
	}

	char buffer[32];
	mc2::fmt::to_buf(buffer, "{:d}", mc2::fmt::as_i32(skill));
	componentFont.render( buffer, SKILLRIGHT+2, yVal, SCROLLLEFT - SKILLRIGHT - 2, SKILLHEIGHT, 0xff005392, 0, 0 );
}

void InfoWindow::setScrollPos( int where )
{
	if ( where < 0 )
		scrollPos = 0;
	else if  ( where > SCROLLMAX - SCROLLMIN )
		scrollPos = SCROLLMAX - SCROLLMIN;
	else
		scrollPos = where;
}



//*************************************************************************************************
// end of file ( InfoWindow.cpp )

