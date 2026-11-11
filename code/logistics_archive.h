// B7.6 Logistics value records (design: .claude/cleanup/cxx23-step4-save-schema-design.md, Logistics rows). Field list is taken from the FIT
// writers LogisticsData::save (code/logisticsdata_save.cpp:124), LogisticsMissionInfo::save (code/logisticsmissioninfo.cpp:448),
// LogisticsVariant::save (code/logisticsvariant_stats.cpp:427), LogisticsPilot::save (code/logisticspilot.cpp:178) and LogisticsMech::save
// (code/logisticsmech.cpp:47), checked against the readers (LogisticsData::load/loadVariant/loadMech, LogisticsPilot::load,
// LogisticsMissionInfo::load).
// Private codec only: the FIT writer is still the save path and nothing calls this.
// Only player-made variants are stored (the writer skips designer variants); a designer mech in the inventory references a stock variant that the
// CSV tables provide, so only mechs of stored variants get a dangling-reference check. Force group and the three paint colours are not in the legacy
// writer; the design adds them deliberately (the legacy loader re-derives the force group from load order).
#pragma once
#include "save_archive.h"
#include "save_sections.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc2::save {

inline constexpr auto tagLogistics = fourcc('L', 'O', 'G', 'I');
// Mirrors of the game constants, kept local so this header does not pull the game headers.
inline constexpr std::int32_t logisticsSaveGameVersion = 10004; // SaveGameVersionNumber (code/logisticsdata.cpp:21)
inline constexpr std::uint32_t logisticsMaxMissions = 50;       // MAX_MISSIONS (code/logisticspilot.h:23)
inline constexpr std::uint32_t logisticsMedalCount = 16;        // Medals enum, MAX_MEDAL (code/logisticspilot.h:44)
inline constexpr std::uint32_t logisticsSpecialtySkillCount = 23; // NUM_SPECIALTY_SKILLS (code/warrior_types.h:155)
inline constexpr std::uint8_t logisticsMaxRank = 4;             // WARRIOR_RANK_ACE; NUM_WARRIOR_RANKS is 5
inline constexpr std::uint32_t logisticsMaxPlayerName = 63;     // MAX_PLAYER_NAME_LENGTH - 1 (code/logisticsmissioninfo.h:21)
inline constexpr std::uint32_t logisticsMaxName = 255;          // readIdString buffers are 256 bytes
inline constexpr std::uint32_t logisticsMaxComponents = 54;     // LogisticsVariant::components[54]
inline constexpr std::uint32_t logisticsMaxCoord = 63;          // codec cap; the grid is far smaller
inline constexpr std::uint8_t logisticsLocationCount = 3;       // LOGISTICS_COMPONENT_LOCATION HEAD/CHEST/LEGS
inline constexpr std::uint32_t logisticsMaxVariants = 256;
inline constexpr std::uint32_t logisticsMaxPilots = 256;
inline constexpr std::uint32_t logisticsMaxInventory = 512;
inline constexpr std::uint32_t logisticsMaxPurchaseFiles = 64;
inline constexpr std::int32_t logisticsMaxForceGroup = 12;      // LogisticsData::loadMech assigns count 1..12

using LogisticsText = BoundedText<logisticsMaxName>;
using PlayerNameText = BoundedText<logisticsMaxPlayerName>;

namespace logistics_detail {
inline bool equalsIgnoreCase(std::string_view x, std::string_view y) {
    if (x.size() != y.size()) return false;
    const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; };
    for (std::size_t i = 0; i < x.size(); ++i)
        if (lower(x[i]) != lower(y[i])) return false;
    return true;
}
// LogisticsPilot::turnAverageIntoRank (code/logisticspilot.cpp:516): the loader recomputes the rank from the two skills.
inline std::uint8_t rankOf(float gunnery, float piloting) {
    const float avg = (gunnery + piloting) / 2.f;
    if (avg > 79) return 4;
    if (avg > 70) return 3;
    if (avg > 60) return 2;
    if (avg > 50) return 1;
    return 0;
}
} // namespace logistics_detail

struct LogisticsComponentRecord {
    std::int32_t componentId = 0;
    std::int32_t x = 0, y = 0;
    std::uint8_t location = 0; // written by the legacy writer, ignored by its reader (addComponent re-derives it)

    void serialize(Archive& a) {
        a(componentId, x, y, location);
        if (!a.good()) return;
        if (componentId < 0) { a.reject("component_id"); return; }
        if (x < 0 || y < 0 || static_cast<std::uint32_t>(x) > logisticsMaxCoord || static_cast<std::uint32_t>(y) > logisticsMaxCoord) {
            a.reject("component_coord");
            return;
        }
        if (location >= logisticsLocationCount) a.reject("component_location");
    }
    bool operator==(const LogisticsComponentRecord&) const = default;
};

struct LogisticsVariantRecord {
    LogisticsText chassisKey;  // chassis file name, matched ignoring case
    LogisticsText variantName; // matched ignoring case by getVariant/loadMech
    std::vector<LogisticsComponentRecord> components;

    void serialize(Archive& a) {
        a(chassisKey, variantName);
        a.sequence(components, logisticsMaxComponents);
        if (!a.good()) return;
        if (chassisKey.bytes.empty()) { a.reject("variant_chassis"); return; }
        if (variantName.bytes.empty()) a.reject("variant_name"); // the reader rejects a variant without a name
    }
    bool operator==(const LogisticsVariantRecord&) const = default;
};

struct LogisticsPilotRecord {
    LogisticsText fileName; // the key mechs and the pilot loader match on (ignoring case)
    std::uint8_t rank = 0;
    float gunnery = 0, piloting = 0;
    std::int32_t mechKills = 0, vehicleKills = 0, infantryKills = 0, missionsCompleted = 0;
    std::array<std::uint8_t, logisticsMaxMissions> missionsPlayed{}; // 0 or 1 (getMissionPlayed tests == 1)
    bool dead = false;
    std::array<bool, logisticsMedalCount> medals{};
    std::array<bool, logisticsSpecialtySkillCount> specialtySkills{};

    void serialize(Archive& a) {
        a(fileName, rank, gunnery, piloting, mechKills, vehicleKills, infantryKills, missionsCompleted, missionsPlayed, dead, medals,
          specialtySkills);
        if (!a.good()) return;
        if (fileName.bytes.empty()) { a.reject("pilot_file"); return; }
        if (gunnery < 0.0f || gunnery > 100.0f || piloting < 0.0f || piloting > 100.0f) { a.reject("pilot_skill"); return; }
        // LogisticsPilot::load recomputes the rank from the skills, so a stored rank that disagrees is stale.
        if (rank > logisticsMaxRank || rank != logistics_detail::rankOf(gunnery, piloting)) { a.reject("pilot_rank"); return; }
        if (mechKills < 0 || vehicleKills < 0 || infantryKills < 0 || missionsCompleted < 0) { a.reject("pilot_counts"); return; }
        for (const auto played : missionsPlayed)
            if (played > 1) { a.reject("pilot_missions_played"); return; }
    }
    bool operator==(const LogisticsPilotRecord&) const = default;
};

struct LogisticsMechRecord {
    LogisticsText variantName;              // the writer's "Variant" key (the reader matches on it)
    LogisticsText chassisKey;               // the writer's "Chassis" key (ignored by the reader; kept to check the variant link)
    std::optional<LogisticsText> pilotFile; // the writer's "Pilot" key; absent = no pilot
    bool designer = false;                  // the writer's "Designer": a stock variant, not in the stored variants
    std::int32_t forceGroup = 0;            // 0 = none (design addition)
    std::uint32_t baseColor = 0, highlightColor1 = 0, highlightColor2 = 0; // design addition

    void serialize(Archive& a) {
        a(variantName, chassisKey, pilotFile, designer, forceGroup, baseColor, highlightColor1, highlightColor2);
        if (!a.good()) return;
        if (variantName.bytes.empty() || chassisKey.bytes.empty()) { a.reject("mech_variant"); return; }
        if (pilotFile && pilotFile->bytes.empty()) { a.reject("mech_pilot_empty"); return; }
        if (forceGroup < 0 || forceGroup > logisticsMaxForceGroup) a.reject("mech_force_group");
    }
    bool operator==(const LogisticsMechRecord&) const = default;
};

// LogisticsMissionInfo::save. The stage block is written only while currentStage < groupCount; hasStage records that.
struct LogisticsCampaignRecord {
    LogisticsText campaignFile;        // "CampaignFile"; the loader refuses the save without it
    LogisticsText campaignName;        // "CampaignName" (display)
    PlayerNameText playerName;         // "PlayerName"
    std::int32_t cbills = 0;           // "CBills"; decrementCBills is unchecked, so a negative balance is representable
    std::int32_t currentStage = 0;     // "Group"
    bool hasStage = false;
    bool bigMoviePlayed = false;       // hasStage only
    LogisticsText missionName;         // hasStage only ("MissionName", for the load dialog)
    LogisticsText missionFileName;     // hasStage only ("MissionFileName")
    std::vector<std::uint32_t> completedMissions; // hasStage only; indices into the group, written in ascending order
    std::vector<LogisticsText> additionalPurchaseFiles; // "AdditionalPurchaseFiles" File0..n

    void serialize(Archive& a) {
        a(campaignFile, campaignName, playerName, cbills, currentStage, hasStage, bigMoviePlayed, missionName, missionFileName);
        a.sequence(completedMissions, logisticsMaxMissions);
        a.sequence(additionalPurchaseFiles, logisticsMaxPurchaseFiles);
        if (!a.good()) return;
        if (campaignFile.bytes.empty()) { a.reject("campaign_file"); return; }
        if (currentStage < 0) { a.reject("campaign_stage"); return; }
        if (!hasStage && (bigMoviePlayed || !missionName.bytes.empty() || !missionFileName.bytes.empty() || !completedMissions.empty())) {
            a.reject("campaign_stray_stage");
            return;
        }
        if (hasStage && missionFileName.bytes.empty()) { a.reject("campaign_mission_file"); return; }
        for (std::size_t i = 1; i < completedMissions.size(); ++i)
            if (completedMissions[i] <= completedMissions[i - 1]) { a.reject("campaign_completed_order"); return; }
        for (const auto& f : additionalPurchaseFiles)
            if (f.bytes.empty()) { a.reject("campaign_purchase_file"); return; }
    }
    bool operator==(const LogisticsCampaignRecord&) const = default;
};

struct LogisticsState {
    std::int32_t version = logisticsSaveGameVersion; // "VersionNumber"
    bool firstTimeResults = false;                   // MissionResults::FirstTimeResults
    LogisticsCampaignRecord campaign;
    std::vector<LogisticsVariantRecord> variants;    // player-made only
    std::vector<LogisticsPilotRecord> pilots;
    std::vector<LogisticsMechRecord> inventory;

    void serialize(Archive& a) {
        a(version, firstTimeResults, campaign);
        a.sequence(variants, logisticsMaxVariants);
        a.sequence(pilots, logisticsMaxPilots);
        a.sequence(inventory, logisticsMaxInventory);
        if (!a.good()) return;
        if (version != logisticsSaveGameVersion) { a.reject("logistics_version"); return; }
        for (std::size_t i = 0; i < variants.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (logistics_detail::equalsIgnoreCase(variants[i].variantName.bytes, variants[j].variantName.bytes)) {
                    a.reject("logistics_duplicate_variant");
                    return;
                }
        for (std::size_t i = 0; i < pilots.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (logistics_detail::equalsIgnoreCase(pilots[i].fileName.bytes, pilots[j].fileName.bytes)) {
                    a.reject("logistics_duplicate_pilot");
                    return;
                }
        for (std::size_t m = 0; m < inventory.size(); ++m) {
            const auto& mech = inventory[m];
            // A stored (player-made) variant must exist under the mech's variant name and chassis; a designer mech names a stock variant.
            if (!mech.designer) {
                bool found = false;
                for (const auto& v : variants)
                    if (logistics_detail::equalsIgnoreCase(v.variantName.bytes, mech.variantName.bytes) &&
                        logistics_detail::equalsIgnoreCase(v.chassisKey.bytes, mech.chassisKey.bytes)) {
                        found = true;
                        break;
                    }
                if (!found) { a.reject("logistics_dangling_variant"); return; }
            }
            if (mech.pilotFile) {
                bool found = false;
                for (const auto& p : pilots)
                    if (logistics_detail::equalsIgnoreCase(p.fileName.bytes, mech.pilotFile->bytes)) { found = true; break; }
                if (!found) { a.reject("logistics_dangling_pilot"); return; }
                for (std::size_t k = 0; k < m; ++k) // one pilot drives one mech (getMech matches the pilot by name)
                    if (inventory[k].pilotFile && logistics_detail::equalsIgnoreCase(inventory[k].pilotFile->bytes, mech.pilotFile->bytes)) {
                        a.reject("logistics_pilot_shared");
                        return;
                    }
            }
        }
    }
    bool operator==(const LogisticsState&) const = default;
};

} // namespace mc2::save
