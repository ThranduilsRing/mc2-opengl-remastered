#ifdef MC2_VULKAN_ISLAND
#define NOMINMAX
#include "vk_render_infra.h"
#include "vulkan_context.h"
#include "vk_owned.h"
#include "imgui.h"
#include "gos_crashbundle.h"
#include "Gates/render_options.h"
#include "Gates/gate_cold.h"
#include "gos_postprocess.h"
#include <windows.h>
#undef min
#undef max
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <utility>

extern long turn;
extern "C" int mc2_vk_terrain_island_prepare(int,int);
extern "C" int mc2_vk_sky_island_prewarm(int,int);
extern "C" int mc2_vk_mech_island_prewarm(int,int);
extern "C" int mc2_vk_color_prop_island_prewarm(int,int);
extern "C" int mc2_vk_legacy_shape_island_prewarm(int,int);
extern "C" int mc2_vk_vfx_island_prewarm(int,int);
extern "C" int mc2_vk_water_island_prewarm(int,int);
extern "C" int mc2_vk_shadow_island_prewarm(int,int);
extern "C" int mc2_vk_overlay_prewarm(int,int);

namespace mc2vk::infra {
namespace {
uint64_t hashBytes(const void* data, size_t n) {
    auto p = static_cast<const unsigned char*>(data);
    uint64_t h = 14695981039346656037ull;
    for (size_t i=0;i<n;++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
constexpr uint64_t MiB=1024ull*1024;
constexpr uint64_t maxCache=64*MiB;
struct CacheHeader { uint64_t magic=0x314548434143324dull; uint64_t bytes=0, hash=0; uint32_t vendor=0, device=0, driver=0; unsigned char uuid[VK_UUID_SIZE]{}; };
struct Allocation { uint64_t bytes; size_t category; bool raw; bool deferred=false; };
constexpr const char* categories[]={"textures","shadows","terrain","vegetation","geometry","staging","targets"};
struct Shader { std::string path; std::vector<uint32_t> code; uint64_t hash=0; };
struct State {
    VkDevice dev=VK_NULL_HANDLE; VkPhysicalDevice phys=VK_NULL_HANDLE; VkPipelineCache cache=VK_NULL_HANDLE;
    CacheHeader identity; std::filesystem::path cachePath;
    std::mutex mutex; bool dirty=false; uint64_t creations=0; double creationMs=0;
    std::chrono::steady_clock::time_point saved, started;
    std::unordered_map<uint64_t,Allocation> allocations;
    std::unordered_map<VkShaderModule,Shader> shaders;
    std::unordered_map<std::string,Shader> overrides;
    std::array<uint64_t,7> current{}, peaks{};
    uint64_t live=0,peak=0,raw=0,budget=2048*MiB,denied=0;
    uint64_t deferredBytes=0;
    uint64_t prewarmCreations=0; std::vector<double> firstFrames; bool measureFrames=false, firstFrameStarted=false;
    std::chrono::steady_clock::time_point previousFrame;
    char driver[512]{}, ledger[2048]{};
    char packages[4096]{};
    // Interlocked publication means the crash handler can skip a slot being written.
    struct Crumb { volatile LONG writing=0; char text[384]{}; };
    std::array<Crumb,256> crumbs; volatile LONG cursor=0;
    // GPU slots advance only for passes, so per-draw bind logging cannot overwrite queued checkpoint identities.
    std::array<Crumb,1024> gpuCrumbs; volatile LONG gpuCursor=0;
    std::array<Crumb,128> identities; volatile LONG identityCursor=0;
    std::array<Crumb,256> resources; volatile LONG resourceCursor=0;
    bool checkpoints=false;
    std::atomic_bool lossReported=false;
    VkBuffer marker=VK_NULL_HANDLE; VkDeviceMemory markerMemory=VK_NULL_HANDLE; volatile uint32_t* markerMap=nullptr;
};
State* statePtr=nullptr;
// The crash filter can run after normal renderer teardown; this process-lifetime storage deliberately survives it.
State& state() { static auto* s=[] { auto* p=std::make_unique<State>().release(); statePtr=p; return p; }(); return *s; }
size_t category(std::string_view name) {
    if (name.find("staging")!=name.npos || name.find("stage")!=name.npos || name.find("frameRing")!=name.npos || name.find("readback")!=name.npos) return 5;
    if (name.find("shadow")!=name.npos) return 1;
    if (name.find("terrain")!=name.npos) return 2;
    if (name.find("vegetation")!=name.npos || name.find("veg.")!=name.npos) return 3;
    if (name.find("texture")!=name.npos || name.find("atlas")!=name.npos) return 0;
    if (name.find("scene")!=name.npos || name.find("post")!=name.npos || name.find("target")!=name.npos) return 6;
    return 4;
}
void saveLocked(State& s) {
    if (!s.cache || !s.dirty) return;
    s.saved=std::chrono::steady_clock::now();
    size_t bytes=0;
    if (vkGetPipelineCacheData(s.dev,s.cache,&bytes,nullptr)!=VK_SUCCESS || bytes>maxCache) return;
    std::vector<unsigned char> data(bytes);
    if (vkGetPipelineCacheData(s.dev,s.cache,&bytes,data.data())!=VK_SUCCESS) return;
    CacheHeader h=s.identity; h.bytes=bytes; h.hash=hashBytes(data.data(),bytes);
    const auto tmp=s.cachePath.wstring()+L".tmp";
    { std::ofstream out(tmp,std::ios::binary|std::ios::trunc); out.write(reinterpret_cast<const char*>(&h),sizeof(h)); out.write(reinterpret_cast<const char*>(data.data()),bytes); out.flush(); if (!out) return; }
    std::error_code ec; std::filesystem::rename(tmp,s.cachePath,ec);
    if (!ec) {
        s.dirty=false; s.saved=std::chrono::steady_clock::now();
        std::fprintf(stderr,"[VK_PIPELINE_CACHE v1] saved_bytes=%zu pipelines=%llu creation_ms=%.3f\n",bytes,s.creations,s.creationMs);
    }
}
}

void init(VkDevice device, VkPhysicalDevice physical) {
    auto& s=state(); s.dev=device; s.phys=physical; s.started=s.saved=std::chrono::steady_clock::now();
    VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(physical,&p);
    s.identity.vendor=p.vendorID; s.identity.device=p.deviceID; s.identity.driver=p.driverVersion; std::memcpy(s.identity.uuid,p.pipelineCacheUUID,VK_UUID_SIZE);
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; props.pNext=&driver; vkGetPhysicalDeviceProperties2(physical,&props);
    std::snprintf(s.driver,sizeof(s.driver),"device=%s vendor=%x device_id=%x driver_version=%u driver=%s info=%s",p.deviceName,p.vendorID,p.deviceID,p.driverVersion,driver.driverName,driver.driverInfo);
    breadcrumb("driver",s.driver);
    std::snprintf(s.packages,sizeof(s.packages),"active_mod=%s dependencies=%s",mc2::gate::str<"MC2_ACTIVE_MOD">().c_str(),mc2::gate::str<"MC2_MOD_DEPS">().c_str());
    // Profile floor is usable on a 512 MiB adapter; OS pressure can lower admission further.
    VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(physical,&mp); uint64_t local=0;
    for (uint32_t i=0;i<mp.memoryHeapCount;++i) if(mp.memoryHeaps[i].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) local+=mp.memoryHeaps[i].size;
    s.budget=std::clamp<uint64_t>(local/2,512*MiB,2048*MiB);
    // The graphics presets already publish ShadowQuality: Potato=0, Medium=1, High=2.
    if(mc2::options::current().shadowQuality==0) s.budget=512*MiB;
    else if(mc2::options::current().shadowQuality==1) s.budget=std::min(s.budget,1024*MiB);
    std::error_code ec; std::filesystem::create_directories("cache/vulkan",ec);
    char key[160]{}; size_t at=std::snprintf(key,sizeof(key),"%08x-%08x-%08x-",p.vendorID,p.deviceID,p.driverVersion);
    for(auto b:p.pipelineCacheUUID) at+=std::snprintf(key+at,sizeof(key)-at,"%02x",b);
    s.cachePath=std::filesystem::path("cache/vulkan")/(std::string(key)+".bin");
    std::vector<unsigned char> data; const char* status="cold";
    { std::ifstream in(s.cachePath,std::ios::binary|std::ios::ate); if(in) {
        const auto total=in.tellg(); in.seekg(0); CacheHeader h{}; in.read(reinterpret_cast<char*>(&h),sizeof(h));
        if(in && h.magic==s.identity.magic && h.vendor==p.vendorID && h.device==p.deviceID && h.driver==p.driverVersion &&
            !std::memcmp(h.uuid,p.pipelineCacheUUID,VK_UUID_SIZE) && h.bytes>=32 && h.bytes<=maxCache && total==std::streamoff(sizeof(h)+h.bytes)) {
            data.resize(size_t(h.bytes)); in.read(reinterpret_cast<char*>(data.data()),data.size());
            // Validate the Vulkan header as well as our envelope before giving bytes to the ICD.
            uint32_t v[4]{}; std::memcpy(v,data.data(),sizeof(v));
            if(!in || hashBytes(data.data(),data.size())!=h.hash || v[0]<32 || v[0]>data.size() || v[1]!=VK_PIPELINE_CACHE_HEADER_VERSION_ONE || v[2]!=p.vendorID || v[3]!=p.deviceID || std::memcmp(data.data()+16,p.pipelineCacheUUID,VK_UUID_SIZE)) data.clear();
        }
        status=data.empty()?"rejected":"warm";
    }}
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; ci.initialDataSize=data.size(); ci.pInitialData=data.data();
    if(vkCreatePipelineCache(device,&ci,nullptr,&s.cache)!=VK_SUCCESS) { ci.initialDataSize=0; ci.pInitialData=nullptr; status="driver_rejected"; vkCreatePipelineCache(device,&ci,nullptr,&s.cache); }
    std::fprintf(stderr,"[VK_PIPELINE_CACHE v1] status=%s initial_bytes=%zu %s\n",status,data.size(),s.driver);
    uint32_t n=0; vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,nullptr);
    std::vector<VkExtensionProperties> extensions(n); vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,extensions.data());
    bool amd=false;
    for(auto& e:extensions) {
        if(!std::strcmp(e.extensionName,VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME)) s.checkpoints=vkCmdSetCheckpointNV!=nullptr;
        if(!std::strcmp(e.extensionName,VK_AMD_BUFFER_MARKER_EXTENSION_NAME)) amd=vkCmdWriteBufferMarkerAMD!=nullptr;
    }
    if(amd) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=s.gpuCrumbs.size()*sizeof(uint32_t); bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(vkCreateBuffer(device,&bi,nullptr,&s.marker)==VK_SUCCESS) {
            VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,s.marker,&req);
            for(uint32_t i=0;i<mp.memoryTypeCount;++i) if((req.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=i;
                if(vkAllocateMemory(device,&ai,nullptr,&s.markerMemory)==VK_SUCCESS && vkBindBufferMemory(device,s.marker,s.markerMemory,0)==VK_SUCCESS) {
                    void* mapped=nullptr; if(vkMapMemory(device,s.markerMemory,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS) {
                        std::memset(mapped,0,size_t(bi.size)); s.markerMap=static_cast<volatile uint32_t*>(mapped);
                        allocation(reinterpret_cast<uint64_t>(s.markerMemory),req.size,"staging.gpuCrashMarkers",true);
                    }
                }
                break;
            }
        }
    }
    std::fprintf(stderr,"[VK_BREADCRUMBS v1] sink=%s cpu_ring=256 %s\n",s.markerMap?"AMD_buffer_marker":s.checkpoints?"NV_checkpoints":"CPU",s.packages);
}
void saveCache() { auto& s=state(); std::lock_guard lock(s.mutex); saveLocked(s); }
VkPipelineCache cacheHandle() { return state().cache; }
void shutdown() { auto& s=state(); saveCache(); if(s.cache) vkDestroyPipelineCache(s.dev,s.cache,nullptr); s.cache=VK_NULL_HANDLE;
    if(s.markerMap) { vkUnmapMemory(s.dev,s.markerMemory); s.markerMap=nullptr; release(reinterpret_cast<uint64_t>(s.markerMemory)); }
    if(s.marker) { vkDestroyBuffer(s.dev,s.marker,nullptr); s.marker=VK_NULL_HANDLE; }
    if(s.markerMemory) { vkFreeMemory(s.dev,s.markerMemory,nullptr); s.markerMemory=VK_NULL_HANDLE; }
    s.dev=VK_NULL_HANDLE;
}
void allocation(uint64_t key,uint64_t bytes,std::string_view name,bool raw) {
    auto& s=state(); std::lock_guard lock(s.mutex); auto c=category(name);
    const auto [entry,inserted]=s.allocations.emplace(key,Allocation{bytes,c,raw});
    if(!inserted) return;
    s.live+=bytes; s.raw+=raw?bytes:0; s.current[c]+=bytes;
    s.peaks[c]=std::max(s.peaks[c],s.current[c]);
    s.peak=std::max(s.peak,s.live);
}
void release(uint64_t key) {
    auto& s=state(); std::lock_guard lock(s.mutex); auto it=s.allocations.find(key);
    if(it==s.allocations.end()) return;
    const auto& [allocationKey,a]=*it;
    s.live-=a.bytes; s.raw-=a.raw?a.bytes:0;
    if(a.deferred) s.deferredBytes-=a.bytes;
    s.current[a.category]-=a.bytes; s.allocations.erase(it);
}
void defer(uint64_t key) {
    auto& s=state(); std::lock_guard lock(s.mutex); auto it=s.allocations.find(key);
    if(it==s.allocations.end()) return;
    auto& [allocationKey,a]=*it;
    if(!a.deferred) { a.deferred=true; s.deferredBytes+=a.bytes; }
}
bool admit(uint64_t bytes) {
    auto& s=state(); uint64_t os=0,usage=0,allocated=0; getOrCreateEngineVulkanContext().deviceLocalMemory(os,usage,allocated);
    std::lock_guard lock(s.mutex); const uint64_t limit=os?std::min(os,s.budget):s.budget;
    const bool ok=s.live<=limit && bytes<=limit-s.live && (!os || (usage<=os && bytes<=os-usage));
    if(!ok) ++s.denied;
    return ok;
}
uint32_t shadowTier(uint32_t requested,uint64_t bpp,uint32_t copies) {
    uint32_t tier=requested; while(tier>512 && !admit(uint64_t(tier)*tier*bpp*copies)) tier/=2;
    if(!admit(uint64_t(tier)*tier*bpp*copies)) return 0;
    if(tier!=requested) std::fprintf(stderr,"[VK_RESIDENCY v1] shadow_tier=%u requested=%u\n",tier,requested);
    return tier;
}
std::string ledgerJson() {
    auto& s=state(); uint64_t os=0,usage=0,allocated=0; getOrCreateEngineVulkanContext().deviceLocalMemory(os,usage,allocated);
    const auto& deferred=getOrCreateEngineVulkanContext().deferred(); std::lock_guard lock(s.mutex);
    std::ostringstream out; out<<"{\"budget_bytes\":"<<s.budget<<",\"live_bytes\":"<<s.live<<",\"peak_bytes\":"<<s.peak<<",\"vma_budget_bytes\":"<<os<<",\"vma_usage_bytes\":"<<usage<<",\"vma_allocated_bytes\":"<<allocated<<",\"dedicated_bytes\":"<<s.raw<<",\"deferred_bytes\":"<<s.deferredBytes<<",\"deferred_entries\":"<<deferred.size()<<",\"deferred_is_subset\":true,\"transition_overlap_included\":true,\"denials\":"<<s.denied<<",\"categories\":{";
    for(size_t i=0;i<7;++i) out<<(i?",":"")<<'"'<<categories[i]<<"\":{\"live\":"<<s.current[i]<<",\"peak\":"<<s.peaks[i]<<'}';
    out<<"}}"; const std::string json=out.str(); std::snprintf(s.ledger,sizeof(s.ledger),"%s",json.c_str()); return json;
}
void panel() {
    if(!ImGui::CollapsingHeader("Vulkan residency")) return;
    ledgerJson(); auto& s=state(); std::lock_guard lock(s.mutex);
    ImGui::Text("Live %.1f / %.0f MiB; peak %.1f MiB",double(s.live)/MiB,double(s.budget)/MiB,double(s.peak)/MiB);
    for(size_t i=0;i<7;++i) ImGui::Text("%s: %.1f MiB (peak %.1f)",categories[i],double(s.current[i])/MiB,double(s.peaks[i])/MiB);
    ImGui::Text("Deferred entries: %zu; allocation denials: %llu",getOrCreateEngineVulkanContext().deferred().size(),s.denied);
    ImGui::TextUnformatted("Live includes resources awaiting GPU completion and transition overlap.");
}
void breadcrumb(std::string_view kind,std::string_view name,uint64_t hash,VkCommandBuffer cb) {
    auto& s=state(); const LONG sequence=InterlockedIncrement(&s.cursor); auto& crumb=s.crumbs[uint32_t(sequence)%s.crumbs.size()];
    if(InterlockedCompareExchange(&crumb.writing,1,0)!=0) return;
    std::snprintf(crumb.text,sizeof(crumb.text),"seq=%ld tick=%ld frame=%llu kind=%.*s name=%.*s hash=%016llx",sequence,::turn,getOrCreateEngineVulkanContext().tracker().frameNo(),int(kind.size()),kind.data(),int(std::min<size_t>(name.size(),230)),name.data(),hash);
    InterlockedExchange(&crumb.writing,0);
    auto retain=[&](auto& ring,volatile LONG& cursor) {
        auto& entry=ring[uint32_t(InterlockedIncrement(&cursor))%ring.size()];
        if(InterlockedCompareExchange(&entry.writing,1,0)==0) { std::memcpy(entry.text,crumb.text,sizeof(entry.text)); InterlockedExchange(&entry.writing,0); }
    };
    if(kind=="shader" || kind=="pipeline") retain(s.identities,s.identityCursor);
    if(kind=="resource") retain(s.resources,s.resourceCursor);
    if(cb) {
        const auto index=uint32_t(InterlockedIncrement(&s.gpuCursor))%s.gpuCrumbs.size();
        auto& gpu=s.gpuCrumbs[index];
        InterlockedExchange(&gpu.writing,1); std::memcpy(gpu.text,crumb.text,sizeof(gpu.text)); InterlockedExchange(&gpu.writing,0);
        if(s.checkpoints) vkCmdSetCheckpointNV(cb,&gpu);
        if(s.markerMap) vkCmdWriteBufferMarkerAMD(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.marker,VkDeviceSize(index)*sizeof(uint32_t),uint32_t(sequence));
    }
}
void crashSnapshot(std::string_view folder) {
    if(!statePtr) return;
    auto& s=*statePtr; char path[MAX_PATH]{}; std::snprintf(path,sizeof(path),"%.*s/gpu_breadcrumbs.txt",int(folder.size()),folder.data());
    HANDLE f=CreateFileA(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr); if(f==INVALID_HANDLE_VALUE) return;
    DWORD wrote=0; auto write=[&](std::string_view p) { WriteFile(f,p.data(),DWORD(std::min<size_t>(p.size(),2048)),&wrote,nullptr); WriteFile(f,"\r\n",2,&wrote,nullptr); };
    write(s.driver); write(s.packages); write(s.ledger);
    write("Recent shader/pipeline identities:");
    for(auto& c:s.identities) if(!c.writing && c.text[0]) write(c.text);
    write("Recent resource names (including retired resources):");
    for(auto& c:s.resources) if(!c.writing && c.text[0]) write(c.text);
    write("Recent recording ticks and GPU pass markers:");
    LONG end=s.cursor; for(LONG i=std::max<LONG>(1,end-255);i<=end;++i) { auto& c=s.crumbs[uint32_t(i)%s.crumbs.size()]; if(!c.writing) {
        write(c.text);
    }}
    write("GPU pass slots and completed sequences (zero means not reached):");
    for(size_t i=0;i<s.gpuCrumbs.size();++i) if(!s.gpuCrumbs[i].writing && s.gpuCrumbs[i].text[0]) {
        write(s.gpuCrumbs[i].text);
        if(s.markerMap) { char gpu[64]{}; std::snprintf(gpu,sizeof(gpu),"gpu_completed_sequence=%u",s.markerMap[i]); write(gpu); }
    }
    CloseHandle(f);
    if(folder=="debug_state") return; // The device-loss snapshot already lives beside the latest state.
    std::snprintf(path,sizeof(path),"%.*s/latest_render_state.json",int(folder.size()),folder.data());
    // Keep crash-time copying independent of the C++ heap, just like the bundle's existing file writer.
    HANDLE input=CreateFileA("debug_state/latest_render_state.json",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(input==INVALID_HANDLE_VALUE) return;
    HANDLE output=CreateFileA(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(output!=INVALID_HANDLE_VALUE) {
        char block[4096]; DWORD count=0;
        while(ReadFile(input,block,sizeof(block),&count,nullptr) && count) {
            if(!WriteFile(output,block,count,&wrote,nullptr) || wrote!=count) break;
        }
        CloseHandle(output);
    }
    CloseHandle(input);
}
namespace {
// Recipes own the arrays referenced by Vulkan's temporary creation structures. Layouts/render passes belong to
// the original pipeline owner and remain alive until that owner removes this recipe.
struct Recipe {
    VkPipeline original=VK_NULL_HANDLE;
    bool isCompute=false, reloadable=true, retired=false;
    VkGraphicsPipelineCreateInfo g{}; VkComputePipelineCreateInfo c{};
    VkPipelineVertexInputStateCreateInfo vi{}; VkPipelineInputAssemblyStateCreateInfo ia{};
    VkPipelineTessellationStateCreateInfo tess{}; VkPipelineViewportStateCreateInfo vp{};
    VkPipelineRasterizationStateCreateInfo rs{}; VkPipelineMultisampleStateCreateInfo ms{};
    VkPipelineDepthStencilStateCreateInfo ds{}; VkPipelineColorBlendStateCreateInfo blend{};
    VkPipelineDynamicStateCreateInfo dyn{}; VkPipelineRenderingCreateInfo rendering{};
    std::vector<VkVertexInputBindingDescription> bindings; std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkViewport> viewports; std::vector<VkRect2D> scissors;
    std::vector<VkPipelineColorBlendAttachmentState> attachments; std::vector<VkDynamicState> dynamics;
    std::vector<VkFormat> formats; std::vector<VkSampleMask> sampleMasks;
    std::vector<VkPipelineShaderStageCreateInfo> stages; std::vector<Shader> shaders;
    uint64_t hash=0;
    void stage(VkPipelineShaderStageCreateInfo info) {
        if(info.pNext || info.pSpecializationInfo || !info.pName || std::strcmp(info.pName,"main")) reloadable=false;
        auto it=state().shaders.find(info.module);
        if(it==state().shaders.end()) { reloadable=false; shaders.emplace_back(); }
        else { const auto& [module,sh]=*it; shaders.push_back(sh); hash^=sh.hash; }
        info.pName="main"; info.pSpecializationInfo=nullptr; info.pNext=nullptr; stages.push_back(info);
    }
    void capture(const VkGraphicsPipelineCreateInfo& in) {
        g=in;
        for(uint32_t i=0;i<in.stageCount;++i) stage(in.pStages[i]);
        if(in.pNext) {
            const auto* base=static_cast<const VkBaseInStructure*>(in.pNext);
            if(base->sType==VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO && !base->pNext) {
                rendering=*static_cast<const VkPipelineRenderingCreateInfo*>(in.pNext);
                if(rendering.colorAttachmentCount) formats.assign(rendering.pColorAttachmentFormats,rendering.pColorAttachmentFormats+rendering.colorAttachmentCount);
            } else reloadable=false;
        }
        if(in.flags&VK_PIPELINE_CREATE_DERIVATIVE_BIT) reloadable=false;
        if(in.pVertexInputState) { vi=*in.pVertexInputState;
            if(vi.vertexBindingDescriptionCount) bindings.assign(vi.pVertexBindingDescriptions,vi.pVertexBindingDescriptions+vi.vertexBindingDescriptionCount);
            if(vi.vertexAttributeDescriptionCount) attributes.assign(vi.pVertexAttributeDescriptions,vi.pVertexAttributeDescriptions+vi.vertexAttributeDescriptionCount); }
        if(in.pInputAssemblyState) ia=*in.pInputAssemblyState;
        if(in.pTessellationState) tess=*in.pTessellationState;
        if(in.pViewportState) { vp=*in.pViewportState;
            if(vp.pViewports) viewports.assign(vp.pViewports,vp.pViewports+vp.viewportCount);
            if(vp.pScissors) scissors.assign(vp.pScissors,vp.pScissors+vp.scissorCount); }
        if(in.pRasterizationState) rs=*in.pRasterizationState;
        if(in.pMultisampleState) { ms=*in.pMultisampleState; if(ms.pSampleMask) sampleMasks.assign(ms.pSampleMask,ms.pSampleMask+(uint32_t(ms.rasterizationSamples)+31)/32); }
        if(in.pDepthStencilState) ds=*in.pDepthStencilState;
        if(in.pColorBlendState) { blend=*in.pColorBlendState; if(blend.attachmentCount) attachments.assign(blend.pAttachments,blend.pAttachments+blend.attachmentCount); }
        if(in.pDynamicState) { dyn=*in.pDynamicState; if(dyn.dynamicStateCount) dynamics.assign(dyn.pDynamicStates,dyn.pDynamicStates+dyn.dynamicStateCount); }
        if(vi.pNext || ia.pNext || tess.pNext || vp.pNext || rs.pNext || ms.pNext || ds.pNext || blend.pNext || dyn.pNext) reloadable=false;
    }
    void wire() {
        g.pStages=stages.data(); c.stage=stages.front();
        if(g.pVertexInputState) { vi.pVertexBindingDescriptions=bindings.data(); vi.pVertexAttributeDescriptions=attributes.data(); g.pVertexInputState=&vi; }
        if(g.pInputAssemblyState) g.pInputAssemblyState=&ia;
        if(g.pTessellationState) g.pTessellationState=&tess;
        if(g.pViewportState) { vp.pViewports=viewports.empty()?nullptr:viewports.data(); vp.pScissors=scissors.empty()?nullptr:scissors.data(); g.pViewportState=&vp; }
        if(g.pRasterizationState) g.pRasterizationState=&rs;
        if(g.pMultisampleState) { ms.pSampleMask=sampleMasks.empty()?nullptr:sampleMasks.data(); g.pMultisampleState=&ms; }
        if(g.pDepthStencilState) g.pDepthStencilState=&ds;
        if(g.pColorBlendState) { blend.pAttachments=attachments.data(); g.pColorBlendState=&blend; }
        if(g.pDynamicState) { dyn.pDynamicStates=dynamics.data(); g.pDynamicState=&dyn; }
        if(rendering.sType) { rendering.pColorAttachmentFormats=formats.data(); g.pNext=&rendering; }
    }
};
auto& recipes() { static auto* r=std::make_unique<std::unordered_map<uint64_t,std::unique_ptr<Recipe>>>().release(); return *r; }
auto& pipelineTokens() { static auto* r=std::make_unique<std::unordered_map<VkPipeline,uint64_t>>().release(); return *r; }
uint64_t nextPipelineToken=0;
struct RetiredPipeline { VkDevice device; VkPipeline handle; ~RetiredPipeline() { if(handle) vkDestroyPipeline(device,handle,nullptr); }
    RetiredPipeline(VkDevice d,VkPipeline p):device(d),handle(p){} RetiredPipeline(RetiredPipeline&& o) noexcept:device(o.device),handle(std::exchange(o.handle,VK_NULL_HANDLE)){} };
void reload() {
#ifndef FINAL
    std::ifstream request("shader_reload.request"); if(!request) return;
    std::string action,path,candidate,oldHash,newHash; std::getline(request,action); std::getline(request,path); std::getline(request,candidate); std::getline(request,oldHash); std::getline(request,newHash); request.close();
    std::error_code fileError; std::filesystem::remove("shader_reload.request",fileError);
    auto fail=[&](std::string_view reason) { std::fprintf(stderr,"[VK_SHADER_RELOAD v1] failure_keeps_old reason=%.*s shader=%s\n",int(reason.size()),reason.data(),path.c_str()); std::fflush(stderr); };
    if(action!="validated-v1") { fail(candidate.empty()?path.c_str():candidate.c_str()); return; }
    // The compile/reflection command supplies a content-bound request. Reject a stale request, even if the file name matches.
    uint64_t expectedOld=0,expectedNew=0;
    try { expectedOld=std::stoull(oldHash,nullptr,16); expectedNew=std::stoull(newHash,nullptr,16); } catch(...) { fail("malformed_request"); return; }
    std::ifstream in(candidate,std::ios::binary|std::ios::ate); if(!in || in.tellg()<20 || in.tellg()>16*MiB || in.tellg()%4!=0) { fail("invalid_candidate"); return; }
    std::vector<uint32_t> code(size_t(in.tellg())/4); in.seekg(0); in.read(reinterpret_cast<char*>(code.data()),code.size()*4);
    if(!in || code[0]!=0x07230203 || hashBytes(code.data(),code.size()*4)!=expectedNew) { fail("candidate_hash"); return; }
    in.close(); // Windows CRT file handles must close before atomic replacement.
    auto& s=state(); std::lock_guard lock(s.mutex);
    struct Pending { Recipe* recipe; VkPipeline pipeline; std::vector<Shader> shaders; };
    std::vector<Pending> pending; bool ok=true; size_t affected=0;
    for(auto& [handle,r]:recipes()) {
        if(r->retired) continue;
        bool match=false; for(auto& shader:r->shaders) if(shader.path==path) { match=true; if(shader.hash!=expectedOld) ok=false; }
        if(!match) continue; ++affected;
        if(!r->reloadable || !ok) { ok=false; break; }
        auto updated=r->shaders; std::vector<VkShaderModule> modules; r->wire();
        for(size_t i=0;i<updated.size();++i) {
            if(updated[i].path==path) { updated[i].code=code; updated[i].hash=expectedNew; }
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize=updated[i].code.size()*4; ci.pCode=updated[i].code.data(); VkShaderModule m=VK_NULL_HANDLE;
            if(vkCreateShaderModule(s.dev,&ci,nullptr,&m)!=VK_SUCCESS) { ok=false; break; }
            modules.push_back(m); r->stages[i].module=m;
        }
        r->wire(); VkPipeline replacement=VK_NULL_HANDLE;
        if(ok) ok=(r->isCompute?vkCreateComputePipelines(s.dev,s.cache,1,&r->c,nullptr,&replacement):vkCreateGraphicsPipelines(s.dev,s.cache,1,&r->g,nullptr,&replacement))==VK_SUCCESS;
        for(auto m:modules) vkDestroyShaderModule(s.dev,m,nullptr);
        if(replacement) pending.push_back({r.get(),replacement,std::move(updated)});
        if(!ok) break;
    }
    if(!ok || !affected) { for(auto& p:pending) vkDestroyPipeline(s.dev,p.pipeline,nullptr); fail(!affected?"no_live_pipeline":"stale_ABI_or_pipeline_creation"); return; }
    std::filesystem::rename(candidate,path,fileError);
    if(fileError) {
        for(auto& p:pending) vkDestroyPipeline(s.dev,p.pipeline,nullptr);
        fail("shader_file_publish"); return;
    }
    s.overrides[path]=Shader{path,code,expectedNew};
    breadcrumb("shader",path,expectedNew);
    // All stages of every affected pipeline succeeded. Publish together at the render-thread frame boundary.
    for(auto& p:pending) {
        const VkPipeline old=p.recipe->original;
        const uint64_t token=pipelineTokens().at(old); pipelineTokens().erase(old); pipelineTokens()[p.pipeline]=token;
        getOrCreateEngineVulkanContext().retire(RetiredPipeline(s.dev,old));
        p.recipe->original=p.pipeline; p.recipe->shaders=std::move(p.shaders); p.recipe->hash=0;
        for(auto& shader:p.recipe->shaders) p.recipe->hash^=shader.hash;
        breadcrumb("pipeline","reloaded",p.recipe->hash);
    }
    s.dirty=true;
    std::fprintf(stderr,"[VK_SHADER_RELOAD v1] success pipelines=%zu shader=%s old=%016llx new=%016llx retirement=graphics_timeline\n",pending.size(),path.c_str(),expectedOld,expectedNew); std::fflush(stderr);
#endif
}
// Some owners keep their original shader modules for later pipeline variants. Those variants must use the latest reload too.
struct LatestStages {
    std::vector<VkShaderModule> temporary;
    VkResult update(VkPipelineShaderStageCreateInfo& stage) {
        auto& s=state(); const auto found=s.shaders.find(stage.module);
        if(found==s.shaders.end()) return VK_SUCCESS;
        const auto& [originalModule,original]=*found;
        const auto latest=s.overrides.find(original.path);
        if(latest==s.overrides.end()) return VK_SUCCESS;
        const auto& [path,updated]=*latest;
        if(updated.hash==original.hash) return VK_SUCCESS;
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize=updated.code.size()*4; ci.pCode=updated.code.data();
        VkShaderModule module=VK_NULL_HANDLE; const auto result=vkCreateShaderModule(s.dev,&ci,nullptr,&module);
        if(result!=VK_SUCCESS) return result;
        temporary.push_back(module); s.shaders[module]=updated; stage.module=module; return VK_SUCCESS;
    }
    ~LatestStages() { auto& s=state(); for(auto module:temporary) { s.shaders.erase(module); vkDestroyShaderModule(s.dev,module,nullptr); } }
};
}
void shader(VkShaderModule module,std::string_view path,std::span<const uint32_t> code) {
    auto& s=state(); std::lock_guard lock(s.mutex); Shader sh{std::string(path),std::vector<uint32_t>(code.begin(),code.end()),hashBytes(code.data(),code.size_bytes())};
    breadcrumb("shader",path,sh.hash); s.shaders[module]=std::move(sh);
}
void forgetShader(VkShaderModule module) {
    auto& s=state(); std::lock_guard lock(s.mutex); s.shaders.erase(module);
}
VkResult graphics(VkDevice dev,VkPipelineCache,uint32_t n,const VkGraphicsPipelineCreateInfo* info,const VkAllocationCallbacks* allocator,VkPipeline* out) {
    auto& s=state(); std::lock_guard lock(s.mutex); const auto begin=std::chrono::steady_clock::now();
    std::vector<VkGraphicsPipelineCreateInfo> updated(info,info+n);
    std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stages(n); LatestStages latest;
    for(uint32_t i=0;i<n;++i) {
        stages[i].assign(info[i].pStages,info[i].pStages+info[i].stageCount);
        for(auto& stage:stages[i]) if(const auto result=latest.update(stage); result!=VK_SUCCESS) { std::fill(out,out+n,VK_NULL_HANDLE); return result; }
        updated[i].pStages=stages[i].data();
    }
    info=updated.data();
    const VkResult result=vkCreateGraphicsPipelines(dev,s.cache,n,info,allocator,out);
    if(result!=VK_SUCCESS) { for(uint32_t i=0;i<n;++i) if(out[i]) { vkDestroyPipeline(dev,out[i],allocator); out[i]=VK_NULL_HANDLE; } }
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count(); s.creationMs+=ms; s.creations+=n; s.dirty=true;
    for(uint32_t i=0;i<n;++i) if(out[i]) { auto r=std::make_unique<Recipe>(); r->original=out[i]; r->capture(info[i]); breadcrumb("pipeline","graphics",r->hash); const uint64_t token=++nextPipelineToken; pipelineTokens()[out[i]]=token; recipes()[token]=std::move(r); }
    std::fprintf(stderr,"[VK_PIPELINE_CREATE v1] kind=graphics count=%u ms=%.3f frame=%llu\n",n,ms,getOrCreateEngineVulkanContext().tracker().frameNo());
    return result;
}
VkResult compute(VkDevice dev,VkPipelineCache,uint32_t n,const VkComputePipelineCreateInfo* info,const VkAllocationCallbacks* allocator,VkPipeline* out) {
    auto& s=state(); std::lock_guard lock(s.mutex); const auto begin=std::chrono::steady_clock::now();
    std::vector<VkComputePipelineCreateInfo> updated(info,info+n); LatestStages latest;
    for(auto& item:updated) if(const auto result=latest.update(item.stage); result!=VK_SUCCESS) { std::fill(out,out+n,VK_NULL_HANDLE); return result; }
    info=updated.data();
    const VkResult result=vkCreateComputePipelines(dev,s.cache,n,info,allocator,out);
    if(result!=VK_SUCCESS) { for(uint32_t i=0;i<n;++i) if(out[i]) { vkDestroyPipeline(dev,out[i],allocator); out[i]=VK_NULL_HANDLE; } }
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count(); s.creationMs+=ms; s.creations+=n; s.dirty=true;
    for(uint32_t i=0;i<n;++i) if(out[i]) { auto r=std::make_unique<Recipe>(); r->original=out[i]; r->isCompute=true; r->c=info[i]; r->stage(info[i].stage); if(info[i].pNext) r->reloadable=false; breadcrumb("pipeline","compute",r->hash); const uint64_t token=++nextPipelineToken; pipelineTokens()[out[i]]=token; recipes()[token]=std::move(r); }
    std::fprintf(stderr,"[VK_PIPELINE_CREATE v1] kind=compute count=%u ms=%.3f frame=%llu\n",n,ms,getOrCreateEngineVulkanContext().tracker().frameNo()); return result;
}
void bind(VkCommandBuffer cb,VkPipelineBindPoint point,VkPipeline pipeline) {
    auto& s=state(); std::lock_guard lock(s.mutex); auto it=pipelineTokens().find(pipeline);
    if(it!=pipelineTokens().end()) { const auto& [handle,token]=*it; breadcrumb("bind","pipeline",recipes().at(token)->hash); }
    vkCmdBindPipeline(cb,point,pipeline);
}
uint64_t adoptPipeline(VkDevice,VkPipeline pipeline) {
    auto& s=state(); std::lock_guard lock(s.mutex); auto it=pipelineTokens().find(pipeline);
    if(it!=pipelineTokens().end()) { const auto& [handle,token]=*it; return token; }
    auto r=std::make_unique<Recipe>(); r->original=pipeline; r->reloadable=false;
    const uint64_t token=++nextPipelineToken; pipelineTokens()[pipeline]=token; recipes()[token]=std::move(r); return token;
}
VkPipeline currentPipeline(uint64_t token) {
    auto& s=state(); std::lock_guard lock(s.mutex); auto it=recipes().find(token);
    if(it==recipes().end()) return VK_NULL_HANDLE;
    const auto& [key,recipe]=*it; return recipe->original;
}
void destroyOwnedPipeline(uint64_t token) {
    auto& s=state(); std::lock_guard lock(s.mutex); auto it=recipes().find(token); if(it==recipes().end()) return;
    const auto& [key,recipe]=*it; const auto pipeline=recipe->original;
    pipelineTokens().erase(pipeline); recipes().erase(it); vkDestroyPipeline(s.dev,pipeline,nullptr);
}
void deferPipeline(uint64_t token) {
    auto& s=state(); std::lock_guard lock(s.mutex); auto it=recipes().find(token);
    // Retired pipeline owners can outlive their creation layouts; they are no longer reload candidates.
    if(it!=recipes().end()) { auto& [key,recipe]=*it; recipe->retired=true; }
}
void frame() {
    auto& s=state(); if(!s.dev) return; reload();
    if(s.measureFrames) {
        const auto now=std::chrono::steady_clock::now();
        if(s.firstFrameStarted) s.firstFrames.push_back(std::chrono::duration<double,std::milli>(now-s.previousFrame).count());
        s.firstFrameStarted=true; s.previousFrame=now;
        if(s.firstFrames.size()==120) {
            std::sort(s.firstFrames.begin(),s.firstFrames.end());
            std::fprintf(stderr,"[VK_FIRST_FRAMES v1] count=120 p95_ms=%.3f max_ms=%.3f late_pipeline_creations=%llu\n",s.firstFrames[113],s.firstFrames.back(),s.creations-s.prewarmCreations); s.measureFrames=false;
        }
    }
    const uint64_t f=getOrCreateEngineVulkanContext().tracker().frameNo();
    if(f%120==0) { const auto json=ledgerJson(); std::fprintf(stderr,"[VK_RESIDENCY v1] %s\n",json.c_str()); }
    if(std::chrono::steady_clock::now()-s.saved>std::chrono::seconds(30)) saveCache();
}
void prewarm(int width,int height) {
    if(width<=0 || height<=0 || !getOrCreateEngineVulkanContext().valid()) return;
    auto& s=state(); const auto begin=std::chrono::steady_clock::now(); const uint64_t before=s.creations;
    int ready=0;
    ready+=mc2_vk_terrain_island_prepare(width,height);
    ready+=mc2_vk_color_prop_island_prewarm(width,height);
    ready+=mc2_vk_mech_island_prewarm(width,height);
    ready+=mc2_vk_legacy_shape_island_prewarm(width,height);
    ready+=mc2_vk_sky_island_prewarm(width,height);
    ready+=mc2_vk_water_island_prewarm(width,height);
    ready+=mc2_vk_vfx_island_prewarm(width,height);
    ready+=mc2_vk_shadow_island_prewarm(width,height);
    mc2_vk_overlay_prewarm(width,height);
    if(auto* post=getGosPostProcess()) { post->prewarmVkFog(); post->prewarmVkScreenFx(); post->prewarmVkHzb(); }
    saveCache();
    s.prewarmCreations=s.creations; s.firstFrames.clear(); s.measureFrames=true; s.firstFrameStarted=false;
    std::fprintf(stderr,"[VK_PIPELINE_PREWARM v1] ready=%d families=8 pipelines=%llu ms=%.3f size=%dx%d\n",ready,s.creations-before,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count(),width,height);
}
void deviceLost() {
    auto& s=state();
    if(s.lossReported.exchange(true)) return;
    breadcrumb("device_lost","graphics_queue");
    if(s.checkpoints && vkGetQueueCheckpointDataNV) {
        std::array<VkCheckpointDataNV,64> checkpoints{};
        for(auto& checkpoint:checkpoints) checkpoint.sType=VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
        uint32_t count=uint32_t(checkpoints.size()); vkGetQueueCheckpointDataNV(getOrCreateEngineVulkanContext().graphicsQueue(),&count,checkpoints.data());
        for(uint32_t i=0;i<std::min<uint32_t>(count,uint32_t(checkpoints.size()));++i) {
            const auto ptr=reinterpret_cast<uintptr_t>(checkpoints[i].pCheckpointMarker);
            const auto begin=reinterpret_cast<uintptr_t>(s.gpuCrumbs.data()),end=begin+sizeof(s.gpuCrumbs);
            if(ptr>=begin && ptr<end && (ptr-begin)%sizeof(State::Crumb)==0) {
                char text[384]{}; std::memcpy(text,reinterpret_cast<State::Crumb*>(ptr)->text,sizeof(text));
                breadcrumb("gpu_checkpoint",text,uint64_t(checkpoints[i].stage));
            }
        }
    }
    std::error_code ec; std::filesystem::create_directories("debug_state",ec); crashSnapshot("debug_state");
    std::fprintf(stderr,"[VK_BREADCRUMBS v1] device_lost snapshot=debug_state/gpu_breadcrumbs.txt\n");
}
}
void mc2PrewarmMissionPipelines() {
    if(auto* post=getGosPostProcess()) mc2vk::infra::prewarm(post->getWidth(),post->getHeight());
}
#endif
