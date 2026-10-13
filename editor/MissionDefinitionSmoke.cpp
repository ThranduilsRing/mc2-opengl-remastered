// Deliberately non-catalog fixture values expose preservation defects that the
// matching stock catalog cannot reveal. Fixtures live only in the smoke runtime.
#include "EditorInterface_internal.h"
#include "EditorMFC.h"

namespace
{
std::string readFixture(const char* path)
{
    File file;
    if (file.open(path) != NO_ERR) return {};
    std::string text(file.getLength(), '\0');
    file.read(reinterpret_cast<MemoryPtr>(text.data()), static_cast<long>(text.size()));
    return text;
}
bool writeFixture(const char* path, const std::string& text)
{
    File file;
    if (file.create(path) != NO_ERR) return false;
    file.write(reinterpret_cast<MemoryPtr>(const_cast<char*>(text.data())), text.size());
    return true;
}
bool replaceValue(std::string& text, const char* block, const char* field, const char* value)
{
    const auto begin = text.find(std::string("[") + block + "]");
    if (begin == std::string::npos) return false;
    const auto nextBlock = text.find("\n[", begin + 1);
    const auto key = text.find(std::string(" ") + field + " = ", begin);
    if (key == std::string::npos || (nextBlock != std::string::npos && key >= nextBlock)) return false;
    const auto start = text.find('=', key) + 2;
    const auto end = text.find_first_of("\r\n", start);
    if (end == std::string::npos) return false;
    text.replace(start, end - start, value);
    return true;
}
}

bool EditorInterface::runDefinitionSmoke()
{
    constexpr const char* pak = "terrain_gen_out/ew_definitions.pak";
    constexpr const char* fit = "terrain_gen_out/ew_definitions.fit";
    if (!EditorData::instance->save(pak, true)) return false;
    const std::string original = readFixture(fit);
    std::string authored = original;
    bool ok = replaceValue(authored, "Part1", "ControlDataType", "7")
        && replaceValue(authored, "Part1", "ObjectProfile", "\"Authored_Profile\"")
        && replaceValue(authored, "Part1", "CSVFile", "\"Authored_CASE\"")
        && replaceValue(authored, "Team0Objective0", "ObjectiveModel", "\"Authored_Model\"")
        && replaceValue(authored, "Team0Objective0", "ModelType", "17")
        && replaceValue(authored, "Team0Objective0", "ModelScale", "1.75")
        && replaceValue(authored, "Cameras", "NewScale", "0.375")
        && replaceValue(authored, "Cameras", "ZoomMax", "1.25");
    if (!ok || !writeFixture(fit, authored) || !EditorData::initTerrainFromPCV(pak)) return false;
    auto getUnit = [] { return *EditorObjectMgr::instance()->getUnits().Begin(); };
    auto getObjective = [] { return *EditorData::instance->TeamsRef().TeamRef(0).ObjectivesRef().Begin(); };
    const auto storedPart = getUnit()->partDefinition();
    const auto storedModel = getObjective()->ModelDefinition();
    ok = storedPart && storedPart->controlDataType == 7 && storedPart->objectProfile == "Authored_Profile"
        && storedPart->csvFile == "Authored_CASE" && storedModel.type == 17 && storedModel.name == "Authored_Model"
        && storedModel.scale == 1.75f;
    ok = EditorData::instance->save("terrain_gen_out/ew_preserved.pak", true) && ok;
    ok = EditorData::initTerrainFromPCV("terrain_gen_out/ew_preserved.pak") && ok;
    ok = getUnit()->partDefinition() == storedPart && getObjective()->ModelDefinition() == storedModel && ok;
    auto& camera = *static_cast<EditorCamera*>(eye);
    const auto storedView = camera.missionView;
    ok = storedView && storedView->scale == 0.375f && storedView->zoomMaximum == 1.25f && ok;
    fprintf(stderr, "[EDITOR_DEFINITIONS] stage=preserved_reload ok=%d\n", ok);

    // Coordinate writers must follow intentional transforms and recover the
    // original FIT reference on undo, including its sub-cell precision.
    const auto verifyTransform = [this](EditorObject* target, auto writeReference, const char* block, const char* xKey, const char* yKey) {
        if (!target) return false;
        const auto readSaved = [&](float& x, float& y) {
            FitIniFile file;
            if (file.create("terrain_gen_out/ew_reference.fit") != NO_ERR) return false;
            if (!writeReference(file)) return false;
            file.close();
            if (file.open("terrain_gen_out/ew_reference.fit") != NO_ERR || file.seekBlock(block) != NO_ERR) return false;
            return EditorFit::floatValue(file, xKey, x) && EditorFit::floatValue(file, yKey, y);
        };
        float beforeX = 0, beforeY = 0, movedX = 0, movedY = 0, undoneX = 0, undoneY = 0;
        const auto position = target->getPosition();
        if (!readSaved(beforeX, beforeY) || !applyObjectTransform(target, position.x + 64, position.y + 64, target->appearance()->rotation)) return false;
        const bool moved = readSaved(movedX, movedY) && movedX == target->getPosition().x && movedY == target->getPosition().y;
        return undoMgr.Undo() && readSaved(undoneX, undoneY) && undoneX == beforeX && undoneY == beforeY && moved;
    };
    NavMarker* marker = nullptr;
    const auto buildings = EditorObjectMgr::instance()->getBuildings();
    for (auto it = buildings.Begin(); !it.IsDone() && !marker; it++) marker = dynamic_cast<NavMarker*>(*it);
    ok = verifyTransform(marker, [marker](FitIniFile& file) { return marker->save(&file, 0); },
        "NavMarker0", "xPos", "yPos") && ok;
    CSpecificStructureObjectiveCondition* targetCondition = nullptr;
    auto& objectives = EditorData::instance->TeamsRef().TeamRef(0).ObjectivesRef();
    for (auto objective = objectives.Begin(); !objective.IsDone() && !targetCondition; objective++)
        for (auto condition = (*objective)->Begin(); !condition.IsDone() && !targetCondition; condition++)
            targetCondition = dynamic_cast<CSpecificStructureObjectiveCondition*>(*condition);
    ok = verifyTransform(targetCondition ? targetCondition->BuildingPtr() : nullptr,
        [targetCondition](FitIniFile& file) { file.writeBlock("Reference"); return targetCondition->Save(&file); },
        "Reference", "PositionX", "PositionY") && ok;
    fprintf(stderr, "[EDITOR_DEFINITIONS] stage=coordinate_transform_undo ok=%d\n", ok);

    if (!marker) return false;
    const auto markerPosition = marker->getPosition();
    const bool markerMoved = applyObjectTransform(marker, markerPosition.x + 64, markerPosition.y + 64, marker->appearance()->rotation);
    const auto authoredMarkerPosition = marker->getPosition();
    bool markerReloaded = markerMoved && EditorData::instance->save("terrain_gen_out/ew_moved.pak", true)
        && EditorData::initTerrainFromPCV("terrain_gen_out/ew_moved.pak");
    bool foundMovedMarker = false;
    const auto loadedBuildings = EditorObjectMgr::instance()->getBuildings();
    for (auto it = loadedBuildings.Begin(); !it.IsDone(); it++)
        if (dynamic_cast<NavMarker*>(*it) && (*it)->getPosition().x == authoredMarkerPosition.x
            && (*it)->getPosition().y == authoredMarkerPosition.y) foundMovedMarker = true;
    markerReloaded = markerReloaded && foundMovedMarker;
    ok = EditorData::initTerrainFromPCV("terrain_gen_out/ew_preserved.pak") && markerReloaded && ok;
    fprintf(stderr, "[EDITOR_DEFINITIONS] stage=marker_transform_reload ok=%d\n", ok);

    handleNewMenuMessage(ID_MISSION_AUTHOR_CAMERA_ZOOM);
    const auto authoredView = camera.missionView;
    ok = authoredView != storedView && undoMgr.Undo() && camera.missionView == storedView
        && undoMgr.Redo() && camera.missionView == authoredView && undoMgr.Undo() && ok;
    fprintf(stderr, "[EDITOR_DEFINITIONS] stage=camera_command_undo_redo ok=%d\n", ok);
    handleNewMenuMessage(ID_MISSION_REFRESH_DEFINITIONS);
    const auto refreshedPart = getUnit()->partDefinition();
    const auto refreshedModel = getObjective()->ModelDefinition();
    ok = refreshedPart != storedPart && refreshedModel != storedModel && ok;
    ok = undoMgr.Undo() && getUnit()->partDefinition() == storedPart
        && getObjective()->ModelDefinition() == storedModel && ok;
    ok = undoMgr.Redo() && getUnit()->partDefinition() == refreshedPart
        && getObjective()->ModelDefinition() == refreshedModel && ok;
    ok = EditorData::instance->save("terrain_gen_out/ew_refreshed.pak", true) && ok;
    ok = EditorData::initTerrainFromPCV("terrain_gen_out/ew_refreshed.pak")
        && getUnit()->partDefinition() == refreshedPart && getObjective()->ModelDefinition() == refreshedModel && ok;
    fprintf(stderr, "[EDITOR_DEFINITIONS] stage=catalog_command_undo_redo_reload ok=%d\n", ok);

    // Each malformed document must fail at its reader, before a save can bless it.
    struct BadLiteral { const char* block; const char* field; const char* value; };
    for (const auto& bad : {
        BadLiteral{"Part1", "ControlDataType", "7garbage"},
        BadLiteral{"Part1", "ObjectProfile", "unquoted"},
        BadLiteral{"Part1", "CSVFile", "unquoted"},
        BadLiteral{"Team0Objective0", "ModelType", "7garbage"},
        BadLiteral{"Cameras", "NewScale", "nan"},
        BadLiteral{"NavMarker0", "radius", "-1"},
        BadLiteral{"Team0Objective1Condition0", "PositionY", "nan"}})
    {
        std::string malformed = original;
        ok = replaceValue(malformed, bad.block, bad.field, bad.value) && writeFixture(fit, malformed)
            && !EditorData::initTerrainFromPCV(pak) && ok;
        fprintf(stderr, "[EDITOR_DEFINITIONS] stage=reject field=%s ok=%d\n", bad.field, ok);
    }
    const bool restored = writeFixture(fit, original) && EditorData::initTerrainFromPCV(pak);
    fprintf(stderr, "[EDITOR_DEFINITIONS] preservation_commands_undo_reload_malformed=%d\n", ok && restored);
    return ok && restored;
}
