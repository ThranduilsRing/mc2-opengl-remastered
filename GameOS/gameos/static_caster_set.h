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
#include <unordered_map>
#include <vector>

#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"

namespace mc2shadow {

enum class CasterKind : uint8_t { TerrainObject = 0, Building = 1, Turret = 2, Gate = 3 };

struct StaticCasterEntry {
    const void* owner = nullptr;   // the caster's appearance: the registration hooks below find the entry by it
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

    // A caster's shadow recipe goes away (destroyed, falling, shape swap) or a new one is registered for it (rubble, a landed tree, a re-registration).
    // Each change queues the caster's leaves before and after as an event; the bake turns those into dirty sub-tiles. A re-registration that puts back
    // exactly the leaves it removed cancels out. Unknown owners (not in the set) are ignored; neither call changes version().
    void ownerInvalidated(const void* owner, int32_t oldRecipe);
    void ownerRegistered(const void* owner, int32_t newRecipe);

    struct CasterEvent {
        std::vector<GpuStaticPropInstance> before, after;   // the caster's leaves before and after the change
    };
    std::vector<CasterEvent> takeEvents();
    bool hasEvents() const { return !events_.empty(); }
    // Process-unique, changes with every applied event (the bake re-resolves its casters when it moves).
    uint64_t deltaVersion() const { return deltaVersion_; }

    // The registry leaves of every live entry, in entry order. Returns the content hash: the handles, kinds, damage states, type IDs and transforms of
    // the set, independent of registry slot numbering.
    uint64_t resolve(std::vector<GpuStaticPropInstance>& leaves) const;

private:
    std::vector<StaticCasterEntry> entries_;
    std::vector<uint8_t> recipeMask_;
    std::unordered_map<const void*, uint32_t> byOwner_;
    std::unordered_map<const void*, std::vector<GpuStaticPropInstance>> removed_;   // leaves an owner lost and may get back unchanged
    std::vector<CasterEvent> events_;
    uint64_t deltaVersion_ = 0;
    uint64_t version_ = 0;
    bool building_ = false;
};

StaticCasterSet& staticCasterSet();

}  // namespace mc2shadow
