// Ray-tracing tier selection from faked device caps (vk_device_profile.h selectRt / formatRtLine). Header-only, no Vulkan.
#include "doctest.h"

#include <string>

#include "vk_device_profile.h"

namespace vkp = mc2vk::profile;

namespace {

vkp::DeviceCaps baseCaps() {
    vkp::DeviceCaps c;
    c.deviceApi = vkp::makeApi(1, 3);
    c.instanceApi = vkp::makeApi(1, 3);
    c.bufferDeviceAddress = true;
    c.rt.minAccelerationStructureScratchOffsetAlignment = 128;
    return c;
}

vkp::DeviceCaps withAccel(vkp::DeviceCaps c = baseCaps()) {
    c.extensions.push_back(std::string(vkp::kExtAccelerationStructure));
    c.extensions.push_back(std::string(vkp::kExtDeferredHostOperations));
    c.rt.accelerationStructure = true;
    return c;
}

vkp::DeviceCaps withQuery(vkp::DeviceCaps c = withAccel()) {
    c.extensions.push_back(std::string(vkp::kExtRayQuery));
    c.rt.rayQuery = true;
    return c;
}

vkp::DeviceCaps withPipeline(vkp::DeviceCaps c = withAccel()) {
    c.extensions.push_back(std::string(vkp::kExtRayTracingPipeline));
    c.rt.rayTracingPipeline = true;
    c.rt.shaderGroupHandleSize = 32;
    c.rt.shaderGroupBaseAlignment = 64;
    c.rt.shaderGroupHandleAlignment = 32;
    return c;
}

bool has(const vkp::RtSelection& s, std::string_view ext) {
    for (const std::string& e : s.extensions)
        if (e == ext) return true;
    return false;
}

}  // namespace

TEST_CASE("rt tier: a device with no RT extensions is tier none and enables nothing") {
    const vkp::RtSelection s = vkp::selectRt(baseCaps());
    CHECK(s.tier == vkp::RtTier::None);
    CHECK_FALSE(s.accel);
    CHECK_FALSE(s.rayQuery);
    CHECK_FALSE(s.rayPipeline);
    CHECK(s.extensions.empty());
    CHECK(s.reason == "no_ext_acceleration_structure");
}

TEST_CASE("rt tier: acceleration structures alone are tier accel") {
    const vkp::RtSelection s = vkp::selectRt(withAccel());
    CHECK(s.tier == vkp::RtTier::Accel);
    CHECK(s.accel);
    CHECK_FALSE(s.rayQuery);
    CHECK_FALSE(s.rayPipeline);
    CHECK(has(s, vkp::kExtAccelerationStructure));
    CHECK(has(s, vkp::kExtDeferredHostOperations));
    CHECK_FALSE(has(s, vkp::kExtRayQuery));
    CHECK(s.reason.find("no_ext_ray_query") != std::string::npos);
    CHECK(s.reason.find("no_ext_ray_tracing_pipeline") != std::string::npos);
}

TEST_CASE("rt tier: ray query and the RT pipeline are judged independently") {
    const vkp::RtSelection q = vkp::selectRt(withQuery());
    CHECK(q.tier == vkp::RtTier::Query);
    CHECK(q.rayQuery);
    CHECK_FALSE(q.rayPipeline);
    CHECK(has(q, vkp::kExtRayQuery));

    const vkp::RtSelection p = vkp::selectRt(withPipeline());
    CHECK(p.tier == vkp::RtTier::Pipeline);
    CHECK(p.rayPipeline);
    CHECK_FALSE(p.rayQuery);
    CHECK(has(p, vkp::kExtRayTracingPipeline));

    const vkp::RtSelection both = vkp::selectRt(withPipeline(withQuery()));
    CHECK(both.tier == vkp::RtTier::QueryPipeline);
    CHECK(both.reason == "-");
    CHECK(both.extensions.size() == 4);
}

TEST_CASE("rt tier: every missing prerequisite falls back with its own reason") {
    {   // extension listed, feature bit off
        vkp::DeviceCaps c = withAccel();
        c.rt.accelerationStructure = false;
        const vkp::RtSelection s = vkp::selectRt(c);
        CHECK(s.tier == vkp::RtTier::None);
        CHECK(s.reason == "no_feature_acceleration_structure");
    }
    {   // missing deferred host operations
        vkp::DeviceCaps c = baseCaps();
        c.extensions.push_back(std::string(vkp::kExtAccelerationStructure));
        c.rt.accelerationStructure = true;
        CHECK(vkp::selectRt(c).reason == "no_ext_deferred_host_operations");
    }
    {   // no buffer device address
        vkp::DeviceCaps c = withAccel();
        c.bufferDeviceAddress = false;
        CHECK(vkp::selectRt(c).reason == "no_buffer_device_address");
    }
    {   // a zero scratch alignment cannot place a scratch buffer
        vkp::DeviceCaps c = withAccel();
        c.rt.minAccelerationStructureScratchOffsetAlignment = 0;
        CHECK(vkp::selectRt(c).reason == "no_scratch_alignment");
    }
    {   // below the engine's API floor
        vkp::DeviceCaps c = withAccel();
        c.deviceApi = vkp::makeApi(1, 0);
        const vkp::RtSelection s = vkp::selectRt(c);
        CHECK(s.tier == vkp::RtTier::None);
        CHECK(s.reason == "api_below_floor");
    }
    {   // pipeline extension without SBT limits
        vkp::DeviceCaps c = withPipeline();
        c.rt.shaderGroupHandleSize = 0;
        const vkp::RtSelection s = vkp::selectRt(c);
        CHECK_FALSE(s.rayPipeline);
        CHECK(s.reason == "no_sbt_limits");
    }
    {   // ray query feature bit off while the extension is listed
        vkp::DeviceCaps c = withQuery();
        c.rt.rayQuery = false;
        const vkp::RtSelection s = vkp::selectRt(c);
        CHECK(s.tier == vkp::RtTier::Accel);
        CHECK(s.reason.find("no_feature_ray_query") != std::string::npos);
    }
}

TEST_CASE("rt tier: selection never touches the raster profile verdict") {
    const vkp::DeviceCaps plain = baseCaps();
    vkp::DeviceCaps rt = withPipeline(withQuery());
    // The RT block is extra data; the same table runs over the same raster fields.
    const vkp::Evaluation a = vkp::evaluate(plain), b = vkp::evaluate(rt);
    CHECK(a.missingRequired == b.missingRequired);
}

TEST_CASE("rt line: one [VK_RT v1] line with the tier, the reason and the limits") {
    vkp::DeviceCaps c = withPipeline(withQuery());
    c.rt.maxInstanceCount = 16777215;
    c.rt.maxPrimitiveCount = 536870911;
    c.rt.maxGeometryCount = 16777215;
    c.rt.maxPerStageDescriptorAccelerationStructures = 500000;
    c.rt.maxRayRecursionDepth = 31;
    c.rt.maxShaderGroupStride = 4096;
    const std::string line = vkp::formatRtLine(c, vkp::selectRt(c));
    CHECK(line.rfind("[VK_RT v1] tier=query+pipeline reason=- accel=1 ray_query=1 ray_pipeline=1 ", 0) == 0);
    CHECK(line.find("max_instances=16777215") != std::string::npos);
    CHECK(line.find("scratch_align=128") != std::string::npos);
    CHECK(line.find("sbt_handle=32 sbt_base_align=64 sbt_handle_align=32 sbt_stride=4096 max_recursion=31") != std::string::npos);
    CHECK(line.find('\n') == std::string::npos);

    const std::string none = vkp::formatRtLine(baseCaps(), vkp::selectRt(baseCaps()));
    CHECK(none.rfind("[VK_RT v1] tier=none reason=no_ext_acceleration_structure accel=0 ray_query=0 ray_pipeline=0 ", 0) == 0);
}
