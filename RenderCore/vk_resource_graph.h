#pragma once
// Fixed-order, single-queue whole-image graph. Compile at resource creation;
// execute barriers immediately before each pass, outside render passes.
// A transient's first write must initialize the whole image (clear/copy).
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace mc2vk::graph {
struct State {
    uint32_t layout = 0, stages = 0, access = 0;
    bool write = false;
};
struct Image {
    // Exact image creation identity: width, height, format, samples, usage, flags.
    std::array<uint32_t, 6> key{};
    bool imported = false;
    State initial{};
};
struct Use { unsigned image; State state; };
struct Pass { std::vector<Use> uses; };
struct Lifetime { int first = -1, last = -1, slot = -1; };
struct Barrier { unsigned image; State before, after; };
struct Plan {
    bool valid = false;
    unsigned slots = 0;
    std::vector<Lifetime> lifetimes;
    std::vector<std::vector<Barrier>> barriers;
};

inline Plan compile(const std::vector<Image>& images, const std::vector<Pass>& passes) {
    Plan plan;
    plan.lifetimes.resize(images.size());
    plan.barriers.resize(passes.size());
    for (unsigned p = 0; p < passes.size(); ++p) {
        std::vector<bool> seen(images.size());
        for (const auto& use : passes[p].uses) {
            if (use.image >= images.size() || seen[use.image] || !use.state.stages) return plan;
            seen[use.image] = true;
            auto& life = plan.lifetimes[use.image];
            if (life.first < 0) life.first = int(p);
            life.last = int(p);
        }
    }
    // Allocate in first-use order, independent of resource declaration order.
    std::vector<unsigned> order;
    for (unsigned i = 0; i < images.size(); ++i)
        if (!images[i].imported && plan.lifetimes[i].first >= 0) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
        return plan.lifetimes[a].first < plan.lifetimes[b].first;
    });
    std::vector<unsigned> owners;
    for (unsigned i : order) {
        auto& life = plan.lifetimes[i];
        for (unsigned s = 0; s < owners.size(); ++s) {
            unsigned previous = owners[s];
            if (images[previous].key == images[i].key && plan.lifetimes[previous].last < life.first) {
                life.slot = int(s);
                owners[s] = i;
                break;
            }
        }
        if (life.slot < 0) { life.slot = int(owners.size()); owners.push_back(i); }
    }
    plan.slots = unsigned(owners.size());
    std::vector<State> states(images.size());
    std::vector<bool> initialized(images.size());
    std::vector<int> slotOwner(plan.slots, -1);
    for (unsigned i = 0; i < images.size(); ++i) {
        states[i] = images[i].imported ? images[i].initial : State{};
        initialized[i] = images[i].imported && images[i].initial.layout != 0;
    }
    for (unsigned p = 0; p < passes.size(); ++p) {
        for (auto use : passes[p].uses) {
            auto& before = states[use.image];
            bool fresh = !initialized[use.image];
            if (fresh) {
                if (!use.state.write) return plan; // No reading undefined transient contents.
                const int slot = plan.lifetimes[use.image].slot;
                if (slot >= 0 && slotOwner[slot] >= 0) {
                    before = states[slotOwner[slot]];
                    before.layout = 0; // Discard, but retire every access to the old slot occupant.
                }
                if (slot >= 0) slotOwner[slot] = int(use.image);
                initialized[use.image] = true;
            }
            if (fresh || before.layout != use.state.layout || before.write || use.state.write) {
                plan.barriers[p].push_back({use.image, before, use.state});
                before = use.state;
            } else {
                // Read/read needs no barrier, but a later writer waits for ALL readers.
                before.stages |= use.state.stages;
                before.access |= use.state.access;
            }
        }
    }
    plan.valid = true;
    return plan;
}
} // namespace mc2vk::graph
