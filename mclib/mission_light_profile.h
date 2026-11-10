// mission_light_profile.h -- LOCAL-LIGHTS-4: which per-mission night profile a mission file name selects.
// Header-only (camera.cpp and the unit tests share it).
#pragma once
#include <cstddef>
#include <string_view>
#include <algorithm>
#include <cmath>

namespace mc2_mission_light {

inline float nightFromSun(float pitch)
{
    const float altitude = pitch > 90.0f ? 180.0f - pitch : pitch;
    return std::clamp((25.0f - altitude) / (25.0f - 10.0f), 0.0f, 1.0f);
}

inline float darkness(float pitch)
{
    const float t = std::clamp((nightFromSun(pitch) - 0.35f) / 0.65f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Night fade of the MOVER (mech/prop) sun shadow: 0 by day, easing toward 1 (no shadow) as the night
// takes the sun. The old flat 0.85*night left a 0.4-floor ground only 9% darker under a mech. A moon
// shadow removes the moon's share of the light, so the fade is the ambient share of (moon + ambient).
inline float moverNightFade(float darkness, float moonLum, float ambientLum)
{
    (void)moonLum; (void)ambientLum;
    return 0.85f * std::clamp(darkness, 0.0f, 1.0f);
}
// Receiver floor of a sun shadow with day floor `dayFloor` under that fade (mirrors mover_shadow.glsl moverShadowFloor).
inline float moverShadowFloor(float dayFloor, float fade) { return dayFloor + (1.0f - dayFloor) * fade; }

// Base name (no directory, no extension) equals `profile`, or starts with it followed by '_' (a save of that
// mission, "mc2_10_saved_204523"); ASCII case-insensitive. "mc2_100" does not match "mc2_10".
inline bool matches(std::string_view missionFileName, std::string_view profile)
{
    const size_t slash = missionFileName.find_last_of("/\\");
    std::string_view name = slash == std::string_view::npos ? missionFileName : missionFileName.substr(slash + 1);
    name = name.substr(0, name.find('.'));
    if (name.size() < profile.size()) return false;
    for (size_t i = 0; i < profile.size(); ++i) {
        char a = name[i], b = profile[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return name.size() == profile.size() || name[profile.size()] == '_';
}

struct MissionLightProfile { const char* mission; float light; float ambient; float ground; };
inline constexpr MissionLightProfile kDefaultNightProfile{nullptr, 0.65f, 0.6f, 0.8f};
inline constexpr MissionLightProfile kMissionLightProfiles[] = {
	{"mc2_10", 0.45f, 0.40f, 0.55f},   // airbase raid just after sunset: fairly dark, lamps carry it
};
inline const MissionLightProfile& missionLightProfile(std::string_view missionName)
{
	for (const auto& profile : kMissionLightProfiles)
		if (mc2_mission_light::matches(missionName, profile.mission))
			return profile;
	return kDefaultNightProfile;
}

inline unsigned char scaleChannel(unsigned char v, float s)
{
	return static_cast<unsigned char>(std::clamp(std::lround(v * s), 0L, 255L));
}

template<class Camera>
inline float sceneScale(const Camera& c)
{
    return c.missionLightName.empty() ? 1.0f :
        1.0f + (c.missionGroundScale - 1.0f) * nightFromSun(c.lightPitch);
}

// `alreadyApplied`: the camera block came from a save whose colours carry the profile; only the ground
// scale (not saved) is restored.
template<class Camera>
inline void apply(Camera& c, bool alreadyApplied)
{
	c.missionGroundScale = 1.0f;
	c.missionLightApplied = false;
	if (c.missionLightName.empty()) return;
	c.missionLightApplied = alreadyApplied;
	const MissionLightProfile& p = missionLightProfile(c.missionLightName);
	c.missionGroundScale = p.ground;
	const float night = mc2_mission_light::nightFromSun(c.lightPitch);
	if (night <= 0.0f) return;
	c.missionLightApplied = true;
	const float ls = alreadyApplied ? 1.0f : 1.0f + (p.light - 1.0f) * night;
	const float as = alreadyApplied ? 1.0f : 1.0f + (p.ambient - 1.0f) * night;
	if (!alreadyApplied)
	for (unsigned char* v : {&c.lightRed, &c.lightGreen, &c.lightBlue, &c.dayLightRed, &c.dayLightGreen, &c.dayLightBlue,
	                         &c.nightLightRed, &c.nightLightGreen, &c.nightLightBlue, &c.sunsetLightRed, &c.sunsetLightGreen, &c.sunsetLightBlue})
		*v = scaleChannel(*v, ls);
	if (!alreadyApplied)
	for (unsigned char* v : {&c.ambientRed, &c.ambientGreen, &c.ambientBlue, &c.dayAmbientRed, &c.dayAmbientGreen, &c.dayAmbientBlue,
	                         &c.nightAmbientRed, &c.nightAmbientGreen, &c.nightAmbientBlue,
	                         &c.terrainShadowRed, &c.terrainShadowGreen, &c.terrainShadowBlue})
		*v = scaleChannel(*v, as);
}

} // namespace mc2_mission_light
