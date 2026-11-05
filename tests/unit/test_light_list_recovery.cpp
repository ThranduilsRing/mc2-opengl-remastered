#include "doctest.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>
#include "../../GameOS/gameos/gos_terrain_lighting.h"

namespace {
#include "../../GameOS/gameos/vk_frame_ring.h"
using VkDeviceSize = uint64_t;
constexpr int VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC = 0;
constexpr int VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC = 1;
bool failPipeline = false, failDescriptors = false;
unsigned pipelineAttempts = 0, descriptorAttempts = 0, lowerLogs = 0;
struct Pipeline {
    bool valid = false;
    explicit operator bool() const { return valid; }
    bool create(int, const char*, std::initializer_list<std::pair<int,int>>, const char*, bool logFailures) {
        ++pipelineAttempts;
        if (failPipeline && logFailures) ++lowerLogs;
        return valid = !failPipeline;
    }
};
struct Sets {
    bool valid = false;
    int count = 0;
    int set(int index) const { return valid && index >= 0 && index < count; }
    bool create(int, Pipeline&, int slots, const char*, bool logFailures) {
        ++descriptorAttempts;
        count = slots;
        if (failDescriptors && logFailures) ++lowerLogs;
        return valid = !failDescriptors;
    }
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
    unsigned allocations = 0;
    VulkanContext& tracker() { return *this; }
    uint64_t frameNo() const { return frame; }
    int device() const { return 0; }
    FrameAlloc frameAlloc(VkDeviceSize size) {
        ++allocations;
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
    uint64_t attempted_ = ~uint64_t(0), retryAt_ = 0, failures_ = 0;
    uint32_t retryDelay_ = 1;
    const char* failure_ = nullptr;
    std::array<FrameAlloc,3> spans_{};
    static constexpr uint32_t kDim = 64;
    uint32_t poolWords_ = 0;
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
        for (int i = 0; i < 80; ++i) { lights[i].lightType = 1; lights[i].farDistance = 10; }   // pool must exceed the 1 MiB fault threshold
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
        for (uint32_t slot = 0; slot < mc2vk::kMaxFramesInFlight; ++slot)
            CHECK(island.lightLists.sets_.set(slot) != 0);
        CHECK(spans[2].bytes->size() > 4u); CHECK(spans[2].bytes->size() <= (1u + 64u*64u*4096u)*4u);
    }
}

TEST_CASE("LightLists/failures cache same-frame attempts and back off with bounded diagnostics") {
    for (int fault = 0; fault < 5; ++fault) {
        CAPTURE(fault);
        mc2vk::ctx = {};
        pipelineAttempts = descriptorAttempts = lowerLogs = 0;
        failPipeline = fault == 0; failDescriptors = fault == 1;
        mc2vk::ctx.rejectAbove = fault == 2 ? 1048576 : fault == 3 ? 65536 : UINT64_MAX;
        mc2vk::ctx.rejectAll = fault == 4;
        TerrainIsland island;
        std::vector<gos_terrain_lighting::GpuTerrainLight> lights(4096);
        for (int i = 0; i < 80; ++i) { lights[i].lightType = 1; lights[i].farDistance = 10; }   // pool must exceed the 1 MiB fault threshold
        Mc2TerrainTier1Feed feed; feed.poolLights = lights.data();
        // Terrain and overlays call the real stage wrapper repeatedly. Only
        // fallback allocations may repeat: the accelerator tries once per frame.
        for (unsigned call = 0; call < 4; ++call) {
            int32_t count = 4096, dim = 64;
            mc2vk::FrameAlloc spans[3];
            const auto allocations = mc2vk::ctx.allocations;
            CHECK(stage_light_pool(&island, feed, count, dim, spans) == (fault != 4));
            CHECK(dim == 0);
            CHECK(count == (fault >= 3 ? 0 : 4096));
            CHECK(island.lightLists.failures_ == 1);
            // A rejected index request consumes no successful accelerator span.
            if (fault == 2) CHECK(mc2vk::ctx.allocations - allocations == (call == 0 ? 4 : 3));
        }
        const char* failure = nullptr;
        mc2vk::FrameAlloc spans[3];
        for (uint64_t frame = 2; frame <= 1024; ++frame) {
            mc2vk::ctx.frame = frame;
            const auto attempts = island.lightLists.failures_;
            const bool eligible = frame >= island.lightLists.retryAt_;
            const auto allocations = mc2vk::ctx.allocations;
            CHECK_FALSE(island.lightLists.stage(mc2vk::ctx, lights.data(), 4096, 1, spans, failure));
            CHECK(island.lightLists.failures_ == attempts + unsigned(eligible));
            if (!eligible) CHECK(mc2vk::ctx.allocations == allocations);
            CHECK(island.lightLists.retryDelay_ <= 256);
            const auto after = mc2vk::ctx.allocations;
            CHECK_FALSE(island.lightLists.stage(mc2vk::ctx, lights.data(), 4096, 1, spans, failure));
            CHECK(mc2vk::ctx.allocations == after);
        }
        CHECK(island.lightLists.failures_ <= 12);
        CHECK(lowerLogs == (fault < 2 ? 1 : 0));
        CHECK(pipelineAttempts <= 12);
        CHECK(descriptorAttempts <= 12);
        // Restore resources at the next scheduled retry. Same-frame success is
        // then reused by all receivers without another allocation.
        mc2vk::ctx.frame = island.lightLists.retryAt_;
        failPipeline = failDescriptors = false;
        mc2vk::ctx.rejectAll = false; mc2vk::ctx.rejectAbove = UINT64_MAX;
        CHECK(island.lightLists.stage(mc2vk::ctx, lights.data(), 4096, 1, spans, failure));
        CHECK(island.lightLists.failures_ == 0);
        CHECK(failure == nullptr);
        const auto allocations = mc2vk::ctx.allocations;
        CHECK(island.lightLists.stage(mc2vk::ctx, lights.data(), 4096, 1, spans, failure));
        CHECK(mc2vk::ctx.allocations == allocations);
    }
}
