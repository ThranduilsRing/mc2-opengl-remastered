// Player settings shared by the game, library readers and tools.
#pragma once
namespace mc2::options {
struct Preferences {
    long DigitalMasterVolume = 255;
    long MusicVolume = 64;
    long RadioVolume = 64;
    long sfxVolume = 64;
    long BettyVolume = 64;
    bool useShadows = true;
    bool useWaterInterestTexture = true;
    bool useHighObjectDetail = true;
    long GameDifficulty = 1;
    bool useUnlimitedAmmo = true;
    long renderer = 0;
    long resolutionX = 800;
    long resolutionY = 600;
    bool fullScreen = false;
    long gammaLevel = 0;
    bool useLeftRightMouseProfile = true;
    long baseColor = static_cast<long>(0xffff7e00u);
    long highlightColor = static_cast<long>(0xffff7e00u);
    long faction = 0;
    bool pilotVideos = true;
    bool UseUpscaledVideos = true;
    bool useLocalShadows = true;
    bool useNonWeaponEffects = false;
    bool asyncMouse = false;
    long fogPos = 50;
    char bitDepth = 32;
    bool saveTranscripts = false;
    bool tutorials = true;
    float scrollSpeedMult = 2.0f;
    float zoomSpeedMult = 2.0f;
    char insigniaFile[256]{};
    char unitName[10][256]{};
    char playerName[10][256]{};
    char ipAddresses[10][24]{};
};
// The applied settings. CPrefs also owns a draft for the options screen's Apply/Cancel behavior.
inline Preferences& runtimePreferences() {
    static Preferences values = [] {
        Preferences p;
        p.DigitalMasterVolume = p.MusicVolume = p.sfxVolume = p.RadioVolume = p.BettyVolume = 0;
        p.GameDifficulty = 0; p.resolutionX = p.resolutionY = 0;
        p.useLocalShadows = false; p.useNonWeaponEffects = true; p.asyncMouse = true;
        return p;
    }();
    return values;
}
} // namespace mc2::options
