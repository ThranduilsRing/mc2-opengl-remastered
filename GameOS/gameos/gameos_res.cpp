#include "gameos.hpp"
#include "strres.h"
#include "MC2Strings.h"
#include "gameos_resource.hpp"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <SDL2/SDL_loadso.h>
#include "mc2fmt.h"

#ifdef PLATFORM_WINDOWS
static void* DL_Open(const char* name) {
	return SDL_LoadObject(name);
}
static void DL_Close(void* handle) {
	SDL_UnloadObject(handle);
}
static void* DL_LoadFunction(void* handle, const char* name) {
	return SDL_LoadFunction(handle, name);
}
// mimic dlerror
static const char* DL_GetError() {
	const char* err = SDL_GetError();
	SDL_ClearError();
	return strlen(err)==0 ? nullptr : err;
}
#else
#include <dlfcn.h>

static void* DL_Open(const char* name) {
    return dlopen(name, RTLD_LAZY);
}
static void DL_Close(void* handle) {
	dlclose(handle);
}
static void* DL_LoadFunction(void* handle, const char* name) {
	return dlsym(handle, name);
}
static const char* DL_GetError() {
	 const char* err = dlerror();
	 return err;
}
#endif


static int MC2_IsMC2ResDLL(char const* fileName)
{
    if (!fileName)
        return 0;

    const char* needle = "mc2res";
    const size_t needleLen = strlen(needle);
    const size_t len = strlen(fileName);

    for (size_t i = 0; i + needleLen <= len; ++i)
    {
        size_t j = 0;
        for (; j < needleLen; ++j)
        {
            if (tolower(static_cast<unsigned char>(fileName[i + j])) != needle[j])
                break;
        }

        if (j == needleLen)
            return 1;
    }

    return 0;
}

HSTRRES __stdcall gos_OpenResourceDLL(char const* FileName, const char** strings, int num)
{
    const char *error;
    void *module;
    get_string_by_id_fptr fptr;

    /*
        mc2res.dll string lookup is now owned by data/defs. Return a valid
        string-resource handle without loading the DLL so gos_GetResourceString
        resolves IDs through MC2Strings instead of the old resource library.

        Stock-install fallback: if the FIT catalog is empty (data/defs missing
        or unreadable), fall through to the legacy DLL load so the engine still
        boots. Per-ID gaps in a loaded catalog still surface as <missing-string:>
        markers; only a fully-empty catalog triggers DLL fallback.
    */
    if (MC2_IsMC2ResDLL(FileName))
    {
        if (MC2Strings::Load())
        {
            gos_StringRes* pstrres = new gos_StringRes();
            pstrres->getStringByIdFptr = nullptr;
            pstrres->module = nullptr;
            pstrres->strings = nullptr;
            pstrres->num_strings = 0;
            return pstrres;
        }

        mc2::fmt::print_err("[MC2Strings] catalog empty, falling back to legacy DLL: {}\n", mc2::fmt::safe(FileName));
    }

	DL_GetError();

    /* Load dynamically loaded library */
    module = DL_Open(FileName);
    gosASSERT(module);
    if (!module) {
        mc2::fmt::print_err("Couldn't open resourse dll: {}\n", mc2::fmt::safe(DL_GetError()));
        return nullptr;
    }

	DL_GetError();    /* Clear any existing error */

    /* Get symbol */
    fptr = (get_string_by_id_fptr)DL_LoadFunction(module, "getStringById");
    if ((error = DL_GetError())) {
        mc2::fmt::print_err("Couldn't load function getStringById in {} (not exported?): {}\n", mc2::fmt::safe(FileName), mc2::fmt::safe(error));
        return nullptr;
    }

    init_string_resources_fptr init_fptr;
    init_fptr = (init_string_resources_fptr)DL_LoadFunction(module, "initStringResources");
    if ((error = DL_GetError())) {
        mc2::fmt::print_err("Couldn't load function initStringResources in {} (not exported?): {}\n", mc2::fmt::safe(FileName), mc2::fmt::safe(error));
        return nullptr;
    }
    init_fptr();

    gos_StringRes* pstrres = new gos_StringRes();
    pstrres->getStringByIdFptr = fptr;
    pstrres->module = module;

    /* deprecated */
    pstrres->strings = nullptr;
    pstrres->num_strings = 0;

    return pstrres;
}
void __stdcall gos_CloseResourceDLL(HSTRRES handle)
{
    gos_StringRes* pstrres = static_cast<gos_StringRes*>(handle);
    gosASSERT(pstrres);

    if (!pstrres)
        return;

    if (!pstrres->module)
    {
        delete pstrres;
        return;
    }

    free_string_resources_fptr free_fptr;
    DL_GetError();

    const char *error;
    free_fptr = (free_string_resources_fptr)DL_LoadFunction(pstrres->module, "freeStringResources");
    if ((error = DL_GetError())) {
        mc2::fmt::print_err("Couldn't find hello: {}\n", mc2::fmt::safe(error));
    } else {
        free_fptr();
    }

    DL_Close(pstrres->module);
    delete pstrres;
}

namespace {

// The text the provider holds for `id`, or nothing when it has none. Silent: the callers decide what a missing id costs.
std::optional<std::string_view> lookupResourceString(HSTRRES handle, DWORD id)
{
    gos_StringRes* pstrres = handle;
    if (!pstrres)
        return std::nullopt;

    /*
        mc2res.dll handles are represented by module == NULL. Those IDs must
        resolve through data/defs. Do not silently fall back to mc2res.dll.
    */
    if (!pstrres->module)
    {
        const char* fitText = MC2Strings::GetByLegacyId(static_cast<long>(id));
        if (fitText && fitText[0])
            return std::string_view(fitText);
        return std::nullopt;
    }

    if (!pstrres->getStringByIdFptr)
        return std::nullopt;

    const char* text = pstrres->getStringByIdFptr(id);
    if (!text)
        return std::nullopt;
    return std::string_view(text);
}

}  // namespace

const char* __stdcall gos_GetResourceString(HSTRRES handle, DWORD id)
{
    gosASSERT(handle);

    if (const std::optional<std::string_view> text = lookupResourceString(handle, id))
        return text->data();   // a view over the provider's NUL-terminated string

    gos_StringRes* pstrres = handle;
    if (!pstrres)
        return MC2Strings::MissingResourceId(id);
    if (!pstrres->module)
        return MC2Strings::MissingLegacyId(static_cast<long>(id));
    if (pstrres->getStringByIdFptr)
        mc2::fmt::print_err("Requested string id: {:d} not found, return missing marker\n", mc2::fmt::as_i32(id));
    return MC2Strings::MissingResourceId(id);
}

std::optional<std::size_t> gos_ResourceStringLength(HSTRRES handle, DWORD id)
{
    if (const std::optional<std::string_view> text = lookupResourceString(handle, id))
        return text->size();
    return std::nullopt;
}
