// Platform R1 public header: the file / registry / user-data / shared-library seam (design: .claude/cleanup/platform-module-design.md).
// Fixed-width types only: no DWORD, HSTRRES or gos_* types cross this header.
//
// R1 moved (implementation lives in Platform/FileSystem.cpp, gos_* wrappers forward here):
//   shared library primitives under gos_OpenResourceDLL / gos_CloseResourceDLL
//   registry:  gos_LoadDataFromRegistry, gos_SaveDataToRegistry
//   user data: gos_GetUserDataDirectory
#pragma once

#include <cstdint>
#include <span>

namespace mc2::platform {

// Load a shared library (dlopen / SDL_LoadObject); nullptr on failure.
void* OpenSharedLibrary(const char* fileName);
void CloseSharedLibrary(void* handle);
// Address of an exported symbol, nullptr when absent.
void* LoadSharedFunction(void* handle, const char* name);
// Pending loader error text (dlerror semantics: reading it clears it), nullptr when none.
const char* SharedLibraryError();

// gos_LoadDataFromRegistry: *size is set to the byte count written to data (0 when no stored value).
void LoadDataFromRegistry(const char* keyName, void* data, std::uint32_t* size, bool localMachine);
// gos_SaveDataToRegistry: not implemented upstream; kept as the same no-op.
void SaveDataToRegistry(const char* keyName, void* data, std::uint32_t size);
// gos_GetUserDataDirectory: per-user config directory (created when missing) into userDir; false on failure.
bool GetUserDataDirectory(std::span<char> userDir);

} // namespace mc2::platform
