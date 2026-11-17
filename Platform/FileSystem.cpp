// Platform R1 (design: .claude/cleanup/platform-module-design.md): the shared-library primitives, registry stubs and
// user data directory moved out of GameOS/gameos/gameos_res.cpp and gameos.cpp unchanged. The bodies below are the
// former gos_OpenResourceDLL/gos_CloseResourceDLL loader helpers (DL_*), gos_LoadDataFromRegistry,
// gos_SaveDataToRegistry and gos_GetUserDataDirectory; the gos_* entry points forward here. No behaviour change.
#include "FileSystem.hpp"

#include "gameos.hpp"
#include "mc2fmt.h"
#include "utils/mc2log.h"

#include <memory>
#include <stdlib.h> // getenv
#include <string.h>
#include <SDL2/SDL_loadso.h>

// fstat
#include <sys/types.h>
#include <sys/stat.h>
#ifndef PLATFORM_WINDOWS
#include <dlfcn.h>
#include <unistd.h>
#else
#include <Shlobj.h> // SHGetKnownFolderPath
#include <Objbase.h> // CoTaskMemFree
#include <direct.h>
#define S_ISDIR(x) ((_S_IFDIR & (x))!=0)
#endif

namespace mc2::platform {
namespace {

static const char* g_user_reg_file = ".registry";
static const char* g_user_base_config_dir = ".config";
static const char* g_user_config_dir = ".mechcommander2";

} // namespace

#ifdef PLATFORM_WINDOWS
void* OpenSharedLibrary(const char* name) {
	return SDL_LoadObject(name);
}
void CloseSharedLibrary(void* handle) {
	SDL_UnloadObject(handle);
}
void* LoadSharedFunction(void* handle, const char* name) {
	return SDL_LoadFunction(handle, name);
}
// mimic dlerror
const char* SharedLibraryError() {
	const char* err = SDL_GetError();
	SDL_ClearError();
	return strlen(err)==0 ? nullptr : err;
}
#else
void* OpenSharedLibrary(const char* name) {
    return dlopen(name, RTLD_LAZY);
}
void CloseSharedLibrary(void* handle) {
	dlclose(handle);
}
void* LoadSharedFunction(void* handle, const char* name) {
	return dlsym(handle, name);
}
const char* SharedLibraryError() {
	 const char* err = dlerror();
	 return err;
}
#endif

// TODO: use inifile.h functionality for this
void LoadDataFromRegistry(const char* keyName, void* pData, std::uint32_t* szData, bool HKLM)
{
    // this code can be useful to get registry file location

    const char* home_dir = getenv("HOME");
    if(home_dir == nullptr)
    {
        MC2_LOG(Io, Debug, "gos_Registry: home directory is not set");
        *szData = 0;
        return;
    }

    const int cfg_dir_size = 1024;
    char cfg_dir[cfg_dir_size] = {0};

    const char* config_dir = getenv("XDG_CONFIG_HOME");
    if(config_dir == nullptr) {
        mc2::fmt::to_buf(cfg_dir, "{}/{}", mc2::fmt::safe(home_dir), mc2::fmt::safe(g_user_base_config_dir));
    } else {
        strncpy(cfg_dir, config_dir, 1023);
        cfg_dir[cfg_dir_size-1] = '\0';
    }

    struct stat st;
    if(-1 == stat(cfg_dir, &st)) {
        MC2_LOG(Core, Debug, "stat: {}", mc2::fmt::safe(strerror(errno)));
        *szData = 0;
        return;
    }
    if(!S_ISDIR(st.st_mode)) {
        MC2_LOG(Core, Debug, "gos_LoadDataFromRegistry: {} is not directory", mc2::fmt::safe(cfg_dir));
        *szData = 0;
        return;
    }

    // 1 + 1 =  for \0 and /
    const size_t confDirBytes = strlen(cfg_dir) + strlen(g_user_config_dir) + 1 + 1;
    auto mc2ConfDir = std::make_unique<char[]>(confDirBytes);
    char* mc2_conf_dir = mc2ConfDir.get();
    mc2::fmt::to_buf_n(mc2_conf_dir, confDirBytes, "{}/{}", cfg_dir, g_user_config_dir);

    // if directory does not exist create it
#ifndef PLATFORM_WINDOWS
    int permissions = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
#endif
	if(-1 == stat(mc2_conf_dir, &st)) {
#ifdef PLATFORM_WINDOWS
        if(-1 == _mkdir(mc2_conf_dir)) {
#else
        if(-1 == mkdir(mc2_conf_dir, permissions)) {
#endif
            MC2_LOG(Core, Debug, "stat: {}", mc2::fmt::safe(strerror(errno)));
            *szData = 0;
            return;
        }
    }

    const size_t regFileBytes = strlen(mc2_conf_dir) + 1 + strlen(g_user_reg_file) + 1;
    auto regFile = std::make_unique<char[]>(regFileBytes);
    char* reg_file = regFile.get();
    mc2::fmt::to_buf_n(reg_file, regFileBytes, "{}/{}", mc2_conf_dir, g_user_reg_file);

    // check if reg file exists
    if(-1 == stat(reg_file, &st)) {
        *szData = 0;
        return;
    } else {
        *szData = 0;
        // open file and read value
    }
}

void SaveDataToRegistry(const char* keyName, void* pData, std::uint32_t szData)
{
    // TODO:
}

bool GetUserDataDirectory(std::span<char> user_dir)
{
    if (user_dir.empty())
        return false;
    const size_t len = user_dir.size();
	const int cfg_dir_size = 1024;
    char cfg_dir[cfg_dir_size] = {0};
    struct stat st;

#ifdef PLATFORM_WINDOWS
	PWSTR homeDir = nullptr;
	SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &homeDir);
	wcstombs(cfg_dir, homeDir, cfg_dir_size-1);
	cfg_dir[cfg_dir_size-1] = '\0';
	CoTaskMemFree(homeDir);
#else
    const char* home_dir = getenv("HOME");
    if(home_dir == nullptr)
    {
        MC2_LOG(Io, Debug, "gos_Registry: home directory is not set");
        return false;
    }

    const char* config_dir = getenv("XDG_CONFIG_HOME");
    if(config_dir != nullptr) {
        strncpy(cfg_dir, config_dir, cfg_dir_size-1);
        cfg_dir[cfg_dir_size-1] = '\0';

        if(-1 == stat(cfg_dir, &st)) {
            MC2_LOG(Core, Debug, "stat: {}", mc2::fmt::safe(strerror(errno)));
            return false;
        }

        if(!S_ISDIR(st.st_mode)) {
            MC2_LOG(Io, Debug, "gos_GetUserDataDirectory: {} is not directory", mc2::fmt::safe(cfg_dir));
            return false;
        }
    } else {
        strncpy(cfg_dir, home_dir, cfg_dir_size-1);
        cfg_dir[cfg_dir_size-1] = '\0';
    }
#endif

    // 1 + 1 =  for \0 and /
    const size_t confDirBytes = strlen(cfg_dir) + strlen(g_user_config_dir) + 1 + 1;
    auto mc2_conf_dirOwner = std::make_unique_for_overwrite<char[]>(confDirBytes);
    char* mc2_conf_dir = mc2_conf_dirOwner.get();
    mc2::fmt::to_buf_n(mc2_conf_dir, confDirBytes, "{}/{}", cfg_dir, g_user_config_dir);

    // if directory does not exist create it
#ifndef PLATFORM_WINDOWS
	int permissions = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH | S_IXUSR | S_IXGRP;
#endif

    if(-1 == stat(mc2_conf_dir, &st)) {
#ifdef PLATFORM_WINDOWS
        if(-1 == _mkdir(mc2_conf_dir)) {
#else
        if(-1 == mkdir(mc2_conf_dir, permissions)) {
#endif
            MC2_LOG(Core, Debug, "stat: {}", mc2::fmt::safe(strerror(errno)));
            mc2_conf_dirOwner.reset();
            return false;
        }
    }

    size_t conf_len = strlen(mc2_conf_dir);
    conf_len = len - 1 < conf_len ? len - 1 : conf_len;

    strncpy(user_dir.data(), mc2_conf_dir, conf_len);
    user_dir[conf_len] = '\0';
    mc2_conf_dirOwner.reset();
    return true;
}

} // namespace mc2::platform
