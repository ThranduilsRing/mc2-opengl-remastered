// B7.6 ObjectiveSet value records (design: .claude/cleanup/cxx23-step4-save-schema-design.md, ObjectiveSet row). Field list is taken
// from the FIT writers in code/objective.cpp (CObjectives::Save :2398, CObjective::Save :2308, the per-species Save methods :2214-2307),
// the species enums in code/objective.h (condition :43, action :342) and the flag table in code/objective_flags.cpp (:38).
// Private codec only: the FIT writer is still the save path and nothing calls this.
// Species payload kinds follow the writers. The three *_ENEMY_UNIT_GROUP condition species have no class and no factory case
// (CObjective::new_CObjectiveCondition), so they can never exist in a live objective and the codec rejects them.
// Watch ids are the ObjectManager watch ids (start at 1; the writers STOP on a null target, so 0 is never a valid reference).
#pragma once
#include "save_archive.h"
#include "save_sections.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mc2::save {

inline constexpr auto tagObjectives = fourcc('O', 'B', 'J', 'S');
// Mirrors of MAX_TEAMS (mclib/dstd.h), NUM_CONDITION_SPECIES / NUM_ACTION_SPECIES (code/objective.h); kept local so this header
// does not pull the game headers.
inline constexpr std::uint32_t objectiveMaxTeams = 8;
inline constexpr std::uint8_t conditionSpeciesCount = 24;
inline constexpr std::uint8_t actionSpeciesCount = 7;
inline constexpr std::uint32_t objectiveMaxText = 2000; // objective_text::kMaxTextLength
inline constexpr std::uint32_t objectiveMaxObjectives = 64;
inline constexpr std::uint32_t objectiveMaxParts = 64;  // per condition/action list
inline constexpr std::uint32_t objectiveMaxFlags = 256;

enum class ConditionPayload : std::uint8_t { None, Num, Unit, Structure, Area, Flag, Time, Invalid };

// condition_species_type order (code/objective.h:43).
constexpr ConditionPayload conditionPayloadOf(std::uint8_t species) {
    switch (species) {
    case 0: case 5: case 10: return ConditionPayload::None;            // destroy / capture-or-destroy / dead-or-fled ALL
    case 1: case 6: case 11: return ConditionPayload::Num;             // ... NUMBER_OF
    case 2: case 7: case 12: return ConditionPayload::Invalid;         // ... GROUP: no factory case
    case 3: case 8: case 13: case 14: case 16: return ConditionPayload::Unit; // specific unit, CAPTURE_UNIT, GUARD_SPECIFIC_UNIT
    case 4: case 9: case 15: case 17: return ConditionPayload::Structure;     // specific structure, CAPTURE_STRUCTURE, GUARD_..._STRUCTURE
    case 18: case 19: case 20: case 21: return ConditionPayload::Area;
    case 22: return ConditionPayload::Flag;
    case 23: return ConditionPayload::Time;
    default: return ConditionPayload::Invalid;
    }
}

enum class ActionPayload : std::uint8_t { Path, Message, ResourceString, Flag, Purchase, Structure, Invalid };

// action_species_type order (code/objective.h:342).
constexpr ActionPayload actionPayloadOf(std::uint8_t species) {
    switch (species) {
    case 0: case 1: return ActionPayload::Path;   // PLAY_BIK (AVIFileName), PLAY_WAV (WAVFileName)
    case 2: return ActionPayload::Message;
    case 3: return ActionPayload::ResourceString;
    case 4: return ActionPayload::Flag;
    case 5: return ActionPayload::Purchase;
    case 6: return ActionPayload::Structure;      // _REMOVE_STRUCTURE
    default: return ActionPayload::Invalid;
    }
}

using ObjectiveText = BoundedText<objectiveMaxText>;

struct ObjectiveCondition {
    std::uint8_t species = 0;
    std::uint32_t num = 0;     // Num
    std::int32_t watchId = 0;  // Unit / Structure
    float centerX = 0, centerY = 0, radius = 0; // Area
    ObjectiveText flagId;      // Flag
    bool flagValue = false;    // Flag
    float time = 0;            // Time

    void serialize(Archive& a) {
        a(species);
        if (!a.good()) return;
        const auto kind = conditionPayloadOf(species);
        if (kind == ConditionPayload::Invalid) { a.reject("condition_species"); return; }
        switch (kind) {
        case ConditionPayload::Num: a(num); break;
        case ConditionPayload::Unit: case ConditionPayload::Structure: a(watchId); break;
        case ConditionPayload::Area: a(centerX, centerY, radius); break;
        case ConditionPayload::Flag: a(flagId, flagValue); break;
        case ConditionPayload::Time: a(time); break;
        default: break;
        }
        if (!a.good()) return;
        // Canonical form: fields foreign to the species stay at their defaults (a writer filling them would not re-encode identically).
        const bool stray = (kind != ConditionPayload::Num && num != 0) ||
                           ((kind != ConditionPayload::Unit && kind != ConditionPayload::Structure) && watchId != 0) ||
                           (kind != ConditionPayload::Area && (centerX != 0 || centerY != 0 || radius != 0)) ||
                           (kind != ConditionPayload::Flag && (!flagId.bytes.empty() || flagValue)) ||
                           (kind != ConditionPayload::Time && time != 0);
        if (stray) { a.reject("condition_stray_payload"); return; }
        if (num > 0x7FFFFFFFu) { a.reject("condition_num"); return; }
        if ((kind == ConditionPayload::Unit || kind == ConditionPayload::Structure) && watchId <= 0) {
            a.reject("condition_watch_id");
            return;
        }
        if (kind == ConditionPayload::Area && radius < 0.0f) { a.reject("condition_radius"); return; }
        if (kind == ConditionPayload::Flag && flagId.bytes.empty()) { a.reject("condition_flag_id"); return; }
        if (kind == ConditionPayload::Time && time < 0.0f) { a.reject("condition_time"); return; }
    }
    bool operator==(const ObjectiveCondition&) const = default;
};

struct ObjectiveAction {
    std::uint8_t species = 0;
    ObjectiveText text;           // Path / Message / Purchase / Flag id
    std::uint32_t resourceStringId = 0; // ResourceString
    bool flagValue = false;       // Flag
    std::int32_t watchId = 0;     // Structure

    void serialize(Archive& a) {
        a(species);
        if (!a.good()) return;
        const auto kind = actionPayloadOf(species);
        if (kind == ActionPayload::Invalid) { a.reject("action_species"); return; }
        switch (kind) {
        case ActionPayload::Path: case ActionPayload::Message: case ActionPayload::Purchase: a(text); break;
        case ActionPayload::ResourceString: a(resourceStringId); break;
        case ActionPayload::Flag: a(text, flagValue); break;
        case ActionPayload::Structure: a(watchId); break;
        default: break;
        }
        if (!a.good()) return;
        const bool usesText = kind == ActionPayload::Path || kind == ActionPayload::Message ||
                              kind == ActionPayload::Purchase || kind == ActionPayload::Flag;
        const bool stray = (!usesText && !text.bytes.empty()) || (kind != ActionPayload::ResourceString && resourceStringId != 0) ||
                           (kind != ActionPayload::Flag && flagValue) || (kind != ActionPayload::Structure && watchId != 0);
        if (stray) { a.reject("action_stray_payload"); return; }
        if (resourceStringId > 0x7FFFFFFFu) { a.reject("action_resource_string"); return; }
        if (kind == ActionPayload::Structure && watchId <= 0) { a.reject("action_watch_id"); return; }
        if (kind == ActionPayload::Flag && text.bytes.empty()) { a.reject("action_flag_id"); return; }
    }
    bool operator==(const ObjectiveAction&) const = default;
};

struct ObjectiveRecord {
    ObjectiveText title, description;
    bool titleUseResourceString = false, descriptionUseResourceString = false;
    std::uint32_t titleResourceStringId = 0, descriptionResourceStringId = 0;
    std::int32_t priority = 1;                  // 1 = primary, 2 = secondary (code/objective.cpp:1806, :2171)
    std::int32_t resourcePoints = 0;
    bool previousPrimaryMustBeComplete = false, allPreviousPrimariesMustBeComplete = false;
    std::uint8_t displayMarker = 0;             // MARKER_TYPES 0..2 (the FIT writer collapses this to a boolean; the codec keeps the value)
    float markerX = 0, markerY = 0;
    bool hiddenTrigger = false;
    bool activateOnFlag = false;
    ObjectiveText activateFlagId;
    bool resetStatusOnFlag = false;
    ObjectiveText resetStatusFlagId;
    ObjectiveText modelName;
    std::int32_t modelType = 0, modelBaseColor = 0, modelHighlightColor = 0, modelHighlightColor2 = 0;
    float modelScale = 0;
    bool isActive = false;
    double activationTime = 0;
    bool resolved = false;
    std::uint8_t resolvedStatus = 0;            // objective_status_type 0..2
    bool changedStatus = false;
    std::vector<ObjectiveCondition> conditions, failureConditions;
    std::vector<ObjectiveAction> actions, failureActions;

    void serialize(Archive& a) {
        a(title, titleUseResourceString, titleResourceStringId, description, descriptionUseResourceString, descriptionResourceStringId,
          priority, resourcePoints, previousPrimaryMustBeComplete, allPreviousPrimariesMustBeComplete, displayMarker, markerX, markerY,
          hiddenTrigger, activateOnFlag, activateFlagId, resetStatusOnFlag, resetStatusFlagId, modelName, modelType, modelBaseColor,
          modelHighlightColor, modelHighlightColor2, modelScale, isActive, activationTime, resolved, resolvedStatus, changedStatus);
        a.sequence(conditions, objectiveMaxParts);
        a.sequence(actions, objectiveMaxParts);
        a.sequence(failureConditions, objectiveMaxParts);
        a.sequence(failureActions, objectiveMaxParts);
        if (!a.good()) return;
        if (priority < 1 || priority > 2) { a.reject("objective_priority"); return; }
        if (resourcePoints < 0) { a.reject("objective_points"); return; }
        if (titleResourceStringId > 0x7FFFFFFFu || descriptionResourceStringId > 0x7FFFFFFFu) { a.reject("objective_resource_string"); return; }
        if (displayMarker > 2) { a.reject("objective_marker"); return; }
        if (modelScale < 0.0f) { a.reject("objective_model_scale"); return; }
        if (activationTime < 0.0) { a.reject("objective_activation_time"); return; }
        if (resolvedStatus > 2) { a.reject("objective_status"); return; }
        // A flag trigger without a flag id can never fire; a flag id without the trigger is a stale field.
        if (activateOnFlag != !activateFlagId.bytes.empty()) { a.reject("objective_activate_flag"); return; }
        if (resetStatusOnFlag != !resetStatusFlagId.bytes.empty()) { a.reject("objective_reset_flag"); return; }
    }
    bool operator==(const ObjectiveRecord&) const = default;
};

struct ObjectiveFlag {
    ObjectiveText id;
    bool value = false;
    void serialize(Archive& a) {
        a(id, value);
        if (a.good() && id.bytes.empty()) a.reject("flag_id");
    }
    bool operator==(const ObjectiveFlag&) const = default;
};

struct TeamObjectives {
    std::uint32_t alignment = 0;
    std::vector<ObjectiveFlag> flags; // CBooleanArray order; ids unique ignoring ASCII case (setElementValue matches with iequals)
    std::vector<ObjectiveRecord> objectives; // evaluation/save order

    void serialize(Archive& a) {
        a(alignment);
        a.sequence(flags, objectiveMaxFlags);
        a.sequence(objectives, objectiveMaxObjectives);
        if (!a.good()) return;
        if (alignment >= objectiveMaxTeams) { a.reject("objectives_alignment"); return; }
        for (std::size_t i = 0; i < flags.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (equalsIgnoreCase(flags[i].id.bytes, flags[j].id.bytes)) { a.reject("objectives_duplicate_flag"); return; }
    }
    bool operator==(const TeamObjectives&) const = default;

private:
    static bool equalsIgnoreCase(const std::string& x, const std::string& y) {
        if (x.size() != y.size()) return false;
        for (std::size_t i = 0; i < x.size(); ++i) {
            const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; };
            if (lower(x[i]) != lower(y[i])) return false;
        }
        return true;
    }
};

struct ObjectiveSetState {
    std::uint32_t version = 3; // CObjectives::Save "Objectives Version"
    std::vector<TeamObjectives> teams; // ascending alignment, unique

    void serialize(Archive& a) {
        a(version);
        a.sequence(teams, objectiveMaxTeams);
        if (!a.good()) return;
        if (version != 3) { a.reject("objectives_version"); return; }
        for (std::size_t i = 1; i < teams.size(); ++i)
            if (teams[i].alignment <= teams[i - 1].alignment) { a.reject("objectives_team_order"); return; }
    }
    bool operator==(const ObjectiveSetState&) const = default;
};

} // namespace mc2::save
