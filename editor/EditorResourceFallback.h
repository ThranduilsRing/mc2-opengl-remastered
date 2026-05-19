/***************************************************************
* FILENAME: EditorResourceFallback.h
* DESCRIPTION: Provides Editor-local fallback resource strings for remastered startup.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES:
*   - Updated Editor Remaster attribution header and human-maintenance comments.
*   - Wired EditorSafeLoadString through MC2Strings (engine FIT bridge) and
*     EditorResourceCatalog (editor FIT catalog) before hardcoded fallback.
*   - Removed accidental self-include.
****************************************************************/

#ifndef EDITOR_RESOURCE_FALLBACK_H
#define EDITOR_RESOURCE_FALLBACK_H

/*
    Editor string resolution order:

    1. MC2Strings::GetByLegacyId() — engine FIT bridge (data/defs/text/en_us/).
       Covers the ~4000 legacy mc2res IDs plus clean localization keys.
    2. EditorResourceCatalog::GetStringByLegacyId() — editor FIT catalog
       (data/defs/catalogs/editor/, data/defs/text/en_us/editor/).
       Covers editor-owned IDS_* strings and terrain/object names.
    3. EditorFallbackResourceString() — hardcoded last-resort table for
       critical editor UI strings that must work even when data/ is absent.
    4. snprintf marker — "mc2res.dll:<id> Not defined" so dialogs that
       check for missing entries still detect gaps.

    Do not revive mc2res.dll as the string owner.
*/

#include <cstdio>
#include <cstring>
#include "MC2Strings.h"
#include "EditorResourceCatalog.h"

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

    /*
        Layer 1: Engine FIT bridge. Handles all legacy mc2res IDs that
        were migrated to data/defs/text/en_us/ in the Phase 1 patch.
    */
    if (MC2Strings::IsLoaded())
    {
        const char* fitText = MC2Strings::GetByLegacyId(resourceID);
        if (fitText && fitText[0] && fitText[0] != '<')
        {
            std::strncpy(buffer, fitText, static_cast<size_t>(bufferLength) - 1);
            buffer[bufferLength - 1] = 0;
            return;
        }
    }

    /*
        Layer 2: Editor FIT catalog. Handles editor-specific IDS_*
        strings from data/defs/text/en_us/editor/ and
        data/defs/catalogs/editor/.
    */
    if (EditorResourceCatalog::GetStringByLegacyId(resourceID, buffer, bufferLength))
        return;

    /*
        Layer 3: Hardcoded fallback for critical editor strings that
        must survive even without data/defs/ deployed.
    */
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
