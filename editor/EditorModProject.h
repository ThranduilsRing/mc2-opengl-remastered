//-------------------------------------------------------------------------------------------------
// EditorModProject.h -- "Mod Project" concept for the MC2 Mission Editor.
//
// A Mod Project binds the editor session to a `mods/<id>/` folder so that:
//   * File save dialogs default to <root>\data\missions\ (only when a project is active;
//     zero behavior change otherwise),
//   * the mod's content is mounted for asset loading (ModPicker::Activate(<id>)),
//   * Playtest uses the project id for MC2_ACTIVE_MOD (path-derived detection stays as
//     the fallback when no project is open).
//
// The project is described by `mods/<id>/mod.json` (minimal schema: {id, name, version,
// dependencies[]}).  Tool-private editor state lives in `mods/<id>/.modproject/editor.json`
// (the engine scans data/ only and skips .modproject/).
//
// JSON is hand-rolled minimal (matching mclib/file.cpp's ReadModJson style) -- no full
// parser.  All Win32 / std::string; the core API is ImGui-free so it links regardless of
// MC2_IMGUI.  The ImGui panel + folder picker is in Draw(), guarded by MC2_IMGUI.
//-------------------------------------------------------------------------------------------------
#ifndef EDITOR_MOD_PROJECT_H
#define EDITOR_MOD_PROJECT_H

#include <string>
#include <vector>

namespace EditorModProject
{
	// --- state accessors ------------------------------------------------------------------
	// True when a project is currently open/bound.
	bool        IsActive();
	// `...\mods\<id>` absolute dir of the active project ("" when inactive).
	const char* RootPath();
	// Active project id ("" when inactive).
	const char* Id();
	// Active project display name (falls back to id when mod.json has no name).
	const char* Name();
	// Active project version string ("" when unknown).
	const char* Version();

	// Default save directory for File save dialogs: `<root>\data\missions` when a project is
	// active, else NULL (callers keep their existing default -- zero behavior change).  The
	// returned pointer is valid until the next Open/CreateNew/Close.
	const char* SaveDirOverride();

	// --- lifecycle ------------------------------------------------------------------------
	// Open the project rooted at `folder` (a `mods/<id>` dir).  Reads/validates mod.json.
	// If mod.json is missing and offerCreate is true, scaffolds a minimal one in place.
	// On success: binds state, mounts the mod (ModPicker::Activate), persists the path.
	// Returns true on success; on failure fills the status line and leaves state unchanged.
	bool Open(const char* folder, bool offerCreate = true);

	// Scaffold a NEW project: `<parentDir>\<id>\{mod.json, data\missions\, .modproject\editor.json}`
	// then Open() it.  Refuses to clobber an existing non-empty mod.json.  Returns true on
	// success.
	bool CreateNew(const char* parentDir, const char* id);

	// Import a stock (or any) mission into the active project's data\missions tree so it can
	// be edited without touching base data\missions (deploy hygiene).  Given a `<stem>.pak`
	// path, copies <stem>.pak + <stem>.fit (+ <stem>_purchase.fit when present) into
	// <root>\data\missions\.  When a destination file already exists and overwriteOk is false,
	// fills the status line and returns false (caller re-invokes with overwriteOk=true after
	// confirming).  On success, `outImportedPak` (optional) receives the absolute path of the
	// copied .pak so the caller can open it.  Requires an active project.
	bool ImportMission(const char* srcPakPath, bool overwriteOk, std::string* outImportedPak);

	// Close the active project (unbinds state, leaves files untouched).  Does NOT change the
	// mounted mod (switching back to stock mid-session is the user's call via the mod picker).
	void Close();

	// One-line status for the UI (last Open/CreateNew/Close result).
	const char* StatusLine();

	// --- persistence ----------------------------------------------------------------------
	// Attempt to re-open the last project recorded in the editor's pointer file (if any).
	// Safe no-op when there is no record or the recorded path no longer validates.  Call once
	// at editor startup.  Never opens dialogs.
	void ReopenLastIfAny();

	// --- ImGui panel (folder picker + New popup + status) --------------------------------
#ifdef MC2_IMGUI
	// Toggle the Mod Project panel window.
	void Toggle();
	// Draw the panel each frame from renderToolbarImGui() (no-op when closed).
	void Draw();
#endif
}

#endif // EDITOR_MOD_PROJECT_H
