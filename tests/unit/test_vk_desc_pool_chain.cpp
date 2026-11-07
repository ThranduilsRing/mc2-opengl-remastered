// The chained descriptor pools: allocation past one pool's capacity moves to a fresh pool and retries once; OUT_OF_POOL_MEMORY
// and FRAGMENTED_POOL both recover; a failed retry or any other error reaches the caller unchanged; reset keeps capacity, so
// repeated missions with the same working set keep the same chain length.
#include "doctest.h"

#include "vk_desc_pool_chain.h"

#include <deque>

using mc2vk::DescPoolChainCore;
using mc2vk::kDescPoolSuccess;
using mc2vk::kVkErrorFragmentedPool;
using mc2vk::kVkErrorOutOfPoolMemory;

namespace {

constexpr int32_t kVkErrorOutOfDeviceMemory = -2;

struct FakePool { size_t id = 0; };

// The backend: pools of `cap` sets each; `exhaustResult` is what an exhausted pool returns.
struct FakeBackend {
    explicit FakeBackend(uint32_t c, int32_t exhausted = kVkErrorOutOfPoolMemory) : cap(c), exhaustResult(exhausted) {}
    uint32_t cap;
    int32_t exhaustResult;
    int32_t createResult = kDescPoolSuccess;
    std::deque<uint32_t> used;   // sets allocated per pool id
    size_t creates = 0;
    size_t resets = 0;

    auto creator() {
        return [this](FakePool* p) -> int32_t {
            if (createResult != kDescPoolSuccess) return createResult;
            p->id = used.size();
            used.push_back(0);
            ++creates;
            return kDescPoolSuccess;
        };
    }
    auto allocator() {
        return [this](FakePool& p) -> int32_t {
            if (used[p.id] >= cap) return exhaustResult;
            ++used[p.id];
            return kDescPoolSuccess;
        };
    }
    auto resetter() {
        return [this](FakePool& p) { used[p.id] = 0; ++resets; };
    }
};

// Allocates `n` identities (one set each); returns how many succeeded.
size_t drive(DescPoolChainCore<FakePool>& chain, FakeBackend& be, size_t n) {
    size_t ok = 0;
    for (size_t i = 0; i < n; ++i)
        if (chain.allocate(be.allocator(), be.creator()) == kDescPoolSuccess) ++ok;
    return ok;
}

}  // namespace

TEST_CASE("desc pool chain: legacy (192), overlay (480) and vfx sprite (64) caps are exceeded by chaining") {
    struct Cap { uint32_t perPool; size_t identities; size_t expectPools; };
    for (const Cap c : {Cap{192, 1000, 6}, Cap{480, 1000, 3}, Cap{64, 1000, 16}}) {
        CAPTURE(c.perPool);
        FakeBackend be(c.perPool);
        DescPoolChainCore<FakePool> chain;
        CHECK(drive(chain, be, c.identities) == c.identities);
        CHECK(chain.length() == c.expectPools);
        CHECK(chain.retries() == c.expectPools - 1);
        CHECK(chain.current() == c.expectPools - 1);
    }
}

TEST_CASE("desc pool chain: reset keeps capacity and repeated missions keep the chain length stable") {
    FakeBackend be(64);
    DescPoolChainCore<FakePool> chain;
    REQUIRE(drive(chain, be, 300) == 300);
    const size_t len = chain.length();
    CHECK(len == 5);
    for (int mission = 0; mission < 5; ++mission) {
        chain.reset(be.resetter());
        CHECK(chain.current() == 0);
        CHECK(chain.length() == len);
        CHECK(drive(chain, be, 300) == 300);
        CHECK(chain.length() == len);   // the kept pools are reused, nothing new is created
    }
    CHECK(be.creates == len);
    CHECK(be.resets == len * 5);
    // A smaller mission reuses a prefix; a larger one grows only by what it needs.
    chain.reset(be.resetter());
    CHECK(drive(chain, be, 10) == 10);
    CHECK(chain.length() == len);
    chain.reset(be.resetter());
    CHECK(drive(chain, be, 400) == 400);
    CHECK(chain.length() == 7);
}

TEST_CASE("desc pool chain: injected OUT_OF_POOL_MEMORY and FRAGMENTED_POOL each recover with one retry on a fresh pool") {
    for (const int32_t err : {kVkErrorOutOfPoolMemory, kVkErrorFragmentedPool}) {
        CAPTURE(err);
        FakeBackend be(1000);
        DescPoolChainCore<FakePool> chain;
        int calls = 0;
        // The seam: the first call on pool 0 reports the injected error; the retry lands on pool 1.
        auto inject = [&](FakePool& p) -> int32_t {
            ++calls;
            if (p.id == 0) return err;
            return be.allocator()(p);
        };
        CHECK(chain.allocate(inject, be.creator()) == kDescPoolSuccess);
        CHECK(calls == 2);
        CHECK(chain.retries() == 1);
        CHECK(chain.length() == 2);
        CHECK(chain.current() == 1);
        CHECK(be.used[1] == 1);
    }
}

TEST_CASE("desc pool chain: a failed retry returns the actual result and retries only once") {
    for (const int32_t err : {kVkErrorOutOfPoolMemory, kVkErrorFragmentedPool}) {
        CAPTURE(err);
        FakeBackend be(4);
        DescPoolChainCore<FakePool> chain;
        int calls = 0;
        auto alwaysExhausted = [&](FakePool&) -> int32_t { ++calls; return err; };
        CHECK(chain.allocate(alwaysExhausted, be.creator()) == err);
        CHECK(calls == 2);
        CHECK(chain.retries() == 1);
        CHECK(chain.length() == 2);
    }
}

TEST_CASE("desc pool chain: a non-pool error returns at once with no retry") {
    FakeBackend be(4);
    DescPoolChainCore<FakePool> chain;
    int calls = 0;
    auto oom = [&](FakePool&) -> int32_t { ++calls; return kVkErrorOutOfDeviceMemory; };
    CHECK(chain.allocate(oom, be.creator()) == kVkErrorOutOfDeviceMemory);
    CHECK(calls == 1);
    CHECK(chain.retries() == 0);
    CHECK(chain.length() == 1);
}

TEST_CASE("desc pool chain: a pool create failure propagates (first pool and growth)") {
    FakeBackend be(2);
    DescPoolChainCore<FakePool> chain;
    be.createResult = kVkErrorOutOfDeviceMemory;
    CHECK(chain.allocate(be.allocator(), be.creator()) == kVkErrorOutOfDeviceMemory);
    CHECK(chain.empty());
    be.createResult = kDescPoolSuccess;
    CHECK(drive(chain, be, 2) == 2);
    be.createResult = kVkErrorOutOfDeviceMemory;
    CHECK(chain.allocate(be.allocator(), be.creator()) == kVkErrorOutOfDeviceMemory);
    CHECK(chain.length() == 1);
    CHECK(chain.retries() == 1);
}

TEST_CASE("desc pool chain: adopt and clear") {
    DescPoolChainCore<FakePool> chain;
    chain.adopt(FakePool{7});
    CHECK(chain.length() == 1);
    CHECK(chain.current() == 0);
    chain.clear();
    CHECK(chain.empty());
}
