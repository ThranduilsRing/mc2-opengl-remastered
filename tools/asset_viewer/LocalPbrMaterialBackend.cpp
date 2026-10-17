// tools/asset_viewer/LocalPbrMaterialBackend.cpp
// Cook-Torrance preview sphere. The shaders are GLSL 450 (shaders_vk/pbr_sphere.{vert,frag}), compiled to SPIR-V at build time.
#include "LocalPbrMaterialBackend.h"
#include "SphereMesh.h"
#include <cstdio>
#include <cstring>

namespace {
// std140, mirrors the UBO in pbr_sphere.vert / pbr_sphere.frag.
struct PbrUbo {
    float viewProj[16];
    float cameraPos[4];
    float lightDir[4];
    float lightColor[4];
    float ambient[4];
    int32_t has[4];   // x = normal, y = orm, z = emissive
};
static_assert(sizeof(PbrUbo) == 64 + 16 * 4 + 16, "PbrUbo std140 layout");
} // namespace

bool LocalPbrMaterialBackend::init() {
    toolvk::PipelineDesc d;
    d.vertSpv = "pbr_sphere.vert.spv";
    d.fragSpv = "pbr_sphere.frag.spv";
    d.stride = sizeof(SphereVertex);
    d.attrs = {{0, 106 /*R32G32B32_SFLOAT*/, 0}, {1, 106, 12}, {2, 109 /*R32G32B32A32_SFLOAT*/, 24}, {3, 103 /*R32G32_SFLOAT*/, 40}};
    d.samplers = 4;
    d.cullBack = true;
    return toolvk::createPipeline(pipe_, d);
}

void LocalPbrMaterialBackend::setMaterial(const MaterialSlotTextures& slots) { slots_ = slots; }

void LocalPbrMaterialBackend::render(const RenderInputs& in, toolvk::Pass& pass) {
    if (!pipe_.impl || !in.mesh) return;
    PbrUbo u{};
    std::memcpy(u.viewProj, in.viewProj, sizeof u.viewProj);
    std::memcpy(u.cameraPos, in.cameraPosWorld, 12);
    std::memcpy(u.lightDir, in.lightDirWorld, 12);
    std::memcpy(u.lightColor, in.lightColor, 12);
    std::memcpy(u.ambient, in.ambient, 12);
    u.has[0] = slots_.normal ? 1 : 0;
    u.has[1] = slots_.orm ? 1 : 0;
    u.has[2] = slots_.emissive ? 1 : 0;
    pass.bind(pipe_);
    // baseColor must exist for a meaningful preview; id 0 samples the white fallback texel.
    pass.draw(&u, sizeof u, {slots_.baseColor, slots_.normal, slots_.orm, slots_.emissive},
              in.mesh->vertexBuffer(), in.mesh->indexBuffer(), in.mesh->indexCount());
}

void LocalPbrMaterialBackend::shutdown() { toolvk::destroyPipeline(pipe_); }
