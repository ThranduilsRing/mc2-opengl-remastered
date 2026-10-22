// MISSION-VISUAL-TUNING-1: optional per-mission renderer tuning profiles.
// See docs/visual-tuning-profiles.md.

#include "visual_tuning_profile.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "ibl_sh_runtime.h"  // g_iblShStrength (extern float)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include "mc2fmt.h"

// Renderer function forward declarations.
// Definitions in gameos_graphics.cpp / gos_postprocess.cpp / gos_particle_bridge.cpp.
extern float gos_GetWaterSkyTintStrength();
extern void  gos_SetWaterSkyTintStrength(float v);
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
// MISSION-LIGHTING-PROFILE-FIELDS-1: mech lane ambient + specular. Both gates
// (MC2_MECH_AMBIENT_V1 / MC2_MECH_SPECULAR_V1) are default-ON, so these are live.
extern "C" void  batcher_setMechAmbientStrength(float s);
extern "C" float batcher_getMechAmbientStrength();
extern "C" void  batcher_setMechSpecularStrength(float s);
extern "C" float batcher_getMechSpecularStrength();
// TERRAIN-CLASSIFY-TUNING-1: RGB classifier thresholds (defined in gameos_graphics.cpp).
extern void gos_SetTerrainClassGrass(float gMinusRLo, float gMinusRHi, float gBrightLo, float gBrightHi);
extern void gos_GetTerrainClassGrass(float* gMinusRLo, float* gMinusRHi, float* gBrightLo, float* gBrightHi);
extern void gos_SetTerrainClassDirt(float rMinusGLo, float rMinusGHi, float rBrightLo, float rBrightHi);
extern void gos_GetTerrainClassDirt(float* rMinusGLo, float* rMinusGHi, float* rBrightLo, float* rBrightHi);

namespace {

// Minimal JSON parser for flat float-value objects, two levels deep.
// Handles {"key": 1.0, ...} and {"mission": {"key": 1.0, ...}, ...}.
struct TinyJson {
    const char* p;
    const char* e;

    void ws() { while (p < e && static_cast<unsigned char>(*p) <= 32) p++; }

    bool eat(char c) { ws(); if (p < e && *p == c) { p++; return true; } return false; }

    std::string str() {
        ws();
        if (p >= e || *p != '"') return {};
        p++;
        const char* s = p;
        while (p < e && *p != '"' && *p != '\\') p++;
        std::string r(s, static_cast<size_t>((p - s)));
        if (p < e) p++;  // skip closing "
        return r;
    }

    float num() {
        ws(); char* ep = nullptr;
        float v = static_cast<float>(strtod(p, &ep));
        if (ep != p) p = ep;
        return v;
    }

    void skipVal() {
        ws(); if (p >= e) return;
        if (*p == '{' || *p == '[') {
            char op = *p, cl = (op == '{') ? '}' : ']';
            int d = 0;
            do {
                if (p >= e) break;
                if (*p == '"') { str(); continue; }
                char c = *p++;
                if (c == op) d++;
                else if (c == cl && --d == 0) return;
            } while (true);
        } else if (*p == '"') {
            str();
        } else {
            char* ep = nullptr; strtod(p, &ep);
            if (ep != p) { p = ep; }
            else while (p < e && *p != ',' && *p != '}' && *p != ']' && static_cast<unsigned char>(*p) > 32) p++;
        }
    }

    // Parse {"key": float, ...} — nested objects are skipped.
    void floatObj(std::map<std::string,float>& out) {
        if (!eat('{')) return;
        for (;;) {
            ws(); if (p >= e || *p == '}') { if (p < e) p++; break; }
            auto k = str(); if (!eat(':')) break;
            ws();
            if (p < e && (*p == '{' || *p == '[')) skipVal();
            else if (!k.empty()) out[k] = num();
            else skipVal();
            eat(',');
        }
    }

    // Parse {"mission": {floatObj}, ...}
    void missionMap(std::map<std::string, std::map<std::string,float>>& out) {
        if (!eat('{')) return;
        for (;;) {
            ws(); if (p >= e || *p == '}') { if (p < e) p++; break; }
            auto k = str(); if (!eat(':')) break;
            if (!k.empty()) {
                std::map<std::string,float> obj;
                floatObj(obj);
                out[k] = std::move(obj);
            } else skipVal();
            eat(',');
        }
    }

    // Parse root: {"defaults": {...}, "missions": {...}, unknown: skip}
    void root(std::map<std::string,float>& defs,
              std::map<std::string, std::map<std::string,float>>& missions) {
        if (!eat('{')) return;
        for (;;) {
            ws(); if (p >= e || *p == '}') break;
            auto k = str(); if (!eat(':')) break;
            if      (k == "defaults")  floatObj(defs);
            else if (k == "missions")  missionMap(missions);
            else                       skipVal();
            eat(',');
        }
    }
};

static bool s_hasFile     = false;
static int  s_appliedKeys = 0;
static char s_profilePath[256]  = "data/visual_tuning.json";
static char s_activeMission[80] = "";

static void applyKey(const char* key, float val, int& count) {
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
    } else if (strcmp(key, "waterSkyTintStrength") == 0) {
        if (!mc2::gate::get<"MC2_WATER_SKYTINT">()) {
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
    } else if (strcmp(key, "mechAmbientStrength") == 0) {
        // MISSION-LIGHTING-PROFILE-FIELDS-1: env value-var wins over profile,
        // matching the IBL/water/VFX env-guard convention. Gate
        // MC2_MECH_AMBIENT_V1 still controls whether the term is applied at all.
        if (!mc2::gate::isNonEmpty<"MC2_MECH_AMBIENT_V1_STRENGTH">()) {
            batcher_setMechAmbientStrength(val);
            count++;
        }
    } else if (strcmp(key, "mechSpecularStrength") == 0) {
        if (!mc2::gate::isNonEmpty<"MC2_MECH_SPECULAR_STRENGTH">()) {
            batcher_setMechSpecularStrength(val);
            count++;
        }
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

static void writeJsonFloatObj(FILE* f, const std::map<std::string,float>& obj, int depth) {
    const char* inner = (depth == 1) ? "    " : "      ";
    const char* close = (depth == 1) ? "  "   : "    ";
    mc2::fmt::print(f, "{{\n");
    bool first = true;
    for (auto& [key, value] : obj) {
        if (!first) mc2::fmt::print(f, ",\n");
        first = false;
        mc2::fmt::print(f, "{}\"{}\": {:g}", mc2::fmt::safe(inner), key.c_str(), value);
    }
    mc2::fmt::print(f, "\n{}}}", mc2::fmt::safe(close));
}

static void writeJson(FILE* f,
                      const std::map<std::string,float>& defs,
                      const std::map<std::string, std::map<std::string,float>>& missions) {
    mc2::fmt::print(f, "{{\n  \"defaults\": ");
    writeJsonFloatObj(f, defs, 1);
    mc2::fmt::print(f, ",\n  \"missions\": {{\n");
    bool firstM = true;
    for (auto& [mission, values] : missions) {
        if (!firstM) mc2::fmt::print(f, ",\n");
        firstM = false;
        mc2::fmt::print(f, "    \"{}\": ", mission.c_str());
        writeJsonFloatObj(f, values, 2);
    }
    mc2::fmt::print(f, "\n  }}\n}}\n");
}

} // namespace

void visualTuning_applyProfile(const char* missionName) {
    s_hasFile    = false;
    s_appliedKeys = 0;
    s_activeMission[0] = '\0';
    if (missionName && missionName[0])
        strncpy(s_activeMission, missionName, sizeof(s_activeMission) - 1);

    const std::string pathEnv = mc2::gate::str<"MC2_VISUAL_TUNING_FILE">();   // "" when unset or empty
    const char* path = !pathEnv.empty() ? pathEnv.c_str() : "data/visual_tuning.json";
    strncpy(s_profilePath, path, sizeof(s_profilePath) - 1);

    FILE* f = fopen(path, "r");
    if (!f) return;  // missing file = silent no-op

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 512 * 1024) { fclose(f); return; }

    std::string buf(static_cast<size_t>(sz), '\0');
    fread(&buf[0], 1, static_cast<size_t>(sz), f);
    fclose(f);
    s_hasFile = true;

    std::map<std::string,float> defs;
    std::map<std::string, std::map<std::string,float>> missions;
    TinyJson jp{ buf.c_str(), buf.c_str() + buf.size() };
    jp.root(defs, missions);

    int count = 0;
    for (auto& [key, value] : defs) applyKey(key.c_str(), value, count);

    if (missionName && missionName[0]) {
        auto it = missions.find(missionName);
        if (it != missions.end())
            for (auto& [key, value] : it->second) applyKey(key.c_str(), value, count);
    }

    s_appliedKeys = count;
    mc2::fmt::print_err("[VisualTuning] mission='{}' applied {:d} keys from '{}'\n", mc2::fmt::safe(missionName ? missionName : "(none)"), mc2::fmt::as_i32(count), mc2::fmt::safe(path));
}

const char* visualTuning_getProfilePath()     { return s_profilePath; }
const char* visualTuning_getActiveMission()   { return s_activeMission; }
bool        visualTuning_hasProfileFile()     { return s_hasFile; }
int         visualTuning_getAppliedKeyCount() { return s_appliedKeys; }

bool visualTuning_saveCurrentToMission() {
    if (!s_activeMission[0]) return false;

    // Snapshot all current tunable values.
    std::map<std::string,float> current;
    current["exposure"]                  = gos_GetExposure();
    current["staticPropIblStrength"]     = g_iblShStrength;
    current["waterSkyTintStrength"]      = gos_GetWaterSkyTintStrength();
    current["vfxBrightness"]             = gos_vfx_getBrightness();
    current["vfxAdditiveBrightness"]     = gos_vfx_getAdditiveBrightness();
    current["vfxLitStrength"]            = gos_vfx_getLitStrength();  // VFX-LIT-PARTICLES-MVP-1
    // Post-stack keys the reader already accepts but the writer used to drop
    // (8-vs-13 asymmetry) -- round-trip them so "Set as Mission Defaults" keeps
    // any live bloom/AO tuning instead of silently losing it.
    // MISSION-LIGHTING-PROFILE-FIELDS-1 mech lane.
    current["mechAmbientStrength"]       = batcher_getMechAmbientStrength();
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

    // Read existing file to preserve other missions and defaults.
    std::map<std::string,float> defs;
    std::map<std::string, std::map<std::string,float>> missions;
    FILE* f = fopen(s_profilePath, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f); rewind(f);
        if (sz > 0 && sz <= 512 * 1024) {
            std::string buf(static_cast<size_t>(sz), '\0');
            fread(&buf[0], 1, static_cast<size_t>(sz), f);
            TinyJson jp{ buf.c_str(), buf.c_str() + buf.size() };
            jp.root(defs, missions);
        }
        fclose(f);
    }

    missions[s_activeMission] = current;

    FILE* out = fopen(s_profilePath, "w");
    if (!out) {
        mc2::fmt::print_err("[VisualTuning] cannot write '{}'\n", mc2::fmt::safe(s_profilePath));
        return false;
    }
    writeJson(out, defs, missions);
    fclose(out);

    // Keep state consistent: next Reset to Profile will re-apply what we just saved.
    s_hasFile    = true;
    s_appliedKeys = static_cast<int>(current.size());
    mc2::fmt::print_err("[VisualTuning] saved {:d} keys for mission '{}' to '{}'\n", mc2::fmt::as_i32(static_cast<int>(current.size())), mc2::fmt::safe(s_activeMission), mc2::fmt::safe(s_profilePath));
    return true;
}
