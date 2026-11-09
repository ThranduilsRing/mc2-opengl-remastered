// tools/mc2fx/mc2fx_core.h
//
// Native inspection plus legacy authoring/sampling for the CLI and previewer.
//
// Dependency-light: forward-declares the gosFX/SpecLibrary types; the .cpp
// includes the heavy engine headers.

#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include "fx/fx_catalog.h"

namespace mc2 { namespace particles { class SpecLibrary; } }

namespace mc2fx {

// Native inspection/rebuild never initialize or call the legacy engine.
std::string dumpCatalogJson(const mc2::fx::Catalog& catalog, bool full = false);
// Offline comparison oracle, linked into the console tool only.
bool compareLegacy(const mc2::fx::Catalog& catalog, const std::vector<unsigned char>& bytes,
                   std::string& report);

// ---- legacy curve sampling (GUI previewer and authoring) ----
// One named curve, sampled across particle age in [0,1]. `type` is a short
// human label ("constant"/"linear"/"spline"/"complex"/"seeded"). `samples`
// holds N evenly-spaced values: samples[k] = curve(age = k/(N-1), seed).
struct SampledCurve {
    std::string name;                 // member name, e.g. "m_pAlpha"
    std::string type;                 // curve-kind label
    std::string group;               // "color" | "scale" | "lifetime" | "emission" | "seed" | "base"
    bool        constant = false;     // true if the age curve is a flat constant
    float       constValue = 0.0f;    // the value if constant (else samples[0])
    std::vector<float> samples;       // N sampled values over age 0..1
};

// Sample ALL decodable curves of the spec at `index` in `lib` over `count` ages
// in [0,1] at fixed `seed`. typeName (classIdName) returned via *typeNameOut if
// non-null; *nameOut receives the effect's m_name. Specs whose subclass has no
// decodable curves return only the base curves (lifeSpan/child-seed). Never
// crashes on an unknown type. Single source of truth shared by dumpFullJson and
// the GUI previewer.
std::vector<SampledCurve> sampleEffectCurves(mc2::particles::SpecLibrary* lib,
                                             unsigned index,
                                             int count = 128, float seed = 0.5f,
                                             const char** typeNameOut = nullptr,
                                             const char** nameOut = nullptr);

// classID int -> human type name (e.g. "Card", "ParticleCloud", "Unknown").
const char* effectClassName(unsigned classID);

// One catalog row (index/name/type) without exposing gosFX headers to callers.
struct CatalogEntry {
    unsigned    index;
    unsigned    classID;
    std::string name;
    std::string typeName;
};

// Flat catalog of all effects in `lib` (index/name/classID/typeName). Lets the
// GUI build its effect list without including any gosFX/engine header.
std::vector<CatalogEntry> effectCatalog(mc2::particles::SpecLibrary* lib);

// Effect count (thin wrapper so callers need no SpecLibrary header).
unsigned effectCount(mc2::particles::SpecLibrary* lib);

// One-time headless engine bring-up (Stuff -> MLR -> MLRTexturePool -> gosFX).
// Idempotent: safe to call more than once; only the first call does work.
void initEngineHeadless();

// Load a .fx blob into the SpecLibrary singleton. initEngineHeadless() must
// have run. Returns the loaded singleton (or nullptr on failure). The singleton
// requires an empty library (SpecLibrary::Load Verify(!m_effects.GetLength())),
// so this can only be called once per process.
mc2::particles::SpecLibrary* loadBlob(const unsigned char* bytes, size_t n);

// Shallow effect-catalog JSON (index/effectID/classID/name) for a loaded lib.
std::string dumpCatalogJson(mc2::particles::SpecLibrary* lib);

// Rich per-effect JSON (`dump --full`): decodes the editable base curves
// (m_lifeSpan, m_minimumChildSeed, m_maximumChildSeed) plus the common billboard
// subclass curves (Singleton/Card colors+scale, Card halfHeight/aspectRatio/index,
// ParticleCloud family particlesPerSecond/pLifeSpan/p-colors/startingSpeed). Each
// curve emits its type + stored values. Unknown subclasses get a decoded:false
// marker; never crashes on an unknown type.
std::string dumpFullJson(mc2::particles::SpecLibrary* lib);

// ---- build / patch ----------------------------------------------------------
// One field edit parsed from a patch.json. `curveType` is "constant" (only type
// supported curve type). `value` is the scalar to store.
struct PatchEdit {
    std::string effect;     // effect name to match (m_name)
    std::string field;      // member name, e.g. "m_lifeSpan", "m_scale"
    std::string curveType;  // "constant"
    double      value;
};

// Parse a patch.json (limited schema, see main.cpp/usage) into a flat edit list.
// Returns false + fills `err` on a parse error. Tiny hand parser; no nlohmann.
bool parsePatchJson(const std::string& text, std::vector<PatchEdit>& edits,
                    std::string& err);

// Apply edits to a loaded library in place. For each edit, locate the named curve
// member on the matched effect and overwrite its stored constant value. Appends a
// human-readable line per edit (applied / not-found / unsupported) to `report`.
// Returns the number of edits actually applied.
unsigned applyPatch(mc2::particles::SpecLibrary* lib,
                    const std::vector<PatchEdit>& edits, std::string& report);

// Re-emit the loaded library to a byte blob via SpecLibrary::Save. Returns true
// on success; `out` receives exactly the bytes the Save path wrote.
//
// `reserveHint` pre-sizes the backing stream buffer (it grows on demand since the
// DynamicMemoryStream::WriteBytes override was fixed, BUGS row 79; a large hint
// avoids the reallocations).
bool saveBlob(mc2::particles::SpecLibrary* lib, std::vector<unsigned char>& out,
              size_t reserveHint = 0);

// ---- authoring (clone / new) ------------------------------------------------
// These author NEW specs by STREAM-SPLICE. The SpecLibrary's m_effects array is
// private and frozen after Load (no public Append), so instead of mutating it we
// (a) serialize a new spec on its own, and (b) splice it onto the saved base blob
// while bumping the spec-count field. The loader then reads count+1 specs. No
// SpecLibrary internals are touched.

// Serialize ONE spec to its own byte stream exactly as SpecLibrary::Save would
// emit it inside the spec loop (classID + name + curves + MLR state). `out`
// receives those bytes. Returns true on success. The virtual Save dispatches to
// the correct subclass, so the result is a fully-formed spec record.
bool saveOneSpecBytes(mc2::particles::SpecLibrary* lib, unsigned index,
                      std::vector<unsigned char>& out);

// CLONE: duplicate the spec named `srcName` under `newName`. Strategy: Save the
// src spec alone, RE-PARSE those bytes via Effect__Specification::Create (the
// loader's own path -> exact correct subclass, fully type-agnostic, no per-class
// Copy plumbing), overwrite m_name, then Save the clone. `out` receives the new
// spec's record bytes. Returns 0 on success; nonzero error code:
//   1 = src not found, 2 = newName already exists, 3 = save/reparse failure.
int cloneSpecBytes(mc2::particles::SpecLibrary* lib, const char* srcName,
                   const char* newName, std::vector<unsigned char>& out);

// NEW: construct a blank spec of `typeName` (e.g. "Card", "ParticleCloud"),
// call BuildDefaults(), set m_name=newName, then Save. `out` receives the record
// bytes. Returns 0 on success; nonzero error:
//   1 = newName already exists, 2 = unsupported/unconstructable type headless,
//   3 = BuildDefaults/Save fault. On fault, `err` describes where.
int newSpecBytes(mc2::particles::SpecLibrary* lib, const char* typeName,
                 const char* newName, std::vector<unsigned char>& out,
                 std::string& err);

// Splice `newSpecBytes` onto `baseBlob` (a full SpecLibrary::Save blob): bump the
// 32-bit spec count (read side: 4 bytes right after the GFX header) by +1 and
// append the new record. `countByteOffset` is where the uint32 count lives in the
// saved blob (determined by probing the header). Returns the spliced blob.
std::vector<unsigned char> spliceSpec(const std::vector<unsigned char>& baseBlob,
                                      const std::vector<unsigned char>& newRecord,
                                      size_t countByteOffset, unsigned newCount);

}  // namespace mc2fx
