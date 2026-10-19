#pragma once
#include <memory>

class MC_TextureManager;
class WeaponEffects;
class Camera;
class Weather;

// Game and mission lifetimes own these managers; mclib globals borrow them.
extern std::unique_ptr<MC_TextureManager> ownedTextureManager;
extern std::unique_ptr<WeaponEffects> ownedWeaponEffects;

// eye can temporarily point at SimpleCamera; the mission retains its GameCamera.
extern std::unique_ptr<Camera> missionCamera;

// Mission lifetime owns the weather; the renderer borrows the legacy global.
extern std::unique_ptr<Weather> missionWeather;
