/***************************************************************
* FILENAME: EditorResourceFallback.h
* DESCRIPTION: Provides Editor-local fallback resource strings for remastered startup.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster attribution header and human-maintenance comments.
****************************************************************/

#ifndef EDITOR_RESOURCE_FALLBACK_H
#define EDITOR_RESOURCE_FALLBACK_H

/*
    Editor migration compatibility layer.

    The original editor calls EditorSafeLoadString(), which routes through the legacy
    GameOS/Win32 resource string system. In the Remastered runtime that table
    is not initialized during editor startup, and the old path eventually
    reaches legacy resource getter with a null resource table.

    Keep the workaround local to the editor graft:
      - do not patch Remastered GameOS
      - do not reintroduce the legacy resource loader
      - return stable fallback strings for editor UI, menus, terrain names,
        error text, and pilot/object labels
      - use EditorResourceCatalog for named catalog data backed by data/editor/*.fit
      - never route Editor string loading back through mc2res.dll as the owner

    This is intentionally boring. The goal is to keep the editor alive first;
    proper localized resource restoration can be handled later as a separate
    data/resource-loader task.
*/

#include <cstdio>
#include <cstring>
#include "EditorResourceFallback.h" // Editor-local replacement for legacy resource-string loading.

#ifndef IDS_UNNAMDE_FOREST
#define IDS_UNNAMDE_FOREST 40071
#endif

#ifndef ID_TERRAINS_INVALID
#define ID_TERRAINS_INVALID 59999
#endif

#ifndef ID_TERRAINS_BLUEWATER
#define ID_TERRAINS_BLUEWATER 60000
#endif

static inline const char* EditorFallbackResourceString(long resourceID)
{
    switch (resourceID)
    {
        case IDS_UNNAMDE_FOREST: return "Forest %d";

        /*
            Common editor strings. Some of these IDs come from the original
            shared MC2 resource DLL rather than Editor/resource.h, so keep the
            fallback text generic when the exact symbol is not visible here.
        */
        case 40090: return "Could not open operation file: %s";
        case 40092: return "Could not open purchase file: %s";
        case 40073: return "No buildings selected.";
        case 40074: return "More than one building selected.";

        default:
            break;
    }

    /*
        TerrainColorMap and TerrainTextures often pass terrain string IDs.
        The original strings are not required for editor startup correctness,
        but readable terrain names are better than blank UI.
    */
    if (resourceID >= ID_TERRAINS_BLUEWATER && resourceID <= 60064)
    {
        static const char* terrainNames[] =
        {
            "Blue Water",
            "Green Water",
            "Mud",
            "Dirty",
            "Forest Floor 1",
            "Forest Floor 2",
            "Forest Floor 3",
            "Mud Flats",
            "Forest Floor 4",
            "Muddy Shore",
            "Cement",
            "Sandy Dirt",
            "Slimy",
            "Cement 2",
            "Cement 3",
            "Cement 4",
            "Cement 5",
            "Cement 6",
            "Cement 7",
            "Cement 8",
            "Sand 1",
            "Sand 2",
            "Mud 1",
            "Mud 2",
            "Grass Light 1",
            "Grass Light 2",
            "Grass Light 3",
            "Cliff 1",
            "Cliff 2",
            "Grass 2",
            "Grass 3"
        };

        const long terrainIndex = resourceID - ID_TERRAINS_BLUEWATER;
        if (terrainIndex >= 0 &&
            terrainIndex < static_cast<long>(sizeof(terrainNames) / sizeof(terrainNames[0])))
        {
            return terrainNames[terrainIndex];
        }
    }

    return NULL;
}

static inline void EditorSafeLoadString(long resourceID, char* buffer, long bufferLength, void* /*resourceHandle*/ = 0)
{
    if (!buffer || bufferLength <= 0)
        return;

    const char* fallback = EditorFallbackResourceString(resourceID);

    if (fallback)
    {
        std::strncpy(buffer, fallback, static_cast<size_t>(bufferLength) - 1);
        buffer[bufferLength - 1] = 0;
        return;
    }

    /*
        Preserve the old "missing resource" shape used by several editor
        dialogs to detect absent entries, while avoiding the crashing
        legacy resource getter path.
    */
    std::snprintf(buffer, static_cast<size_t>(bufferLength), "mc2res.dll:%ld Not defined", resourceID);
    buffer[bufferLength - 1] = 0;
}

#endif // EDITOR_RESOURCE_FALLBACK_H
