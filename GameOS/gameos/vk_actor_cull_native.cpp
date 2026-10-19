// Context-owned native producer for lagged actor visibility. No GL dependencies.
#include "vk_actor_cull_native.h"
#include "vk_compute_pipeline.h"
#include "vk_one_shot.h"
#include "vk_island_util.h"
#include "vulkan_context.h"
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace gpu_cull {
namespace {
struct alignas(16) CullParams { float mvp[16]; uint32_t counts[4]; float options[4]; };
static_assert(sizeof(CullParams)==96);
void bufferBarrier(VkCommandBuffer cb,VkBuffer buffer,VkPipelineStageFlags srcStage,VkAccessFlags srcAccess,
                   VkPipelineStageFlags dstStage,VkAccessFlags dstAccess,VkDeviceSize bytes=VK_WHOLE_SIZE) {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask=srcAccess; barrier.dstAccessMask=dstAccess;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer=buffer; barrier.offset=0; barrier.size=bytes;
    vkCmdPipelineBarrier(cb,srcStage,dstStage,0,0,nullptr,1,&barrier,0,nullptr);
}

struct NativeCull final : mc2vk::IslandState {
    struct Slot {
        mc2vk::OwnedBuffer records, params, output, staging;
        mc2vk::OwnedCommandPool pool;
        mc2vk::OwnedQueryPool queries;
        VkCommandBuffer cb=VK_NULL_HANDLE;
        mc2vk::GpuTicket ticket;
        uint32_t count=0, frame=0;
        std::vector<uint32_t> ids;
        bool submitted=false;
    };
    mc2vk::ComputePipeline pipeline;
    mc2vk::ComputeSets sets;
    std::array<Slot,3> slots;
    uint32_t capacity=0;
    bool timed=false;
    double timestampPeriod=0;
    uint64_t timestampMask=UINT64_MAX;
    std::optional<uint64_t> lastDispatchNs;
    uint32_t readySamples=0;
    VkDeviceSize bytes=0;
    bool init(mc2vk::VulkanContext& ctx, uint32_t cap) {
        if (!cap || cap>1048576) return false;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(ctx.physicalDevice(),&properties);
        uint32_t familyCount=0; vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(),&familyCount,nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(ctx.physicalDevice(),&familyCount,families.data());
        const uint32_t bits=ctx.graphicsFamily()<familyCount ? families[ctx.graphicsFamily()].timestampValidBits : 0;
        timed=bits!=0; timestampPeriod=properties.limits.timestampPeriod;
        timestampMask=bits && bits<64 ? (uint64_t(1)<<bits)-1 : UINT64_MAX;
        capacity=cap; bytes=VkDeviceSize(4+cap)*sizeof(uint32_t);
        const auto S=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, U=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (!pipeline.create(ctx.device(),vk_island_util::spvDir()+"vk_actor_cull.comp.spv",{{0,S},{1,U},{2,S}},"actorCull") ||
            !sets.create(ctx.device(),pipeline,3,"actorCull.sets")) return false;
        for (uint32_t i=0;i<3;++i) {
            auto& slot=slots[i];
            if (!slot.records.createHostWritable(ctx.allocator(),ctx.device(),VkDeviceSize(cap)*sizeof(GpuActorRecord),
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,"actorCull.records",int(i)) ||
                !slot.params.createHostWritable(ctx.allocator(),ctx.device(),sizeof(CullParams),
                                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,"actorCull.params",int(i))) return false;
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size=bytes;
            info.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo device{}; device.usage=VMA_MEMORY_USAGE_AUTO;
            if (!slot.output.create(ctx.allocator(),ctx.device(),info,device,"actorCull.output",int(i))) return false;
            info.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo host{}; host.usage=VMA_MEMORY_USAGE_AUTO;
            host.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            if (!slot.staging.create(ctx.allocator(),ctx.device(),info,host,"actorCull.readback",int(i)) ||
                !mc2vk::beginTransientCommandBuffer(ctx,slot.pool,slot.cb)) return false;
            // beginTransientCommandBuffer returns a begun command buffer; initialize it
            // empty now so the first submission can reset the pool like every later one.
            if (vkEndCommandBuffer(slot.cb)!=VK_SUCCESS) return false;
            sets.write(ctx.device(),i,0,S,slot.records.get());
            sets.write(ctx.device(),i,1,U,slot.params.get());
            sets.write(ctx.device(),i,2,S,slot.output.get());
            slot.ids.resize(cap);
            if (timed) {
                VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; q.queryType=VK_QUERY_TYPE_TIMESTAMP; q.queryCount=2;
                if (!mc2vk::create(slot.queries,ctx.device(),[&](VkQueryPool* pool){return vkCreateQueryPool(ctx.device(),&q,nullptr,pool);})) timed=false;
            }
        }
        return true;
    }
};
NativeCull* state() {
    if (!mc2vk::engineVulkanContextValid()) return nullptr;
    auto& owner=mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::ActorCull);
    return static_cast<NativeCull*>(owner.get());
}
}
namespace { bool selftestNative(); }
bool nativeCullInit(uint32_t capacity) {
    if (!mc2vk::engineVulkanContextValid()) return false;
    nativeCullShutdown();
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext();
    auto owner=std::make_unique<NativeCull>();
    if (!owner->init(ctx,capacity)) return false;
    ctx.islandSlot(mc2vk::IslandSlot::ActorCull)=std::move(owner);
    if (!selftestNative()) { nativeCullShutdown(); return false; }
    state()->lastDispatchNs.reset(); state()->readySamples=0;
    for (auto& slot:state()->slots) slot.submitted=false;
    return true;
}
void nativeCullShutdown() {
    if (state()) mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::ActorCull);
}
NativeCullSubmit nativeCullSubmit(uint32_t i, std::span<const GpuActorRecord> records,
                                 const float* mvp, float dilation, uint32_t frame) {
    auto* s=state();
    if (!s || i>=3 || !mvp || records.empty() || records.size()>s->capacity) return NativeCullSubmit::Failed;
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext(); auto& slot=s->slots[i];
    ctx.poll();
    if (slot.submitted && !ctx.isComplete(slot.ticket)) return NativeCullSubmit::Busy;
    CullParams params{}; std::memcpy(params.mvp,mvp,sizeof(params.mvp));
    params.counts[0]=static_cast<uint32_t>(records.size()); params.counts[1]=frame; params.options[0]=dilation;
    if (!slot.records.upload(records.data(),records.size_bytes()) || !slot.params.upload(&params,sizeof(params)) ||
        vkResetCommandPool(ctx.device(),slot.pool.get(),0)!=VK_SUCCESS) return NativeCullSubmit::Failed;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(slot.cb,&begin)!=VK_SUCCESS) return NativeCullSubmit::Failed;
    bufferBarrier(slot.cb,slot.records.get(),VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
    bufferBarrier(slot.cb,slot.params.get(),VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_UNIFORM_READ_BIT);
    // Only this slot is reused. A device-wide ALL_COMMANDS memory barrier also
    // joins every unrelated image and descriptor history under sync validation.
    bufferBarrier(slot.cb,slot.output.get(),VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdFillBuffer(slot.cb,slot.output.get(),0,16,0);
    bufferBarrier(slot.cb,slot.output.get(),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,16);
    if (s->timed) {
        vkCmdResetQueryPool(slot.cb,slot.queries.get(),0,2);
        vkCmdWriteTimestamp(slot.cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,slot.queries.get(),0);
    }
    s->pipeline.bind(slot.cb,s->sets.set(i)); s->pipeline.dispatch(slot.cb,(params.counts[0]+63)/64);
    if (s->timed) vkCmdWriteTimestamp(slot.cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,slot.queries.get(),1);
    bufferBarrier(slot.cb,slot.output.get(),VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    bufferBarrier(slot.cb,slot.staging.get(),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    // The tail beyond the submitted records has no reader this frame.
    const VkDeviceSize usedBytes=VkDeviceSize(4+params.counts[0])*sizeof(uint32_t);
    VkBufferCopy copy{0,0,usedBytes}; vkCmdCopyBuffer(slot.cb,slot.output.get(),slot.staging.get(),1,&copy);
    bufferBarrier(slot.cb,slot.staging.get(),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    if (vkEndCommandBuffer(slot.cb)!=VK_SUCCESS) return NativeCullSubmit::Failed;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&slot.cb;
    uint64_t ticket=0;
    if (ctx.submit(submit,VK_NULL_HANDLE,&ticket)!=VK_SUCCESS) return NativeCullSubmit::Failed;
    slot.ticket={ticket,0}; slot.submitted=true; slot.count=params.counts[0]; slot.frame=frame;
    for (uint32_t j=0;j<slot.count;++j) slot.ids[j]=records[j].actorId;
    return NativeCullSubmit::Submitted;
}
bool nativeCullPoll(uint32_t i, NativeCullHeader& header, uint32_t& count,
                    std::span<uint32_t> visibility, std::span<uint32_t> ids) {
    auto* s=state(); if (!s || i>=3) return false;
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext(); auto& slot=s->slots[i];
    ctx.poll();
    if (!slot.submitted || !ctx.isComplete(slot.ticket) || visibility.size()<slot.count || ids.size()<slot.count) return false;
    const auto* mapped=static_cast<const uint32_t*>(slot.staging.map()); if (!mapped) return false;
    vmaInvalidateAllocation(ctx.allocator(),slot.staging.allocation(),0,VK_WHOLE_SIZE);
    std::memcpy(&header,mapped,sizeof(header));
    const bool valid=header.frame==slot.frame;
    if (valid) std::memcpy(visibility.data(),mapped+4,size_t(slot.count)*sizeof(uint32_t));
    slot.staging.unmap();
    if (!valid) return false;
    count=slot.count; std::copy_n(slot.ids.data(),count,ids.data());
    if (s->timed) {
        uint64_t ticks[2]{};
        if (vkGetQueryPoolResults(ctx.device(),slot.queries.get(),0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS)
            s->lastDispatchNs=static_cast<uint64_t>(double((ticks[1]-ticks[0])&s->timestampMask)*s->timestampPeriod);
    }
    if (++s->readySamples==1 || s->readySamples%256==0) {
        if (s->lastDispatchNs) std::printf("[GPU_CULL v2] event=native_result frame=%u records=%u visible=%u gpu_ns=%llu readback_bytes=%llu\n",
            header.frame,count,header.visible,static_cast<unsigned long long>(*s->lastDispatchNs),
            static_cast<unsigned long long>(VkDeviceSize(count+4)*sizeof(uint32_t)));
        else std::printf("[GPU_CULL v2] event=native_result frame=%u records=%u visible=%u gpu_ns=unavailable\n",header.frame,count,header.visible);
        std::fflush(stdout);
    }
    return true;
}
std::optional<uint64_t> nativeCullLastDispatchNs() {
    auto* producer=state(); return producer ? producer->lastDispatchNs : std::nullopt;
}
namespace {
bool selftestNative() {
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext();
    unsigned checked=0;
    auto check=[&](const std::vector<GpuActorRecord>& records, const std::vector<uint32_t>& expected,
                   const float* matrix, uint32_t dilated) {
        uint32_t expectedVisible=0;
        for (auto bit:expected) expectedVisible+=bit;
        // Engine missions have room for all probes. Small callers still get real
        // GPU coverage by submitting one case at a time.
        const uint32_t capacity=state()->capacity;
        for (uint32_t first=0;first<records.size();) {
            const uint32_t count=(std::min)(capacity,static_cast<uint32_t>(records.size())-first);
            const uint32_t frame=0xa100u+checked;
            if (nativeCullSubmit(0,{records.data()+first,count},matrix,0.08f,frame)!=NativeCullSubmit::Submitted ||
                !ctx.wait(state()->slots[0].ticket,5ull*1000*1000*1000,"actorCull_selftest")) return false;
            NativeCullHeader header{}; uint32_t copied=0;
            std::vector<uint32_t> bits(count),ids(count);
            if (!nativeCullPoll(0,header,copied,bits,ids) || copied!=count || header.frame!=frame) return false;
            uint32_t visible=0;
            for (uint32_t i=0;i<count;++i) {
                if (bits[i]!=expected[first+i] || ids[i]!=records[first+i].actorId) return false;
                visible+=bits[i]; ++checked;
            }
            if (header.visible!=visible || header.culled!=count-visible) return false;
            if (count==records.size() && (header.visible!=expectedVisible || header.dilated!=dilated)) return false;
            first+=count;
        }
        return true;
    };
    std::vector<GpuActorRecord> records(8);
    for (uint32_t i=0;i<records.size();++i) { records[i].worldCenter[2]=0.5f; records[i].actorId=100+i; }
    records[1].worldCenter[0]=2;
    records[2].worldCenter[0]=1.04f;
    records[3].worldCenter[0]=1.2f; records[3].boundingRadius=0.25f;
    records[4].worldCenter[2]=-0.02f;
    records[5].worldCenter[2]=1.2f;
    records[6].worldCenter[0]=3; records[6].flags=Flag_AlwaysVisible;
    records[7].worldCenter[1]=2;
    const std::vector<uint32_t> expected{1,0,1,1,1,0,1,0};
    float matrix[16]{}; matrix[0]=matrix[5]=matrix[10]=matrix[15]=1;
    if (!check(records,expected,matrix,2)) return false;
    for (auto& record:records) record.worldCenter[2]=-record.worldCenter[2];
    matrix[15]=-1;
    if (!check(records,expected,matrix,2)) return false;
    records.resize(4); for (auto& record:records) record.flags=0;
    records[1].boundingRadius=1; records[2].flags=Flag_AlwaysVisible; records[3].boundingRadius=0;
    matrix[15]=0;
    if (!check(records,{0,1,1,0},matrix,0)) return false;
    std::printf("[GPU_CULL v2] event=native_predicate_selftest passed=%u failed=0 backend=vulkan\n",checked);
    std::fflush(stdout);
    return true;
}
}

}
