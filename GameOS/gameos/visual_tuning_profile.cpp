// L1 compatibility environment: preserve legacy renderer values and color interpretation.
#include "mc2_czstring.h"
#include "visual_tuning_profile.h"
#include "cloud_threshold_band.h"
#include "../../GameAdapters/MissionEnvironmentAdapter.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "Gates/render_options.h"
#include "ibl_sh_runtime.h"
#include "gos_postprocess.h"
void mc2SetCloudWaterScale(float v);   // vulkan_post_screenfx_native.cpp
#include "../../RenderCore/IblHdriRegistry.h"
#include "../../RenderCore/IblShRegistry.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "mc2fmt.h"

// Renderer function forward declarations.
// Definitions in gameos_graphics.cpp / gos_postprocess.cpp / gos_particle_bridge.cpp.
extern float gos_GetWaterSkyTintStrength();
extern void  gos_SetWaterSkyTintStrength(float v);
extern int   gos_GetWaterReflectionLegacy();
extern void  gos_SetWaterReflectionQuality(int quality);
extern float gos_GetExposure();
extern void  gos_SetExposure(float v);
extern "C" void  gos_vfx_setBrightness(float v);
extern "C" void  gos_vfx_setAdditiveBrightness(float v);
extern "C" float gos_vfx_getBrightness(void);
extern "C" float gos_vfx_getAdditiveBrightness(void);
extern "C" void  gos_vfx_setLitStrength(float v);   // VFX-LIT-PARTICLES-MVP-1
extern "C" float gos_vfx_getLitStrength(void);      // VFX-LIT-PARTICLES-MVP-1
// Getters for the post-stack keys (reader already had the setters above) so the
// writer can round-trip the full reader vocabulary (fixes the 8-vs-13 asymmetry).
// Mech specular tuning.
extern "C" void  batcher_setMechSpecularStrength(float s);
extern "C" float batcher_getMechSpecularStrength();
// TERRAIN-CLASSIFY-TUNING-1: RGB classifier thresholds (defined in gameos_graphics.cpp).
extern void gos_SetTerrainClassGrass(float gMinusRLo, float gMinusRHi, float gBrightLo, float gBrightHi);
extern void gos_GetTerrainClassGrass(float* gMinusRLo, float* gMinusRHi, float* gBrightLo, float* gBrightHi);
extern void gos_SetTerrainClassDirt(float rMinusGLo, float rMinusGHi, float rBrightLo, float rBrightHi);
extern void gos_GetTerrainClassDirt(float* rMinusGLo, float* rMinusGHi, float* rBrightLo, float* rBrightHi);


namespace {
using Json = nlohmann::json;
using Environment = RenderCore::MissionEnvironment;
Environment s_environment;
VisualTuningSource s_editableSource;
std::string s_profilePath = "data/visual_tuning.json";
bool s_hasFile = false;
int s_appliedKeys = 0;

std::map<std::string, float> engineDefaults() {
    return {{"gradeR",1}, {"gradeG",1}, {"gradeB",1},
        {"gradeShadowsR",1}, {"gradeShadowsG",1}, {"gradeShadowsB",1},
        {"gradeHighlightsR",1}, {"gradeHighlightsG",1}, {"gradeHighlightsB",1},
        {"topWarmthR",1}, {"topWarmthG",1}, {"topWarmthB",1},
        {"vignette",0}, {"topWarmthStrength",0},
        {"unitAmbientScale",1.2f}, {"buildingSpillIntensityScale",1.0f},
        {"exposure",1}, {"shadowSoftness",2.5f}, {"staticPropIblStrength",0.5f},
        {"waterSkyTintStrength",0}, {"waterReflections",static_cast<float>(mc2::options::current().waterReflections)},
        {"vfxBrightness",1}, {"vfxAdditiveBrightness",1},
        {"vfxLitStrength",0.7f}, {"mechSpecularStrength",0.05f},
        {"cloudStrength",0.30f}, {"cloudCoverage",0.20f}, {"cloudSoftness",0.12f}, {"cloudWaterScale",0.5f},
        {"terrainClassGrassGmRLo",-0.02f}, {"terrainClassGrassGmRHi",0.06f},
        {"terrainClassGrassBrLo",0.22f}, {"terrainClassGrassBrHi",0.40f},
        {"terrainClassDirtRmGLo",-0.02f}, {"terrainClassDirtRmGHi",0.06f},
        {"terrainClassDirtBrLo",0.22f}, {"terrainClassDirtBrHi",0.45f}};
}

bool validValue(std::string_view key, double value) {
    if (!std::isfinite(value)) return false;
    float hi = 64;
    float lo = 0;
    if (key == "vignette" || key == "topWarmthStrength") hi = 1;
    else if (key.starts_with("grade") || key.starts_with("topWarmth")) hi = 2;
    else if (key == "unitAmbientScale" || key == "buildingSpillIntensityScale") hi = 2;
    else if (key == "staticPropIblStrength") hi = 3;
    else if (key == "mechSpecularStrength") hi = 4;
    else if (key == "vfxBrightness" || key == "vfxAdditiveBrightness") hi = 8;
    else if (key == "vfxLitStrength" || key == "cloudStrength" || key == "cloudCoverage" || key == "cloudSoftness" || key == "cloudWaterScale") hi = 1;
    else if (key.find("terrainClass") == 0) {
        hi = 1;
        if (key.find("GmR") != std::string::npos || key.find("RmG") != std::string::npos) lo = -1;
    }
    return value >= lo && value <= hi;
}

Json environmentDocument(const Environment& env) {
    const auto& l = env.lighting;
    Json output{{"exposureMultiplier",env.output.exposureMultiplier}, {"transform",env.output.transform}};
    output["exposureEV"] = env.output.exposureEV ? Json(*env.output.exposureEV) : Json(nullptr);
    output["grade"] = env.output.grade;
    output["shadows"] = env.output.shadows;
    output["highlights"] = env.output.highlights;
    output["topWarmth"] = env.output.topWarmth;
    output["vignette"] = env.output.vignette;
    output["topWarmthStrength"] = env.output.topWarmthStrength;
    return {{"schemaVersion",Environment::kSchemaVersion}, {"sourceSchemaVersion",env.sourceSchemaVersion},
        {"mission",env.mission}, {"colorContract",env.colorContract}, {"directionalSource",env.directionalSource},
        {"lighting",{{"cameraDirection",l.cameraDirection}, {"effectiveDirection",l.effectiveDirection},
            {"directionalColor",l.directionalColor}, {"ambientColor",l.ambientColor}, {"flatLight",l.flatLight},
            {"authoredPitch",l.authoredPitch}, {"night",l.night}, {"nightFactor",l.nightFactor},
            {"daylightLevel",l.daylightLevel}, {"skyNumber",l.skyNumber}}},
        {"sky",{{"asset",env.sky.asset}, {"provider",env.sky.provider}, {"shIdentity",env.sky.shIdentity},
            {"shConvention",env.sky.shConvention}, {"orientation",env.sky.orientation}, {"diffuseSh",env.sky.diffuseSh},
            {"hemisphereSky",env.sky.hemisphereSky}, {"hemisphereGround",env.sky.hemisphereGround}}},
        {"unitAmbientScale",env.unitAmbientScale},
        {"buildingSpillIntensityScale",env.buildingSpillIntensityScale},
        {"output",output}, {"atmosphere",env.atmosphere}, {"localLights",env.localLights},
        {"clouds",env.clouds}, {"cacheStatus",env.cacheStatus}, {"legacyTuning",env.legacyTuning},
        {"providers",env.providers}};
}

void finish(Environment& env) {
    const auto rgb = [&](std::string_view prefix) {
        const std::string key(prefix);
        return std::array<float,3>{env.legacyTuning.at(key+"R"),
            env.legacyTuning.at(key+"G"),env.legacyTuning.at(key+"B")};
    };
    env.unitAmbientScale = env.legacyTuning.at("unitAmbientScale");
    env.buildingSpillIntensityScale = env.legacyTuning.at("buildingSpillIntensityScale");
    env.output.grade = rgb("grade");
    env.output.shadows = rgb("gradeShadows");
    env.output.highlights = rgb("gradeHighlights");
    env.output.topWarmth = rgb("topWarmth");
    env.output.vignette = env.legacyTuning.at("vignette");
    env.output.topWarmthStrength = env.legacyTuning.at("topWarmthStrength");
    env.output.exposureMultiplier = env.legacyTuning.at("exposure");
    env.output.exposureEV = env.output.exposureMultiplier > 0
        ? std::optional<float>(std::log2(env.output.exposureMultiplier)) : std::nullopt;
    env.sky.asset = RenderCore::lookupHdriForSkyNumber(env.lighting.skyNumber).exrPath;
    const auto& sh = RenderCore::lookupShSet(env.mission.c_str());
    env.sky.shIdentity = sh.name;
    for (size_t i=0; i<9; ++i)
        std::copy_n(sh.coeffs[i],3,env.sky.diffuseSh[i].begin());
    // Weak mission-colored sky at night, never the daylight HDRI coefficients.
    const float night = env.lighting.night ? 1.0f : std::clamp(env.lighting.nightFactor,0.0f,1.0f);
    for (size_t c=0; c<3; ++c) {
        const float ambient = env.lighting.ambientColor[c];
        env.sky.hemisphereSky[c] = ambient * (1.0f - 0.75f*night);
        env.sky.hemisphereGround[c] = env.sky.hemisphereSky[c]*0.5f;
        for (size_t i=0; i<9; ++i) {
            const float nightSh = i==0 ? ambient*0.25f*0.75f/0.886227f
                : i==1 ? ambient*0.25f*0.25f/(2.0f*0.511664f) : 0.0f;
            env.sky.diffuseSh[i][c] = env.sky.diffuseSh[i][c]*(1.0f-night) + nightSh*night;
        }
    }
    if (night > 0) env.sky.shIdentity = "mission-ambient-night";
    // FNV-1a over canonical JSON, never struct padding or addresses. No bake cache yet.
    uint64_t hash = 14695981039346656037ull;
    auto values = environmentDocument(env);
    values.erase("sourceSchemaVersion");
    values.erase("providers");
    for (unsigned char c : values.dump()) { hash ^= c; hash *= 1099511628211ull; }
    env.effectiveHash = hash;
}

std::optional<Json> parseSource(const VisualTuningSource& source, std::vector<std::string>& diagnostics) {
    if (source.text.empty()) {
        diagnostics.push_back(source.error.empty() ? "missing profile: " + source.path : source.error);
        return std::nullopt;
    }
    auto doc = Json::parse(source.text, nullptr, false);
    if (!doc.is_object()) {
        diagnostics.push_back("invalid JSON profile: " + source.path);
        return std::nullopt;
    }
    const auto version = doc.find("schemaVersion");
    if (version != doc.end() && (!version->is_number_integer() ||
        (*version != 1 && *version != Environment::kSchemaVersion))) {
        diagnostics.push_back("unsupported schemaVersion: " + source.path);
        return std::nullopt;
    }
    if (doc.contains("colorContract") && doc["colorContract"] != "legacy-authored-v1") {
        diagnostics.push_back("unsupported colorContract: " + source.path);
        return std::nullopt;
    }
    return doc;
}

void overlay(Environment& env, const Json& values, std::string_view providerView) {
    const std::string provider(providerView);
    if (!values.is_object()) { env.diagnostics.push_back("expected object: " + provider); return; }
    for (auto it=values.begin(); it!=values.end(); ++it) {
        if (it.key() == "terrainLightingV1Strength" || it.key() == "terrainLightingV2Floor") continue;
        if (!env.legacyTuning.contains(it.key())) {
            env.diagnostics.push_back("unknown key: " + provider + ":" + it.key());
        } else if (!it->is_number() || !validValue(it.key(), it->get<double>())) {
            env.diagnostics.push_back("invalid value: " + provider + ":" + it.key());
        } else {
            env.legacyTuning[it.key()] = it->get<float>();
        }
    }
}

VisualTuningSource readProfileSource(std::string_view path) {
    return GameAdapters::Environment::readProfileSource(path);
}

} // namespace

RenderCore::MissionEnvironment visualTuning_resolve(std::string_view mission,
    const std::vector<VisualTuningSource>& sources, const std::map<std::string,float>& sessionOverrides) {
    Environment env;
    env.mission = mission;
    env.legacyTuning = engineDefaults();
    env.providers = {"engine-defaults", "mission.fit:legacy-camera"};
    for (const auto& source : sources) {
        auto parsed = parseSource(source,env.diagnostics);
        if (!parsed) continue;
        const auto& doc = *parsed;
        env.sourceSchemaVersion = doc.value("schemaVersion",1u);
        const auto provider = source.provider + ":" + source.path;
        env.providers.push_back(provider);
        if (doc.contains("defaults")) overlay(env,doc["defaults"],provider+":defaults");
        if (doc.contains("missions")) {
            const auto& missions = doc["missions"];
            if (!missions.is_object()) env.diagnostics.push_back("expected missions object: " + provider);
            else if (missions.contains(env.mission)) overlay(env,missions[env.mission],provider+":"+env.mission);
        }
    }
    if (!sessionOverrides.empty()) {
        overlay(env,Json(sessionOverrides),"explicit-session");
        env.providers.push_back("explicit-session");
    }
    for (const auto& prefix : {"terrainClassGrassGmR", "terrainClassGrassBr", "terrainClassDirtRmG", "terrainClassDirtBr"}) {
        const std::string lo = std::string(prefix)+"Lo", hi = std::string(prefix)+"Hi";
        if (env.legacyTuning[lo] >= env.legacyTuning[hi]) {
            const auto defaults = engineDefaults();
            env.legacyTuning[lo] = defaults.at(lo); env.legacyTuning[hi] = defaults.at(hi);
            env.diagnostics.push_back("invalid threshold order: " + std::string(prefix));
        }
    }
    finish(env);
    return env;
}

const Environment& visualTuning_environment() { return s_environment; }

void visualTuning_setMissionLighting(const RenderCore::LegacyMissionLighting& lighting) {
    if (s_environment.lighting == lighting) return;
    s_environment.lighting = lighting;
    if (!s_environment.legacyTuning.empty()) finish(s_environment);
}

std::string visualTuning_environmentJson() {
    auto doc = environmentDocument(s_environment);
    doc["effectiveHash"] = s_environment.effectiveHash;
    doc["hashAlgorithm"] = "fnv1a64-canonical-json";
    doc["diagnostics"] = s_environment.diagnostics;
    return doc.dump();
}

namespace {

static void applyKey(mc2::czstring key, float val, int& count) {
    if (strcmp(key, "exposure") == 0) {
        gos_SetExposure(val);
        count++;

    } else if (strcmp(key, "terrainLightingV1Strength") == 0 ||
               strcmp(key, "terrainLightingV2Floor") == 0) {
        // Retired keys: shipped installs still carry them; ignored silently.
    } else if (strcmp(key, "staticPropIblStrength") == 0) {
        // This is the shipped value of the SH-IBL probe strength: it runs on
        // every mission load and replaces the batcher's pre-mission fallback.
        g_iblShStrength = val < 0.0f ? 0.0f : (val > 3.0f ? 3.0f : val);
        count++;
    } else if (strcmp(key, "waterReflections") == 0) {
        if (val >= 0 && val <= 2 && std::floor(val) == val) {
            gos_SetWaterReflectionQuality(static_cast<int>(val));
            count++;
        }
    } else if (strcmp(key, "waterSkyTintStrength") == 0) {
        {
            gos_SetWaterSkyTintStrength(val);
            count++;
        }
    } else if (strcmp(key, "vfxBrightness") == 0) {
        gos_vfx_setBrightness(val);
        count++;
    } else if (strcmp(key, "vfxAdditiveBrightness") == 0) {
        gos_vfx_setAdditiveBrightness(val);
        count++;
    } else if (strcmp(key, "vfxLitStrength") == 0) {
        // VFX-LIT-PARTICLES-MVP-1: per-mission lit-smoke strength (0..1). Gate
        // MC2_VFX_LIT_PARTICLES still controls whether lighting is applied.
        gos_vfx_setLitStrength(val);
        count++;
    } else if (strcmp(key, "mechSpecularStrength") == 0) {
        {
            batcher_setMechSpecularStrength(val);
            count++;
        }
    } else if (strcmp(key, "cloudStrength") == 0) {
        if (gosPostProcess* pp = getGosPostProcess()) pp->setCloudStrength(val);
        count++;
    } else if (strcmp(key, "cloudCoverage") == 0 || strcmp(key, "cloudSoftness") == 0) {
        // band math: cloud_threshold_band.h (pure, unit-tested)
        static float s_cov = 0.20f, s_soft = 0.12f;
        (strcmp(key, "cloudCoverage") == 0 ? s_cov : s_soft) = val;
        if (gosPostProcess* pp = getGosPostProcess()) {
            const CloudThreshBand band = cloudThreshBand(s_cov, s_soft);
            pp->setCloudThreshLo(band.lo);
            pp->setCloudThreshHi(band.hi);
        }
        count++;
    } else if (strcmp(key, "cloudWaterScale") == 0) {
        mc2SetCloudWaterScale(val);
        count++;
    // TERRAIN-CLASSIFY-TUNING-1: per-mission RGB classifier thresholds.
    } else if (strcmp(key, "terrainClassGrassGmRLo") == 0) {
        float a, b, c, d;
        gos_GetTerrainClassGrass(&a, &b, &c, &d);
        gos_SetTerrainClassGrass(val, b, c, d); count++;
    } else if (strcmp(key, "terrainClassGrassGmRHi") == 0) {
        float a, b, c, d;
        gos_GetTerrainClassGrass(&a, &b, &c, &d);
        gos_SetTerrainClassGrass(a, val, c, d); count++;
    } else if (strcmp(key, "terrainClassGrassBrLo") == 0) {
        float a, b, c, d;
        gos_GetTerrainClassGrass(&a, &b, &c, &d);
        gos_SetTerrainClassGrass(a, b, val, d); count++;
    } else if (strcmp(key, "terrainClassGrassBrHi") == 0) {
        float a, b, c, d;
        gos_GetTerrainClassGrass(&a, &b, &c, &d);
        gos_SetTerrainClassGrass(a, b, c, val); count++;
    } else if (strcmp(key, "terrainClassDirtRmGLo") == 0) {
        float a, b, c, d;
        gos_GetTerrainClassDirt(&a, &b, &c, &d);
        gos_SetTerrainClassDirt(val, b, c, d); count++;
    } else if (strcmp(key, "terrainClassDirtRmGHi") == 0) {
        float a, b, c, d;
        gos_GetTerrainClassDirt(&a, &b, &c, &d);
        gos_SetTerrainClassDirt(a, val, c, d); count++;
    } else if (strcmp(key, "terrainClassDirtBrLo") == 0) {
        float a, b, c, d;
        gos_GetTerrainClassDirt(&a, &b, &c, &d);
        gos_SetTerrainClassDirt(a, b, val, d); count++;
    } else if (strcmp(key, "terrainClassDirtBrHi") == 0) {
        float a, b, c, d;
        gos_GetTerrainClassDirt(&a, &b, &c, &d);
        gos_SetTerrainClassDirt(a, b, c, val); count++;
    } else {
        static std::map<std::string,bool> s_warned;
        if (!s_warned[key]) {
            s_warned[key] = true;
            mc2::fmt::print_err("[VisualTuning] unknown key '{}' -- ignored\n", mc2::fmt::safe(key));
        }
    }
}


} // namespace

void visualTuning_applyProfile(mc2::czstring missionName) {
    const auto base = readProfileSource("data/visual_tuning.json");
    std::vector<VisualTuningSource> sources{base};
    s_editableSource = base;
    const auto sessionPath = mc2::gate::str<"MC2_VISUAL_TUNING_FILE">();
    if (!sessionPath.empty()) {
        s_editableSource = readProfileSource(sessionPath);
        if (s_editableSource.provider == "base-loose") s_editableSource.editable = true;
        sources.push_back(s_editableSource);
    }
    s_profilePath = s_editableSource.path;
    std::map<std::string,float> session;
    if (mc2::gate::get<"MC2_WATER_SKYTINT">()) session["waterSkyTintStrength"] = 0.15f;
    if (mc2::gate::isNonEmpty<"MC2_MECH_SPECULAR_STRENGTH">())
        session["mechSpecularStrength"] = mc2::gate::get<"MC2_MECH_SPECULAR_STRENGTH">();
    s_environment = visualTuning_resolve(missionName ? missionName : "",sources,session);
    s_hasFile = s_environment.providers.size() > (session.empty() ? 2u : 3u);
    int count = 0;
    for (const auto& [key,value] : s_environment.legacyTuning) applyKey(key.c_str(),value,count);
    s_appliedKeys = count;
    const auto& waterOptions = mc2::options::current();
    mc2::fmt::print_err("[WATER_QUALITY] reflection={} detail={} legacy={} apply=live\n",
        waterOptions.reflectionQuality, waterOptions.waterDetail, gos_GetWaterReflectionLegacy());
    for (const auto& diagnostic : s_environment.diagnostics)
        mc2::fmt::print_err("[VisualTuning] {}\n",diagnostic);
    mc2::fmt::print_err("[MissionEnvironment] mission='{}' schema=2 sourceSchema={} hash={} color={} providers={} keys={}\n",
        s_environment.mission,s_environment.sourceSchemaVersion,s_environment.effectiveHash,
        s_environment.colorContract,Json(s_environment.providers).dump(),count);
}

const char* visualTuning_getProfilePath() { return s_profilePath.c_str(); }
const char* visualTuning_getActiveMission() { return s_environment.mission.c_str(); }
bool visualTuning_hasProfileFile() { return s_hasFile; }
int visualTuning_getAppliedKeyCount() { return s_appliedKeys; }

bool visualTuning_saveCurrentToMission() {
    if (s_environment.mission.empty()) return false;
    if (!s_editableSource.editable) {
        mc2::fmt::print_err("[VisualTuning] '{}' is not an active editable override; preserve its provider\n",s_profilePath);
        return false;
    }

    // Snapshot all current tunable values.
    std::map<std::string,float> current = s_environment.legacyTuning;
    current["exposure"]                  = gos_GetExposure();
    current["staticPropIblStrength"]     = g_iblShStrength;
    current["waterSkyTintStrength"]      = gos_GetWaterSkyTintStrength();
    current["waterReflections"]         = static_cast<float>(gos_GetWaterReflectionLegacy());
    current["vfxBrightness"]             = gos_vfx_getBrightness();
    current["vfxAdditiveBrightness"]     = gos_vfx_getAdditiveBrightness();
    current["vfxLitStrength"]            = gos_vfx_getLitStrength();  // VFX-LIT-PARTICLES-MVP-1
    // Post-stack keys the reader already accepts but the writer used to drop
    // (8-vs-13 asymmetry) -- round-trip them so "Set as Mission Defaults" keeps
    // any live bloom/AO tuning instead of silently losing it.
    // MISSION-LIGHTING-PROFILE-FIELDS-1 mech lane.
    current["mechSpecularStrength"]      = batcher_getMechSpecularStrength();
    // TERRAIN-CLASSIFY-TUNING-1: RGB classifier thresholds.
    {
        float a, b, c, d;
        gos_GetTerrainClassGrass(&a, &b, &c, &d);
        current["terrainClassGrassGmRLo"] = a;
        current["terrainClassGrassGmRHi"] = b;
        current["terrainClassGrassBrLo"]  = c;
        current["terrainClassGrassBrHi"]  = d;
    }
    {
        float a, b, c, d;
        gos_GetTerrainClassDirt(&a, &b, &c, &d);
        current["terrainClassDirtRmGLo"] = a;
        current["terrainClassDirtRmGHi"] = b;
        current["terrainClassDirtBrLo"]  = c;
        current["terrainClassDirtBrHi"]  = d;
    }


    // Re-read the editable provider so external changes to unrelated missions survive.
    auto source = readProfileSource(s_profilePath);
    if (!source.error.empty()) return false;
    Json doc = Json::object();
    if (!source.text.empty()) {
        std::vector<std::string> diagnostics;
        auto parsed = parseSource(source,diagnostics);
        if (!parsed) return false;
        doc = std::move(*parsed);
    }
    if (doc.contains("missions") && !doc["missions"].is_object()) return false;
    if (!doc.contains("missions")) doc["missions"] = Json::object();
    auto& entry = doc["missions"][s_environment.mission];
    if (!entry.is_null() && !entry.is_object()) return false;
    if (entry.is_null()) entry = Json::object();
    for (const auto& [key,value] : current) {
        if (!validValue(key,value)) return false;
        entry[key] = value;
    }
    doc["schemaVersion"] = Environment::kSchemaVersion;
    doc["colorContract"] = "legacy-authored-v1";
    const auto bytes = doc.dump(2) + "\n";
    const auto temporary = s_profilePath + ".tmp";
    std::ofstream output(temporary,std::ios::binary | std::ios::trunc);
    if (!output || !output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()))) {
        mc2::fmt::print_err("[VisualTuning] cannot write '{}'\n",s_profilePath);
        return false;
    }
    output.close();
    std::error_code error;
    if (output) std::filesystem::rename(temporary,s_profilePath,error);
    if (!output || error) {
        std::filesystem::remove(temporary,error);
        return false;
    }
    s_hasFile = true;
    return true;
}

