"""Run the production initializer against allocation/upload fault injection.

Only Vulkan/VMA ownership is stubbed; the C++ method bodies come from the engine.
This catches partial publication without a GPU, a game install, or a driver crash.
"""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child
from smoke_lib import nofocus


def probe_source(engine_source):
    methods = []
    for signature in ("bool ShadowBake::ensureIncremental()", "void ShadowBake::releaseIncremental()"):
        start = engine_source.index(signature)
        end = engine_source.index("\n}", start) + 2
        methods.append(engine_source[start:end])
    return r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>
static int operation, failAt, liveBuffers;
static bool succeed() { return ++operation != failAt; }
struct OwnedBuffer {
    bool valid = false;
    OwnedBuffer() = default;
    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(OwnedBuffer&& other) {
        reset(); valid = std::exchange(other.valid, false); return *this;
    }
    ~OwnedBuffer() { reset(); }
    void reset() { if (valid) --liveBuffers; valid = false; }
    explicit operator bool() const { return valid; }
    bool upload(const void*, size_t) { return valid && succeed(); }
};
struct OwnedQueryPool {
    bool valid = false;
    explicit operator bool() const { return valid; }
};
static constexpr int kMaxFramesInFlight = 3, kSubTilesPerFrame = 2, kSlice = 256;
namespace mc2shadow {
struct Quality { int level = 2; };
static Quality shadowQuality() { return {}; }
}
struct GpuStaticPropInstance { char data[112]; };
static constexpr int VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT = 1;
static constexpr int VK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 2;
static constexpr int VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO = 1;
static constexpr int VK_QUERY_TYPE_TIMESTAMP = 2;
struct VkQueryPoolCreateInfo { int type; int queryType = 0, queryCount = 0; };
static bool makeHostBuffer(OwnedBuffer& b, int, int, size_t, int, const char*) {
    b.reset(); b.valid = succeed(); liveBuffers += b.valid; return b.valid;
}
static int vkCreateQueryPool(int, const VkQueryPoolCreateInfo* info, void*, int* out) {
    assert(info->queryCount == 2 * kMaxFramesInFlight);
    *out = succeed(); return *out ? 0 : -1;
}
template<class F> bool create(OwnedQueryPool& pool, int, F call) {
    int handle = 0; bool ok = call(&handle) == 0 && handle;
    pool.valid = ok; return ok;
}
static void blog(const char*) {}
struct ShadowBake {
    int allocator_ = 1, device_ = 1;
    uint32_t incInstanceCap_ = 0;
    OwnedBuffer incParams_, incCasterUbo_, incPass_, incInstances_;
    OwnedBuffer incHeights_[kMaxFramesInFlight];
    OwnedQueryPool incQueries_;
    uint64_t incHeightsGen_[kMaxFramesInFlight] = {};
    bool incQueryUsed_[kMaxFramesInFlight] = {};
    bool ensureIncremental();
    void releaseIncremental();
    bool complete() const {
        return incParams_ && incCasterUbo_ && incPass_ && incInstances_ && incQueries_;
    }
};
''' + "\n".join(methods) + r'''
int main() {
    // Four allocations, pass upload, query allocation. Reproduce the W91
    // fourth-allocation refusal, then retry and recreate for every parity.
    for (int failure = 1; failure <= 6; ++failure) {
        ShadowBake bake;
        operation = 0; failAt = failure;
        assert(!bake.ensureIncremental());
        assert(!bake.incParams_ && liveBuffers == 0);
        failAt = 0;
        assert(bake.ensureIncremental() && bake.complete());
        int before = operation;
        assert(bake.ensureIncremental() && operation == before);
        for (int frame = 0; frame < 90; ++frame) {
            bake.releaseIncremental();
            assert(liveBuffers == 0);
            operation = 0; failAt = 4;
            assert(!bake.ensureIncremental());
            assert(!bake.incParams_ && liveBuffers == 0);
            failAt = 0;
            assert(bake.ensureIncremental() && bake.complete());
            bake.incQueryUsed_[frame % kMaxFramesInFlight] = true;
        }
    }
    assert(liveBuffers == 0);
}
'''


def test_incremental_initialization_rolls_back_and_retries(tmp_path):
    source = tmp_path / "shadow_init.cpp"
    source.write_text(probe_source((ROOT / "GameOS/gameos/vulkan_shadow_bake.cpp").read_text()))
    exe = tmp_path / ("shadow_init.exe" if os.name == "nt" else "shadow_init")
    result = subprocess.run(["clang++", "-std=c++20", "-O0", str(source), "-o", str(exe)],
                            capture_output=True, text=True,
                            **console_child.child_kwargs(redirected=True))
    assert result.returncode == 0, result.stderr
    result = nofocus.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
