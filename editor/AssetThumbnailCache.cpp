/***************************************************************
* FILENAME: AssetThumbnailCache.cpp
* DESCRIPTION: See AssetThumbnailCache.h.
* DATE: 2026-06-10
****************************************************************/

#include "stdafx.h"

#include "AssetThumbnailCache.h"

#include "EditorObjectMgr.h"
#include "paths.h"           // artPath (extern char artPath[])

#include "EditorTexture.h"

#include <unordered_map>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Internal cache
// ---------------------------------------------------------------------------
static std::unordered_map<int, RenderCore::ServiceTextureRef> s_cache;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static RenderCore::ServiceTextureRef loadThumbnail(int objectID)
{
    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    if (!mgr)
        return {};

    // ID validity guard: getTGAFileName uses getGroup/getIndexInGroup on the
    // raw packed ID; protect against obviously-bad values.
    const char* tgaBase = mgr->getTGAFileName(objectID);
    if (!tgaBase || !tgaBase[0])
        return {};
    if (std::strcmp(tgaBase, "NONE") == 0 || std::strcmp(tgaBase, "0") == 0)
        return {};

    // Build full path: artPath + tgaBase + ".tga"
    // artPath is "data\art\" (or similar, read from ini at startup).
    char path[512];
    std::snprintf(path, sizeof(path), "%s%s.tga", artPath, tgaBase);

    return loadEditorTexture(path);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
AssetThumbnailCache::TexHandle AssetThumbnailCache::get(int objectID)
{
    auto it = s_cache.find(objectID);
    if (it == s_cache.end()) it = s_cache.emplace(objectID, loadThumbnail(objectID)).first;
    return RenderCore::uiTextureId(it->second.get());
}

void AssetThumbnailCache::clear()
{
    s_cache.clear();
}
