// tools/asset_viewer/AppearanceRoster.h
// S5: enumerable roster of valid override appearance keys.
// Source = unique FileName= values across <deploy>/data/tgl/*.ini
// (the same string the engine uses as the override lookup key; see bdactor.cpp).
// Read-only, cached, refreshable.
#pragma once
#include <string>
#include <string_view>
#include <vector>

class AppearanceRoster {
public:
    void load(std::string_view deployDir);
    void refresh(std::string_view deployDir);     // = clear + load
    const std::vector<std::string>& names() const { return names_; }
    int scannedFileCount() const { return scannedFiles_; }
    bool contains(std::string_view name) const;   // case-insensitive

private:
    std::vector<std::string> names_;
    std::string loadedDir_;
    int  scannedFiles_ = 0;
    bool loaded_ = false;
};
