// Deliberately non-catalog fixture values expose preservation defects that the
// matching stock catalog cannot reveal. Fixtures live only in the smoke runtime.
#include "EditorInterface_internal.h"
#include "EditorMFC.h"

extern char missionScriptName[1024];

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
bool replaceValue(std::string& text, const char* block, const char* field, std::string_view value)
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
bool removeField(std::string& text, const char* block, const char* field)
{
    if (!replaceValue(text, block, field, "")) return false;
    const auto begin = text.find(std::string("[") + block + "]");
    const auto key = text.find(std::string(" ") + field + " = ", begin);
    const auto start = text.rfind('\n', key) + 1;
    const auto end = text.find('\n', key);
    text.erase(start, end - start + 1);
    return true;
}
void addDeclaration(std::string& text, const char* block, std::string_view declaration)
{
    const auto begin = text.find(std::string("[") + block + "]");
    const auto end = text.find('\n', begin);
    text.insert(end + 1, std::string(declaration) + "\n");
}
}

bool EditorInterface::runDefinitionSmoke()
{
    constexpr const char* pak = "terrain_gen_out/ew_definitions.pak";
    constexpr const char* fit = "terrain_gen_out/ew_definitions.fit";
    if (!EditorData::instance->save(pak, true)) return false;
    const std::string original = readFixture(fit);
    const std::string originalPak = readFixture(pak);
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
    auto getUnit = [] { return EditorObjectMgr::instance()->getUnits().front(); };
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
    fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=preserved_reload ok=%d\n", ok);

    // Coincident units can carry different definitions. Each instance must
    // recover its own authored values even when every old lookup key matches.
    auto* first = getUnit();
    auto* second = dynamic_cast<Unit*>(EditorObjectMgr::instance()->addBuilding(first->getPosition(),
        first->getGroup(), first->getIndexInGroup(), first->getAlignment(), first->appearance()->rotation, 1, false));
    if (!second) return false;
    second->setSquad(first->getSquad());
    const PartDefinition secondDefinition{9, "Second_Profile", "Second_CASE"};
    second->restorePartDefinition(secondDefinition);
    const bool distinct = first->instanceId() != second->instanceId()
        && first->getID() == second->getID() && first->getSquad() == second->getSquad()
        && first->getPosition().x == second->getPosition().x && first->getPosition().y == second->getPosition().y;
    handleNewMenuMessage(ID_MISSION_REFRESH_DEFINITIONS);
    const auto firstRefreshed = first->partDefinition(), secondRefreshed = second->partDefinition();
    const bool duplicateProof = distinct && firstRefreshed != storedPart && secondRefreshed != secondDefinition
        && undoMgr.Undo() && first->partDefinition() == storedPart && second->partDefinition() == secondDefinition
        && undoMgr.Redo() && first->partDefinition() == firstRefreshed && second->partDefinition() == secondRefreshed
        && undoMgr.Undo() && first->partDefinition() == storedPart && second->partDefinition() == secondDefinition;
    ok = EditorObjectMgr::instance()->deleteObject(second) && duplicateProof && ok;
    fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=duplicate_unit_refresh_undo_redo ok=%d\n", ok);

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
    for (auto it = buildings.begin(); it != buildings.end() && !marker; it++) marker = dynamic_cast<NavMarker*>(*it);
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
    fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=coordinate_transform_undo ok=%d\n", ok);

    if (!marker) return false;
    const auto markerPosition = marker->getPosition();
    const bool markerMoved = applyObjectTransform(marker, markerPosition.x + 64, markerPosition.y + 64, marker->appearance()->rotation);
    const auto authoredMarkerPosition = marker->getPosition();
    bool markerReloaded = markerMoved && EditorData::instance->save("terrain_gen_out/ew_moved.pak", true)
        && EditorData::initTerrainFromPCV("terrain_gen_out/ew_moved.pak");
    bool foundMovedMarker = false;
    const auto loadedBuildings = EditorObjectMgr::instance()->getBuildings();
    for (auto it = loadedBuildings.begin(); it != loadedBuildings.end(); it++)
        if (dynamic_cast<NavMarker*>(*it) && (*it)->getPosition().x == authoredMarkerPosition.x
            && (*it)->getPosition().y == authoredMarkerPosition.y) foundMovedMarker = true;
    markerReloaded = markerReloaded && foundMovedMarker;
    ok = EditorData::initTerrainFromPCV("terrain_gen_out/ew_preserved.pak") && markerReloaded && ok;
    fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=marker_transform_reload ok=%d\n", ok);

    handleNewMenuMessage(ID_MISSION_AUTHOR_CAMERA_ZOOM);
    const auto authoredView = camera.missionView;
    ok = authoredView != storedView && undoMgr.Undo() && camera.missionView == storedView
        && undoMgr.Redo() && camera.missionView == authoredView && undoMgr.Undo() && ok;
    fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=camera_command_undo_redo ok=%d\n", ok);
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
    fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=catalog_command_undo_redo_reload ok=%d\n", ok);

    // Oversized fields are ignored without discarding the rest of the mission.
    const auto expectedObjectives = EditorData::instance->TeamsRef().TeamRef(0).ObjectivesRef().Count();
    for (const char* field : {"Title", "Description", "ActivateFlagID", "ResetStatusFlagID", "ObjectiveModel"})
    {
        std::string malformed = original;
        const std::string oversized = "\"" + std::string(2001, 'x') + "\"";
        ok = replaceValue(malformed, "Team0Objective1", "Title", "\"after_objective\"") && ok;
        const bool continued = replaceValue(malformed, "Team0Objective0", field, oversized.c_str())
            && writeFixture(fit, malformed) && EditorData::initTerrainFromPCV(pak);
        auto* objective = getObjective();
        const std::string value = std::string_view(field) == "Title" ? objective->Title()
            : std::string_view(field) == "Description" ? objective->Description()
            : std::string_view(field) == "ActivateFlagID" ? objective->ActivateFlagID()
            : std::string_view(field) == "ResetStatusFlagID" ? objective->ResetStatusFlagID()
            : objective->ModelDefinition().name;
        const bool rejected = value.empty();
        auto after = EditorData::instance->TeamsRef().TeamRef(0).ObjectivesRef().Begin();
        after++;
        const bool following = !after.IsDone() && (*after)->Title() == "after_objective"
            && getUnit() && EditorData::instance->TeamsRef().TeamRef(0).ObjectivesRef().Count() == expectedObjectives;
        ok = continued && rejected && following && ok;
        fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=oversized_text field=%s rejected=%d continued=%d ok=%d\n",
            field, rejected, continued && following, ok);
    }
    for (const char* field : {"MissionName", "Author", "Blurb", "Blurb2", "AVIFilename", "DownloadURL", "ScenarioScript"})
    {
        auto& data = *EditorData::instance;
        data.MissionName("keep_name"); data.Author("keep_author"); data.Blurb("keep_blurb");
        data.Blurb2("keep_blurb2"); data.VideoFilename("keep_video"); data.DownloadURL("keep_url");
        strcpy(missionScriptName, "keep_script");
        std::string malformed = original;
        const bool script = std::string_view(field) == "ScenarioScript";
        ok = replaceValue(malformed, "Team0Objective1", "Title", "\"after_objective\"") && ok;
        const auto limit = script ? sizeof(missionScriptName) - 1 : objective_text::kMaxTextLength;
        const std::string oversized = "\"" + std::string(limit + 1, 'x') + "\"";
        const bool continued = replaceValue(malformed, script ? "Script" : "MissionSettings", field, oversized.c_str())
            && writeFixture(fit, malformed) && EditorData::initTerrainFromPCV(pak);
        const std::string value = script ? std::string(missionScriptName)
            : std::string_view(field) == "MissionName" ? data.MissionName()
            : std::string_view(field) == "Author" ? data.Author()
            : std::string_view(field) == "Blurb" ? data.Blurb()
            : std::string_view(field) == "Blurb2" ? data.Blurb2()
            : std::string_view(field) == "AVIFilename" ? data.VideoFilename() : data.DownloadURL();
        const std::string expected = script ? "keep_script"
            : std::string_view(field) == "MissionName" ? "keep_name"
            : std::string_view(field) == "Author" ? "keep_author"
            : std::string_view(field) == "Blurb" ? "keep_blurb"
            : std::string_view(field) == "Blurb2" ? "keep_blurb2"
            : std::string_view(field) == "AVIFilename" ? "keep_video" : "keep_url";
        const bool rejected = value == expected;
        auto after = EditorData::instance->TeamsRef().TeamRef(0).ObjectivesRef().Begin();
        after++;
        const bool following = !after.IsDone() && (*after)->Title() == "after_objective";
        ok = continued && rejected && following && getUnit() && ok;
        fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=oversized_text field=%s rejected=%d continued=%d ok=%d\n",
            field, rejected, continued && following, ok);
    }
    const auto payloadProbe = [&](auto& object, const char* kind, const char* field) {
        for (const auto mode : {"oversized", "unterminated", "trailing", "nul"})
        {
            const auto caseName = std::format("{}_{}", kind, mode);
            const auto path = std::format("terrain_gen_out/ew_payload_{}.fit", caseName);
            const auto savedPath = std::format("terrain_gen_out/ew_payload_{}.saved.fit", caseName);
            const auto document = [&](std::string_view literal, bool value) {
                return std::format("FITini\n[Probe]\nst {} = {}\nst After = \"after_payload_exact\"\nb Value = {}\nFitEnd\n",
                    field, literal, value ? "TRUE" : "FALSE");
            };
            EditorFit::MissionTextReader reader;
            bool passed = writeFixture(path.c_str(), document("\"keep_payload_exact\"", false))
                && reader.open(path.c_str()) == NO_ERR && reader.seekBlock("Probe") == NO_ERR && object.Read(&reader);
            reader.close();
            const std::string literal = std::string_view(mode) == "oversized" ? "\"" + std::string(2001, 'x') + "\""
                : std::string_view(mode) == "unterminated" ? "\"unterminated"
                : std::string_view(mode) == "trailing" ? "\"rejected\" garbage"
                : std::string("\"bad") + '\0' + "tail\"";
            passed = writeFixture(path.c_str(), document(literal, true)) && reader.open(path.c_str()) == NO_ERR
                && reader.seekBlock("Probe") == NO_ERR && object.Read(&reader) && passed;
            const bool following = EditorFit::text(reader, "After") == "after_payload_exact";
            reader.close();
            FitIniFile saved;
            passed = saved.create(savedPath.c_str()) == NO_ERR && passed;
            saved.writeBlock("Probe");
            passed = object.Save(&saved) && passed;
            saved.close();
            passed = reader.open(savedPath.c_str()) == NO_ERR && reader.seekBlock("Probe") == NO_ERR && passed;
            const bool retained = EditorFit::text(reader, field) == "keep_payload_exact";
            bool value = false;
            const bool flag = std::string_view(field) == "FlagID";
            passed = retained && following && (!flag || (reader.readIdBoolean("Value", value) == NO_ERR && value)) && passed;
            ok = passed && ok;
            fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=payload case=%s field=%s retained=%d following=%d ok=%d\n",
                caseName.c_str(), field, retained, following, passed);
        }
    };
    CPlayBIK bik(0); CPlayWAV wav(0); CDisplayTextMessage message(0);
    CMakeNewTechnologyAvailable technology(0); CSetBooleanFlag setFlag(0); CBooleanFlagIsSet conditionFlag(0);
    payloadProbe(bik, "bik", "AVIFileName"); payloadProbe(wav, "wav", "WAVFileName");
    payloadProbe(message, "message", "Message"); payloadProbe(technology, "technology", "PurchaseFileName");
    payloadProbe(setFlag, "action_flag", "FlagID"); payloadProbe(conditionFlag, "condition_flag", "FlagID");

    for (const auto mode : {"unterminated", "block_boundary", "trailing", "extra_quote", "nul", "unquoted", "cr"})
    {
        const auto path = std::format("terrain_gen_out/ew_literal_{}.fit", mode);
        const std::string literal = std::string_view(mode) == "trailing" ? "\"rejected\" garbage"
            : std::string_view(mode) == "extra_quote" ? "\"rejected\" \"second\""
            : std::string_view(mode) == "nul" ? std::string("\"bad") + '\0' + "tail\""
            : std::string_view(mode) == "unquoted" ? "unquoted"
            : std::string_view(mode) == "cr" ? "\"line1\r\nline2\rmid\"" : "\"unterminated";
        const bool block = std::string_view(mode) == "block_boundary";
        const auto document = std::string("FITini\n[Probe]\nul NumConditions = 0\nst Title = ") + literal
            + (block ? "\n[Next]\n" : "\n") + "st Description = \"after_description_exact\"\nFitEnd\n";
        CObjective objective(0);
        objective.Title("keep_title_exact");
        EditorFit::MissionTextReader reader;
        bool passed = writeFixture(path.c_str(), document) && reader.open(path.c_str()) == NO_ERR
            && reader.seekBlock("Probe") == NO_ERR && objective.Read(&reader, 0, 3);
        const std::string expected = std::string_view(mode) == "cr" ? "line1\r\nline2mid" : "keep_title_exact";
        const bool retained = objective.Title() == expected;
        if (block) passed = reader.seekBlock("Next") == NO_ERR && passed;
        const bool following = EditorFit::text(reader, "Description") == "after_description_exact";
        passed = retained && following && passed;
        ok = passed && ok;
        fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=literal case=%s retained=%d following=%d ok=%d\n", mode, retained, following, passed);
    }
    for (const auto mode : {"unterminated", "trailing", "nul", "unquoted"})
    {
        std::string malformed = original;
        const std::string literal = std::string_view(mode) == "trailing" ? "\"rejected\" garbage"
            : std::string_view(mode) == "nul" ? std::string("\"bad") + '\0' + "tail\""
            : std::string_view(mode) == "unquoted" ? "unquoted" : "\"unterminated";
        EditorData::instance->Author("keep_author_exact");
        const auto fitPath = std::format("terrain_gen_out/ew_metadata_{}.fit", mode);
        const auto pakPath = std::format("terrain_gen_out/ew_metadata_{}.pak", mode);
        const bool continued = replaceValue(malformed, "MissionSettings", "Author", literal)
            && replaceValue(malformed, "MissionSettings", "Blurb", "\"after_blurb_exact\"")
            && writeFixture(pakPath.c_str(), originalPak) && writeFixture(fitPath.c_str(), malformed)
            && EditorData::initTerrainFromPCV(pakPath.c_str());
        const bool retained = EditorData::instance->Author() == "keep_author_exact";
        const bool following = EditorData::instance->Blurb() == "after_blurb_exact";
        ok = continued && retained && following && ok;
        fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=metadata_literal case=%s retained=%d following=%d ok=%d\n",
            mode, retained, following, continued && retained && following);
    }
    struct SpeciesProbe { const char* name; const char* suffix; const char* count; const char* key; std::string value; };
    for (const auto& probe : {
        SpeciesProbe{"condition", "Condition", "NumConditions", "ConditionSpeciesString", "\"UnknownCondition\""},
        SpeciesProbe{"action", "Action", "NumActions", "ActionSpeciesString", "\"UnknownAction\""},
        SpeciesProbe{"failure_condition", "FailureCondition", "NumFailureConditions", "FailureConditionSpeciesString", "\"UnknownCondition\""},
        SpeciesProbe{"failure_action", "FailureAction", "NumFailureActions", "FailureActionSpeciesString", "\"UnknownAction\""},
        SpeciesProbe{"negative", "Condition", "NumConditions", "ConditionSpecies", "-1"},
        SpeciesProbe{"out_of_range", "Condition", "NumConditions", "ConditionSpecies", "9999"},
        SpeciesProbe{"unsupported", "Condition", "NumConditions", "ConditionSpecies", "2"},
        SpeciesProbe{"numeric_garbage", "Condition", "NumConditions", "ConditionSpecies", "1garbage"},
        SpeciesProbe{"literal_unterminated", "Condition", "NumConditions", "ConditionSpeciesString", "\"unterminated"},
        SpeciesProbe{"literal_trailing", "Condition", "NumConditions", "ConditionSpeciesString", "\"DestroyAllEnemyUnits\" garbage"},
        SpeciesProbe{"literal_nul", "Condition", "NumConditions", "ConditionSpeciesString", std::string("\"DestroyAllEnemyUnits") + '\0' + "\""},
        SpeciesProbe{"literal_unquoted", "Condition", "NumConditions", "ConditionSpeciesString", "DestroyAllEnemyUnits"}})
    {
        const auto path = std::format("terrain_gen_out/ew_species_{}.fit", probe.name);
        const auto savedPath = std::format("terrain_gen_out/ew_species_{}.saved.fit", probe.name);
        std::string document = "FITini\n[Probe]\nul NumConditions = 0\nul NumActions = 0\nul NumFailureConditions = 0\nul NumFailureActions = 0\n";
        bool passed = replaceValue(document, "Probe", probe.count, "2");
        const bool condition = std::string_view(probe.suffix).ends_with("Condition");
        const bool numeric = std::string_view(probe.key) == "ConditionSpecies";
        const auto secondKey = std::string_view(probe.suffix).starts_with("Failure") ? probe.key
            : condition ? "ConditionSpeciesString" : "ActionSpeciesString";
        document += std::format("[Team0Objective0{}0]\n{} {} = {}\n[Team0Objective0{}1]\nst {} = \"{}\"\n{}\nFitEnd\n",
            probe.suffix, numeric ? "ul" : "st", probe.key, probe.value, probe.suffix,
            secondKey, condition ? "DestroyNumberOfEnemyUnits" : "PlayWAV",
            condition ? "ul Num = 9" : "st WAVFileName = \"after.wav\"");
        CObjective objective(0);
        EditorFit::MissionTextReader reader;
        passed = writeFixture(path.c_str(), document) && reader.open(path.c_str()) == NO_ERR
            && reader.seekBlock("Probe") == NO_ERR && objective.Read(&reader, 0, 3) && passed;
        reader.close();
        FitIniFile saved;
        passed = saved.create(savedPath.c_str()) == NO_ERR && passed;
        saved.writeBlock("Probe"); passed = objective.Save(&saved, 0) && passed; saved.close();
        passed = reader.open(savedPath.c_str()) == NO_ERR && reader.seekBlock("Probe") == NO_ERR && passed;
        unsigned long count = 0;
        passed = reader.readIdULong(probe.count, count) == NO_ERR && count == 1 && passed;
        const auto block = std::format("Team0Objective0{}0", probe.suffix);
        passed = reader.seekBlock(block.c_str()) == NO_ERR && passed;
        const bool following = EditorFit::text(reader, secondKey) == (condition ? "DestroyNumberOfEnemyUnits" : "PlayWAV");
        unsigned long num = 0;
        passed = following && (condition ? reader.readIdULong("Num", num) == NO_ERR && num == 9
            : EditorFit::text(reader, "WAVFileName") == "after.wav") && passed;
        ok = passed && ok;
        fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=species case=%s following=%d ok=%d\n", probe.name, following, passed);
    }

    std::string boundaryFixture = original;
    const std::string boundaryScript = std::string(sizeof(missionScriptName) - 2, 'x') + "\n";
    const std::string quotedScript = "\"" + boundaryScript + "\"";
    const bool scriptBoundary = replaceValue(boundaryFixture, "Script", "ScenarioScript", quotedScript.c_str())
        && writeFixture(fit, boundaryFixture) && EditorData::initTerrainFromPCV(pak)
        && std::string(missionScriptName) == boundaryScript;
    ok = scriptBoundary && ok;
    fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=script_boundary ok=%d\n", scriptBoundary);
    ok = writeFixture(fit, original) && EditorData::initTerrainFromPCV(pak) && ok;

    // Missing required fields preserve the serialized document, live objects,
    // dirty flag and an executable undo/redo history.
    handleNewMenuMessage(ID_MISSION_REFRESH_DEFINITIONS);
    ok = undoMgr.Undo() && ok;
    ok = EditorData::instance->save("terrain_gen_out/ew_before_reject.pak", true) && ok;
    const auto beforeReject = readFixture("terrain_gen_out/ew_before_reject.fit");
    auto* const previousLand = land;
    auto* const previousUnit = getUnit();
    const bool previousDirty = EditorData::instance->MissionNeedsSaving();
    const bool previousUndo = undoMgr.HaveUndo(), previousRedo = undoMgr.HaveRedo();
    const bool budgetRejected = writeFixture("terrain_gen_out/ew_reader_budget.pak", originalPak)
        && writeFixture("terrain_gen_out/ew_reader_budget.fit", std::string(EditorFit::kMaxMissionBytes + 1, 'x'))
        && !EditorData::initTerrainFromPCV("terrain_gen_out/ew_reader_budget.pak");
    const bool budgetUnchanged = land == previousLand && getUnit() == previousUnit
        && EditorData::instance->MissionNeedsSaving() == previousDirty
        && undoMgr.HaveUndo() == previousUndo && undoMgr.HaveRedo() == previousRedo
        && EditorData::instance->save("terrain_gen_out/ew_after_reject.pak", true)
        && readFixture("terrain_gen_out/ew_after_reject.fit") == beforeReject;
    fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=reader_budget rejected=%d unchanged=%d ok=%d\n",
        budgetRejected, budgetUnchanged, budgetRejected && budgetUnchanged);
    if (!budgetRejected || !budgetUnchanged) return false;
    struct MissingPayload { const char* name; const char* species; const char* legacy; const char* present; const char* field; };
    for (const auto& probe : {
        MissingPayload{"legacy_num", "DestroyNumberOfEnemyUnits", "1", "", "Num"},
        MissingPayload{"legacy_unit_watch", "DestroySpecificEnemyUnit", "3", "l MoverWID = 1\nf PositionY = 0", "PositionX"},
        MissingPayload{"legacy_structure_watch", "DestroySpecificStructure", "4", "l BuildingWID = 1\nf PositionX = 0", "PositionY"},
        MissingPayload{"string_unit_watch", "DestroySpecificEnemyUnit", nullptr, "l BuildingWID = 1\nf PositionY = 0", "PositionX"},
        MissingPayload{"string_structure_watch", "DestroySpecificStructure", nullptr, "l MoverWID = 1\nf PositionX = 0", "PositionY"},
        MissingPayload{"capture_unit", "CaptureSpecificUnit", nullptr, "f PositionY = 0", "PositionX"},
        MissingPayload{"capture_structure", "CaptureSpecificStructure", nullptr, "f PositionX = 0", "PositionY"},
        MissingPayload{"action_file", "PlayBIK", nullptr, "", "AVIFileName"}})
    {
        std::string malformed = original;
        const bool action = std::string_view(probe.name) == "action_file";
        const auto block = action ? "Team0Objective0Action0" : "Team0Objective0Condition0";
        const auto key = action ? "ActionSpeciesString" : "ConditionSpeciesString";
        bool prepared = replaceValue(malformed, block, key, std::string("\"") + probe.species + "\"");
        if (probe.legacy)
            prepared = removeField(malformed, block, key) && replaceValue(malformed, block, "ConditionSpecies", probe.legacy) && prepared;
        if (*probe.present) addDeclaration(malformed, block, probe.present);
        const auto fitPath = std::format("terrain_gen_out/ew_missing_{}.fit", probe.name);
        const auto pakPath = std::format("terrain_gen_out/ew_missing_{}.pak", probe.name);
        const bool rejected = prepared && writeFixture(pakPath.c_str(), originalPak)
            && writeFixture(fitPath.c_str(), malformed) && !EditorData::initTerrainFromPCV(pakPath.c_str());
        const bool unchanged = land == previousLand && getUnit() == previousUnit
            && EditorData::instance->MissionNeedsSaving() == previousDirty
            && undoMgr.HaveUndo() == previousUndo && undoMgr.HaveRedo() == previousRedo
            && EditorData::instance->save("terrain_gen_out/ew_after_reject.pak", true)
            && readFixture("terrain_gen_out/ew_after_reject.fit") == beforeReject;
        const bool passed = rejected && unchanged;
        ok = passed && ok;
        fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=missing_payload case=%s field=%s rejected=%d unchanged=%d ok=%d\n",
            probe.name, probe.field, rejected, unchanged, passed);
        // History belongs to the original document; a failed fixture cannot replay it safely.
        if (!passed) return false;
    }
    struct Required { const char* block; const char* field; };
    for (const auto& required : {Required{"Team0Objective0", "NumConditions"},
        Required{"Team0Objectives", "NumObjectives"}, Required{"Objectives Version", "Version"},
        Required{"MissionSettings", "scenarioTuneNum"}, Required{"MissionSettings", "MissionName"},
        Required{"MissionSettings", "Blurb2"}, Required{"Script", "ScenarioScript"},
        Required{"MissionSettings", "MissionNameResourceStringID"}})
    {
        std::string malformed = original;
        if (std::string_view(required.field) == "MissionNameResourceStringID")
            ok = replaceValue(malformed, "MissionSettings", "MissionNameUseResourceString", "TRUE") && ok;
        else if (std::string_view(required.field) == "MissionName")
            ok = replaceValue(malformed, "MissionSettings", "MissionNameUseResourceString", "FALSE") && ok;
        const bool rejected = removeField(malformed, required.block, required.field)
            && writeFixture(fit, malformed) && !EditorData::initTerrainFromPCV(pak);
        const bool unchanged = land == previousLand && getUnit() == previousUnit
            && EditorData::instance->MissionNeedsSaving() == previousDirty
            && undoMgr.HaveUndo() == previousUndo && undoMgr.HaveRedo() == previousRedo
            && EditorData::instance->save("terrain_gen_out/ew_after_reject.pak", true)
            && readFixture("terrain_gen_out/ew_after_reject.fit") == beforeReject;
        ok = rejected && unchanged && ok;
        fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=missing_required field=%s rejected=%d unchanged=%d ok=%d\n",
            required.field, rejected, unchanged, ok);
        if (!rejected || !unchanged) return false;
    }
    for (const auto& [field, value] : {std::pair{"ModelScale", "0"}, {"ModelScale", "-1"},
        {"ModelScale", "1e100"}, {"ModelScale", "1garbage"}, {"ModelType", "256"}, {"Num", "1garbage"}})
    {
        const auto path = std::format("terrain_gen_out/cq_candidate_{}_{}", field, value);
        auto malformed = original;
        const bool payload = std::string_view(field) == "Num";
        const auto block = payload ? "Team0Objective0Condition0" : "Team0Objective0";
        const bool prepared = !payload || replaceValue(malformed, block, "ConditionSpeciesString", "\"DestroyNumberOfEnemyUnits\"");
        if (payload) {
            removeField(malformed, block, "Num");
            addDeclaration(malformed, block, "ul Num = 0");
        }
        const bool rejected = prepared && replaceValue(malformed, block, field, value)
            && writeFixture((path + ".pak").c_str(), originalPak) && writeFixture((path + ".fit").c_str(), malformed)
            && !EditorData::initTerrainFromPCV((path + ".pak").c_str());
        const bool unchanged = land == previousLand && getUnit() == previousUnit
            && EditorData::instance->MissionNeedsSaving() == previousDirty
            && undoMgr.HaveUndo() == previousUndo && undoMgr.HaveRedo() == previousRedo
            && EditorData::instance->save("terrain_gen_out/ew_after_reject.pak", true)
            && readFixture("terrain_gen_out/ew_after_reject.fit") == beforeReject;
        const bool history = rejected && unchanged && undoMgr.Redo() && undoMgr.Undo();
        fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=candidate_rejection field=%s value=%s rejected=%d unchanged=%d history=%d ok=%d\n",
            field, value, rejected, unchanged, history, rejected && unchanged && history);
        if (!rejected || !unchanged || !history) return false;
    }
    ok = undoMgr.Redo() && undoMgr.Undo() && ok;

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
        fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=reject field=%s ok=%d\n", bad.field, ok);
    }
    const std::string modelFixture = "FITini\n[Probe]\nst ObjectiveModel = \"keep_model\"\nl ModelType = 17\nf ModelScale = 1.75\nul NumConditions = 0\nFitEnd\n";
    for (const auto field : {"ObjectiveModel", "ModelType", "ModelScale", "all"})
    {
        const auto path = std::format("terrain_gen_out/ew_model_default_{}.fit", field);
        CObjective objective(0);
        EditorFit::MissionTextReader reader;
        bool passed = writeFixture(path.c_str(), modelFixture) && reader.open(path.c_str()) == NO_ERR
            && reader.seekBlock("Probe") == NO_ERR && objective.Read(&reader, 0, 3);
        reader.close();
        std::string missing = modelFixture;
        for (const auto key : {"ObjectiveModel", "ModelType", "ModelScale"})
            if (std::string_view(field) == "all" || std::string_view(field) == key) passed = removeField(missing, "Probe", key) && passed;
        passed = writeFixture(path.c_str(), missing) && reader.open(path.c_str()) == NO_ERR
            && reader.seekBlock("Probe") == NO_ERR && objective.Read(&reader, 0, 3) && passed;
        const auto model = objective.ModelDefinition();
        passed = model.name == (std::string_view(field) == "all" || std::string_view(field) == "ObjectiveModel" ? "" : "keep_model")
            && model.type == (std::string_view(field) == "all" || std::string_view(field) == "ModelType" ? 0 : 17)
            && model.scale == (std::string_view(field) == "all" || std::string_view(field) == "ModelScale" ? 1.0f : 1.75f) && passed;
        ok = passed && ok;
        fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=model_default case=%s ok=%d\n", field, passed);
    }
    for (const auto suffix : {"Condition", "FailureCondition"})
    {
        const auto path = std::format("terrain_gen_out/ew_condition_failure_{}.fit", suffix);
        std::string document = modelFixture;
        bool prepared = std::string_view(suffix) == "Condition" ? replaceValue(document, "Probe", "NumConditions", "1") : true;
        if (std::string_view(suffix) == "FailureCondition") addDeclaration(document, "Probe", "ul NumFailureConditions = 1");
        const auto end = document.find("FitEnd");
        document.insert(end, std::format("[Team0Objective0{}0]\nst {}SpeciesString = \"DestroyNumberOfEnemyUnits\"\n", suffix, suffix));
        CObjective objective(0);
        EditorFit::MissionTextReader reader;
        const bool rejected = prepared && writeFixture(path.c_str(), document) && reader.open(path.c_str()) == NO_ERR
            && reader.seekBlock("Probe") == NO_ERR && !objective.Read(&reader, 0, 3);
        ok = rejected && ok;
        fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=condition_failure case=%s ok=%d\n", suffix, rejected);
    }
    CObjective display(0);
    display.Title("display_title"); display.Description("display_description");
    CObjective displayCopy(display);
    bool exact = display == displayCopy;
    displayCopy.Title("Display_title"); exact = !(display == displayCopy) && exact;
    displayCopy = display; displayCopy.Description("Display_description"); exact = !(display == displayCopy) && exact;
    CDisplayTextMessage lower(0), upper(0);
    EditorFit::MissionTextReader messageReader;
    exact = writeFixture("terrain_gen_out/ew_message_lower.fit", "FITini\n[Probe]\nst Message = \"display_message\"\nFitEnd\n")
        && messageReader.open("terrain_gen_out/ew_message_lower.fit") == NO_ERR
        && messageReader.seekBlock("Probe") == NO_ERR && lower.Read(&messageReader) && exact;
    messageReader.close();
    exact = writeFixture("terrain_gen_out/ew_message_upper.fit", "FITini\n[Probe]\nst Message = \"Display_message\"\nFitEnd\n")
        && messageReader.open("terrain_gen_out/ew_message_upper.fit") == NO_ERR
        && messageReader.seekBlock("Probe") == NO_ERR && upper.Read(&messageReader) && !lower.operator==(upper) && exact;
    ok = exact && ok;
    fprintf(stderr, "[EDITOR_DEFINITIONS v1] stage=display_equality ok=%d\n", exact);
    const bool restored = writeFixture(fit, original) && EditorData::initTerrainFromPCV(pak);
    fprintf(stderr, "[EDITOR_DEFINITIONS v1] preservation_commands_undo_reload_malformed=%d\n", ok && restored);
    return ok && restored;
}
