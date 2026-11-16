#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace mc2shadow {
// A permutation is reusable only for the complete, ordered input type sequence.
class InstanceOrder {
    std::vector<uint32_t> order_;
    std::vector<uint32_t> types_;
public:
    void clear() {
        order_.clear();
        types_.clear();
    }
    template<class Instance>
    const std::vector<uint32_t>& update(std::span<const Instance> instances) {
        const auto count = static_cast<uint32_t>(instances.size());
        bool same = types_.size() == count;
        for (uint32_t i = 0; same && i < count; ++i)
            same = types_[i] == instances[i].typeID;
        if (same) return order_;
        clear();
        types_.reserve(count);
        order_.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            types_.push_back(instances[i].typeID);
            order_[i] = i;
        }
        // Reset to identity before the original sort, including equal-type ties.
        std::sort(order_.begin(), order_.end(), [instances](uint32_t a, uint32_t b) {
            return instances[a].typeID < instances[b].typeID;
        });
        return order_;
    }
};
}
