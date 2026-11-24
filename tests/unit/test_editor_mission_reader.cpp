// Model compatibility is independent of the editor's MFC world and catalog.
// The corpus exercises the same decoder used by CObjective::Read on original bytes.
#include "doctest.h"
#include "../../editor/AuthoredMissionState.h"
#include <bit>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace
{
std::filesystem::path readerScratch()
{
    const auto root = std::filesystem::path(MC2_REPO_ROOT);
    const auto path = root / "scratch/editor-reader-tests";
    std::filesystem::create_directories(path);
    return path;
}
std::string fixture(const char* name, std::string_view bytes)
{
    const auto path = readerScratch() / name;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
    return path.string();
}
}

TEST_CASE("editor model defaults replace stale optional values")
{
    for (const unsigned mask : {1u, 2u, 4u, 7u})
    {
        std::string document = "FITini\n[Probe]\n";
        if (!(mask & 1)) document += "st ObjectiveModel = \"stored_model\"\n";
        if (!(mask & 2)) document += "l ModelType = 17\n";
        if (!(mask & 4)) document += "f ModelScale = 1.75\n";
        document += "FitEnd\n";
        EditorFit::MissionTextReader reader;
        REQUIRE(reader.open(fixture("defaults.fit", document).c_str()) == NO_ERR);
        REQUIRE(reader.seekBlock("Probe") == NO_ERR);
        ObjectiveModelDefinition model{"stale_model", 42, 2.5f};
        const auto result = EditorFit::readObjectiveModel(reader, model);
        CHECK(result.accepted);
        CHECK(result.defaultedFields == mask);
        CHECK(result.rejectedFields == 0);
        CHECK(model.name == (mask & 1 ? "" : "stored_model"));
        CHECK(model.type == (mask & 2 ? 0 : 17));
        CHECK(model.scale == (mask & 4 ? 1.0f : 1.75f));
    }
}

TEST_CASE("editor model rejects present malformed scalars without replacing values")
{
    for (const auto& [type, scale] : {std::pair{"-1", "1.0"}, {"256", "1.0"}, {"7garbage", "1.0"},
                                    {"0", "nan"}, {"0", "0"}, {"0", "-1"}, {"0", "1garbage"}})
    {
        const auto bytes = std::format("FITini\n[Probe]\nst ObjectiveModel = \"stored\"\nl ModelType = {}\nf ModelScale = {}\nFitEnd\n", type, scale);
        EditorFit::MissionTextReader reader;
        REQUIRE(reader.open(fixture("malformed.fit", bytes).c_str()) == NO_ERR);
        REQUIRE(reader.seekBlock("Probe") == NO_ERR);
        ObjectiveModelDefinition model{"keep_exact", 17, 1.75f};
        const auto before = model;
        const auto result = EditorFit::readObjectiveModel(reader, model);
        CHECK_FALSE(result.accepted);
        CHECK(result.defaultedFields == 0);
        CHECK(result.rejectedFields != 0);
        CHECK(model == before);
    }
}

TEST_CASE("editor mission input enforces its byte budget and rejects a short read")
{
    EditorFit::MissionTextReader reader;
    const auto oversized = fixture("oversized.fit", std::string(EditorFit::kMaxMissionBytes + 1, 'x'));
    CHECK(reader.open(oversized.c_str()) != NO_ERR);
    CHECK_FALSE(reader.isOpen());
    const auto truncated = fixture("short.fit", "FITini\n[Probe]\nst Value = \"before_truncation\"\nFitEnd\n");
    File input;
    REQUIRE(input.open(truncated.c_str()) == NO_ERR);
    REQUIRE(input.getLength() > 0);
    { std::ofstream output(truncated, std::ios::binary | std::ios::trunc); REQUIRE(output.good()); }
    CHECK_FALSE(EditorFit::readMissionSource(input, truncated));
}

TEST_CASE("editor objective model corpus" * doctest::skip())
{
    const auto manifest = readerScratch().parent_path() / "e2-reader-corpus/manifest.tsv";
    std::ifstream list(manifest);
    REQUIRE(list.good());
    struct Counts { unsigned missions = 0, loaded = 0, models = 0, defaults = 0, rejected = 0; };
    std::map<std::string, Counts> totals;
    const std::regex objective(R"(\[((?:Team[0-9]+)?Objective[0-9]+)\])");
    for (std::string row; std::getline(list, row);)
    {
        const auto tab = row.find('\t');
        REQUIRE(tab != row.npos);
        const auto group = row.substr(0, tab), path = row.substr(tab + 1);
        auto& count = totals[group];
        ++count.missions;
        EditorFit::MissionTextReader reader;
        REQUIRE(reader.open(path.c_str()) == NO_ERR);
        std::ifstream input(path, std::ios::binary);
        const std::string source{std::istreambuf_iterator<char>(input), {}};
        bool loaded = true;
        unsigned models = 0;
        for (auto match = std::sregex_iterator(source.begin(), source.end(), objective); match != std::sregex_iterator(); ++match)
        {
            const auto block = (*match)[1].str();
            REQUIRE(reader.seekBlock(block.c_str()) == NO_ERR);
            ObjectiveModelDefinition model;
            const auto result = EditorFit::readObjectiveModel(reader, model);
            loaded = result.accepted && loaded;
            count.defaults += std::popcount(result.defaultedFields);
            count.rejected += std::popcount(result.rejectedFields);
            ++models;
        }
        REQUIRE(models > 0);
        count.models += models;
        count.loaded += loaded;
    }
    const std::map<std::string, Counts> expected = {
        {"stock", {24, 24, 173, 0, 0}}, {"MC2-Exodus", {62, 62, 397, 0, 0}},
        {"MC2-MagicExpansion", {32, 32, 242, 0, 0}}, {"MC2X-CVE-G", {32, 32, 344, 0, 0}},
        {"MC2X-REAP3", {33, 33, 376, 0, 0}}, {"MCO-Carver5", {24, 24, 173, 0, 0}},
        {"MechCommanderOmnitech", {67, 67, 445, 0, 0}}
    };
    REQUIRE(totals.size() == expected.size());
    for (const auto& [group, count] : totals)
    {
        INFO(group);
        REQUIRE(expected.contains(group));
        const auto& baseline = expected.at(group);
        CHECK(count.missions == baseline.missions);
        CHECK(count.loaded == count.missions);
        CHECK(count.models == baseline.models);
        CHECK(count.defaults == baseline.defaults);
        CHECK(count.rejected == baseline.rejected);
        fprintf(stderr, "[EDITOR_DOCUMENT v1] event=model_corpus group=%s scope=objective_model missions=%u loaded=%u models=%u fields_defaulted=%u fields_rejected=%u\n",
            group.c_str(), count.missions, count.loaded, count.models, count.defaults, count.rejected);
    }
}
