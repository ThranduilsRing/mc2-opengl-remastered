// GameOS/gameos/vk_device_profile.cpp -- reads the capabilities of a real physical device into profile::DeviceCaps (see vk_device_profile.h for the table
// and the verdict). Inert unless MC2_VULKAN is defined.
#ifdef MC2_VULKAN

#include "vk_device_profile.h"

#include "vk_caps_json.h"

#include <volk.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>

namespace mc2vk::profile {

DeviceCaps gather(VkPhysicalDevice pd, uint32_t instanceApi) {
    DeviceCaps c;
    c.instanceApi = instanceApi;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    c.deviceApi = props.apiVersion;
    c.maxImageDimension2D = props.limits.maxImageDimension2D;
    c.maxBoundDescriptorSets = props.limits.maxBoundDescriptorSets;
    c.maxPushConstantsSize = props.limits.maxPushConstantsSize;
    c.maxStorageBufferRange = props.limits.maxStorageBufferRange;
    c.maxComputeWorkGroupInvocations = props.limits.maxComputeWorkGroupInvocations;

    // A core 1.0 feature: read before the version early-out below so a pre-1.2 device is reported with the bit it really has.
    VkPhysicalDeviceFeatures core10{};
    vkGetPhysicalDeviceFeatures(pd, &core10);
    c.independentBlend = core10.independentBlend == VK_TRUE;
    c.samplerAnisotropy = core10.samplerAnisotropy == VK_TRUE;
    c.pipelineStatisticsQuery = core10.pipelineStatisticsQuery == VK_TRUE;
    c.shaderSampledImageArrayDynamicIndexing = core10.shaderSampledImageArrayDynamicIndexing == VK_TRUE;
    c.multiDrawIndirect = core10.multiDrawIndirect == VK_TRUE;
    c.drawIndirectFirstInstance = core10.drawIndirectFirstInstance == VK_TRUE;

    if (c.usableApi() >= makeApi(1, 1)) {             // physical-device properties2 is core from 1.1
        VkPhysicalDeviceIDProperties id{};
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceSubgroupProperties sg{};
        sg.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
        id.pNext = &sg;
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};   // core from 1.2
        if (c.usableApi() >= kFloorApi) sg.pNext = &drv;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &id;
        vkGetPhysicalDeviceProperties2(pd, &props2);
        if (c.usableApi() >= kFloorApi) c.driverId = static_cast<uint32_t>(drv.driverID);
        c.subgroupSize = sg.subgroupSize;
        c.subgroupStages = sg.supportedStages;
        c.subgroupOps = sg.supportedOperations;
        c.deviceLuidValid = id.deviceLUIDValid == VK_TRUE;
        for (size_t i = 0; i < sizeof(c.deviceLuid); ++i) c.deviceLuid[i] = id.deviceLUID[i];
        for (size_t i = 0; i < sizeof(c.deviceUuid); ++i) c.deviceUuid[i] = id.deviceUUID[i];
    }

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> have(extCount);
    if (extCount) vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, have.data());
    for (const VkExtensionProperties& e : have) c.extensions.emplace_back(e.extensionName);

    {   // the depth formats the engine hardcodes must be attachable and sampleable (optimal tiling)
        constexpr VkFormatFeatureFlags kNeed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_D32_SFLOAT, &fp);
        c.depthD32 = (fp.optimalTilingFeatures & kNeed) == kNeed;
        vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
        c.depthD32S8 = (fp.optimalTilingFeatures & kNeed) == kNeed;
    }

    // The feature structs are only valid to chain at the versions / with the extensions that define them.
    if (c.usableApi() < kFloorApi) return c;

    VkPhysicalDeviceVulkan11Features f11{};
    f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr{};
    dr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
    VkPhysicalDeviceSynchronization2FeaturesKHR s2{};
    s2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;

    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    void** tail = &f2.pNext;
    auto chain = [&](auto& s) { *tail = &s; tail = &s.pNext; };
    chain(f11);
    chain(f12);
    const bool core13 = c.usableApi() >= kUseApi13;
    if (core13) {
        chain(f13);
    } else {
        if (c.hasExtension(kExtDynamicRendering)) chain(dr);
        if (c.hasExtension(kExtSynchronization2)) chain(s2);
    }
    // Ray tracing: chained only when the extension is listed (the structs are invalid to chain otherwise).
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asF{};
    asF.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    VkPhysicalDeviceRayQueryFeaturesKHR rqF{};
    rqF.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rpF{};
    rpF.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    const bool hasAs = c.hasExtension(kExtAccelerationStructure), hasRq = c.hasExtension(kExtRayQuery), hasRp = c.hasExtension(kExtRayTracingPipeline);
    if (hasAs) chain(asF);
    if (hasRq) chain(rqF);
    if (hasRp) chain(rpF);
    // Optional extension tier (selectExt): each struct is chained only when its extension is listed.
    VkPhysicalDeviceMeshShaderFeaturesEXT meshF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsrF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    VkPhysicalDeviceShaderQuadControlFeaturesKHR quadF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR};
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR mrF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR};
    VkPhysicalDeviceHostImageCopyFeatures hicF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES};
    VkPhysicalDevicePipelineBinaryFeaturesKHR pbF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgcF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT};
    VkPhysicalDeviceDescriptorHeapFeaturesEXT heapF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR csdF{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    VkPhysicalDeviceMaintenance5Features m5F{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES};
    const bool hasMesh = c.hasExtension(kExtMeshShader), hasFsr = c.hasExtension(kExtFragmentShadingRate), hasQuad = c.hasExtension(kExtShaderQuadControl),
               hasMr = c.hasExtension(kExtShaderMaximalReconvergence), hasHic = c.hasExtension(kExtHostImageCopy), hasPb = c.hasExtension(kExtPipelineBinary),
               hasGpl = c.hasExtension(kExtGraphicsPipelineLibrary), hasDgc = c.hasExtension(kExtDeviceGeneratedCommands),
               hasHeap = c.hasExtension(kExtDescriptorHeap), hasCsd = c.hasExtension(kExtComputeShaderDerivatives), hasM5 = c.hasExtension(kExtMaintenance5);
    if (hasMesh) chain(meshF);
    if (hasFsr) chain(fsrF);
    if (hasQuad) chain(quadF);
    if (hasMr) chain(mrF);
    if (hasHic) chain(hicF);
    if (hasPb) chain(pbF);
    if (hasGpl) chain(gplF);
    if (hasDgc) chain(dgcF);
    if (hasHeap) chain(heapF);
    if (hasCsd) chain(csdF);
    if (hasM5) chain(m5F);
    vkGetPhysicalDeviceFeatures2(pd, &f2);

    {
        ExtCaps& x = c.ext;
        x.taskShader = hasMesh && meshF.taskShader == VK_TRUE;
        x.meshShader = hasMesh && meshF.meshShader == VK_TRUE;
        x.fsrPipeline = hasFsr && fsrF.pipelineFragmentShadingRate == VK_TRUE;
        x.fsrPrimitive = hasFsr && fsrF.primitiveFragmentShadingRate == VK_TRUE;
        x.fsrAttachment = hasFsr && fsrF.attachmentFragmentShadingRate == VK_TRUE;
        x.shaderQuadControl = hasQuad && quadF.shaderQuadControl == VK_TRUE;
        x.shaderMaximalReconvergence = hasMr && mrF.shaderMaximalReconvergence == VK_TRUE;
        x.hostImageCopy = hasHic && hicF.hostImageCopy == VK_TRUE;
        x.pipelineBinaries = hasPb && pbF.pipelineBinaries == VK_TRUE;
        x.graphicsPipelineLibrary = hasGpl && gplF.graphicsPipelineLibrary == VK_TRUE;
        x.deviceGeneratedCommands = hasDgc && dgcF.deviceGeneratedCommands == VK_TRUE;
        x.descriptorHeap = hasHeap && heapF.descriptorHeap == VK_TRUE;
        x.computeDerivativeGroupQuads = hasCsd && csdF.computeDerivativeGroupQuads == VK_TRUE;
        x.computeDerivativeGroupLinear = hasCsd && csdF.computeDerivativeGroupLinear == VK_TRUE;
        x.maintenance5 = hasM5 && m5F.maintenance5 == VK_TRUE;
        VkPhysicalDeviceMeshShaderPropertiesEXT meshP{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
        VkPhysicalDeviceFragmentShadingRatePropertiesKHR fsrP{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
        VkPhysicalDeviceHostImageCopyProperties hicP{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES};
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplP{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT dgcP{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_PROPERTIES_EXT};
        VkPhysicalDeviceDescriptorHeapPropertiesEXT heapP{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        void** ptail = &p2.pNext;
        auto pchain = [&](auto& s) { *ptail = &s; ptail = &s.pNext; };
        if (hasMesh) pchain(meshP);
        if (hasFsr) pchain(fsrP);
        if (hasHic) pchain(hicP);
        if (hasGpl) pchain(gplP);
        if (hasDgc) pchain(dgcP);
        if (hasHeap) pchain(heapP);
        if (p2.pNext) vkGetPhysicalDeviceProperties2(pd, &p2);
        if (hasMesh) {
            x.maxMeshOutputVertices = meshP.maxMeshOutputVertices;
            x.maxMeshOutputPrimitives = meshP.maxMeshOutputPrimitives;
            x.maxPreferredMeshWorkGroupInvocations = meshP.maxPreferredMeshWorkGroupInvocations;
            x.maxTaskWorkGroupTotalCount = meshP.maxTaskWorkGroupTotalCount;
        }
        if (hasFsr) {
            x.fsrMinTexelW = fsrP.minFragmentShadingRateAttachmentTexelSize.width;
            x.fsrMinTexelH = fsrP.minFragmentShadingRateAttachmentTexelSize.height;
            x.fsrMaxTexelW = fsrP.maxFragmentShadingRateAttachmentTexelSize.width;
            x.fsrMaxTexelH = fsrP.maxFragmentShadingRateAttachmentTexelSize.height;
        }
        if (hasHic) x.identicalMemoryTypeRequirements = hicP.identicalMemoryTypeRequirements == VK_TRUE;
        if (hasGpl) x.graphicsPipelineLibraryFastLinking = gplP.graphicsPipelineLibraryFastLinking == VK_TRUE;
        if (hasDgc) x.maxIndirectSequenceCount = dgcP.maxIndirectSequenceCount;
        if (hasHeap) {
            x.maxSamplerHeapSize = heapP.maxSamplerHeapSize;
            x.maxResourceHeapSize = heapP.maxResourceHeapSize;
            x.samplerDescriptorSize = heapP.samplerDescriptorSize;
            x.imageDescriptorSize = heapP.imageDescriptorSize;
            x.bufferDescriptorSize = heapP.bufferDescriptorSize;
        }
    }

    if (hasAs) {
        c.rt.accelerationStructure = asF.accelerationStructure == VK_TRUE;
        c.rt.rayQuery = hasRq && rqF.rayQuery == VK_TRUE;
        c.rt.rayTracingPipeline = hasRp && rpF.rayTracingPipeline == VK_TRUE;
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asP{};
        asP.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rpP{};
        rpP.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        if (hasRp) asP.pNext = &rpP;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &asP;
        vkGetPhysicalDeviceProperties2(pd, &p2);
        c.rt.maxGeometryCount = asP.maxGeometryCount;
        c.rt.maxInstanceCount = asP.maxInstanceCount;
        c.rt.maxPrimitiveCount = asP.maxPrimitiveCount;
        c.rt.maxPerStageDescriptorAccelerationStructures = asP.maxPerStageDescriptorAccelerationStructures;
        c.rt.maxDescriptorSetAccelerationStructures = asP.maxDescriptorSetAccelerationStructures;
        c.rt.minAccelerationStructureScratchOffsetAlignment = asP.minAccelerationStructureScratchOffsetAlignment;
        if (hasRp) {
            c.rt.shaderGroupHandleSize = rpP.shaderGroupHandleSize;
            c.rt.maxRayRecursionDepth = rpP.maxRayRecursionDepth;
            c.rt.maxShaderGroupStride = rpP.maxShaderGroupStride;
            c.rt.shaderGroupBaseAlignment = rpP.shaderGroupBaseAlignment;
            c.rt.shaderGroupHandleAlignment = rpP.shaderGroupHandleAlignment;
        }
    }

    c.drawIndirectCount = f12.drawIndirectCount == VK_TRUE;
    c.shaderDrawParameters = f11.shaderDrawParameters == VK_TRUE;
    c.timelineSemaphore = f12.timelineSemaphore == VK_TRUE;
    c.bufferDeviceAddress = f12.bufferDeviceAddress == VK_TRUE;
    c.descriptorIndexing = f12.descriptorIndexing == VK_TRUE;
    c.runtimeDescriptorArray = f12.runtimeDescriptorArray == VK_TRUE;
    c.shaderSampledImageArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
    c.descriptorBindingPartiallyBound = f12.descriptorBindingPartiallyBound == VK_TRUE;
    c.dynamicRendering = (core13 ? f13.dynamicRendering : dr.dynamicRendering) == VK_TRUE;
    c.synchronization2 = (core13 ? f13.synchronization2 : s2.synchronization2) == VK_TRUE;
    return c;
}

// ---- per-GPU capability report (JSON) ----

namespace {

struct FeatureFamily { const char* ext; uint32_t promotedApi; };

// Queries one feature struct on its own (so one unsupported family cannot poison the rest) and writes {"queried":..., fields...}. Skipped, with
// "queried":false, when neither the extension nor the core version that promoted it is present.
template <class S>
void featureFamily(JsonWriter& j, const DeviceCaps& c, VkPhysicalDevice pd, std::string_view family, FeatureFamily f, VkStructureType sType,
                   std::initializer_list<std::pair<const char*, VkBool32 S::*>> fields) {
    j.beginObject(family);
    const bool avail = (f.ext && c.hasExtension(f.ext)) || (f.promotedApi && c.deviceApi >= f.promotedApi);
    j.boolean("queried", avail);
    if (avail) {
        S s{};
        s.sType = sType;
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &s;
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        for (const auto& [name, member] : fields) j.boolean(name, s.*member == VK_TRUE);
    }
    j.endObject();
}

}  // namespace

bool writeCapsReport(VkPhysicalDevice pd, uint32_t instanceApi, std::string_view path) {
    const DeviceCaps c = gather(pd, instanceApi);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    const VkPhysicalDeviceLimits& L = props.limits;

    JsonWriter j;
    j.beginObject();
    j.num("schema", 1);
    j.beginObject("device")
        .str("name", props.deviceName)
        .num("vendor_id", props.vendorID)
        .num("device_id", props.deviceID)
        .num("device_type", static_cast<uint64_t>(props.deviceType))
        .str("api_version", versionText(props.apiVersion))
        .str("instance_api", versionText(instanceApi))
        .num("driver_version_raw", props.driverVersion);
    if (c.usableApi() >= makeApi(1, 2)) {
        VkPhysicalDeviceDriverProperties drv{};
        drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &drv;
        vkGetPhysicalDeviceProperties2(pd, &p2);
        j.num("driver_id", static_cast<uint64_t>(drv.driverID)).str("driver_name", drv.driverName).str("driver_info", drv.driverInfo)
            .str("conformance", std::to_string(drv.conformanceVersion.major) + "." + std::to_string(drv.conformanceVersion.minor) + "." +
                                    std::to_string(drv.conformanceVersion.subminor) + "." + std::to_string(drv.conformanceVersion.patch));
    }
    j.endObject();

    std::vector<std::string> exts = c.extensions;
    std::sort(exts.begin(), exts.end());
    j.beginArray("extensions");
    for (const std::string& e : exts) j.str({}, e);
    j.endArray();

    // The disputed / roadmap extensions by name, so a family with no struct in the pinned headers (cooperative_matrix_maintenance1, image_tiling_control,
    // opacity micromap KHR) is still answered by the driver's extension list.
    static const char* const kTracked[] = {
        "VK_KHR_ray_query", "VK_KHR_acceleration_structure", "VK_KHR_ray_tracing_pipeline", "VK_KHR_ray_tracing_position_fetch", "VK_KHR_deferred_host_operations",
        "VK_KHR_ray_tracing_maintenance1", "VK_EXT_opacity_micromap", "VK_KHR_opacity_micromap", "VK_NV_ray_tracing_invocation_reorder", "VK_EXT_ray_tracing_invocation_reorder",
        "VK_KHR_cooperative_matrix", "VK_NV_cooperative_matrix", "VK_NV_cooperative_matrix2", "VK_EXT_cooperative_matrix_maintenance1", "VK_KHR_cooperative_matrix_maintenance1",
        "VK_KHR_maintenance5", "VK_KHR_maintenance6", "VK_KHR_maintenance7", "VK_KHR_maintenance8", "VK_KHR_maintenance9", "VK_KHR_maintenance10",
        "VK_EXT_host_image_copy", "VK_EXT_image_tiling_control", "VK_KHR_image_tiling_control", "VK_EXT_present_timing", "VK_KHR_present_id2", "VK_KHR_present_wait2",
        "VK_EXT_device_fault", "VK_KHR_device_fault", "VK_EXT_descriptor_heap", "VK_EXT_device_generated_commands", "VK_EXT_mesh_shader",
        "VK_KHR_compute_shader_derivatives", "VK_NV_compute_shader_derivatives", "VK_KHR_fragment_shading_rate", "VK_KHR_shader_quad_control",
        "VK_KHR_shader_maximal_reconvergence", "VK_KHR_pipeline_binary", "VK_EXT_graphics_pipeline_library", "VK_KHR_pipeline_library"};
    j.beginObject("tracked_extensions");
    for (const char* e : kTracked) j.boolean(e, c.hasExtension(e));
    j.endObject();

    using F = FeatureFamily;
    j.beginObject("features");
    featureFamily<VkPhysicalDeviceRayQueryFeaturesKHR>(j, c, pd, "ray_query", F{"VK_KHR_ray_query", 0}, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        {{"rayQuery", &VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery}});
    featureFamily<VkPhysicalDeviceAccelerationStructureFeaturesKHR>(j, c, pd, "acceleration_structure", F{"VK_KHR_acceleration_structure", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        {{"accelerationStructure", &VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructure},
         {"accelerationStructureCaptureReplay", &VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructureCaptureReplay},
         {"accelerationStructureIndirectBuild", &VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructureIndirectBuild},
         {"accelerationStructureHostCommands", &VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructureHostCommands},
         {"descriptorBindingAccelerationStructureUpdateAfterBind", &VkPhysicalDeviceAccelerationStructureFeaturesKHR::descriptorBindingAccelerationStructureUpdateAfterBind}});
    featureFamily<VkPhysicalDeviceRayTracingPipelineFeaturesKHR>(j, c, pd, "ray_tracing_pipeline", F{"VK_KHR_ray_tracing_pipeline", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        {{"rayTracingPipeline", &VkPhysicalDeviceRayTracingPipelineFeaturesKHR::rayTracingPipeline},
         {"rayTracingPipelineTraceRaysIndirect", &VkPhysicalDeviceRayTracingPipelineFeaturesKHR::rayTracingPipelineTraceRaysIndirect},
         {"rayTraversalPrimitiveCulling", &VkPhysicalDeviceRayTracingPipelineFeaturesKHR::rayTraversalPrimitiveCulling}});
    featureFamily<VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR>(j, c, pd, "ray_tracing_position_fetch", F{"VK_KHR_ray_tracing_position_fetch", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR,
        {{"rayTracingPositionFetch", &VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR::rayTracingPositionFetch}});
    featureFamily<VkPhysicalDeviceOpacityMicromapFeaturesEXT>(j, c, pd, "opacity_micromap_ext", F{"VK_EXT_opacity_micromap", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT,
        {{"micromap", &VkPhysicalDeviceOpacityMicromapFeaturesEXT::micromap},
         {"micromapCaptureReplay", &VkPhysicalDeviceOpacityMicromapFeaturesEXT::micromapCaptureReplay},
         {"micromapHostCommands", &VkPhysicalDeviceOpacityMicromapFeaturesEXT::micromapHostCommands}});
    featureFamily<VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT>(j, c, pd, "invocation_reorder_ext", F{"VK_EXT_ray_tracing_invocation_reorder", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT,
        {{"rayTracingInvocationReorder", &VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT::rayTracingInvocationReorder}});
    featureFamily<VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV>(j, c, pd, "invocation_reorder_nv", F{"VK_NV_ray_tracing_invocation_reorder", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV,
        {{"rayTracingInvocationReorder", &VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV::rayTracingInvocationReorder}});
    featureFamily<VkPhysicalDeviceCooperativeMatrixFeaturesKHR>(j, c, pd, "cooperative_matrix_khr", F{"VK_KHR_cooperative_matrix", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,
        {{"cooperativeMatrix", &VkPhysicalDeviceCooperativeMatrixFeaturesKHR::cooperativeMatrix},
         {"cooperativeMatrixRobustBufferAccess", &VkPhysicalDeviceCooperativeMatrixFeaturesKHR::cooperativeMatrixRobustBufferAccess}});
    featureFamily<VkPhysicalDeviceMaintenance5Features>(j, c, pd, "maintenance5", F{"VK_KHR_maintenance5", makeApi(1, 4)},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES, {{"maintenance5", &VkPhysicalDeviceMaintenance5Features::maintenance5}});
    featureFamily<VkPhysicalDeviceMaintenance6Features>(j, c, pd, "maintenance6", F{"VK_KHR_maintenance6", makeApi(1, 4)},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES, {{"maintenance6", &VkPhysicalDeviceMaintenance6Features::maintenance6}});
    featureFamily<VkPhysicalDeviceMaintenance7FeaturesKHR>(j, c, pd, "maintenance7", F{"VK_KHR_maintenance7", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR, {{"maintenance7", &VkPhysicalDeviceMaintenance7FeaturesKHR::maintenance7}});
    featureFamily<VkPhysicalDeviceMaintenance8FeaturesKHR>(j, c, pd, "maintenance8", F{"VK_KHR_maintenance8", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR, {{"maintenance8", &VkPhysicalDeviceMaintenance8FeaturesKHR::maintenance8}});
    featureFamily<VkPhysicalDeviceMaintenance9FeaturesKHR>(j, c, pd, "maintenance9", F{"VK_KHR_maintenance9", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_9_FEATURES_KHR, {{"maintenance9", &VkPhysicalDeviceMaintenance9FeaturesKHR::maintenance9}});
    featureFamily<VkPhysicalDeviceHostImageCopyFeatures>(j, c, pd, "host_image_copy", F{"VK_EXT_host_image_copy", makeApi(1, 4)},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES, {{"hostImageCopy", &VkPhysicalDeviceHostImageCopyFeatures::hostImageCopy}});
    featureFamily<VkPhysicalDevicePresentTimingFeaturesEXT>(j, c, pd, "present_timing", F{"VK_EXT_present_timing", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
        {{"presentTiming", &VkPhysicalDevicePresentTimingFeaturesEXT::presentTiming},
         {"presentAtAbsoluteTime", &VkPhysicalDevicePresentTimingFeaturesEXT::presentAtAbsoluteTime},
         {"presentAtRelativeTime", &VkPhysicalDevicePresentTimingFeaturesEXT::presentAtRelativeTime}});
    featureFamily<VkPhysicalDevicePresentId2FeaturesKHR>(j, c, pd, "present_id2", F{"VK_KHR_present_id2", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, {{"presentId2", &VkPhysicalDevicePresentId2FeaturesKHR::presentId2}});
    featureFamily<VkPhysicalDevicePresentWait2FeaturesKHR>(j, c, pd, "present_wait2", F{"VK_KHR_present_wait2", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR, {{"presentWait2", &VkPhysicalDevicePresentWait2FeaturesKHR::presentWait2}});
    featureFamily<VkPhysicalDeviceFaultFeaturesEXT>(j, c, pd, "device_fault_ext", F{"VK_EXT_device_fault", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
        {{"deviceFault", &VkPhysicalDeviceFaultFeaturesEXT::deviceFault}, {"deviceFaultVendorBinary", &VkPhysicalDeviceFaultFeaturesEXT::deviceFaultVendorBinary}});
    featureFamily<VkPhysicalDeviceFaultFeaturesKHR>(j, c, pd, "device_fault_khr", F{"VK_KHR_device_fault", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_KHR,
        {{"deviceFault", &VkPhysicalDeviceFaultFeaturesKHR::deviceFault}, {"deviceFaultVendorBinary", &VkPhysicalDeviceFaultFeaturesKHR::deviceFaultVendorBinary}});
    featureFamily<VkPhysicalDeviceDescriptorHeapFeaturesEXT>(j, c, pd, "descriptor_heap", F{"VK_EXT_descriptor_heap", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
        {{"descriptorHeap", &VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap},
         {"descriptorHeapCaptureReplay", &VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeapCaptureReplay}});
    featureFamily<VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT>(j, c, pd, "device_generated_commands_ext", F{"VK_EXT_device_generated_commands", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT,
        {{"deviceGeneratedCommands", &VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT::deviceGeneratedCommands},
         {"dynamicGeneratedPipelineLayout", &VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT::dynamicGeneratedPipelineLayout}});
    featureFamily<VkPhysicalDeviceMeshShaderFeaturesEXT>(j, c, pd, "mesh_shader_ext", F{"VK_EXT_mesh_shader", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
        {{"taskShader", &VkPhysicalDeviceMeshShaderFeaturesEXT::taskShader}, {"meshShader", &VkPhysicalDeviceMeshShaderFeaturesEXT::meshShader},
         {"multiviewMeshShader", &VkPhysicalDeviceMeshShaderFeaturesEXT::multiviewMeshShader},
         {"primitiveFragmentShadingRateMeshShader", &VkPhysicalDeviceMeshShaderFeaturesEXT::primitiveFragmentShadingRateMeshShader},
         {"meshShaderQueries", &VkPhysicalDeviceMeshShaderFeaturesEXT::meshShaderQueries}});
    featureFamily<VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR>(j, c, pd, "compute_shader_derivatives_khr", F{"VK_KHR_compute_shader_derivatives", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR,
        {{"computeDerivativeGroupQuads", &VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR::computeDerivativeGroupQuads},
         {"computeDerivativeGroupLinear", &VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR::computeDerivativeGroupLinear}});
    featureFamily<VkPhysicalDeviceFragmentShadingRateFeaturesKHR>(j, c, pd, "fragment_shading_rate", F{"VK_KHR_fragment_shading_rate", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
        {{"pipelineFragmentShadingRate", &VkPhysicalDeviceFragmentShadingRateFeaturesKHR::pipelineFragmentShadingRate},
         {"primitiveFragmentShadingRate", &VkPhysicalDeviceFragmentShadingRateFeaturesKHR::primitiveFragmentShadingRate},
         {"attachmentFragmentShadingRate", &VkPhysicalDeviceFragmentShadingRateFeaturesKHR::attachmentFragmentShadingRate}});
    featureFamily<VkPhysicalDeviceShaderQuadControlFeaturesKHR>(j, c, pd, "shader_quad_control", F{"VK_KHR_shader_quad_control", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR, {{"shaderQuadControl", &VkPhysicalDeviceShaderQuadControlFeaturesKHR::shaderQuadControl}});
    featureFamily<VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR>(j, c, pd, "shader_maximal_reconvergence", F{"VK_KHR_shader_maximal_reconvergence", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR,
        {{"shaderMaximalReconvergence", &VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR::shaderMaximalReconvergence}});
    featureFamily<VkPhysicalDevicePipelineBinaryFeaturesKHR>(j, c, pd, "pipeline_binary", F{"VK_KHR_pipeline_binary", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, {{"pipelineBinaries", &VkPhysicalDevicePipelineBinaryFeaturesKHR::pipelineBinaries}});
    featureFamily<VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT>(j, c, pd, "graphics_pipeline_library", F{"VK_EXT_graphics_pipeline_library", 0},
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
        {{"graphicsPipelineLibrary", &VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT::graphicsPipelineLibrary}});
    j.endObject();

    j.beginArray("cooperative_matrix_shapes");
    if (c.hasExtension("VK_KHR_cooperative_matrix") && vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR) {
        uint32_t n = 0;
        vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(pd, &n, nullptr);
        std::vector<VkCooperativeMatrixPropertiesKHR> shapes(n);
        for (auto& s : shapes) s.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
        if (n) vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(pd, &n, shapes.data());
        for (uint32_t i = 0; i < n && i < shapes.size(); ++i) {
            const auto& s = shapes[i];
            j.beginObject().num("M", s.MSize).num("N", s.NSize).num("K", s.KSize).num("A", s.AType).num("B", s.BType).num("C", s.CType).num("Result", s.ResultType)
                .boolean("saturating", s.saturatingAccumulation == VK_TRUE).num("scope", s.scope).endObject();
        }
    }
    j.endArray();

    j.beginObject("limits")
        .num("maxImageDimension2D", L.maxImageDimension2D).num("maxBoundDescriptorSets", L.maxBoundDescriptorSets)
        .num("maxPushConstantsSize", L.maxPushConstantsSize).num("maxStorageBufferRange", L.maxStorageBufferRange)
        .num("maxUniformBufferRange", L.maxUniformBufferRange).num("maxComputeSharedMemorySize", L.maxComputeSharedMemorySize)
        .num("maxComputeWorkGroupInvocations", L.maxComputeWorkGroupInvocations).num("maxMemoryAllocationCount", L.maxMemoryAllocationCount)
        .num("maxPerStageDescriptorSampledImages", L.maxPerStageDescriptorSampledImages).num("maxPerStageResources", L.maxPerStageResources)
        .num("maxColorAttachments", L.maxColorAttachments).num("maxDrawIndirectCount", L.maxDrawIndirectCount)
        .num("subgroupSize", c.subgroupSize).num("subgroupSupportedStages", c.subgroupStages).num("subgroupSupportedOperations", c.subgroupOps)
        .num("meshMaxOutputVertices", c.ext.maxMeshOutputVertices).num("meshMaxOutputPrimitives", c.ext.maxMeshOutputPrimitives)
        .num("meshMaxPreferredWorkGroupInvocations", c.ext.maxPreferredMeshWorkGroupInvocations).num("taskMaxWorkGroupTotalCount", c.ext.maxTaskWorkGroupTotalCount)
        .num("fsrMinAttachmentTexelWidth", c.ext.fsrMinTexelW).num("fsrMinAttachmentTexelHeight", c.ext.fsrMinTexelH)
        .num("fsrMaxAttachmentTexelWidth", c.ext.fsrMaxTexelW).num("fsrMaxAttachmentTexelHeight", c.ext.fsrMaxTexelH)
        .endObject();

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    if (qn) vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
    j.beginArray("queue_families");
    for (uint32_t i = 0; i < qn && i < qs.size(); ++i)
        j.beginObject().num("index", i).num("flags", qs[i].queueFlags).num("count", qs[i].queueCount).num("timestamp_valid_bits", qs[i].timestampValidBits).endObject();
    j.endArray();
    j.endObject();

    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << j.text() << '\n';
    return static_cast<bool>(out);
}

}  // namespace mc2vk::profile

#endif  // MC2_VULKAN
