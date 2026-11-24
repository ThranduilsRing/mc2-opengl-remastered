// model_override_registry.cpp -- see the header.
// ONLY engine TU permitted to include nlohmann/json (scripts/check-json-isolation.sh).
#include "mc2_czstring.h"
#include "model_override_registry.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <utility>
#include "../RenderCore/SurfaceMaterial.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "Gates/render_options.h"
#include "mc2fmt.h"

using nlohmann::json;

static void logDrop(std::string_view why, std::string_view key) {
    mc2::fmt::print_err("[MODOVERRIDE] dropped '{}': {}\n", key.substr(0, key.find('\0')), why.substr(0, why.find('\0')));
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

static bool endsWith(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) != suffix[i]) return false;
    return true;
}

static bool isSafeSource(std::string_view s) {
    if (s.empty()) return false;
    if (s[0] == '/' || s[0] == '\\') return false;
    if (s.size() >= 2 && s[1] == ':') return false;
    // conservative: rejects any "..", including the rare literal "a..b.glb"; traversal safety > that edge case.
    if (s.find("..") != std::string::npos) return false;
    return endsWith(s, ".glb") || endsWith(s, ".gltf");
}

static std::string trimSource(std::string_view s) {
    size_t b = 0, en = s.size();
    while (b < en && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (en > b && std::isspace(static_cast<unsigned char>(s[en - 1]))) --en;
    return std::string(s.substr(b, en - b));
}

const ModelOverrideCondition* ModelOverrideRecord::condition(std::string_view name) const {
    for (const auto& c : conditions)
        if (c.name == name) return &c;
    return nullptr;
}

// materials v1 (redux-pbr: normal or ORM TGA required) and v2 (content pipeline: .tga/.ktx2 maps,
// map-less class scalars, explicit alpha mode, the full SurfaceClass vocabulary). The class decides
// whether metal is allowed at all; scalars absent in v2 take the class defaults.
static void parseMaterials(const json& entry, ModelOverrideRecord& rec) {
    if (!entry.contains("materials")) return;
    try {
        const auto& package = entry.at("materials");
        if (!package.at("version").is_number_integer() || !package.at("slots").is_array())
            throw std::runtime_error("unsupported material package");
        const int64_t version = package.at("version").get<int64_t>();
        if (version != 1 && version != 2) throw std::runtime_error("unsupported material package");
        std::vector<ModelOverrideMaterial> parsed;
        auto ranged = [](const json& row, std::string_view key, float fallback, float hi) {
            if (row.contains(key) && !row.at(key).is_number())
                throw std::runtime_error("material scalar must be numeric");
            const float v = row.value(key, fallback);
            if (!std::isfinite(v) || v < 0.0f || v > hi)
                throw std::runtime_error("material scalar out of range");
            return v;
        };
        auto path = [version](const json& row, std::string_view key) {
            std::string s = row.value(key, std::string());
            if (s.empty()) return s;
            for (const unsigned char c : s)
                if (c < 32u) throw std::runtime_error("control character in material path");
            const std::filesystem::path candidate(s);
            const bool typeOk = endsWith(s, ".tga") || (version >= 2 && endsWith(s, ".ktx2"));
            if (candidate.has_root_name() || candidate.has_root_directory() ||
                candidate.generic_string() != s || s.find(':') != std::string::npos ||
                s.find("..") != std::string::npos || !typeOk)
                throw std::runtime_error("unsafe/unsupported material map path");
            return s;
        };
        for (const auto& row : package.at("slots")) {
            if (!row.at("reviewed").get<bool>()) throw std::runtime_error("unreviewed material");
            ModelOverrideMaterial m;
            if (!row.at("slot").is_number_integer()) throw std::runtime_error("material slot must be integer");
            const int64_t slot = row.at("slot").get<int64_t>();
            if (slot < 0 || slot > 255) throw std::runtime_error("invalid material slot");
            m.slot = static_cast<uint32_t>(slot);
            for (const auto& prior : parsed)
                if (prior.slot == m.slot) throw std::runtime_error("duplicate material slot");
            const auto cls = RenderCore::surfaceClassFromName(row.at("class").get<std::string>());
            if (!cls || (version == 1 && static_cast<uint32_t>(*cls) > 4u))
                throw std::runtime_error("unknown material class");
            m.materialClass = static_cast<uint32_t>(*cls);
            const auto defaults = RenderCore::kSurfaceClassDefaults[m.materialClass];
            m.normalPath = path(row, "normal");
            m.ormPath = path(row, "orm");
            m.albedoPath = path(row, "albedo");
            m.teamMaskPath = path(row, "teamMask");
            if (!m.teamMaskPath.empty() && !endsWith(m.teamMaskPath, ".ktx2")) throw std::runtime_error("teamMask must be ktx2");
            if (!m.albedoPath.empty() && !endsWith(m.albedoPath, ".ktx2")) throw std::runtime_error("albedo must be ktx2");
            if (version == 1 && m.normalPath.empty() && m.ormPath.empty()) throw std::runtime_error("empty material");
            const auto alpha = RenderCore::surfaceAlphaFromName(row.value("alphaMode", std::string("STOCK")));
            if (!alpha || *alpha == RenderCore::SurfaceAlpha::Blend) throw std::runtime_error("unsupported alpha mode");
            m.alphaMode = static_cast<uint32_t>(*alpha);
            m.doubleSided = row.value("doubleSided", false);
            m.alphaCutoff = ranged(row, "alphaCutoff", 0.5f, 1.0f);
            m.normalStrength = ranged(row, "normalStrength", 1.0f, 2.0f);
            m.aoStrength = ranged(row, "aoStrength", 1.0f, 1.0f);
            m.roughness = ranged(row, "roughness", version == 1 ? 1.0f : defaults.roughness, 1.0f);
            m.metallic = ranged(row, "metallic", version == 1 ? 0.0f : defaults.metallic, 1.0f);
            if (m.metallic != 0.0f && !RenderCore::surfaceClassMayBeMetallic(*cls))
                throw std::runtime_error("material class cannot be metallic");
            if (row.contains("uvScale")) {
                const auto& uv = row.at("uvScale");
                if (!uv.is_array() || uv.size() != 2) throw std::runtime_error("invalid UV scale");
                for (int i = 0; i < 2; ++i) {
                    if (!uv[i].is_number()) throw std::runtime_error("UV scale must be numeric");
                    m.uvScale[i] = uv[i].get<float>();
                    if (!std::isfinite(m.uvScale[i]) || m.uvScale[i] <= 0.0f || m.uvScale[i] > 64.0f)
                        throw std::runtime_error("invalid UV scale");
                }
            }
            parsed.push_back(std::move(m));
        }
        rec.materials = std::move(parsed);
    } catch (const std::exception& error) {
        // Reject the optional package atomically, preserving the model override.
        logDrop(error.what(), rec.overrideClass + ":" + rec.appearanceName + ":materials");
        rec.materials.clear();
    }
}

// LOD chain entries: each must be an object with integer `lod` >= 1 (strictly ascending; LOD0 is the
// chain's own source) and a safe source. A bad entry drops only itself (logged).
static std::vector<ModelOverrideLod> parseLods(const json& e, std::string_view key) {
    std::vector<ModelOverrideLod> out;
    if (!e.contains("lods") || !e["lods"].is_array()) return out;
    int lastLod = 0;
    for (const auto& le : e["lods"]) {
        if (!le.is_object()) { logDrop("lods entry not an object", key); continue; }
        if (!le.contains("lod") || !le["lod"].is_number_integer()) { logDrop("lods entry missing integer 'lod'", key); continue; }
        const int lod = le["lod"].get<int>();
        if (lod <= lastLod) { logDrop("lods 'lod' not strictly ascending (LOD0=source)", key); continue; }
        std::string lsrc = trimSource(le.value("source", std::string()));
        if (!isSafeSource(lsrc)) { logDrop("lods entry unsafe/non-glTF source", key); continue; }
        out.push_back({lod, std::move(lsrc), le.value("distance", 0.0f)});
        lastLod = lod;
    }
    return out;
}

// An optional chain object {"source", "lods"}; an invalid one is dropped (logged), never the record.
static ModelOverrideChain parseChain(const json& e, std::string_view field, std::string_view key) {
    ModelOverrideChain chain;
    if (!e.contains(field)) return chain;
    const auto& c = e.at(field);
    const std::string where = std::string(key) + ":" + std::string(field);
    if (!c.is_object()) { logDrop("chain is not an object", where); return chain; }
    std::string source = trimSource(c.value("source", std::string()));
    if (!isSafeSource(source)) { logDrop("unsafe/non-glTF chain source", where); return chain; }
    chain.sourceRelPath = std::move(source);
    chain.lods = parseLods(c, where);
    return chain;
}

// Parse manifestPath and return all valid records stamped with dir.
// No duplicate-key check here -- callers apply their own policy.
static std::vector<ModelOverrideRecord> parseManifest(std::string_view manifestPath, std::string_view dir) {
    std::vector<ModelOverrideRecord> out;
    std::ifstream in{std::filesystem::path(manifestPath)};
    if (!in.is_open()) return out;
    json root;
    try { in >> root; }
    catch (const std::exception& e) {
        mc2::fmt::print_err("[MODOVERRIDE] parse error in {}: {}\n", manifestPath, mc2::fmt::safe(e.what()));
        return out;
    }
    if (!root.is_object() || !root.contains("overrides") || !root["overrides"].is_array()) {
        mc2::fmt::print_err("[MODOVERRIDE] {}: missing 'overrides' array\n", manifestPath);
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
            // split on FIRST ':'; MC2 appearance names contain no ':'.
            size_t colon = replaces.find(':');
            if (colon == std::string::npos) { logDrop("replaces not '<class>:<name>'", key); continue; }
            std::string cls  = normalizeKey(replaces.substr(0, colon));
            std::string name = normalizeKey(replaces.substr(colon + 1));
            if (cls.empty() || name.empty()) { logDrop("empty class or name in replaces", key); continue; }
            if (cls != "staticprop" && cls != "tree" && cls != "vehicle") { logDrop("class not staticProp|tree|vehicle", key); continue; }
            if (e.contains("class") && normalizeKey(e.value("class", std::string())) != cls) {
                logDrop("class field disagrees with replaces", key); continue;
            }
            std::string source = trimSource(e.value("source", std::string()));
            if (!isSafeSource(source)) { logDrop("unsafe/non-glTF source path", key); continue; }

            ModelOverrideRecord rec;
            rec.overrideClass  = cls;
            rec.appearanceName = name;
            rec.sourceRelPath  = source;
            rec.manifestDir    = dir;
            rec.scale          = scale;
            rec.lateCoverageFringe = e.contains("lateCoverageFringe") &&
                e["lateCoverageFringe"].is_boolean() && e["lateCoverageFringe"].get<bool>();
            parseMaterials(e, rec);
            rec.lods = parseLods(e, key);
            rec.destroyed = parseChain(e, "destroyed", key);
            if (e.contains("conditions") && e["conditions"].is_object()) {
                for (const auto& [condName, c] : e["conditions"].items()) {
                    const std::string cn = normalizeKey(condName);
                    if (cn != "worn" && cn != "abandoned") { logDrop("unknown condition " + cn, key); continue; }
                    const json wrapped = {{"intact", c}};
                    ModelOverrideCondition cond{cn, parseChain(wrapped, "intact", key),
                                                c.is_object() ? parseChain(c, "destroyed", key) : ModelOverrideChain{}};
                    if (!cond.intact.present()) continue;   // parseChain logged why
                    rec.conditions.push_back(std::move(cond));
                }
            }
            out.push_back(std::move(rec));
        } catch (const std::exception& ex) {
            logDrop(ex.what(), "<entry>");
        }
    }
    return out;
}

int ModelOverrideRegistry::loadFromFile(std::string_view manifestPath, std::string_view manifestDir) {
    records_.clear();
    manifestDir_ = manifestDir;
    // Dup check: first entry wins within a single file.
    for (auto& rec : parseManifest(manifestPath, manifestDir)) {
        const bool dup = std::any_of(records_.begin(), records_.end(), [&](const auto& r) {
            return r.overrideClass == rec.overrideClass && r.appearanceName == rec.appearanceName; });
        if (dup) { logDrop("duplicate key (first entry wins)", rec.overrideClass + ":" + rec.appearanceName); continue; }
        records_.push_back(std::move(rec));
    }
    return static_cast<int>(records_.size());
}

int ModelOverrideRegistry::mergeFromFile(std::string_view manifestPath, std::string_view manifestDir) {
    int merged = 0;
    for (auto& rec : parseManifest(manifestPath, manifestDir)) {
        auto it = std::find_if(records_.begin(), records_.end(), [&](const auto& r) {
            return r.overrideClass == rec.overrideClass && r.appearanceName == rec.appearanceName; });
        if (it != records_.end()) *it = std::move(rec);
        else records_.push_back(std::move(rec));
        ++merged;
    }
    return merged;
}

int ModelOverrideRegistry::mergeConditionsFromFile(std::string_view pathView) {
    const std::string path(pathView);
    std::ifstream in(path.c_str());   // path-audit: loose-only (mod/model_overrides tree, beside models.json)
    if (!in.is_open()) return 0;
    int merged = 0;
    try {
        json root;
        in >> root;
        if (!root.is_object() || root.value("version", 0) != 1 || !root.contains("missions") || !root["missions"].is_object())
            throw std::runtime_error("needs version 1 and a missions object");
        for (const auto& [mission, picks] : root["missions"].items()) {
            if (!picks.is_object()) continue;
            for (const auto& [appearance, cond] : picks.items()) {
                const std::string c = cond.is_string() ? normalizeKey(cond.get<std::string>()) : std::string();
                if (c != "pristine" && c != "worn" && c != "abandoned") { logDrop("unknown condition", path + ":" + appearance); continue; }
                Selection s{normalizeKey(mission), normalizeKey(appearance), c == "pristine" ? std::string() : c};
                auto it = std::find_if(selections_.begin(), selections_.end(), [&](const Selection& x) {
                    return x.mission == s.mission && x.appearance == s.appearance; });
                if (it != selections_.end()) *it = std::move(s);
                else selections_.push_back(std::move(s));
                ++merged;
            }
        }
    } catch (const std::exception& e) {
        mc2::fmt::print_err("[MODOVERRIDE] {}: conditions ignored ({})\n", path.c_str(), mc2::fmt::safe(e.what()));
    }
    return merged;
}

std::string ModelOverrideRegistry::selectedCondition(std::string_view mission, std::string_view appearanceName) const {
    // Missions arrive as paths or stems; compare the lowercase stem.
    std::string m = normalizeKey(mission);
    if (const size_t slash = m.find_last_of("/\\"); slash != std::string::npos) m.erase(0, slash + 1);
    if (const size_t dot = m.find_last_of('.'); dot != std::string::npos) m.erase(dot);
    const std::string a = normalizeKey(appearanceName);
    const Selection* wildcard = nullptr;
    for (const auto& s : selections_) {
        if (s.mission != m) continue;
        if (s.appearance == a) return s.condition;
        if (s.appearance == "*") wildcard = &s;
    }
    return wildcard ? wildcard->condition : std::string();
}

const ModelOverrideRecord* ModelOverrideRegistry::resolve(mc2::czstring overrideClass, mc2::czstring appearanceName) const {
    if (!overrideClass || !appearanceName) return nullptr;
    const std::string cls  = normalizeKey(overrideClass);
    const std::string name = normalizeKey(appearanceName);
    for (const auto& r : records_)
        if (r.overrideClass == cls && r.appearanceName == name) return &r;
    return nullptr;
}

const ModelOverrideRecord* ModelOverrideRegistry::resolveBuilding(mc2::czstring shapeName) const {
    const ModelOverrideRecord* prop = resolve("staticProp", shapeName);
    return prop ? prop : resolve("tree", shapeName);
}

std::vector<std::string> modelOverrideMountOrder(std::string_view activeMod, std::string_view envDeps,
                                                 const std::vector<std::string>& modJsonDeps) {
    // mclib/vfs.cpp InitModSearchPaths: deps = mod.json deps, then MC2_MOD_DEPS (comma/semicolon, highest-priority-first),
    // de-duplicated; priority active > deps[0] > ... > deps[N-1] > base.
    std::vector<std::string> deps = modJsonDeps;
    const std::string all(envDeps);
    for (size_t i = 0; i <= all.size();) {
        size_t j = all.find_first_of(",;", i);
        if (j == std::string::npos) j = all.size();
        std::string d = trimSource(std::string_view(all).substr(i, j - i));
        if (!d.empty() && std::find(deps.begin(), deps.end(), d) == deps.end()) deps.push_back(d);
        i = j + 1;
    }
    std::vector<std::string> out;
    for (auto it = deps.rbegin(); it != deps.rend(); ++it)
        if (*it != activeMod) out.push_back("mods/" + *it);
    if (!activeMod.empty()) out.push_back("mods/" + std::string(activeMod));
    return out;
}

static std::vector<std::string> activeModJsonDeps(std::string_view activeMod) {
    std::vector<std::string> deps;
    if (activeMod.empty()) return deps;
    std::ifstream in(std::filesystem::path("mods") / activeMod / "mod.json");   // path-audit: loose-only (mod root metadata)
    if (!in.is_open()) return deps;
    try {
        json root;
        in >> root;
        if (root.is_object() && root.contains("dependencies") && root["dependencies"].is_array())
            for (const auto& d : root["dependencies"])
                if (d.is_string()) deps.push_back(d.get<std::string>());
    } catch (const std::exception&) {}
    return deps;
}

ModelOverrideRegistry& ModelOverrideRegistry::instance() {
    static ModelOverrideRegistry g;
    static std::once_flag once;
    std::call_once(once, [] {
        // Dense bundled geometry stalls 24-EU/shared-DDR4 presets; retain stock buildings.
        // Install-local manifests and mounted mods still take priority.
        const bool bundledArt = mc2::options::current().remasteredBuildings && !mc2::options::lowCost();
        if (bundledArt)
            g.loadFromFile("mods/mc2-industrial-kit/data/model_overrides/models.json",
                           "mods/mc2-industrial-kit/data/model_overrides");
        if (bundledArt)
            g.mergeFromFile("mods/mc2-vehicle-kit/data/model_overrides/models.json",
                            "mods/mc2-vehicle-kit/data/model_overrides");
        const int base = g.mergeFromFile("data/model_overrides/models.json", "data/model_overrides");
        g.mergeConditionsFromFile("data/model_overrides/conditions.json");
        mc2::fmt::print_err("[MODOVERRIDE] install registry: {:d} override(s)\n", mc2::fmt::as_i32(base));
        // Every mounted mod (content packs ride MC2_MOD_DEPS beside any campaign mod) merges in VFS
        // priority order, so the highest-priority provider wins a duplicate key. Source paths are
        // relative to that mod's model_overrides directory: Assimp opens files directly, so the
        // per-record manifestDir is what routes a GLB to its provider.
        std::string active = mc2::gate::str<"MC2_ACTIVE_MOD">();
        if (!bundledArt && (active == "mc2-industrial-kit" || active == "mc2-vehicle-kit")) active.clear();
        for (const auto& root : modelOverrideMountOrder(active, mc2::gate::str<"MC2_MOD_DEPS">(), activeModJsonDeps(active))) {
            if (root == "mods/mc2-industrial-kit") continue; // Already merged at lowest priority.
            if (root == "mods/mc2-vehicle-kit") continue;
            const std::string dir = (std::filesystem::path(root) / "data/model_overrides").generic_string();
            const int n = g.mergeFromFile(dir + "/models.json", dir);
            const int c = g.mergeConditionsFromFile(dir + "/conditions.json");
            if (n || c)
                mc2::fmt::print_err("[MODOVERRIDE] {}: {:d} override(s), {:d} condition pick(s) merged (total {:d})\n",
                                    root.c_str(), mc2::fmt::as_i32(n), mc2::fmt::as_i32(c), mc2::fmt::as_i32(g.count()));
        }
        fflush(stderr);
    });
    return g;
}
