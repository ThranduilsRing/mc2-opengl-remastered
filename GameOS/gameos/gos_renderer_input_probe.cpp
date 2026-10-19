// GameOS/gameos/gos_renderer_input_probe.cpp
//
// RENDERER-INPUT-READBACK-1. See gos_renderer_input_probe.h for the contract.

#include "gos_renderer_input_probe.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "vk_scene_readback.h"

#include "gos_postprocess.h"
#include "gos_static_prop_batcher.h"
#include "gos_static_prop_registry.h"
#include "gameos.hpp"  // gos_IsModernShadowPathActive

extern uint32_t g_mc2FrameCounter;

namespace gos_render_probe {

//---------------------------------------------------------------------------
bool sceneImageSize(int* w, int* h)
{
    int width=0, height=0;
    if (!mc2vk::sceneReadbackExtent(width,height,mc2vk::SceneColor)) return false;
    if (w) *w=width;
    if (h) *h=height;
    return true;
}

//---------------------------------------------------------------------------
bool readScenePixel(int px, int py, PixelProbe& out)
{
    out = PixelProbe();
    out.srcFrameHint = g_mc2FrameCounter > 0 ? (unsigned long)(g_mc2FrameCounter-1u) : 0UL;
    int w=0,h=0;
    if (!mc2vk::sceneReadbackExtent(w,h,mc2vk::SceneColor)) return false;
    out.sceneAvailable=true; out.sceneWidth=w; out.sceneHeight=h; out.px=px; out.py=py;
    if (px<0 || py<0 || px>=w || py>=h) return false;
    mc2vk::ScenePixel pixel;
    if (!mc2vk::readScenePixels(px,py,1,1,mc2vk::SceneColor,&pixel,1)) return false;
    std::memcpy(out.rgba,pixel.rgba,4);
    out.inBounds=true;
    return true;
}

//---------------------------------------------------------------------------
namespace {

// The registry feeds and staticPropGetModelMatrix both hand back the SAME
// baked floats, so an exact bitwise-equal translation test is the correct
// join key -- no epsilon, no "close enough" that could pair a prop with its
// neighbour. Kept as a named helper so that intent is not lost.
inline bool sameTranslation(const float a[3], const float* m /*row-major 16*/)
{
    return a[0] == m[3] && a[1] == m[7] && a[2] == m[11];
}

void scanFeed(const std::vector<GpuStaticPropInstance>& feed,
              bool haveRecipe, uint32_t recipeTypeId, const float recipePos[3],
              ShadowFeedHit& hit)
{
    hit = ShadowFeedHit();
    if (!haveRecipe)
        return;
    hit.typeID = recipeTypeId;
    for (size_t i = 0; i < feed.size(); ++i) {
        if (feed[i].typeID != recipeTypeId)
            continue;
        ++hit.leavesTotal;
        if (sameTranslation(recipePos, feed[i].modelMatrix)) {
            if (hit.leavesExact == 0) {
                hit.submittedPos[0] = feed[i].modelMatrix[3];
                hit.submittedPos[1] = feed[i].modelMatrix[7];
                hit.submittedPos[2] = feed[i].modelMatrix[11];
            }
            ++hit.leavesExact;
            hit.found = true;
        }
    }
    // DELIBERATELY NOT filled in when leavesExact == 0.
    //
    // An earlier draft, on a type-match with no exact translation match, fell
    // back to the first same-type entry's pose so submittedPos would "have a
    // value". That is the defect this whole family exists to remove: the
    // caller would then compute shadowDrift against an unrelated sibling
    // instance of the same building type and read a large, meaningless number
    // as evidence of a stale pose. leavesTotal > 0 with leavesExact == 0 is
    // already a complete answer -- "this type is being fed, but nothing is
    // being fed at THIS recipe's location" -- and submittedPos stays (0,0,0)
    // with found=false so no drift can be computed from it.
}

}  // namespace

void queryShadowFeed(int32_t recipeIndex, ShadowFeedReport& out)
{
    out = ShadowFeedReport();

    out.modernShadowPathActive = gos_IsModernShadowPathActive();

    out.registryQueried = true;

    uint32_t recipeTypeId = 0xFFFFFFFFu;
    float modelMatrix[16] = {0.0f};
    if (recipeIndex >= 0 &&
        GpuStaticPropRegistry::staticPropGetModelMatrix(recipeIndex, modelMatrix) &&
        GpuStaticPropRegistry::staticPropGetTypeId(recipeIndex, &recipeTypeId)) {
        out.recipeResolved = true;
        out.recipePos[0] = modelMatrix[3];
        out.recipePos[1] = modelMatrix[7];
        out.recipePos[2] = modelMatrix[11];
    }

    // Both feeds are rebuilt here rather than sampled from the renderer,
    // because the renderer's copies are local to their call sites. These call
    // the SAME registry entry points the passes call, so what comes back is
    // what the pass would be handed if it ran on this frame -- not a
    // reimplementation of the filter.
    std::vector<GpuStaticPropInstance> bldg;
    GpuStaticPropRegistry::getBuildingShadowInstances(bldg);
    out.buildingFeedTotal = (int)bldg.size();
    scanFeed(bldg, out.recipeResolved, recipeTypeId, out.recipePos,
             out.inBuildingFeed);

    // The dynamic caster feed never includes buildings: they cast through the
    // static building map, and the caller is given the static-pass draw counts
    // alongside to tell a drawn feed from an undrawn one.
    std::vector<GpuStaticPropInstance> dyn;
    GpuStaticPropRegistry::getDynamicPropShadowInstances(dyn);
    out.dynamicFeedTotal = (int)dyn.size();
    scanFeed(dyn, out.recipeResolved, recipeTypeId, out.recipePos,
             out.inDynamicFeed);
}

}  // namespace gos_render_probe
