// GameOS/gameos/gos_renderer_input_probe.cpp
//
// RENDERER-INPUT-READBACK-1. See gos_renderer_input_probe.h for the contract.

#include "gos_renderer_input_probe.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <GL/glew.h>

#include "gos_postprocess.h"
#include "gos_static_prop_batcher.h"
#include "gos_static_prop_registry.h"
#include "gameos.hpp"  // gos_IsModernShadowPathActive

extern uint32_t g_mc2FrameCounter;

namespace gos_render_probe {

//---------------------------------------------------------------------------
bool sceneFboSize(int* w, int* h)
{
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->getSceneFBO())
        return false;
    const int fw = pp->getWidth();
    const int fh = pp->getHeight();
    if (fw <= 0 || fh <= 0)
        return false;
    if (w) *w = fw;
    if (h) *h = fh;
    return true;
}

//---------------------------------------------------------------------------
bool readScenePixel(int px, int py, PixelProbe& out)
{
    out = PixelProbe();
    // frame N is being BUILT; the scene FBO still holds frame N-1's render.
    out.srcFrameHint = (g_mc2FrameCounter > 0)
        ? (unsigned long)(g_mc2FrameCounter - 1u)
        : 0UL;

    gosPostProcess* pp = getGosPostProcess();
    if (!pp)
        return false;
    const GLuint fbo = pp->getSceneFBO();
    const int w = pp->getWidth();
    const int h = pp->getHeight();
    if (!fbo || w <= 0 || h <= 0)
        return false;

    out.fboAvailable = true;
    out.fboWidth = w;
    out.fboHeight = h;
    out.px = px;
    out.py = py;
    if (px < 0 || py < 0 || px >= w || py >= h)
        return false;   // inBounds stays false; rgba stays zero

    // Same shape as the MC2_LOAD_RACE_TRACE scene oracle
    // (gameosmain.cpp:2383-2392): bind the scene FBO for READ explicitly so
    // the probe does not depend on whatever happened to be bound, then put
    // the binding back. GL_PACK_ALIGNMENT is forced to 1 for the same reason
    // gos_screenshot.cpp does it -- a 1x1 RGBA read is 4-byte aligned so it
    // would survive the default, but the restore-pair is cheap and keeps this
    // from becoming a latent shear bug if the format is ever widened.
    GLint prevRead = 0;
    GLint prevPack = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prevPack);

    // RAW-GL-FBO-BIND-GATE-1 disposition. This TU owns no FBO -- it borrows
    // gosPostProcess's scene FBO for one 1x1 glReadPixels. The bind below is
    // GL_READ_FRAMEBUFFER, so no draw can be retargeted by it, and
    // GL_READ_FRAMEBUFFER_BINDING is captured two lines above and restored
    // three lines below (the restore half is auto-exempt under the checker's
    // prev/saved rule; this acquire half is the one that needs the tag). Same
    // class as the two brackets already on the gate's file allowlist --
    // gameosmain.cpp "screenshot / video READ-blit" and RenderWorld.cpp
    // "readback" -- but tagged per-CALL-SITE instead of allowlisting the whole
    // file, so a GL_DRAW_FRAMEBUFFER bind added here later still fails the gate.
    // FBO-OWNER: read-only readback bracket over the scene FBO, restored below.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out.rgba);
    glPixelStorei(GL_PACK_ALIGNMENT, prevPack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);

    out.inBounds = true;
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
    gos_GetStaticBuildingShadowCounts(out.bldgDrawTypes, out.bldgDrawInst,
                                      out.bldgDrawCalls);

    if (!GpuStaticPropRegistry::isEnabled())
        return;   // registryQueried stays false -- totals would be a lie
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

    // includeBuildings=false mirrors the default the per-frame dynamic caster
    // draw uses when the static building map is active. When it is NOT active
    // the engine passes true (mclib/txmmgr.cpp:3283 `includeBldg`), so a
    // building can legitimately appear in the dynamic feed too; that is why
    // the caller is given the static-pass draw counts alongside.
    std::vector<GpuStaticPropInstance> dyn;
    GpuStaticPropRegistry::getDynamicPropShadowInstances(dyn, false);
    out.dynamicFeedTotal = (int)dyn.size();
    scanFeed(dyn, out.recipeResolved, recipeTypeId, out.recipePos,
             out.inDynamicFeed);
}

}  // namespace gos_render_probe
