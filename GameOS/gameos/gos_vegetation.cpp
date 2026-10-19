// CPU vegetation instances and native Vulkan crossed-card submission.
#include "gos_vegetation.h"
#include "vulkan_vfx_geometry.h"
#include "texture_decode.h"
#include "gos_postprocess.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "mc2fmt.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern const float* gos_GetTerrainMVPMat4();
namespace {
std::vector<GosVegetation::Instance> instances;
struct Region { size_t begin, end; float x, y, z, radius; };
std::vector<Region> regions;
auto cell(const GosVegetation::Instance& i) {
    return std::pair{int(std::floor(i.x/128.0f)),int(std::floor(i.y/128.0f))};
}
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

// The old private vegetation_atlas_v2.png was never part of the payload. Build
// its eight shader slots from the tracked cards so a clean install is complete.
std::optional<mc2tex::DecodedTexture> builtinAtlas() {
    static constexpr std::array<const char*, 8> paths{
        "data/vegetation/cards/grass_01.png", "data/vegetation/cards/grass_02.png",
        "data/vegetation/cards/grass_03.png", "data/vegetation/cards/grass_04.png",
        "data/vegetation/cards/fern_01.png", "data/vegetation/cards/fern_02.png",
        "data/vegetation/cards/fern_03.png", "data/vegetation/cards/fern_04.png"};
    mc2tex::DecodedTexture atlas;
    atlas.layout = mc2tex::PixelLayout::RGBA8;
    atlas.format = gos_Texture_Alpha;
    int cellWidth = 0, cellHeight = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        const auto tile = mc2tex::decodeFile(paths[i], gos_Texture_Alpha, 0);
        if (!tile || tile->layout != mc2tex::PixelLayout::RGBA8) return std::nullopt;
        if (i == 0) {
            cellWidth = tile->width; cellHeight = tile->height;
            atlas.width = cellWidth * 4; atlas.height = cellHeight * 2;
            atlas.pixels.resize(size_t(atlas.width) * atlas.height * 4);
        }
        if (tile->width != cellWidth || tile->height != cellHeight) return std::nullopt;
        for (int y = 0; y < cellHeight; ++y) {
            const size_t dst = ((i / 4 * cellHeight + y) * atlas.width + i % 4 * cellWidth) * 4;
            std::copy_n(tile->pixels.data() + size_t(y) * cellWidth * 4, size_t(cellWidth) * 4,
                        atlas.pixels.data() + dst);
        }
    }
    return atlas;
}
}
bool GosVegetation::isEnabled() { return mc2::gate::get<"MC2_VEGETATION_CARDS">(); }
uint32_t GosVegetation::instanceCount() { return static_cast<uint32_t>(instances.size()); }
void GosVegetation::setAtlasPath(const char* path) { atlasOverride = path; }
void GosVegetation::init() {
    if (initialized) return;
    const auto envPath = mc2::gate::str<"MC2_VEGETATION_ATLAS">();
    const char* path = !envPath.empty() ? envPath.c_str() : atlasOverride ? atlasOverride : "data/textures/vegetation_atlas_v2.png";
    const auto decoded = (!envPath.empty() || atlasOverride)
        ? mc2tex::decodeFile(path, gos_Texture_Alpha, 0) : builtinAtlas();
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
    regions.clear();
    visibility.assign(1024, 2u);
    initialized = atlasReady = false;
    peak = 0;
    frameDraws = sessionDraws = 0;
    reason = "no_call";
    mc2_vk_vfx_vegetation_atlas(GosTexels{});
}
void GosVegetation::uploadInstances(const Instance* data, uint32_t count) {
    if (!initialized) return;
    instances.clear();
    if (data && count) instances.assign(data, data+count);
    // Placement is immutable between uploads. Group nearby clumps once so the
    // frame walk rejects regions instead of revisiting the whole map.
    std::sort(instances.begin(),instances.end(),[](const auto& a,const auto& b) { return cell(a)<cell(b); });
    regions.clear();
    for (size_t begin=0; begin<instances.size();) {
        size_t end=begin+1;
        const auto key=cell(instances[begin]);
        while (end<instances.size() && cell(instances[end])==key) ++end;
        float lo=instances[begin].z,hi=lo,pad=0.0f;
        for (size_t j=begin;j<end;++j) {
            lo=std::min(lo,instances[j].z); hi=std::max(hi,instances[j].z);
            pad=std::max(pad,1.5f*instances[j].scale);
        }
        regions.push_back({begin,end,(key.first+0.5f)*128.0f,(key.second+0.5f)*128.0f,
            (lo+hi)*0.5f,std::sqrt(2.0f*64.0f*64.0f+(hi-lo)*(hi-lo)*0.25f)+pad});
        begin=end;
    }
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
    static thread_local std::vector<VfxGeometryVertex> vertices;
    vertices.clear();
    // Conservative clip-space sphere planes reject whole clumps before upload.
    std::array<std::array<float, 5>, 4> planes{};
    for (int plane=0; plane<4; ++plane) {
        const int axis=plane/2;
        const float sign=(plane&1) ? -1.0f : 1.0f;
        for (int c=0; c<4; ++c) planes[plane][c]=mvp[c*4+3]+sign*mvp[c*4+axis];
        const auto& p=planes[plane];
        planes[plane][4]=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
    }
    const float pixelScale=0.5f*pp->getHeight()*std::sqrt(mvp[1]*mvp[1]+mvp[5]*mvp[5]+mvp[9]*mvp[9]);
    for (const auto& region : regions) {
        if (!force) {
            bool outside=false;
            for (const auto& p : planes)
                if (p[0]*region.x+p[1]*region.y+p[2]*region.z+p[3] < -region.radius*p[4]) { outside=true; break; }
            const float dx=region.x-cx,dy=region.y-cy,range=maxDist+region.radius;
            if (outside || dx*dx+dy*dy>range*range) continue;
        }
        for (size_t index=region.begin;index<region.end;++index) {
        const auto& i=instances[index];
        const uint32_t role=(i.atlasFrame>>4u)&3u;
        const float scale=i.scale*(role==3u ? 0.25f : role==2u ? 0.6f : 1.0f);
        if (!force) {
            bool outside=false;
            for (const auto& p : planes)
                if (p[0]*i.x+p[1]*i.y+p[2]*i.z+p[3] < -1.5f*scale*p[4]) { outside=true; break; }
            if (outside) continue;
        }
        uint32_t vis=2;
        if (chunkSide>0 && blockSide>0) {
            int col=std::clamp(int((i.x+halfMap)/blockSide),0,chunkSide-1);
            int row=std::clamp(int((-i.y+halfMap)/blockSide),0,chunkSide-1);
            size_t block=size_t(row)*chunkSide+col;
            if (block<visibility.size()) vis=visibility[block];
        }
        float dx=i.x-cx,dy=i.y-cy,dz=i.z-cz;
        const float distSq=dx*dx+dy*dy;
        if (!force && distSq>maxDist*maxDist && i.seed<1.0f) continue;
        float dist2=std::sqrt(distSq),dist3=std::sqrt(distSq+dz*dz);
        if (!force && (vis==0 || i.seed<smooth(fadeStart,maxDist,dist2))) continue;
        if (!force) {
            const float clipW=mvp[3]*i.x+mvp[7]*i.y+mvp[11]*i.z+mvp[15];
            if (clipW<=0.001f) continue;
            const float pixels=scale*pixelScale/clipW;
            const float density=smooth(3.0f,12.0f,pixels)*(vis>=2 ? 1.0f : 0.4f);
            const float seed=i.seed*17.371f+0.137f;
            if (seed-std::floor(seed)>=density) continue;
        }
        VfxGeometryVertex v{};
        v.position[0]=i.x; v.position[1]=i.y; v.position[2]=i.z;
        v.pad0=i.yaw;
        v.color[0]=i.scale; v.color[1]=i.seed; v.color[2]=std::max(lz,0.0f);
        v.flags=i.atlasFrame;
        v.extra[0]=time; v.extra[1]=dist3;
        v.extra[2]=1.0f; // whole-clump LOD replaces screen-space Bayer thinning
        v.extra[3]=1.0f-smooth(fadeStart*0.8f,fadeStart,dist3);
        vertices.push_back(v);
        }
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
