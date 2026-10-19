#pragma once
#include "preferences_values.h"
inline auto& DigitalMasterVolume = mc2::options::runtimePreferences().DigitalMasterVolume;
inline auto& MusicVolume = mc2::options::runtimePreferences().MusicVolume;
inline auto& sfxVolume = mc2::options::runtimePreferences().sfxVolume;
inline auto& RadioVolume = mc2::options::runtimePreferences().RadioVolume;
inline auto& BettyVolume = mc2::options::runtimePreferences().BettyVolume;
inline auto& useShadows = mc2::options::runtimePreferences().useShadows;
inline auto& useLocalShadows = mc2::options::runtimePreferences().useLocalShadows;
inline auto& useWaterInterestTexture = mc2::options::runtimePreferences().useWaterInterestTexture;
inline auto& useUnlimitedAmmo = mc2::options::runtimePreferences().useUnlimitedAmmo;
inline auto& GameDifficulty = mc2::options::runtimePreferences().GameDifficulty;
inline auto& renderer = mc2::options::runtimePreferences().renderer;
inline auto& resolutionX = mc2::options::runtimePreferences().resolutionX;
inline auto& resolutionY = mc2::options::runtimePreferences().resolutionY;
inline auto& gammaLevel = mc2::options::runtimePreferences().gammaLevel;
inline auto& useLeftRightMouseProfile = mc2::options::runtimePreferences().useLeftRightMouseProfile;
inline auto& useNonWeaponEffects = mc2::options::runtimePreferences().useNonWeaponEffects;
inline auto& useHighObjectDetail = mc2::options::runtimePreferences().useHighObjectDetail;
inline volatile bool& mc2UseAsyncMouse = mc2::options::runtimePreferences().asyncMouse;
