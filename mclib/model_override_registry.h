// model_override_registry.h -- render-only model overrides from model_overrides/models.json.
// Engine-independent parse (no TG_*/GameOS deps). Records are keyed by "<class>:<appearanceName>";
// renderOnly must be true and fallback "stock". Invalid entries are logged and dropped, never fatal.
//
// A record carries what tools/content_pipeline/package.py cooks: the intact LOD chain, an optional
// destroyed chain (the engine renders it when the building is destroyed; collision and passability
// stay stock), condition variants (pristine is the record itself; worn/abandoned are opt-in per
// mission via conditions.json) and per-slot authored material inputs (materials v1/v2).
// Every mounted mod's models.json merges in VFS priority order (lowest first, later wins).
#pragma once
#include "mc2_czstring.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Authored material inputs for one slot. Base colour stays the GLB's texture (stock or authored).
struct ModelOverrideMaterial {
    uint32_t slot = 0;
    bool doubleSided = false;
    uint32_t materialClass = 0;   // RenderCore::SurfaceClass
    uint32_t alphaMode = 0;       // RenderCore::SurfaceAlpha (Stock keeps the stock coverage policy)
    float alphaCutoff = 0.5f;
    std::string normalPath;       // .tga or .ktx2 (BC5 / RGBA8), manifest-relative; empty = flat
    std::string ormPath;          // .tga or .ktx2 (BC7 / RGBA8); empty = class scalars, full coverage
    std::string teamMaskPath;     // .ktx2 (BC7 linear, R = base slot, G = highlight slot, B = secondary slot); empty = no team tint
    std::string albedoPath;       // .ktx2 only (BC7, own mip chain): per-object albedo that replaces the tiered tgl texture; empty = tiered
    float uvScale[2] = {1.0f, 1.0f};
    float normalStrength = 1.0f;
    float aoStrength = 1.0f;
    float roughness = 1.0f;
    float metallic = 0.0f;
};

struct ModelOverrideLod {
    int         lod = 0;          // LOD index (>=1; LOD0 is the chain's own source)
    std::string sourceRelPath;    // .glb/.gltf relative to the manifest dir (validated safe)
    float       distance = 0.0f;  // activation distance; 0 = unset
};

// One state's geometry: LOD0 source plus ascending lower LODs. Empty source = absent.
struct ModelOverrideChain {
    std::string sourceRelPath;
    std::vector<ModelOverrideLod> lods;
    bool present() const { return !sourceRelPath.empty(); }
};

struct ModelOverrideCondition {
    std::string name;             // "worn" | "abandoned"
    ModelOverrideChain intact;
    ModelOverrideChain destroyed; // absent = the record's (pristine) destroyed chain
};

struct ModelOverrideRecord {
    // Explicit opt-in for thin fence coverage composited after vegetation.
    bool lateCoverageFringe = false;
    std::vector<ModelOverrideMaterial> materials;
    std::string overrideClass;    // "staticprop" | "tree" | "vehicle" -- NORMALIZED lowercase
    std::string appearanceName;   // from "replaces" after ':' -- NORMALIZED lowercase
    std::string sourceRelPath;    // pristine intact LOD0 (.glb/.gltf, manifest-relative)
    std::string manifestDir;      // directory from which this record's paths resolve
    float       scale = 1.0f;     // must be exactly 1.0
    std::vector<ModelOverrideLod> lods;        // pristine intact LOD1.. (ascending)
    ModelOverrideChain destroyed;              // pristine destroyed chain (optional)
    std::vector<ModelOverrideCondition> conditions;

    // The condition variant by name; nullptr = pristine (the record's own chains).
    const ModelOverrideCondition* condition(std::string_view name) const;

    // Object-map bodies share a pre-finalize GPU recipe. Without an explicit
    // team mask the shader uses the stock dominant-channel palette classifier.
    bool usesShaderTeamPaint() const {
        for (const auto& m : materials)
            if (!m.albedoPath.empty() || !m.normalPath.empty() || !m.ormPath.empty() || !m.teamMaskPath.empty()) return true;
        return false;
    }
};

class ModelOverrideRegistry {
public:
    // Clears registry then loads from manifestPath. Returns count.
    int loadFromFile(std::string_view manifestPath, std::string_view manifestDir);
    // Additive: loads manifestPath without clearing. Later files WIN on dup key.
    // Returns count of entries merged in (not total).
    int mergeFromFile(std::string_view manifestPath, std::string_view manifestDir);
    // conditions.json: {"version":1,"missions":{"<mission>":{"*":"worn","<appearance>":"abandoned"}}}.
    // Later files win per (mission, appearance). Returns selections merged.
    int mergeConditionsFromFile(std::string_view path);
    // The condition chosen for an appearance in a mission ("" = pristine, the default).
    std::string selectedCondition(std::string_view mission, std::string_view appearanceName) const;
    const ModelOverrideRecord* resolve(mc2::czstring overrideClass, mc2::czstring appearanceName) const;
    int count() const { return static_cast<int>(records_.size()); }
    const std::string& manifestDir() const { return manifestDir_; }
    static ModelOverrideRegistry& instance();
private:
    struct Selection { std::string mission, appearance, condition; };
    std::vector<ModelOverrideRecord> records_;
    std::vector<Selection> selections_;
    std::string manifestDir_;
};

// Mounted mod roots ("mods/<id>"), lowest priority first, in the order the file VFS mounts
// them (mclib/vfs.cpp InitModSearchPaths): MC2_MOD_DEPS and the active mod.json's
// dependencies below the active mod. modJsonDeps is the active mod's declared dependency list.
std::vector<std::string> modelOverrideMountOrder(std::string_view activeMod, std::string_view envDeps,
                                                 const std::vector<std::string>& modJsonDeps);
