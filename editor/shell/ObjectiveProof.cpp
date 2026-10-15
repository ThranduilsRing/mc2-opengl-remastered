#include "PanelProof.h"
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <format>
#include <cmath>

namespace editor::shell {
using namespace document;
void PanelProof::objectiveCrudProof()
{
    if (document_.objectives().size() < 3 || document_.objectives()[2].team != 0)
        throw std::runtime_error("objective_crud_fixture_missing");
    const auto seed = document_.objectives()[1].section;
    for (std::size_t list = 0; list < 4; ++list) {
        while (!document_.objective(seed)->entries[list].empty())
            if (!history_.deleteObjectiveEntry(document_.objective(seed)->entries[list].back().section))
                throw std::runtime_error("objective_entry_fixture_delete_failed");
        for (int i = 0; i < 2; ++i) {
            const auto entry = history_.addObjectiveEntry(seed, list);
            if (!entry || !history_.setObjectiveFlag(*entry, std::string(i ? "e6c-second" : "e6c-first"), 0, i != 0))
                throw std::runtime_error("objective_entry_fixture_add_failed");
        }
    }
    if (!history_.replace(document_.fitBytes(), {document_.pakBytes().begin(), document_.pakBytes().end()}, "entry-fixture", log_))
        throw std::runtime_error("objective_entry_fixture_replace_failed");
    const auto id = document_.objectives()[1].section;
    const auto count = document_.objectives().size();
    saveCheck("fixture");
    files_ = std::make_unique<FileWorkflow>(document_, history_, panels_, output_ / "widget-save", log_);
    click("objective.toggle");
    operation("objective-add", "objective.add", [this, count] {
        if (document_.objectives().size() != count + 1) throw std::runtime_error("objective_add_widget_wrong");
    });
    const auto select = [this, id] { click("objective.select"); click("objective.select." + std::to_string(id.value)); };
    select();
    operation("objective-copy", "objective.copy", [this, count] {
        if (document_.objectives().size() != count + 1) throw std::runtime_error("objective_copy_widget_wrong");
    });
    select();
    operation("objective-delete", "objective.delete", [this, id, count] {
        if (document_.objective(id) || document_.objectives().size() != count - 1) throw std::runtime_error("objective_delete_widget_wrong");
    });
    select();
    operation("objective-up", "objective.up", [this, id] {
        if (document_.objective(id)->exportedIndex != 0) throw std::runtime_error("objective_up_widget_wrong");
    });
    select();
    operation("objective-down", "objective.down", [this, id] {
        if (document_.objective(id)->exportedIndex != 2) throw std::runtime_error("objective_down_widget_wrong");
    });
    select(); click("objective.entries");
    for (std::size_t list = 0; list < 4; ++list) {
        const auto key = "objective.entry." + std::to_string(list);
        const auto name = "entry-" + std::to_string(list);
        click("objective.list." + std::to_string(list));
        operation(name + "-add", key + ".add", [this, id, list] {
            const auto& entries = document_.objective(id)->entries[list];
            if (entries.size() != 3 || !entries.back().flag || entries.back().flag->id != "" || !entries.back().flag->value)
                throw std::runtime_error("objective_entry_add_widget_wrong");
        });
        click(key + ".select.0");
        input(key + ".id", "e6c-edited-" + std::to_string(list)); click(key + ".value");
        operation(name + "-flag", key + ".apply", [this, id, list] {
            const auto& flag = document_.objective(id)->entries[list][0].flag;
            if (!flag || flag->id != "e6c-edited-" + std::to_string(list) || !flag->value)
                throw std::runtime_error("objective_flag_widget_wrong");
        });
        operation(name + "-delete", key + ".delete", [this, id, list] {
            const auto& entries = document_.objective(id)->entries[list];
            if (entries.size() != 1 || entries[0].flag->id != "e6c-second")
                throw std::runtime_error("objective_entry_delete_widget_wrong");
        });
        click(key + ".select.1");
        operation(name + "-up", key + ".up", [this, id, list] {
            if (document_.objective(id)->entries[list][0].flag->id != "e6c-second")
                throw std::runtime_error("objective_entry_up_widget_wrong");
        });
        click(key + ".select.0");
        operation(name + "-down", key + ".down", [this, id, list] {
            if (document_.objective(id)->entries[list][1].flag->id != "e6c-first")
                throw std::runtime_error("objective_entry_down_widget_wrong");
        });
    }
    click("redo", [this] { beforeFit_ = document_.fitBytes(); beforePak_ = {document_.pakBytes().begin(), document_.pakBytes().end()}; });
    click("save", [this] {
        auto saved = MissionDocument::read(output_ / "widget-save.fit", output_ / "widget-save.pak", log_, document_.markerObjects());
        if (!saved || saved->fitBytes() != beforeFit_) throw std::runtime_error("objective_crud_save_widget_wrong");
    });
    click("undo"); click("reopen");
    click("dirty.discard", [this] {
        if (document_.fitBytes() != beforeFit_ || history_.canUndo() || history_.canRedo())
            throw std::runtime_error("objective_crud_reopen_widget_wrong");
    });
}
void PanelProof::objectivesProof()
{
    if (document_.objectives().size() < 2) throw std::runtime_error("objective_fixture_missing");
    const auto owner = document_.objectives()[1];
    const auto base = document_.section(owner.section)->name;
    const Entity* target{};
    for (const auto& unit : document_.entities()) if (unit.exportedPart) {
        const auto close = [&](const Entity& other) {
            if (!other.exportedPart) return false;
            for (int axis = 0; axis < 2; ++axis) {
                const auto a = static_cast<float>(unit.position[axis]), b = static_cast<float>(other.position[axis]);
                if (a != b && std::abs(a - b) / std::max(std::abs(a), std::abs(b)) >= 0.00001) return false;
            }
            return true;
        };
        if (std::ranges::count_if(document_.entities(), close) == 1) { target = &unit; break; }
    }
    if (!target) throw std::runtime_error("objective_unique_fixture_target_missing");
    const auto part = *target->exportedPart;
    const auto x = target->position[0], y = target->position[1];
    auto fit = document_.fitBytes();
    struct Edit { Span span; std::string text; };
    std::vector<Edit> edits;
    for (const auto& entry : owner.entries[0]) edits.push_back({document_.section(entry.section)->syntax, {}});
    edits.push_back({document_.find(owner.section, "NumConditions")->syntax, "5"});
    const auto parts = std::ranges::count_if(document_.entities(), [](const Entity& e) { return e.exportedPart.has_value(); });
    edits.push_back({document_.find("Parts", "NumParts")->syntax, std::to_string(parts + 2)});
    std::string added;
    for (int i = 1; i <= 2; ++i) added += std::format("[Part{}]\r\nul ObjectNumber = 1\r\nf PositionX = 1000000\r\nf PositionY = 1000000\r\nc TeamID = 0\r\n", parts + i);
    for (int i = 0; i < 5; ++i) {
        added += std::format("[{}Condition{}]\r\nst ConditionSpeciesString = \"{}\"\r\n", base, i,
            i == 4 ? "VendorPredicate" : "DestroySpecificEnemyUnit");
        if (i == 3) added += "l MoverWID = 7\r\n";
        else if (i != 4) added += std::format("f PositionX = {:.9g}\r\nf PositionY = {:.9g}\r\nul Commander = 007\r\n",
            i == 0 ? -1e30 : i == 1 ? 1000000.0 : x, i == 0 ? -1e30 : i == 1 ? 1000000.0 : y);
    }
    edits.push_back({{document_.syntax().endMarker, document_.syntax().endMarker}, added});
    std::ranges::sort(edits, [](const Edit& a, const Edit& b) { return a.span.begin > b.span.begin; });
    for (const auto& edit : edits) fit.replace(edit.span.begin, edit.span.end - edit.span.begin, edit.text);
    if (!history_.replace(std::move(fit), {document_.pakBytes().begin(), document_.pakBytes().end()}, "objective-fixture", log_))
        throw std::runtime_error("objective_reference_fixture_failed");
    const auto id = document_.objectives()[1].section;
    EntityId targetId;
    for (const auto& entity : document_.entities()) if (entity.exportedPart == part) targetId = entity.id;
    // Model-name edits require a camera-supported type.
    if (!history_.setObjectiveScalar(id, "ModelType", std::int64_t{14})) throw std::runtime_error("objective_fixture_type_failed");
    const auto payloadOwner = history_.addObjective(0);
    if (!payloadOwner) throw std::runtime_error("objective_payload_fixture_failed");
    for (const auto list : {0U, 2U}) for (const auto species : {ConditionSpecies::destroyNumber, ConditionSpecies::elapsed, ConditionSpecies::moveAny})
        if (!history_.addObjectiveCondition(*payloadOwner, list, species)) throw std::runtime_error("objective_payload_fixture_failed");
    saveCheck("fixture");
    files_ = std::make_unique<FileWorkflow>(document_, history_, panels_, output_ / "widget-save", log_);
    click("objective.toggle"); click("objective.select"); click("objective.select." + std::to_string(id.value));
    const auto rows = [this](const std::string& stem) {
        std::ofstream out(output_ / (stem + ".tsv"), std::ios::binary);
        for (const auto& row : panels_.objectiveRows()) out << row << '\n';
    };
    click("", [rows] { rows("objective-rows-before"); });
    const auto entry = document_.objective(id)->entries[0][0].section;
    const auto resolve = "objective.resolve." + std::to_string(entry.value);
    click(resolve);
    operation("objective-resolve", resolve + "." + std::to_string(targetId.value), [this, id, targetId] {
        if (document_.objective(id)->entries[0][0].reference.target != targetId)
            throw std::runtime_error("objective_resolve_widget_wrong");
    });
    click("redo");
    click("", [rows] { rows("objective-rows-resolved"); });
    click("undo");
    for (const auto& spec : objectiveSchema()) {
        const auto old = document_.objectiveScalar(id, spec.key)->value;
        Scalar value;
        if (spec.type == 0) value = std::int64_t{spec.key == "Priority" ? 2 : spec.key == "ModelType" ? 13 : 17};
        else if (spec.type == 1) value = 123.25;
        else if (spec.type == 2) value = !std::get<bool>(old);
        else value = std::string(spec.key == "Title" || spec.key == "Description" ? "E6a objective\n\nsecond line\n" : "E6a objective scalar");
        if (value == old) throw std::runtime_error("objective_fixture_already_equals_expected");
        if (spec.type == 2) click("objective.value");
        else input("objective.value", spec.type == 3 ? std::get<std::string>(value) : spec.type == 1 ? "123.25" : spec.key == "Priority" ? "2" : spec.key == "ModelType" ? "13" : "17");
        std::string name = "objective-" + std::string(spec.key);
        std::ranges::replace(name, ' ', '_');
        operation(name, "objective.apply", [this, id, key = spec.key, value] {
            const auto* field = document_.objectiveScalar(id, key);
            if (!field || field->value != value) throw std::runtime_error("objective_widget_value_wrong");
        });
        click("objective.next");
    }
    click("objective.select"); click("objective.select." + std::to_string(payloadOwner->value));
    click("objective.entries");
    for (const auto list : {0U, 2U}) {
        click("objective.list." + std::to_string(list));
        const auto& entries = document_.objective(*payloadOwner)->entries[list];
        for (std::size_t e = 0; e < entries.size(); ++e) {
            const auto entryId = entries[e].section;
            const auto species = std::get<ConditionSpecies>(entries[e].species);
            const auto spec = std::ranges::find(conditionSchema(), species, &ConditionSpec::species);
            click("objective.entry." + std::to_string(list) + ".select." + std::to_string(e));
            for (const auto& field : spec->fields) {
                const auto key = "objective.entry." + std::to_string(list);
                input(key + "." + std::string(field.key), field.type == 0 ? "-17" : "-1.25");
                operation(std::format("widget-payload-{}-{}", list, field.key), key + ".apply." + std::string(field.key),
                    [this, entryId, field] {
                        const Scalar value = field.type == 0 ? Scalar{std::int64_t{-17}} : Scalar{-1.25};
                        if (document_.find(entryId, field.key)->value != Value{value}) throw std::runtime_error("objective_payload_widget_wrong");
                    });
            }
        }
        const auto key = "objective.entry." + std::to_string(list);
        click(key + ".species"); click(key + ".species.DestroySpecificEnemyUnit");
        click(key + ".target"); click(key + ".target." + std::to_string(targetId.value));
        operation(std::format("widget-payload-{}-create", list), key + ".add", [this, payloadOwner, list, targetId] {
            const auto& created = document_.objective(*payloadOwner)->entries[list];
            if (created.size() != 4 || std::get<ConditionSpecies>(created.back().species) != ConditionSpecies::destroyUnit ||
                created.back().reference.status != ReferenceStatus::Resolved || created.back().reference.target != targetId)
                throw std::runtime_error("objective_condition_creation_widget_wrong");
        });
    }
    click("redo", [this] { beforeFit_ = document_.fitBytes(); beforePak_ = {document_.pakBytes().begin(), document_.pakBytes().end()}; });
    click("save", [this] {
        auto saved = MissionDocument::read(output_ / "widget-save.fit", output_ / "widget-save.pak", log_, document_.markerObjects());
        if (!saved || saved->fitBytes() != beforeFit_) throw std::runtime_error("objective_save_widget_wrong");
    });
    click("undo"); click("reopen");
    click("dirty.discard", [this] {
        if (document_.fitBytes() != beforeFit_ || std::vector<std::byte>(document_.pakBytes().begin(), document_.pakBytes().end()) != beforePak_ ||
            history_.canUndo() || history_.canRedo()) throw std::runtime_error("objective_reopen_widget_wrong");
    });
}
}
