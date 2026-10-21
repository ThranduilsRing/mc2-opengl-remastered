// static_caster_set.cpp -- see the header.
#include "static_caster_set.h"

#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"
#include "gos_static_prop_registry.h"
#include "mc2fmt.h"
#include "shadow_bake_math.h"

#include <algorithm>
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
    building_ = true;
}

void StaticCasterSet::add(const StaticCasterEntry& entry) {
    if (!building_ || entry.recipeIndex < 0) return;
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
    version_ = 0;
    building_ = false;
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
