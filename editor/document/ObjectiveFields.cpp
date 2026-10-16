#include "MissionDocument.h"
#include "CommandHistory.h"
#include "FitReader.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <format>
#include <charconv>
#include <set>

namespace editor::document {
const TeamFlagRow* TeamFlags::resolve(std::string_view id) const
{
    std::string memoryId;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (id[i] == '\n' && (i == 0 || id[i - 1] != '\r')) memoryId += '\r';
        memoryId += id[i];
    }
    for (const auto& row : rows)
        if (row.status == "initialized" && row.id && identifier(*row.id) == identifier(memoryId)) return &row;
    return nullptr;
}

TeamFlags MissionDocument::teamFlags(std::uint32_t team) const
{
    TeamFlags result;
    const auto block = std::format("Team{}BooleanFlags", team);
    const auto bytes = fitBytes();
    const FitReader reader(bytes);
    if (!reader.block(block)) return result;
    for (const auto& candidate : syntax().sections)
        if (!candidate.parent && candidate.name == block) { result.section = candidate.id; break; }
    std::set<std::size_t> indices;
    if (result.section) for (const auto node : section(*result.section)->fields) {
        const auto name = identifier(field(node)->name);
        for (const std::string_view prefix : {"booleanstring", "booleanvalue"}) {
            if (!name.starts_with(prefix)) continue;
            const auto suffix = std::string_view(name).substr(prefix.size());
            std::size_t index{};
            const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
            if (parsed.ec == std::errc{} && parsed.ptr == suffix.data() + suffix.size() && suffix == std::to_string(index)) indices.insert(index);
        }
    }
    for (std::size_t index = 0; index < nodeLimit; ++index) {
        const auto id = reader.read(block, "st", std::format("BooleanString{}", index));
        if (!id && id.error() == "absent") break;
        indices.insert(index);
    }
    bool stopped{};
    for (const auto index : indices) {
        if (index != result.stop) stopped = true;
        const auto id = reader.read(block, "st", std::format("BooleanString{}", index));
        const auto value = reader.read(block, "b", std::format("BooleanValue{}", index));
        TeamFlagRow row{index};
        if (id) {
            row.id.emplace();
            for (const auto c : std::get<std::string>(*id)) { if (c == '\n') *row.id += '\r'; *row.id += c; }
        }
        const bool oversized = !id && id.error() == "reader_string_buffer_limit";
        if (value) row.value = std::get<bool>(*value);
        if (stopped) row.status = "ignored tail after first absent ID";
        else if (oversized) { row.status = "oversized ID skipped"; ++result.stop; }
        else if (!row.id) { row.status = "absent/unreadable ID ends loading"; stopped = true; }
        else {
            row.status = "initialized";
            ++result.stop;
        }
        if (row.id) for (const auto& earlier : result.rows)
            if (earlier.id && identifier(*earlier.id) == identifier(*row.id)) { row.duplicateOf = earlier.index; break; }
        result.rows.push_back(std::move(row));
    }
    return result;
}

std::span<const ActionSpec> actionSchema(bool creation)
{
    static constexpr ObjectiveFieldSpec bik[] = {{"AVIFileName", "st", 3}};
    static constexpr ObjectiveFieldSpec wav[] = {{"WAVFileName", "st", 3}};
    static constexpr ObjectiveFieldSpec text[] = {{"Message", "st", 3}};
    static constexpr ObjectiveFieldSpec technology[] = {{"PurchaseFileName", "st", 3}};
    static constexpr ObjectiveFieldSpec resource[] = {{"ResourceStringID", "ul", 0, -2147483648.0, 2147483647.0}};
    static constexpr ObjectiveFieldSpec flag[] = {{"FlagID", "st", 3}, {"Value", "b", 2}};
    static constexpr ActionSpec specs[] = {
        {ActionSpecies::bik, "PlayBIK", bik}, {ActionSpecies::wav, "PlayWAV", wav},
        {ActionSpecies::text, "DisplayTextMessage", text},
        {ActionSpecies::technology, "MakeNewTechnologyAvailable", technology},
        {ActionSpecies::resourceText, "DisplayResourceTextMessage", resource},
        {ActionSpecies::flag, "SetBooleanFlag", flag}};
    return std::span{specs}.first(creation ? 6 : 5);
}

std::string objectivePayloadWire(const ObjectiveFieldSpec& field, const Scalar& value)
{
    auto encoded = value;
    // FIT assembles multiline text differently unless the opening line is empty.
    if (auto* text = std::get_if<std::string>(&encoded); text && text->find('\n') != text->npos) {
        std::string wire = "\r\n";
        for (const char c : *text) { if (c == '\n') wire += '\r'; wire += c; }
        *text = std::move(wire);
    }
    Field spelling; spelling.prefix = field.prefix;
    return field.type == 1 ? std::format("{:.9g}", std::get<double>(value)) : formatValue(spelling, Value{encoded});
}

std::expected<std::string, LoadError> encodeActionDraft(const ActionDraft& draft)
{
    const auto schema = actionSchema(true);
    const auto spec = std::ranges::find(schema, draft.species, &ActionSpec::species);
    const auto reject = [](std::string reason) { return std::unexpected(LoadError{"action", std::move(reason), 0}); };
    if (spec == schema.end()) return reject("unsupported_action_creation");
    if (draft.payload.size() != spec->fields.size()) return reject("required_action_payload");
    if (draft.flagIndex && draft.species != ActionSpecies::flag) return reject("unexpected_FlagIndex");
    std::string result;
    for (std::size_t i = 0; i < spec->fields.size(); ++i) {
        auto field = spec->fields[i];
        if (draft.flagIndex && i == 0) field = {"FlagIndex", "ul", 0, -2147483648.0, 2147483647.0};
        auto value = draft.payload[i];
        if (auto* text = std::get_if<std::string>(&value)) {
            for (auto at = text->find("\r\n"); at != text->npos; at = text->find("\r\n", at)) text->erase(at, 1);
            if (text->empty() && field.key != "FlagID") return reject("required_action_text");
        }
        if (auto valid = validateObjectiveScalar(field, value); !valid) return std::unexpected(valid.error());
        const auto wire = objectivePayloadWire(field, value);
        const auto declaration = std::string(field.prefix) + " " + std::string(field.key) + " = " + wire + "\r\n";
        auto check = decodeFit("FITini\r\n[Team0Objective0Action0]\r\n" + declaration + "FITend\r\n");
        if (!check || check->fields.size() != 1 || check->fields[0].opaque ||
            check->fields[0].warning == "reader_string_buffer_limit" || check->fields[0].value != Value{value})
            return reject("reader_conversion_changes_value");
        result += declaration;
    }
    if (auto valid = validateObjectiveBlock("[Action]\r\nst ActionSpeciesString = \"" + std::string(spec->name) + "\"\r\n" + result); !valid)
        return std::unexpected(valid.error());
    return result;
}

std::span<const ObjectiveFieldSpec> objectivePayloadSchema(const ObjectiveEntry& entry)
{
    if (const auto* species = std::get_if<ConditionSpecies>(&entry.species)) {
        const auto schema = conditionSchema();
        const auto spec = std::ranges::find(schema, *species, &ConditionSpec::species);
        if (spec != schema.end() && !spec->reference) return spec->fields;
    }
    if (const auto* species = std::get_if<ActionSpecies>(&entry.species)) {
        const auto schema = actionSchema();
        const auto spec = std::ranges::find(schema, *species, &ActionSpec::species);
        if (spec != schema.end()) return spec->fields;
    }
    return {};
}

std::span<const ConditionSpec> conditionSchema()
{
    static constexpr ObjectiveFieldSpec num[] = {{"Num", "ul", 0, -2147483648.0, 2147483647.0}};
    constexpr auto real = std::numeric_limits<float>::max();
    static constexpr ObjectiveFieldSpec time[] = {{"Time", "f", 1, -real, real}};
    static constexpr ObjectiveFieldSpec area[] = {
        {"TargetCenterX", "f", 1, -real, real}, {"TargetCenterY", "f", 1, -real, real},
        {"TargetRadius", "f", 1, -real, real}};
    static constexpr ObjectiveFieldSpec xy[] = {
        {"PositionX", "f", 1, -real, real}, {"PositionY", "f", 1, -real, real}};
    static constexpr ConditionSpec specs[] = {
        {ConditionSpecies::destroyAll, "DestroyAllEnemyUnits", {}},
        {ConditionSpecies::captureOrDestroyAll, "CaptureOrDestroyAllEnemyUnits", {}},
        {ConditionSpecies::deadOrFledAll, "DeadOrFledAllEnemyUnits", {}},
        {ConditionSpecies::destroyNumber, "DestroyNumberOfEnemyUnits", num},
        {ConditionSpecies::captureOrDestroyNumber, "CaptureOrDestroyNumberOfEnemyUnits", num},
        {ConditionSpecies::deadOrFledNumber, "DeadOrFledNumberOfEnemyUnits", num},
        {ConditionSpecies::elapsed, "ElapsedMissionTime", time},
        {ConditionSpecies::moveAny, "MoveAnyUnitToArea", area},
        {ConditionSpecies::moveAll, "MoveAllUnitsToArea", area},
        {ConditionSpecies::moveSurvivors, "MoveAllSurvivingUnitsToArea", area},
        {ConditionSpecies::moveMechs, "MoveAllSurvivingMechsToArea", area},
        {ConditionSpecies::destroyUnit, "DestroySpecificEnemyUnit", xy, true},
        {ConditionSpecies::destroyStructure, "DestroySpecificStructure", xy, true},
        {ConditionSpecies::captureOrDestroyUnit, "CaptureOrDestroySpecificEnemyUnit", xy, true},
        {ConditionSpecies::captureOrDestroyStructure, "CaptureOrDestroySpecificStructure", xy, true},
        {ConditionSpecies::deadOrFledUnit, "DeadOrFledSpecificEnemyUnit", xy, true},
        {ConditionSpecies::captureUnit, "CaptureSpecificUnit", xy, true},
        {ConditionSpecies::captureStructure, "CaptureSpecificStructure", xy, true},
        {ConditionSpecies::guardUnit, "GuardSpecificUnit", xy, true},
        {ConditionSpecies::guardStructure, "GuardSpecificStructure", xy, true}};
    return specs;
}

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
    const FitReader reader(syntax_.originalBytes);
    const auto& block = section(item.section)->name;
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
            } else if (spec.type == 3 && field->warning == "reader_string_buffer_limit")
                warning = "ignored by the game";
            else if (field->opaque || field->value.size() != 1)
                return std::unexpected(LoadError{section(item.section)->name + "/" + std::string(spec.key), "malformed_value", 0});
            else {
                value = field->value[0];
                if (field->warning.starts_with("outside signed")) warning = field->warning;
            }
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
        auto actual = reader.read(block, spec.prefix, spec.key);
        if (!actual && spec.key == "PreviousPrimaryObjectiveMustBeComplete") actual = reader.read(block, "b", "PreviousObjectiveMustBeComplete");
        if (actual) value = std::move(*actual);
        else {
            if (spec.type == 0) value = spec.integerDefault;
            else if (spec.type == 1) value = static_cast<double>(spec.integerDefault);
            else if (spec.type == 2) value = false;
            else value = spec.key == "Description" ? item.scalars.front().value : Scalar{std::string{}};
            if (field) warning = "ignored by the game";
            if (spec.key == "ActivateFlagID")
                if (auto index = reader.read(block, "ul", "ActivateFlagIndex")) value = std::to_string(std::get<std::int64_t>(*index));
        }
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
    const auto requested = value;
    if (auto* text = std::get_if<std::string>(&value); text && text->find('\n') != text->npos) {
        // FIT text needs an empty opening line and CRLF for lossless disk/RAM reads.
        std::string wire = "\r\n";
        for (const char c : *text) { if (c == '\n') wire += '\r'; wire += c; }
        *text = std::move(wire);
    }
    if (spec->type == 3) {
        auto fit = document_.fitBytes();
        const auto* section = document_.section(id);
        Field spelling; spelling.prefix = spec->prefix;
        const auto wire = formatValue(spelling, Value{value});
        if (const auto* field = document_.find(id, actualKey)) {
            fit.replace(field->syntax.begin, field->syntax.end - field->syntax.begin, wire);
            if (field->prefix != spec->prefix) {
                auto begin = fit.rfind('\n', field->syntax.begin);
                begin = begin == fit.npos ? 0 : begin + 1;
                while (fit[begin] == ' ' || fit[begin] == '\t') ++begin;
                fit.replace(begin, field->prefix.size(), spec->prefix);
            }
        } else {
            const auto nl = fit.find("\r\n") == fit.npos ? "\n" : "\r\n";
            fit.insert(section->syntax.end, std::string(nl) + std::string(spec->prefix) + " " + std::string(actualKey) + " = " + wire + nl);
        }
        auto decoded = decodeFit(fit);
        if (!decoded) return reject(decoded.error().reason);
        const auto& block = decoded->sections[static_cast<std::size_t>(section - document_.syntax().sections.data())];
        if (auto valid = validateObjectiveBlock(std::string_view(fit).substr(block.syntax.begin, block.syntax.end - block.syntax.begin)); !valid)
            return reject(valid.error().reason);
        const auto& fields = decoded->sections[static_cast<std::size_t>(section - document_.syntax().sections.data())].fields;
        const auto field = std::ranges::find_if(decoded->fields, [&](const Field& field) {
            return field.name == actualKey && std::ranges::find(fields, field.id) != fields.end();
        });
        if (field == decoded->fields.end() || field->opaque) return reject("reader_string_buffer_limit");
        if (field->value != Value{requested}) return reject("reader_conversion_changes_value");
        return commitObjectiveFit(std::move(fit), "Objective " + std::string(key));
    }
    auto result = setScalar(id, actualKey, spec->prefix, std::move(value), "Objective " + std::string(key), true);
    if (!result && result.error().reason == "history_budget") return reject(result.error().reason);
    return result;
}
}
