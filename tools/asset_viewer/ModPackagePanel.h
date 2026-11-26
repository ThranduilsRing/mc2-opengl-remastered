// tools/asset_viewer/ModPackagePanel.h
// Mod Package: browse an installed mod (mods/<id>/): manifest, file list, model overrides, validation. Read-only.
// Validation reuses the Mod Workbench record rules (OverrideManifest.h ValidateRecordRules) per models.json record,
// plus on-disk source checks and dependency presence.
#pragma once
#include "WorkbenchWarning.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct ModPackageInfo {
    std::string dir, id;
    std::vector<std::pair<std::string, std::string>> manifest;   // key -> display value
    struct FileRow { std::string rel; uint64_t size = 0; };
    std::vector<FileRow> files;
    uint64_t totalBytes = 0;
    struct OverrideRow {
        std::string key;          // "tree:tc1_1"
        std::string source;
        int lods = 0;
        std::vector<Warning> warnings;
    };
    std::vector<OverrideRow> overrides;
    std::vector<Warning> problems;   // manifest-level (dependency missing, mod.json unreadable, ...)
    int blocks() const;
    int warns() const;
};

class ModPackagePanel {
public:
    void drawBrowser();
    void drawDetails();
    void select(const std::string& id);
    // Pure: scan <deployDir>/mods/* (dirs holding a mod.json) -> ids.
    static std::vector<std::string> ListMods(const std::string& deployDir);
    static ModPackageInfo Read(const std::string& deployDir, const std::string& id);
private:
    bool built_ = false;
    std::vector<std::string> ids_;
    int selected_ = -1;
    ModPackageInfo info_;
};
