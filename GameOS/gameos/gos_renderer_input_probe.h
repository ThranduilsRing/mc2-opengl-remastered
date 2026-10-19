// Native pixel probes and CPU shadow-feed membership for command-channel diagnostics.
#pragma once
#include <cstdint>
#include <cstddef>
namespace gos_render_probe {
// The command channel runs before this frame's rendering, so the readback is
// from the preceding render. Coordinates retain the scene's bottom-left origin;
// the final native presentation flips Y after the scene has been composited.
struct PixelProbe {
    bool sceneAvailable = false;
    int sceneWidth = 0, sceneHeight = 0;
    int px = 0, py = 0;
    bool inBounds = false;
    unsigned char rgba[4] = {};
    unsigned long srcFrameHint = 0;
};
// Explicit diagnostic read; restores native attachment layouts on return.
bool readScenePixel(int px,int py,PixelProbe& out);
bool sceneImageSize(int* w,int* h);

//---------------------------------------------------------------------------
// Static-prop shadow feed membership.
//
// The two shadow feeds are the ACTUAL renderer inputs for prop shadows:
//   * GpuStaticPropRegistry::getBuildingShadowInstances() -- consumed ONCE per
//     mission (mclib/txmmgr.cpp:3097-3108, inside the one-shot
//     gos_BeginShadowPrePass block) into the world-fixed static shadow map.
//   * GpuStaticPropRegistry::getDynamicPropShadowInstances() -- rebuilt every
//     frame (mclib/txmmgr.cpp:3283) for the dynamic/CSM shadow map.
// BOTH read the registry's BAKED modelMatrix, never the object's live
// transform. That is the mechanism behind the leading black-quad hypothesis:
// a building that animates after mission load keeps casting from the pose the
// recipe was baked with. `submittedPos` below is that baked pose; the caller
// diffs it against the live object position and the delta IS the assert.
//---------------------------------------------------------------------------
struct ShadowFeedHit {
    bool     found        = false;
    int      leavesTotal  = 0;    // entries in this feed sharing the recipe typeID
    int      leavesExact  = 0;    // entries whose translation matches the recipe's
    float    submittedPos[3] = {0.0f, 0.0f, 0.0f};  // baked translation actually fed
    uint32_t typeID       = 0xFFFFFFFFu;
};

struct ShadowFeedReport {
    bool  registryQueried   = false;  // false => registry disabled; counts are meaningless
    bool  recipeResolved    = false;  // false => recipeIndex < 0 or tombstoned
    float recipePos[3]      = {0.0f, 0.0f, 0.0f};  // staticPropGetModelMatrix translation
    int   buildingFeedTotal = 0;
    int   dynamicFeedTotal  = 0;
    ShadowFeedHit inBuildingFeed;
    ShadowFeedHit inDynamicFeed;

    // Did the STATIC building shadow pass actually DRAW this mission?
    // gos_GetStaticBuildingShadowCounts is zero until the static building
    // shadow has drawn -- i.e. this is a behavioural receipt, not a config echo. Membership in a feed that never drew is not
    // a shadow; reporting membership without this would be a lying instrument.
    int  bldgDrawTypes = 0;
    int  bldgDrawInst  = 0;
    int  bldgDrawCalls = 0;
    bool modernShadowPathActive = false;  // gos_IsModernShadowPathActive()
};

// recipeIndex comes from Appearance::getStaticRecipeIndex(). A negative index
// is not an error: it means the object is NOT on the recipe path at all, and
// the report comes back with recipeResolved=false plus the feed totals, which
// is itself the answer to "why does this thing cast no prop shadow".
void queryShadowFeed(int32_t recipeIndex, ShadowFeedReport& out);

}  // namespace gos_render_probe
