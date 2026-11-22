#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace RenderCore {

// CPU record only. The radiometric library and generated GPU ABI belong to L2.
// These colors retain the legacy authored byte interpretation, not physical units.
struct LegacyMissionLighting {
    std::array<float, 3> cameraDirection{0, 0, 1};
    std::array<float, 3> effectiveDirection{0, 0, 1};
    std::array<float, 3> directionalColor{1, 1, 1};
    std::array<float, 3> ambientColor{0, 0, 0};
    std::array<float, 3> flatLight{1, 1, 1};
    std::array<float, 3> flatRatio{1, 1, 1};   // live flat light / authored flat light; exactly 1 with no sun keys, at night, with no mission
    float sunGain = 1;                         // authored/live sun elevation (sqrt of the sin pitch ratio), the mech and vehicle directional-term gain; exactly 1 with no keys
    float ambientRatio = 1;                   // live ambient luma / authored ambient luma (scales the prop SH sky fill); exactly 1 with no keys
    float authoredPitch = 45;
    float nightFactor = 0;
    float daylightLevel = 1;
    int skyNumber = 0;
    bool night = false;
    bool operator==(const LegacyMissionLighting&) const = default;
};

struct MissionEnvironment {
    static constexpr uint32_t kSchemaVersion = 2;
    uint32_t sourceSchemaVersion = 1;
    std::string mission;
    std::string colorContract = "legacy-authored-v1";
    std::string directionalSource = "legacy-fixed";
    LegacyMissionLighting lighting;
    struct Sky {
        std::string asset;
        std::string provider = "legacy-sky-loader";
        std::string shIdentity = "default";
        std::string shConvention = "legacy-generated-Y-up";
        std::string orientation = "legacy-sun-aligned";
        std::array<std::array<float, 3>, 9> diffuseSh{};
        std::array<float, 3> hemisphereSky{};
        std::array<float, 3> hemisphereGround{};
    } sky;
    float unitAmbientScale = 1.2f;
    float buildingSpillIntensityScale = 1.0f;
    struct Output {
        float exposureMultiplier = 1;
        std::optional<float> exposureEV = 0;
        std::array<float, 3> grade{1, 1, 1};
        std::array<float, 3> shadows{1, 1, 1};
        std::array<float, 3> highlights{1, 1, 1};
        std::array<float, 3> topWarmth{1, 1, 1};
        float vignette = 0;
        float topWarmthStrength = 0;
        std::string transform = "profile-grade";
    } output;
    std::string atmosphere = "legacy-per-receiver";
    std::string localLights = "building-anchored-SUM/MAX-compression";
    std::string clouds = "legacy-whole-color-multiply";
    std::string cacheStatus = "legacy-no-environment-cook";
    std::map<std::string, float> legacyTuning;
    std::vector<std::string> providers;
    std::vector<std::string> diagnostics;
    uint64_t effectiveHash = 0;
};

} // namespace RenderCore
