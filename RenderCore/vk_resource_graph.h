#pragma once
// Fixed-order, single-queue resource graph. Compile at resource creation;
// execute barriers immediately before each pass, outside render passes.
// A transient's first write must initialize the whole image (clear/copy).
// Masks are 64-bit (synchronization2 stage/access values). Buffers are images with
// `buffer = true` (layout always 0). Images may carry several mip levels; the state is tracked per mip.
// State can be carried across frames: feed Plan::finalStates of one compile as `carry` of the next.
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace mc2vk::graph {
struct State {
    uint32_t layout = 0;
    uint64_t stages = 0, access = 0;
    bool write = false;
};
struct Image {
    // Exact image creation identity: width, height, format, samples, usage, flags.
    std::array<uint32_t, 6> key{};
    bool imported = false;
    State initial{};
    uint32_t mips = 1;     // mip levels tracked separately (1 for a buffer)
    bool buffer = false;   // a buffer: no layout
};
// mipCount 0 = every mip from baseMip to the end.
struct Use { unsigned image; State state; uint32_t baseMip = 0, mipCount = 0; };
struct Pass { std::vector<Use> uses; };
struct Lifetime { int firstPass = -1, lastPass = -1, slot = -1; };
struct Barrier { unsigned image; State before, after; uint32_t baseMip = 0, mipCount = 1; };
struct Plan {
    bool valid = false;
    unsigned slots = 0;
    std::vector<Lifetime> lifetimes;
    std::vector<std::vector<Barrier>> barriers;
    std::vector<unsigned> mipOffset;   // first flattened state index of every image
    std::vector<State> finalStates;    // per image per mip after the last pass: the `carry` of the next compile
};

inline bool sameState(const State& a, const State& b) {
    return a.layout == b.layout && a.stages == b.stages && a.access == b.access && a.write == b.write;
}

inline Plan compile(const std::vector<Image>& images, const std::vector<Pass>& passes,
                    const std::vector<State>* carry = nullptr) {
    Plan plan;
    plan.lifetimes.resize(images.size());
    plan.barriers.resize(passes.size());
    plan.mipOffset.resize(images.size() + 1);
    unsigned total = 0;
    for (unsigned i = 0; i < images.size(); ++i) {
        plan.mipOffset[i] = total;
        total += std::max(images[i].mips, 1u);
    }
    plan.mipOffset[images.size()] = total;
    if (carry && carry->size() != total) return plan;
    auto range = [&](const Use& use, uint32_t& m0, uint32_t& n) {
        const uint32_t mips = std::max(images[use.image].mips, 1u);
        m0 = use.baseMip;
        n = use.mipCount ? use.mipCount : (m0 < mips ? mips - m0 : 0);
        return n > 0 && m0 + n <= mips;
    };
    for (unsigned p = 0; p < passes.size(); ++p) {
        std::vector<bool> seen(images.size());
        for (const auto& use : passes[p].uses) {
            uint32_t m0, n;
            if (use.image >= images.size() || seen[use.image] || !use.state.stages || !range(use, m0, n)) return plan;
            seen[use.image] = true;
            auto& life = plan.lifetimes[use.image];
            if (life.firstPass < 0) life.firstPass = static_cast<int>(p);
            life.lastPass = static_cast<int>(p);
        }
    }
    // Allocate in first-use order, independent of resource declaration order.
    std::vector<unsigned> order;
    for (unsigned i = 0; i < images.size(); ++i)
        if (!images[i].imported && plan.lifetimes[i].firstPass >= 0) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
        return plan.lifetimes[a].firstPass < plan.lifetimes[b].firstPass;
    });
    std::vector<unsigned> owners;
    for (unsigned i : order) {
        auto& life = plan.lifetimes[i];
        for (unsigned s = 0; s < owners.size(); ++s) {
            unsigned previous = owners[s];
            if (images[previous].key == images[i].key && images[previous].mips == images[i].mips &&
                images[previous].buffer == images[i].buffer && plan.lifetimes[previous].lastPass < life.firstPass) {
                life.slot = static_cast<int>(s);
                owners[s] = i;
                break;
            }
        }
        if (life.slot < 0) { life.slot = static_cast<int>(owners.size()); owners.push_back(i); }
    }
    plan.slots = static_cast<unsigned>(owners.size());
    std::vector<State> states(total);
    std::vector<State> visible(total);   // stages/access the last producer barrier made the contents visible to
    std::vector<bool> initialized(images.size());
    std::vector<int> slotOwner(plan.slots, -1);
    for (unsigned i = 0; i < images.size(); ++i) {
        if (!images[i].imported) continue;
        const unsigned mips = plan.mipOffset[i + 1] - plan.mipOffset[i];
        for (unsigned m = 0; m < mips; ++m) {
            const State s = carry ? (*carry)[plan.mipOffset[i] + m] : images[i].initial;
            states[plan.mipOffset[i] + m] = s;
            if (!s.write) visible[plan.mipOffset[i] + m] = s;   // an imported read state is already visible to what it names
        }
        const State& s0 = states[plan.mipOffset[i]];
        initialized[i] = s0.layout != 0 || s0.stages != 0;
    }
    for (unsigned p = 0; p < passes.size(); ++p) {
        for (const auto& use : passes[p].uses) {
            uint32_t m0, n;
            range(use, m0, n);
            const unsigned base = plan.mipOffset[use.image];
            const bool fresh = !initialized[use.image];
            if (fresh) {
                if (!use.state.write) return plan;                                       // No reading undefined transient contents.
                if (m0 != 0 || n != plan.mipOffset[use.image + 1] - base) return plan;   // The first write covers every mip.
                const int slot = plan.lifetimes[use.image].slot;
                State discarded{};
                if (slot >= 0 && slotOwner[slot] >= 0) {
                    // Every mip of the old occupant counts: its accesses are all retired, not only mip 0's.
                    const unsigned ob = plan.mipOffset[slotOwner[slot]], oe = plan.mipOffset[slotOwner[slot] + 1];
                    for (unsigned om = ob; om < oe; ++om) {
                        discarded.stages |= states[om].stages;
                        discarded.access |= states[om].access;
                        discarded.write = discarded.write || states[om].write;
                    }
                    discarded.layout = 0; // Discard, but retire every access to the old slot occupant.
                }
                for (uint32_t m = 0; m < n; ++m) states[base + m] = discarded;
                if (slot >= 0) slotOwner[slot] = static_cast<int>(use.image);
                initialized[use.image] = true;
            }
            // Per mip, then merge neighbouring mips that need the same transition into one barrier.
            bool open = false;
            Barrier cur{};
            for (uint32_t m = m0; m < m0 + n; ++m) {
                auto& before = states[base + m];
                auto& vis = visible[base + m];
                bool need = false;
                const State src = before;
                if (fresh || before.layout != use.state.layout || before.write || use.state.write) {
                    need = true;
                    before = use.state;
                    vis = use.state;
                } else {
                    // Read/read: a reader with a stage/access the producer barrier did not cover needs its own barrier. A later writer waits for ALL readers.
                    if ((use.state.stages & ~vis.stages) || (use.state.access & ~vis.access)) {
                        need = true;
                        vis.stages |= use.state.stages;
                        vis.access |= use.state.access;
                    }
                    before.stages |= use.state.stages;
                    before.access |= use.state.access;
                }
                if (!need) { open = false; continue; }
                if (open && sameState(cur.before, src) && cur.baseMip + cur.mipCount == m) {
                    ++cur.mipCount;
                    plan.barriers[p].back().mipCount = cur.mipCount;
                } else {
                    cur = {use.image, src, use.state, m, 1};
                    plan.barriers[p].push_back(cur);
                    open = true;
                }
            }
        }
    }
    // Cross-frame retire. A plan is compiled once and replayed every frame, so the first write of a transient re-enters a slot whose previous
    // frame's last accesses (the reads of the final passes) may still be in flight. The write discards the contents (layout 0 stays) but its
    // edge must source every end-of-plan stage/access of the images sharing the slot; a zero source would be emitted as TOP_OF_PIPE / NONE,
    // which waits for nothing and is a write-after-read hazard on the second frame.
    std::vector<State> slotEnd(plan.slots);
    for (unsigned i = 0; i < images.size(); ++i) {
        const int slot = plan.lifetimes[i].slot;
        if (images[i].imported || slot < 0) continue;
        for (unsigned m = plan.mipOffset[i]; m < plan.mipOffset[i + 1]; ++m) {
            slotEnd[slot].stages |= states[m].stages;
            slotEnd[slot].access |= states[m].access;
            slotEnd[slot].write = slotEnd[slot].write || states[m].write;
        }
    }
    for (unsigned p = 0; p < passes.size(); ++p) {
        for (auto& b : plan.barriers[p]) {
            const auto& life = plan.lifetimes[b.image];
            if (images[b.image].imported || life.slot < 0 || life.firstPass != static_cast<int>(p)) continue;   // only the fresh (first-write) edge
            b.before.stages |= slotEnd[life.slot].stages;
            b.before.access |= slotEnd[life.slot].access;
            b.before.write = b.before.write || slotEnd[life.slot].write;
        }
    }
    plan.finalStates = std::move(states);
    plan.valid = true;
    return plan;
}
} // namespace mc2vk::graph
