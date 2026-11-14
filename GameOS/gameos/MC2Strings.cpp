/***************************************************************
 * FILENAME: MC2Strings.cpp
 * DESCRIPTION: Implements FIT-backed engine string lookup for replacing
 *              mc2res.dll string resolution with data/defs text records.
 *
 * AUTHOR: Microsoft Corporation
 * CREATED: Unknown
 *
 * UPDATED BY: Methuselas
 * UPDATED: 2026-05-05
 *
 * CHANGES:
 * - Added typed-block FIT loading for String, ResourceTextAlias, LegacyTextAlias, and
 *   MenuTextBinding records used by the engine text bridge.
 * - Added legacyId lookup with explicit localization alias precedence,
 *   then coverage-safe mc2res_legacy fallback.
 * - Added loud missing-string markers instead of silent mc2res fallback.
 * - Scoped Phase 1 loading to engine/game/viewer text and menu paths.
 * - Added locale path seam with en_us as the current fixed default.
 ***************************************************************/

#include "mc2_czstring.h"
#include "MC2Strings.h"
#include "utils/mc2log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "mc2fmt.h"

// Forward-declare mod file enumeration from mclib/file.cpp.
// MC2Strings must not pull in all of file.h; this thin seam is intentional.
using ModFitCallback = void (*)(mc2::czstring absPath, void* userData);
void EnumerateModFitFiles(mc2::czstring keyPrefix, ModFitCallback cb, void* userData);

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace
{
    struct StringRecord
    {
        std::string key;
        std::string text;
        long legacyId;
        std::string source;
        std::string sourceFile;

        StringRecord()
            : legacyId(0)
        {
        }
    };

    struct CatalogState
    {
        bool attempted;
        bool loaded;

        std::map<std::string, StringRecord> byKey;
        std::map<long, std::string> keyByLegacyId;
        std::map<long, std::string> keyByLegacyAlias;
        std::map<long, std::string> legacySourceById;

        std::map<unsigned int, std::string> keyByResourceId;
        std::map<std::string, std::string> keyByResourceSymbol;

        std::map<std::string, std::string> keyByMenuBinding;

        // The legacy-id maps as the stock files left them, before any mod overlay: the fallback for a bad override.
        bool stockTaken = false;
        std::map<long, std::string> stockAliasByLegacyId;
        std::map<long, std::string> stockKeyByLegacyId;
        unsigned long long revision = 1;

        CatalogState()
            : attempted(false)
            , loaded(false)
        {
        }
    };

    CatalogState& State()
    {
        static CatalogState state;
        return state;
    }

    bool TraceEnabled()
    {
        static const bool enabled = (false);
        return enabled;
    }

    void Trace(const char* fmt, ...)
    {
        if (!TraceEnabled())
            return;

        char line[2048] = {0};

        va_list args;
        va_start(args, fmt);
#ifdef _MSC_VER
        _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, args);
#else
        vsnprintf(line, sizeof(line), fmt, args);
#endif
        va_end(args);

        MC2_LOG(Ui, Info, "strings: {}", line);
    }

    std::string Trim(std::string_view value)
    {
        std::string::size_type begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
            ++begin;

        std::string::size_type end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
            --end;

        return std::string(value.substr(begin, end - begin));
    }

    std::string ToLower(std::string value)
    {
        for (char & i : value)
            i = static_cast<char>(std::tolower(static_cast<unsigned char>(i)));

        return value;
    }

    bool IsNumeric(std::string_view value)
    {
        const std::string v = Trim(value);
        if (v.empty())
            return false;

        std::string::size_type i = 0;
        if (v[0] == '-' || v[0] == '+')
            i = 1;

        if (i >= v.size())
            return false;

        for (; i < v.size(); ++i)
        {
            if (!std::isdigit(static_cast<unsigned char>(v[i])))
                return false;
        }

        return true;
    }

    long ToLong(std::string_view value, long fallback = 0)
    {
        const std::string trimmed = Trim(value);
        if (trimmed.empty())
            return fallback;

        char* end = nullptr;
        const long result = std::strtol(trimmed.c_str(), &end, 10);
        if (!end || *end != 0)
            return fallback;

        return result;
    }

    std::string Unescape(std::string_view value)
    {
        std::string out;
        out.reserve(value.size());

        for (std::string::size_type i = 0; i < value.size(); ++i)
        {
            const char c = value[i];
            if (c == '\\' && i + 1 < value.size())
            {
                const char next = value[++i];
                switch (next)
                {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '\\':
                        // Stock mc2res line-break code is a backslash before the newline ("TITLE\<newline>text"); the bridge keeps it
                        // as an escaped backslash followed by \n. It is a break code, not text: drop it (the font has a visible glyph).
                        if (i + 2 < value.size() && value[i + 1] == '\\' && value[i + 2] == 'n')
                            break;
                        out.push_back(next);
                        break;
                    case '"':
                        out.push_back(next);
                        break;
                    default:
                        out.push_back(next);
                        break;
                }
            }
            else
            {
                out.push_back(c);
            }
        }

        return out;
    }

    std::string Unquote(std::string_view value)
    {
        const std::string trimmed = Trim(value);
        if (trimmed.size() >= 2 && trimmed[0] == '"' && trimmed[trimmed.size() - 1] == '"')
            return Unescape(trimmed.substr(1, trimmed.size() - 2));

        return trimmed;
    }

    bool ReadWholeFile(mc2::czstring path, std::string& out)
    {
        out.clear();

        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in)
            return false;

        in.seekg(0, std::ios::end);
        const std::ifstream::pos_type size = in.tellg();
        if (size < 0)
            return false;

        in.seekg(0, std::ios::beg);
        out.resize(static_cast<std::string::size_type>(size));
        if (!out.empty())
            in.read(&out[0], static_cast<std::streamsize>(size));

        return true;
    }

    bool IsBlockTypeStart(std::string_view text, std::string::size_type pos)
    {
        /*
            Typed-block FIT files generated for data/defs put block declarations
            at line start. Requiring line-start prevents quoted diagnostic text
            such as "String {" from being treated as a real block.
        */
        while (pos > 0)
        {
            const char prev = text[pos - 1];
            if (prev == '\n' || prev == '\r')
                return true;

            if (prev != ' ' && prev != '\t')
                return false;

            --pos;
        }

        return true;
    }

    bool FindNextTypedBlock(std::string_view text,
                            std::string::size_type& searchPos,
                            std::string& blockType,
                            std::string& blockBody)
    {
        blockType.clear();
        blockBody.clear();

        while (searchPos < text.size())
        {
            std::string::size_type pos = searchPos;

            while (pos < text.size())
            {
                while (pos < text.size() &&
                       (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n'))
                    ++pos;

                if (pos >= text.size())
                {
                    searchPos = pos;
                    return false;
                }

                if (IsBlockTypeStart(text, pos) &&
                    (std::isalpha(static_cast<unsigned char>(text[pos])) || text[pos] == '_'))
                    break;

                std::string::size_type nextLine = text.find('\n', pos);
                if (nextLine == std::string::npos)
                {
                    searchPos = text.size();
                    return false;
                }
                pos = nextLine + 1;
            }

            const std::string::size_type typeStart = pos;
            while (pos < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[pos])) || text[pos] == '_'))
                ++pos;

            std::string type(text.substr(typeStart, pos - typeStart));

            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
                ++pos;

            if (pos >= text.size() || text[pos] != '{')
            {
                searchPos = pos;
                continue;
            }

            const std::string::size_type bodyStart = pos + 1;
            ++pos;

            int depth = 1;
            bool inQuote = false;
            bool escaping = false;

            for (; pos < text.size(); ++pos)
            {
                const char c = text[pos];

                if (inQuote)
                {
                    if (escaping)
                    {
                        escaping = false;
                        continue;
                    }

                    if (c == '\\')
                    {
                        escaping = true;
                        continue;
                    }

                    if (c == '"')
                        inQuote = false;

                    continue;
                }

                if (c == '"')
                {
                    inQuote = true;
                    continue;
                }

                if (c == '{')
                {
                    ++depth;
                    continue;
                }

                if (c == '}')
                {
                    --depth;
                    if (depth == 0)
                    {
                        blockType = type;
                        blockBody = text.substr(bodyStart, pos - bodyStart);
                        searchPos = pos + 1;
                        return true;
                    }
                }
            }

            searchPos = text.size();
            return false;
        }

        return false;
    }

    bool ReadField(std::string_view blockBody, std::string_view fieldName, std::string& out)
    {
        out.clear();

        std::string::size_type pos = 0;
        const std::string field(fieldName);

        while ((pos = blockBody.find(field, pos)) != std::string::npos)
        {
            const bool leftOk = (pos == 0) ||
                (!std::isalnum(static_cast<unsigned char>(blockBody[pos - 1])) && blockBody[pos - 1] != '_');

            std::string::size_type scan = pos + field.size();
            const bool rightOk = (scan >= blockBody.size()) ||
                (!std::isalnum(static_cast<unsigned char>(blockBody[scan])) && blockBody[scan] != '_');

            if (!leftOk || !rightOk)
            {
                pos = scan;
                continue;
            }

            while (scan < blockBody.size() && std::isspace(static_cast<unsigned char>(blockBody[scan])))
                ++scan;

            if (scan >= blockBody.size() || blockBody[scan] != '=')
            {
                pos = scan;
                continue;
            }

            ++scan;
            while (scan < blockBody.size() && std::isspace(static_cast<unsigned char>(blockBody[scan])))
                ++scan;

            if (scan >= blockBody.size())
                return false;

            if (blockBody[scan] == '"')
            {
                const std::string::size_type valueStart = scan;
                ++scan;

                bool escaping = false;
                for (; scan < blockBody.size(); ++scan)
                {
                    const char c = blockBody[scan];
                    if (escaping)
                    {
                        escaping = false;
                        continue;
                    }

                    if (c == '\\')
                    {
                        escaping = true;
                        continue;
                    }

                    if (c == '"')
                    {
                        out = Unquote(blockBody.substr(valueStart, scan - valueStart + 1));
                        return true;
                    }
                }

                out = Unquote(blockBody.substr(valueStart));
                return true;
            }

            const std::string::size_type valueStart = scan;
            while (scan < blockBody.size() &&
                   blockBody[scan] != '\n' &&
                   blockBody[scan] != '\r' &&
                   blockBody[scan] != ';')
            {
                ++scan;
            }

            out = Trim(blockBody.substr(valueStart, scan - valueStart));
            return true;
        }

        return false;
    }

#ifdef PLATFORM_WINDOWS
    void EnumerateFitFiles(const std::string& directory, std::vector<std::string>& files)
    {
        std::string pattern = directory + "\\*.fit";

        WIN32_FIND_DATAA findData;
        HANDLE findHandle = FindFirstFileA(pattern.c_str(), &findData);
        if (findHandle == INVALID_HANDLE_VALUE)
            return;

        do
        {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                files.push_back(directory + "\\" + findData.cFileName);
        }
        while (FindNextFileA(findHandle, &findData));

        FindClose(findHandle);
    }
#else
    void EnumerateFitFiles(const std::string& directory, std::vector<std::string>& files)
    {
        DIR* dir = opendir(directory.c_str());
        if (!dir)
            return;

        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr)
        {
            const char* name = entry->d_name;
            const size_t len = std::strlen(name);
            if (len > 4 && std::strcmp(name + len - 4, ".fit") == 0)
                files.push_back(directory + "/" + name);
        }

        closedir(dir);
    }
#endif

    void NormalizePath(std::string& path)
    {
#ifdef PLATFORM_WINDOWS
        for (std::string::size_type i = 0; i < path.size(); ++i)
        {
            if (path[i] == '/')
                path[i] = '\\';
        }
#endif
    }

    std::string JoinPath(std::string_view a, std::string_view b)
    {
#ifdef PLATFORM_WINDOWS
        const char sep = '\\';
#else
        const char sep = '/';
#endif
        if (a.empty())
            return std::string(b);

        if (a[a.size() - 1] == '/' || a[a.size() - 1] == '\\')
            return std::string(a).append(b);

        return std::string(a).append(1, sep).append(b);
    }

    bool SourceIsMC2Res(std::string_view source)
    {
        return ToLower(Trim(source)) == "mc2res";
    }

    void StoreStringRecord(const StringRecord& record)
    {
        if (record.key.empty())
            return;

        CatalogState& state = State();

        if (state.byKey.find(record.key) == state.byKey.end())
        {
            state.byKey[record.key] = record;
        }
        else
        {
            Trace("duplicate String key '%s' from %s; keeping first", record.key.c_str(), record.sourceFile.c_str());
        }

        if (record.legacyId > 0)
        {
            const std::map<long, std::string>::iterator idIt = state.keyByLegacyId.find(record.legacyId);
            if (idIt == state.keyByLegacyId.end())
            {
                state.keyByLegacyId[record.legacyId] = record.key;
                state.legacySourceById[record.legacyId] = record.source;
            }
            else
            {
                auto& [legacyIdKey, priorKey] = *idIt;
                const std::string oldSource = state.legacySourceById[record.legacyId];
                const bool oldIsMC2Res = SourceIsMC2Res(oldSource);
                const bool newIsMC2Res = SourceIsMC2Res(record.source);

                if (!oldIsMC2Res && newIsMC2Res)
                {
                    // mc2res canonical key overrides a prior non-mc2res entry
                    Trace("legacyId %ld remapped to canonical mc2res key '%s' from prior key '%s'",
                          record.legacyId, record.key.c_str(), priorKey.c_str());
                    priorKey = record.key;
                    state.legacySourceById[record.legacyId] = record.source;
                }
                else if (oldIsMC2Res && !newIsMC2Res)
                {
                    // Mod/game-specific entry overrides mc2res placeholder (e.g. "undefined")
                    Trace("legacyId %ld: mod key '%s' overrides mc2res placeholder '%s'",
                          record.legacyId, record.key.c_str(), priorKey.c_str());
                    priorKey = record.key;
                    state.legacySourceById[record.legacyId] = record.source;
                }
                else
                {
                    Trace("duplicate legacyId %ld from %s key='%s'; keeping key='%s'",
                          record.legacyId,
                          record.sourceFile.c_str(),
                          record.key.c_str(),
                          priorKey.c_str());
                }
            }
        }
    }

    void StoreResourceAlias(std::string_view resourceId, std::string_view textKey)
    {
        if (resourceId.empty() || textKey.empty())
            return;

        if (IsNumeric(resourceId))
        {
            const long id = ToLong(resourceId, -1);
            if (id >= 0)
                State().keyByResourceId[static_cast<unsigned int>(id)] = textKey;
        }
        else
        {
            State().keyByResourceSymbol[std::string(resourceId)] = textKey;
        }
    }

    void StoreLegacyAlias(std::string_view legacyId, std::string_view textKey)
    {
        if (legacyId.empty() || textKey.empty())
            return;

        if (!IsNumeric(legacyId))
            return;

        const long id = ToLong(legacyId, -1);
        if (id > 0)
            State().keyByLegacyAlias[id] = textKey;
    }

    std::string MenuBindingKey(mc2::czstring menuId, mc2::czstring commandId)
    {
        std::string key;
        if (menuId)
            key += menuId;
        key += "|";
        if (commandId)
            key += commandId;
        return key;
    }

    void StoreMenuBinding(const std::string& menuId, const std::string& commandId, const std::string& textKey)
    {
        if (menuId.empty() || commandId.empty() || textKey.empty())
            return;

        State().keyByMenuBinding[MenuBindingKey(menuId.c_str(), commandId.c_str())] = textKey;
    }

    void LoadFile(const std::string& path)
    {
        std::string fileText;
        if (!ReadWholeFile(path.c_str(), fileText))
            return;

        std::string::size_type searchPos = 0;
        std::string blockType;
        std::string blockBody;

        long stringCount = 0;
        long aliasCount = 0;
        long legacyAliasCount = 0;
        long menuCount = 0;

        while (FindNextTypedBlock(fileText, searchPos, blockType, blockBody))
        {
            if (blockType == "String")
            {
                StringRecord record;
                record.sourceFile = path;

                ReadField(blockBody, "key", record.key);

                if (!ReadField(blockBody, "text", record.text))
                    ReadField(blockBody, "value", record.text);

                std::string legacyId;
                if (ReadField(blockBody, "legacyId", legacyId))
                    record.legacyId = ToLong(legacyId, 0);

                ReadField(blockBody, "source", record.source);

                if (!record.key.empty())
                {
                    StoreStringRecord(record);
                    ++stringCount;
                }
            }
            else if (blockType == "ResourceTextAlias")
            {
                std::string resourceId;
                std::string textKey;

                ReadField(blockBody, "resourceId", resourceId);
                ReadField(blockBody, "textKey", textKey);
                StoreResourceAlias(resourceId, textKey);
                ++aliasCount;
            }
            else if (blockType == "LegacyTextAlias")
            {
                std::string legacyId;
                std::string textKey;

                ReadField(blockBody, "legacyId", legacyId);
                ReadField(blockBody, "textKey", textKey);
                StoreLegacyAlias(legacyId, textKey);
                ++legacyAliasCount;
            }
            else if (blockType == "MenuTextBinding")
            {
                std::string menuId;
                std::string commandId;
                std::string textKey;

                ReadField(blockBody, "menuId", menuId);
                ReadField(blockBody, "commandId", commandId);
                ReadField(blockBody, "textKey", textKey);
                StoreMenuBinding(menuId, commandId, textKey);
                ++menuCount;
            }
        }

        ++State().revision;   // strings or aliases may have changed: template caches revalidate

        if (stringCount || aliasCount || legacyAliasCount || menuCount)
            Trace("loaded %ld String, %ld ResourceTextAlias, %ld LegacyTextAlias, %ld MenuTextBinding from %s",
                  stringCount, aliasCount, legacyAliasCount, menuCount, path.c_str());
    }

    bool TryLoadDirectory(std::string_view rawDirectory)
    {
        std::string directory(rawDirectory);
        NormalizePath(directory);

        std::vector<std::string> files;
        EnumerateFitFiles(directory, files);
        if (files.empty())
            return false;

        std::sort(files.begin(), files.end());

        for (std::vector<std::string>::const_iterator it = files.begin(); it != files.end(); ++it)
            LoadFile(*it);

        return true;
    }

    const char* ActiveLocale()
    {
        /*
            Phase 1 deliberately fixes both active and fallback locale to en_us.
            This is a localization seam, not a full language-selection system.
            Later phases may replace this with config/UI/command-line selection.
        */
        return "en_us";
    }

    void AddTextRootCandidates(std::vector<std::string>& dirs, std::string_view root, mc2::czstring locale)
    {
        if (root.empty() || !locale || !locale[0])
            return;

        dirs.push_back(JoinPath(root, (std::string("text/") + locale).c_str()));
        dirs.push_back(JoinPath(root, (std::string("data/defs/text/") + locale).c_str()));
    }

    void AddEngineSupportRootCandidates(std::vector<std::string>& dirs, std::string_view root)
    {
        if (root.empty())
            return;

        dirs.push_back(JoinPath(root, "debug"));
        dirs.push_back(JoinPath(root, "data/defs/debug"));
        dirs.push_back(JoinPath(root, "menus/game"));
        dirs.push_back(JoinPath(root, "data/defs/menus/game"));
        dirs.push_back(JoinPath(root, "menus/viewer"));
        dirs.push_back(JoinPath(root, "data/defs/menus/viewer"));
    }

    void AddDefsRootCandidates(std::vector<std::string>& dirs, std::string_view root)
    {
        AddTextRootCandidates(dirs, root, ActiveLocale());
        AddEngineSupportRootCandidates(dirs, root);
    }

    const char* MissingMarker(mc2::czstring prefix, mc2::czstring value)
    {
        static char buffers[8][256];
        static unsigned int index = 0;

        char* buffer = buffers[index++ % 8];

        mc2::fmt::to_buf_n(buffer, 256, "<missing-string:{}:{}>", mc2::fmt::safe(prefix ? prefix : "unknown"), mc2::fmt::safe(value ? value : ""));

        buffer[255] = 0;
        return buffer;
    }

    const char* MissingMarkerNumber(mc2::czstring prefix, long value)
    {
        char number[64] = {0};
        mc2::fmt::to_buf(number, "{:d}", mc2::fmt::as_i32(value));
        if ( false )
        {
            mc2::fmt::print_out("[strings] missing prefix='{}' id={:d}\n", mc2::fmt::safe(prefix ? prefix : "null"), mc2::fmt::as_i32(value));
            fflush(stdout);
        }
        return MissingMarker(prefix, number);
    }
}

namespace MC2Strings
{
    std::string UnescapeFitString(std::string_view value)
    {
        return Unescape(value);
    }

    bool Load()
    {
        CatalogState& state = State();
        if (state.attempted)
            return state.loaded;

        state.attempted = true;

        Trace("MC2 data/defs string trace start");

        std::vector<std::string> dirs;

        const std::string envRoot = mc2::gate::str<"MC2_DEFS_ROOT">();   // "" when unset or empty
        if (!envRoot.empty())
            AddDefsRootCandidates(dirs, envRoot.c_str());

        const char* locale = ActiveLocale();
        dirs.push_back(std::string("data/defs/text/") + locale);
        dirs.push_back(std::string("./data/defs/text/") + locale);
        dirs.push_back(std::string("../data/defs/text/") + locale);
        dirs.push_back(std::string("../../data/defs/text/") + locale);

        dirs.push_back("data/defs/debug");
        dirs.push_back("./data/defs/debug");
        dirs.push_back("../data/defs/debug");
        dirs.push_back("../../data/defs/debug");

        dirs.push_back("data/defs/menus/game");
        dirs.push_back("./data/defs/menus/game");
        dirs.push_back("../data/defs/menus/game");
        dirs.push_back("../../data/defs/menus/game");

        dirs.push_back("data/defs/menus/viewer");
        dirs.push_back("./data/defs/menus/viewer");
        dirs.push_back("../data/defs/menus/viewer");
        dirs.push_back("../../data/defs/menus/viewer");

        for (std::vector<std::string>::const_iterator it = dirs.begin(); it != dirs.end(); ++it)
            TryLoadDirectory(*it);

        state.stockAliasByLegacyId = state.keyByLegacyAlias;
        state.stockKeyByLegacyId = state.keyByLegacyId;
        state.stockTaken = true;

        // Load mod-overlay text FITs (e.g. mods/cveg/data/defs/text/en_us/*.fit).
        // These are NOT discoverable via raw filesystem scan (only the base data/ dir is on-disk).
        // EnumerateModFitFiles returns them sorted by filename — last-write-wins for aliases.
        {
            struct Loader { static void cb(mc2::czstring absPath, void* /*ud*/) {
                LoadFile(absPath);
            }};
            std::string prefix = std::string("data/defs/text/") + locale + "/";
            EnumerateModFitFiles(prefix.c_str(), Loader::cb, nullptr);
        }

        state.loaded = !state.byKey.empty();
        ++state.revision;

        Trace("string catalog loaded=%d keys=%u legacyIds=%u resourceIds=%u resourceSymbols=%u menuBindings=%u",
              state.loaded ? 1 : 0,
              static_cast<unsigned int>(state.byKey.size()),
              static_cast<unsigned int>(state.keyByLegacyId.size()),
              static_cast<unsigned int>(state.keyByResourceId.size()),
              static_cast<unsigned int>(state.keyByResourceSymbol.size()),
              static_cast<unsigned int>(state.keyByMenuBinding.size()));

        return state.loaded;
    }

    bool IsLoaded()
    {
        return Load();
    }

    const char* Get(mc2::czstring key)
    {
        if (!key || !key[0])
            return nullptr;

        Load();

        const std::map<std::string, StringRecord>::const_iterator it = State().byKey.find(key);
        if (it == State().byKey.end())
            return nullptr;

        const auto& [mapKey, stringRecord] = *it;
        return stringRecord.text.c_str();
    }

    namespace
    {
        /*
            Lookup order:
            1. Explicit LegacyTextAlias to the clean localization layer.
            2. Direct mc2res_legacy/String legacyId coverage.
            3. Missing marker at caller.
        */
        const char* LookupLegacy(const std::map<long, std::string>& aliases, const std::map<long, std::string>& ids, long legacyId)
        {
            const std::map<long, std::string>::const_iterator aliasIt = aliases.find(legacyId);
            if (aliasIt != aliases.end())
            {
                const auto& [aliasId, aliasKey] = *aliasIt;
                const char* aliasText = Get(aliasKey.c_str());
                if (aliasText && aliasText[0])
                    return aliasText;
            }

            const std::map<long, std::string>::const_iterator it = ids.find(legacyId);
            if (it == ids.end())
                return nullptr;

            const auto& [mapKey, stringKey] = *it;
            return Get(stringKey.c_str());
        }
    }

    const char* GetByLegacyId(long legacyId)
    {
        if (legacyId <= 0)
            return nullptr;

        Load();
        return LookupLegacy(State().keyByLegacyAlias, State().keyByLegacyId, legacyId);
    }

    const char* GetStockByLegacyId(long legacyId)
    {
        if (legacyId <= 0)
            return nullptr;

        Load();
        const CatalogState& state = State();
        return state.stockTaken ? LookupLegacy(state.stockAliasByLegacyId, state.stockKeyByLegacyId, legacyId) : GetByLegacyId(legacyId);
    }

    unsigned long long Revision()
    {
        return State().revision;
    }

    void LoadOverlayFile(mc2::czstring path)
    {
        if (path && path[0])
        {
            Load();
            LoadFile(path);
        }
    }

    const char* GetByResourceId(unsigned int resourceId)
    {
        Load();

        const std::map<unsigned int, std::string>::const_iterator it = State().keyByResourceId.find(resourceId);
        if (it != State().keyByResourceId.end())
        {
            const auto& [mapKey, stringKey] = *it;
            return Get(stringKey.c_str());
        }

        return GetByLegacyId(static_cast<long>(resourceId));
    }

    const char* GetByResourceSymbol(mc2::czstring resourceSymbol)
    {
        if (!resourceSymbol || !resourceSymbol[0])
            return nullptr;

        Load();

        const std::map<std::string, std::string>::const_iterator it = State().keyByResourceSymbol.find(resourceSymbol);
        if (it == State().keyByResourceSymbol.end())
            return nullptr;

        const auto& [mapKey, stringKey] = *it;
        return Get(stringKey.c_str());
    }

    const char* GetMenuText(mc2::czstring menuId, mc2::czstring commandId)
    {
        if (!menuId || !menuId[0] || !commandId || !commandId[0])
            return nullptr;

        Load();

        const std::string bindingKey = MenuBindingKey(menuId, commandId);
        const std::map<std::string, std::string>::const_iterator it = State().keyByMenuBinding.find(bindingKey);
        if (it == State().keyByMenuBinding.end())
            return nullptr;

        const auto& [mapKey, stringKey] = *it;
        return Get(stringKey.c_str());
    }

    const char* GetMenuTextByIds(unsigned int menuId, unsigned int commandId)
    {
        char menu[32] = {0};
        char command[32] = {0};
        mc2::fmt::to_buf(menu, "{:d}", mc2::fmt::as_u32(menuId));
        mc2::fmt::to_buf(command, "{:d}", mc2::fmt::as_u32(commandId));
        return GetMenuText(menu, command);
    }

    bool HasKey(mc2::czstring key)
    {
        return Get(key) != nullptr;
    }

    bool HasLegacyId(long legacyId)
    {
        return GetByLegacyId(legacyId) != nullptr;
    }

    bool HasResourceId(unsigned int resourceId)
    {
        return GetByResourceId(resourceId) != nullptr;
    }

    bool HasResourceSymbol(mc2::czstring resourceSymbol)
    {
        return GetByResourceSymbol(resourceSymbol) != nullptr;
    }

    bool HasMenuBinding(mc2::czstring menuId, mc2::czstring commandId)
    {
        return GetMenuText(menuId, commandId) != nullptr;
    }

    const char* MissingKey(mc2::czstring key)
    {
        return MissingMarker("key", key);
    }

    const char* MissingLegacyId(long legacyId)
    {
        return MissingMarkerNumber("legacyId", legacyId);
    }

    const char* MissingResourceId(unsigned int resourceId)
    {
        return MissingMarkerNumber("resourceId", static_cast<long>(resourceId));
    }

    const char* MissingResourceSymbol(mc2::czstring resourceSymbol)
    {
        return MissingMarker("resourceSymbol", resourceSymbol);
    }

    const char* MissingMenuBinding(mc2::czstring menuId, mc2::czstring commandId)
    {
        const std::string key = MenuBindingKey(menuId, commandId);
        return MissingMarker("menu", key.c_str());
    }

    void LoadModFits()
    {
        // Inject mod-overlay text FITs after InitModSearchPaths() has run.
        // MC2Strings::Load() fires lazily on the first string lookup, which may
        // happen before mod paths are indexed — at that point EnumerateModFitFiles
        // returns nothing because g_modIndex is empty.  This function bypasses the
        // one-shot attempted-guard and loads the mod FITs unconditionally.
        // Last-write-wins: z_cveg_aliases.fit (sorted after mechlopedia_legacy_aliases.fit)
        // will overwrite any wrong keyByLegacyAlias entries with correct cveg keys.
        const char* locale = ActiveLocale();
        struct Loader {
            static void cb(mc2::czstring absPath, void* /*ud*/) {
                LoadFile(absPath);
            }
        };
        std::string prefix = std::string("data/defs/text/") + locale + "/";
        EnumerateModFitFiles(prefix.c_str(), Loader::cb, nullptr);

        // Refresh loaded flag in case catalog was previously empty
        State().loaded = !State().byKey.empty();
        ++State().revision;

    }

    bool CompileMessage(std::string_view text, std::span<const ArgKind> signature, CompiledMessage& out, std::string* why)
    {
        out.pieces.clear();
        const auto fail = [&](const char* reason)
        {
            if (why)
                *why = reason;
            out.pieces.clear();
            return false;
        };
        const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };

        MessagePiece current;
        size_t used = 0;
        size_t i = 0;
        while (i < text.size())
        {
            const char c = text[i++];
            if (c != '%')
            {
                current.literal.push_back(c);
                continue;
            }
            if (i >= text.size())
                return fail("trailing percent");
            if (text[i] == '%')
            {
                current.literal.push_back('%');
                ++i;
                continue;
            }

            std::string flags;
            while (i < text.size() && (text[i] == '-' || text[i] == '+' || text[i] == ' ' || text[i] == '#' || text[i] == '0'))
                flags.push_back(text[i++]);
            std::string spec = flags;
            size_t digits = 0;
            while (i < text.size() && isDigit(text[i]))
            {
                spec.push_back(text[i++]);
                ++digits;
            }
            if (digits > 4)
                return fail("width too large");
            if (i < text.size() && text[i] == '.')
            {
                spec.push_back(text[i++]);
                digits = 0;
                while (i < text.size() && isDigit(text[i]))
                {
                    spec.push_back(text[i++]);
                    ++digits;
                }
                if (digits > 4)
                    return fail("precision too large");
            }

            IntWidth width = IntWidth::Int;
            if (i < text.size() && text[i] == 'h')
            {
                ++i;
                width = IntWidth::Short;
                if (i < text.size() && text[i] == 'h')
                {
                    ++i;
                    width = IntWidth::Char;
                }
            }
            else if (i < text.size() && text[i] == 'l')
            {
                ++i;
                width = IntWidth::Long;
                if (i < text.size() && text[i] == 'l')
                {
                    ++i;
                    width = IntWidth::LongLong;
                }
            }
            if (i >= text.size())
                return fail("truncated conversion");

            const char conv = text[i++];
            ArgKind need = ArgKind::Int;
            switch (conv)
            {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
                need = ArgKind::Int;
                break;
            case 's':
                need = ArgKind::Str;
                if (width != IntWidth::Int || flags.find_first_of("+ #0") != std::string::npos)
                    return fail("unsupported %s modifier");
                break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
                need = ArgKind::Float;
                if (width != IntWidth::Int && width != IntWidth::Long)   // %lf is a double, as in C
                    return fail("unsupported float modifier");
                width = IntWidth::Int;
                break;
            case 'n':
                return fail("%n is rejected");
            default:
                return fail("unsupported conversion");
            }

            if (used >= signature.size())
                return fail("more conversions than arguments");
            if (signature[used] != need)
                return fail("argument type mismatch");

            current.arg = static_cast<int>(used++);
            current.conv = conv;
            current.width = width;
            current.spec = spec;
            out.pieces.push_back(std::move(current));
            current = MessagePiece();
        }
        if (!current.literal.empty())
            out.pieces.push_back(std::move(current));
        return true;
    }

    namespace
    {
        template <class T>
        void AppendPrintf(std::string& out, const char* format, T value)
        {
            char stackBuf[128];   // not "small": rpcndr.h defines that as a macro
            const int needed = std::snprintf(stackBuf, sizeof(stackBuf), format, value);
            if (needed < 0)
                return;
            if (static_cast<size_t>(needed) < sizeof(stackBuf))
            {
                out.append(stackBuf, static_cast<size_t>(needed));
                return;
            }
            std::string big(static_cast<size_t>(needed) + 1, '\0');
            std::snprintf(big.data(), big.size(), format, value);
            out.append(big.data(), static_cast<size_t>(needed));
        }

        const char* LengthText(IntWidth width)
        {
            switch (width)
            {
            case IntWidth::Char: return "hh";
            case IntWidth::Short: return "h";
            case IntWidth::Long: return "l";
            case IntWidth::LongLong: return "ll";
            default: return "";
            }
        }

        void AppendInt(std::string& out, const MessagePiece& piece, long long value)
        {
            const std::string format = "%" + piece.spec + LengthText(piece.width) + piece.conv;
            const bool isSigned = piece.conv == 'd' || piece.conv == 'i';
            // Cast to the type the conversion reads, so the result matches a correctly typed sprintf on this platform.
            switch (piece.width)
            {
            case IntWidth::Char:
                isSigned ? AppendPrintf(out, format.c_str(), static_cast<int>(static_cast<signed char>(value)))
                         : AppendPrintf(out, format.c_str(), static_cast<unsigned int>(static_cast<unsigned char>(value)));
                break;
            case IntWidth::Short:
                isSigned ? AppendPrintf(out, format.c_str(), static_cast<int>(static_cast<short>(value)))
                         : AppendPrintf(out, format.c_str(), static_cast<unsigned int>(static_cast<unsigned short>(value)));
                break;
            case IntWidth::Long:
                isSigned ? AppendPrintf(out, format.c_str(), static_cast<long>(value))
                         : AppendPrintf(out, format.c_str(), static_cast<unsigned long>(value));
                break;
            case IntWidth::LongLong:
                isSigned ? AppendPrintf(out, format.c_str(), value)
                         : AppendPrintf(out, format.c_str(), static_cast<unsigned long long>(value));
                break;
            default:
                isSigned ? AppendPrintf(out, format.c_str(), static_cast<int>(value))
                         : AppendPrintf(out, format.c_str(), static_cast<unsigned int>(value));
                break;
            }
        }
    }

    std::string FormatMessage(const CompiledMessage& message, std::span<const MessageArg> args)
    {
        std::string out;
        for (const MessagePiece& piece : message.pieces)
        {
            out += piece.literal;
            if (piece.arg < 0 || static_cast<size_t>(piece.arg) >= args.size())
                continue;
            const MessageArg& arg = args[static_cast<size_t>(piece.arg)];
            switch (piece.conv)
            {
            case 's':
                if (arg.kind == ArgKind::Str)
                    AppendPrintf(out, ("%" + piece.spec + "s").c_str(), std::string(arg.s).c_str());
                break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
                if (arg.kind == ArgKind::Float)
                    AppendPrintf(out, ("%" + piece.spec + piece.conv).c_str(), arg.d);
                break;
            default:
                if (arg.kind == ArgKind::Int)
                    AppendInt(out, piece, arg.i);
                break;
            }
        }
        return out;
    }

    namespace
    {
        struct CachedMessage
        {
            unsigned long long revision = 0;
            std::vector<ArgKind> signature;
            CompiledMessage message;
        };

        std::mutex g_messageCacheLock;
        std::map<long, CachedMessage> g_messageCache;

        CompiledMessage LiteralMessage(std::string_view text)
        {
            CompiledMessage literal;
            MessagePiece piece;
            piece.literal = std::string(text);
            literal.pieces.push_back(std::move(piece));
            return literal;
        }

        // The template for the id as the catalog holds it now; a rejected override falls back to the stock text.
        CompiledMessage BuildLegacyMessage(long legacyId, std::span<const ArgKind> signature)
        {
            CompiledMessage compiled;
            const char* current = GetByLegacyId(legacyId);
            if (!current || !current[0])
            {
                mc2::fmt::print_err("[MC2Strings] message id={:d} has no template\n", mc2::fmt::as_i32(legacyId));
                return compiled;
            }

            std::string why;
            if (CompileMessage(current, signature, compiled, &why))
                return compiled;

            const char* stock = GetStockByLegacyId(legacyId);
            const bool haveStock = stock && stock[0] && std::strcmp(stock, current) != 0;
            if (haveStock && CompileMessage(stock, signature, compiled, nullptr))
            {
                mc2::fmt::print_err("[MC2Strings] message id={:d} override rejected ({}); using the stock template\n",
                    mc2::fmt::as_i32(legacyId), mc2::fmt::safe(why.c_str()));
                return compiled;
            }
            mc2::fmt::print_err("[MC2Strings] message id={:d} template rejected ({}); printing it as plain text\n",
                mc2::fmt::as_i32(legacyId), mc2::fmt::safe(why.c_str()));
            return LiteralMessage(current);
        }
    }

    std::string FormatLegacyMessage(long legacyId, std::span<const ArgKind> signature, std::span<const MessageArg> args)
    {
        Load();
        const unsigned long long revision = Revision();

        std::lock_guard<std::mutex> lock(g_messageCacheLock);
        CachedMessage& entry = g_messageCache[legacyId];
        if (entry.revision != revision || !std::equal(signature.begin(), signature.end(), entry.signature.begin(), entry.signature.end()))
        {
            entry.message = BuildLegacyMessage(legacyId, signature);
            entry.signature.assign(signature.begin(), signature.end());
            entry.revision = revision;
        }
        return FormatMessage(entry.message, args);
    }
}
