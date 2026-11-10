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
// R1 would move (design table, domain Window/input and File, fate R; none of these moved in R0):
//   Window/input: gos_GetMouseInfo, gos_SetMousePosition, gos_TestSetRawMouse, gos_SetScreenMode,
//                 gos_ProbeDrawableSize, gos_GetWindowDisplayIndex, gos_GetNumDisplayModes,
//                 gos_GetDisplayModeByIndex, gos_TerminateApplication, gos_UserExit, gos_AbortTermination,
//                 gos_RunMainLoop, gos_EnableIME / ToggleIME / PositionIME / FinalizeStringIME /
//                 SetIMELevel / SetIMEAppearance / GetIMEAppearance / SetIMEInsertMode
//                 (mouse needs the UI canvas box from RenderCore first; window calls need g_sdl_window ownership)
//   File:         gos_OpenResourceDLL, gos_CloseResourceDLL, gos_GetResourceString, gos_GetResourceData,
//                 gos_GetFile, gos_OpenMemoryMappedFile, gos_CloseMemoryMappedFile, gos_ReadFileInBackground,
//                 gos_CloseBackgroundFile, gos_LoadDataFromRegistry, gos_SaveDataToRegistry,
//                 gos_SaveStringToRegistry, gos_GetUserDataDirectory, gos_CheckValidTGA
//   Frozen network quarantine (N, not an input API): gos_Net*.
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

} // namespace mc2::platform
