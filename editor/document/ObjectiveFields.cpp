#include "MissionDocument.h"
#include "CommandHistory.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace editor::document {
std::span<const ObjectiveFieldSpec> objectiveSchema()
{
    constexpr double whole = 2147483647;
    constexpr double real = std::numeric_limits<float>::max();
    static constexpr ObjectiveFieldSpec fields[] = {
        {"Title", "st", 3}, {"Description", "st", 3},
        {"Priority", "ul", 0, 0, whole, 1}, {"ResourcePoints", "ul", 0, 0, whole},
        {"PreviousPrimaryObjectiveMustBeComplete", "b", 2},
        {"AllPreviousPrimaryObjectivesMustBeComplete", "b", 2},
        {"DisplayMarker", "b", 2}, {"MarkerX", "f", 1, -real, real}, {"MarkerY", "f", 1, -real, real},
        {"HiddenTrigger", "b", 2}, {"ActivateOnFlag", "b", 2}, {"ActivateFlagID", "st", 3},
        {"ResetStatusOnFlag", "b", 2}, {"ResetStatusFlagID", "st", 3},
        {"TitleUseResourceString", "b", 2}, {"TitleResourceStringID", "ul", 0, 0, whole},
        {"DescriptionUseResourceString", "b", 2}, {"DescriptionResourceStringID", "ul", 0, 0, whole},
        {"ObjectiveModel", "st", 3}, {"ModelType", "l", 0, 0, 255},
        {"BaseColor", "l", 0, -2147483648.0, whole, -1},
        {"HighlightColor", "l", 0, -2147483648.0, whole, -1},
        {"HighlightColor2", "l", 0, -2147483648.0, whole, -1},
        {"ModelScale", "f", 1, std::numeric_limits<float>::denorm_min(), real, 1},
        {"ModelID", "l", 0, -1, whole, -1}, {"ModelID Version", "ul", 0, 0, whole}
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
std::expected<void, LoadError> MissionDocument::decodeObjectiveScalars(Objective& item) const
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
        if (field) {
            // FIT prefixes select distinct readers; a shared C++ integer alternative cannot validate them.
            if (field->prefix != spec.prefix)
                return std::unexpected(LoadError{section(item.section)->name + "/" + std::string(spec.key), "invalid_objective_prefix", 0});
            if (field->opaque || field->value.size() != 1)
                return std::unexpected(LoadError{section(item.section)->name + "/" + std::string(spec.key), "malformed_value", 0});
            value = field->value[0];
            // FitIniFile joins the first two physical string lines; the model reflects the game's text.
            if (auto* text = std::get_if<std::string>(&value))
                if (const auto first = text->find('\n'); first != text->npos) text->erase(first, 1);
        }
        auto valid = validateObjectiveScalar(spec, value);
        if (!valid) {
            valid.error().field = section(item.section)->name + "/" + valid.error().field;
            return valid;
        }
        // Legacy activation indices supply a default ID; their lexemes remain authored syntax.
        if (!field && spec.key == "ActivateFlagID")
            if (const auto* legacy = find(item.section, "ActivateFlagIndex")) value = std::to_string(std::get<std::int64_t>(legacy->value[0]));
        item.scalars.push_back({spec.key, field ? std::optional{field->id} : std::nullopt, std::move(value)});
    }
    return {};
}

std::expected<void, LoadError> CommandHistory::setObjectiveScalar(NodeId id, std::string_view key, Scalar value)
{
    const auto reject = [&](std::string reason) -> std::expected<void, LoadError> {
        if (log_) log_("[EDITOR_OBJECTIVE_REJECT v1] field=" + std::string(key) + " reason=" + reason + " rejected=1");
        return std::unexpected(LoadError{std::string(key), std::move(reason), 0});
    };
    const auto* old = document_.objectiveScalar(id, key);
    const auto specs = objectiveSchema();
    const auto spec = std::ranges::find(specs, key, &ObjectiveFieldSpec::key);
    if (!old || spec == specs.end()) return reject("unsupported_objective_field");
    if (auto* text = std::get_if<std::string>(&value)) {
        for (auto at = text->find("\r\n"); at != text->npos; at = text->find("\r\n", at)) text->erase(at, 1);
    }
    auto valid = validateObjectiveScalar(*spec, value);
    if (!valid) return reject(valid.error().reason);
    if (value == old->value) return {};
    // The original alias remains authoritative when present; no other objective bytes are rewritten.
    const auto actualKey = old->source ? std::string_view(document_.field(*old->source)->name) : key;
    if (auto* text = std::get_if<std::string>(&value); text && text->find('\n') != text->npos) {
        // An empty opening line absorbs FitIniFile's first-line join. CRLF keeps empty lines on its RAM path too.
        std::string wire = "\r\n";
        for (const char c : *text) { if (c == '\n') wire += '\r'; wire += c; }
        *text = std::move(wire);
    }
    auto result = setScalar(id, actualKey, spec->prefix, std::move(value), "Objective " + std::string(key), true);
    if (!result && result.error().reason == "history_budget") return reject(result.error().reason);
    return result;
}
}
