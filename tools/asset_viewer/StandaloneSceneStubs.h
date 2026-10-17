// tools/asset_viewer/StandaloneSceneStubs.h
// Backend-A v2 SEAM: the scene-coupled inputs static_prop.{vert,frag} require but a
// standalone tool has no mission for. v3 replaces these with real mission feeders
// WITHOUT touching the render path. Provides: a 1-entry LightsData SSBO (binding 20:
// ambient + one infinite directional), minimal Instances/Colors/PerType SSBOs
// (bindings 0/1/2: one identity instance, lightDataIndex 0). A 1x1 white shadow tex
// is built but UNUSED in the minimal config (no shadow sampler) — future seam.
#pragma once

class StandaloneSceneStubs {
public:
    void build(const float lightDirWorld[3], const float lightColor[3], float ambient);
    void bind(const float modelMatrix[16]) const;   // binds SSBOs 0/1/2/20; re-uploads model
    void destroy();
    bool valid() const { return built_; }
private:
    unsigned instancesSsbo_ = 0, colorsSsbo_ = 0, perTypeSsbo_ = 0, lightsSsbo_ = 0;
    unsigned shadowTex_ = 0;
    bool built_ = false;
};
