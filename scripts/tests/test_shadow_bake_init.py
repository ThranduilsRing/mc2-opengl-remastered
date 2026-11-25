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


def test_cache_load_wait_respects_deadline_and_preserves_future(tmp_path):
    # Trigger: vulkan_shadow_bake.h and vulkan_shadow_island_live.cpp load barrier.
    header = (ROOT / "GameOS/gameos/vulkan_shadow_bake.h").read_text()
    start = header.index("    void waitForCacheLoad(")
    method = header[start:header.index("\n    }", start) + 6]
    source = tmp_path / "cache_wait.cpp"
    source.write_text(r'''
#include <cassert>
#include <chrono>
#include <future>
struct ShadowBake {
    std::future<int> cacheLoad_;
''' + method + r'''
};
int main() {
    using namespace std::chrono;
    std::promise<int> stalled;
    ShadowBake bake{stalled.get_future()};
    const auto deadline = steady_clock::now() + milliseconds(30);
    bake.waitForCacheLoad(deadline);
    assert(steady_clock::now() >= deadline);
    assert(steady_clock::now() < deadline + seconds(1));
    assert(bake.cacheLoad_.valid());
    assert(bake.cacheLoad_.wait_for(seconds(0)) == std::future_status::timeout);
    stalled.set_value(7);
    assert(bake.cacheLoad_.wait_for(seconds(0)) == std::future_status::ready);
    assert(bake.cacheLoad_.get() == 7);
    bake.waitForCacheLoad(deadline); // A consumed/absent future is also safe.
}
''')
    exe = tmp_path / ("cache_wait.exe" if os.name == "nt" else "cache_wait")
    result = subprocess.run(["clang++", "-std=c++20", "-pthread", str(source), "-o", str(exe)],
                            capture_output=True, text=True, timeout=30,
                            **console_child.child_kwargs(redirected=True))
    assert result.returncode == 0, result.stderr
    # Hang guard only (the program asserts its own 1 s bound); the first launch of a fresh exe under a loaded board took > 3 s.
    result = nofocus.run([str(exe)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr


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
// vk-memory-placement S2/F10: the GPU-read feeds allocate through makeGpuReadBuffer (same ownership contract).
static bool makeGpuReadBuffer(OwnedBuffer& b, int a, int d, size_t n, int u, const char* name) {
    return makeHostBuffer(b, a, d, n, u, name);
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
    cache = (ROOT / 'GameOS/gameos/vulkan_shadow_bake_cache.cpp').read_text()
    start = cache.index('bool ShadowBake::ensureEncoder()')
    methods.append(cache[start:cache.index('\n}', start) + 2])
    probe = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>
#define ZoneScopedN(x)
namespace mc2vk { inline void bumpDescEpoch() {} }   // descriptor write-cache epoch (vk_desc_write_cache.h)
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
    bool upload(const void*, size_t) { return valid && succeed(); }
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
       VMA_MEMORY_USAGE_AUTO = 1, VK_FORMAT_R16G16B16A16_SFLOAT = 1, VK_FORMAT_R32_SFLOAT = 2,
       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT = 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT = 8, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT = 16, VK_WHOLE_SIZE = -1,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 1, VK_IMAGE_LAYOUT_GENERAL = 2, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 1,
       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC = 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 2,
       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER = 3 };
using VkDescriptorSet = int;
struct VkDescriptorBufferInfo { int buffer; int offset, range; };
struct VkDescriptorImageInfo { int sampler, view, layout; };
struct VkWriteDescriptorSet {
    int sType, dstSet, descriptorCount, descriptorType;
    uint32_t dstBinding;
    const VkDescriptorBufferInfo* pBufferInfo;
    const VkDescriptorImageInfo* pImageInfo;
};
static void vkUpdateDescriptorSets(int, int, const VkWriteDescriptorSet*, int, void*) {}
struct VkBufferCreateInfo { int type; uint64_t size = 0; int usage = 0; };
struct VmaAllocationCreateInfo { int usage = 0; };
static constexpr int kMaskFmt = 1, kWorkFmt = 2, kObjectFmt = 3, kWindowFmt = 4;
static constexpr int kTopLevel = 7, kTileLevels = 8, kSlice = 256, kSliceSubTile = kTileLevels;
namespace mc2shadow {
static constexpr int kSubTileTexels = 32, kRegionTexels = 1024, kTileTexels = 512;
struct Quality { int windowSide = 512; };
static Quality shadowQuality() { return {}; }
static uint64_t bc4ChainBytes(int side, int) { return side * side; }
}
static bool makeHostBuffer(Owned& buffer, int, int, uint64_t, int, const char*) { return buffer.create(); }
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
    bool tileGeneral_ = false;
    uint64_t transientBytes_ = 0;
    OwnedImage maskImage_, topR8_, objectImage_, windowImage_, scratch_[4], tileR8_;
    OwnedImageView maskView_, topR8View_, objectView_, windowView_, scratchView_[4], tileR8View_;
    Owned objectPass_, windowPass_, staging_, blockStaging_, bc4Params_, depthRawSampler_;
    uint64_t tileLevelOff_[kTileLevels] = {};
    std::vector<uint64_t> topLevelOff_;
    int bc4TileSet_ = 1, bc4TopSet_ = 2;
    struct Dirty { int side = 0; void reset(int n) { side = n; } } dirty_;
    static int objectMapSize() { return 512; }
    bool ensureEncoder();
    bool ensureProducts(int);
    bool ensureTransients();
    bool productsComplete() const {
        return maskImage_ && maskView_ && topR8_ && topR8View_ && objectImage_ && objectView_;
    }
    bool transientsComplete() const {
        if (!(windowImage_ && windowView_ && staging_ && tileR8_ && tileR8View_)) return false;
        for (int i = 0; i < 4; ++i) if (!(scratch_[i] && scratchView_[i])) return false;
        return true;
    }
};
''' + '\n'.join(methods) + r'''
int main() {
    for (int failure = 1; failure <= 6; ++failure) {   // vk-dynamic-rendering-s1 (W147): no framebuffers, one allocation fewer
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
            assert(bake.maskTexels_ == 4096 && bake.productsComplete() && live == 6);
        }
        failAt = 0;
        assert(bake.ensureProducts(8192) && bake.productsComplete() && live == 6);
    }
    assert(live == 0);
    for (int failure = 1; failure <= 16; ++failure) {
        ShadowBake bake;
        operation = 0; failAt = failure;
        assert(!bake.ensureTransients());
        if (failure <= 13) assert(live == 0 && !bake.windowImage_);
        else assert(bake.transientsComplete()); // encoder refusal retries separately
        failAt = 0;
        assert(bake.ensureTransients() && bake.transientsComplete());
    }
    assert(live == 0);
    for (int failure = 1; failure <= 3; ++failure) {
        ShadowBake bake;
        operation = 0; failAt = failure;
        assert(!bake.ensureEncoder() && live == 0);
        failAt = 0;
        assert(bake.ensureEncoder() && live == 2);
        int before = operation;
        assert(bake.ensureEncoder() && operation == before);
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
def test_object_rebakes_bound_height_builds_and_refresh_edits(tmp_path):
    # Trigger: vulkan_shadow_bake.cpp; execute production refresh and success paths.
    engine = (ROOT / "GameOS/gameos/vulkan_shadow_bake.cpp").read_text()
    guard = next(line for line in engine.splitlines() if "&& !readHeightGrid()" in line)
    refresh = engine.split("bool ShadowBake::readHeightGrid()", 1)[1].split("bool ShadowBake::recordColdTile", 1)[0]
    success = refresh[refresh.index("    heightsMoved_ = false;"):]
    probe = r'''
#include <cassert>
#include <vector>
bool recordedMask_ = false, heightsMoved_ = false, fail = false;
std::vector<float> heights_;
unsigned builds = 0, heightsGen_ = 0;
struct Store { unsigned version = 1; unsigned heightVersion() const { return version; } };
Store field; Store* store = &field;
unsigned readHeightVersion_ = ~0u;
float terrain = 2;
void blog(const char*) {}
bool readHeightGrid() { if (fail) return false; ++builds; heights_.assign(16, terrain);
''' + success + r'''
bool record() {
''' + guard + r'''
    return true;
}
int main() {
    for (int i = 0; i < 300; ++i) assert(record());
    assert(builds == 1 && heights_[0] == 2 && heightsGen_ == 1);
    terrain = 3; heightsMoved_ = true; fail = true; assert(!record());
    assert(heightsMoved_ && heightsGen_ == 1); fail = false; assert(record());
    for (int i = 0; i < 300; ++i) assert(record());
    assert(builds == 2 && heights_[0] == 3 && !heightsMoved_ && heightsGen_ == 2);
    recordedMask_ = true; assert(record());
    assert(builds == 3 && heightsGen_ == 3);
    recordedMask_ = false; terrain = 4; ++field.version; fail = true;
    assert(!record() && readHeightVersion_ != field.version);
    fail = false; assert(record());
    assert(builds == 4 && heights_[0] == 4 && readHeightVersion_ == field.version);
    for (int i = 0; i < 300; ++i) assert(record());
    assert(builds == 4);
}
'''
    source = tmp_path / "height_budget.cpp"
    source.write_text(probe)
    exe = tmp_path / ("height_budget.exe" if os.name == "nt" else "height_budget")
    result = subprocess.run(["clang++", "-std=c++20", str(source), "-o", str(exe)],
                            capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    assert result.returncode == 0, result.stderr
    result = nofocus.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_planar_images_share_residents_without_reimport(tmp_path):
    # Trigger: terrain island run/internal and color prop island; execute borrowing paths.
    terrain = (ROOT / 'GameOS/gameos/vulkan_terrain_island_run.cpp').read_text()
    props = (ROOT / 'GameOS/gameos/vulkan_color_prop_island.cpp').read_text()
    helper = terrain[terrain.index('static bool borrow_terrain_images'):terrain.index('static int run_terrain')]
    importer = props.split('bool cp_import_albedo_array(', 1)[1]
    branch = importer.split('    if (const auto* main = s->imageSource) {', 1)[1].split('    const bool compressed', 1)[0]
    images = 'colormapTexture macroGradientTexture cementTexture shoreMaskTexture materialTexture matNormalTexture concreteDetailTexture forestMaskTexture'.split()
    flags = 'colormapReady colormapBreakup cementReady shoreMaskReady matNormalReady concreteDetailReady cliffRockPresent'.split()
    probe = r'''
#include <array>
#include <cassert>
#include <memory>
#include <cstdint>
struct Ref {
    std::shared_ptr<int> p;
    int* get() const { return p.get(); }
    explicit operator bool() const { return bool(p); }
};
struct Textures {
    unsigned retains = 0;
    Ref retain(int* p) { ++retains; return {p ? owners.at(*p) : nullptr}; }
    std::array<std::shared_ptr<int>, 16> owners;
} textures;
namespace mc2vk {
struct Context { Textures& textures() { return ::textures; } } context;
Context& getOrCreateEngineVulkanContext() { return context; }
}
struct TerrainIsland {
    bool inited = true, disabled = false;
    std::array<float, 9> colormapCookParams{};
    unsigned matNormalLayers = 0;
''' + 'Ref ' + ', '.join(images) + ';\nbool ' + ', '.join(flags) + r''';
};
TerrainIsland* parked = nullptr;
TerrainIsland* parkedIsland() { return parked; }
void write_descriptors(TerrainIsland*) {}
''' + helper + r'''
namespace mc2::prop_buckets { constexpr unsigned kMaxBuckets = 2; }
struct ColorPropIsland {
    ColorPropIsland* imageSource = nullptr;
    std::array<Ref, 2> albedoTexture, albedoOnTexture;
    bool materialReady = true, materialReadyOn = true;
    unsigned albedoGlTexImported = 4, albedoOnGlTexImported = 5, albedoLayers = 2, albedoOnLayers = 3;
    unsigned alphaContentHash = 17, materialGeneration = 0;
};
struct AlbedoTarget { Ref* texture; unsigned* layers; unsigned* importedGlName; bool* ready; int* samp; };
bool cp_make_albedo_sampler(ColorPropIsland*, int*, int) { return true; }
bool borrow(ColorPropIsland* s, const AlbedoTarget& tgt, unsigned changeKey) {
    if (const auto* main = s->imageSource) {
''' + branch + r'''
    assert(false); return false; // main import/pack must be unreachable for planar
}
int main() {
    for (int i = 0; i < 16; ++i) textures.owners[i] = std::make_shared<int>(i);
    TerrainIsland main{}, planar{}; parked = &main;
''' + '\n'.join(f'    main.{name} = {{textures.owners[{i}]}};' for i, name in enumerate(images)) + r'''
    main.colormapReady = main.concreteDetailReady = true;
    main.matNormalLayers = 9;
    assert(borrow_terrain_images(&planar));
    auto retains = textures.retains;
    for (int i = 0; i < 300; ++i) assert(borrow_terrain_images(&planar));
    assert(textures.retains == retains && planar.colormapReady && planar.concreteDetailReady && planar.matNormalLayers == 9);
    main.materialTexture = {textures.owners[8]}; assert(borrow_terrain_images(&planar));
    assert(planar.materialTexture.get() == main.materialTexture.get() && textures.retains == retains + 1);
    parked = nullptr; assert(!borrow_terrain_images(&planar));
    main.colormapTexture = {}; assert(planar.colormapTexture);
    ColorPropIsland source, reflection; reflection.imageSource = &source;
    for (unsigned b = 0; b < 2; ++b) {
        source.albedoTexture[b] = {textures.owners[b]}; source.albedoOnTexture[b] = {textures.owners[b + 2]};
        unsigned layers = 0, key = 0; bool ready = false;
        AlbedoTarget off{&reflection.albedoTexture[b], &layers, &key, &ready, nullptr};
        assert(borrow(&reflection, off, 4) && layers == 2 && key == 4 && ready);
        assert(reflection.albedoTexture[b].get() == source.albedoTexture[b].get());
        AlbedoTarget on{&reflection.albedoOnTexture[b], &layers, &key, &ready, nullptr};
        assert(!borrow(&reflection, on, 6));
        source.materialReadyOn = false; assert(!borrow(&reflection, on, 5)); source.materialReadyOn = true;
        assert(borrow(&reflection, on, 5) && layers == 3 && key == 5);
        assert(reflection.albedoOnTexture[b].get() == source.albedoOnTexture[b].get());
    }
}
'''
    source = tmp_path / 'planar_share.cpp'
    source.write_text(probe)
    exe = tmp_path / ('planar_share.exe' if os.name == 'nt' else 'planar_share')
    result = subprocess.run(['clang++', '-std=c++20', str(source), '-o', str(exe)],
                            capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    assert result.returncode == 0, result.stderr
    result = nofocus.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
