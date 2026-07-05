// fx_def_registry.cpp — FX-DEFS-SIDECAR-1.
// Mirrors anim_override_registry.cpp. Permitted to include nlohmann/json
// (allowlisted in scripts/check-json-isolation.sh).
//
// This TU is the ONLY place that bridges the engine-independent EffectDef
// parse (fx_def_registry.h) to the live gosFX/MLR spec types, so the parse
// layer above stays header-light like its siblings.
#include "fx_def_registry.h"

#include "gosfx/gosfxheaders.hpp"
#include "particles/spec_library.h"
#include <mlr/mlrtexturepool.hpp>

#include <nlohmann/json.hpp>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <utility>

using nlohmann::json;

namespace mc2fxdefs {

namespace {

void fxdefLog(const char* why, const std::string& key) {
    fprintf(stderr, "[FXDEF] %s: %s\n", why, key.c_str());
    fflush(stderr);
}

std::string fxdefLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

std::string fxdefTrim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Parse ONE *.fxdef.json file. Returns false on hard parse failure (file
// unreadable / not an object / missing "effect") — caller logs + skips.
// Soft problems (unknown curve key, non-numeric value) are logged per-field
// and just drop that field; the rest of the def still loads ("friendly
// warning, not failure" per proposal §3.1).
bool fxdefParseOne(const std::string& path, EffectDef& out) {
    std::ifstream in(path.c_str());
    if (!in.is_open()) return false;

    json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        fxdefLog("parse error", path + ": " + e.what());
        return false;
    }
    if (!root.is_object()) {
        fxdefLog("root is not a JSON object", path);
        return false;
    }
    if (!root.contains("effect") || !root["effect"].is_string()) {
        fxdefLog("missing string 'effect' key", path);
        return false;
    }

    out.effectNameRaw = root["effect"].get<std::string>();
    out.effectKey     = fxdefLower(fxdefTrim(out.effectNameRaw));
    out.sourcePath     = path;
    if (out.effectKey.empty()) {
        fxdefLog("empty 'effect' name", path);
        return false;
    }

    if (root.contains("disabled")) {
        if (root["disabled"].is_boolean()) {
            out.disabled = root["disabled"].get<bool>();
        } else {
            fxdefLog("'disabled' is not a bool (ignored)", out.effectNameRaw);
        }
    }

    if (root.contains("texture")) {
        if (root["texture"].is_string()) {
            std::string tex = fxdefTrim(root["texture"].get<std::string>());
            if (!tex.empty()) {
                out.texture = tex;
                out.hasTexture = true;
            } else {
                fxdefLog("empty 'texture' string (ignored)", out.effectNameRaw);
            }
        } else {
            fxdefLog("'texture' is not a string (ignored)", out.effectNameRaw);
        }
    }

    if (root.contains("blend")) {
        if (root["blend"].is_string()) {
            std::string b = fxdefLower(fxdefTrim(root["blend"].get<std::string>()));
            if (b == "additive") { out.hasBlend = true; out.blendAdditive = true; }
            else if (b == "alpha") { out.hasBlend = true; out.blendAdditive = false; }
            else fxdefLog("'blend' not 'additive'|'alpha' (ignored)", out.effectNameRaw);
        } else {
            fxdefLog("'blend' is not a string (ignored)", out.effectNameRaw);
        }
    }

    if (root.contains("curves")) {
        if (root["curves"].is_object()) {
            static const char* kKnown[] = {
                "alpha", "red", "green", "blue", "scale", "lifespan"
            };
            for (auto it = root["curves"].begin(); it != root["curves"].end(); ++it) {
                std::string field = fxdefLower(fxdefTrim(it.key()));
                bool known = false;
                for (const char* k : kKnown) if (field == k) { known = true; break; }
                if (!known) {
                    fxdefLog(("unknown curve key '" + it.key() + "' (did you mean alpha/red/green/blue/scale/lifeSpan?)").c_str(),
                             out.effectNameRaw);
                    continue;
                }
                // v1: constant-number values only (matches mc2fx patch.json
                // "constant" tier). A future schema may accept {"type":"constant","value":N}
                // objects too — accept both shapes.
                double v = 0.0;
                bool haveVal = false;
                if (it.value().is_number()) {
                    v = it.value().get<double>();
                    haveVal = true;
                } else if (it.value().is_object() && it.value().contains("value")
                           && it.value()["value"].is_number()) {
                    v = it.value()["value"].get<double>();
                    haveVal = true;
                }
                if (!haveVal) {
                    fxdefLog(("curve '" + field + "' has no numeric value (ignored)").c_str(),
                             out.effectNameRaw);
                    continue;
                }
                CurveOverride co;
                co.field = field;
                co.value = v;
                out.curves.push_back(co);
            }
        } else {
            fxdefLog("'curves' is not an object (ignored)", out.effectNameRaw);
        }
    }

    // Reserved v1-forward keys ("flipbook","erosion","distortion","light"):
    // presence is fine, no structured parse yet (slices #4-#6). Anything else
    // unrecognized is likewise tolerated per proposal ("unknown keys = warning,
    // not failure") -- we deliberately do NOT enumerate/reject extra keys here.
    return true;
}

std::vector<EffectDef> fxdefParseDir(const std::string& dir) {
    std::vector<EffectDef> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string fname = entry.path().filename().string();
        // Match "*.fxdef.json" (case-insensitive suffix).
        const std::string lower = fxdefLower(fname);
        static const std::string suffix = ".fxdef.json";
        if (lower.size() <= suffix.size()) continue;
        if (lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) != 0) continue;

        EffectDef def;
        if (fxdefParseOne(entry.path().string(), def)) {
            out.push_back(std::move(def));
        }
    }
    return out;
}

}  // namespace

int EffectDefRegistry::loadFromDir(const std::string& dir) {
    defs_.clear();
    byKey_.clear();
    auto parsed = fxdefParseDir(dir);
    for (auto& def : parsed) {
        auto it = byKey_.find(def.effectKey);
        if (it != byKey_.end()) {
            fxdefLog("duplicate effect key in base dir (first file wins)", def.effectNameRaw);
            continue;
        }
        byKey_[def.effectKey] = defs_.size();
        defs_.push_back(std::move(def));
    }
    return (int)defs_.size();
}

int EffectDefRegistry::mergeFromDir(const std::string& dir) {
    auto parsed = fxdefParseDir(dir);
    int merged = 0;
    for (auto& def : parsed) {
        auto it = byKey_.find(def.effectKey);
        if (it != byKey_.end()) {
            defs_[it->second] = std::move(def);  // mod wins on dup key
        } else {
            byKey_[def.effectKey] = defs_.size();
            defs_.push_back(std::move(def));
        }
        ++merged;
    }
    return merged;
}

const EffectDef* EffectDefRegistry::resolve(const char* effectName) const {
    if (!effectName || defs_.empty()) return nullptr;
    auto it = byKey_.find(fxdefLower(effectName));
    if (it == byKey_.end()) return nullptr;
    return &defs_[it->second];
}

namespace {

// Slimmed engine-side mirror of tools/mc2fx/mc2fx_core.cpp's setCurveByName —
// scoped to the schema-v1 field set (alpha/red/green/blue/scale/lifeSpan).
// Returns true if the field was recognized+applied for this spec's classID.
bool fxdefSetCurveByName(gosFX::Effect__Specification* spec, unsigned classID,
                          const std::string& field, double v) {
    Stuff::Scalar val = static_cast<Stuff::Scalar>(v);
    auto setConst = [&](gosFX::ConstantCurve& c) { c.m_value = val; };
    auto setComplex = [&](gosFX::ComplexCurve& c) { c.SetCurve(val); };

    if (field == "lifespan") { setConst(spec->m_lifeSpan); return true; }

    if (classID == gosFX::SingletonClassID || classID == gosFX::CardClassID ||
        classID == gosFX::ShapeClassID) {
        auto* s = static_cast<gosFX::Singleton__Specification*>(spec);
        if (field == "red")   { setComplex(s->m_red.m_ageCurve);   return true; }
        if (field == "green") { setComplex(s->m_green.m_ageCurve); return true; }
        if (field == "blue")  { setComplex(s->m_blue.m_ageCurve);  return true; }
        if (field == "alpha") { setComplex(s->m_alpha.m_ageCurve); return true; }
        if (field == "scale") { setComplex(s->m_scale.m_ageCurve); return true; }
        return false;
    }
    if (classID == gosFX::ParticleCloudClassID || classID == gosFX::PointCloudClassID ||
        classID == gosFX::SpinningCloudClassID || classID == gosFX::ShardCloudClassID ||
        classID == gosFX::PertCloudClassID || classID == gosFX::CardCloudClassID ||
        classID == gosFX::ShapeCloudClassID || classID == gosFX::EffectCloudClassID) {
        auto* p = static_cast<gosFX::ParticleCloud__Specification*>(spec);
        if (field == "red")   { setComplex(p->m_pRed.m_ageCurve);   return true; }
        if (field == "green") { setComplex(p->m_pGreen.m_ageCurve); return true; }
        if (field == "blue")  { setComplex(p->m_pBlue.m_ageCurve);  return true; }
        if (field == "alpha") { setComplex(p->m_pAlpha.m_ageCurve); return true; }
        // "scale" has no direct ParticleCloud-family analog in v1 (per-particle
        // size lives on subclasses e.g. CardCloud halfHeight — deferred).
        return false;
    }
    return false;
}

// Rebind spec->m_state's texture to textureName, mirroring exactly what
// MLRState::Load does when it reads a texture name off the stream
// (mlrstate.cpp:178-199): look up-or-add in the live MLRTexturePool, then
// SetTextureHandle to the pool's handle. Safe to call post-Load (pool is
// long-lived and this is the same call the stock loader itself makes).
bool fxdefRebindTexture(gosFX::Effect__Specification* spec, const std::string& textureName) {
    if (!MidLevelRenderer::MLRTexturePool::Instance) {
        fxdefLog("no MLRTexturePool::Instance (too early?) — texture override skipped",
                 textureName);
        return false;
    }
    MidLevelRenderer::MLRTexture* texture =
        (*MidLevelRenderer::MLRTexturePool::Instance)(textureName.c_str(), 0);
    if (!texture) {
        texture = MidLevelRenderer::MLRTexturePool::Instance->Add(textureName.c_str(), 0);
    }
    if (!texture) {
        fxdefLog("texture add/lookup failed (bad path?) — override skipped", textureName);
        return false;
    }
    spec->m_state.SetTextureHandle(texture->GetTextureHandle());
    return true;
}

}  // namespace

int EffectDefRegistry::applyAll(mc2::particles::SpecLibrary* lib) const {
    if (!lib || defs_.empty()) return 0;
    int applied = 0;
    for (const auto& def : defs_) {
        gosFX::Effect__Specification* spec = lib->Find(def.effectNameRaw.c_str());
        if (!spec) {
            fxdefLog("effect name not found in loaded SpecLibrary (typo, or not in mc2.fx catalog)",
                     def.effectNameRaw);
            continue;
        }

        bool touchedThis = false;

        if (def.disabled) {
            // v1 "disabled" == force lifeSpan to ~0 so the effect is a
            // functional no-op without touching SpecLibrary's frozen array
            // (no public remove/deactivate exists — see gosfx-modder-dropin
            // -path.md). Logged distinctly so it's obvious in diagnostics.
            spec->m_lifeSpan.m_value = 0.0f;
            fxdefLog("disabled (lifeSpan forced to 0)", def.effectNameRaw);
            touchedThis = true;
        }

        if (def.hasTexture) {
            if (fxdefRebindTexture(spec, def.texture)) touchedThis = true;
        }

        if (def.hasBlend) {
            spec->m_state.SetAlphaMode(def.blendAdditive
                ? MidLevelRenderer::MLRState::OneOneMode
                : MidLevelRenderer::MLRState::AlphaInvAlphaMode);
            touchedThis = true;
        }

        if (!def.curves.empty()) {
            unsigned classID = static_cast<unsigned>(spec->GetClassID());
            for (const auto& co : def.curves) {
                if (fxdefSetCurveByName(spec, classID, co.field, co.value)) {
                    touchedThis = true;
                } else {
                    fxdefLog(("curve field '" + co.field + "' unsupported for this effect's class").c_str(),
                             def.effectNameRaw);
                }
            }
        }

        if (touchedThis) {
            ++applied;
            fprintf(stderr, "[FXDEF] applied overlay to '%s' (from %s)\n",
                    def.effectNameRaw.c_str(), def.sourcePath.c_str());
            fflush(stderr);
        }
    }
    return applied;
}

EffectDefRegistry& EffectDefRegistry::instance() {
    static EffectDefRegistry g;
    static std::once_flag once;
    std::call_once(once, [] {
        int base = g.loadFromDir("data/effects/defs");
        if (base > 0) {
            fprintf(stderr, "[FXDEF] base registry: %d def(s)\n", base);
            fflush(stderr);
        }
        const char* activeMod = getenv("MC2_ACTIVE_MOD");
        if (activeMod && activeMod[0]) {
            std::string modDir = std::string("mods/") + activeMod + "/data/effects/defs";
            std::error_code ec;
            if (std::filesystem::is_directory(modDir, ec)) {
                int n = g.mergeFromDir(modDir);
                fprintf(stderr, "[FXDEF] mod '%s': %d def(s) merged (total %d)\n",
                        activeMod, n, g.count());
                fflush(stderr);
            }
        }
    });
    return g;
}

}  // namespace mc2fxdefs
