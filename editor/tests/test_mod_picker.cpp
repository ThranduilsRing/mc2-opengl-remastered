#include "../../tests/unit/doctest.h"
#include "../ModPickerPaths.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
fs::path tempInstall()
{
    const auto dir = fs::absolute(fs::path("scratch/mod-picker-tests") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir / "mods");
    return dir;
}
}

TEST_CASE("mod picker/enumerates mods/*/mod.json with id and name, skips folders without a manifest")
{
    const auto install = tempInstall();
    fs::copy("tests/fixtures/mods/minimal-mod", install / "mods" / "minimal-mod", fs::copy_options::recursive);
    fs::create_directories(install / "mods" / "no-manifest" / "data");
    const auto mods = ModPickerPaths::Scan(install / "mods");
    REQUIRE(mods.size() == 1);
    CHECK(mods[0].id == "minimal-mod");
    CHECK(mods[0].name == "Minimal mod");
    fs::remove_all(install);
}

TEST_CASE("mod picker/save dir is mods/<id>/data/missions for a mod and empty for stock")
{
    const auto install = tempInstall();
    fs::copy("tests/fixtures/mods/minimal-mod", install / "mods" / "minimal-mod", fs::copy_options::recursive);
    CHECK(ModPickerPaths::SaveDir(install / "mods", "", true).empty());
    CHECK_FALSE(fs::exists(install / "data" / "missions"));

    const auto dir = ModPickerPaths::SaveDir(install / "mods", "minimal-mod", true);
    CHECK(dir == install / "mods" / "minimal-mod" / "data" / "missions");
    REQUIRE(fs::is_directory(dir));
    // A save into that directory lands under the mod, never under stock data/missions.
    std::ofstream(dir / "unit.pak", std::ios::binary) << "x";
    CHECK(fs::exists(install / "mods" / "minimal-mod" / "data" / "missions" / "unit.pak"));
    CHECK_FALSE(fs::exists(install / "data" / "missions"));
    fs::remove_all(install);
}
