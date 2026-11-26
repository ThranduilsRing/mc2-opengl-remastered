#include "../../tests/unit/doctest.h"
#include "../LauncherMissions.h"
#include <fstream>

// Trigger paths: editor/LauncherMissions.h and editor/newsinglemission.cpp.
TEST_CASE("launcher/loose campaigns copy complete pairs without overwriting edits")
{
    const auto root = std::filesystem::current_path() / "launcher-test-install";
    REQUIRE(!std::filesystem::exists(root));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    const auto source = root / "data/missions";
    std::filesystem::create_directories(source);
    auto write = [](const auto& path, const char* value) { std::ofstream(path, std::ios::binary) << value; };
    auto read = [](const auto& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    CHECK(editor::launcher::campaignMissions(source).empty());
    for (const char* extension : {".pak", ".fit", ".abl"}) write(source / (std::string("mc2_01") + extension), extension);
    write(source / "mc2_02.pak", "incomplete");
    write(source / "mc2_m01.pak", "multiplayer");
    write(source / "mc2_m01.fit", "multiplayer");
    CHECK(editor::launcher::campaignMissions(source) == std::vector<std::string>{"mc2_01"});
    write(source / "mc2_03.pak", "later map");
    write(source / "mc2_03.fit", "later metadata");
    CHECK(editor::launcher::campaignMissions(source) == std::vector<std::string>{"mc2_01", "mc2_03"});
    std::error_code error;
    const auto first = editor::launcher::copyCampaign(root, "mc2_01", error);
    REQUIRE_FALSE(error);
    REQUIRE(std::filesystem::exists(first));
    CHECK(read(first) == ".pak");
    CHECK(read(first.parent_path() / "mc2_01.fit") == ".fit");
    CHECK(read(first.parent_path() / "mc2_01.abl") == ".abl");
    write(first, "owner edits");
    const auto second = editor::launcher::copyCampaign(root, "mc2_01", error);
    REQUIRE_FALSE(error);
    CHECK(first != second);
    CHECK(read(first) == "owner edits");
    CHECK(read(second) == ".pak");
    CHECK(read(source / "mc2_01.pak") == ".pak");
    std::filesystem::remove(source / "mc2_01.abl");
    CHECK_FALSE(editor::launcher::copyCampaign(root, "mc2_01", error).empty());
    CHECK_FALSE(error);
    CHECK(editor::launcher::copyCampaign(root, "../mc2_01", error).empty());
    CHECK(error);
    error.clear();
    CHECK(editor::launcher::copyCampaign(root, "mc2_02", error).empty());
    CHECK(error);
}
