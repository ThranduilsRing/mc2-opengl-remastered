// model_override_registry.cpp — MODEL-OVERRIDE-MVP-1 Slice 1.
// ONLY engine TU permitted to include nlohmann/json (scripts/check-json-isolation.sh).
#include "model_override_registry.h"
#include <nlohmann/json.hpp>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <utility>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "mc2fmt.h"

using nlohmann::json;

static void logDrop(const char* why, const std::string& key) {
    mc2::fmt::print_err("[MODOVERRIDE] dropped '{}': {}\n", key.c_str(), mc2::fmt::safe(why));
    fflush(stderr);
}

static std::string normalizeKey(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    std::string out(s.substr(b, e - b));
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

static bool isSafeSource(std::string_view s) {
    if (s.empty()) return false;
    if (s[0] == '/' || s[0] == '\\') return false;
    if (s.size() >= 2 && s[1] == ':') return false;
    // conservative: rejects any "..", including the rare literal "a..b.glb"; traversal safety > that edge case.
    if (s.find("..") != std::string::npos) return false;
    std::string low(s); for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool glb  = low.size() >= 4 && low.compare(low.size() - 4, 4, ".glb")  == 0;
    const bool gltf = low.size() >= 5 && low.compare(low.size() - 5, 5, ".gltf") == 0;
    return glb || gltf;
}

static std::string trimSource(std::string_view s) {
    size_t b = 0, en = s.size();
    while (b < en && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (en > b && std::isspace(static_cast<unsigned char>(s[en - 1]))) --en;
    return std::string(s.substr(b, en - b));
}

// Parse manifestPath and return all valid records stamped with dir.
// No duplicate-key check here — callers apply their own policy.
static std::vector<ModelOverrideRecord> parseManifest(
        const std::string& manifestPath, std::string_view dir) {
    std::vector<ModelOverrideRecord> out;

    std::ifstream in(manifestPath.c_str());
    if (!in.is_open()) return out;
    json root;
    try { in >> root; }
    catch (const std::exception& e) {
        mc2::fmt::print_err("[MODOVERRIDE] parse error in {}: {}\n", manifestPath.c_str(), mc2::fmt::safe(e.what()));
        return out;
    }
    if (!root.is_object() || !root.contains("overrides") || !root["overrides"].is_array()) {
        mc2::fmt::print_err("[MODOVERRIDE] {}: missing 'overrides' array\n", manifestPath.c_str());
        return out;
    }

    for (const auto& e : root["overrides"]) {
        try {
        if (!e.is_object()) { logDrop("entry is not an object", "<non-object>"); continue; }
        std::string replaces = e.value("replaces", std::string());
        const std::string key = replaces.empty() ? "<no-replaces>" : replaces;

        if (e.value("type", std::string()) != "model") { logDrop("type!=model", key); continue; }
        if (!e.value("renderOnly", false))             { logDrop("renderOnly!=true", key); continue; }
        if (e.value("fallback", std::string()) != "stock") { logDrop("fallback!=stock", key); continue; }

        const float scale = e.value("scale", 1.0f);
        if (scale != 1.0f) { logDrop("scale!=1.0 (MVP requires 1.0)", key); continue; }

        // split on FIRST ':'; MC2 appearance names contain no ':' so a trailing ':' in name is not expected.
        size_t colon = replaces.find(':');
        if (colon == std::string::npos) { logDrop("replaces not '<class>:<name>'", key); continue; }
        std::string cls  = normalizeKey(replaces.substr(0, colon));
        std::string name = normalizeKey(replaces.substr(colon + 1));
        if (cls.empty() || name.empty()) { logDrop("empty class or name in replaces", key); continue; }

        if (cls != "staticprop" && cls != "tree") { logDrop("class not staticProp|tree", key); continue; }
        if (e.contains("class")) {
            std::string clsField = normalizeKey(e.value("class", std::string()));
            if (clsField != cls) { logDrop("class field disagrees with replaces", key); continue; }
        }

        std::string source = trimSource(e.value("source", std::string()));
        if (!isSafeSource(source)) { logDrop("unsafe/non-glTF source path", key); continue; }

        ModelOverrideRecord rec;
        rec.overrideClass  = cls;
        rec.appearanceName = name;
        rec.sourceRelPath  = source;
        rec.manifestDir    = dir;
        rec.scale          = scale;

        // TREE-OVERRIDE-LOD-MVP-1 Task 3/4: optional lower-detail LOD chain.
        // Minimal-but-safe parse: each entry must be an object with int `lod`>=1
        // and a safe `source`; entries kept in ascending-lod order; a non-
        // ascending or duplicate lod, or an unsafe/missing source, drops THAT
        // LOD entry (logged) but keeps the record's LOD0. (Full validation +
        // unit tests are Task 4.)
        if (e.contains("lods") && e["lods"].is_array()) {
            int lastLod = 0;  // LOD0 implied by `source`
            for (const auto& le : e["lods"]) {
                if (!le.is_object()) { logDrop("lods entry not an object", key); continue; }
                if (!le.contains("lod") || !le["lod"].is_number_integer()) {
                    logDrop("lods entry missing integer 'lod'", key); continue;
                }
                const int lod = le["lod"].get<int>();
                if (lod <= lastLod) { logDrop("lods 'lod' not strictly ascending (LOD0=source)", key); continue; }

                std::string lsrc = trimSource(le.value("source", std::string()));
                if (!isSafeSource(lsrc)) { logDrop("lods entry unsafe/non-glTF source", key); continue; }

                ModelOverrideLod l;
                l.lod          = lod;
                l.sourceRelPath = lsrc;
                l.distance     = le.value("distance", 0.0f);
                rec.lods.push_back(std::move(l));
                lastLod = lod;
            }
        }

        out.push_back(std::move(rec));
        } catch (const std::exception& ex) {
            logDrop(ex.what(), "<entry>"); continue;
        }
    }
    return out;
}

int ModelOverrideRegistry::loadFromFile(const std::string& manifestPath,
                                        std::string_view manifestDir) {
    records_.clear();
    manifestDir_ = manifestDir;

    auto parsed = parseManifest(manifestPath, manifestDir);
    // Dup check: first entry wins within a single file.
    for (auto& rec : parsed) {
        bool dup = false;
        for (const auto& r : records_)
            if (r.overrideClass == rec.overrideClass && r.appearanceName == rec.appearanceName)
                { dup = true; break; }
        if (dup) { logDrop("duplicate key (first entry wins)", rec.overrideClass + ":" + rec.appearanceName); continue; }
        records_.push_back(std::move(rec));
    }
    return static_cast<int>(records_.size());
}

int ModelOverrideRegistry::mergeFromFile(const std::string& manifestPath,
                                         std::string_view manifestDir) {
    auto parsed = parseManifest(manifestPath, manifestDir);
    int merged = 0;
    for (auto& rec : parsed) {
        // Mod wins on dup key: replace existing base record if present.
        bool replaced = false;
        for (auto& r : records_) {
            if (r.overrideClass == rec.overrideClass && r.appearanceName == rec.appearanceName) {
                r = std::move(rec);
                replaced = true;
                ++merged;
                break;
            }
        }
        if (!replaced) {
            records_.push_back(std::move(rec));
            ++merged;
        }
    }
    return merged;
}

const ModelOverrideRecord* ModelOverrideRegistry::resolve(
        const char* overrideClass, const char* appearanceName) const {
    if (!overrideClass || !appearanceName) return nullptr;
    const std::string cls  = normalizeKey(overrideClass);
    const std::string name = normalizeKey(appearanceName);
    for (const auto& r : records_) {
        if (r.overrideClass == cls && r.appearanceName == name) return &r;
    }
    return nullptr;
}

ModelOverrideRegistry& ModelOverrideRegistry::instance() {
    static ModelOverrideRegistry g;
    static std::once_flag once;
    std::call_once(once, []{
        // Always load base registry first.
        int base = g.loadFromFile("data/model_overrides/models.json", "data/model_overrides");
        mc2::fmt::print_err("[MODOVERRIDE] base registry: {:d} override(s)\n", mc2::fmt::as_i32(base));
        fflush(stderr);

        // Additive mod merge: mod entries WIN on dup key (mod overrides base).
        // Source paths in the mod's models.json must be relative to the mod's
        // manifestDir (mods/<id>/data/model_overrides), not full data-relative.
        // GLB load uses ImportGeometryFromFile (raw Assimp, not File::open),
        // so per-record manifestDir is the only way to route to the mod's files.
        const std::string activeModStr = mc2::gate::str<"MC2_ACTIVE_MOD">();
        const char* activeMod = activeModStr.c_str();
        if (activeMod[0]) {
            std::string modManifest = std::string("mods/") + activeMod
                                      + "/data/model_overrides/models.json";
            std::string modDir      = std::string("mods/") + activeMod
                                      + "/data/model_overrides";
            std::ifstream probe(modManifest.c_str());
            if (probe.is_open()) {
                probe.close();
                int n = g.mergeFromFile(modManifest, modDir);
                mc2::fmt::print_err("[MODOVERRIDE] mod '{}': {:d} override(s) merged (total {:d})\n", mc2::fmt::safe(activeMod), mc2::fmt::as_i32(n), mc2::fmt::as_i32(g.count()));
                fflush(stderr);
            } else {
                mc2::fmt::print_err("[MODOVERRIDE] mod '{}' has no models.json; base only\n", mc2::fmt::safe(activeMod));
                fflush(stderr);
            }
        }
    });
    return g;
}
