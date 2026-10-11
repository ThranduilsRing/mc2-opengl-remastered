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

// INFO-IMGUI-SLICE-2: pilot section content shared by the legacy procedural
// draw and the ImGui panel (same never-duplicate-enumeration rule as
// InfoLoadoutRow / gatherWeaponRows). Strings arrive fully resolved (dead-pilot
// fallback, rank string incl. the non-contiguous ACE id) so neither path
// re-derives them.
struct InfoPilotRows
{
	bool        active;        // pWarrior->active(): draw the pilot photo
	std::string name;          // pilot name, or IDS_NOPILOT text when !active
	std::string rankText;      // IDS_GREEN+rank, or IDS_ACE when rank==4
	int         rank;          // raw rank (indexes the medallion textures)
	int         gunnery;       // MWS_GUNNERY skill value
	int         piloting;      // MWS_PILOTING skill value
	std::vector<std::string> specialties;   // IDS_SPECIALTY+i for set skills
};

#ifndef AFONT_H
#include"afont.h"
#endif

inline constexpr int SCROLLUP = 1;
inline constexpr int SCROLLDOWN = 2;


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

		// NOTE (MERGE-WAVE-1): nifty's IN-MISSION-HUD-IMGUI-SCOPE-1 slice D
		// (MC2_HUD_IMGUI_INFOWINDOW) info panel was superseded by the
		// ui-phase1 INFO-IMGUI slices below (same feature, pixel-verified);
		// slice-D impl removed at merge. infowindowmodel.h + its unit test
		// remain (standalone pure kernel).
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

		// INFO-IMGUI-SLICE-2: pilot section content (name / rank / skills /
		// specialties) used by BOTH drawScrollingStuff and the ImGui panel.
		// Returns false when the section is hidden (enemy / non-mech /
		// disabled / destroyed / no pilot) — legacy then skips it exactly as
		// the old inline condition did.
		bool gatherPilotRows( InfoPilotRows& out ) const;

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
