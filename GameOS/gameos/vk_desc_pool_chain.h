#pragma once
// vk_desc_pool_chain.h -- an island's chained descriptor pools (VK descriptor plan lane 2, Astra review p13).
// WHAT: DescPoolChainCore keeps one island's pools in a chain. allocate() tries the current pool; on OUT_OF_POOL_MEMORY or FRAGMENTED_POOL
// it moves to the next pool of the chain (one kept from an earlier mission, already reset) or creates a fresh pool of the same size, and
// retries ONCE. Any other result, or a failed retry, goes back to the caller with the actual VkResult: the island keeps its own failure path.
// reset() resets every pool and rewinds to the first (mission unload, after the GPU is idle). Capacity is kept, so the chain length is the
// largest working set one mission needed, never a sum over missions.
// WHY: the legacy-shape (192), overlay (480) and vfx-sprite (64) texture caches had hard caps that mods overflow.
// Pure logic, no Vulkan (the backend is two callables): tests/unit/test_vk_desc_pool_chain.cpp drives it past those caps and injects both errors.
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace mc2vk {

// VkResult values as plain integers, so this header needs no Vulkan include.
inline constexpr int32_t kDescPoolSuccess = 0, kVkErrorFragmentedPool = -12, kVkErrorOutOfPoolMemory = -1000069000;
constexpr bool descPoolExhausted(int32_t result) { return result == kVkErrorOutOfPoolMemory || result == kVkErrorFragmentedPool; }

template <class Pool>
class DescPoolChainCore {
public:
    // alloc(Pool&) -> int32_t allocates the caller's sets from that pool; create(Pool*) -> int32_t makes one more pool of the chain's size.
    template <class Alloc, class Create>
    int32_t allocate(Alloc&& alloc, Create&& create) {
        if (pools_.empty()) {
            if (const int32_t r = grow(create); r != kDescPoolSuccess) return r;
        }
        const int32_t first = alloc(pools_[cur_]);
        if (!descPoolExhausted(first)) return first;
        ++retries_;
        if (cur_ + 1 < pools_.size()) ++cur_;   // a pool kept from an earlier mission (reset with the chain)
        else if (const int32_t r = grow(create); r != kDescPoolSuccess) return r;
        return alloc(pools_[cur_]);   // the one retry
    }
    // resetPool(Pool&) resets one pool; every set allocated from the chain is then invalid.
    template <class Reset>
    void reset(Reset&& resetPool) {
        for (auto& pool : pools_) resetPool(pool);
        cur_ = 0;
    }
    // Adopts an already created pool as the chain's first (init).
    void adopt(Pool pool) { pools_.clear(); pools_.push_back(std::move(pool)); cur_ = 0; }
    void clear() { pools_.clear(); cur_ = 0; }
    size_t length() const { return pools_.size(); }
    size_t current() const { return cur_; }
    uint64_t retries() const { return retries_; }   // exhausted-pool recoveries since init
    bool empty() const { return pools_.empty(); }

private:
    template <class Create>
    int32_t grow(Create&& create) {
        Pool pool{};
        const int32_t r = create(&pool);
        if (r != kDescPoolSuccess) return r;
        pools_.push_back(std::move(pool));
        cur_ = pools_.size() - 1;
        return r;
    }
    std::vector<Pool> pools_;
    size_t cur_ = 0;
    uint64_t retries_ = 0;
};

}  // namespace mc2vk
