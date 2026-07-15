#ifndef INFOWINDOW_H
#define INFOWINDOW_H
/*************************************************************************************************\
InfoWindow.h			: Interface for the InfoWindow component.
//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/
#include"mclib.h"

#ifndef CONTROLGUI_H
#include"controlgui.h"
#endif

#include<vector>   // INFO-IMGUI-SLICE-1: shared gatherLoadoutRows return type
#include<string>
#include<cstdint>

class Mover;
class ForceGroupIcon;

// INFO-IMGUI-SLICE-1: one loadout entry (weapon / ammo / equipment) shared by
// the legacy procedural draw and the ImGui panel so the two paths cannot drift
// on WHAT is listed. text = display string ("N/M  NAME"), argb = category color.
struct InfoLoadoutRow
{
	std::string  text;
	uint32_t     argb;
};

#ifndef AFONT_H
#include"afont.h"
#endif

#define SCROLLUP	1
#define SCROLLDOWN	2


//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
InfoWindow:
**************************************************************************************************/
class InfoWindow
{
	public:

		InfoWindow();
		~InfoWindow();

		void setUnit( Mover* pNewUnit );
		void update();
		void render();

		// INFO-IMGUI-SLICE-1: gate + ImGui panel. Default OFF -> legacy path.
		static bool imguiInfoEnabled();
		void renderImGui();

	private:

		// INFO-IMGUI-SLICE-1: shared weapon dedup/enumeration used by BOTH
		// drawScrollingStuff (legacy draw) and gatherLoadoutRows (ImGui). Fills
		// the caller's arrays exactly as the legacy inline loop did; returns the
		// number of distinct weapon entries. Reusing this is what guarantees the
		// two info paths list identical hardware.
		int gatherWeaponRows( unsigned char names[60], char disabledCount[60][2],
			long ammo[60], char ranges[60], bool bDraw[4] ) const;

		// INFO-IMGUI-SLICE-1: formatted loadout list (weapons + ammo + equipment)
		// for the ImGui panel. Built on top of gatherWeaponRows so it can never
		// drift from the legacy enumeration.
		void gatherLoadoutRows( std::vector<InfoLoadoutRow>& out ) const;

		float		scrollPos;
		float		scrollLength;
		float		infoLength;

		float		lastYClick;

		// DATA
		Mover*		pUnit;

		static ButtonData	buttonData[2];
		static StaticInfo*	skillInfos;

		unsigned long		backgroundTexture;
		aFont				nameFont;
		aFont				componentFont;

		static ControlButton		buttons[2];

		bool		bUnitChanged;

		friend class ControlGui;

		// HELPER FUNCTIONS
		void drawScrollingStuff();
		void handleClick( int ID );
		void drawDivider( float yVal );
		void drawSkillBar( int skill, float yVal, float height );
		void setScrollPos( int where );
		void drawName( const char* name );

		ForceGroupIcon* icon;

		static  long SCROLLLEFT;
		static  long SCROLLRIGHT;
		static  long SCROLLTOP;
		static  long SCROLLBOTTOM;
		static long	 SCROLLMIN;
		static long SCROLLMAX;
		static long SCROLLBUTTONU;
		static long SCROLLBUTTONV;
		static long SCROLLBUTTONWIDTH;
		static long SCROLLBUTTONHEIGHT;
		static  long SECTIONSKIP;
		static  long NAMELEFT;
		static  long NAMERIGHT;
		static  long NAMETOP;
		static  long NAMEBOTTOM;
		static  long HEALTHLEFT;
		static	long HEALTHRIGHT;
		static long HEALTHTOP;
		static long HEALTHBOTTOM;
		static long DIVIDERCOLOR;
		static long DIVIDERLEFT;
		static long DIVIDERRIGHT;
		static long	PILOTLEFT;
		static long PILOTRIGHT;
		static long PILOTHEIGHT;
		static long MECHLEFT;
		static long MECHRIGHT;
		static long MECHHEIGHT;
		static long MECHBACKLEFT;
		static long MECHBACKRIGHT;
		static long MECHBACKHEIGHT;
		static long SKILLLEFT;
		static long SKILLHEIGHT;
		static long SKILLSKIP;
		static long SKILLRIGHT;
		static long SKILLUNITWIDTH;
		static long NUMBERSKILLBARS;
		static long INFOLEFT;
		static long INFOTOP;
		static long INFOWIDTH;
		static long INFOHEIGHT;
		static long SCROLLCOLOR;
		static long SCROLLBUTTONX;
		static long COMPONENTLEFT;
		static long SCROLLBOXLEFT;
		static long SCROLLBOXRIGHT;
		static long SCROLLBOXTOP;
		static long SCROLLBOXBOTTOM;
		static long PILOTNAMELEFT;

		static GUI_RECT NameRect;

		static void init( FitIniFile& file );

		static InfoWindow* s_instance;



};


//*************************************************************************************************
#endif  // end of file ( InfoWindow.h )
