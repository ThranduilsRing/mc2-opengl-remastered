// Platform R0 public header: window / input / file seam (design: .claude/cleanup/platform-module-design.md).
// Fixed-width types only: no DWORD, HWND, SDL or gos_* types cross this header.
//
// R0 moved (implementation lives in Platform/WindowInput.cpp, gos_* wrappers forward here):
//   keyboard / key status:  GetKey, DescribeKey, KeyboardFlush, GetKeyStatus
//
// R1-file moved to Platform/FileSystem.hpp: shared-library primitives (resource DLL), registry load/save, user data dir.
// R1-file left in place (not clean moves): gos_GetFile (STOP stub in gameos_fileio.cpp, which the unit-test target compiles
// alone), gos_GetResourceString/gos_OpenResourceDLL string-catalog logic (MC2Strings, game layer). Not present in the tree:
// gos_GetResourceData, gos_OpenMemoryMappedFile/Close (only tool stubs), gos_ReadFileInBackground, gos_CloseBackgroundFile,
// gos_SaveStringToRegistry, gos_CheckValidTGA.
//
// R1-window moved here (gos_* wrappers forward; window access goes through the extern g_sdl_window that gos_render owns,
// read in one place, never copied):
//   termination:  gos_TerminateApplication (RequestExit / ExitRequested also back ExitGameOS / gosExitGameOS, the exit-trace shim moved too)
//   display:      gos_GetWindowDisplayIndex, gos_GetNumDisplayModes, gos_GetDisplayModeByIndex, gos_ProbeDrawableSize (the SDL part)
//   IME:          gos_EnableIME / ToggleIME / PositionIME / FinalizeStringIME / SetIMELevel / SetIMEAppearance / GetIMEAppearance
//                 (empty by design, as before)
// R1-window left in place:
//   gos_GetMouseInfo / gos_SetMousePosition / gos_TestSetRawMouse: they normalize by the UI canvas box (gos_Compute43Box,
//     gos_render.h cursor policy), which is RenderCore state; moving them would pull RenderCore into Platform.
//   gos_SetScreenMode: HUD-RES-CLAMP policy plus g_gos_renderer->setScreenMode (RenderCore); not a window primitive.
//   gos_UserExit, gos_AbortTermination, gos_RunMainLoop, gos_SetIMEInsertMode: declared in gameos.hpp, no definition anywhere in
//     the tree (nothing to move).
//   gos_Net*: frozen (N).
// R1-file moved to Platform/FileSystem.hpp (see above); gos_Net* stays frozen.
#pragma once

#include <cstdint>

namespace mc2::platform {

// gos_GetKey: next pressed key as (gosEnum_KeyIndex << 8) | ascii, 0 when none.
std::uint32_t GetKey();
// gos_DescribeKey: displayable name of a GetKey() value.
const char* DescribeKey(std::uint32_t key);
// gos_KeyboardFlush: drop the pending key press.
void KeyboardFlush();
// gos_GetKeyStatus: gosEnum_KeyStatus value for a gosEnum_KeyIndex (mouse buttons are keys 1..5).
int GetKeyStatus(int keyIndex);

// Termination: sets the exit flag and logs the caller (MC2_TRACE_EXIT adds a stack).
void RequestExit(const char* caller);
bool ExitRequested();
void TerminateApplication();

// Display queries on the game window / desktop displays. GetWindowDisplayIndex needs the window to exist.
int GetWindowDisplayIndex();
int GetNumDisplayModes(int displayIndex);
bool GetDisplayModeByIndex(int displayIndex, int modeIndex, int* width, int* height, int* bpp);
// Fresh drawable size in pixels; false when there is no window yet.
bool ProbeDrawableSize(int* w, int* h);

// IME (no-ops). The appearance struct is a GameOS type and crosses as an opaque pointer.
void EnableIME(bool enabledDisabled);
void ToggleIME(bool activeInactive);
void GetIMEAppearance(void* appearance);
void SetIMEAppearance(const void* appearance);
void PositionIME(std::uint32_t x, std::uint32_t y);
void SetIMELevel(std::uint32_t imeLevel);
void FinalizeStringIME();

} // namespace mc2::platform
