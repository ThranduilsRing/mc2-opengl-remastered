// Trigger: code/unit_display_name.h (resolve, readNameCell) and code/icon_registry.h (firstCell, fitCell,
// fitWidget, sheetServes, Registry::select: the order apply() draws by), with the mech-bay name and icon paths
// that call them. The fixtures under fixtures/unit_display are copied read-only from the MCO install
// (data/objects/ryoken.csv, compbas.csv, data/art/buildings.csv), from the stock buildings.csv (header row)
// and from data/defs/text/en_us/mc2res_legacy.fit; buildings.csv is the header and three mech rows.
#include "doctest.h"
#include "csvfile.h"
#include "unit_display_name.h"
#include "icon_registry.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string fixture(const char* name)
{
    const auto path = std::filesystem::path(MC2_TEST_FIXTURE_DIR) / "unit_display" / name;
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

// aObject's rect contract: move() is relative, resize() keeps the top-left, width()/height() are floats.
struct FakeWidget {
    float x = 0, y = 0, w = 0, h = 0;
    float width() const { return w; }
    float height() const { return h; }
    void move(float dx, float dy) { x += dx; y += dy; }
    void resize(long ww, long hh) { w = static_cast<float>(ww); h = static_cast<float>(hh); }
};

// Art as init() registers it: named icons with their source's rank, and a sheet of eight 25 x 30 cells.
// Ranks are LookupModPriority's: 0 the active mod, 1 its dependency, 999998 loose in the install, 1000000 stock.
struct Art {
    unit_icons::Registry registry;
    std::map<std::string, int> priority;
    Art() { registry.add("missing", 16, 16, std::vector<uint32_t>(256, 0xff202020)); }
    const unit_icons::Icon* icon(const std::string& key, int rank, int size = 64) {
        registry.add(key, size, size, std::vector<uint32_t>(static_cast<size_t>(size) * size, 0xff00ff00));
        priority[key] = rank;
        return registry.find(key);
    }
    const unit_icons::Icon* cell(const std::string& key, long index) {
        if (!registry.find(unit_icons::legacyKey(key, 0))) registry.sheet(key, sheet(8, 1, 25, 30), 200, 30, 25, 30);
        return registry.find(unit_icons::legacyKey(key, index));
    }
    // What apply() asks for one chassis: null is the placeholder it draws.
    const unit_icons::Icon* pick(const char* stem, const char* name, long index, const std::string& file,
                                 int dataRank, int sheetRank) {
        const unit_icons::Query query{stem, name, file, index, dataRank, sheetRank, {}};
        return registry.select(priority, query).icon;
    }
};
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
    for (std::string line; std::getline(rows, line);) {
        const auto row = cells(line);
        if (row.size() < 4 || row[3] != "MECH") continue;                    // the stock header row
        (line.rfind("RyokenCustom,", 0) == 0 ? freeText : row[1] == "4" ? stock : mco) = line;
    }
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

TEST_CASE("unit icons: a sheet serves only the chassis the same source declares") {
    using unit_icons::sheetServes;
    CHECK(sheetServes(1000000, 1000000));   // base sheet, base chassis
    CHECK(sheetServes(999998, 999998));     // both loose in the install
    CHECK(sheetServes(5, 1000000));         // a mod's sheet may serve a base chassis
    CHECK(sheetServes(5, 5));               // a mod's sheet serves its own chassis
    CHECK(sheetServes(1000000, 999998));    // v0.4c: loose chassis CSV, sheet packed with the stock data
    CHECK_FALSE(sheetServes(1000000, 5));   // a mod chassis has no cell in a base sheet
    CHECK_FALSE(sheetServes(5, 6));         // nor in another mod's sheet: that index names another mech
}

TEST_CASE("unit icons: a mod mech's authored-name icon wins over the stock stem icon another source owns") {
    const std::string sheet = "data/art/mcui_gn_mechicons.tga";
    Art art;
    const auto* stockRyoken = art.icon("mech/ryoken", 999998);    // the install's loose icon for that stem
    const auto* stormCrow = art.icon("mech/storm crow", 0);       // MCO's own file
    const auto* cell = art.cell(sheet, 4);
    REQUIRE(stockRyoken != stormCrow);
    // MCO's data/objects/ryoken.csv is the Storm Crow: the stock Ryoken art is another mech's.
    CHECK(art.pick("ryoken", "storm crow", 4, sheet, 0, 1000000) == stormCrow);
    // A stock Ryoken keeps the install's icon.
    CHECK(art.pick("ryoken", "ryoken", 4, sheet, 999998, 1000000) == stockRyoken);
    CHECK(art.pick("ryoken", "ryoken", 4, sheet, 1000000, 1000000) == stockRyoken);
}

TEST_CASE("unit icons: a mod chassis sharing a stock stem gets no stock art, by the stem or by the HUD path") {
    const std::string sheet = "data/art/mc2x_mechicons.tga";
    // What apply() asks for a mod's data/objects/ryoken.csv (the Storm Crow): data rank 0, the stock sheet at
    // 1000000, HUD front art keyed by the stem.
    const unit_icons::Query stormCrow{"ryoken", "storm crow", sheet, 4, 0, 1000000, {"hud/front/ryoken"}};
    Art stock;
    const auto* stockIcon = stock.icon("mech/ryoken", 999998);           // the install's loose icon for that stem
    const auto* stockHud = stock.icon("hud/front/ryoken", 999998);
    stock.cell(sheet, 4);
    REQUIRE(stockIcon != nullptr);
    REQUIRE(stockHud != nullptr);
    // Only the stock stem icon and the stock HUD file exist: both are another mech's, the placeholder it is.
    CHECK(stock.registry.select(stock.priority, stormCrow).icon == nullptr);
    CHECK(stock.registry.select(stock.priority, stormCrow).hud == nullptr);
    // The mod's own HUD front art serves its chassis.
    Art mod;
    const auto* ownHud = mod.icon("hud/front/ryoken", 0);
    CHECK(mod.registry.select(mod.priority, stormCrow).icon == ownHud);
    // A stock Ryoken keeps the install's HUD art.
    const unit_icons::Query stockRyoken{"ryoken", "ryoken", sheet, 4, 999998, 1000000, {"hud/front/ryoken"}};
    CHECK(stock.registry.select(stock.priority, stockRyoken).icon == stockHud);
}

TEST_CASE("unit icons: a BT2018-style mod shows its one own icon or the placeholder, never another mech's cell") {
    const std::string sheet = "data/art/mc2x_mechicons.tga";
    Art art;
    const auto* cell = art.cell(sheet, 3);                        // the stock cell the copied CSV still names
    // One own icon (data/art/mechicons/bt2018_enforcer.png), authored "Enforcer MC2 Adaptation": the stem comes first.
    const auto* own = art.icon("mech/bt2018_enforcer", 0);
    art.icon("mech/enforcer mc2 adaptation", 0);
    CHECK(art.pick("bt2018_enforcer", "enforcer mc2 adaptation", 3, sheet, 0, 1000000) == own);
    // No own art: the placeholder, although the stock sheet has the cell the CSV names.
    CHECK(art.pick("bt2018_hunchback", "hunchback mc2 adaptation", 3, sheet, 0, 1000000) == nullptr);
    // Another mod's drop-in sheet (Exodus, MCO ship the stock path) is not this mod's layout either.
    CHECK(art.pick("bt2018_hunchback", "hunchback mc2 adaptation", 3, sheet, 0, 1) == nullptr);
    // A dependency's file that happens to carry the authored name is not the mod's art.
    Art borrowed;
    borrowed.icon("mech/hunchback mc2 adaptation", 1);
    borrowed.cell(sheet, 3);
    CHECK(borrowed.pick("bt2018_hunchback", "hunchback mc2 adaptation", 3, sheet, 0, 1000000) == nullptr);
    // The mod's own sheet does carry its chassis.
    CHECK(art.pick("bt2018_hunchback", "hunchback mc2 adaptation", 3, sheet, 0, 0) == cell);
    // A stock mech keeps its cell under the stock sheet, under a loose CSV, and under a mod's drop-in sheet.
    CHECK(art.pick("madcat", "mad cat", 3, sheet, 1000000, 1000000) == cell);
    CHECK(art.pick("madcat", "mad cat", 3, sheet, 999998, 1000000) == cell);
    CHECK(art.pick("madcat", "mad cat", 3, sheet, 1000000, 0) == cell);
}

TEST_CASE("unit icons: an unrelated sheet of a lower rank holds no precedence over a dependency chassis' own art") {
    const std::string sheet = "data/art/mc2x_mechicons.tga";
    constexpr int active = 0, dependency = 1;
    Art dep;
    const auto* cell = dep.cell(sheet, 3);
    const auto* own = dep.icon("mech/depmech", dependency);
    const unit_icons::Query query{"depmech", "depmech", sheet, 3, dependency, active, {}};
    REQUIRE(cell != own);
    CHECK(dep.registry.select(dep.priority, query).icon == own);
    CHECK(dep.registry.select(dep.priority, query).own);
    Art depHud;
    const auto* ownHud = depHud.icon("hud/front/depmech", dependency);
    depHud.cell(sheet, 3);
    const unit_icons::Query hudQuery{"depmech", "depmech", sheet, 3, dependency, active, {"hud/front/depmech"}};
    CHECK(depHud.registry.select(depHud.priority, hudQuery).icon == ownHud);
    CHECK(dep.pick("othermech", "othermech", 3, sheet, dependency, active) == nullptr);
}

TEST_CASE("unit icons: a dependency's chassis keeps its authored-name icon under the active mod's sheet of the stock "
          "path, the Exodus/MagicExpansion layering") {
    const std::string sheet = "data/art/mcui_gn_mechicons.tga";
    constexpr int active = 0, dependency = 1;
    const std::string name = unit_icons::canonical(cellAfter(fixture("ryoken.csv"), "MechName"));
    Art dep;
    const auto* cell = dep.cell(sheet, 4);
    const auto* stormCrow = dep.icon("mech/" + name, dependency);
    const unit_icons::Query query{"ryoken", name, sheet, 4, dependency, active,
                                  {"hud/front/ryoken", "hud/mcui_high7/front/ryoken", "hud/mcui_high8/front/ryoken"}};
    REQUIRE(cell != stormCrow);
    CHECK(dep.registry.select(dep.priority, query).icon == stormCrow);
    CHECK(dep.registry.select(dep.priority, query).own);
}

TEST_CASE("unit icons: a card authored for a larger box lands inside the page's slot") {
    using unit_icons::cardScale;
    using unit_icons::scaleCardChild;
    CHECK(cardScale(75, 64, 75, 64) == std::array<float, 2>{1.f, 1.f});    // stock card: nothing moves
    CHECK(cardScale(0, 64, 90, 80) == std::array<float, 2>{1.f, 1.f});
    // MCO's 90 x 80 box into the stock 75 x 64 slot: every child inside the slot, the art in proportion.
    const auto k = cardScale(75, 64, 90, 80);
    FakeWidget icon{3, 31, 40, 48}, pilot{51, 30, 33, 46}, link{1, 27, 84, 52}, name{4, 4, 220, 24};
    scaleCardChild(icon, 3, 31, k, true);
    scaleCardChild(pilot, 51, 30, k, true);
    scaleCardChild(link, 1, 27, k, true);
    scaleCardChild(name, 4, 4, k, false);
    CHECK(icon.w == 33);
    CHECK(icon.h == 38);
    CHECK(pilot.w == 28);
    CHECK(pilot.h == 37);
    CHECK(icon.x + icon.w <= 75);
    CHECK(pilot.x + pilot.w <= 75);
    CHECK(pilot.y + pilot.h <= 64);
    CHECK(link.x + link.w <= 75);
    CHECK(link.y + link.h <= 64);
    CHECK(name.w == 220);                                                    // text keeps its own size
    CHECK(name.x == doctest::Approx(4 * 75.f / 90));
    const FakeWidget same = [&] { FakeWidget w{3, 31, 25, 30}; scaleCardChild(w, 3, 31, cardScale(75, 64, 75, 64), true); return w; }();
    CHECK(same.x == 3);
    CHECK(same.y == 31);
    CHECK(same.w == 25);
    CHECK(same.h == 30);
}

TEST_CASE("unit icons: reassigning a cell's icon never moves the cell") {
    using unit_icons::fitCell;
    using unit_icons::fitWidget;
    // A 25 x 30 cell at (100, 200). 64 x 64 and 100 x 60 leave an odd padding whose float half is .5.
    FakeWidget widget{100, 200, 25, 30};
    for (int round = 0; round < 4; ++round)
        for (const auto [iw, ih] : {std::array{64, 64}, std::array{100, 60}, std::array{25, 30}, std::array{16, 16}}) {
            const std::array<int, 4> expected = fitCell(25, 30, iw, ih);
            fitWidget(widget, 25, 30, iw, ih);
            CHECK(widget.x == 100 + expected[0]);
            CHECK(widget.y == 200 + expected[1]);
            CHECK(widget.w == expected[2]);
            CHECK(widget.h == expected[3]);
        }
    CHECK(widget.y == 200 + fitCell(25, 30, 16, 16)[1]);
}

TEST_CASE("unit display: the master list's NameID cell reads as a slot or as free text through the real CSVFile") {
    const auto path = std::filesystem::path(MC2_TEST_FIXTURE_DIR) / "unit_display" / "buildings.csv";
    CSVFile master;
    REQUIRE(master.open(path.string().c_str()) == NO_ERR);
    // Row 1 is the header, rows 2..4 the mechs: the file's last row is its last mech, and the loader stops one
    // row later, where readString fails.
    long id = -1;
    REQUIRE(master.readLong(4, 3, id) == NO_ERR);
    CHECK(id == 0);
    char text[256] = "";
    mc2::names::readNameCell(master, 2, 3, id, text, sizeof(text));     // MCO's row: its own slot
    CHECK(id == 32081);
    CHECK(std::string(text).empty());
    mc2::names::readNameCell(master, 3, 3, id, text, sizeof(text));     // the stock row: the stock slot
    CHECK(id == 30915);
    CHECK(std::string(text).empty());
    id = 0;
    text[0] = 'x';
    mc2::names::readNameCell(master, 4, 3, id, text, sizeof(text));     // free text: the chassis' name
    CHECK(id == 0);
    CHECK(std::string(text) == "Storm Crow Custom");
    CHECK(mc2::names::resolve(slot(id), text, "", "ryokencustom") == "Storm Crow Custom");
    char type[256] = "";
    CHECK(master.readString(4, 4, type, sizeof(type)) == NO_ERR);        // the file's last row
    CHECK(std::string(type) == "MECH");
    CHECK(master.readString(5, 4, type, sizeof(type)) != NO_ERR);        // the loader's break, one row later
    mc2::names::readNameCell(master, 5, 3, id, text, sizeof(text));     // past the last row: nothing
    CHECK(id == 0);
    CHECK(std::string(text) == "Storm Crow Custom");
}

TEST_CASE("unit display: a NameID cell is a slot only when the whole cell is one integer") {
    struct Ram : CSVFile {                                                  // a master list held in memory
        long load(const std::string& text) {
            const auto result = File::open(text.data(), static_cast<int>(text.size()));
            return result == NO_ERR ? afterOpen() : result;
        }
    } master;
    // Column 3 of rows 2..5: a name starting with a digit, one starting with a sign, a slot, a negative slot.
    REQUIRE(master.load("File Name,Group ID,NameID,Type\n"
                        "3DCustom,22,3D Custom,MECH\n"
                        "Uziel,22,+Uziel,MECH\n"
                        "Ryoken,22,32081,MECH\n"
                        "Vulture,22,-7,VEHICLE\n") == NO_ERR);
    long id = -1;
    char text[256] = "";
    mc2::names::readNameCell(master, 2, 3, id, text, sizeof(text));
    CHECK(id == -1);                                                       // no slot was read
    CHECK(std::string(text) == "3D Custom");
    text[0] = 'x';
    mc2::names::readNameCell(master, 3, 3, id, text, sizeof(text));
    CHECK(id == -1);
    CHECK(std::string(text) == "+Uziel");
    text[0] = 'x';
    mc2::names::readNameCell(master, 4, 3, id, text, sizeof(text));     // a slot: the caller's text is left alone
    CHECK(id == 32081);
    CHECK(text[0] == 'x');
    mc2::names::readNameCell(master, 5, 3, id, text, sizeof(text));     // the file's last row
    CHECK(id == -7);
}

}
