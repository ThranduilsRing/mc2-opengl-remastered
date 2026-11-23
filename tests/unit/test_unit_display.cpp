// Trigger: code/unit_display_name.h and code/icon_registry.h (firstCell, fitCell, sheetServes), with the
// mech-bay name and icon paths that call them. The fixtures under fixtures/unit_display are copied
// read-only from the MCO install (data/objects/ryoken.csv, compbas.csv, data/art/buildings.csv), from the
// stock buildings.csv and from data/defs/text/en_us/mc2res_legacy.fit.
#include "doctest.h"
#include "unit_display_name.h"
#include "icon_registry.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string fixture(const char* name)
{
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "unit_display" / name;
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

// The cells of one CSV line, honoring the quotes the authoring sheets use.
std::vector<std::string> cells(const std::string& line)
{
    std::vector<std::string> out;
    std::string cell;
    bool quoted = false;
    for (size_t i = 0; i <= line.size(); ++i)
    {
        const char c = i < line.size() ? line[i] : ',';
        if (c == '"') { quoted = !quoted; continue; }
        if (c == ',' && !quoted) { out.push_back(cell); cell.clear(); continue; }
        cell += c;
    }
    return out;
}

// The cell after `label` in the first row that starts with it: what CSVFile hands the loader for that row.
std::string cellAfter(const std::string& csv, const std::string& label)
{
    std::istringstream lines(csv);
    for (std::string line; std::getline(lines, line);)
    {
        const auto row = cells(line);
        if (!row.empty() && row[0] == label) return row.size() > 1 ? row[1] : std::string();
    }
    return {};
}

// The string table's text for a legacy slot.
std::string slot(long id)
{
    const std::string table = fixture("string_table.fit");
    const auto key = table.find("key = \"legacy.mc2res." + std::to_string(id) + "\"");
    if (key == std::string::npos) return {};
    const auto text = table.find("text = \"", key) + 8;
    return table.substr(text, table.find('"', text) - text);
}

// What LogisticsChassis::getDisplayName asks the resolver, in its order.
std::string chassisName(long nameID, const std::string& freeText, const std::string& variant, const std::string& stem)
{
    return mc2::names::resolve(slot(nameID), freeText, variant, stem);
}

// What MasterComponent::initEXCEL and LogisticsComponent::init ask: the row's name slot, then its authored name.
std::string componentName(const std::string& row)
{
    const auto fields = cells(row);
    return mc2::names::resolve(slot(32000 + std::stol(fields[0])), fields[2], "", "");
}

// A sheet of cw x ch cells; each cell holds a mech-sized opaque block with a transparent margin.
std::vector<uint32_t> sheet(int cols, int rows, int cw, int ch)
{
    std::vector<uint32_t> pixels(static_cast<size_t>(cols) * cw * rows * ch, 0);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            for (int y = 6; y < ch - 6; ++y)
                for (int x = 5; x < cw - 5; ++x)
                    pixels[static_cast<size_t>(r * ch + y) * cols * cw + c * cw + x] = 0xff00ff00;
    return pixels;
}
}

TEST_SUITE("unit display") {

TEST_CASE("unit display: the MCO Storm Crow row resolves to a real name at every step") {
    const std::string ryoken = fixture("ryoken.csv");
    const std::string nameID = cellAfter(ryoken, "DescIndex");
    REQUIRE(nameID == "32081");
    // The mod's slot is one the stock table never filled: this is the word the mech bay showed.
    CHECK_FALSE(mc2::names::usable(slot(std::stol(nameID))));
    CHECK(chassisName(std::stol(nameID), cellAfter(ryoken, "MechName"), cellAfter(ryoken, "VariantName"), "ryoken") == "Storm Crow");
    // No free-text name: the variant names the chassis; neither: the file does.
    CHECK(chassisName(std::stol(nameID), "", cellAfter(ryoken, "VariantName"), "ryoken") == "Storm Crow Prime");
    CHECK(chassisName(std::stol(nameID), "", "", "ryoken") == "ryoken");
}

TEST_CASE("unit display: a stock mech keeps its string-table name and a free-text NameID cell names the chassis") {
    std::istringstream rows(fixture("buildings.csv"));
    std::string mco, stock, freeText;
    for (std::string line; std::getline(rows, line);)
        (line.rfind("RyokenCustom,", 0) == 0 ? freeText : cells(line)[1] == "4" ? stock : mco) = line;
    REQUIRE(cells(stock).size() > 3);
    CHECK(slot(std::stol(cells(stock)[2])) == "Ryoken");
    CHECK(chassisName(std::stol(cells(stock)[2]), "Mad Dog", "Ryoken Prime", "ryoken") == "Ryoken");
    // MCO's row points the same chassis at its own slot; the free-text row carries a name, not a number.
    CHECK(cells(mco)[2] == "32081");
    CHECK(chassisName(0, cells(freeText)[2], "", "ryokencustom") == "Storm Crow Custom");
}

TEST_CASE("unit display: a mod weapon row falls back to its authored name, a stock row keeps its slot") {
    std::istringstream rows(fixture("compbas.csv"));
    for (std::string line; std::getline(rows, line);) {
        const std::string authored = cells(line)[2];
        if (cells(line)[0] == "1") CHECK(componentName(line) == "Fusion Engine");
        else {
            CHECK_FALSE(mc2::names::usable(slot(32128)));
            CHECK(componentName(line) == authored);
        }
    }
}

TEST_CASE("unit display: placeholders and missing markers are not names") {
    CHECK(mc2::names::usable("Storm Crow"));
    for (const char* text : {"", "undefined", "Undefined", " <missing-string:32081>", "NULL for ID: 0"})
        CHECK_FALSE(mc2::names::usable(text));
    CHECK(mc2::names::resolve("", "", "", "").empty());
}

TEST_CASE("unit icons: a sheet that reached the named path yields its first cell, one icon stays whole") {
    using unit_icons::firstCell;
    const std::pair<int, int> cell{25, 30}, whole{64, 64};
    CHECK(firstCell(100, 60, sheet(4, 2, 25, 30)) == cell);                 // the four-over-three strip the bay showed
    CHECK(firstCell(25, 60, sheet(1, 2, 25, 30)) == cell);                  // a two-row HUD strip
    CHECK(firstCell(64, 64, sheet(1, 1, 64, 64)) == whole);                 // MCO's own 64x64 icon, margins and all
    auto hollow = sheet(1, 1, 64, 64);
    for (uint32_t& p : hollow) p = 0;                                        // one mech in two pieces: still one icon
    for (int y = 0; y < 64; ++y) for (int x : {8, 20, 36, 50}) hollow[static_cast<size_t>(y) * 64 + x] = 0xff00ff00;
    CHECK(firstCell(64, 64, hollow) == whole);
}

TEST_CASE("unit icons: an icon of any source size maps inside the cell rect") {
    using unit_icons::fitCell;
    const std::array<int, 4> exact{0, 0, 25, 30}, square{0, 2, 25, 25}, strip{0, 12, 40, 24}, empty{0, 0, 10, 10};
    CHECK(fitCell(25, 30, 25, 30) == exact);
    CHECK(fitCell(25, 30, 64, 64) == square);
    CHECK(fitCell(40, 48, 100, 60) == strip);                               // a 4x2 strip, one cell high
    CHECK(fitCell(10, 10, 0, 0) == empty);
    for (const auto [cw, ch, iw, ih] : {std::array{25, 30, 64, 64}, std::array{40, 48, 16, 16}, std::array{96, 45, 300, 20}}) {
        const auto [x, y, w, h] = fitCell(cw, ch, iw, ih);
        CHECK(x >= 0);
        CHECK(y >= 0);
        CHECK(w > 0);
        CHECK(h > 0);
        CHECK(w <= cw);
        CHECK(h <= ch);
        CHECK(2 * x + w <= cw);
        CHECK(2 * y + h <= ch);
    }
}

TEST_CASE("unit icons: a sheet serves only the chassis the same source owns") {
    using unit_icons::sheetServes;
    CHECK(sheetServes(1000000, 1000000));   // base sheet, base chassis
    CHECK(sheetServes(999998, 999998));     // both loose in the install
    CHECK(sheetServes(5, 1000000));         // a mod's sheet may serve a base chassis
    CHECK(sheetServes(5, 5));               // a mod's sheet serves its own chassis
    CHECK_FALSE(sheetServes(1000000, 5));   // a mod chassis has no cell in a base sheet
    CHECK_FALSE(sheetServes(1000000, 999998));
}

}
