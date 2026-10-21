// static_caster_set.h -- the gameplay-keyed set of static shadow casters the shadow bake renders.
//
// WHAT: one entry per gameplay object that casts a static shadow (trees and props, buildings, turrets, gates): its game handle, the static-prop registry
// recipe of its shadow LOD (LOD0), its damage state and, once resolved, its transforms. The game side rebuilds the set at mission load from the object
// arrays; the bake resolves it into registry leaves and keys its products on the set's content.
// WHY: the bake must not depend on the render registry's generation (it bumps on LOD swaps, material and light writes) nor on what the camera has seen:
// the set is complete at load and changes only when a caster does (P5 adds the per-caster events).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct GpuStaticPropInstance;

namespace mc2shadow {

enum class CasterKind : uint8_t { TerrainObject = 0, Building = 1, Turret = 2, Gate = 3 };

struct StaticCasterEntry {
    uint32_t gameHandle = 0;
    int32_t recipeIndex = -1;   // the registry recipe of the caster's shadow LOD
    CasterKind kind = CasterKind::TerrainObject;
    uint8_t damageState = 0;
};

class StaticCasterSet {
public:
    // A rebuild replaces the whole set; version() changes when it ends (never 0 after the first one).
    void beginRebuild();
    void add(const StaticCasterEntry& entry);
    void endRebuild();
    // A map boundary: empty, version 0 (no bake may run until the next mission's rebuild).
    void clear();

    uint64_t version() const { return version_; }
    const std::vector<StaticCasterEntry>& entries() const { return entries_; }
    // Per registry recipe index, 1 when an entry owns it (the cascades skip those recipes once the bake has them).
    const std::vector<uint8_t>& recipeMask() const { return recipeMask_; }

    // The registry leaves of every live entry, in entry order. Returns the content hash: the handles, kinds, damage states, type IDs and transforms of
    // the set, independent of registry slot numbering.
    uint64_t resolve(std::vector<GpuStaticPropInstance>& leaves) const;

private:
    std::vector<StaticCasterEntry> entries_;
    std::vector<uint8_t> recipeMask_;
    uint64_t version_ = 0;
    bool building_ = false;
};

StaticCasterSet& staticCasterSet();

}  // namespace mc2shadow
