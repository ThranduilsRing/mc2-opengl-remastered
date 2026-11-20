// tools/asset_viewer/VfxPanel.h
// VFX: effect definitions from data/effects/*.fx (and mods/*/data/effects/*.fx), decoded by the native catalog
// reader (mclib/fx). Parameters, authored curves with a small plot, and the effect's texture thumbnail. No simulation.
#pragma once
#include "fx/fx_catalog.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class VfxPanel {
public:
    ~VfxPanel();
    void drawBrowser();
    void drawDetails();
    void select(const std::string& name);
    struct Entry { std::string file; int fileIdx = 0; int effectIdx = 0; };
    // Pure loader (also used by the smoke test): decodes every *.fx under <deployDir>/data/effects and mods/*/data/effects.
    // Returns the number of effects; per-file errors are appended to `errors`.
    int Load(const std::string& deployDir, std::vector<std::string>& errors);
    const std::vector<Entry>& entries() const { return entries_; }
    const mc2::fx::EffectRecord& record(const Entry& e) const { return catalogs_[e.fileIdx]->effects()[e.effectIdx]; }
    static const char* ClassName(uint32_t classId);
private:
    uint32_t thumb(const std::string& texName);
    bool built_ = false;
    std::vector<std::unique_ptr<mc2::fx::Catalog>> catalogs_;
    std::vector<Entry> entries_;
    std::vector<std::string> errors_;
    std::map<std::string, uint32_t> thumbs_;   // texture name -> toolvk texture (0 = not found)
    int selected_ = -1;
    char filter_[128] = {0};
};
