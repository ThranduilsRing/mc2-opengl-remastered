#include "doctest.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include "../../GameOS/gameos/gos_terrain_lighting.h"

namespace {
using VkDeviceSize = uint64_t;
constexpr int VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC = 0;
constexpr int VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC = 1;
bool failPipeline = false, failDescriptors = false;
struct Pipeline {
    bool valid = false;
    explicit operator bool() const { return valid; }
    bool create(int, const char*, std::initializer_list<std::pair<int,int>>, const char*) { return valid = !failPipeline; }
};
struct Sets {
    bool valid = false;
    int set(int) const { return valid; }
    bool create(int, Pipeline&, int, const char*) { return valid = !failDescriptors; }
};
namespace mc2vk {
struct FrameAlloc {
    std::shared_ptr<std::vector<char>> bytes;
    void* ptr = nullptr;
    explicit operator bool() const { return bool(bytes); }
};
struct VulkanContext {
    uint64_t frame = 1;
    VkDeviceSize rejectAbove = UINT64_MAX;
    bool rejectAll = false;
    VulkanContext& tracker() { return *this; }
    uint64_t frameNo() const { return frame; }
    int device() const { return 0; }
    FrameAlloc frameAlloc(VkDeviceSize size) {
        if (rejectAll || size > rejectAbove) return {};
        auto bytes = std::make_shared<std::vector<char>>(size);
        return {bytes, bytes->data()};
    }
};
VulkanContext ctx;
VulkanContext& getOrCreateEngineVulkanContext() { return ctx; }
struct LightLists {
    Pipeline pipeline_;
    Sets sets_;
    FrameAlloc params_;
    uint64_t frame_ = ~uint64_t(0);
    std::array<FrameAlloc,3> spans_{};
    static constexpr uint32_t kDim = 64;
    float minInv[4]{};
#include "light_lists_stage.inc"
};
}
// The production gate uses a class-type string parameter.
namespace mc2::gate {
template<size_t N> struct Key { char value[N]; constexpr Key(const char (&s)[N]) { std::copy_n(s,N,value); } };
template<Key Name> bool get() { return true; }
}
struct TerrainIsland { mc2vk::LightLists lightLists; };
struct Mc2TerrainTier1Feed {
    const void* poolLights = nullptr;
    float lightPoolMoverScale = 1;
    const uint32_t* lightGridHeader = nullptr;
    const uint32_t* lightGridIndices = nullptr;
    uint32_t lightGridHeaderWords = 0, lightGridIndexWords = 0;
};
void vlog(const char*, ...) {}
#define ZoneScopedN(name)
#include "terrain_light_stage.inc"
#undef ZoneScopedN
}

TEST_CASE("LightLists/optional failures retain terrain bindings and recover next frame") {
    for (int fault = 0; fault < 5; ++fault) {
        mc2vk::ctx = {};
        failPipeline = fault == 0; failDescriptors = fault == 1;
        mc2vk::ctx.rejectAbove = fault == 2 ? 1048576 : fault == 3 ? 65536 : UINT64_MAX;
        mc2vk::ctx.rejectAll = fault == 4;
        TerrainIsland island;
        std::vector<gos_terrain_lighting::GpuTerrainLight> lights(4096);
        lights[0].lightType = 1; lights[0].farDistance = 10;
        Mc2TerrainTier1Feed feed; feed.poolLights = lights.data();
        int32_t count = 4096, dim = 64;
        mc2vk::FrameAlloc spans[3];
        CHECK(stage_light_pool(&island,feed,count,dim,spans) == (fault != 4));
        CHECK(dim == 0);
        CHECK(count == (fault >= 3 ? 0 : 4096));
        CHECK(island.lightLists.frame_ != mc2vk::ctx.frame);
        if (fault != 4) for (auto& span : spans) CHECK(bool(span));
        ++mc2vk::ctx.frame;
        failPipeline = failDescriptors = false;
        mc2vk::ctx.rejectAll = false; mc2vk::ctx.rejectAbove = UINT64_MAX;
        count = 4096; dim = 64;
        CHECK(stage_light_pool(&island,feed,count,dim,spans));
        CHECK(count == 4096); CHECK(dim == 64);
        CHECK(island.lightLists.sets_.valid);
        CHECK(spans[2].bytes->size() == 64u*64u*4096u*4u);
    }
}
