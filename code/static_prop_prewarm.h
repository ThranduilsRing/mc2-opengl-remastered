#pragma once
// Render-side mission-load prewarm of the static prop light bakes. Lives outside objmgr.h so the sim
// headers (objmgr.h is reachable from the sim include firewall's translation units) never name Camera.
class Camera;
class GameObjectManager;

// Call after finalizeGeometry() and eye init, when world lights are valid.
void prewarmStaticPropLightBakes(GameObjectManager& objects, Camera* cam);
