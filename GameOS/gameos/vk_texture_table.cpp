#include "vk_texture_table.h"
#include "vk_texture_residency.h"
#include "vulkan_context.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <chrono>

namespace mc2vk {
bool TextureTable::rebuildSamplers() {
    VkPhysicalDeviceFeatures features{}; vkGetPhysicalDeviceFeatures(ctx_.physicalDevice(), &features);
    VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(ctx_.physicalDevice(), &properties);
    std::array<OwnedSampler, kTexSamplerCount> next;
    for (uint32_t i = 0; i < kTexSamplerCount; ++i) {
        const auto kind = static_cast<TexSampler>(i);
        const bool clamp = kind == TexSampler::ClampBiased || kind == TexSampler::ClampBiasedLod16 ||
                           kind == TexSampler::ClampAniso8Biased;
        if (i < 2 || i >= 6) {
            if (!infra::makeMaterialSampler(ctx_.device(), next[i], clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT, bias_)) return false;
            continue;
        }
        // Match makeMaterialSampler, with the additional policies used by other islands.
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        infra::materialSampler(info); bias_ = info.mipLodBias;
        info.magFilter = info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = info.addressModeV = info.addressModeW = clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.maxLod = VK_LOD_CLAMP_NONE;
        if (kind == TexSampler::ClampBiasedLod16) info.maxLod = 16;
        if (kind == TexSampler::RepeatBaseLevel) { info.maxLod = 0; info.mipLodBias = 0; }
        if (kind == TexSampler::RepeatBiasedLod0) info.maxLod = 0;
        if (kind == TexSampler::ClampAniso8Biased && features.samplerAnisotropy) {
            info.anisotropyEnable = VK_TRUE; info.maxAnisotropy = std::min(8.0f, properties.limits.maxSamplerAnisotropy);
        }
        if (!create(next[i], ctx_.device(), [&](VkSampler* h) { return vkCreateSampler(ctx_.device(), &info, nullptr, h); })) return false;
    }
    for (uint32_t i = 0; i < kTexSamplerCount; ++i) {
        if (samplers_[i]) ctx_.retire(std::move(samplers_[i]));
        samplers_[i] = std::move(next[i]);
    }
    slots_.changed(capacity_); // sampler binding uses a single journal entry
    return true;
}

bool TextureTable::init() {
    constexpr auto stages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity_, stages, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, kTexSamplerCount, stages, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity_, stages, nullptr}
    };
    const VkDescriptorBindingFlags flags[]{VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bindingFlags.bindingCount = 3; bindingFlags.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.pNext = &bindingFlags; info.bindingCount = 3; info.pBindings = bindings;
    if (!create(layout_, ctx_.device(), [&](VkDescriptorSetLayout* h) { return vkCreateDescriptorSetLayout(ctx_.device(), &info, nullptr, h); })) return false;
    const VkDescriptorPoolSize sizes[]{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * capacity_ * kMaxFramesInFlight},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kTexSamplerCount * kMaxFramesInFlight}
    };
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = kMaxFramesInFlight; pool.poolSizeCount = 2; pool.pPoolSizes = sizes;
    if (!create(pool_, ctx_.device(), [&](VkDescriptorPool* h) { return vkCreateDescriptorPool(ctx_.device(), &pool, nullptr, h); })) return false;
    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> layouts; layouts.fill(layout_.get());
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool_.get(); alloc.descriptorSetCount = kMaxFramesInFlight; alloc.pSetLayouts = layouts.data();
    if (MC2_VK_ALLOC_SETS(ctx_.device(), &alloc, sets_.data()) != VK_SUCCESS || !rebuildSamplers()) return false;
    for (auto& tokens : tokens_) tokens.resize(capacity_);
    if (mc2_diag::tagRequested("VK_TEXTURE_SERVICE"))
        std::fprintf(stderr, "[VK_TEXTABLE v1] capacity=%u sets=%u samplers=%u\n", capacity_, kMaxFramesInFlight, kTexSamplerCount);
    return true;
}

void TextureTable::beginFrame(uint32_t frameSlot) {
    assert(ctx_.tracker().frameOpen() && begunFrame_ != ctx_.tracker().frameNo());
    begunFrame_ = ctx_.tracker().frameNo();
    const auto started = std::chrono::steady_clock::now();
    if (infra::materialBiasStale(bias_) && !rebuildSamplers()) throw std::runtime_error("texture table sampler rebuild failed");
    bool full = false;
    const auto changes = slots_.changesSince(applied_[frameSlot], full);
    full = full || !initialized_[frameSlot];
    std::vector<uint32_t> dirty;
    if (full) { for (uint32_t i = 0; i <= capacity_; ++i) dirty.push_back(i); }
    else for (const auto& entry : changes) dirty.push_back(entry.slot);
    std::sort(dirty.begin(), dirty.end()); dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
    std::vector<VkDescriptorImageInfo> images; images.reserve(dirty.size() * 2 + kTexSamplerCount);
    std::vector<VkWriteDescriptorSet> writes; writes.reserve(dirty.size() * 2 + 1);
    const auto write = [&](uint32_t binding, uint32_t slot, VkDescriptorType type, uint32_t count, size_t offset) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets_[frameSlot]; w.dstBinding = binding; w.dstArrayElement = slot;
        w.descriptorType = type; w.descriptorCount = count; w.pImageInfo = images.data() + offset;
        writes.push_back(w);
    };
    for (const auto i : dirty) {
        if (i == capacity_) {
            const size_t start = images.size();
            for (const auto& sampler : samplers_) images.push_back({sampler.get(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
            write(1, 0, VK_DESCRIPTOR_TYPE_SAMPLER, kTexSamplerCount, start);
        } else {
            const auto& slot = slots_.slot(i);
            const auto handle = i ? slot.handle : RenderCore::TextureHandle::invalid();
            const bool array = residency_.arrayView(handle);
            images.push_back({VK_NULL_HANDLE, residency_.view(array ? RenderCore::TextureHandle::invalid() : handle), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            write(0, i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, images.size() - 1);
            images.push_back({VK_NULL_HANDLE, residency_.view(array ? handle : RenderCore::TextureHandle::invalid(), true), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            write(2, i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, images.size() - 1);
            tokens_[frameSlot][i] = {static_cast<uint16_t>(i), slot.gen};
        }
    }
    if (!writes.empty()) vkUpdateDescriptorSets(ctx_.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    applied_[frameSlot] = slots_.revision(); initialized_[frameSlot] = true;
    if (full && mc2_diag::tagRequested("VK_TEXTURE_SERVICE")) std::fprintf(stderr, "[VK_TEXTABLE_REWRITE v1] slot=%u writes=%zu us=%.3f\n", frameSlot, writes.size(),
                           std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());
    if ((!writes.empty() || ctx_.tracker().frameNo() % 120 == 0) && mc2_diag::tagRequested("VK_TEXTURE_SERVICE"))
        std::fprintf(stderr, "[VK_TEXTABLE_FRAME v1] frame=%llu slot=%u writes=%zu full=%u stale=%llu\n",
                     static_cast<unsigned long long>(ctx_.tracker().frameNo()), frameSlot, writes.size(), full ? 1u : 0u, static_cast<unsigned long long>(stale_));
}

uint32_t TextureTable::resolve(RenderCore::TexSlot token) {
    const auto slot = slots_.resolve(token);
    if (token.slot && !slot) { ++stale_; return 0; }
    const auto& applied = tokens_[ctx_.frameContext().slot][slot];
    return applied.gen == token.gen ? slot : 0;
}
void TextureTable::bind(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t setIndex) const {
    const auto descriptor = set(ctx_.frameContext().slot);
    vkCmdBindDescriptorSets(cb, point, layout, setIndex, 1, &descriptor, 0, nullptr);
}
}
