// tools/mc2fx/mc2fx_core.cpp
//
// Implementation of the mc2fx reusable load/dump/save core. The heavy engine
// headers live here so mc2fx_core.h stays dependency-light.

#define _CRT_SECURE_NO_WARNINGS
#include "mc2fx_core.h"

#include <cstdio>
#include <cstring>

#include "gosfxheaders.hpp"          // pulls gosfx.hpp + Stuff + MLR
#include "particles/spec_library.h"
#include "gosfx/effect.hpp"
#include "gosfx/singleton.hpp"
#include "gosfx/card.hpp"
#include "gosfx/particlecloud.hpp"
#include "gosfx/pointcloud.hpp"
#include "gosfx/spinningcloud.hpp"
#include "gosfx/shardcloud.hpp"
#include "gosfx/cardcloud.hpp"
#include "gosfx/effectcloud.hpp"
#include "gosfx/tube.hpp"
#include "gosfx/debriscloud.hpp"
#include "gosfx/pointlight.hpp"
#include "mlr/mlr.hpp"
#include "mlr/mlrtexturepool.hpp"
#include "mlr/gosimagepool.hpp"
#include <windows.h>

#include <cstdlib>
#include <cmath>

// Heap installer provided by mc2fx_stubs.cpp.
extern void InstallMc2fxHeaps();

namespace mc2fx {

namespace {

bool g_engineInited = false;

std::string jsonEscape(const char* s)
{
    std::string r;
    if (!s) return r;
    for (const char* p = s; *p; ++p) {
        char c = *p;
        if (c == '"' || c == '\\') { r.push_back('\\'); r.push_back(c); }
        else if (c == '\n') r += "\\n";
        else if (c == '\t') r += "\\t";
        else r.push_back(c);
    }
    return r;
}

// classID int -> human type name (gosfx.hpp ClassID enum order).
const char* classIdName(unsigned id)
{
    switch (id) {
        case gosFX::EffectClassID:        return "Effect";
        case gosFX::ParticleCloudClassID: return "ParticleCloud";
        case gosFX::PointCloudClassID:    return "PointCloud";
        case gosFX::SpinningCloudClassID: return "SpinningCloud";
        case gosFX::ShardCloudClassID:    return "ShardCloud";
        case gosFX::PertCloudClassID:     return "PertCloud";
        case gosFX::CardCloudClassID:     return "CardCloud";
        case gosFX::ShapeCloudClassID:    return "ShapeCloud";
        case gosFX::EffectCloudClassID:   return "EffectCloud";
        case gosFX::SingletonClassID:     return "Singleton";
        case gosFX::CardClassID:          return "Card";
        case gosFX::ShapeClassID:         return "Shape";
        case gosFX::TubeClassID:          return "Tube";
        case gosFX::DebrisCloudClassID:   return "DebrisCloud";
        case gosFX::PointLightClassID:    return "PointLight";
        default:                          return "Unknown";
    }
}

// JSON-format a finite scalar (avoid "nan"/"inf" in output).
void emitNum(std::string& out, double v)
{
    char b[64];
    if (!std::isfinite(v)) { out += "0.0"; return; }
    std::snprintf(b, sizeof b, "%.6g", v);
    out += b;
}

// --- curve value emitters -------------------------------------------------
// Each emits a JSON object describing the curve's type + stored values. All
// Curve subclass value members (m_value/m_slope/m_a/m_b, ComplexCurve keys via
// public GetKeyCount/operator[]) are public, so no engine edit is needed.

void emitConstant(std::string& out, gosFX::ConstantCurve& c)
{
    out += "{ \"type\": \"constant\", \"value\": ";
    emitNum(out, c.m_value);
    out += " }";
}

void emitLinear(std::string& out, gosFX::LinearCurve& c)
{
    out += "{ \"type\": \"linear\", \"value\": ";
    emitNum(out, c.m_value);
    out += ", \"slope\": ";
    emitNum(out, c.m_slope);
    out += " }";
}

void emitSpline(std::string& out, gosFX::SplineCurve& c)
{
    out += "{ \"type\": \"spline\", \"value\": ";
    emitNum(out, c.m_value);
    out += ", \"slope\": ";
    emitNum(out, c.m_slope);
    out += ", \"a\": ";
    emitNum(out, c.m_a);
    out += ", \"b\": ";
    emitNum(out, c.m_b);
    out += " }";
}

void emitComplex(std::string& out, gosFX::ComplexCurve& c)
{
    out += "{ \"type\": \"complex\", \"keys\": [";
    int n = c.GetKeyCount();
    for (int i = 0; i < n; ++i) {
        gosFX::CurveKey& k = c[i];
        if (i) out += ",";
        out += " { \"time\": ";
        emitNum(out, k.m_time);
        out += ", \"value\": ";
        emitNum(out, k.m_value);
        out += ", \"slope\": ";
        emitNum(out, k.m_slope);
        out += " }";
    }
    out += " ] }";
}

// SeededCurveOf<C,S,type>: emit { seeded, age:<curve>, seed:<curve> }.
template <class C, class S, gosFX::Curve::CurveType T>
void emitSeeded(std::string& out, gosFX::SeededCurveOf<C, S, T>& c,
                void (*ageEmit)(std::string&, C&),
                void (*seedEmit)(std::string&, S&))
{
    out += "{ \"type\": \"seeded\", \"seeded\": ";
    out += c.m_seeded ? "true" : "false";
    out += ", \"age\": ";
    ageEmit(out, c.m_ageCurve);
    out += ", \"seed\": ";
    seedEmit(out, c.m_seedCurve);
    out += " }";
}

// emit a `"field": <curve>,` line with indent.
void emitField(std::string& out, const char* name, const std::string& curveJson,
               bool trailingComma)
{
    out += "        \"";
    out += name;
    out += "\": ";
    out += curveJson;
    out += trailingComma ? ",\n" : "\n";
}

// SEH probe helper — MUST be its own function (no C++ unwinding objects in
// scope) per MSVC C2712. Returns 0 on success (bytes via *outBytes), else the
// SEH exception code; *faultAddr set on fault.
unsigned trySaveSpecInner(gosFX::Effect::Specification* spec,
                          Stuff::DynamicMemoryStream* s,
                          size_t* outBytes, void** faultAddr)
{
    __try {
        spec->Save(s);
        *outBytes = (size_t)s->GetBytesUsed();
        return 0;
    } __except (*faultAddr = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

unsigned trySaveSpec(gosFX::Effect::Specification* spec, size_t* outBytes, void** faultAddr)
{
    // Stream allocated/freed OUTSIDE the SEH frame so trySaveSpecInner has no
    // destructible automatics (MSVC C2712). Leak on fault (diagnostic-only).
    // Born-large per-spec: see saveBlob note — base WriteBytes never grows.
    Stuff::DynamicMemoryStream* s = new Stuff::DynamicMemoryStream(static_cast<DWORD>(4u * 1024u * 1024u));
    unsigned code = trySaveSpecInner(spec, s, outBytes, faultAddr);
    if (code == 0) delete s;
    return code;
}

// --- curve type label ------------------------------------------------------
template <class C> const char* curveTypeLabel(C&) { return "curve"; }
const char* curveTypeLabel(gosFX::ConstantCurve&) { return "constant"; }
const char* curveTypeLabel(gosFX::LinearCurve&)   { return "linear"; }
const char* curveTypeLabel(gosFX::SplineCurve&)   { return "spline"; }
const char* curveTypeLabel(gosFX::ComplexCurve&)  { return "complex"; }

// Is a curve a flat constant? (ConstantCurve is; others only if degenerate.)
bool curveIsConstant(gosFX::ConstantCurve&) { return true; }
bool curveIsConstant(gosFX::LinearCurve& c) { return c.m_slope == 0.0f; }
bool curveIsConstant(gosFX::SplineCurve& c) {
    return c.m_slope == 0.0f && c.m_a == 0.0f && c.m_b == 0.0f;
}
bool curveIsConstant(gosFX::ComplexCurve& c) { return c.GetKeyCount() <= 1; }

// Sample one plain (non-seeded) age curve across [0,1].
template <class C>
mc2fx::SampledCurve samplePlain(const char* name, const char* group, C& curve,
                                int count)
{
    mc2fx::SampledCurve sc;
    sc.name = name; sc.group = group;
    sc.type = curveTypeLabel(curve);
    sc.constant = curveIsConstant(curve);
    sc.samples.resize(count);
    for (int k = 0; k < count; ++k) {
        float age = (count > 1) ? float(k) / float(count - 1) : 0.0f;
        sc.samples[k] = (float)curve.ComputeValue(age, 0.0f);
    }
    sc.constValue = count ? sc.samples[0] : 0.0f;
    return sc;
}

// Sample one SeededCurveOf<...> across [0,1] at fixed seed. Reports the AGE
// curve's type (the editable part), and marks constant from the age curve.
template <class C, class S, gosFX::Curve::CurveType T>
mc2fx::SampledCurve sampleSeeded(const char* name, const char* group,
                                 gosFX::SeededCurveOf<C, S, T>& curve,
                                 int count, float seed)
{
    mc2fx::SampledCurve sc;
    sc.name = name; sc.group = group;
    sc.type = std::string("seeded(") + curveTypeLabel(curve.m_ageCurve) + ")";
    sc.constant = curveIsConstant(curve.m_ageCurve) && !curve.m_seeded;
    sc.samples.resize(count);
    for (int k = 0; k < count; ++k) {
        float age = (count > 1) ? float(k) / float(count - 1) : 0.0f;
        sc.samples[k] = (float)curve.ComputeValue(age, seed);
    }
    sc.constValue = count ? sc.samples[0] : 0.0f;
    return sc;
}

}  // namespace

std::vector<SampledCurve> sampleEffectCurves(mc2::particles::SpecLibrary* lib,
                                             unsigned index, int count, float seed,
                                             const char** typeNameOut,
                                             const char** nameOut)
{
    std::vector<SampledCurve> out;
    if (!lib || index >= lib->Count() || count < 1) return out;
    gosFX::Effect::Specification* spec = lib->At(index);
    if (!spec) return out;
    unsigned classID = static_cast<unsigned>(spec->GetClassID());
    if (typeNameOut) *typeNameOut = classIdName(classID);
    if (nameOut) *nameOut = (spec->m_name) ? static_cast<const char*>(spec->m_name) : "";

    // Base curves — present on every spec.
    out.push_back(samplePlain("m_lifeSpan", "lifetime", spec->m_lifeSpan, count));
    out.push_back(samplePlain("m_minimumChildSeed", "seed", spec->m_minimumChildSeed, count));
    out.push_back(samplePlain("m_maximumChildSeed", "seed", spec->m_maximumChildSeed, count));

    if (classID == gosFX::SingletonClassID || classID == gosFX::CardClassID ||
        classID == gosFX::ShapeClassID) {
        gosFX::Singleton__Specification* s =
            static_cast<gosFX::Singleton__Specification*>(spec);
        out.push_back(sampleSeeded("m_red",   "color", s->m_red,   count, seed));
        out.push_back(sampleSeeded("m_green", "color", s->m_green, count, seed));
        out.push_back(sampleSeeded("m_blue",  "color", s->m_blue,  count, seed));
        out.push_back(sampleSeeded("m_alpha", "color", s->m_alpha, count, seed));
        out.push_back(sampleSeeded("m_scale", "scale", s->m_scale, count, seed));
        if (classID == gosFX::CardClassID) {
            gosFX::Card__Specification* cs =
                static_cast<gosFX::Card__Specification*>(spec);
            out.push_back(sampleSeeded("m_halfHeight",  "scale", cs->m_halfHeight,  count, seed));
            out.push_back(sampleSeeded("m_aspectRatio", "scale", cs->m_aspectRatio, count, seed));
            out.push_back(sampleSeeded("m_index",       "base",  cs->m_index,       count, seed));
        }
    } else if (classID == gosFX::ParticleCloudClassID ||
               classID == gosFX::PointCloudClassID ||
               classID == gosFX::SpinningCloudClassID ||
               classID == gosFX::ShardCloudClassID ||
               classID == gosFX::PertCloudClassID ||
               classID == gosFX::CardCloudClassID ||
               classID == gosFX::ShapeCloudClassID ||
               classID == gosFX::EffectCloudClassID) {
        gosFX::ParticleCloud__Specification* p =
            static_cast<gosFX::ParticleCloud__Specification*>(spec);
        out.push_back(samplePlain ("m_particlesPerSecond", "emission", p->m_particlesPerSecond, count));
        out.push_back(sampleSeeded("m_startingSpeed", "base",     p->m_startingSpeed, count, seed));
        out.push_back(sampleSeeded("m_pLifeSpan",     "lifetime", p->m_pLifeSpan,     count, seed));
        out.push_back(sampleSeeded("m_pRed",   "color", p->m_pRed,   count, seed));
        out.push_back(sampleSeeded("m_pGreen", "color", p->m_pGreen, count, seed));
        out.push_back(sampleSeeded("m_pBlue",  "color", p->m_pBlue,  count, seed));
        out.push_back(sampleSeeded("m_pAlpha", "color", p->m_pAlpha, count, seed));
    }
    // else: base-only (DebrisCloud/Tube/PointLight/Effect/unknown).
    return out;
}

const char* effectClassName(unsigned classID) { return classIdName(classID); }

unsigned effectCount(mc2::particles::SpecLibrary* lib)
{
    return lib ? lib->Count() : 0u;
}

std::vector<CatalogEntry> effectCatalog(mc2::particles::SpecLibrary* lib)
{
    std::vector<CatalogEntry> out;
    if (!lib) return out;
    unsigned n = lib->Count();
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        gosFX::Effect::Specification* spec = lib->At(i);
        CatalogEntry e;
        e.index = i;
        e.classID = spec ? static_cast<unsigned>(spec->GetClassID()) : 0u;
        e.typeName = classIdName(e.classID);
        e.name = (spec && spec->m_name) ? static_cast<const char*>(spec->m_name) : "";
        out.push_back(std::move(e));
    }
    return out;
}

void initEngineHeadless()
{
    if (g_engineInited) return;
    g_engineInited = true;

    // --- engine bring-up (headless) ---
    InstallMc2fxHeaps();
    // Stuff master class registry MUST init first; the gosFX spec factory
    // dispatch reads its registry.
    Stuff::InitializeClasses();
    // MLR limits mirror the engine's gameos init; not load-critical.
    MidLevelRenderer::InitializeClasses(64u * 1024u, 16u * 1024u, 1024u, 4096u, true);
    // MLRState::Load resolves texture NAMES through MLRTexturePool::Instance.
    gos_PushCurrentHeap(MidLevelRenderer::Heap);
    MidLevelRenderer::MLRTexturePool::Instance =
        new MidLevelRenderer::MLRTexturePool(
            new MidLevelRenderer::TGAFilePool("data\\tgl\\128\\"));
    gos_PopCurrentHeap();

    gosFX::InitializeClasses();
}

mc2::particles::SpecLibrary* loadBlob(const unsigned char* bytes, size_t n)
{
    if (!bytes || n == 0) return nullptr;
    gos_PushCurrentHeap(gosFX::Heap);
    // const_cast: MemoryStream takes a non-const void* but only reads on Load.
    Stuff::MemoryStream stream(const_cast<unsigned char*>(bytes),
                               static_cast<DWORD>(n));
    mc2::particles::SpecLibrary* lib = mc2::particles::SpecLibrary::Instance();
    lib->Load(&stream);
    gos_PopCurrentHeap();
    return lib;
}

std::string dumpCatalogJson(mc2::particles::SpecLibrary* lib)
{
    std::string out;
    if (!lib) return out;
    unsigned count = lib->Count();
    out.reserve(count * 64 + 128);
    char line[256];
    std::snprintf(line, sizeof line, "{\n  \"count\": %u,\n  \"effects\": [\n", count);
    out += line;
    for (unsigned i = 0; i < count; ++i) {
        gosFX::Effect::Specification* spec = lib->At(i);
        const char* name = (spec && spec->m_name) ? static_cast<const char*>(spec->m_name) : "";
        unsigned classID = spec ? static_cast<unsigned>(spec->GetClassID()) : 0u;
        unsigned effectID = spec ? spec->m_effectID : 0u;
        std::snprintf(line, sizeof line,
            "    { \"index\": %u, \"effectID\": %u, \"classID\": %u, \"name\": \"%s\" }%s\n",
            i, effectID, classID, jsonEscape(name).c_str(), (i + 1 < count) ? "," : "");
        out += line;
    }
    out += "  ]\n}\n";
    return out;
}

std::string dumpFullJson(mc2::particles::SpecLibrary* lib)
{
    std::string out;
    if (!lib) return out;
    unsigned count = lib->Count();
    out.reserve(count * 256 + 128);
    char line[256];
    std::snprintf(line, sizeof line, "{\n  \"count\": %u,\n  \"effects\": [\n", count);
    out += line;

    for (unsigned i = 0; i < count; ++i) {
        gosFX::Effect::Specification* spec = lib->At(i);
        unsigned classID = spec ? static_cast<unsigned>(spec->GetClassID()) : 0u;
        const char* name = (spec && spec->m_name) ? static_cast<const char*>(spec->m_name) : "";
        const char* tn = classIdName(classID);

        out += "    {\n";
        std::snprintf(line, sizeof line,
            "      \"index\": %u, \"effectID\": %u, \"classID\": %u,\n"
            "      \"typeName\": \"%s\", \"name\": \"%s\",\n",
            i, spec ? spec->m_effectID : 0u, classID, tn, jsonEscape(name).c_str());
        out += line;

        // Base Effect__Specification curves — present on every spec.
        out += "      \"decoded\": true,\n      \"fields\": {\n";
        std::string c;
        c.clear(); emitConstant(c, spec->m_lifeSpan);          emitField(out, "m_lifeSpan", c, true);
        c.clear(); emitSpline(c, spec->m_minimumChildSeed);    emitField(out, "m_minimumChildSeed", c, true);
        c.clear(); emitSpline(c, spec->m_maximumChildSeed);    emitField(out, "m_maximumChildSeed", c, false);

        // Billboard subclass curves.
        if (classID == gosFX::SingletonClassID || classID == gosFX::CardClassID ||
            classID == gosFX::ShapeClassID) {
            // Singleton + its subclasses (Card, Shape) share the color/scale block.
            gosFX::Singleton__Specification* s =
                static_cast<gosFX::Singleton__Specification*>(spec);
            out.erase(out.size() - 1);  // turn last "\n" into ",\n"
            out += ",\n";
            c.clear(); emitSeeded(c, s->m_red,   emitComplex, emitLinear); emitField(out, "m_red",   c, true);
            c.clear(); emitSeeded(c, s->m_green, emitComplex, emitLinear); emitField(out, "m_green", c, true);
            c.clear(); emitSeeded(c, s->m_blue,  emitComplex, emitLinear); emitField(out, "m_blue",  c, true);
            c.clear(); emitSeeded(c, s->m_alpha, emitComplex, emitLinear); emitField(out, "m_alpha", c, true);
            c.clear(); emitSeeded(c, s->m_scale, emitComplex, emitComplex);
            emitField(out, "m_scale", c, classID == gosFX::CardClassID);

            if (classID == gosFX::CardClassID) {
                gosFX::Card__Specification* cs =
                    static_cast<gosFX::Card__Specification*>(spec);
                c.clear(); emitSeeded(c, cs->m_halfHeight,  emitConstant, emitComplex); emitField(out, "m_halfHeight",  c, true);
                c.clear(); emitSeeded(c, cs->m_aspectRatio, emitConstant, emitComplex); emitField(out, "m_aspectRatio", c, true);
                c.clear(); emitSeeded(c, cs->m_index,       emitComplex,  emitSpline);  emitField(out, "m_index",       c, false);
            }
        } else if (classID == gosFX::ParticleCloudClassID ||
                   classID == gosFX::PointCloudClassID ||
                   classID == gosFX::SpinningCloudClassID ||
                   classID == gosFX::ShardCloudClassID ||
                   classID == gosFX::PertCloudClassID ||
                   classID == gosFX::CardCloudClassID ||
                   classID == gosFX::ShapeCloudClassID ||
                   classID == gosFX::EffectCloudClassID) {
            // All derive (transitively) from ParticleCloud__Specification.
            // NOTE: DebrisCloud/Tube/PointLight do NOT — they derive directly
            // from Effect__Specification, so they get base fields only.
            gosFX::ParticleCloud__Specification* p =
                static_cast<gosFX::ParticleCloud__Specification*>(spec);
            out.erase(out.size() - 1);
            out += ",\n";
            c.clear(); emitComplex(c, p->m_particlesPerSecond);                  emitField(out, "m_particlesPerSecond", c, true);
            c.clear(); emitSeeded(c, p->m_startingSpeed, emitComplex, emitComplex); emitField(out, "m_startingSpeed", c, true);
            c.clear(); emitSeeded(c, p->m_pLifeSpan, emitComplex, emitSpline);    emitField(out, "m_pLifeSpan", c, true);
            c.clear(); emitSeeded(c, p->m_pRed,   emitComplex, emitLinear);       emitField(out, "m_pRed",   c, true);
            c.clear(); emitSeeded(c, p->m_pGreen, emitComplex, emitLinear);       emitField(out, "m_pGreen", c, true);
            c.clear(); emitSeeded(c, p->m_pBlue,  emitComplex, emitLinear);       emitField(out, "m_pBlue",  c, true);
            c.clear(); emitSeeded(c, p->m_pAlpha, emitComplex, emitLinear);       emitField(out, "m_pAlpha", c, false);
        }
        // else: base-only fields already emitted; not a decoded subclass but
        // base curves are still valid. Mark partial via "subclassDecoded".

        out += "      }\n";
        out += (i + 1 < count) ? "    },\n" : "    }\n";
    }
    out += "  ]\n}\n";
    return out;
}

bool saveBlob(mc2::particles::SpecLibrary* lib, std::vector<unsigned char>& out,
              size_t reserveHint)
{
    if (!lib) return false;
    // ASan ground truth (memorystream.cpp:275): the Save path writes through the
    // virtual MemoryStream::WriteBytes(const void*, size_t). DynamicMemoryStream's
    // would-be growing override is declared WriteBytes(const void*, DWORD); on x64
    // DWORD(32) != size_t(64), so it does NOT override the size_t virtual and is
    // never dispatched on this path. The base writer does a raw Mem_Copy +
    // AdvancePointer with NO reallocation -> a buffer born too small overruns the
    // heap (the non-deterministic __fastfail). Fix tool-side by pre-sizing the
    // stream large enough to hold the whole blob so the base writer never overruns.
    gos_PushCurrentHeap(gosFX::Heap);
    if (std::getenv("MC2FX_SAVE_TRACE")) {
        // Per-spec probe: localize which effect's Save faults, with SEH so a
        // fault reports the address instead of killing the process.
        for (unsigned i = 0; i < lib->Count(); ++i) {
            std::fprintf(stderr, "[save] spec %u/%u cls=%u ...", i, lib->Count(),
                         (unsigned)lib->At(i)->GetClassID());
            std::fflush(stderr);
            size_t nb = 0; void* addr = nullptr;
            unsigned code = trySaveSpec(lib->At(i), &nb, &addr);
            if (code == 0) std::fprintf(stderr, " ok (%zu B)\n", nb);
            else std::fprintf(stderr, " FAULT code=0x%08X at %p\n", code, addr);
            std::fflush(stderr);
        }
    }
    // Born-large: base WriteBytes never grows the buffer (see note above).
    DWORD reserve = reserveHint ? static_cast<DWORD>(reserveHint) : (8u * 1024u * 1024u);
    Stuff::DynamicMemoryStream stream(reserve);
    stream.Rewind();  // streamSize stays = reserve (capacity); pos back to start
    lib->Save(&stream);
    size_t used = stream.GetBytesUsed();
    bool ok = false;
    if (used > 0) {
        stream.Rewind();
        const unsigned char* base = static_cast<const unsigned char*>(stream.GetPointer());
        out.assign(base, base + used);
        ok = true;
    }
    gos_PopCurrentHeap();
    return ok;
}

// ---------------------------------------------------------------------------
// authoring (clone / new) — stream-splice
// ---------------------------------------------------------------------------

namespace {

// classID for a human type name (inverse of classIdName). 0 = unknown.
unsigned classIdForName(const std::string& t)
{
    if (t == "Effect")        return gosFX::EffectClassID;
    if (t == "ParticleCloud") return gosFX::ParticleCloudClassID;
    if (t == "PointCloud")    return gosFX::PointCloudClassID;
    if (t == "SpinningCloud") return gosFX::SpinningCloudClassID;
    if (t == "ShardCloud")    return gosFX::ShardCloudClassID;
    if (t == "PertCloud")     return gosFX::PertCloudClassID;
    if (t == "CardCloud")     return gosFX::CardCloudClassID;
    if (t == "ShapeCloud")    return gosFX::ShapeCloudClassID;
    if (t == "EffectCloud")   return gosFX::EffectCloudClassID;
    if (t == "Singleton")     return gosFX::SingletonClassID;
    if (t == "Card")          return gosFX::CardClassID;
    if (t == "Shape")         return gosFX::ShapeClassID;
    if (t == "Tube")          return gosFX::TubeClassID;
    if (t == "DebrisCloud")   return gosFX::DebrisCloudClassID;
    if (t == "PointLight")    return gosFX::PointLightClassID;
    return 0u;
}

// Save a single spec to its own DynamicMemoryStream (born-large; the base
// WriteBytes never grows — see saveBlob note). Returns the record bytes. SEH-free
// here; callers that fear a fault should route through the build path's existing
// SEH probe. Returns false on empty output.
bool saveSpecToBytes(gosFX::Effect::Specification* spec, std::vector<unsigned char>& out)
{
    if (!spec) return false;
    gos_PushCurrentHeap(gosFX::Heap);
    Stuff::DynamicMemoryStream stream(8u * 1024u * 1024u);
    stream.Rewind();
    spec->Save(&stream);
    size_t used = stream.GetBytesUsed();
    bool ok = false;
    if (used > 0) {
        stream.Rewind();
        const unsigned char* base = static_cast<const unsigned char*>(stream.GetPointer());
        out.assign(base, base + used);
        ok = true;
    }
    gos_PopCurrentHeap();
    return ok;
}

// Re-parse a saved single-spec record back into a fresh spec object via the
// loader's own Create() path (exact subclass, type-agnostic). Caller owns/leaks
// the returned object (diagnostic tool; process is short-lived).
gosFX::Effect::Specification* reparseSpec(const std::vector<unsigned char>& rec)
{
    gos_PushCurrentHeap(gosFX::Heap);
    Stuff::MemoryStream stream(const_cast<unsigned char*>(rec.data()),
                               static_cast<DWORD>(rec.size()));
    gosFX::Effect::Specification* spec =
        gosFX::Effect::Specification::Create(&stream, gosFX::CurrentGFXVersion);
    gos_PopCurrentHeap();
    return spec;
}

}  // namespace

bool saveOneSpecBytes(mc2::particles::SpecLibrary* lib, unsigned index,
                      std::vector<unsigned char>& out)
{
    if (!lib || index >= lib->Count()) return false;
    return saveSpecToBytes(lib->At(index), out);
}

int cloneSpecBytes(mc2::particles::SpecLibrary* lib, const char* srcName,
                   const char* newName, std::vector<unsigned char>& out)
{
    if (!lib || !srcName || !newName) return 3;
    gosFX::Effect::Specification* src = lib->Find(srcName);
    if (!src) return 1;
    if (lib->Find(newName)) return 2;

    // Save src alone -> reparse via Create (correct subclass, all curves) ->
    // rename -> re-save. Fully type-agnostic; no per-class Copy plumbing.
    std::vector<unsigned char> srcRec;
    if (!saveSpecToBytes(src, srcRec)) return 3;
    gosFX::Effect::Specification* clone = reparseSpec(srcRec);
    if (!clone) return 3;
    gos_PushCurrentHeap(gosFX::Heap);
    clone->m_name = newName;
    gos_PopCurrentHeap();
    if (!saveSpecToBytes(clone, out)) return 3;
    return 0;
}

int newSpecBytes(mc2::particles::SpecLibrary* lib, const char* typeName,
                 const char* newName, std::vector<unsigned char>& out,
                 std::string& err)
{
    if (!lib || !typeName || !newName) { err = "null arg"; return 3; }
    if (lib->Find(newName)) { err = "name exists"; return 1; }
    unsigned classID = classIdForName(typeName);
    if (classID == 0u) { err = std::string("unknown type '") + typeName + "'"; return 2; }

    // ParticleCloud / SpinningCloud / Singleton are INTERMEDIATE base classes:
    // their classID has no concrete `__Specification::Make` factory, so a blank
    // spec of those types reloads via Create() -> fault. They exist only to be
    // subclassed (PointCloud/ShardCloud/.../Card). Reject up front with guidance.
    if (classID == gosFX::ParticleCloudClassID ||
        classID == gosFX::SpinningCloudClassID ||
        classID == gosFX::SingletonClassID) {
        err = std::string("type '") + typeName +
              "' is an abstract base class (no concrete factory; unloadable). "
              "Use a concrete leaf type (PointCloud/ShardCloud/CardCloud/"
              "EffectCloud/Card/Tube/DebrisCloud/PointLight).";
        return 2;
    }

    gos_PushCurrentHeap(gosFX::Heap);
    gosFX::Effect::Specification* spec = nullptr;
    // Only concrete leaf types with a headless-constructable ctor (no MLRShape/
    // asset needed).
    switch (classID) {
        case gosFX::PointCloudClassID:
            spec = new gosFX::PointCloud__Specification();
            break;
        case gosFX::ShardCloudClassID:
            spec = new gosFX::ShardCloud__Specification();
            break;
        case gosFX::CardCloudClassID:
            spec = new gosFX::CardCloud__Specification();
            break;
        case gosFX::EffectCloudClassID:
            spec = new gosFX::EffectCloud__Specification();
            break;
        case gosFX::CardClassID:
            spec = new gosFX::Card__Specification();
            break;
        case gosFX::TubeClassID:
            spec = new gosFX::Tube__Specification();
            break;
        case gosFX::DebrisCloudClassID:
            spec = new gosFX::DebrisCloud__Specification();
            break;
        case gosFX::PointLightClassID:
            spec = new gosFX::PointLight__Specification();
            break;
        default:
            // PertCloud(sides) / ShapeCloud(MLRShape*) / Shape(MLRShape*) /
            // Effect(base) need an asset or arg -> not authored blank headless.
            gos_PopCurrentHeap();
            err = std::string("type '") + typeName +
                  "' not constructable headless (needs asset/arg)";
            return 2;
    }
    if (!spec) { gos_PopCurrentHeap(); err = "ctor returned null"; return 3; }
    spec->BuildDefaults();
    spec->m_name = newName;
    gos_PopCurrentHeap();

    if (!saveSpecToBytes(spec, out)) { err = "Save produced no bytes"; return 3; }
    return 0;
}

std::vector<unsigned char> spliceSpec(const std::vector<unsigned char>& baseBlob,
                                      const std::vector<unsigned char>& newRecord,
                                      size_t countByteOffset, unsigned newCount)
{
    std::vector<unsigned char> out = baseBlob;
    // overwrite the little-endian uint32 count in place
    if (countByteOffset + 4 <= out.size()) {
        out[countByteOffset + 0] = static_cast<unsigned char>(newCount & 0xFF);
        out[countByteOffset + 1] = static_cast<unsigned char>((newCount >> 8) & 0xFF);
        out[countByteOffset + 2] = static_cast<unsigned char>((newCount >> 16) & 0xFF);
        out[countByteOffset + 3] = static_cast<unsigned char>((newCount >> 24) & 0xFF);
    }
    out.insert(out.end(), newRecord.begin(), newRecord.end());
    return out;
}

// ---------------------------------------------------------------------------
// build / patch
// ---------------------------------------------------------------------------

namespace {

// Minimal JSON scanner for the fixed patch schema:
//   { "edits": [ { "effect": "...", "set": { "<field>": { "type":"constant",
//                  "value": <num> }, ... } }, ... ] }
// Tolerant of whitespace; does NOT validate the whole grammar — it pulls the
// string/number tokens it needs in document order.
struct Scanner {
    const char* p;
    const char* end;
    explicit Scanner(const std::string& s) : p(s.c_str()), end(s.c_str() + s.size()) {}
    void skipWs() { while (p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p; }
    bool eof() { skipWs(); return p >= end; }
    char peek() { skipWs(); return p < end ? *p : '\0'; }
    bool eat(char c) { skipWs(); if (p < end && *p == c) { ++p; return true; } return false; }
    bool parseString(std::string& out) {
        skipWs();
        if (p >= end || *p != '"') return false;
        ++p; out.clear();
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    default:  out.push_back(*p);   break;
                }
            } else out.push_back(*p);
            ++p;
        }
        if (p >= end) return false;
        ++p;  // closing quote
        return true;
    }
    bool parseNumber(double& out) {
        skipWs();
        char* e = nullptr;
        out = std::strtod(p, &e);
        if (e == p) return false;
        p = e;
        return true;
    }
};

}  // namespace

bool parsePatchJson(const std::string& text, std::vector<PatchEdit>& edits,
                    std::string& err)
{
    Scanner sc(text);
    if (!sc.eat('{')) { err = "expected '{' at start"; return false; }
    // find "edits"
    bool foundEdits = false;
    while (!sc.eof()) {
        std::string key;
        if (!sc.parseString(key)) { err = "expected key string"; return false; }
        if (!sc.eat(':')) { err = "expected ':' after key"; return false; }
        if (key == "edits") { foundEdits = true; break; }
        // skip an unknown top-level value crudely: only objects/arrays/strings
        // are expected; bail if encountered.
        err = "unexpected top-level key '" + key + "' before 'edits'";
        return false;
    }
    if (!foundEdits) { err = "no 'edits' array"; return false; }
    if (!sc.eat('[')) { err = "expected '[' for edits array"; return false; }
    if (sc.peek() == ']') { sc.eat(']'); return true; }  // empty

    for (;;) {
        if (!sc.eat('{')) { err = "expected '{' for edit object"; return false; }
        std::string effect;
        bool haveSet = false;
        std::vector<PatchEdit> localFields;
        for (;;) {
            std::string key;
            if (!sc.parseString(key)) { err = "expected key in edit object"; return false; }
            if (!sc.eat(':')) { err = "expected ':' in edit object"; return false; }
            if (key == "effect") {
                if (!sc.parseString(effect)) { err = "effect must be string"; return false; }
            } else if (key == "set") {
                haveSet = true;
                if (!sc.eat('{')) { err = "expected '{' for set"; return false; }
                if (sc.peek() != '}') {
                    for (;;) {
                        std::string field;
                        if (!sc.parseString(field)) { err = "expected field name in set"; return false; }
                        if (!sc.eat(':')) { err = "expected ':' after field"; return false; }
                        if (!sc.eat('{')) { err = "field value must be a curve object"; return false; }
                        PatchEdit pe; pe.field = field; pe.curveType = "constant"; pe.value = 0.0;
                        bool haveVal = false;
                        for (;;) {
                            std::string ck;
                            if (!sc.parseString(ck)) { err = "expected key in curve object"; return false; }
                            if (!sc.eat(':')) { err = "expected ':' in curve object"; return false; }
                            if (ck == "type") {
                                if (!sc.parseString(pe.curveType)) { err = "type must be string"; return false; }
                            } else if (ck == "value") {
                                if (!sc.parseNumber(pe.value)) { err = "value must be number"; return false; }
                                haveVal = true;
                            } else {
                                // skip unknown scalar value
                                double dummy; std::string ds;
                                if (sc.peek() == '"') sc.parseString(ds);
                                else sc.parseNumber(dummy);
                            }
                            if (sc.eat(',')) continue;
                            if (sc.eat('}')) break;
                            err = "malformed curve object"; return false;
                        }
                        (void)haveVal;
                        localFields.push_back(pe);
                        if (sc.eat(',')) continue;
                        if (sc.eat('}')) break;
                        err = "malformed set object"; return false;
                    }
                } else {
                    sc.eat('}');
                }
            } else {
                err = "unexpected key '" + key + "' in edit object"; return false;
            }
            if (sc.eat(',')) continue;
            if (sc.eat('}')) break;
            err = "malformed edit object"; return false;
        }
        if (!haveSet) { err = "edit missing 'set'"; return false; }
        for (auto& f : localFields) { f.effect = effect; edits.push_back(f); }
        if (sc.eat(',')) continue;
        if (sc.eat(']')) break;
        err = "malformed edits array"; return false;
    }
    return true;
}

namespace {

// Set the stored value of a base/billboard curve member by name. Returns:
//   1 = applied, 0 = field not found on this spec, -1 = unsupported curve kind.
// For ConstantCurve we set m_value directly. For Spline/Linear we set m_value
// (the constant term) and zero the higher coefficients so the curve becomes a
// constant. For SeededCurveOf<...> we collapse the age curve to the constant.
int setCurveByName(gosFX::Effect::Specification* spec, unsigned classID,
                   const std::string& field, double v)
{
    Stuff::Scalar val = static_cast<Stuff::Scalar>(v);

    auto setConst = [&](gosFX::ConstantCurve& c) { c.m_value = val; };
    auto setSpline = [&](gosFX::SplineCurve& c) {
        c.m_value = val; c.m_slope = 0.0f; c.m_a = 0.0f; c.m_b = 0.0f;
    };
    // Collapse a ComplexCurve age curve to a single constant key. SetCurve(v)
    // sets one flat key (public on ComplexCurve).
    auto setComplex = [&](gosFX::ComplexCurve& c) { c.SetCurve(val); };

    // --- base fields (every spec) ---
    if (field == "m_lifeSpan")         { setConst(spec->m_lifeSpan);        return 1; }
    if (field == "m_minimumChildSeed") { setSpline(spec->m_minimumChildSeed); return 1; }
    if (field == "m_maximumChildSeed") { setSpline(spec->m_maximumChildSeed); return 1; }

    if (classID == gosFX::SingletonClassID || classID == gosFX::CardClassID ||
        classID == gosFX::ShapeClassID) {
        gosFX::Singleton__Specification* s =
            static_cast<gosFX::Singleton__Specification*>(spec);
        // age curve of these color/scale members is a ComplexCurve.
        if (field == "m_red")   { setComplex(s->m_red.m_ageCurve);   return 1; }
        if (field == "m_green") { setComplex(s->m_green.m_ageCurve); return 1; }
        if (field == "m_blue")  { setComplex(s->m_blue.m_ageCurve);  return 1; }
        if (field == "m_alpha") { setComplex(s->m_alpha.m_ageCurve); return 1; }
        if (field == "m_scale") { setComplex(s->m_scale.m_ageCurve); return 1; }
        if (classID == gosFX::CardClassID) {
            gosFX::Card__Specification* cs =
                static_cast<gosFX::Card__Specification*>(spec);
            if (field == "m_halfHeight")  { setConst(cs->m_halfHeight.m_ageCurve);  return 1; }
            if (field == "m_aspectRatio") { setConst(cs->m_aspectRatio.m_ageCurve); return 1; }
            if (field == "m_index")       { setComplex(cs->m_index.m_ageCurve);     return 1; }
        }
    } else if (classID == gosFX::ParticleCloudClassID ||
               classID == gosFX::PointCloudClassID ||
               classID == gosFX::SpinningCloudClassID ||
               classID == gosFX::ShardCloudClassID ||
               classID == gosFX::PertCloudClassID ||
               classID == gosFX::CardCloudClassID ||
               classID == gosFX::ShapeCloudClassID ||
               classID == gosFX::EffectCloudClassID) {
        gosFX::ParticleCloud__Specification* p =
            static_cast<gosFX::ParticleCloud__Specification*>(spec);
        if (field == "m_particlesPerSecond") { setComplex(p->m_particlesPerSecond);    return 1; }
        if (field == "m_startingSpeed")      { setComplex(p->m_startingSpeed.m_ageCurve); return 1; }
        if (field == "m_pLifeSpan")          { setComplex(p->m_pLifeSpan.m_ageCurve);   return 1; }
        if (field == "m_pRed")   { setComplex(p->m_pRed.m_ageCurve);   return 1; }
        if (field == "m_pGreen") { setComplex(p->m_pGreen.m_ageCurve); return 1; }
        if (field == "m_pBlue")  { setComplex(p->m_pBlue.m_ageCurve);  return 1; }
        if (field == "m_pAlpha") { setComplex(p->m_pAlpha.m_ageCurve); return 1; }
    }
    return 0;  // field not recognized for this spec
}

}  // namespace

unsigned applyPatch(mc2::particles::SpecLibrary* lib,
                    const std::vector<PatchEdit>& edits, std::string& report)
{
    unsigned applied = 0;
    char line[512];
    for (const auto& e : edits) {
        if (e.curveType != "constant") {
            std::snprintf(line, sizeof line,
                "  UNSUPPORTED curve type '%s' for %s.%s (only 'constant')\n",
                e.curveType.c_str(), e.effect.c_str(), e.field.c_str());
            report += line;
            continue;
        }
        gosFX::Effect::Specification* spec = lib->Find(e.effect.c_str());
        if (!spec) {
            std::snprintf(line, sizeof line,
                "  NOT-FOUND effect '%s' (field %s skipped)\n",
                e.effect.c_str(), e.field.c_str());
            report += line;
            continue;
        }
        unsigned classID = static_cast<unsigned>(spec->GetClassID());
        int r = setCurveByName(spec, classID, e.field, e.value);
        if (r == 1) {
            std::snprintf(line, sizeof line,
                "  OK %s.%s = %g (classID %u %s)\n",
                e.effect.c_str(), e.field.c_str(), e.value, classID, classIdName(classID));
            report += line;
            ++applied;
        } else {
            std::snprintf(line, sizeof line,
                "  UNSUPPORTED field '%s' on effect '%s' (classID %u %s)\n",
                e.field.c_str(), e.effect.c_str(), classID, classIdName(classID));
            report += line;
        }
    }
    return applied;
}

}  // namespace mc2fx
