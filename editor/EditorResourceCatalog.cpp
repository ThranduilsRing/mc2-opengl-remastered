/***************************************************************
* FILENAME: EditorResourceCatalog.cpp
* DESCRIPTION: Implements the Editor-local resource catalog backed by data/editor FIT files.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 05/16/2026
* MODIFICATION: by Methuselas
* CHANGES: Remove temporary catalog trace calls after FIT loading performance validation.
****************************************************************/

#include "stdafx.h"
#include "EditorResourceCatalog.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    struct NamedEntry
    {
        std::string key;
        std::string value;
        std::string file;
        long legacyId;
        long legacyStringId;
        long index;

        NamedEntry()
            : legacyId(-1), legacyStringId(-1), index(-1)
        {
        }
    };

    bool g_loaded = false;

    std::map<std::string, NamedEntry> g_stringsByKey;
    std::map<long, std::string> g_stringKeyByLegacyId;

    std::map<std::string, NamedEntry> g_terrainByKey;
    std::map<long, std::string> g_terrainKeyByLegacyStringId;
    std::map<long, std::string> g_terrainKeyByIndex;

    std::map<std::string, NamedEntry> g_baseTerrainByKey;
    std::map<long, std::string> g_baseTerrainKeyByLegacyStringId;
    std::map<long, std::string> g_baseTerrainKeyByIndex;

    std::map<std::string, NamedEntry> g_objectGroupByKey;
    std::map<long, std::string> g_objectGroupKeyByIndex;
    std::map<long, std::string> g_objectGroupKeyByLegacyStringId;

    std::map<std::string, NamedEntry> g_objectsByKey;
    std::map<long, std::string> g_objectKeyByLegacyId;

    std::map<std::string, NamedEntry> g_overlaysByKey;
    std::map<long, std::string> g_overlayKeyByLegacyId;

    static std::string trim(const std::string& s)
    {
        std::string::size_type b = 0;
        while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
            ++b;

        std::string::size_type e = s.size();
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
            --e;

        return s.substr(b, e - b);
    }

    static std::string unquote(const std::string& s)
    {
        std::string t = trim(s);
        if (t.size() >= 2)
        {
            const char first = t[0];
            const char last = t[t.size() - 1];
            if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
                return t.substr(1, t.size() - 2);
        }
        return t;
    }

    static long toLong(const std::string& s, long fallback = -1)
    {
        char* end = 0;
        const long v = std::strtol(trim(s).c_str(), &end, 10);
        if (!end || *end != 0)
            return fallback;
        return v;
    }

    static bool copyOut(const std::string& value, char* buffer, long bufferLength)
    {
        if (!buffer || bufferLength <= 0 || value.empty())
            return false;

        std::strncpy(buffer, value.c_str(), static_cast<size_t>(bufferLength) - 1);
        buffer[bufferLength - 1] = 0;
        return true;
    }

    static const NamedEntry* lookupEntryByKey(const std::map<std::string, NamedEntry>& table,
                                              const char* key)
    {
        if (!key || !key[0])
            return 0;

        const std::map<std::string, NamedEntry>::const_iterator it = table.find(key);
        if (it == table.end())
            return 0;

        return &it->second;
    }

    static const NamedEntry* lookupEntryByMappedId(const std::map<std::string, NamedEntry>& table,
                                                   const std::map<long, std::string>& keyById,
                                                   long id)
    {
        const std::map<long, std::string>::const_iterator keyIt = keyById.find(id);
        if (keyIt == keyById.end())
            return 0;

        return lookupEntryByKey(table, keyIt->second.c_str());
    }

    static bool lookupNameByKey(const std::map<std::string, NamedEntry>& table,
                                const char* key,
                                char* buffer,
                                long bufferLength)
    {
        const NamedEntry* entry = lookupEntryByKey(table, key);
        return entry ? copyOut(entry->value, buffer, bufferLength) : false;
    }

    static bool lookupNameByMappedId(const std::map<std::string, NamedEntry>& table,
                                     const std::map<long, std::string>& keyById,
                                     long id,
                                     char* buffer,
                                     long bufferLength)
    {
        const NamedEntry* entry = lookupEntryByMappedId(table, keyById, id);
        return entry ? copyOut(entry->value, buffer, bufferLength) : false;
    }

    static bool lookupFileByKey(const std::map<std::string, NamedEntry>& table,
                                const char* key,
                                char* buffer,
                                long bufferLength)
    {
        const NamedEntry* entry = lookupEntryByKey(table, key);
        return entry ? copyOut(entry->file, buffer, bufferLength) : false;
    }

    static bool lookupFileByMappedId(const std::map<std::string, NamedEntry>& table,
                                     const std::map<long, std::string>& keyById,
                                     long id,
                                     char* buffer,
                                     long bufferLength)
    {
        const NamedEntry* entry = lookupEntryByMappedId(table, keyById, id);
        return entry ? copyOut(entry->file, buffer, bufferLength) : false;
    }

    static void storeNamedEntry(std::map<std::string, NamedEntry>& table,
                                const std::string& key,
                                const NamedEntry& entry,
                                bool allowOverwrite)
    {
        if (allowOverwrite || table.find(key) == table.end())
            table[key] = entry;
    }

    static void storeMappedKey(std::map<long, std::string>& table,
                               long id,
                               const std::string& key,
                               bool allowOverwrite)
    {
        if (id < 0)
            return;

        if (allowOverwrite || table.find(id) == table.end())
        {
            table[id] = key;
            return;
        }

        /* trace removed: Catalog: preserve clean mapping */
}

    static void storeEntry(const std::string& blockType, const NamedEntry& entry, bool allowOverwriteMappings)
    {
        if (entry.key.empty())
        {
            /* trace removed: Catalog: skip block */
return;
        }

        NamedEntry stored = entry;
        if (stored.value.empty())
            stored.value = stored.key;

        if (blockType == "String")
        {
            storeNamedEntry(g_stringsByKey, stored.key, stored, allowOverwriteMappings);
            if (stored.legacyId >= 0)
                storeMappedKey(g_stringKeyByLegacyId, stored.legacyId, stored.key, allowOverwriteMappings);
            if (stored.legacyStringId >= 0)
                storeMappedKey(g_stringKeyByLegacyId, stored.legacyStringId, stored.key, allowOverwriteMappings);
        }
        else if (blockType == "Terrain")
        {
            storeNamedEntry(g_terrainByKey, stored.key, stored, allowOverwriteMappings);
            if (stored.legacyStringId >= 0)
                storeMappedKey(g_terrainKeyByLegacyStringId, stored.legacyStringId, stored.key, allowOverwriteMappings);
            if (stored.legacyId >= 0)
                storeMappedKey(g_terrainKeyByLegacyStringId, stored.legacyId, stored.key, allowOverwriteMappings);
            if (stored.index >= 0)
                storeMappedKey(g_terrainKeyByIndex, stored.index, stored.key, allowOverwriteMappings);
        }
        else if (blockType == "BaseTerrain" || blockType == "TerrainBase")
        {
            storeNamedEntry(g_baseTerrainByKey, stored.key, stored, allowOverwriteMappings);
            if (stored.legacyStringId >= 0)
                storeMappedKey(g_baseTerrainKeyByLegacyStringId, stored.legacyStringId, stored.key, allowOverwriteMappings);
            if (stored.legacyId >= 0)
                storeMappedKey(g_baseTerrainKeyByLegacyStringId, stored.legacyId, stored.key, allowOverwriteMappings);
            if (stored.index >= 0)
                storeMappedKey(g_baseTerrainKeyByIndex, stored.index, stored.key, allowOverwriteMappings);
        }
        else if (blockType == "ObjectGroup")
        {
            storeNamedEntry(g_objectGroupByKey, stored.key, stored, allowOverwriteMappings);
            if (stored.index >= 0)
                storeMappedKey(g_objectGroupKeyByIndex, stored.index, stored.key, allowOverwriteMappings);
            if (stored.legacyStringId >= 0)
                storeMappedKey(g_objectGroupKeyByLegacyStringId, stored.legacyStringId, stored.key, allowOverwriteMappings);
            if (stored.legacyId >= 0)
                storeMappedKey(g_objectGroupKeyByLegacyStringId, stored.legacyId, stored.key, allowOverwriteMappings);
        }
        else if (blockType == "Object")
        {
            storeNamedEntry(g_objectsByKey, stored.key, stored, allowOverwriteMappings);
            if (stored.legacyId >= 0)
                storeMappedKey(g_objectKeyByLegacyId, stored.legacyId, stored.key, allowOverwriteMappings);
        }
        else if (blockType == "Overlay")
        {
            storeNamedEntry(g_overlaysByKey, stored.key, stored, allowOverwriteMappings);
            if (stored.legacyId >= 0)
                storeMappedKey(g_overlayKeyByLegacyId, stored.legacyId, stored.key, allowOverwriteMappings);
        }
        else
        {
            /* trace removed: Catalog: skip unsupported block */
return;
        }

        /* trace removed: Catalog: stored block */
}

    static bool parseCatalogFile(const char* path, bool allowOverwriteMappings = true)
    {
        std::ifstream in(path);
        if (!in)
        {
            return false;
        }

        /* trace removed: Catalog: open ok */
std::string line;
        std::string blockType;
        std::string pendingBlockType;
        NamedEntry entry;
        bool inBlock = false;
        long lineNumber = 0;
        long baseBefore = static_cast<long>(g_baseTerrainByKey.size());

        while (std::getline(in, line))
        {
            ++lineNumber;

            std::string s = trim(line);
            if (s.empty())
                continue;
            if (s.size() >= 2 && s[0] == '/' && s[1] == '/')
                continue;
            if (s[0] == '#')
                continue;

            const std::string::size_type comment = s.find("//");
            if (comment != std::string::npos)
                s = trim(s.substr(0, comment));
            if (s.empty())
                continue;

            if (!inBlock)
            {
                const std::string::size_type brace = s.find('{');
                if (brace != std::string::npos)
                {
                    blockType = trim(s.substr(0, brace));
                    if (blockType.empty() && !pendingBlockType.empty())
                        blockType = pendingBlockType;
                    pendingBlockType.clear();

                    entry = NamedEntry();
                    inBlock = true;
                    /* trace removed: Catalog: begin block */
}
                else if (s.find('=') == std::string::npos)
                {
                    pendingBlockType = s;
                    /* trace removed: Catalog: pending block */
}

                continue;
            }

            if (s.find('}') != std::string::npos)
            {
                /* trace removed: Catalog: end block */
storeEntry(blockType, entry, allowOverwriteMappings);
                blockType.clear();
                entry = NamedEntry();
                inBlock = false;
                continue;
            }

            const std::string::size_type eq = s.find('=');
            if (eq == std::string::npos)
            {
                /* trace removed: Catalog: ignored line */
continue;
            }

            const std::string k = trim(s.substr(0, eq));
            std::string v = trim(s.substr(eq + 1));
            if (!v.empty() && v[v.size() - 1] == ';')
                v.erase(v.size() - 1);

            const std::string value = unquote(v);

            if (k == "key" || k == "Key")
                entry.key = value;
            else if (k == "value" || k == "Value")
                entry.value = value;
            else if (k == "displayName" || k == "DisplayName" || k == "name" || k == "Name")
                entry.value = value;
            else if (k == "file" || k == "File" || k == "texture" || k == "Texture" || k == "path" || k == "Path" || k == "textureFile" || k == "TextureFile")
                entry.file = value;
            else if (k == "legacyId" || k == "LegacyId")
                entry.legacyId = toLong(v);
            else if (k == "legacyStringId" || k == "LegacyStringId")
                entry.legacyStringId = toLong(v);
            else if (k == "index" || k == "Index")
                entry.index = toLong(v);
            else
            {
                /* Expected metadata fields not consumed by this lightweight catalog are ignored. */
            }
}

        /* trace removed: Catalog: finished path */
return true;
    }

    static void loadAll()
    {
        if (g_loaded)
            return;

        g_loaded = true;
        // Reset the catalog trace only when trace logging is explicitly enabled.
        if (getenv("MC2_EDITOR_TRACE") != NULL)
        {
            FILE* fp = 0;
#ifdef _MSC_VER
            fopen_s(&fp, "editor-catalog-trace.log", "w");
#else
            fp = fopen("editor-catalog-trace.log", "w");
#endif
            if (fp)
            {
                fprintf(fp, "EditorResourceCatalog trace start\n");
                fclose(fp);
            }
        }

/*
            Editor resource catalog load order:

            1. Individual editor FIT files from data/defs/ (catalogs, menus, text).
               These are the authoritative clean localization surface.
            2. editor_strings_legacy.fit last, as fallback for editor IDS_*
               numeric lookups not covered by the clean files.

            data/editor/ is deprecated and no longer searched.
        */
        /*
            Load exact authoritative defs paths directly.
            Avoid N x M directory/file scans that caused long editor popup stalls
            while probing invalid path combinations.
        */
        const char* cleanPaths[] =
        {
            "data/defs/catalogs/editor/editor_base_terrain.fit",
            "data/defs/catalogs/editor/editor_terrain.fit",
            "data/defs/catalogs/editor/editor_overlays.fit",
            "data/defs/catalogs/editor/editor_objects.fit",

            "data/defs/menus/editor/editor_menus.fit",

            "data/defs/text/en_us/editor/editor.fit",
            "data/defs/text/en_us/editor/editor_mclib.fit",
            "data/defs/text/en_us/editor/editor_mfc.fit",
            "data/defs/text/en_us/editor/editor_cursor.fit",
            "data/defs/text/en_us/editor/editor_techscript.fit",
            "data/defs/text/en_us/editor/menu_text_bindings.fit",
            "data/defs/text/en_us/editor/mfc_text_bindings.fit",
            0
        };

        for (int i = 0; cleanPaths[i]; ++i)
        {
            parseCatalogFile(cleanPaths[i]);
        }


        /*
            Legacy fallback: editor_strings_legacy.fit provides the correct
            legacyId mappings for editor IDS_* defines (40001–40093).
            Loaded last so its legacyId entries overwrite any mis-mapped
            IDs from the clean files (e.g. 40001 collision between
            editor.fit and editor_strings_legacy.fit).
        */
        const char* legacyPaths[] =
        {
            "data/defs/text/en_us/editor/editor_strings_legacy.fit",
            "./data/defs/text/en_us/editor/editor_strings_legacy.fit",
            0
        };

        for (int l = 0; legacyPaths[l]; ++l)
        {
            parseCatalogFile(legacyPaths[l], false);
        }

    }
}

namespace EditorResourceCatalog
{
    // by Methuselas: keep resource ownership here. The Editor catalog loads
    // from data/defs/ FIT files and only uses legacy numeric IDs as compatibility
    // bridges for older dialogs; mc2res.dll must not become the owner again.
    bool Load()
    {
        loadAll();
        return true;
    }

    bool GetStringByKey(const char* key, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupNameByKey(g_stringsByKey, key, buffer, bufferLength);
    }

    bool GetStringByLegacyId(long legacyId, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupNameByMappedId(g_stringsByKey, g_stringKeyByLegacyId, legacyId, buffer, bufferLength);
    }

    bool GetTerrainNameByKey(const char* key, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupNameByKey(g_terrainByKey, key, buffer, bufferLength);
    }

    bool GetTerrainNameByLegacyStringId(long legacyStringId, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupNameByMappedId(g_terrainByKey, g_terrainKeyByLegacyStringId, legacyStringId, buffer, bufferLength);
    }

    bool GetTerrainNameByIndex(long index, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupNameByMappedId(g_terrainByKey, g_terrainKeyByIndex, index, buffer, bufferLength);
    }

    bool GetTerrainFileByKey(const char* key, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupFileByKey(g_terrainByKey, key, buffer, bufferLength);
    }

    bool GetTerrainFileByLegacyStringId(long legacyStringId, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupFileByMappedId(g_terrainByKey, g_terrainKeyByLegacyStringId, legacyStringId, buffer, bufferLength);
    }

    bool GetTerrainFileByIndex(long index, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupFileByMappedId(g_terrainByKey, g_terrainKeyByIndex, index, buffer, bufferLength);
    }

    bool GetBaseTerrainNameByKey(const char* key, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupNameByKey(g_baseTerrainByKey, key, buffer, bufferLength);
    }

    bool GetBaseTerrainNameByLegacyStringId(long legacyStringId, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupNameByMappedId(g_baseTerrainByKey, g_baseTerrainKeyByLegacyStringId, legacyStringId, buffer, bufferLength);
    }

    bool GetBaseTerrainNameByIndex(long index, char* buffer, long bufferLength)
    {
        loadAll();

        const bool ok = lookupNameByMappedId(g_baseTerrainByKey, g_baseTerrainKeyByIndex, index, buffer, bufferLength);
        /* trace removed: Catalog: GetBaseTerrainNameByIndex */
return ok;
    }

    bool GetBaseTerrainFileByKey(const char* key, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupFileByKey(g_baseTerrainByKey, key, buffer, bufferLength);
    }

    bool GetBaseTerrainFileByLegacyStringId(long legacyStringId, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupFileByMappedId(g_baseTerrainByKey, g_baseTerrainKeyByLegacyStringId, legacyStringId, buffer, bufferLength);
    }

    bool GetBaseTerrainFileByIndex(long index, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupFileByMappedId(g_baseTerrainByKey, g_baseTerrainKeyByIndex, index, buffer, bufferLength);
    }

    bool GetObjectGroupNameByIndex(long index, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupNameByMappedId(g_objectGroupByKey, g_objectGroupKeyByIndex, index, buffer, bufferLength);
    }

    bool GetObjectGroupNameByLegacyStringId(long legacyStringId, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupNameByMappedId(g_objectGroupByKey, g_objectGroupKeyByLegacyStringId, legacyStringId, buffer, bufferLength);
    }

    bool GetObjectNameByKey(const char* key, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupNameByKey(g_objectsByKey, key, buffer, bufferLength);
    }

    bool GetObjectNameByLegacyId(long legacyId, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupNameByMappedId(g_objectsByKey, g_objectKeyByLegacyId, legacyId, buffer, bufferLength);
    }

    bool GetOverlayNameByKey(const char* key, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupNameByKey(g_overlaysByKey, key, buffer, bufferLength);
    }

    bool GetOverlayNameByLegacyId(long legacyId, char* buffer, long bufferLength)
    {
        loadAll();
        return lookupNameByMappedId(g_overlaysByKey, g_overlayKeyByLegacyId, legacyId, buffer, bufferLength);
    }
}
