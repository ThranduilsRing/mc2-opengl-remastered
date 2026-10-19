// model_override_registry.h — MODEL-OVERRIDE-MVP-1 Slice 1.
// Engine-independent parse of model_overrides/models.json. No TG_*/GameOS deps.
// Render-only model overrides keyed by "<class>:<appearanceName>". MVP rules:
// renderOnly must be true, fallback must be "stock". Invalid entries are logged
// and dropped, never fatal. See docs/model-override-mvp-plan.md.
#pragma once
#include <string>
#include <string_view>
#include <vector>

// TREE-OVERRIDE-LOD-MVP-1 Task 3/4: one lower-detail LOD source for an override.
// lod is the LOD index (>=1; LOD0 is the record's own `source`). distance is the
// camera-distance band at which this LOD becomes active (Task 5 consumes it; Task
// 3 only needs the source registered+baked). Minimal parse here (full validation
// + unit tests land in Task 4) — kept safe: isSafeSource + ascending lod.
struct ModelOverrideLod {
    int         lod = 0;          // LOD index (1..MAX_LODS-1 for entries; LOD0 = record.source)
    std::string sourceRelPath;   // .glb/.gltf relative to manifest dir (validated safe)
    float       distance = 0.0f; // activation distance (Task 5); 0 = unset
};

struct ModelOverrideRecord {
    std::string overrideClass;   // "staticprop" | "tree" — NORMALIZED lowercase
    std::string appearanceName;  // from "replaces" after ':' — NORMALIZED lowercase
    std::string sourceRelPath;   // .glb/.gltf relative to manifestDir (validated safe)
    std::string manifestDir;     // directory from which this record's source paths resolve
    float       scale = 1.0f;    // MVP requires exactly 1.0
    // TREE-OVERRIDE-LOD-MVP-1: optional lower-detail LOD chain (LOD0 == source).
    // Ascending `lod`, each source isSafeSource. Empty = single-LOD (LOD0 only).
    std::vector<ModelOverrideLod> lods;
    // MVP invariants (validated at load; entry dropped + logged if violated):
    //   type=="model", renderOnly==true, fallback=="stock", scale==1.0,
    //   safe relative .glb/.gltf source, class in {staticProp,tree},
    //   class field (if present) agrees with replaces. Duplicate key: first wins.
};

class ModelOverrideRegistry {
public:
    // Clears registry then loads from manifestPath. Returns count.
    int loadFromFile(const std::string& manifestPath,
                     std::string_view manifestDir);
    // Additive: loads manifestPath without clearing. Mod entries WIN on dup key.
    // Returns count of entries merged in (not total).
    int mergeFromFile(const std::string& manifestPath,
                      std::string_view manifestDir);
    const ModelOverrideRecord* resolve(const char* overrideClass,
                                       const char* appearanceName) const;
    int count() const { return static_cast<int>(records_.size()); }
    // Manifest dir the records were loaded from; override source paths are
    // resolved relative to it (avoids hardcoding the literal at call sites).
    const std::string& manifestDir() const { return manifestDir_; }
    static ModelOverrideRegistry& instance();
private:
    std::vector<ModelOverrideRecord> records_;
    std::string manifestDir_;   // retained for Slice 2 relative source-path resolution
};
