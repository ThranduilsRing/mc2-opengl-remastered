// static_caster_set.cpp -- see the header.
#include "static_caster_set.h"

#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"
#include "gos_static_prop_registry.h"
#include "mc2fmt.h"
#include "shadow_bake_math.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mc2shadow {

namespace {
uint64_t g_nextVersion = 1;   // unique for the process, so a rebuilt set never matches a key taken from an older one
}

StaticCasterSet& staticCasterSet() {
    static StaticCasterSet s_set;
    return s_set;
}

void StaticCasterSet::beginRebuild() {
    entries_.clear();
    recipeMask_.clear();
    byOwner_.clear();
    removed_.clear();
    events_.clear();
    building_ = true;
}

void StaticCasterSet::add(const StaticCasterEntry& entry) {
    if (!building_ || entry.recipeIndex < 0) return;
    if (entry.owner) byOwner_[entry.owner] = static_cast<uint32_t>(entries_.size());
    entries_.push_back(entry);
    const size_t r = static_cast<size_t>(entry.recipeIndex);
    if (recipeMask_.size() <= r) recipeMask_.resize(r + 1, 0);
    recipeMask_[r] = 1;
}

void StaticCasterSet::endRebuild() {
    building_ = false;
    version_ = g_nextVersion++;
    uint32_t byKind[4] = {0, 0, 0, 0};
    for (const StaticCasterEntry& e : entries_) ++byKind[static_cast<uint8_t>(e.kind) & 3u];
    mc2::fmt::print_err("[SHADOW_BAKE] caster_set version={} entries={} terrain_objects={} buildings={} turrets={} gates={}\n",
                        mc2::fmt::as_u64(version_), mc2::fmt::as_size(entries_.size()), byKind[0], byKind[1], byKind[2], byKind[3]);
}

void StaticCasterSet::clear() {
    entries_.clear();
    recipeMask_.clear();
    byOwner_.clear();
    removed_.clear();
    events_.clear();
    version_ = 0;
    building_ = false;
}

namespace {
template <class Map>
const typename Map::mapped_type* findOwner(const Map& m, const void* owner) {
    const auto it = m.find(owner);
    return it == m.end() ? nullptr : &it->second;
}
// Within a texel of the mask: a re-registration that moves a caster by less than this changes no shadow worth a rebake (the matrix is rebuilt from
// the object's float state each time and wobbles in the last bits).
bool sameLeaves(const std::vector<GpuStaticPropInstance>& a, const std::vector<GpuStaticPropInstance>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].typeID != b[i].typeID) return false;
        for (int k = 0; k < 16; ++k) {
            const bool translation = (k == 3 || k == 7 || k == 11);
            if (std::fabs(a[i].modelMatrix[k] - b[i].modelMatrix[k]) > (translation ? 0.75f : 0.01f)) return false;
        }
    }
    return true;
}
}  // namespace

void StaticCasterSet::ownerInvalidated(const void* owner, int32_t oldRecipe) {
    if (building_ || version_ == 0 || !owner) return;
    const uint32_t* idx = findOwner(byOwner_, owner);
    if (!idx) return;
    StaticCasterEntry& e = entries_[*idx];
    if (e.recipeIndex < 0 || e.recipeIndex != oldRecipe) return;
    std::vector<GpuStaticPropInstance> leaves;
    GpuStaticPropRegistry::getRecipeShadowLeaves(oldRecipe, leaves);
    if (static_cast<size_t>(oldRecipe) < recipeMask_.size()) recipeMask_[static_cast<size_t>(oldRecipe)] = 0;
    e.recipeIndex = -1;
    e.damageState = 1;
    removed_[owner] = std::move(leaves);
}

void StaticCasterSet::ownerRegistered(const void* owner, int32_t newRecipe) {
    if (building_ || version_ == 0 || !owner || newRecipe < 0) return;
    const uint32_t* idx = findOwner(byOwner_, owner);
    if (!idx) return;
    StaticCasterEntry& e = entries_[*idx];
    if (e.recipeIndex == newRecipe) return;
    CasterEvent ev;
    GpuStaticPropRegistry::getRecipeShadowLeaves(newRecipe, ev.after);
    if (auto node = removed_.extract(owner); !node.empty()) ev.before = std::move(node.mapped());
    e.recipeIndex = newRecipe;
    const size_t r = static_cast<size_t>(newRecipe);
    if (recipeMask_.size() <= r) recipeMask_.resize(r + 1, 0);
    recipeMask_[r] = 1;
    ++deltaVersion_;
    if (sameLeaves(ev.before, ev.after)) return;   // a re-registration of the same leaves: nothing to rebake
    // Turrets turn: their shadow is a mover's (P6). Until then the bake keeps their pristine pose and their re-registrations queue nothing.
    if (e.kind == CasterKind::Turret) return;
    events_.push_back(std::move(ev));
}

std::vector<StaticCasterSet::CasterEvent> StaticCasterSet::takeEvents() {
    // Owners that lost their recipe and never got one back (a felled tree still falling, a building with no rubble shape) are removals.
    for (auto& [owner, leaves] : removed_) {
        if (const uint32_t* idx = findOwner(byOwner_, owner); idx && entries_[*idx].kind == CasterKind::Turret) continue;
        CasterEvent ev;
        ev.before = std::move(leaves);
        events_.push_back(std::move(ev));
        ++deltaVersion_;
    }
    removed_.clear();
    std::vector<CasterEvent> out;
    out.swap(events_);
    return out;
}

uint64_t StaticCasterSet::resolve(std::vector<GpuStaticPropInstance>& leaves) const {
    leaves.clear();
    // Hash in handle order, so the key does not depend on the order the game arrays were walked in.
    std::vector<uint32_t> order(entries_.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b) {
        const StaticCasterEntry& x = entries_[a];
        const StaticCasterEntry& y = entries_[b];
        return x.gameHandle != y.gameHandle ? x.gameHandle < y.gameHandle : x.recipeIndex < y.recipeIndex;
    });
    uint64_t h = fnv1a(nullptr, 0);
    std::vector<GpuStaticPropInstance> one;
    for (uint32_t i : order) {
        const StaticCasterEntry& e = entries_[i];
        one.clear();
        if (GpuStaticPropRegistry::getRecipeShadowLeaves(e.recipeIndex, one) == 0) continue;
        h = fnv1a(&e.gameHandle, sizeof(e.gameHandle), h);
        h = fnv1a(&e.kind, sizeof(e.kind), h);
        h = fnv1a(&e.damageState, sizeof(e.damageState), h);
        for (const GpuStaticPropInstance& leaf : one) {
            h = fnv1a(&leaf.typeID, sizeof(leaf.typeID), h);
            h = fnv1a(leaf.modelMatrix, sizeof(leaf.modelMatrix), h);
        }
        leaves.insert(leaves.end(), one.begin(), one.end());
    }
    return h;
}

}  // namespace mc2shadow
