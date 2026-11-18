// Lagged actor visibility with CPU-owned IDs paired with every accepted GPU result.
#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace RenderCore {
enum class VisibilityTier { Tier1_NMinusOne, Tier2_NMinusTwo, Tier3_Conservative };
class VisibilityHistory {
public:
    static constexpr uint32_t kSlots=3, kStaleLimit=10, kNone=UINT32_MAX;
    struct Sample {
        std::vector<uint32_t> ids;
        std::vector<uint8_t> bits;
        uint32_t count=0, visibleCount=0, dilatedAdmits=0, frame=0;
        bool valid=false;
    };
    struct SnapshotStats { uint32_t visible=0, invisible=0, orAdmits=0, overflow=0; };
    explicit VisibilityHistory(uint32_t capacity=0) : visible_(1,1) {
        for (auto& sample:samples_) { sample.ids.resize(capacity); sample.bits.resize(capacity); }
    }
    uint32_t writeSlot() const { return current_; }
    void advance() { current_=(current_+1)%kSlots; }
    uint32_t lastGoodSlot() const { return lastGood_; }
    uint32_t staleFrames() const { return stale_; }
    uint32_t visibleCount() const { return lastGood_==kNone ? kNone : samples_[lastGood_].visibleCount; }
    uint32_t dilatedAdmits() const { return lastGood_==kNone ? 0 : samples_[lastGood_].dilatedAdmits; }
    // fetch copies a COMPLETED result and its immutable actor IDs into the sample;
    // false leaves the last accepted copy intact. It must never block the frame.
    template<class Fetch> VisibilityTier consume(Fetch&& fetch) {
        auto accept=[&](uint32_t slot) {
            auto& sample=samples_[slot];
            if (!fetch(slot,sample)) return false;
            sample.valid=sample.count<=sample.ids.size() && sample.count<=sample.bits.size();
            return sample.valid;
        };
        const uint32_t n1=(current_+kSlots-1)%kSlots, n2=(current_+kSlots-2)%kSlots;
        if (accept(n1)) {
            lastGood_=n1; stale_=0;
            accept(n2); // the earlier submit is complete when N-1 is; cache it for OR.
            return VisibilityTier::Tier1_NMinusOne;
        }
        if (accept(n2)) {
            lastGood_=n2; stale_=0;
            accept((n2+kSlots-1)%kSlots);
            return VisibilityTier::Tier2_NMinusTwo;
        }
        if (stale_<kStaleLimit) ++stale_;
        if (stale_>=kStaleLimit) lastGood_=kNone;
        return VisibilityTier::Tier3_Conservative;
    }
    SnapshotStats buildSnapshot(uint32_t maxActorHandle, bool useOr, bool countStats=false) {
        // Object handles include terrain objects; their range grows with the
        // mission, independently of the number of submitted dynamic actors.
        visible_.resize(maxActorHandle,1);
        std::fill(visible_.begin(),visible_.end(),uint8_t{1}); SnapshotStats stats;
        if (lastGood_==kNone || !samples_[lastGood_].valid) return stats;
        const uint32_t cap=maxActorHandle;
        if (countStats) stats.visible=cap ? cap-1 : 0;
        auto apply=[&](const Sample& sample, bool primary) {
            if (!sample.valid) return;
            for (uint32_t i=0;i<sample.count;++i) {
                const uint32_t id=sample.ids[i];
                if (!id) continue;
                if (id>=cap) { if (!stats.overflow) stats.overflow=id; continue; }
                const uint8_t bit=sample.bits[i]!=0;
                const uint8_t before=visible_[id];
                if (primary) visible_[id]=bit;
                else if (bit && !before) { visible_[id]=1; ++stats.orAdmits; }
                // Account for transitions, including duplicate IDs and OR re-admission.
                if (countStats && before!=visible_[id]) {
                    if (visible_[id]) { ++stats.visible; --stats.invisible; }
                    else { --stats.visible; ++stats.invisible; }
                }
            }
        };
        apply(samples_[lastGood_],true);
        if (useOr) apply(samples_[(lastGood_+kSlots-1)%kSlots],false);
        return stats;
    }
    bool isVisible(uint32_t id) const {
        return lastGood_==kNone || id==0 || id>=visible_.size() || visible_[id]!=0;
    }
private:
    std::array<Sample,kSlots> samples_;
    std::vector<uint8_t> visible_;
    uint32_t current_=0, lastGood_=kNone, stale_=0;
};
}
