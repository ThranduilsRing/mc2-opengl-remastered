#include "MissionDocument.h"
#include "CommandHistory.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace editor::document {
std::span<const ObjectiveFieldSpec> objectiveSchema()
{
    constexpr double whole = 2147483647;
    constexpr double signedMin = -2147483648.0;
    constexpr double real = std::numeric_limits<float>::max();
    static constexpr ObjectiveFieldSpec fields[] = {
        {"Title", "st", 3}, {"Description", "st", 3},
        {"Priority", "ul", 0, signedMin, whole, 1}, {"ResourcePoints", "ul", 0, signedMin, whole},
        {"PreviousPrimaryObjectiveMustBeComplete", "b", 2},
        {"AllPreviousPrimaryObjectivesMustBeComplete", "b", 2},
        {"DisplayMarker", "b", 2}, {"MarkerX", "f", 1, -real, real}, {"MarkerY", "f", 1, -real, real},
        {"HiddenTrigger", "b", 2}, {"ActivateOnFlag", "b", 2}, {"ActivateFlagID", "st", 3},
        {"ResetStatusOnFlag", "b", 2}, {"ResetStatusFlagID", "st", 3},
        {"TitleUseResourceString", "b", 2}, {"TitleResourceStringID", "ul", 0, signedMin, whole},
        {"DescriptionUseResourceString", "b", 2}, {"DescriptionResourceStringID", "ul", 0, signedMin, whole},
        {"ObjectiveModel", "st", 3}, {"ModelType", "l", 0, signedMin, whole},
        {"BaseColor", "l", 0, -2147483648.0, whole, -1},
        {"HighlightColor", "l", 0, -2147483648.0, whole, -1},
        {"HighlightColor2", "l", 0, -2147483648.0, whole, -1},
        {"ModelScale", "f", 1, -real, real, 1},
        {"ModelID", "l", 0, signedMin, whole, -1}, {"ModelID Version", "ul", 0, signedMin, whole}
    };
    return fields;
}

std::expected<void, LoadError> validateObjectiveScalar(const ObjectiveFieldSpec& spec, const Scalar& value)
{
    const auto reject = [&](const char* reason) { return std::unexpected(LoadError{std::string(spec.key), reason, 0}); };
    if (value.index() != spec.type) return reject("malformed_value");
    if (const auto* text = std::get_if<std::string>(&value)) {
        if (text->size() > objectiveTextLimit) return reject("objective_text_limit");
        if (text->find_first_of("\"\r") != text->npos || text->find('\0') != text->npos) return reject("invalid_objective_text");
    }
    if (const auto* number = std::get_if<std::int64_t>(&value))
        if (*number < static_cast<std::int64_t>(spec.minimum) || *number > static_cast<std::int64_t>(spec.maximum)) return reject("out_of_range");
    if (const auto* number = std::get_if<double>(&value))
        if (!std::isfinite(*number) || *number < spec.minimum || *number > spec.maximum) return reject("out_of_range");
    return {};
}

const Objective* MissionDocument::objective(NodeId id) const
{
    const auto found = std::ranges::find(objectives_, id, &Objective::section);
    return found == objectives_.end() ? nullptr : &*found;
}
const ObjectiveScalar* MissionDocument::objectiveScalar(NodeId id, std::string_view key) const
{
    const auto* item = objective(id);
    if (!item) return nullptr;
    const auto found = std::ranges::find(item->scalars, key, &ObjectiveScalar::key);
    return found == item->scalars.end() ? nullptr : &*found;
}
void MissionDocument::preserveObjectiveField(NodeId id, std::string_view warning)
{
    auto& field = syntax_.fields[fieldIndex_.at(id.value)];
    field.opaque = true; field.warning = warning;
}
std::expected<void, LoadError> MissionDocument::decodeObjectiveScalars(Objective& item)
{
    for (const auto& spec : objectiveSchema()) {
        const auto* field = find(item.section, spec.key);
        if (!field && spec.key == "PreviousPrimaryObjectiveMustBeComplete") field = find(item.section, "PreviousObjectiveMustBeComplete");
        Scalar value;
        if (spec.type == 0) value = spec.integerDefault;
        else if (spec.type == 1) value = static_cast<double>(spec.integerDefault);
        else if (spec.type == 2) value = false;
        else value = std::string{};
        if (spec.key == "Description") value = item.scalars.front().value;
        std::string_view warning;
        if (field) {
            // FIT prefixes select readers independently of the C++ value type.
            if (field->prefix != spec.prefix) {
                if (spec.key == "DisplayMarker")
                    return std::unexpected(LoadError{section(item.section)->name + "/DisplayMarker", "invalid_objective_prefix", 0});
                warning = "ignored by the game";
            } else if (field->opaque || field->value.size() != 1)
                return std::unexpected(LoadError{section(item.section)->name + "/" + std::string(spec.key), "malformed_value", 0});
            else {
                value = field->value[0];
                if (field->warning.starts_with("outside signed")) warning = field->warning;
            }
            // Imported text reflects FIT's first-line join.
            if (auto* text = std::get_if<std::string>(&value))
                if (const auto first = text->find('\n'); first != text->npos) text->erase(first, 1);
        }
        auto valid = validateObjectiveScalar(spec, value);
        if (!valid) {
            if (field && spec.type == 3 && valid.error().reason == "objective_text_limit") {
                warning = "ignored by the game";
                value = spec.key == "Description" ? item.scalars.front().value : Scalar{std::string{}};
            } else if (field && valid.error().reason == "out_of_range") {
                warning = spec.key == "ModelID" || spec.key == "ModelID Version" ? "ignored by the game" :
                    "outside signed 32-bit range; the game reader overflows or wraps this value";
            }
            else {
                valid.error().field = section(item.section)->name + "/" + valid.error().field;
                return valid;
            }
        }
        // Activation indices supply fallback IDs without losing authored spelling.
        if ((!field || !warning.empty()) && spec.key == "ActivateFlagID")
            if (const auto* legacy = find(item.section, "ActivateFlagIndex")) {
                if (legacy->prefix == "ul" && legacy->value.size() == 1 && std::holds_alternative<std::int64_t>(legacy->value[0])) {
                    value = std::to_string(std::get<std::int64_t>(legacy->value[0]));
                }
                if (!field && !legacy->warning.empty()) { field = legacy; warning = legacy->warning; }
            }
        if (!warning.empty() && spec.key == "PreviousPrimaryObjectiveMustBeComplete")
            if (const auto* legacy = find(item.section, "PreviousObjectiveMustBeComplete"); legacy && legacy->prefix == "b" &&
                legacy->value.size() == 1 && std::holds_alternative<bool>(legacy->value[0])) value = legacy->value[0];
        if (field && !warning.empty()) preserveObjectiveField(field->id, warning);
        item.scalars.push_back({spec.key, field ? std::optional{field->id} : std::nullopt, std::move(value), warning});
    }
    const auto scalar = [&](std::string_view key) -> const Scalar& { return std::ranges::find(item.scalars, key, &ObjectiveScalar::key)->value; };
    const auto type = std::get<std::int64_t>(scalar("ModelType"));
    // Other model types assert in the game camera.
    if (!std::get<std::string>(scalar("ObjectiveModel")).empty() && type != 5 && type != 13 && type != 14 && type != 15)
        return std::unexpected(LoadError{section(item.section)->name + "/ModelType", "unsupported_model_type_asserts_in_game_camera", 0});
    return {};
}

std::expected<void, LoadError> CommandHistory::setObjectiveScalar(NodeId id, std::string_view key, Scalar value)
{
    const auto reject = [&](std::string reason) -> std::expected<void, LoadError> {
        return std::unexpected(rejectObjective(std::move(reason)));
    };
    const auto* old = document_.objectiveScalar(id, key);
    const auto specs = objectiveSchema();
    const auto spec = std::ranges::find(specs, key, &ObjectiveFieldSpec::key);
    if (!old || spec == specs.end()) return reject("unsupported_objective_field");
    if (!old->warning.empty() && old->warning != "ignored by the game") return reject(std::string(old->warning));
    if (auto* text = std::get_if<std::string>(&value)) {
        for (auto at = text->find("\r\n"); at != text->npos; at = text->find("\r\n", at)) text->erase(at, 1);
    }
    auto valid = validateObjectiveScalar(*spec, value);
    if (!valid) return reject(valid.error().reason);
    if (value == old->value && old->warning.empty()) return {};
    // Preserve authored prerequisite aliases and legacy activation indices.
    const auto actualKey = old->source && document_.field(*old->source)->name != "ActivateFlagIndex" ?
        std::string_view(document_.field(*old->source)->name) : key;
    if (auto* text = std::get_if<std::string>(&value); text && text->find('\n') != text->npos) {
        // FIT text needs an empty opening line and CRLF for lossless disk/RAM reads.
        std::string wire = "\r\n";
        for (const char c : *text) { if (c == '\n') wire += '\r'; wire += c; }
        *text = std::move(wire);
    }
    auto result = setScalar(id, actualKey, spec->prefix, std::move(value), "Objective " + std::string(key), true);
    if (!result && result.error().reason == "history_budget") return reject(result.error().reason);
    return result;
}
}
