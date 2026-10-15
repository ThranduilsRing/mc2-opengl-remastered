#include "../../tests/unit/doctest.h"
#include "CommandHistory.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace editor::document;
namespace {
void require(bool value, const char* name) { if (!value) throw std::runtime_error(name); }
void u32(std::vector<std::byte>& bytes, std::size_t p, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[p + i] = static_cast<std::byte>(value >> (8 * i));
}
std::vector<std::byte> fixturePak()
{
    std::vector<std::byte> bytes(28 + 128);
    u32(bytes, 0, 0xfeedface); u32(bytes, 4, 28); u32(bytes, 8, 28);
    for (std::size_t i = 1; i < 5; ++i) u32(bytes, 8 + i * 4, 0xe0000000 | 156);
    return bytes;
}
std::string fixtureFit() { return
    "FITini\r\n// an extension remains authored\r\n[MissionSettings]\r\n"
    "st MissionName = \"Original\" // comment\r\nst Blurb2 = \"second\"\r\nuc scenarioTuneNum = 0\r\n"
    "[Parts]\r\nul NumParts = 0\r\n[Music]\r\nuc scenarioTuneNum = 0\r\n"
    "[Script]\r\nst ScenarioScript = \"unit\"\r\n[Extension]\r\nf[3] Values = 1.00,0.5,0.30,\r\nul[0] Empty = \r\n"
    "Brain { compatibilityMode = \"Enhanced\"; OPORD { type = Patrol; } }\r\n"
    "[Objectives Version]\r\nul Version = 2\r\n[Objectives]\r\nul NumObjectives = 0\r\nFITend\r\n"; }
}
int q3DocumentTests()
{
    try {
        const auto fixture = fixtureFit();
        std::vector<std::string> logs;
        const Diagnostic log = [&](const std::string& text) { logs.push_back(text); };
        auto loaded = MissionDocument::decode(fixture, fixturePak(), "unit", log);
        require(loaded.has_value() && logs.empty(), "valid_import");
        auto document = std::move(*loaded);
        require(document.fitBytes() == fixture && document.archive().originalBytes == fixturePak(), "lossless_fit_pak");
        require(document.terrainSide() == 2 && document.terrain().size() == 4, "typed_terrain");
        require(document.find("Extension", "Values")->value.size() == 3, "typed_array");
        require(document.find("Extension", "Empty")->value.empty(), "optional_empty_array");
        auto multiline = fixture;
        multiline.replace(multiline.find("\"Original\""), 10, "\"first\r\nsecond = text\"" );
        auto multilineDocument = MissionDocument::decode(multiline, fixturePak(), "multiline", log);
        require(multilineDocument && std::get<std::string>(multilineDocument->find("MissionSettings", "MissionName")->value[0]) == "first\nsecond = text", "CR_semantics");
        CommandHistory multilineHistory(*multilineDocument);
        require(multilineHistory.setText(multilineDocument->find("MissionSettings", "MissionName")->id, "replacement", "Name").has_value() &&
            multilineHistory.undo() && multilineDocument->fitBytes() == multiline, "multiline_undo_original_bytes");
        auto withObjective = fixture;
        withObjective.replace(withObjective.find("NumObjectives = 0"), 17, "NumObjectives = 1");
        withObjective.insert(withObjective.find("FITend"),
            "[Objective0]\r\nul NumConditions = 1\r\nul NumActions = 1\r\n"
            "[Objective0Condition0]\r\nst ConditionSpeciesString = \"MoveAnyUnitToArea\"\r\n"
            "f TargetCenterX = 12.0\r\nf TargetCenterY = 3.0\r\nf TargetRadius = 5.0\r\n"
            "[Objective0Action0]\r\nst ActionSpeciesString = \"DisplayTextMessage\"\r\nst Message = \"message\"\r\n");
        auto objectives = MissionDocument::decode(withObjective, fixturePak(), "unit", log);
        require(objectives && objectives->objectives().size() == 1 && objectives->objectives()[0].entries[0].size() == 1 &&
            std::get<ConditionSpecies>(objectives->objectives()[0].entries[0][0].species) == ConditionSpecies::moveAny, "typed_objective");
        CommandHistory history(document);
        const auto title = document.find("MissionSettings", "MissionName")->id;
        require(history.setText(title, "changed", "Name").has_value(), "production_command");
        const auto changed = document.fitBytes();
        require(changed != fixture && document.modified(), "changed_dirty");
        require(changed.find("// comment\r\n") != std::string::npos && changed.find("compatibilityMode") != std::string::npos, "preserve_extensions");
        require(history.undo() && document.fitBytes() == fixture && !document.modified(), "undo_original");
        require(history.redo() && document.fitBytes() == changed, "redo_changed");
        require(history.undo() && history.setText(title, "branch", "Name").has_value() && !history.canRedo(), "redo_branch");
        require(history.undo() && history.setText(title, "Original", "Noop").has_value() && !document.modified(), "noop");
        require(!history.setText(title, std::string("bad\0text", 8), "Bad").has_value() && document.fitBytes() == fixture, "invalid_command_atomic");
        CommandHistory bounded(document, 1000);
        for (int i = 0; i < 20; ++i) require(bounded.setText(title, std::to_string(i), "Name").has_value(), "budget_edits");
        require(bounded.evicted() > 0 && bounded.bytesUsed() <= 1000, "history_eviction");
        require(bounded.undo(), "replacement_retains_redo_setup");
        auto badFit = [&](std::string input, const char* name) {
            logs.clear(); auto result = MissionDocument::decode(std::move(input), fixturePak(), "bad", log);
            require(!result && logs.size() == 1 && logs[0].find("rejected=1") != std::string::npos, name);
        };
        const auto rejectField = [&](std::string input, std::string_view field) {
            const auto before = document.fitBytes();
            const auto revision = document.revision();
            const auto pak = document.archive().originalBytes;
            const auto dirty = document.modified();
            const auto used = bounded.bytesUsed(), evicted = bounded.evicted();
            const auto undo = bounded.canUndo(), redo = bounded.canRedo();
            logs.clear();
            auto result = bounded.replace(std::move(input), fixturePak(), "bad", log);
            require(!result && logs.size() == 1 && logs[0].find("field=" + std::string(field) + " ") != std::string::npos &&
                logs[0].find("rejected=1") != std::string::npos && document.fitBytes() == before && document.revision() == revision &&
                document.archive().originalBytes == pak && document.modified() == dirty && bounded.bytesUsed() == used &&
                bounded.evicted() == evicted && bounded.canUndo() == undo && bounded.canRedo() == redo,
                "malformed_field_single_log_unchanged");
            require(bounded.redo() && bounded.undo() && document.fitBytes() == before, "replacement_retains_executable_history");
        };
        auto withPart = fixture;
        withPart.replace(withPart.find("NumParts = 0"), 12, "NumParts = 1");
        withPart.insert(withPart.find("FITend"), "[Part1]\r\nul ObjectNumber = 1\r\nf PositionX = 1.0\r\nf PositionY = 2.0\r\n");
        const auto appendPart = [&](std::string declaration) {
            auto input = withPart; input.insert(input.find("FITend"), declaration + "\r\n"); return input;
        };
        for (const auto declaration : {"f TeamID = 1e30", "st TeamID = \"bad\"", "l TeamID = -2", "l TeamID = 8",
                 "ul TeamID = 4294967295", "l[2] TeamID = 0,1,"})
            rejectField(appendPart(declaration), "Part1/TeamID");
        for (const auto field : {"PositionZ", "Rotation"})
            for (const auto prefix : {"st ", "l "})
                rejectField(appendPart(std::string(prefix) + field + (std::string_view(prefix) == "st " ? " = \"bad\"" : " = 10")),
                    "Part1/" + std::string(field));
        for (const auto team : {-1, 0, 7}) {
            logs.clear();
            auto valid = MissionDocument::decode(appendPart("l TeamID = " + std::to_string(team) + "\r\nf PositionZ = 3.0\r\nf Rotation = 12.0"), fixturePak(), "valid", log);
            require(valid && logs.empty() && valid->entities()[0].team == team && valid->entities()[0].position[2] == 3 && valid->entities()[0].rotation == 12,
                "valid_part_payload_boundaries");
        }
        auto absentPart = MissionDocument::decode(withPart, fixturePak(), "valid", log);
        require(absentPart && absentPart->entities()[0].team == 0 && absentPart->entities()[0].position[2] == 0 && absentPart->entities()[0].rotation == 0,
            "absent_optional_part_defaults");
        for (const auto suffix : {"Condition", "Action"}) {
            const auto entry = "Objective0" + std::string(suffix) + "0";
            const auto species = std::string_view(suffix) == "Condition" ? "BooleanFlagIsSet" : "SetBooleanFlag";
            const auto flagFixture = [&](std::string declaration) {
                auto input = fixture;
                input.replace(input.find("NumObjectives = 0"), 17, "NumObjectives = 1");
                input.insert(input.find("FITend"), "[Objective0]\r\nul NumConditions = " + std::string(std::string_view(suffix) == "Condition" ? "1" : "0") +
                    "\r\nul NumActions = " + (std::string_view(suffix) == "Action" ? "1" : "0") + "\r\n[" + entry + "]\r\nst " + suffix +
                    "SpeciesString = \"" + species + "\"\r\n" + declaration + "\r\n");
                return input;
            };
            for (const auto declaration : {"st FlagIndex = \"bad\"", "f FlagIndex = 1e30", "l FlagIndex = -1", "ul FlagIndex = 2147483648",
                     "ul FlagIndex = 4294967295", "l[2] FlagIndex = 0,1,"}) {
                rejectField(flagFixture(declaration), entry + "/FlagIndex");
                rejectField(flagFixture(std::string(declaration) + "\r\nst FlagID = \"valid\""), entry + "/FlagIndex");
            }
            for (const auto index : {"0", "2147483647"}) {
                logs.clear(); auto valid = MissionDocument::decode(flagFixture("ul FlagIndex = " + std::string(index)), fixturePak(), "valid", log);
                require(valid && logs.empty(), "valid_legacy_flag_boundaries");
            }
        }
        badFit("FITini\n[Block]\nst A = \"unclosed\nst B = \"next\"\nFITend", "quote_boundary");
        badFit("FITini\n[Block]\nst A = \"a\" garbage\nFITend", "literal_garbage");
        auto nul = fixture; nul.insert(40, 1, '\0'); badFit(nul, "embedded_nul");
        badFit("FITini\n[B]\nf A = nan\nFITend", "nonfinite");
        badFit("FITini\n[B]\nf[2] A = 1,\nFITend", "array_count");
        badFit("FITini\n[B]\nst A = \"x\"\nst A = \"y\"\nFITend", "duplicate_field");
        auto missing = fixture; missing.erase(missing.find("st MissionName"), missing.find("st Blurb2") - missing.find("st MissionName"));
        badFit(missing, "missing_required");
        auto missingRadius = withObjective;
        missingRadius.erase(missingRadius.find("f TargetRadius"), 22);
        badFit(missingRadius, "missing_objective_payload");
        for (const auto scale : {"0", "-1", "1e100"}) {
            auto input = withObjective;
            input.insert(input.find("[Objective0Condition0]"), "f ModelScale = " + std::string(scale) + "\r\n");
            rejectField(std::move(input), "Objective0/ModelScale");
        }
        auto invalidSpecies = withObjective;
        invalidSpecies.replace(invalidSpecies.find("MoveAnyUnitToArea"), 17, "UnknownSpecies");
        auto opaqueSpecies = MissionDocument::decode(invalidSpecies, fixturePak(), "opaque", log);
        require(opaqueSpecies && opaqueSpecies->fitBytes() == invalidSpecies &&
            std::holds_alternative<std::monostate>(opaqueSpecies->objectives()[0].entries[0][0].species), "opaque_species_preserved");
        auto depth = std::string("FITini\n[Extension]\n");
        for (std::size_t i = 0; i < nestingLimit + 2; ++i) depth += "Nested { ";
        for (std::size_t i = 0; i < nestingLimit + 2; ++i) depth += " }";
        badFit(depth + "\nFITend", "depth_limit");
        badFit(std::string(fitByteLimit + 1, ' '), "file_limit");
        auto packets = fixturePak(); u32(packets, 8, 29);
        logs.clear(); require(!MissionDocument::decode(fixture, packets, "bad", log) && logs.size() == 1, "packet_bounds");
        packets = fixturePak(); u32(packets, 28, 0x7fc00000);
        logs.clear(); require(!MissionDocument::decode(fixture, packets, "bad", log) && logs.size() == 1, "terrain_nan");
        const auto directory = std::filesystem::path("scratch/q3-document-tests") / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(directory);
        require(document.save(directory / "mission", log).has_value() && !document.modified(), "savepoint");
        require(history.setText(title, "published", "Name").has_value() && document.modified(), "edit_after_savepoint");
        require(document.save(directory / "mission", log).has_value(), "replace_saved_pair");
        auto reopened = MissionDocument::read(directory / "mission.fit", directory / "mission.pak", log);
        require(reopened && reopened->fitBytes() == document.fitBytes(), "transaction_reopen");
        auto importedLongName = fixture;
        const std::string longName = "long name " + std::string(8192, 'x') + " retained tail";
        importedLongName.replace(importedLongName.find("\"Original\""), 10, '"' + longName + '"');
        auto longDocument = MissionDocument::decode(importedLongName, fixturePak(), "long-name", log);
        require(longDocument.has_value(), "valid_long_name_import");
        CommandHistory longHistory(*longDocument);
        const auto longId = longDocument->find("MissionSettings", "MissionName")->id;
        require(longHistory.setText(longId, std::get<std::string>(longDocument->field(longId)->value[0]), "Apply").has_value() &&
            longDocument->fitBytes() == importedLongName && !longHistory.canUndo() && longDocument->save(directory / "long-name", log).has_value(),
            "long_name_apply_save_preserved");
        auto longReopen = MissionDocument::read(directory / "long-name.fit", directory / "long-name.pak", log);
        require(longReopen && std::get<std::string>(longReopen->field(longId)->value[0]) == longName, "long_name_reopen_exact");
        const auto journal = directory / "mission.transaction.interrupted";
        { std::ofstream pending(journal); pending << "pending\n"; }
        logs.clear();
        require(!MissionDocument::read(directory / "mission.fit", directory / "mission.pak", log) && logs.size() == 1 &&
            logs[0].find("recovery_required") != std::string::npos, "interrupted_publication_not_loaded");
        const auto savedBytes = reopened->fitBytes();
        require(history.setText(title, "pending save", "Name").has_value(), "pending_save_edit");
        const auto pendingBytes = document.fitBytes();
        const auto pendingRevision = document.revision();
        const auto filesBefore = std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{});
        logs.clear();
        auto pendingSave = document.save(directory / "mission", log);
        require(!pendingSave && pendingSave.error().reason == "recovery_required" && logs.size() == 1 &&
            document.modified() && document.fitBytes() == pendingBytes && document.revision() == pendingRevision &&
            filesBefore == std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}),
            "pending_save_retains_state_and_files");
        std::filesystem::rename(journal, directory / "interrupted-journal.retained");
        auto stillSaved = MissionDocument::read(directory / "mission.fit", directory / "mission.pak", log);
        require(stillSaved && stillSaved->fitBytes() == savedBytes && history.undo(), "pending_save_preserves_published_pair_and_history");
        const auto activeJournal = directory / "reserved.transaction.active";
        { std::ofstream reserved(activeJournal); reserved << "committed\nexclusive sentinel\n"; }
        logs.clear();
        const auto reservedFiles = std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{});
        auto collision = document.save(directory / "reserved", log);
        std::ifstream sentinel(activeJournal);
        const std::string sentinelBytes{std::istreambuf_iterator<char>(sentinel), {}};
        require(!collision && collision.error().reason == "journal_write_failed" && logs.size() == 1 &&
            sentinelBytes == "committed\nexclusive sentinel\n" &&
            reservedFiles == std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}),
            "exclusive_create_preserves_existing_journal_and_creates_no_stages");
        for (const auto state : {"", "unknown\n"}) {
            const auto brokenJournal = directory / "broken.transaction.retained";
            { std::ofstream broken(brokenJournal, std::ios::trunc); broken << state; }
            logs.clear();
            auto refused = document.save(directory / "broken", log);
            require(!refused && refused.error().reason == "recovery_required" && logs.size() == 1 &&
                !std::filesystem::exists(directory / "broken.fit"), "malformed_journal_blocks_save");
        }
        require(history.undo() && document.modified(), "undo_past_savepoint");
        std::filesystem::create_directory(directory / "blocked.pak");
        logs.clear();
        const auto retained = document.fitBytes();
        require(!document.save(directory / "blocked", log) && logs.size() == 1 && document.modified() && document.fitBytes() == retained &&
            !std::filesystem::exists(directory / "blocked.fit"), "failed_save_retains_document_and_savepoint");
        require(bounded.replace(fixture, fixturePak(), "replacement", log).has_value() && document.fitBytes() == fixture &&
            !document.modified() && !bounded.canUndo() && !bounded.canRedo() && bounded.bytesUsed() == 0 && bounded.evicted() == 0,
            "successful_replacement_resets_history_and_savepoint");
        std::cout << "editor_document_tests: PASS (syntax, ownership, typed values, commands, malformed input)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "editor_document_tests: FAIL " << error.what() << '\n'; return 1; }
}
TEST_CASE("document/Q3 lossless syntax, history and publication")
{
    CHECK(q3DocumentTests() == 0);
}
