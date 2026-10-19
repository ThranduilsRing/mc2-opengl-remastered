#ifndef ASSET_THUMBNAIL_CACHE_H
#define ASSET_THUMBNAIL_CACHE_H
/***************************************************************
* FILENAME: AssetThumbnailCache.h
* DESCRIPTION: Per-object-ID thumbnail cache for the Asset Browser.
*   Loads artPath + tgaName + ".tga" through the shared CPU decoder and owns
*   a session service reference. Returns a generation-checked ImTextureID,
*   or 0 for missing art. Missing entries are cached until catalog reload.
*   Call clear() when the catalog is reloaded (e.g. new map).
* DATE: 2026-06-10
****************************************************************/

#include <cstdint>

class AssetThumbnailCache
{
public:
    // Opaque handle returned to callers.  0 means "no thumbnail".
    using TexHandle = uint64_t;

    // Look up (or load) the thumbnail for this object catalog ID.
    // Returns 0 if the TGA is NONE/missing.
    static TexHandle get(int objectID);

    // Release all service references and clear the table.  Call on catalog reload.
    static void clear();
};

#endif // ASSET_THUMBNAIL_CACHE_H
