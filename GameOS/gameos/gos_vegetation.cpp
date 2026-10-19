// CPU vegetation instances and native Vulkan crossed-card submission.
#include "gos_vegetation.h"
#include "vulkan_vfx_geometry.h"
#include "texture_decode.h"
#include "gos_postprocess.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "mc2fmt.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern const float* gos_GetTerrainMVPMat4();
namespace {
std::vector<GosVegetation::Instance> instances;
std::vector<uint32_t> visibility(1024, 2u);
bool initialized = false, atlasReady = false, summaryEmitted = false;
uint32_t peak = 0;
int frameDraws = 0, sessionDraws = 0;
const char* reason = "no_call";
const char* atlasOverride = nullptr;
float smooth(float a, float b, float x) {
    float t = std::clamp((x-a)/(b-a), 0.0f, 1.0f);
    return t*t*(3.0f-2.0f*t);
}
}
bool GosVegetation::isEnabled() { return mc2::gate::get<"MC2_VEGETATION_CARDS">(); }
uint32_t GosVegetation::instanceCount() { return static_cast<uint32_t>(instances.size()); }
void GosVegetation::setAtlasPath(const char* path) { atlasOverride = path; }
void GosVegetation::init() {
    if (initialized) return;
    const auto envPath = mc2::gate::str<"MC2_VEGETATION_ATLAS">();
    const char* path = !envPath.empty() ? envPath.c_str() : atlasOverride ? atlasOverride : "data/textures/vegetation_atlas_v2.png";
    const auto decoded = mc2tex::decodeFile(path, gos_Texture_Alpha, 0);
    if (decoded) {
        GosTexels atlas;
        atlas.status = GosTexels::Status::Ready;
        atlas.width = decoded->width;
        atlas.height = decoded->height;
        atlas.vkFormat = mc2tex::kVkFormatRgba8Unorm;
        // decodeFile may return RGB8; publish the same RGBA alpha policy as ordinary textures.
        auto bytes = std::make_shared<std::vector<uint8_t>>();
        if (decoded->layout == mc2tex::PixelLayout::RGB8) {
            bytes->reserve(size_t(atlas.width)*atlas.height*4);
            for (size_t i=0; i<decoded->pixels.size(); i+=3) {
                bytes->insert(bytes->end(), decoded->pixels.begin()+i, decoded->pixels.begin()+i+3);
                bytes->push_back(255);
            }
        } else *bytes = decoded->pixels;
        atlas.bytes = bytes;
        mc2_vk_vfx_vegetation_atlas(atlas);
        atlasReady = true;
    }
    std::atexit([] { GosVegetation::emitSummary(); });
    initialized = true;
}
void GosVegetation::shutdown() {
    emitSummary();
    instances.clear();
    visibility.assign(1024, 2u);
    initialized = atlasReady = summaryEmitted = false;
    peak = 0;
    frameDraws = sessionDraws = 0;
    reason = "no_call";
    mc2_vk_vfx_vegetation_atlas(GosTexels{});
}
void GosVegetation::uploadInstances(const Instance* data, uint32_t count) {
    if (!initialized) return;
    instances.clear();
    if (data && count) instances.assign(data, data+count);
    peak = std::max(peak, static_cast<uint32_t>(instances.size()));
}
void GosVegetation::setBlockVisibility(const uint32_t* data, uint32_t count) {
    if (data && count) visibility.assign(data, data+count);
}
void GosVegetation::flush(float lx, float ly, float lz, float lw, float time,
    float cx, float cy, float cz, float halfMap, float blockSide, int chunkSide) {
    frameDraws = 0;
    if (!isEnabled()) { reason="disabled"; return; }
    if (instances.empty()) { reason="no_instances"; return; }
    if (!atlasReady) { reason="no_atlas"; return; }
    const auto* mvp = gos_GetTerrainMVPMat4();
    auto* pp = getGosPostProcess();
    if (!mvp || !pp) { reason="missing_buffers"; return; }
    const bool force = mc2::gate::get<"MC2_VEG_DEBUG_FORCE_VISIBLE">();
    const float maxDist = mc2::gate::get<"MC2_VEG_MAX_DIST">();
    const float fadeStart = maxDist*0.5f;
    std::vector<VfxGeometryVertex> vertices;
    for (const auto& i : instances) {
        uint32_t vis=2;
        if (chunkSide>0 && blockSide>0) {
            int col=std::clamp(int((i.x+halfMap)/blockSide),0,chunkSide-1);
            int row=std::clamp(int((-i.y+halfMap)/blockSide),0,chunkSide-1);
            size_t block=size_t(row)*chunkSide+col;
            if (block<visibility.size()) vis=visibility[block];
        }
        float dx=i.x-cx,dy=i.y-cy,dz=i.z-cz;
        float dist2=std::sqrt(dx*dx+dy*dy),dist3=std::sqrt(dx*dx+dy*dy+dz*dz);
        if (!force && (vis==0 || i.seed<smooth(fadeStart,maxDist,dist2))) continue;
        VfxGeometryVertex v{};
        v.position[0]=i.x; v.position[1]=i.y; v.position[2]=i.z;
        v.pad0=i.yaw;
        v.color[0]=i.scale; v.color[1]=i.seed; v.color[2]=std::max(lz,0.0f);
        v.flags=i.atlasFrame;
        v.extra[0]=time; v.extra[1]=dist3;
        v.extra[2]=force || vis>=2 ? 1.0f : 0.4f;
        v.extra[3]=1.0f-smooth(fadeStart*0.8f,fadeStart,dist3);
        vertices.push_back(v);
    }
    VfxParticleDraw draw{};
    draw.count=static_cast<uint32_t>(vertices.size());
    draw.texture=kVfxVegetationAtlas;
    draw.atlasColumns=kVfxVegetationAtlas;
#ifdef MC2_VULKAN_ISLAND
    if (!vertices.empty() && mc2_vk_vfx_island_run(vertices.data(), draw.count, &draw, 1, mvp,
        nullptr,nullptr,pp->getWidth(),pp->getHeight())) {
        frameDraws=1; ++sessionDraws; reason="submitted";
    } else reason="culled";
#else
    reason="native_unavailable";
#endif
}
GosVegetation::VegFrameStats GosVegetation::getFrameStats() {
    return {static_cast<int>(instances.size()),frameDraws,reason};
}
void GosVegetation::emitSummary() {
    if (!isEnabled() || summaryEmitted) return;
    mc2::fmt::print_err("VEG_SUMMARY instance_count={:d} draw_calls={:d} flush_reason={}\n",
        mc2::fmt::as_u32(peak),mc2::fmt::as_i32(sessionDraws),reason);
    std::fflush(stderr); summaryEmitted=true;
}
