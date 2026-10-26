#include "WorkbenchValidation.h"
#include <cmath>
#include <string_view>
std::vector<Warning> ValidateSemantics(const WorkbenchOverride& rec, const SemanticInputs& in){
    std::vector<Warning> w; auto warn=[&](std::string_view c,std::string_view m){ w.push_back({WarnSeverity::Warn,std::string(c),std::string(m)}); };
    if (in.stockMesh && in.stockMesh->ok){
        if (in.maxFootprintRatio>1.5f || in.maxFootprintRatio<0.67f)
            warn("bounds-delta","override footprint "+std::to_string(in.maxFootprintRatio)+"x stock — verify scale (runtime forces 1.0)");
        if (in.pivotOffsetXZ>0.25f) warn("pivot-xz","override off-center vs stock by "+std::to_string(in.pivotOffsetXZ));
        if (std::fabs(in.pivotOffsetY)>0.25f) warn("pivot-y","override base vertically offset by "+std::to_string(in.pivotOffsetY));
    } else warn("no-stock","no stock bound — bounds/pivot not validated");
    for (const auto& t: in.missingTextures) warn("texture-missing","referenced texture not found: "+t);
    bool alpha=false;
    if (in.overrideMesh) for (const auto& sm: in.overrideMesh->submeshes)
        if (sm.textureName.rfind("a_",0)==0 || sm.textureName.find("_a_")!=std::string::npos) alpha=true;
    if (alpha && !in.hasImpostorLod) warn("overdraw","alpha-card override with no far-LOD impostor — likely high overdraw in-game (heuristic)");
    // S5 LOD authoring guidance (WARN, never BLOCK).
    if (rec.lods.empty()) {
        warn("lod0-only", "only LOD0 present — no impostor/far LODs (acceptable, but no distance falloff)");
    } else if (rec.lods.front().distance != 0.0f) {
        warn("lod-first-distance", "first LOD entry distance should be 0 (LOD0 baseline); got "
             + std::to_string(rec.lods.front().distance));
    }
    return w;
}
