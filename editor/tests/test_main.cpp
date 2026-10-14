#define DOCTEST_CONFIG_IMPLEMENT
#include "../../tests/unit/doctest.h"
#include "TestInputs.h"

#include <iostream>
#include <string_view>
#include <vector>

namespace editor::tests {
namespace {
// The main stack owns fixture paths; doctest callbacks borrow them only during the run.
const TestInputs* activeInputs{};
}

const TestInputs& inputs() { return *activeInputs; }

int run(int argc, char** argv)
{
    TestInputs fixtures;
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
    if (fixtures.placementBase.empty()) context.addFilter("test-suite-exclude", "placement-live");
    else if (fixtures.placementMods.empty() || fixtures.placementGoldens.empty() || fixtures.placementOutput.empty()) {
        std::cerr << "Placement proof requires --placement-base/mods/goldens/output\n"; return 2;
    }
    if (!haveStock) {
        std::cout << "SKIPPED stock-fixture tests: --stock-dir was not supplied or is not an accessible directory\n";
        context.addFilter("test-suite-exclude", "stock-fixture");
    }
    activeInputs = &fixtures;
    const int result = context.run();
    activeInputs = nullptr;
    if (context.shouldExit())
        std::cout << "Fixture options: --stock-dir PATH --byte-grade-dir PATH --require-stock-corpus\n";
    return result;
}
}

int main(int argc, char** argv) { return editor::tests::run(argc, argv); }
