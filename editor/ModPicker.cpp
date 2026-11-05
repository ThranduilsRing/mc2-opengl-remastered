//-------------------------------------------------------------------------------------------------
// ModPicker.cpp -- see ModPicker.h.
//-------------------------------------------------------------------------------------------------
#include "stdafx.h"
#include "ModPicker.h"
#include "Gates/gate_cold.h"
#include "Gates/gate_name.h"

#ifdef MC2_IMGUI
#include "imgui.h"
#endif

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// mclib/file.h -- reads MC2_ACTIVE_MOD, clears + re-indexes mods/<mod>/data/* (+ deps).
extern void InitModSearchPaths(const char* modsRoot);

namespace
{
	std::vector<std::string> s_mods;       // discovered mod folder names
	bool  s_scanned  = false;
	int   s_selected = 0;                  // 0 = None (stock); else s_mods[idx-1]
	char  s_active[128] = "";              // active mod folder name ("" = stock)

}

namespace ModPicker
{

void ScanMods()
{
	s_mods.clear();
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA( "mods\\*", &fd );
	if ( h != INVALID_HANDLE_VALUE )
	{
		do
		{
			if ( ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) &&
			     fd.cFileName[0] != '.' )
				s_mods.push_back( fd.cFileName );
		}
		while ( FindNextFileA( h, &fd ) );
		FindClose( h );
	}
	s_scanned = true;
}

int ModCount()
{
	if ( !s_scanned ) ScanMods();
	return (int)s_mods.size();
}

const char* ModName( int index )
{
	if ( !s_scanned ) ScanMods();
	if ( index < 0 || index >= (int)s_mods.size() ) return "";
	return s_mods[index].c_str();
}

// Activate a mod ("" or NULL = stock). Sets MC2_ACTIVE_MOD then re-indexes via the
// same path the game/editor startup uses. InitModSearchPaths clears g_modIndex first,
// so switching mods (or back to stock) is clean. Keeps s_selected in sync so the
// ImGui toolbar combo reflects a change made from the MFC startup dialog.
void Activate( const char* modId )
{
	const char* id = modId ? modId : "";
	_putenv_s( mc2::gate::name<"MC2_ACTIVE_MOD">(), id );
	mc2::gate::invalidate( mc2::gate::name<"MC2_ACTIVE_MOD">() );
	InitModSearchPaths( "./mods/" );
	strncpy( s_active, id, sizeof( s_active ) - 1 );
	s_active[sizeof( s_active ) - 1] = '\0';

	s_selected = 0;
	for ( int i = 0; i < (int)s_mods.size(); ++i )
		if ( s_mods[i] == id ) { s_selected = i + 1; break; }
}

const char* ActiveMod() { return s_active; }

#ifdef MC2_IMGUI
void Draw()
{
	if ( !s_scanned )
		ScanMods();

	ImGui::TextUnformatted( "Mod content" );
	ImGui::SameLine();
	const char* cur = ( s_selected == 0 || s_selected > (int)s_mods.size() )
	                ? "None (stock)"
	                : s_mods[s_selected - 1].c_str();
	ImGui::SetNextItemWidth( 160.0f );
	if ( ImGui::BeginCombo( "##modpicker", cur ) )
	{
		if ( ImGui::Selectable( "None (stock)", s_selected == 0 ) )
			Activate( "" );
		for ( int i = 0; i < (int)s_mods.size(); ++i )
		{
			bool sel = ( s_selected == i + 1 );
			if ( ImGui::Selectable( s_mods[i].c_str(), sel ) )
				Activate( s_mods[i].c_str() );
		}
		ImGui::EndCombo();
	}
	if ( ImGui::IsItemHovered() )
		ImGui::SetTooltip(
			"Mount a mod's data/ over base data/ BEFORE loading a mission.\n"
			"Mods can shadow stock files -- pick None for stock missions.\n"
			"Dependencies (mod.json) are pulled in automatically.\n"
			"Change this, THEN load the mission to apply." );

	ImGui::SameLine();
	if ( ImGui::SmallButton( "Rescan" ) )
		ScanMods();
}
#endif

}
