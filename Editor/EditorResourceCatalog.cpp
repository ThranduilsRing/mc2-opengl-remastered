/***************************************************************
* FILENAME: EditorResourceCatalog.cpp
* DESCRIPTION: Implements the Editor-local resource catalog backed by data/editor FIT files.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster attribution header and human-maintenance comments.
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

    static void catalogTrace(const char* fmt, ...)
    {
        char line[2048] = {0};

        va_list args;
        va_start(args, fmt);
#ifdef _MSC_VER
        _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, args);
#else
        vsnprintf(line, sizeof(line), fmt, args);
#endif
        va_end(args);

        FILE* fp = 0;
#ifdef _MSC_VER
        fopen_s(&fp, "editor-catalog-trace.log", "a");
#else
        fp = fopen("editor-catalog-trace.log", "a");
#endif
        if (fp)
        {
            fprintf(fp, "%s\n", line);
            fclose(fp);
        }

#ifdef _WIN32
        OutputDebugStringA(line);
        OutputDebugStringA("\n");
#endif
    }

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

    static void storeEntry(const std::string& blockType, const NamedEntry& entry)
    {
        if (entry.key.empty())
        {
            catalogTrace("Catalog: skip block='%s' reason=empty-key name='%s' file='%s' index=%ld legacyStringId=%ld",
                         blockType.c_str(),
                         entry.value.c_str(),
                         entry.file.c_str(),
                         entry.index,
                         entry.legacyStringId);
            return;
        }

        NamedEntry stored = entry;
        if (stored.value.empty())
            stored.value = stored.key;

        if (blockType == "String")
        {
            g_stringsByKey[stored.key] = stored;
            if (stored.legacyId >= 0)
                g_stringKeyByLegacyId[stored.legacyId] = stored.key;
            if (stored.legacyStringId >= 0)
                g_stringKeyByLegacyId[stored.legacyStringId] = stored.key;
        }
        else if (blockType == "Terrain")
        {
            g_terrainByKey[stored.key] = stored;
            if (stored.legacyStringId >= 0)
                g_terrainKeyByLegacyStringId[stored.legacyStringId] = stored.key;
            if (stored.legacyId >= 0)
                g_terrainKeyByLegacyStringId[stored.legacyId] = stored.key;
            if (stored.index >= 0)
                g_terrainKeyByIndex[stored.index] = stored.key;
        }
        else if (blockType == "BaseTerrain" || blockType == "TerrainBase")
        {
            g_baseTerrainByKey[stored.key] = stored;
            if (stored.legacyStringId >= 0)
                g_baseTerrainKeyByLegacyStringId[stored.legacyStringId] = stored.key;
            if (stored.legacyId >= 0)
                g_baseTerrainKeyByLegacyStringId[stored.legacyId] = stored.key;
            if (stored.index >= 0)
                g_baseTerrainKeyByIndex[stored.index] = stored.key;
        }
        else if (blockType == "ObjectGroup")
        {
            g_objectGroupByKey[stored.key] = stored;
            if (stored.index >= 0)
                g_objectGroupKeyByIndex[stored.index] = stored.key;
            if (stored.legacyStringId >= 0)
                g_objectGroupKeyByLegacyStringId[stored.legacyStringId] = stored.key;
            if (stored.legacyId >= 0)
                g_objectGroupKeyByLegacyStringId[stored.legacyId] = stored.key;
        }
        else if (blockType == "Object")
        {
            g_objectsByKey[stored.key] = stored;
            if (stored.legacyId >= 0)
                g_objectKeyByLegacyId[stored.legacyId] = stored.key;
        }
        else if (blockType == "Overlay")
        {
            g_overlaysByKey[stored.key] = stored;
            if (stored.legacyId >= 0)
                g_overlayKeyByLegacyId[stored.legacyId] = stored.key;
        }
        else
        {
            catalogTrace("Catalog: skip unsupported block='%s' key='%s'", blockType.c_str(), stored.key.c_str());
            return;
        }

        catalogTrace("Catalog: stored block='%s' key='%s' name='%s' file='%s' index=%ld legacyId=%ld legacyStringId=%ld",
                     blockType.c_str(),
                     stored.key.c_str(),
                     stored.value.c_str(),
                     stored.file.c_str(),
                     stored.index,
                     stored.legacyId,
                     stored.legacyStringId);
    }

    static bool parseCatalogFile(const char* path)
    {
        std::ifstream in(path);
        if (!in)
        {
            catalogTrace("Catalog: open failed path='%s'", path ? path : "<null>");
            return false;
        }

        catalogTrace("Catalog: open ok path='%s'", path ? path : "<null>");

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
                    catalogTrace("Catalog: begin block path='%s' line=%ld type='%s'",
                                 path ? path : "<null>",
                                 lineNumber,
                                 blockType.c_str());
                }
                else if (s.find('=') == std::string::npos)
                {
                    pendingBlockType = s;
                    catalogTrace("Catalog: pending block path='%s' line=%ld type='%s'",
                                 path ? path : "<null>",
                                 lineNumber,
                                 pendingBlockType.c_str());
                }

                continue;
            }

            if (s.find('}') != std::string::npos)
            {
                catalogTrace("Catalog: end block path='%s' line=%ld type='%s' key='%s' name='%s' file='%s' index=%ld legacyStringId=%ld",
                             path ? path : "<null>",
                             lineNumber,
                             blockType.c_str(),
                             entry.key.c_str(),
                             entry.value.c_str(),
                             entry.file.c_str(),
                             entry.index,
                             entry.legacyStringId);

                storeEntry(blockType, entry);
                blockType.clear();
                entry = NamedEntry();
                inBlock = false;
                continue;
            }

            const std::string::size_type eq = s.find('=');
            if (eq == std::string::npos)
            {
                catalogTrace("Catalog: ignored line path='%s' line=%ld text='%s'",
                             path ? path : "<null>",
                             lineNumber,
                             s.c_str());
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
                catalogTrace("Catalog: unknown field path='%s' line=%ld block='%s' field='%s' value='%s'",
                             path ? path : "<null>",
                             lineNumber,
                             blockType.c_str(),
                             k.c_str(),
                             value.c_str());
        }

        catalogTrace("Catalog: finished path='%s' baseTerrainAdded=%ld baseTerrainTotal=%ld",
                     path ? path : "<null>",
                     static_cast<long>(g_baseTerrainByKey.size()) - baseBefore,
                     static_cast<long>(g_baseTerrainByKey.size()));

        return true;
    }

    static void loadAll()
    {
        if (g_loaded)
            return;

        g_loaded = true;

        // Reset the trace for each process run.
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

        const char* basePaths[] =
        {
            "data/editor/",
            "./data/editor/",
            "Editor/data/editor/",
            "./Editor/data/editor/",
            0
        };

        const char* files[] =
        {
            "editor_strings.fit",
            "editor_base_terrain.fit",
            "editor_terrain.fit",
            "editor_overlays.fit",
            "editor_objects.fit",
            "editor_menus.fit",
            0
        };

        for (int b = 0; basePaths[b]; ++b)
        {
            for (int f = 0; files[f]; ++f)
            {
                std::string path = std::string(basePaths[b]) + files[f];
                parseCatalogFile(path.c_str());
            }
        }

        catalogTrace("Catalog: loadAll complete strings=%ld baseTerrain=%ld terrain=%ld objects=%ld overlays=%ld",
                     static_cast<long>(g_stringsByKey.size()),
                     static_cast<long>(g_baseTerrainByKey.size()),
                     static_cast<long>(g_terrainByKey.size()),
                     static_cast<long>(g_objectsByKey.size()),
                     static_cast<long>(g_overlaysByKey.size()));
    }
}

namespace EditorResourceCatalog
{
    // by Methuselas: keep resource ownership here. The Editor catalog loads
    // from data/editor/*.fit and only uses legacy numeric IDs as compatibility
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
        catalogTrace("Catalog: GetBaseTerrainNameByIndex index=%ld ok=%d value='%s'",
                     index,
                     ok ? 1 : 0,
                     (ok && buffer) ? buffer : "");
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
