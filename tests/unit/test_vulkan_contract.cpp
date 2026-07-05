// VULKAN-CONTRACT-COVERAGE-1 offline validation.
//
// Proves the per-resource Vulkan descriptor-class contract (vulkan_contract.h) is
// COMPLETE and KIND-CONSISTENT: every live-default-path resource row declares a real
// descriptor class (I1), and that class agrees with the registry's RenderResourceKind
// (I2 — Buffer->UBO/SSBO, Texture*->CombinedImageSampler/StorageImage/InputAttachment).
// All offline, no GL/engine/smoke. The header's static_asserts already prove this at
// compile time; these doctest cases make the coverage/consistency visible and named at
// run time, and add a deliberate-break-style negative check on the predicate.
#include "doctest.h"
#include "RenderCore/vulkan_contract.h"

using namespace RenderCore;
using namespace RenderCore::vulkan;

TEST_SUITE("VulkanContract") {

TEST_CASE("I1: every contract row declares a real descriptor class and frequency") {
    for (int i = 0; i < kVkResourceContractCount; ++i) {
        const VkResourceContract& r = kVkResourceContracts[i];
        INFO("row ", i, " id=", static_cast<unsigned>(r.id),
             " cls=", vkDescriptorClassName(r.cls),
             " freq=", vkResourceFreqName(r.freq));
        CHECK(static_cast<unsigned>(r.id)   != static_cast<unsigned>(RenderResourceId::Unknown));
        CHECK(static_cast<unsigned>(r.cls)  != static_cast<unsigned>(VkDescriptorClass::None));
        CHECK(static_cast<unsigned>(r.freq) != static_cast<unsigned>(VkResourceFreq::None));
        CHECK(static_cast<unsigned>(r.kind) != static_cast<unsigned>(RenderResourceKind::Unknown));
    }
}

TEST_CASE("I2: descriptor class is consistent with the registry RenderResourceKind") {
    for (int i = 0; i < kVkResourceContractCount; ++i) {
        const VkResourceContract& r = kVkResourceContracts[i];
        INFO("row ", i, " id=", static_cast<unsigned>(r.id),
             " kind=", toString(r.kind), " cls=", vkDescriptorClassName(r.cls));
        CHECK(vkKindConsistent(r.kind, r.cls));
        // Cross-check the two class-family predicates are mutually exclusive.
        if (r.kind == RenderResourceKind::Buffer)
            CHECK(vkClassIsBufferClass(r.cls));
        else
            CHECK(vkClassIsImageClass(r.cls));
    }
}

TEST_CASE("coverage aggregate + no duplicate ids (matches the header static_asserts)") {
    RenderResourceId offending = RenderResourceId::Unknown;
    CHECK(vkContractsCoverageOk(&offending));
    CHECK(static_cast<unsigned>(offending) == 0u);   // Unknown == untouched
    CHECK(vkContractsNoDuplicateIds());
}

TEST_CASE("descriptor-class totals match the inventory (Part-1/Part-2 enum-backed live set)") {
    CHECK(kVkResourceContractCount == 17);
    CHECK(vkCountByClass(VkDescriptorClass::UniformBuffer) == 1);
    CHECK(vkCountByClass(VkDescriptorClass::StorageBuffer) == 11);
    // 4 FBO-backed samplers + POSTPROCESS-VK-IMAGE-OWNERSHIP-1 subgraph depth = 5.
    CHECK(vkCountByClass(VkDescriptorClass::CombinedImageSampler) == 5);
    // Nothing live today lands in these classes (spec §"Totals by descriptor type").
    CHECK(vkCountByClass(VkDescriptorClass::StorageImage) == 0);
    CHECK(vkCountByClass(VkDescriptorClass::InputAttachment) == 0);
}

TEST_CASE("POSTPROCESS-VK-IMAGE-OWNERSHIP-1: subgraph depth is a sampler row; color is excluded") {
    // The subgraph-owned DEPTH image IS a descriptor-bound sampler.
    bool depthPresent = false;
    for (int i = 0; i < kVkResourceContractCount; ++i)
        if (kVkResourceContracts[i].id == RenderResourceId::PostprocessSubgraphDepth) {
            depthPresent = true;
            CHECK(kVkResourceContracts[i].cls == VkDescriptorClass::CombinedImageSampler);
        }
    CHECK(depthPresent);

    // The subgraph-owned COLOR image is DELIBERATELY EXCLUDED: it is a pure color
    // attachment + transfer src/dst, never sampled, never bound via a descriptor set.
    // Modeling it as any descriptor class would be wrong; its ownership lives in the
    // enum id + the transition-chain table (vulkan_layout_chain.h) instead.
    for (int i = 0; i < kVkResourceContractCount; ++i)
        CHECK(static_cast<unsigned>(kVkResourceContracts[i].id)
              != static_cast<unsigned>(RenderResourceId::PostprocessSubgraphColor));
}

TEST_CASE("negative: the consistency predicate REJECTS a mismatched kind/class") {
    // A Buffer declared as a sampler, or a Texture2D declared as an SSBO, must FAIL —
    // this is exactly what a fat-fingered table row would look like. (The shipped table
    // is proven clean above; here we exercise the guard itself so a real deliberate-break
    // would fire.)
    CHECK_FALSE(vkKindConsistent(RenderResourceKind::Buffer,
                                 VkDescriptorClass::CombinedImageSampler));
    CHECK_FALSE(vkKindConsistent(RenderResourceKind::Texture2D,
                                 VkDescriptorClass::StorageBuffer));
    CHECK_FALSE(vkKindConsistent(RenderResourceKind::Buffer,
                                 VkDescriptorClass::None));
    CHECK_FALSE(vkKindConsistent(RenderResourceKind::Unknown,
                                 VkDescriptorClass::StorageBuffer));
    // Sanity: the legit pairings PASS.
    CHECK(vkKindConsistent(RenderResourceKind::Buffer, VkDescriptorClass::UniformBuffer));
    CHECK(vkKindConsistent(RenderResourceKind::Buffer, VkDescriptorClass::StorageBuffer));
    CHECK(vkKindConsistent(RenderResourceKind::Texture2D,
                           VkDescriptorClass::CombinedImageSampler));
}

} // TEST_SUITE("VulkanContract")
