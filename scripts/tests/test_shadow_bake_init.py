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


def test_products_and_transients_publish_atomically(tmp_path):
    engine = (ROOT / 'GameOS/gameos/vulkan_shadow_bake.cpp').read_text()
    methods = []
    for signature in ('bool ShadowBake::ensureProducts(int maskTexels)',
                      'bool ShadowBake::ensureTransients()'):
        start = engine.index(signature)
        methods.append(engine[start:engine.index('\n}', start) + 2])
    probe = r'''
#include <cassert>
#include <cstdint>
#include <utility>
#define ZoneScopedN(x)
static int operation, failAt, live;
static bool succeed() { return ++operation != failAt; }
struct Owned {
    bool valid = false;
    Owned() = default;
    Owned(const Owned&) = delete;
    ~Owned() { reset(); }
    Owned& operator=(Owned&& other) {
        reset(); valid = std::exchange(other.valid, false); return *this;
    }
    void reset() { live -= valid; valid = false; }
    explicit operator bool() const { return valid; }
    int get() const { return valid; }
    template<class... A> bool create(A...) {
        reset(); valid = succeed(); live += valid; return valid;
    }
};
using OwnedImage = Owned;
using OwnedImageView = Owned;
using OwnedFramebuffer = Owned;
using OwnedBuffer = Owned;
using VkFormat = int;
using VkDeviceSize = uint64_t;
enum { VK_IMAGE_USAGE_SAMPLED_BIT = 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT = 2,
       VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 4, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT = 8,
       VK_IMAGE_ASPECT_COLOR_BIT = 1, VK_IMAGE_ASPECT_DEPTH_BIT = 2,
       VK_IMAGE_USAGE_STORAGE_BIT = 16, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 1,
       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 1, VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 2,
       VMA_MEMORY_USAGE_AUTO = 1, VK_FORMAT_R16G16B16A16_SFLOAT = 1, VK_FORMAT_R32_SFLOAT = 2 };
struct VkBufferCreateInfo { int type; uint64_t size = 0; int usage = 0; };
struct VmaAllocationCreateInfo { int usage = 0; };
static constexpr int kMaskFmt = 1, kWorkFmt = 2, kObjectFmt = 3, kWindowFmt = 4;
static constexpr int kTopLevel = 7, kTileLevels = 8;
namespace mc2shadow {
static constexpr int kSubTileTexels = 32, kRegionTexels = 1024, kTileTexels = 512;
struct Quality { int windowSide = 512; };
static Quality shadowQuality() { return {}; }
}
static bool makeImage(Owned& image, Owned& view, int, int, int, uint32_t, uint32_t,
                      int, int, const char*) {
    return image.create() && view.create();
}
static bool makeFramebuffer(Owned& fb, int, int, int view, int) {
    assert(view); return fb.create();
}
static void blog(const char*) {}
struct ShadowBake {
    int allocator_ = 1, device_ = 1;
    int maskTexels_ = 0;
    uint32_t maskMips_ = 0, topSide_ = 0, topMips_ = 0;
    bool maskInitialized_ = true, topInitialized_ = true, objectInitialized_ = true;
    uint64_t transientBytes_ = 0;
    OwnedImage maskImage_, topR8_, objectImage_, windowImage_, scratch_[4], tileR8_;
    OwnedImageView maskView_, topR8View_, objectView_, windowView_, scratchView_[4], tileR8View_;
    OwnedFramebuffer objectFb_, windowFb_;
    Owned objectPass_, windowPass_, staging_;
    struct Dirty { int side = 0; void reset(int n) { side = n; } } dirty_;
    static int objectMapSize() { return 512; }
    bool ensureEncoder() { return succeed(); }
    bool ensureProducts(int);
    bool ensureTransients();
    bool productsComplete() const {
        return maskImage_ && maskView_ && topR8_ && topR8View_ && objectImage_ && objectView_ && objectFb_;
    }
    bool transientsComplete() const {
        if (!(windowImage_ && windowView_ && windowFb_ && staging_ && tileR8_ && tileR8View_)) return false;
        for (int i = 0; i < 4; ++i) if (!(scratch_[i] && scratchView_[i])) return false;
        return true;
    }
};
''' + '\n'.join(methods) + r'''
int main() {
    for (int failure = 1; failure <= 7; ++failure) {
        ShadowBake bake;
        operation = 0; failAt = failure;
        assert(!bake.ensureProducts(4096));
        assert(live == 0 && bake.maskTexels_ == 0 && bake.dirty_.side == 0);
        failAt = 0;
        assert(bake.ensureProducts(4096) && bake.productsComplete());
        int before = operation;
        assert(bake.ensureProducts(4096) && operation == before);
        // Refuse each mask/top allocation at a map resize; retain all old products.
        for (int resizeFailure = 1; resizeFailure <= 4; ++resizeFailure) {
            operation = 0; failAt = resizeFailure;
            assert(!bake.ensureProducts(8192));
            assert(bake.maskTexels_ == 4096 && bake.productsComplete() && live == 7);
        }
        failAt = 0;
        assert(bake.ensureProducts(8192) && bake.productsComplete() && live == 7);
    }
    assert(live == 0);
    for (int failure = 1; failure <= 15; ++failure) {
        ShadowBake bake;
        operation = 0; failAt = failure;
        assert(!bake.ensureTransients());
        if (failure <= 14) assert(live == 0 && !bake.windowImage_);
        else assert(bake.transientsComplete()); // encoder refusal retries separately
        failAt = 0;
        assert(bake.ensureTransients() && bake.transientsComplete());
    }
    assert(live == 0);
}
'''
    source = tmp_path / 'shadow_products.cpp'
    source.write_text(probe)
    exe = tmp_path / 'shadow_products.exe'
    result = subprocess.run(['clang++', '-std=c++20', str(source), '-o', str(exe)],
                            capture_output=True, text=True,
                            **console_child.child_kwargs(redirected=True))
    assert result.returncode == 0, result.stderr
    result = nofocus.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
