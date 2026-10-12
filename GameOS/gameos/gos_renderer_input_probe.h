// GameOS/gameos/gos_renderer_input_probe.h
//
// RENDERER-INPUT-READBACK-1 -- engine-side half of the command-channel
// readback verbs (`shadow_feed`, `pixel`).
//
// WHY A SEPARATE TU AT ALL: both probes need things code/missiongui.cpp is
// forbidden to touch.
//   * the pixel probe calls glReadPixels. scripts/check-no-raw-gl-from-game.sh
//     forbids `gl[A-Z]*(` anywhere under code/ and mclib/ (allowlisted TUs
//     only), so the read MUST live engine-side.
//   * the shadow-feed probe needs gos_static_prop_registry.h +
//     gos_static_prop_batcher.h, which drag <GL/glew.h> and Stuff/Stuff.hpp
//     behind them.
// So the game side gets these two POD structs and two free functions, and
// nothing else crosses.
//
// Both entry points are READ-ONLY with respect to renderer state: no draw is
// issued, no registry record is mutated, and every GL binding the pixel probe
// changes is restored before it returns.

#pragma once

#include <cstdint>
#include <cstddef>

namespace gos_render_probe {

//---------------------------------------------------------------------------
// Scene-FBO pixel readback.
//
// PROVENANCE, READ THIS BEFORE BELIEVING A RESULT: the command channel is
// polled from MissionInterfaceManager::update(), which runs in the GAME-LOGIC
// half of the frame, before this frame's render. The scene FBO therefore holds
// the MOST RECENTLY COMPLETED render, i.e. frame (current - 1). `srcFrameHint`
// carries that number so a driver never has to guess, and the ack prints it.
// Do not compare a pixel sampled at frame N against a `screenshot` TGA
// scheduled at frame N -- the TGA is an END-of-frame capture and is one frame
// AHEAD of this probe. That off-by-one is the single most likely way this
// instrument could lie, so it is surfaced rather than hidden.
//---------------------------------------------------------------------------
struct PixelProbe {
    bool          fboAvailable = false;  // false => post-process/scene FBO not up
    int           fboWidth     = 0;
    int           fboHeight    = 0;
    int           px           = 0;      // sampled texel, FBO space, origin BOTTOM-left
    int           py           = 0;
    bool          inBounds     = false;  // false => nothing was read, rgba is zero
    unsigned char rgba[4]      = {0, 0, 0, 0};
    unsigned long srcFrameHint = 0;      // frame whose render this pixel came from
};

// Samples one texel of the scene colour attachment. `px`/`py` are in FBO
// texel space with the origin at the BOTTOM-left (GL convention), which is
// NOT the same origin as Camera::projectZ's screen coords -- the caller does
// that flip and documents it.
// Returns out.inBounds. Restores GL_READ_FRAMEBUFFER and GL_PACK_ALIGNMENT.
bool readScenePixel(int px, int py, PixelProbe& out);

// Scene-FBO dimensions without reading a texel. A caller that must map a
// projected screen coordinate into FBO space needs the size BEFORE it can
// choose a texel, and the FBO is not required to match the window (free
// resize / render scale). Returns false and leaves *w/*h untouched when the
// post-process chain is not up.
bool sceneFboSize(int* w, int* h);

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
