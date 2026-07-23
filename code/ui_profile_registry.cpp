// code/ui_profile_registry.cpp  (engine ONLY — never in mc2_tests)
// UI-PROFILE-1 (Phase-2 slice 6): optional additive-merge UIProfile registry.
// inifile.h drags file I/O (forbidden in the doctest target), so this engine-only
// TU owns the impure load; the pure facet + override decision are in ui_profile.h.
#include "ui_profile_registry.h"
#include "ui_profile.h"
#include "unitprofile.h"      // mc2UnitProfileDataEnabled()
#include "inifile.h"          // FitIniFile, seekBlock, readIdString/readIdLong
#include <map>
#include <string>
#include <cctype>

namespace {

// Lowercase a filename key so lookup is case-insensitive (chassis filenames are
// stored lowercased by LogisticsChassis::init).
std::string lowerKey(const char* s) {
    std::string k;
    if (s) for (const char* p = s; *p; ++p) k += (char)std::tolower((unsigned char)*p);
    return k;
}

// The optional data source. A stock install ships no such file, so the map stays
// empty and every lookup returns null -> legacy accessors -> stock parity.
const char* kUIProfileDataFile = "data/defs/ui/ui_profiles.fit";

// Each block's id is a chassis filename stem; keys/values map onto UIProfile.
void readBlockInto(FitIniFile& fit, UIProfile& p) {
    char buf[UI_ASSET_NAME_CAP] = {0};
    if (fit.readIdString("Icon", buf, sizeof(buf)) == NO_ERR)       p.icon       = UIAssetName(buf);
    if (fit.readIdString("Portrait", buf, sizeof(buf)) == NO_ERR)   p.portrait   = UIAssetName(buf);
    if (fit.readIdString("Minimap", buf, sizeof(buf)) == NO_ERR)    p.minimap    = UIAssetName(buf);
    if (fit.readIdString("Silhouette", buf, sizeof(buf)) == NO_ERR) p.silhouette = UIAssetName(buf);
    long id = UI_STRING_ID_UNSET;
    if (fit.readIdLong("NameID", id) == NO_ERR)         p.nameStringId = id;
    id = UI_STRING_ID_UNSET;
    if (fit.readIdLong("EncyclopediaID", id) == NO_ERR) p.encyclopediaStringId = id;
}

// Strip directory + extension from a chassis filename, leaving the stem that a
// UIProfile block is keyed on (e.g. "objects/mech_hbk4g.csv" -> "mech_hbk4g").
std::string chassisStem(const char* fileName) {
    std::string s = lowerKey(fileName);
    const size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    return s;
}

// Lazily-resolved registry. The optional data .fit is opened ONCE; each chassis
// UIProfile is then resolved on demand via a direct seekBlock(stem) and cached
// (present-or-absent, so a miss is never re-sought). Absent file / gate OFF ->
// srcOk=false -> every lookup returns null -> legacy accessors -> stock parity.
struct UIProfileRegistry {
    FitIniFile fit;
    bool opened = false;
    bool srcOk  = false;
    std::map<std::string, UIProfile> cache;   // stem -> resolved profile
    std::map<std::string, bool>      have;    // stem -> block present?

    void openOnce() {
        if (opened) return;
        opened = true;
        if (!mc2UnitProfileDataEnabled()) return;   // gate OFF -> no source
        if (!fileExists(kUIProfileDataFile)) return; // stock: no such file
        srcOk = (fit.open(kUIProfileDataFile) == NO_ERR);
    }

    const UIProfile* resolve(const std::string& stem) {
        openOnce();
        if (!srcOk) return nullptr;
        auto hit = have.find(stem);
        if (hit != have.end())
            return hit->second ? &cache[stem] : nullptr;
        // First time: seek the chassis-stem block. Absent -> record miss.
        if (fit.seekBlock(stem.c_str()) != NO_ERR) {
            have[stem] = false;
            return nullptr;
        }
        UIProfile p;
        readBlockInto(fit, p);
        cache[stem] = p;
        have[stem]  = true;
        return &cache[stem];
    }
};

UIProfileRegistry& registry() {
    static UIProfileRegistry reg;
    return reg;
}

} // namespace

const UIProfile* uiProfileForChassis(const char* chassisFileName) {
    // Gate OFF -> always null -> legacy (parity). Cheap early-out before any open.
    if (!mc2UnitProfileDataEnabled()) return nullptr;
    if (!chassisFileName || !chassisFileName[0]) return nullptr;
    return registry().resolve(chassisStem(chassisFileName));
}
