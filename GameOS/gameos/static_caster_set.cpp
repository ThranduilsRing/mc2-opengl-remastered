// static_caster_set.cpp -- see the header.
#include "static_caster_set.h"
#include <array>

#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"
#include "gos_static_prop_registry.h"
#include "mc2fmt.h"
#include "shadow_bake_math.h"
#include "shadow_mover_feed.h"

#include <algorithm>
#include <optional>
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
    clear();
    building_ = true;
}

void StaticCasterSet::add(const StaticCasterEntry& entry) {
    if (!building_ || (entry.recipeIndex < 0 && entry.kind != CasterKind::Turret)) return;
    nextGameHandle_ = (std::max)(nextGameHandle_, entry.gameHandle + 1);
    if (entry.kind == CasterKind::Turret) {
        if (entry.owner) byOwner_[entry.owner] = {static_cast<uint32_t>(movers_.size()),true};
        movers_.push_back(entry);
        return;
    }
    if (entry.owner) byOwner_[entry.owner] = {static_cast<uint32_t>(entries_.size()),false};
    entries_.push_back(entry);
    const size_t r = static_cast<size_t>(entry.recipeIndex);
    if (recipeMask_.size() <= r) recipeMask_.resize(r + 1, 0);
    recipeMask_[r] = 1;
}

void StaticCasterSet::endRebuild() {
    building_ = false;
    settled_ = false;
    version_ = g_nextVersion++;
    takeLoadSnapshot();
    std::array<uint32_t, 4> byKind = {0, 0, 0, 0};
    for (const StaticCasterEntry& e : entries_) ++byKind[static_cast<uint8_t>(e.kind) & 3u];
    mc2::fmt::print_err("[SHADOW_BAKE] caster_set version={} entries={} terrain_objects={} buildings={} turrets={} gates={} load_leaves={} load_hash={:016x}\n",
                        mc2::fmt::as_u64(version_), mc2::fmt::as_size(entries_.size()), byKind[0], byKind[1], byKind[2], byKind[3],
                        mc2::fmt::as_size(loadLeaves_.size()), mc2::fmt::as_u64(loadHash_));
}

void StaticCasterSet::takeLoadSnapshot() {
    loadHash_ = resolve(loadLeaves_);
    // per-entry ranges of the snapshot, for the drift log
    loadRanges_.assign(entries_.size(), {0u, 0u});
    std::vector<GpuStaticPropInstance> one;
    uint32_t at = 0;
    for (uint32_t i : resolveOrder()) {
        one.clear();
        const uint32_t n = static_cast<uint32_t>(GpuStaticPropRegistry::getRecipeShadowLeaves(entries_[i].recipeIndex, one));
        loadRanges_[i] = {at, n};
        at += n;
    }
}

void StaticCasterSet::settleLoadSnapshot() {
    // Once per set: the bake re-enters recordBake after its async cache read; a second settle would drop events queued since the first while the
    // mask (keyed by the first snapshot) still shows the object intact.
    if (building_ || version_ == 0 || settled_) return;
    settled_ = true;
    takeLoadSnapshot();
    events_.clear();
    removed_.clear();
    churn_.clear();
}

void StaticCasterSet::clear() {
    moverPropFeed().clear();
    entries_.clear();
    movers_.clear();
    recipeMask_.clear();
    byOwner_.clear();
    removed_.clear();
    churn_.clear();
    events_.clear();
    loadLeaves_.clear();
    loadRanges_.clear();
    loadHash_ = 0;
    version_ = 0;
    settled_ = false;
    nextGameHandle_ = 1;
    building_ = false;
    admitNewOwners_ = false;
}

namespace {
template <class Map>
std::optional<typename Map::mapped_type> findOwner(const Map& m, const void* owner) {
    const auto it = m.find(owner);
    if (it == m.end()) return {};
    const auto& [key, value] = *it;
    return value;
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
bool sameLeafTypes(const std::vector<GpuStaticPropInstance>& a, const std::vector<GpuStaticPropInstance>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i].typeID != b[i].typeID) return false;
    return true;
}
}  // namespace

void StaticCasterSet::ownerInvalidated(const void* owner, int32_t oldRecipe) {
    if (building_ || version_ == 0 || !owner) return;
    auto idx = findOwner(byOwner_, owner);
    if (!idx) return;
    StaticCasterEntry& e = entryAt(*idx);
    if (e.recipeIndex < 0 || e.recipeIndex != oldRecipe) return;
    if (idx->mover) { e.recipeIndex=-1; return; }
    std::vector<GpuStaticPropInstance> leaves;
    GpuStaticPropRegistry::getRecipeShadowLeaves(oldRecipe, leaves);
    if (static_cast<size_t>(oldRecipe) < recipeMask_.size()) recipeMask_[static_cast<size_t>(oldRecipe)] = 0;
    e.recipeIndex = -1;
    e.damageState = 1;
    removed_[owner] = std::move(leaves);
}

void StaticCasterSet::admitOwner(const void* owner, CasterKind kind) {
    if (building_ || version_ == 0 || !owner || findOwner(byOwner_, owner)) return;
    StaticCasterEntry fresh;
    fresh.owner = owner;
    fresh.gameHandle = nextGameHandle_++;
    fresh.kind = kind;
    const bool mover=kind==CasterKind::Turret;
    auto& owners=mover?movers_:entries_;
    byOwner_[owner] = {static_cast<uint32_t>(owners.size()),mover};
    owners.push_back(fresh);
}

void StaticCasterSet::ownerRegistered(const void* owner, int32_t newRecipe) {
    if (building_ || version_ == 0 || !owner || newRecipe < 0) return;
    auto idx = findOwner(byOwner_, owner);
    if (!idx) {
        if (!admitNewOwners_) return;
        admitOwner(owner);
        idx = findOwner(byOwner_, owner);
    }
    StaticCasterEntry& e = entryAt(*idx);
    if (e.recipeIndex == newRecipe) return;
    if (idx->mover) { e.recipeIndex=newRecipe; return; }
    CasterEvent ev;
    GpuStaticPropRegistry::getRecipeShadowLeaves(newRecipe, ev.after);
    if (auto node = removed_.extract(owner); !node.empty()) ev.before = std::move(node.mapped());
    e.recipeIndex = newRecipe;
    const size_t r = static_cast<size_t>(newRecipe);
    if (recipeMask_.size() <= r) recipeMask_.resize(r + 1, 0);
    recipeMask_[r] = 1;
    if (sameLeaves(ev.before, ev.after)) return;   // a re-registration of the same leaves: nothing to rebake
    // An appearance swap (different typeIDs/leaf count: damage, destroyed, rubble) is not motion; only moves of the same shape count toward demotion.
    if (!ev.before.empty() && !sameLeafTypes(ev.before, ev.after)) churn_.erase(owner);
    else if (!ev.before.empty() && ++churn_[owner] >= kDemoteAfterChanges) {
        // A moving owner must not be a static caster: it would emit a removed/added pair, and dirty its footprint, every frame. The entry stays in
        // place as a tombstone (load ranges keep their indices); the owner continues as a mover and its old static leaves are removed once.
        StaticCasterEntry mover = e;
        e.recipeIndex = -1;
        if (static_cast<size_t>(newRecipe) < recipeMask_.size()) recipeMask_[static_cast<size_t>(newRecipe)] = 0;
        byOwner_[owner] = {static_cast<uint32_t>(movers_.size()), true};
        movers_.push_back(mover);
        ev.after.clear();
        ++deltaVersion_;
        events_.push_back(std::move(ev));
        return;
    }
    ++deltaVersion_;
    events_.push_back(std::move(ev));
}

std::vector<StaticCasterSet::CasterEvent> StaticCasterSet::takeEvents() {
    // Owners that lost their recipe and never got one back (a felled tree still falling, a building with no rubble shape) are removals.
    if (!removed_.empty()) {
        for (uint32_t i : resolveOrder()) {
            const auto& entry = entries_[i];
            auto node = removed_.extract(entry.owner);
            if (node.empty()) continue;
            CasterEvent ev;
            ev.before = std::move(node.mapped());
            events_.push_back(std::move(ev));
            ++deltaVersion_;
        }
    }
    removed_.clear();
    std::vector<CasterEvent> out;
    out.swap(events_);
    return out;
}

// Entry indices in handle order, so the key does not depend on the order the game arrays were walked in.
std::vector<uint32_t> StaticCasterSet::resolveOrder() const {
    std::vector<uint32_t> order(entries_.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b) {
        const StaticCasterEntry& x = entries_[a];
        const StaticCasterEntry& y = entries_[b];
        return x.gameHandle != y.gameHandle ? x.gameHandle < y.gameHandle : x.recipeIndex < y.recipeIndex;
    });
    return order;
}

uint32_t StaticCasterSet::logDriftSinceLoad(uint32_t maxLines) const {
    std::vector<GpuStaticPropInstance> one;
    uint32_t drifted = 0, logged = 0;
    for (uint32_t i = 0; i < entries_.size(); ++i) {
        const StaticCasterEntry& e = entries_[i];
        one.clear();
        GpuStaticPropRegistry::getRecipeShadowLeaves(e.recipeIndex, one);
        const auto [firstLeaf, leafCount] = i < loadRanges_.size() ? loadRanges_[i] : std::pair<uint32_t, uint32_t>{0u, 0u};
        float maxDelta = 0.0f;
        bool differs = one.size() != leafCount;
        for (uint32_t k = 0; !differs && k < leafCount; ++k) {
            const GpuStaticPropInstance& a = loadLeaves_[firstLeaf + k];
            if (a.typeID != one[k].typeID) { differs = true; break; }
            for (int m = 0; m < 16; ++m) maxDelta = (std::max)(maxDelta, std::fabs(a.modelMatrix[m] - one[k].modelMatrix[m]));
        }
        if (maxDelta > 0.0f) differs = true;
        if (!differs) continue;
        ++drifted;
        if (logged++ < maxLines)
            mc2::fmt::print_err("[SHADOW_BAKE] caster drift: handle={} kind={} recipe={} damage={} leaves load={} now={} max_matrix_delta={:.4f}\n",
                                e.gameHandle, static_cast<uint32_t>(e.kind), e.recipeIndex, static_cast<uint32_t>(e.damageState), leafCount,
                                mc2::fmt::as_size(one.size()), maxDelta);
    }
    mc2::fmt::print_err("[SHADOW_BAKE] caster drift since load: {} of {} entries\n", drifted, mc2::fmt::as_size(entries_.size()));
    return drifted;
}

bool StaticCasterSet::isMoverOwner(const void* owner) const {
    const auto index=findOwner(byOwner_,owner);
    return index && index->mover;
}

void StaticCasterSet::appendMoverLeaves(std::vector<GpuStaticPropInstance>& leaves, uint64_t frame) const {
    std::vector<uint32_t> order(movers_.size());
    for (uint32_t i=0;i<order.size();++i) order[i]=i;
    std::sort(order.begin(),order.end(),[this](uint32_t a,uint32_t b) { return movers_[a].gameHandle<movers_[b].gameHandle; });
    for (uint32_t i:order)
        if (!moverPropFeed().contains(movers_[i].owner,frame)) GpuStaticPropRegistry::getRecipeShadowLeaves(movers_[i].recipeIndex,leaves);
}

uint64_t StaticCasterSet::resolve(std::vector<GpuStaticPropInstance>& leaves) const {
    leaves.clear();
    uint64_t h = fnv1a(nullptr, 0);
    std::vector<GpuStaticPropInstance> one;
    for (uint32_t i : resolveOrder()) {
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
