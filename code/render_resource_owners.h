#pragma once
#include <memory>

class WeaponEffects;
class GameCamera;
class Weather;

// Game and mission lifetimes own these managers; mclib globals borrow them.
extern std::unique_ptr<WeaponEffects> ownedWeaponEffects;

// eye can temporarily point at SimpleCamera; the mission retains its GameCamera.
extern std::unique_ptr<GameCamera> missionCamera;

// Mission lifetime owns the weather; the renderer borrows the legacy global.
extern std::unique_ptr<Weather> missionWeather;
