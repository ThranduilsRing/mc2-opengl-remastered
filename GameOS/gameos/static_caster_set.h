// static_caster_set.h -- the gameplay-keyed set of static shadow casters the shadow bake renders.
//
// WHAT: one entry per gameplay object that casts a static shadow (trees and props, buildings, gates); turrets are tracked separately as rigid movers.
// Each entry holds its game handle, the static-prop registry
// recipe of its shadow LOD (LOD0), its damage state and, once resolved, its transforms. The game side rebuilds the set at mission load from the object
// arrays; the bake resolves it into registry leaves and keys its products on the set's content.
// WHY: the bake must not depend on the render registry's generation (it bumps on LOD swaps, material and light writes) nor on what the camera has seen:
// the set is complete at load and changes only when a caster does (P5 adds the per-caster events).
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
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
    void appendMoverLeaves(std::vector<GpuStaticPropInstance>& leaves, uint64_t frame = 0) const;
    bool isMoverOwner(const void* owner) const;
    size_t moverOwnerCount() const { return movers_.size(); }
    // Per registry recipe index, 1 when an entry owns it (the cascades skip those recipes once the bake has them).
    const std::vector<uint8_t>& recipeMask() const { return recipeMask_; }

    // A caster's shadow recipe goes away (destroyed, falling, shape swap) or a new one is registered for it (rubble, a landed tree, a re-registration).
    // Each change queues the caster's leaves before and after as an event; the bake turns those into dirty sub-tiles. A re-registration that puts back
    // exactly the leaves it removed cancels out. Unknown owners are ignored unless editor admission is enabled; neither call changes version().
    // In the editor objects are placed after the load walk: an unknown owner that registers a shadow recipe joins the set (and queues its leaves).
    void setAdmitNewOwners(bool admit) { admitNewOwners_ = admit; }
    // Explicit editor placement ownership, including recipes that register later.
    // Preview cursors never call this and cannot join the bake.
    void admitOwner(const void* owner, CasterKind kind = CasterKind::TerrainObject);
    void ownerInvalidated(const void* owner, int32_t oldRecipe);
    void ownerRegistered(const void* owner, int32_t newRecipe);

    struct CasterEvent {
        std::vector<GpuStaticPropInstance> before, after;   // the caster's leaves before and after the change
    };
    std::vector<CasterEvent> takeEvents();
    bool hasEvents() const { return !events_.empty() || !removed_.empty(); }
    // Process-unique, changes with every applied event (the bake re-resolves its casters when it moves).
    uint64_t deltaVersion() const { return deltaVersion_; }

    // The registry leaves of every live entry, in entry order. Returns the content hash: the handles, kinds, damage states, type IDs and transforms of
    // the set, independent of registry slot numbering.
    uint64_t resolve(std::vector<GpuStaticPropInstance>& leaves) const;
    // The set as endRebuild() resolved it, before the mission ran a frame. The cold bake renders this (so its key does not depend on what combat did
    // before the bake ran); every event since is queued for the dirty path anyway.
    uint64_t loadSnapshot(std::vector<GpuStaticPropInstance>& leaves) const { leaves = loadLeaves_; return loadHash_; }
    // Only the first mask of a set uses the load snapshot; later full rebakes include consumed edits.
    uint64_t resolveForBake(uint64_t bakedSetVersion, std::vector<GpuStaticPropInstance>& leaves) const {
        return bakedSetVersion != version() ? loadSnapshot(leaves) : resolve(leaves);
    }
    // Logs the entries whose leaves differ from the load snapshot ([SHADOW_BAKE] caster drift): what moved between load and the bake.
    uint32_t logDriftSinceLoad(uint32_t maxLines) const;   // returns the number of drifted entries
    // Re-takes the load snapshot from the live set and drops the events queued since endRebuild(). The first mask of a set calls it: the presentation
    // pass applies load-time damage/destroyed status and shape swaps during the first frames, after the set was built, and a snapshot taken before them
    // leaves the cooked mask stale on arrival (hundreds of drifted entries -> ~1000 dirty sub-tiles drained at 2 a frame). Nothing is baked yet, so
    // those changes are not edits.
    void settleLoadSnapshot();
    // A static owner that keeps changing its leaves (a moving body registered as a static prop) is demoted to a mover after this many real changes.
    static constexpr uint8_t kDemoteAfterChanges = 3;

private:
    struct OwnerIndex { uint32_t index; bool mover; };
    StaticCasterEntry& entryAt(OwnerIndex index) { return index.mover ? movers_[index.index] : entries_[index.index]; }
    std::vector<uint32_t> resolveOrder() const;
    void takeLoadSnapshot();
    std::vector<StaticCasterEntry> entries_;
    std::vector<StaticCasterEntry> movers_;
    std::vector<uint8_t> recipeMask_;
    // Pointer identity is used only for lookup; event emission follows gameplay handle order.
    std::unordered_map<const void*, OwnerIndex> byOwner_;
    std::unordered_map<const void*, uint8_t> churn_;   // real (non-cancelling) re-registrations per static owner since the last settle
    std::unordered_map<const void*, std::vector<GpuStaticPropInstance>> removed_;   // leaves an owner lost and may get back unchanged
    std::vector<CasterEvent> events_;
    std::vector<GpuStaticPropInstance> loadLeaves_;
    std::vector<std::pair<uint32_t, uint32_t>> loadRanges_;   // per entry: first leaf and count in loadLeaves_
    uint64_t loadHash_ = 0;
    uint64_t deltaVersion_ = 0;
    uint64_t version_ = 0;
    uint32_t nextGameHandle_ = 1;
    bool building_ = false;
    bool admitNewOwners_ = false;
};

StaticCasterSet& staticCasterSet();

}  // namespace mc2shadow
