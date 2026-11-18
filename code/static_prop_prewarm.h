#pragma once
// Render-side mission-load prewarm of the static prop light bakes. Lives outside objmgr.h so the sim
// headers (objmgr.h is reachable from the sim include firewall's translation units) never name Camera.
class Camera;
class GameObjectManager;

// Call after finalizeGeometry() and eye init, when world lights are valid.
void prewarmStaticPropLightBakes(GameObjectManager& objects, Camera* cam);

// Mission load: upload the movers' paint/AO/normal textures so the first mech flush does not (110-256 ms in frame 1).
void prewarmMoverGpuTextures(GameObjectManager& objects);

// Mission load: load and upload the HUD sheets the first HUD frame would (MCUI_High3, 88-108 ms in frame 0).
void prewarmHudSheets();
