// Optional extension tier selection from faked device caps (vk_device_profile.h selectExt / formatExtLine). Header-only, no Vulkan.
#include "doctest.h"

#include <algorithm>
#include <string>
#include <vector>

#include "vk_device_profile.h"

namespace vkp = mc2vk::profile;

namespace {

void addExt(vkp::DeviceCaps& c, std::initializer_list<std::string_view> names) {
    for (std::string_view n : names) c.extensions.emplace_back(n);
}

vkp::DeviceCaps base() {
    vkp::DeviceCaps c;
    c.deviceApi = vkp::makeApi(1, 4);
    c.instanceApi = vkp::makeApi(1, 3);
    c.bufferDeviceAddress = true;
    return c;
}

vkp::DeviceCaps xtx() {
    vkp::DeviceCaps c = base();
    addExt(c, {vkp::kExtMeshShader, vkp::kExtFragmentShadingRate, vkp::kExtShaderQuadControl, vkp::kExtShaderMaximalReconvergence, vkp::kExtHostImageCopy,
               vkp::kExtPipelineBinary, vkp::kExtGraphicsPipelineLibrary, vkp::kExtPipelineLibrary, vkp::kExtDeviceGeneratedCommands, vkp::kExtDescriptorHeap,
               vkp::kExtComputeShaderDerivatives, vkp::kExtMaintenance5});
    vkp::ExtCaps& x = c.ext;
    x.taskShader = x.meshShader = true;
    x.maxMeshOutputVertices = 256;
    x.maxMeshOutputPrimitives = 256;
    x.fsrPipeline = x.fsrPrimitive = x.fsrAttachment = true;
    x.fsrMinTexelW = x.fsrMinTexelH = 8;
    x.shaderQuadControl = x.shaderMaximalReconvergence = true;
    x.hostImageCopy = x.pipelineBinaries = x.graphicsPipelineLibrary = x.deviceGeneratedCommands = x.descriptorHeap = true;
    x.computeDerivativeGroupQuads = x.computeDerivativeGroupLinear = true;
    x.maintenance5 = true;
    return c;
}

vkp::DeviceCaps gtx1050ti() {
    vkp::DeviceCaps c = base();
    addExt(c, {vkp::kExtHostImageCopy, vkp::kExtPipelineBinary, vkp::kExtGraphicsPipelineLibrary, vkp::kExtPipelineLibrary, vkp::kExtMaintenance5});
    c.ext.hostImageCopy = c.ext.pipelineBinaries = c.ext.graphicsPipelineLibrary = c.ext.maintenance5 = true;
    return c;
}

vkp::DeviceCaps rtx2070s() {
    vkp::DeviceCaps c = base();
    addExt(c, {vkp::kExtMeshShader, vkp::kExtFragmentShadingRate, vkp::kExtShaderQuadControl, vkp::kExtShaderMaximalReconvergence, vkp::kExtHostImageCopy,
               vkp::kExtGraphicsPipelineLibrary, vkp::kExtPipelineLibrary});
    vkp::ExtCaps& x = c.ext;
    x.taskShader = x.meshShader = x.fsrPipeline = x.fsrAttachment = true;
    x.shaderQuadControl = x.shaderMaximalReconvergence = x.hostImageCopy = x.graphicsPipelineLibrary = true;
    return c;
}

std::string parts(const vkp::ExtSelection& s) {
    std::string out;
    for (size_t i = 0; i < 9; ++i)
        if (s.part(i)) out += std::string(out.empty() ? "" : ",") + std::string(vkp::kExtPartNames[i]);
    return out;
}

}  // namespace

TEST_CASE("ext tier: XTX caps select every part and enable the dependency extensions") {
    const vkp::ExtSelection s = vkp::selectExt(xtx(), "");
    CHECK(parts(s) == "mesh,fsr,quad,host_copy,pipeline_binary,gpl,dgc,heap,csd");
    CHECK(s.reason == "-");
    CHECK(std::ranges::count(s.extensions, std::string(vkp::kExtMaintenance5)) == 1);
    CHECK(std::ranges::count(s.extensions, std::string(vkp::kExtShaderMaximalReconvergence)) == 1);
    CHECK(std::ranges::count(s.extensions, std::string(vkp::kExtPipelineLibrary)) == 1);
    CHECK(s.extensions.size() == 12);
    const std::string line = vkp::formatExtLine(xtx(), s, "");
    CHECK(line.rfind("[VK_EXT v1] mesh=1 fsr=1 fsr_kinds=pipe+prim+att quad=1 host_copy=1 pipeline_binary=1 gpl=1 dgc=1 heap=1 csd=1 deny=- reason=- ", 0) == 0);
    CHECK(line.find("mesh_max_verts=256 mesh_max_prims=256 fsr_texel=8x8") != std::string::npos);
}

TEST_CASE("ext tier: the AMD proprietary driver gets the eight parts but not gpl (its shader cache hangs on gpl)") {
    vkp::DeviceCaps c = xtx();
    c.driverId = vkp::kDriverIdAmdProprietary;
    const vkp::ExtSelection s = vkp::selectExt(c, "");
    CHECK(parts(s) == "mesh,fsr,quad,host_copy,pipeline_binary,dgc,heap,csd");
    CHECK(s.reason == "gpl:amd_proprietary_shader_cache");
    // A part the device lacks keeps its own reason.
    c.ext.meshShader = false;
    CHECK(vkp::selectExt(c, "").reason.find("mesh:no_feature") != std::string::npos);
}

TEST_CASE("ext tier: pipeline cache file name carries the key") {
    const uint8_t uuid[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 255};
    CHECK(vkp::pipelineCacheStem(0x1002, 0x744c, 7, uuid, 0) == "00001002-0000744c-00000007-000102030405060708090a0b0c0d0eff");   // key 0 = old stem, no suffix
    CHECK(vkp::pipelineCacheStem(0x1002, 0x744c, 7, uuid, 0xdeadbeef).ends_with("_kdeadbeef"));
    CHECK(vkp::pipelineCacheStem(0x1002, 0x744c, 7, uuid, 1) != vkp::pipelineCacheStem(0x1002, 0x744c, 7, uuid, 2));
}

TEST_CASE("ext tier: cache_key is 0 for no part, stable, and differs per part set") {
    const vkp::DeviceCaps c = xtx();
    CHECK(vkp::extCacheKey(vkp::ExtSelection{}) == 0);
    CHECK(vkp::extCacheKey(vkp::selectExt(c, "all")) == 0);
    const uint32_t all = vkp::extCacheKey(vkp::selectExt(c, ""));
    CHECK(all != 0);
    CHECK(all == vkp::extCacheKey(vkp::selectExt(c, "")));
    std::vector<uint32_t> keys{all};
    for (const std::string_view part : vkp::kExtPartNames) {
        const uint32_t k = vkp::extCacheKey(vkp::selectExt(c, part));   // every part but one
        CHECK(k != 0);
        keys.push_back(k);
    }
    std::ranges::sort(keys);
    CHECK(std::ranges::adjacent_find(keys) == keys.end());
    CHECK(vkp::formatExtLine(c, vkp::selectExt(c, "all"), "all").find("cache_key=00000000") != std::string::npos);
}

TEST_CASE("ext tier: cache_key is unique over all 512 part sets and pins a golden value") {
    std::vector<uint32_t> keys;
    for (uint32_t mask = 0; mask < 512; ++mask) {
        vkp::ExtSelection s;
        s.mesh = mask & 1; s.fsr = mask & 2; s.quad = mask & 4; s.hostCopy = mask & 8; s.pipelineBinary = mask & 16;
        s.gpl = mask & 32; s.dgc = mask & 64; s.heap = mask & 128; s.csd = mask & 256;
        const uint32_t k = vkp::extCacheKey(s);
        CHECK((k == 0) == (mask == 0));
        keys.push_back(k);
        if (mask == 0x1DF) CHECK(k == 0xaeced53du);   // every part but gpl: the AMD proprietary set
    }
    std::ranges::sort(keys);
    CHECK(std::ranges::adjacent_find(keys) == keys.end());
}

TEST_CASE("ext tier: 1050 Ti caps select host_copy, pipeline_binary and gpl only") {
    CHECK(parts(vkp::selectExt(gtx1050ti(), "")) == "host_copy,pipeline_binary,gpl");
}

TEST_CASE("ext tier: 2070 Super caps select mesh, fsr, quad, host_copy and gpl") {
    CHECK(parts(vkp::selectExt(rtx2070s(), "")) == "mesh,fsr,quad,host_copy,gpl");
}

TEST_CASE("ext tier: a part with a missing dependency is 0 while the others stay") {
    vkp::DeviceCaps c = xtx();
    c.ext.shaderMaximalReconvergence = false;
    vkp::ExtSelection s = vkp::selectExt(c, "");
    CHECK_FALSE(s.quad);
    CHECK(s.mesh);
    CHECK(s.reason == "quad:no_feature_maximal_reconvergence");
    c = xtx();
    c.ext.maintenance5 = false;
    s = vkp::selectExt(c, "");
    CHECK_FALSE(s.heap);
    CHECK_FALSE(s.dgc);
    CHECK_FALSE(s.pipelineBinary);
    CHECK(s.hostCopy);
    CHECK(std::ranges::count(s.extensions, std::string(vkp::kExtMaintenance5)) == 0);
    c = xtx();
    c.deviceApi = c.instanceApi = vkp::makeApi(1, 2);
    CHECK_FALSE(vkp::selectExt(c, "").hostCopy);   // host image copy needs 1.3
}

TEST_CASE("ext tier: pipeline_binary at instance 1.3 without maintenance5 is 0") {
    vkp::DeviceCaps c = xtx();
    c.extensions.erase(std::ranges::find(c.extensions, std::string(vkp::kExtMaintenance5)));
    const vkp::ExtSelection s = vkp::selectExt(c, "");
    CHECK_FALSE(s.pipelineBinary);
    CHECK(s.reason.find("pipeline_binary:no_maintenance5") != std::string::npos);
}

TEST_CASE("ext tier: deny all, a list and an unknown token") {
    const vkp::ExtSelection none = vkp::selectExt(xtx(), "all");
    CHECK_FALSE(none.any());
    CHECK(none.extensions.empty());
    CHECK(none.reason.find("mesh:denied") != std::string::npos);
    const vkp::ExtSelection two = vkp::selectExt(xtx(), "mesh,fsr");
    CHECK(parts(two) == "quad,host_copy,pipeline_binary,gpl,dgc,heap,csd");
    CHECK(two.reason == "mesh:denied,fsr:denied");
    const vkp::ExtSelection bad = vkp::selectExt(xtx(), "mesh, bogus");
    CHECK_FALSE(bad.mesh);
    CHECK(bad.fsr);
    CHECK(bad.reason.find("deny_unknown_bogus") != std::string::npos);
    CHECK(vkp::formatExtLine(xtx(), none, "all").find("deny=all reason=") != std::string::npos);
}
