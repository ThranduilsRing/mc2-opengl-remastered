// anim_override_registry.cpp — ANIM-OVERRIDE-MVP-1.
// Mirrors model_override_registry.cpp. Permitted to include nlohmann/json
// (allowlisted in scripts/check-json-isolation.sh).
#include "anim_override_registry.h"
#include <nlohmann/json.hpp>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <utility>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "mc2fmt.h"

using nlohmann::json;

static void animLogDrop(std::string_view why, std::string_view key) {
    mc2::fmt::print_err("[ANIMOVERRIDE] dropped '{}': {}\n", key.substr(0, key.find('\0')), why.substr(0, why.find('\0')));
    fflush(stderr);
}

static std::string animNormalizeKey(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    std::string out(s.substr(b, e - b));
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

static std::string animTrim(std::string_view s) {
    size_t b = 0, en = s.size();
    while (b < en && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (en > b && std::isspace(static_cast<unsigned char>(s[en - 1]))) --en;
    return std::string(s.substr(b, en - b));
}

// Safe relative .ase/.agl source. Rejects absolute paths, drive letters, and any
// ".." (traversal). Returns the basename WITHOUT extension in `baseOut`.
static bool animSafeSource(std::string_view s, std::string& baseOut) {
    if (s.empty()) return false;
    if (s[0] == '/' || s[0] == '\\') return false;
    if (s.size() >= 2 && s[1] == ':') return false;
    if (s.find("..") != std::string::npos) return false;
    std::string low(s); for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool ase = low.size() >= 4 && low.compare(low.size() - 4, 4, ".ase") == 0;
    const bool agl = low.size() >= 4 && low.compare(low.size() - 4, 4, ".agl") == 0;
    if (!ase && !agl) return false;
    baseOut = s.substr(0, s.size() - 4);  // strip the 4-char extension
    if (baseOut.empty()) return false;
    return true;
}

static std::vector<AnimOverrideRecord> animParseManifest(
        const std::string& manifestPath, std::string_view dir) {
    std::vector<AnimOverrideRecord> out;

    std::ifstream in(manifestPath.c_str());
    if (!in.is_open()) return out;
    json root;
    try { in >> root; }
    catch (const std::exception& e) {
        mc2::fmt::print_err("[ANIMOVERRIDE] parse error in {}: {}\n", manifestPath.c_str(), mc2::fmt::safe(e.what()));
        return out;
    }
    if (!root.is_object() || !root.contains("overrides") || !root["overrides"].is_array()) {
        mc2::fmt::print_err("[ANIMOVERRIDE] {}: missing 'overrides' array\n", manifestPath.c_str());
        return out;
    }

    for (const auto& e : root["overrides"]) {
        try {
        if (!e.is_object()) { animLogDrop("entry is not an object", "<non-object>"); continue; }
        std::string replaces = e.value("replaces", std::string());
        std::string gestureRaw = e.value("gesture", std::string());
        const std::string key = (replaces.empty() ? "<no-replaces>" : replaces)
                                + "/" + (gestureRaw.empty() ? "<no-gesture>" : gestureRaw);

        if (e.value("type", std::string()) != "anim")     { animLogDrop("type!=anim", key); continue; }
        if (e.value("fallback", std::string()) != "stock"){ animLogDrop("fallback!=stock", key); continue; }

        // replaces == "mech:<name>"
        size_t colon = replaces.find(':');
        if (colon == std::string::npos) { animLogDrop("replaces not '<class>:<name>'", key); continue; }
        std::string cls  = animNormalizeKey(replaces.substr(0, colon));
        std::string name = animNormalizeKey(replaces.substr(colon + 1));
        if (cls != "mech")  { animLogDrop("class not 'mech'", key); continue; }
        if (name.empty())   { animLogDrop("empty mech name in replaces", key); continue; }

        std::string gesture = animNormalizeKey(gestureRaw);
        if (gesture.empty()) { animLogDrop("empty gesture", key); continue; }

        std::string source = animTrim(e.value("source", std::string()));
        std::string base;
        if (!animSafeSource(source, base)) { animLogDrop("unsafe/non-.ase/.agl source path", key); continue; }

        AnimOverrideRecord rec;
        rec.mechName    = name;
        rec.gesture     = gesture;
        rec.sourceBase  = base;
        rec.manifestDir = dir;
        out.push_back(std::move(rec));
        } catch (const std::exception& ex) {
            animLogDrop(ex.what(), "<entry>"); continue;
        }
    }
    return out;
}

int AnimOverrideRegistry::loadFromFile(const std::string& manifestPath,
                                       std::string_view manifestDir) {
    records_.clear();
    manifestDir_ = manifestDir;

    auto parsed = animParseManifest(manifestPath, manifestDir);
    for (auto& rec : parsed) {
        bool dup = false;
        for (const auto& r : records_)
            if (r.mechName == rec.mechName && r.gesture == rec.gesture) { dup = true; break; }
        if (dup) { animLogDrop("duplicate key (first entry wins)", rec.mechName + ":" + rec.gesture); continue; }
        records_.push_back(std::move(rec));
    }
    return static_cast<int>(records_.size());
}

int AnimOverrideRegistry::mergeFromFile(const std::string& manifestPath,
                                        std::string_view manifestDir) {
    auto parsed = animParseManifest(manifestPath, manifestDir);
    int merged = 0;
    for (auto& rec : parsed) {
        bool replaced = false;
        for (auto& r : records_) {
            if (r.mechName == rec.mechName && r.gesture == rec.gesture) {
                r = std::move(rec); replaced = true; ++merged; break;
            }
        }
        if (!replaced) { records_.push_back(std::move(rec)); ++merged; }
    }
    return merged;
}

const AnimOverrideRecord* AnimOverrideRegistry::resolve(
        const char* mechName, const char* gesture) const {
    if (!mechName || !gesture) return nullptr;
    if (records_.empty()) return nullptr;  // fast no-op for the common (no-mod) case
    const std::string m = animNormalizeKey(mechName);
    const std::string g = animNormalizeKey(gesture);
    for (const auto& r : records_) {
        if (r.mechName == m && r.gesture == g) return &r;
    }
    return nullptr;
}

AnimOverrideRegistry& AnimOverrideRegistry::instance() {
    static AnimOverrideRegistry g;
    static std::once_flag once;
    std::call_once(once, []{
        int base = g.loadFromFile("data/anim_overrides/anims.json", "data/anim_overrides");
        if (base > 0) {
            mc2::fmt::print_err("[ANIMOVERRIDE] base registry: {:d} override(s)\n", mc2::fmt::as_i32(base));
            fflush(stderr);
        }
        const std::string activeModStr = mc2::gate::str<"MC2_ACTIVE_MOD">();
        const char* activeMod = activeModStr.c_str();
        if (activeMod[0]) {
            std::string modManifest = std::string("mods/") + activeMod
                                      + "/data/anim_overrides/anims.json";
            std::string modDir      = std::string("mods/") + activeMod
                                      + "/data/anim_overrides";
            std::ifstream probe(modManifest.c_str());
            if (probe.is_open()) {
                probe.close();
                int n = g.mergeFromFile(modManifest, modDir);
                mc2::fmt::print_err("[ANIMOVERRIDE] mod '{}': {:d} override(s) merged (total {:d})\n", mc2::fmt::safe(activeMod), mc2::fmt::as_i32(n), mc2::fmt::as_i32(g.count()));
                fflush(stderr);
            }
        }
    });
    return g;
}
