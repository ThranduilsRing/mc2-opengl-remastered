#include "PanelProof.h"
#include <algorithm>
#include <stdexcept>

namespace editor::shell {
using namespace document;
void PanelProof::objectiveCrudProof()
{
    if (document_.objectives().size() < 3 || document_.objectives()[2].team != 0)
        throw std::runtime_error("objective_crud_fixture_missing");
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
    const auto id = document_.objectives()[1].section;
    // Model-name edits need a camera-supported type even when the input has no model.
    if (!history_.setObjectiveScalar(id, "ModelType", std::int64_t{14})) throw std::runtime_error("objective_fixture_type_failed");
    saveCheck("fixture");
    files_ = std::make_unique<FileWorkflow>(document_, history_, panels_, output_ / "widget-save", log_);
    click("objective.toggle"); click("objective.select"); click("objective.select." + std::to_string(id.value));
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
    // The final redo supplies a changed widget save; undo makes reopen replace dirty state.
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
