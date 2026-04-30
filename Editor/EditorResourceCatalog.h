/***************************************************************
* FILENAME: EditorResourceCatalog.h
* DESCRIPTION: Declares the Editor-local resource catalog interface.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster attribution header and human-maintenance comments.
****************************************************************/

#ifndef EDITOR_RESOURCE_CATALOG_H
#define EDITOR_RESOURCE_CATALOG_H

/*
    Editor-owned resource catalog.

    New editor resources are keyed by stable namespaced string keys.
    The data/editor/*.fit files are the authoritative Editor resource source.
    Legacy numeric IDs are compatibility metadata only, used to bridge old
    dialogs while the editor is migrated away from mc2res.dll.
    Do not revive mc2res.dll as the Editor string owner.
*/

#include <cstddef>

namespace EditorResourceCatalog
{
    bool Load();

    bool GetStringByKey(const char* key, char* buffer, long bufferLength);
    bool GetStringByLegacyId(long legacyId, char* buffer, long bufferLength);

    bool GetTerrainNameByKey(const char* key, char* buffer, long bufferLength);
    bool GetTerrainNameByLegacyStringId(long legacyStringId, char* buffer, long bufferLength);
    bool GetTerrainNameByIndex(long index, char* buffer, long bufferLength);
    bool GetTerrainFileByKey(const char* key, char* buffer, long bufferLength);
    bool GetTerrainFileByLegacyStringId(long legacyStringId, char* buffer, long bufferLength);
    bool GetTerrainFileByIndex(long index, char* buffer, long bufferLength);

    bool GetBaseTerrainNameByKey(const char* key, char* buffer, long bufferLength);
    bool GetBaseTerrainNameByLegacyStringId(long legacyStringId, char* buffer, long bufferLength);
    bool GetBaseTerrainNameByIndex(long index, char* buffer, long bufferLength);
    bool GetBaseTerrainFileByKey(const char* key, char* buffer, long bufferLength);
    bool GetBaseTerrainFileByLegacyStringId(long legacyStringId, char* buffer, long bufferLength);
    bool GetBaseTerrainFileByIndex(long index, char* buffer, long bufferLength);


    bool GetObjectGroupNameByIndex(long index, char* buffer, long bufferLength);
    bool GetObjectGroupNameByLegacyStringId(long legacyStringId, char* buffer, long bufferLength);

    bool GetObjectNameByKey(const char* key, char* buffer, long bufferLength);
    bool GetObjectNameByLegacyId(long legacyId, char* buffer, long bufferLength);

    bool GetOverlayNameByKey(const char* key, char* buffer, long bufferLength);
    bool GetOverlayNameByLegacyId(long legacyId, char* buffer, long bufferLength);
}

#endif // EDITOR_RESOURCE_CATALOG_H
