// tools/asset_viewer/AppearanceRoster.cpp
#include "AppearanceRoster.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace fs = std::filesystem;

static std::string trimmed(std::string s) {
    auto notspace = [](unsigned char c){ return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}
static std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void AppearanceRoster::load(std::string_view deployDir) {
    if (loaded_ && loadedDir_ == deployDir) return;
    names_.clear();
    scannedFiles_ = 0;
    loadedDir_ = deployDir;
    loaded_ = true;

    fs::path tglDir = fs::path(deployDir) / "data" / "tgl";
    std::error_code ec;
    if (!fs::is_directory(tglDir, ec)) return;

    // One .ini contributes multiple FileName values by design (FileName0=base,
    // FileName1=LOD1, FileName=damaged, etc.). The roster is an inclusive superset
    // of every shape name reachable via the engine's override key (bdactor.cpp:288-340).
    std::unordered_set<std::string> seenLower;
    for (auto& de : fs::directory_iterator(tglDir, ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        if (lower(de.path().extension().string()) != ".ini") continue;
        ++scannedFiles_;
        std::ifstream f(de.path());
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = lower(trimmed(line.substr(0, eq)));
            // FIT-ini typed keys look like `st FileName0` — strip the `st ` prefix.
            if (key.rfind("st ", 0) == 0) key = trimmed(key.substr(3));
            // Accept "filename" or "filename<N>" (LOD/damage shape names). The roster
            // is an inclusive superset of the engine override key (bdactor.cpp:288-340);
            // the modder picks the right base name.
            if (key.compare(0, 8, "filename") != 0) continue;
            bool ok = true;
            for (size_t k = 8; k < key.size(); ++k) if (!std::isdigit((unsigned char)key[k])) { ok = false; break; }
            if (!ok) continue;
            std::string val = trimmed(line.substr(eq + 1));
            if (val.empty()) continue;
            std::string lv = lower(val);
            if (seenLower.count(lv)) continue;
            seenLower.insert(lv);
            names_.push_back(val);
        }
    }
    std::sort(names_.begin(), names_.end(),
              [](const std::string& a, const std::string& b){ return lower(a) < lower(b); });
}

void AppearanceRoster::refresh(std::string_view deployDir) {
    loaded_ = false;
    loadedDir_.clear();
    load(deployDir);
}

bool AppearanceRoster::contains(std::string_view name) const {
    std::string ln = lower(std::string(name));
    for (const auto& n : names_) if (lower(n) == ln) return true;
    return false;
}
