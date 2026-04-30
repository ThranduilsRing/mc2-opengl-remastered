//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

// Editores.cpp : Defines the entry point and exported resource helpers for
// the editor resource DLL.
//
#include <windows.h>

static HINSTANCE g_hInstance = NULL;

extern "C" __declspec(dllexport) void initStringResources()
{
}

extern "C" __declspec(dllexport) void freeStringResources()
{
}

extern "C" __declspec(dllexport) const char* getStringById(unsigned int id)
{
    static char buffer[4096];

    buffer[0] = '\0';

    if (g_hInstance)
    {
        int loaded = LoadStringA(g_hInstance, id, buffer, sizeof(buffer));
        if (loaded > 0)
            return buffer;
    }

    return NULL;
}

BOOL APIENTRY DllMain(HINSTANCE hModule,
                      DWORD     ul_reason_for_call,
                      LPVOID    lpReserved)
{
    lpReserved;

    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
        g_hInstance = hModule;

    return TRUE;
}
