// Offline ObjectTypeManager sweep. Run from an isolated, overlaid mod install.
#include "objtype.h"
#include "inifile.h"
#include "fastfile.h"
#include "paths.h"
#include "utils/mc2log.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <io.h>
#include <string>

int main(int argc, char** argv)
{
    using nlohmann::json;
    json report = {{"loaded", 0}, {"failures", json::array()}};
    // Engine STOP/FATAL is soft in RelWithDebInfo; capture it, including legacy
    // writes, rather than treating a returned stub object as successful loading.
    FILE* capture = std::tmpfile();
    if (!capture) { std::puts("{\"loaded\":0,\"failures\":[\"log capture failed\"]}"); return 1; }
    const int output = _dup(_fileno(stdout));
    const int errors = _dup(_fileno(stderr));
    std::fflush(stdout);
    _dup2(_fileno(capture), _fileno(stdout));
    _dup2(_fileno(capture), _fileno(stderr));
    mc2::log::setSpec("*:info");
    try {
        if (argc != 2) throw std::runtime_error("usage: mc2_mod_load <install>");
        std::filesystem::current_path(argv[1]);
        FitIniFile config;
        if (config.open("system.cfg") != NO_ERR) throw std::runtime_error("system.cfg open failed");
        if (config.seekBlock("systemPaths") == NO_ERR)
            config.readIdString("objectPath", objectPath, 79);
        if (config.seekBlock("FastFiles") == NO_ERR) {
            if (config.readIdLong("NumFastFiles", maxFastFiles) != NO_ERR || maxFastFiles < 0 || maxFastFiles > 256)
                throw std::runtime_error("invalid FastFiles count");
            fastFiles = static_cast<FastFile**>(std::calloc(maxFastFiles, sizeof(FastFile*)));
            for (long i = 0; i < maxFastFiles; ++i) {
                char name[256];
                const auto key = "File" + std::to_string(i);
                if (config.readIdString(key.c_str(), name, 255) != NO_ERR || !FastFileInit(name) ||
                    !fastFiles[numFastFiles - 1]->isOpen())
                    throw std::runtime_error("FastFile open failed: " + key);
            }
        }
        config.close();
        InitModSearchPaths("mods");
        ObjectTypeManager manager;
        if (manager.init("object2", 0, 0) != NO_ERR) throw std::runtime_error("Object2.pak open failed");
        auto* table = ObjectTypeManager::objectFile;
        for (int id = 1; id < table->getNumPackets(); ++id) {
            if (table->seekPacket(id) != NO_ERR) {
                report["failures"].push_back({{"id", id}, {"error", "packet seek failed"}});
                continue;
            }
            if (table->getPacketSize() == 0) continue; // unused ordinal
            FitIniFile definition;
            long kind = -1;
            const bool valid = definition.open(table, table->getPacketSize()) == NO_ERR &&
                definition.seekBlock("ObjectClass") == NO_ERR &&
                definition.readIdLong("ObjectTypeNum", kind) == NO_ERR;
            definition.close();
            if (!valid) {
                report["failures"].push_back({{"id", id}, {"error", "invalid ObjectClass"}});
                continue;
            }
            if (kind != BATTLEMECH_TYPE && kind != VEHICLE_TYPE && kind != BUILDING_TYPE && kind != TREEBUILDING_TYPE)
                continue;
            std::fprintf(stderr, "\n[MOD_LOAD] id=%d\n", id);
            const auto progress = "[MOD_LOAD] id=" + std::to_string(id) + "\n";
            _write(errors, progress.data(), static_cast<unsigned int>(progress.size()));
            if (!manager.load(id) && !manager.get(id, false))
                report["failures"].push_back({{"id", id}, {"error", "type load returned null"}});
            else report["loaded"] = report["loaded"].get<int>() + 1;
        }
        if (report["loaded"] == 0) report["failures"].push_back("no object types loaded");
    } catch (const std::exception& e) {
        report["failures"].push_back(e.what());
    }
    FastFileFini();
    std::fflush(stdout);
    std::fflush(stderr);
    std::rewind(capture);
    std::string log;
    char bytes[4096];
    while (const auto count = std::fread(bytes, 1, sizeof(bytes), capture)) log.append(bytes, count);
    // Preserve context and every occurrence for the Python pre-check report.
    report["log"] = log;
    for (const char* marker : {"FATAL", "STOP", "[CRASH]", "ObjectTypeManager.", "[OBJTYPE_INIT_FAILED]"})
        if (log.find(marker) != std::string::npos) report["failures"].push_back(marker);
    _dup2(output, _fileno(stdout));
    _dup2(errors, _fileno(stderr));
    _close(output);
    _close(errors);
    std::fclose(capture);
    std::puts(report.dump().c_str());
    return report["failures"].empty() ? 0 : 1;
}
