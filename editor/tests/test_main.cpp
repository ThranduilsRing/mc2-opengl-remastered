#define DOCTEST_CONFIG_IMPLEMENT
#include "../../tests/unit/doctest.h"
#include "TestInputs.h"
#include "ObjectCatalog.h"

#include <iostream>
#include <fstream>
#include <string_view>
#include <vector>
#ifdef MC2_EDITOR_READER_TESTS
#include "PanelProof.h"
#endif

namespace editor::tests {
namespace {
// The main stack owns fixture paths; doctest callbacks borrow them only during the run.
const TestInputs* activeInputs{};
}

const TestInputs& inputs() { return *activeInputs; }

int run(int argc, char** argv)
{
    TestInputs fixtures;
    auto executable = std::filesystem::absolute(argv[0]);
    auto root = executable.parent_path();
    while (!std::filesystem::is_directory(root / "editor/tests/goldens") && root.has_relative_path())
        root = root.parent_path();
    if (!std::filesystem::is_directory(root / "editor/tests/goldens")) {
        std::cerr << "Cannot locate editor fixtures above " << executable << '\n';
        return 2;
    }
    fixtures.repositoryRoot = root;
    std::vector<const char*> arguments{argv[0]};
    bool requireCorpus{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        std::filesystem::path* destination{};
        std::string_view option;
        for (const auto name : {std::string_view("--stock-dir"), std::string_view("--byte-grade-dir"),
            std::string_view("--placement-base"), std::string_view("--placement-mods"),
            std::string_view("--placement-goldens"), std::string_view("--placement-output")}) {
            if (argument == name || argument.starts_with(std::string(name) + '=')) {
                option = name;
                if (name == "--stock-dir") destination = &fixtures.stockDirectory;
                else if (name == "--byte-grade-dir") destination = &fixtures.byteGradeDirectory;
                else if (name == "--placement-base") destination = &fixtures.placementBase;
                else if (name == "--placement-mods") destination = &fixtures.placementMods;
                else if (name == "--placement-goldens") destination = &fixtures.placementGoldens;
                else destination = &fixtures.placementOutput;
                break;
            }
        }
        if (destination) {
            std::string_view value;
            if (argument.size() > option.size()) value = argument.substr(option.size() + 1);
            else if (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with('-')) value = argv[++i];
            if (value.empty()) {
                std::cerr << option << " requires a nonempty path\n";
                return 2;
            }
            *destination = value;
        } else if (argument == "--require-stock-corpus") requireCorpus = true;
        else arguments.push_back(argv[i]);
    }

    // Keep generated inputs and all relative scratch paths outside the source tree.
    const auto output = std::filesystem::temp_directory_path() / "mc2-editor-tests" / executable.stem();
    std::filesystem::create_directories(output);
    for (auto* path : {&fixtures.stockDirectory, &fixtures.byteGradeDirectory, &fixtures.placementBase,
        &fixtures.placementMods, &fixtures.placementGoldens, &fixtures.placementOutput})
        if (!path->empty()) *path = std::filesystem::absolute(*path);
    std::filesystem::current_path(output);
    if (fixtures.placementBase.empty()) {
        for (const auto& candidate : {root, std::filesystem::path("G:/mc2-installs/_lean-seed"),
            std::filesystem::path("A:/Games/mc2-opengl/mc2-win64-v0.4c"),
            std::filesystem::path("A:/Games/mc2-opengl/mc2-win64-v0.4")}) {
            if (std::filesystem::is_regular_file(candidate / "mission.fst")) {
                fixtures.placementBase = candidate;
                break;
            }
        }
    }
    if (fixtures.placementMods.empty()) fixtures.placementMods = "A:/Games/mc2-opengl";
    if (fixtures.placementGoldens.empty()) fixtures.placementGoldens = root / "editor/tests/goldens/e5a-stock";
    if (fixtures.placementOutput.empty()) fixtures.placementOutput = output / "placements";
    std::filesystem::create_directories(fixtures.placementOutput);
    for (const auto* extension : {".fit", ".pak"})
        std::filesystem::copy_file(fixtures.placementGoldens / (std::string("e5_reference_before") + extension),
            fixtures.placementOutput / (std::string("e5_reference_before") + extension),
            std::filesystem::copy_options::overwrite_existing);
#ifdef MC2_EDITOR_READER_TESTS
    const bool generateReaderFixture = fixtures.byteGradeDirectory.empty();
#endif
    if (fixtures.byteGradeDirectory.empty()) fixtures.byteGradeDirectory = output / "byte-grade";
    std::filesystem::create_directories(fixtures.byteGradeDirectory);
    if (fixtures.stockDirectory.empty()) {
        fixtures.stockDirectory = output / "stock";
        std::filesystem::create_directories(fixtures.stockDirectory);
        auto resolver = document::FileResolver::mount({fixtures.placementBase});
        if (!resolver) { std::cerr << "Cannot mount stock editor inputs\n"; return 2; }
        for (int i = 1; i <= 24; ++i) {
            const auto stem = std::string("mc2_") + (i < 10 ? "0" : "") + std::to_string(i);
            for (const auto* extension : {".fit", ".pak"}) {
                const auto name = stem + extension;
                auto file = resolver->read("data/missions/" + name);
                if (!file) { std::cerr << "Missing stock fixture " << name << '\n'; return 2; }
                std::ofstream stream(fixtures.stockDirectory / name, std::ios::binary);
                stream.write(reinterpret_cast<const char*>(file->bytes.data()), file->bytes.size());
            }
        }
        requireCorpus = true;
    }
    std::error_code error;
    const bool haveStock = !fixtures.stockDirectory.empty() && std::filesystem::is_directory(fixtures.stockDirectory, error);
    if (requireCorpus) {
        if (!haveStock) {
            std::cerr << "Stock corpus proof failed: --stock-dir must name a directory containing all 24 FIT/PAK pairs\n";
            return 1;
        }
        for (int i = 1; i <= 24; ++i) {
            const auto stem = std::string("mc2_") + (i < 10 ? "0" : "") + std::to_string(i);
            for (const auto extension : {".fit", ".pak"}) {
                const auto path = fixtures.stockDirectory / (stem + extension);
                if (!std::filesystem::is_regular_file(path, error)) {
                    std::cerr << "Stock corpus proof failed: missing fixture " << path << '\n';
                    return 1;
                }
            }
        }
    }

    doctest::Context context(static_cast<int>(arguments.size()), arguments.data());
    if (!fixtures.placementBase.empty()) {
        auto resolver = document::FileResolver::mount({fixtures.placementBase});
        if (!resolver) { std::cerr << "Marker catalog mount failed\n"; return 2; }
        fixtures.markerObjects = document::ObjectCatalog::load(*resolver, {}).navMarkerObjects();
    }
    if (fixtures.placementBase.empty()) context.addFilter("test-suite-exclude", "placement-live");
    else if (fixtures.placementMods.empty() || fixtures.placementGoldens.empty() || fixtures.placementOutput.empty()) {
        std::cerr << "Placement proof requires --placement-base/mods/goldens/output\n"; return 2;
    }
    if (!haveStock) {
        std::cout << "SKIPPED stock-fixture tests: --stock-dir was not supplied or is not an accessible directory\n";
        context.addFilter("test-suite-exclude", "stock-fixture");
    }
    activeInputs = &fixtures;
#ifdef MC2_EDITOR_READER_TESTS
    if (generateReaderFixture) {
        auto seed = fixtures.placementGoldens / "e5_reference_before";
        auto doc = document::MissionDocument::read(seed.string() + ".fit", seed.string() + ".pak", {}, fixtures.markerObjects);
        if (!doc) { std::cerr << "Cannot load reader seed\n"; return 2; }
        document::CommandHistory history(*doc);
        shell::EntityPanels panels(*doc, history);
        auto* imgui = ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1440, 960); io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels{}; int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        shell::PanelProof proof(*doc, history, panels, fixtures.byteGradeDirectory, "objectives_panel", [](const std::string&) {}, false);
        bool running = true;
        while (running) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(1440, 230));
            ImGui::Begin("Files"); proof.drawFiles(); ImGui::End();
            panels.draw(); ImGui::Render(); running = proof.tick();
        }
        ImGui::DestroyContext(imgui);
        if (!proof.passed()) { std::cerr << "Reader fixture generation failed\n"; return 2; }
    }
#endif
    const int result = context.run();
    activeInputs = nullptr;
    if (context.shouldExit())
        std::cout << "Fixture options: --stock-dir PATH --byte-grade-dir PATH --require-stock-corpus\n";
    return result;
}
}

int main(int argc, char** argv) { return editor::tests::run(argc, argv); }
