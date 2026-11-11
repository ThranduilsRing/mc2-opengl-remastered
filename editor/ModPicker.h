//-------------------------------------------------------------------------------------------------
// ModPicker.h -- editor pre-load mod selector.
//
// Lets the user choose which mod's content is mounted BEFORE loading a mission,
// instead of an always-on env var. Mods shadow base data/* (mods can overwrite
// stock files), so the default is None (stock) to keep stock editing clean.
//
// On selection it sets MC2_ACTIVE_MOD and re-runs InitModSearchPaths (which clears
// + re-indexes), so the next mission load resolves that mod's assets. Self-contained;
// no change to the mclib mod system.
//-------------------------------------------------------------------------------------------------
#ifndef MOD_PICKER_H
#define MOD_PICKER_H

namespace ModPicker
{
#ifdef MC2_IMGUI
	// Draw the combo. Call from renderToolbarImGui(), above the load/generate actions.
	void Draw();
#endif
	// Currently active mod folder name; "" = None (stock).
	const char* ActiveMod();

	// --- non-ImGui API (used by the MFC IDD_NEW_SINGLEPLAYER startup dialog) -------
	// (Re)scan mods/ for mod folders.
	void ScanMods();
	// Discovered mod folders (after ScanMods / first use).
	int  ModCount();
	const char* ModName( int index );
	// Activate a mod ("" or NULL = stock): sets MC2_ACTIVE_MOD + re-indexes. Shared
	// state with Draw(), so the toolbar combo reflects it too.
	void Activate( const char* modId );
}

#endif // MOD_PICKER_H
