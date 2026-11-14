// The immutable Vulkan geometry must include support craft on both load paths.
#include "doctest.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST_CASE("support_craft: preload precedes immutable geometry on mission init and quickload") {
    const auto root = std::filesystem::path(MC2_REPO_ROOT);
    for (const auto* path : {"code/mission_init.cpp", "code/saveload_load.cpp"}) {
        CAPTURE(path);
        std::ifstream file(root / path);
        REQUIRE(file.good());
        const std::string source((std::istreambuf_iterator<char>(file)), {});
        const auto preload = source.find("\n\tpreloadMissionSupportAtLoad();");
        const auto finalize = source.find(std::string(path) == "code/mission_init.cpp"
            ? "\n\tinitFinalizeGeometry();" : "\n\tGameAdapters::StaticProp::finalizeGeometry();");
        REQUIRE(preload != std::string::npos);
        REQUIRE(finalize != std::string::npos);
        CHECK(preload < finalize);
    }
}
