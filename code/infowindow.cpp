#define INFOWINDOW_CPP
/*************************************************************************************************\
InfoWindow.cpp			: Implementation of the InfoWindow component.
//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"infowindow.h"
#include"mclib.h"
#include "../resource.h"
#include"mover.h"
#include"mechicon.h"
#include"team.h"
#include"txmmgr.h"
#include"estring.h"
#include"gameos.hpp"        // INFO-IMGUI-SLICE-1: gos_ComputeHudCanvasBox + Environment
#include"imgui.h"           // INFO-IMGUI-SLICE-1: ImGui info panel
#include<cstdlib>           // INFO-IMGUI-SLICE-1: getenv
#include<cctype>            // INFO-IMGUI-SLICE-2: toupper (bitmap-font-style caps)
#include<cstdint>           // INFO-IMGUI-SLICE-2: intptr_t (ImTextureID cast)

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
StaticInfo* InfoWindow::skillInfos = 0;
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

InfoWindow* InfoWindow::s_instance = NULL;


#define SCROLLAMOUNT		3.f * Environment.screenWidth/640.f
extern float WeaponRanges[NUM_WEAPON_RANGE_TYPES][2];

// initialize statics
ButtonData InfoWindow::buttonData[2];;

extern bool useUnlimitedAmmo;

InfoWindow::InfoWindow()
{
	pUnit = NULL;
	scrollLength = 0;
	scrollPos = 0;
	backgroundTexture = 0;
	icon = NULL;
	infoLength = 0;

	skillInfos = new StaticInfo[7];
	memset( skillInfos, 0, sizeof( StaticInfo ) *  7);
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
			long gosID = mcTextureManager->get_gosTextureHandle( skillInfos[i].textureHandle );
			mcTextureManager->removeTexture( gosID );
		}
	}

    // sebi: WTF??? thank you clang
	//memset( skillInfos, 0, sizeof( skillInfos ));
	memset( skillInfos, 0, sizeof( skillInfos ) * 7);

	//sebi: ORIG BUG FIX what a reason to clear this and then iterate over it?
	// obviously clear shoul dbe done after!
	//memset( buttonData, 0, sizeof( buttonData ) );


	for (int i = 0; i < 2; i++ )
	{
		if ( buttonData[i].textureHandle )
		{
			long gosID = mcTextureManager->get_gosTextureHandle( buttonData[i].textureHandle );
			mcTextureManager->removeTexture( gosID );
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
		sprintf( SkillText, "Skill%ld", i );
		skillInfos[i].init( file, SkillText ,ControlGui::hiResOffsetX, ControlGui::hiResOffsetY);
	
	}

	InfoWindow::COMPONENTLEFT = InfoWindow::PILOTLEFT;
	

}

InfoWindow::~InfoWindow()
{
	delete [] skillInfos;
}

void InfoWindow::setUnit(Mover* pNewMover)
{

	if ( pNewMover && ( pNewMover->getTeamId() != Team::home->getId() &&
		( CONTACT_VISUAL != pNewMover->getContactStatus(Team::home->getId(), true)) 
		&& !pNewMover->isDisabled() ))
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
			icon = NULL;
		}

		icon = NULL;

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

void InfoWindow::drawName( const char* name )
{
	GUI_RECT rect = { NAMELEFT, NAMETOP, NAMERIGHT, SCROLLTOP};
	drawRect( rect, 0xff000000 );
	drawEmptyRect( NameRect, 0xff002f55, 0xff002f55 );
	nameFont.render( name, NAMELEFT, NAMETOP, NAMERIGHT - NAMELEFT, NAMEBOTTOM - NAMETOP, 0xff5c96c2, 0, 3 );
}

// INFO-IMGUI-SLICE-1: gate. Default OFF -> legacy procedural panel, byte-identical.
bool InfoWindow::imguiInfoEnabled()
{
	static const bool s_on = []{
		const char* v = getenv("MC2_UI_IMGUI_INFO");
		return v && v[0] && v[0] != '0';
	}();
	return s_on;
}

// INFO-IMGUI-SLICE-1: ImGui unit-info panel. Reproduces the procedural legacy
// draw (header box + unit name + health bar + loadout list) bound to the live
// pUnit read this frame. The mech paperdoll icon is DEFERRED to slice-2 (its
// area is left blank). Loadout content comes from the shared gatherLoadoutRows
// helper so it cannot drift from the legacy enumeration.
void InfoWindow::renderImGui()
{
	// Map the authored InfoWindow rect (buttonLayout800: X=3 Y=434 W=162 H=163,
	// 800x600 design space) into the 16:9 HUD canvas box so the panel tracks the
	// same aspect anchor as the rest of the HUD chrome (same pattern as
	// PauseWindow::renderImGui).
	int bx = 0, by = 0, bw = 0, bh = 0;
	if ( !gos_ComputeHudCanvasBox( Environment.drawableWidth, Environment.drawableHeight,
			&bx, &by, &bw, &bh ) )
	{ bx = 0; by = 0; bw = Environment.drawableWidth; bh = Environment.drawableHeight; }
	const float sx = (float)bw / 800.0f;
	const float sy = (float)bh / 600.0f;
	const float px = bx +   3.0f * sx;
	const float py = by + 434.0f * sy;
	const float pw = 162.0f * sx;
	const float ph = 163.0f * sy;

	ImGui::SetNextWindowPos( ImVec2( px, py ) );
	ImGui::SetNextWindowSize( ImVec2( pw, ph ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
	ImGui::Begin( "##mc2_info", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBackground );

	ImDrawList* dl = ImGui::GetWindowDrawList();

	// Panel backing: legacy fills the info area black. Header sub-box is dark blue
	// (legacy InfoHeaderBox 0xff002f55). Authored (800x600) sub-rects derived from
	// the InfoWindow layout: header (3,434)..(165,455); content box below it.
	auto AX = [&]( float ax ){ return bx + ax * sx; };
	auto AY = [&]( float ay ){ return by + ay * sy; };

	// full-panel black backing
	dl->AddRectFilled( ImVec2( px, py ), ImVec2( px + pw, py + ph ), IM_COL32( 0, 0, 0, 255 ) );

	// header box (dark blue). ~21px tall header band at the top of the panel.
	const float hdrTop = AY( 436.0f );
	const float hdrBot = AY( 456.0f );
	dl->AddRectFilled( ImVec2( AX( 4.0f ), hdrTop ), ImVec2( AX( 164.0f ), hdrBot ),
		IM_COL32( 0x00, 0x2F, 0x55, 255 ) );

	// INFO-IMGUI-SLICE-2: legacy panel text is a bitmap font whose glyphs are
	// ALL-CAPS; ImGui's default font is mixed-case. Uppercase every string the
	// panel displays so the text reads like the legacy panel. (Loading the
	// legacy bitmap font into the ImGui atlas is deferred — no existing ImGui
	// font-loading helper in the repo, and building an importer is out of
	// slice scope.)
	auto ToUpper = []( std::string s ){
		for ( char& c : s )
			c = (char)toupper( (unsigned char)c );
		return s;
	};

	// unit name (legacy drawName blue 0xff5c96c2), centered vertically in header.
	char nameBuf[256];
	const char* name;
	if ( pUnit )
		name = pUnit->getIfaceName();
	else
	{
		cLoadString( IDS_NOUNIT, nameBuf, 255 );
		name = nameBuf;
	}
	if ( name )
	{
		const std::string upName = ToUpper( name );
		const ImVec2 ts = ImGui::CalcTextSize( upName.c_str() );
		const float ncy = ( hdrTop + hdrBot ) * 0.5f - ts.y * 0.5f;
		dl->AddText( ImVec2( AX( 8.0f ), ncy ), IM_COL32( 0x5C, 0x96, 0xC2, 255 ), upName.c_str() );
	}

	// health bar (legacy HEALTH rect, just under the header). Color by barStatus:
	// >=0.5 green, >0.2 yellow, else red. Dim bg (argb & 0xff7f7f7f) full width,
	// then filled fg to width*barStatus.
	const float hbL = AX( 8.0f );
	const float hbR = AX( 160.0f );
	const float hbT = AY( 458.0f );
	const float hbB = AY( 463.0f );
	if ( pUnit )
	{
		float barStatus = pUnit->getAppearance()->barStatus;
		if ( barStatus > 1.0f ) barStatus = 1.0f;
		if ( barStatus < 0.0f ) barStatus = 0.0f;

		DWORD color;
		if ( barStatus >= 0.5 )      color = SB_GREEN;
		else if ( barStatus > 0.2 )  color = SB_YELLOW;
		else                         color = SB_RED;

		// SB_* pack as ARGB; force full alpha for display (legacy blends 0x6f but
		// over the black backing the fill reads solid like the reference).
		auto argb2im = []( uint32_t argb ){
			return IM_COL32( (argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff, (argb >> 24) & 0xff );
		};
		const uint32_t dim  = ( (uint32_t)color & 0xff7f7f7f ) | 0xff000000u;
		const uint32_t full = ( (uint32_t)color )              | 0xff000000u;

		dl->AddRectFilled( ImVec2( hbL, hbT ), ImVec2( hbR, hbB ), argb2im( dim ) );
		dl->AddRectFilled( ImVec2( hbL, hbT ),
			ImVec2( hbL + ( hbR - hbL ) * barStatus, hbB ), argb2im( full ) );
	}

	// INFO-IMGUI-SLICE-2: mech paperdoll (front + back schematic). Same atlas
	// sub-rects the legacy renderUnitIcon/-Back draw (damage-state UVs exposed
	// by ForceGroupIcon::getUnitIcon*TexUV), composited via the ImGui draw list
	// so it layers correctly over the panel backing. Authored rects mirror the
	// buttonLayout800 MechIcon block (front 14..45, back 51..82, 38 tall),
	// placed in the area slice-1 reserved: (4,465)..(164,505).
	if ( pUnit && icon )
	{
		unsigned int iconTex = 0;
		float u0, v0, u1, v1;
		const float dollTop = AY( 466.0f );
		const float dollBot = AY( 504.0f );
		if ( icon->getUnitIconTexUV( iconTex, u0, v0, u1, v1 ) )
		{
			dl->AddImage( (ImTextureID)(intptr_t)iconTex,
				ImVec2( AX( 14.0f ), dollTop ), ImVec2( AX( 45.0f ), dollBot ),
				ImVec2( u0, v0 ), ImVec2( u1, v1 ) );
		}
		if ( icon->getUnitIconBackTexUV( iconTex, u0, v0, u1, v1 ) )
		{
			dl->AddImage( (ImTextureID)(intptr_t)iconTex,
				ImVec2( AX( 51.0f ), dollTop ), ImVec2( AX( 82.0f ), dollBot ),
				ImVec2( u0, v0 ), ImVec2( u1, v1 ) );
		}
	}

	// loadout list in a scrolling child region. ImGui's scrollbar replaces the
	// legacy hand-rolled thumb/scroll-buttons. Rows from the shared helper.
	const float listTop = AY( 507.0f );
	ImGui::SetCursorScreenPos( ImVec2( AX( 6.0f ), listTop ) );
	if ( ImGui::BeginChild( "##mc2_info_loadout",
			ImVec2( AX( 164.0f ) - AX( 6.0f ), ( py + ph ) - listTop - 2.0f ),
			false, ImGuiWindowFlags_NoBackground ) )
	{
		if ( pUnit )
		{
			std::vector<InfoLoadoutRow> rows;
			gatherLoadoutRows( rows );
			ImGui::PushTextWrapPos( 0.0f );  // wrap at child width
			for ( const InfoLoadoutRow& r : rows )
			{
				const uint32_t a = r.argb;
				ImGui::PushStyleColor( ImGuiCol_Text,
					IM_COL32( (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff, (a >> 24) & 0xff ) );
				// INFO-IMGUI-SLICE-2: ALL-CAPS like the legacy bitmap font.
				ImGui::TextUnformatted( ToUpper( r.text ).c_str() );
				ImGui::PopStyleColor();
			}
			ImGui::PopTextWrapPos();

			// INFO-IMGUI-SLICE-2: pilot section (photo + name + rank + skills +
			// specialty skills). Content from the shared gatherPilotRows helper
			// (the legacy drawScrollingStuff consumes the SAME gather). Legacy
			// pilot-section blue is 0xff005392; skill meters are simplified to
			// "GUNNERY/PILOTING <value>" text rows this slice (the legacy
			// medallion/bar textures stay legacy-only).
			InfoPilotRows pilot;
			if ( gatherPilotRows( pilot ) )
			{
				ImGui::Dummy( ImVec2( 1.0f, 4.0f * sy ) );

				const ImU32 pilotBlue = IM_COL32( 0x00, 0x53, 0x92, 255 );

				// pilot photo (authored 24x36, PilotLeft block) + name beside it
				unsigned int pTex = 0;
				float pu0, pv0, pu1, pv1;
				if ( pilot.active && icon && icon->getPilotIconTexUV( pTex, pu0, pv0, pu1, pv1 ) )
				{
					ImGui::Image( (ImTextureID)(intptr_t)pTex,
						ImVec2( 24.0f * sx, 36.0f * sy ),
						ImVec2( pu0, pv0 ), ImVec2( pu1, pv1 ) );
					ImGui::SameLine();
				}
				ImGui::PushStyleColor( ImGuiCol_Text, pilotBlue );
				ImGui::TextUnformatted( ToUpper( pilot.name ).c_str() );

				// rank + skill rows
				ImGui::TextUnformatted( ToUpper( pilot.rankText ).c_str() );
				ImGui::Text( "GUNNERY  %d", pilot.gunnery );
				ImGui::Text( "PILOTING %d", pilot.piloting );

				// specialty skills
				for ( const std::string& s : pilot.specialties )
					ImGui::TextUnformatted( ToUpper( s ).c_str() );
				ImGui::PopStyleColor();
			}

			// INFO-IMGUI-SLICE-2 capture-fixture assist: when the MC2_BOOT_INFO
			// capture fixture is driving (headless recipe only — env already
			// registered by slice-1), pin the list scrolled to bottom from
			// bootFrame+3000 on so a late screenshot (recipe: boot 700 / shot
			// 4200) can pixel-verify the pilot rows. Capture the top-of-panel
			// state with a screenshot frame below bootFrame+3000. No effect
			// when MC2_BOOT_INFO is unset (normal play).
			{
				static const long s_bootInfoScroll = []{
					const char* v = getenv("MC2_BOOT_INFO");
					return (v && v[0]) ? atol(v) : 0L;
				}();
				if ( s_bootInfoScroll > 0 )
				{
					extern uint32_t g_mc2FrameCounter;
					if ( (long)g_mc2FrameCounter >= s_bootInfoScroll + 3000 )
						ImGui::SetScrollY( ImGui::GetScrollMaxY() );
				}
			}
		}
	}
	ImGui::EndChild();

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


	drawScrollingStuff();

	GUI_RECT tmpRect = { SCROLLBOXLEFT, SCROLLBOXBOTTOM, SCROLLBOXRIGHT, INFOTOP + INFOHEIGHT };
	drawRect( tmpRect, 0xff000000 );

	for ( int i = 0; i < 2; i++ )
	{
		if ( buttons[i].isEnabled() )
			buttons[i].render();
	}
	
	long scrollBarLength = buttons[1].location[0].y - buttons[0].location[2].y - 4 - SCROLLBUTTONHEIGHT;

	gos_VERTEX v[4];
	for (int i = 0; i < 4; i++ )
	{
		v[i].argb = 0xff5c96c2;
		v[i].frgb = 0;
		v[i].rhw = .5;
		v[i].x = SCROLLBUTTONX;
		v[i].y = SCROLLMIN + scrollPos;
		v[i].z = 0.f;
		v[i].u = v[i].v = 0.f;
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

		if ( buttonHeight < SCROLLBUTTONHEIGHT )
			buttonHeight = SCROLLBUTTONHEIGHT;

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
		drawName( pUnit->getIfaceName() );
	
		gos_SetRenderState( gos_State_Texture, 0 );

		// draw the health bar
		DWORD					color;
		
		float barStatus = pUnit->getAppearance()->barStatus;
			
		if (barStatus > 1.0f)
			barStatus = 1.0f;

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
		for ( int i = 0; i < 4; i++ )
			v[i].argb = dimColor;

		gos_DrawQuads( v, 4 );

		v[2].x = v[3].x = HEALTHLEFT + (HEALTHRIGHT - HEALTHLEFT)* barStatus;

		for (int i = 0; i < 4; i++ )
			v[i].argb = color | 0xff000000;

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
	if ( pUnit && ( pUnit->getTeamId() != Team::home->getId() &&
		( CONTACT_VISUAL != (pUnit->getContactStatus(Team::home->getId(), true)) 
		&& !pUnit->isDisabled() ) ) )
	{
		setUnit( 0 );
		return;
	}

	// HUD-corrected: the unit-info panel + scroll bar draw shrunk by s_hud_scale,
	// so hit-test in HUD-inverse space to match the drawn (shrunk) widgets. No-op
	// when the HUD scale is off / outside the rendered bottom band.
	long mouseX = userInput->getMouseHudX();
	long mouseY = userInput->getMouseHudY();



	if ( icon )
		icon->update();

	for ( int i = 0; i < 2; i++ )
	{	
		if ( buttons[i].location[0].x <= mouseX && mouseX <= buttons[i].location[2].x
				&& mouseY >= buttons[i].location[0].y && mouseY <= buttons[i].location[1].y  )
		{
			if ( userInput->getMouseLeftButtonState() == MC2_MOUSE_DOWN && buttons[i].isEnabled() )
			{
				{
					handleClick( buttons[i].ID );
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

			if ( buttonHeight < SCROLLBUTTONHEIGHT )
				buttonHeight = SCROLLBUTTONHEIGHT;

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

					if( newScrollPos < 0 )
							newScrollPos = 0;

					if( newScrollPos > infoLength )
						newScrollPos = infoLength;

					
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
int InfoWindow::gatherWeaponRows( unsigned char names[60], char disabledCount[60][2],
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
void InfoWindow::gatherLoadoutRows( std::vector<InfoLoadoutRow>& out ) const
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

	int count = gatherWeaponRows( names, disabledCount, ammo, ranges, bDraw );

	EString rowText;
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
			rowText.Format( "%ld/%ld  %s", disabledCount[k][0], disabledCount[k][1], tmpName );
			out.push_back( { std::string( rowText.Data() ), textColors[j] } );

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

	for ( int e = 0; e < 4; e++ )
	{
		if ( eq[e] != 255 )
		{
			char tmpName[256];
			cLoadString( IDS_COMP_ABBR0 + pUnit->inventory[eq[e]].masterID, tmpName, 255 );
			out.push_back( { std::string( tmpName ), 0xffc29b00u } );
		}
	}
}

// INFO-IMGUI-SLICE-2: pilot section content shared by BOTH the legacy draw and
// the ImGui panel. Verbatim lift of the former inline reads in
// drawScrollingStuff (same visibility condition, same dead-pilot fallback, same
// ACE rank special-case, same specialty enumeration) so the two paths cannot
// drift on WHAT the pilot section says.
bool InfoWindow::gatherPilotRows( InfoPilotRows& out ) const
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
	EString capHeader;



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
					cLoadString( IDS_COMP_ABBR0 + names[i], tmpName, 255 );
					capHeader.Format( "%ld/%ld  %s", disabledCount[i][0], disabledCount[i][1], tmpName );
					componentFont.render( capHeader, COMPONENTLEFT, curY, SCROLLLEFT - COMPONENTLEFT, height, textColors[j], 0, 0 );
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
	count[0] = pUnit->ecm;
	count[1] = pUnit->probe;
	count[2] = pUnit->jumpJets;
	count[3] = pUnit->isMech() ?  pUnit->sensor : 255;

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
			cLoadString( IDS_COMP_ABBR0 + pUnit->inventory[count[curWeapon]].masterID, tmpName, 255 );

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
	InfoPilotRows pilotRows;
	if ( gatherPilotRows( pilotRows ) )
	{
		if ( icon )
		{
			if ( curY > NAMETOP )
			{
				if ( pilotRows.active )
					icon->renderPilotIcon( PILOTLEFT, curY, PILOTRIGHT, curY + PILOTHEIGHT );
				GUI_RECT tmpRect = { (long)PILOTLEFT, (long)curY, (long)(PILOTRIGHT + 1), (long)(curY + PILOTHEIGHT + 1) };
				drawEmptyRect( tmpRect, SCROLLCOLOR, SCROLLCOLOR  );

				float right = SCROLLLEFT;
				float top = curY + PILOTHEIGHT/2 - height/2;
				float bottom = top + height;
				// draw the name of the pilot
				capHeader = pilotRows.name.c_str();
				componentFont.render( capHeader, PILOTNAMELEFT, top, right - PILOTNAMELEFT, bottom - top, 0xff005392, 0, 0 );

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
				GUI_RECT tmpRect = { (long)(v[0].x - .5), (long)(v[0].y - .5), (long)(v[2].x + 1.5), (long)(v[2].y + 1.5) };
				drawEmptyRect( tmpRect, 0xff002f55, 0xff002f55  );


				unsigned long gosID = mcTextureManager->get_gosTextureHandle( skillInfos[currentSkill].textureHandle );
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



		for ( size_t s = 0; s < pilotRows.specialties.size(); s++ )
		{
			if ( curY > NAMETOP )
			{
				componentFont.render( pilotRows.specialties[s].c_str(), NAMELEFT, curY, NAMERIGHT - NAMELEFT, height, 0xff005392, 0, 0 );
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
		setScrollPos( scrollPos - SCROLLAMOUNT );
		break;

	case SCROLLDOWN:
		setScrollPos( scrollPos + SCROLLAMOUNT );
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
	for ( int i = 0; i < 2; i++ )
		v[i].rhw = .5f;

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

	GUI_RECT outSideRect = { (long)(left - SKILLSKIP + .5f), (long)(yVal - .5f), (long)(right + SKILLSKIP + .5f), (long)(yVal + height + 1.5f)};
	drawRect( outSideRect, 0xff000000 );
	GUI_RECT rect = { (long)(left  + InfoWindow::SKILLSKIP), (long)(yVal + InfoWindow::SKILLSKIP + .5f),
		(long)(left  + InfoWindow::SKILLSKIP + SKILLUNITWIDTH), (long)(yVal + height - InfoWindow::SKILLSKIP + .5f) };

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
	sprintf( buffer, "%ld", skill );
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

