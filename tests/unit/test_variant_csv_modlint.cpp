// tests/unit/test_variant_csv_modlint.cpp
// Offline doctest coverage for the pure legacy chassis-CSV variant-block
// validator (tools/mc2_modlint/modlint_variant_csv.h): the engine-faithful
// naive CSV parser (1-based rows/cols, strstr(',') split, no quote handling),
// the compbas populated-ordinal set, and every VARC_ rule with EXACT row/field
// reporting. The rules mirror the real consumers:
// LogisticsData::initVariants row walk (23+97n, stop at first empty name),
// LogisticsVariant::init loadout rows (26..96+off col 5, 0xff sentinel),
// BattleMech::init armor rows (115+off), getVariant()'s 8-bit index mask.
// Header-only, drags no engine/GL/IO.
#include "doctest.h"

#include "../../tools/mc2_modlint/modlint_variant_csv.h"

#include <string>
#include <vector>

namespace {

// Build a minimal but geometry-correct chassis CSV: 19 header rows, then
// variant blocks of exactly 97 rows each (name at 23+97n, items at 26..96+off,
// armor at 115+off). `loadouts` = per-block list of {itemRow(0-based within
// items), compbasId-as-string}.
struct BlockSpec {
    std::string name;
    std::vector<std::pair<int, std::string>> items;   // {item index 0..70, id string}
};

std::string makeChassisCsv(const std::vector<BlockSpec>& blocks, int truncateLastBlockAtRow = 0) {
    std::vector<std::string> rows;
    for (int r = 1; r <= 19; ++r)
        rows.push_back(r == 3 ? "MechName,TestMech,," : "Header,,,");
    for (size_t n = 0; n < blocks.size(); ++n) {
        std::vector<std::string> blk(97, ",,,,");            // rows 20+97n .. 116+97n
        blk[0] = "VariantInfo,,,,";                          // row 20+off
        blk[1] = "Identification,,,,";                       // 21+off
        blk[2] = "VariantID,000" + std::to_string(n) + ",,,";// 22+off
        blk[3] = "VariantName," + blocks[n].name + ",,,";    // 23+off
        blk[5] = "MasterComponentID,Load#,X,Y,CompBas#";     // 25+off
        for (int i = 0; i <= 70; ++i)                        // 26..96+off
            blk[6 + i] = "Item" + std::to_string(i) + "MasterId,94,-1,-1,255,EMPTY!";
        for (const auto& it : blocks[n].items)
            blk[6 + it.first] = "Item" + std::to_string(it.first) +
                                "MasterId,4,-1,-1," + it.second + ",SomeComp";
        blk[94] = "CurrentInternalStructure,3,17,17";        // 114+off
        blk[95] = "CurrentArmorPoints,9,8,8";                // 115+off
        for (const std::string& r : blk) rows.push_back(r);
    }
    if (truncateLastBlockAtRow > 0 && (size_t)truncateLastBlockAtRow < rows.size())
        rows.resize((size_t)truncateLastBlockAtRow);
    std::string out;
    for (const std::string& r : rows) out += r + "\n";
    return out;
}

bool hasReason(const std::vector<ModderFinding>& fs, const char* reason) {
    for (const auto& f : fs) if (f.reason == reason) return true;
    return false;
}

const ModderFinding* findReason(const std::vector<ModderFinding>& fs, const char* reason) {
    for (const auto& f : fs) if (f.reason == reason) return &f;
    return nullptr;
}

const std::string kCompbas =
    "Component Table,Type,Name\n"
    "0,undefined,undefined\n"
    "1,Engine,Fusion Engine\n"
    "4,Actuator,Arm Actuator\n"
    "39,undefined,undefined\n"
    "141,EnergyWeapon,ER Large Laser\n"
    "146,EnergyWeapon,ER PPC\n";

} // namespace

// ------------------------------------------------------------- parser -------

TEST_CASE("variant-csv: parser is 1-based and engine-faithful (absent cells empty)") {
    VariantCsvTable t = parseVariantCsv("a,b,c\r\nd,e\n");
    CHECK(t.rowCount() == 2);
    CHECK(t.cell(1, 1) == "a");
    CHECK(t.cell(1, 3) == "c");
    CHECK(t.cell(2, 2) == "e");
    CHECK(t.cell(2, 3) == "");     // absent col
    CHECK(t.cell(3, 1) == "");     // absent row
    CHECK(t.cell(0, 1) == "");     // 0 is not a valid 1-based index
}

TEST_CASE("variant-csv: naive comma split — quotes are NOT column-quoting (engine parity)") {
    VariantCsvTable t = parseVariantCsv("x,\"a, b\",y\n");
    CHECK(t.cell(1, 2) == "\"a");  // the engine's strstr(',') would split here too
    CHECK(t.cell(1, 3) == "b\"");
}

TEST_CASE("variant-csv: chassis heuristic keys on a VariantName label in col 1") {
    CHECK(looksLikeChassisCsv(parseVariantCsv("foo,,\nVariantName,Atlas Prime,\n")));
    CHECK_FALSE(looksLikeChassisCsv(parseVariantCsv("0,undefined,undefined\n1,Engine,Fusion\n")));
}

TEST_CASE("variant-csv: compbas populated set skips 'undefined' rows and the header") {
    std::set<int> ids = parseCompbasPopulatedIds(kCompbas);
    CHECK(ids.count(1) == 1);
    CHECK(ids.count(146) == 1);
    CHECK(ids.count(0) == 0);      // undefined
    CHECK(ids.count(39) == 0);     // undefined (free ordinal)
    CHECK(ids.size() == 4);
}

// --------------------------------------------------------------- rules ------

TEST_CASE("variant-csv: clean two-block chassis with resolvable loadout -> ZERO findings") {
    std::string csv = makeChassisCsv({
        { "Atlas Prime", { {0, "1"}, {22, "146"} } },
        { "Atlas AS7-X Sampler", { {0, "1"}, {22, "141"} } },
    });
    std::set<int> ids = parseCompbasPopulatedIds(kCompbas);
    auto fs = validateVariantCsvAdd(parseVariantCsv(csv), "atlas.csv", "sample_new_variant", &ids);
    for (const auto& f : fs) MESSAGE(formatModderFinding(f));
    CHECK(fs.empty());
}

TEST_CASE("variant-csv: the real sample_new_variant geometry (4 blocks, name at 314) is clean") {
    std::string csv = makeChassisCsv({
        { "Atlas Prime", {} }, { "B", {} }, { "C", {} }, { "Atlas AS7-X Sampler", {} } });
    VariantCsvTable t = parseVariantCsv(csv);
    CHECK(t.cell(314, 2) == "Atlas AS7-X Sampler");   // 23 + 97*3
    auto fs = validateVariantCsvAdd(t, "atlas.csv", "", nullptr);
    CHECK(fs.empty());
}

TEST_CASE("variant-csv: no variant block at all -> VARC_NO_VARIANT_BLOCK naming row 23") {
    auto fs = validateVariantCsvAdd(parseVariantCsv("VariantName,,\nHeader,,\n"),
                                    "broken.csv", "m", nullptr);
    const ModderFinding* f = findReason(fs, "VARC_NO_VARIANT_BLOCK");
    REQUIRE(f != nullptr);
    CHECK(f->field == "row 23 col 2");
    CHECK(f->source == "m");
    CHECK(f->file == "broken.csv");
}

TEST_CASE("variant-csv: off-stride name row -> VARC_OFF_STRIDE with exact and nearest rows") {
    // One good block, then a stray VariantName row jammed one row too early
    // (row 119 instead of 120): the classic botched-append.
    std::string csv = makeChassisCsv({ { "Atlas Prime", {} } });
    VariantCsvTable t = parseVariantCsv(csv);
    while (t.rows.size() < 119) t.rows.push_back({ "" });
    t.rows[118] = { "VariantName", "Botched Append" };   // row 119, (119-23)%97 == 96
    auto fs = validateVariantCsvAdd(t, "atlas.csv", "m", nullptr);
    const ModderFinding* f = findReason(fs, "VARC_OFF_STRIDE");
    REQUIRE(f != nullptr);
    CHECK(f->field == "row 119 col 1");
    CHECK(f->expected.find("row 120") != std::string::npos);   // nearest legal 23+97n
}

TEST_CASE("variant-csv: on-grid block past an empty name row -> VARC_UNREACHABLE_BLOCK") {
    // Block 0 and block 2 named, block 1's name cell EMPTIED: engine stops at
    // row 120, so the block-2 name at row 217 is unreachable.
    std::string csv = makeChassisCsv({ { "A", {} }, { "B", {} }, { "C", {} } });
    VariantCsvTable t = parseVariantCsv(csv);
    t.rows[119] = { "VariantName", "" };                  // row 120 name emptied
    auto fs = validateVariantCsvAdd(t, "atlas.csv", "m", nullptr);
    // BOTH the emptied on-grid row 120 and the shadowed block-2 row 217 are
    // reported unreachable; the row-217 one must cite the stop row and name.
    const ModderFinding* f217 = nullptr;
    for (const auto& f : fs)
        if (f.reason == "VARC_UNREACHABLE_BLOCK" && f.field == "row 217 col 2") f217 = &f;
    REQUIRE(f217 != nullptr);
    CHECK(f217->expected.find("row 120") != std::string::npos);  // where the walk stopped
    CHECK(f217->actual.find("'C'") != std::string::npos);
}

TEST_CASE("variant-csv: duplicate variant name (case-insensitive) -> VARC_DUPLICATE_NAME with both rows") {
    std::string csv = makeChassisCsv({ { "Atlas Prime", {} }, { "ATLAS PRIME", {} } });
    auto fs = validateVariantCsvAdd(parseVariantCsv(csv), "atlas.csv", "m", nullptr);
    const ModderFinding* f = findReason(fs, "VARC_DUPLICATE_NAME");
    REQUIRE(f != nullptr);
    CHECK(f->field == "row 120 col 2");
    CHECK(f->actual.find("row 23") != std::string::npos);
}

TEST_CASE("variant-csv: non-numeric CompBas# -> VARC_COMPONENT_ID_MALFORMED with exact row/col") {
    std::string csv = makeChassisCsv({ { "A", { {2, "banana"} } } });
    auto fs = validateVariantCsvAdd(parseVariantCsv(csv), "atlas.csv", "m", nullptr);
    const ModderFinding* f = findReason(fs, "VARC_COMPONENT_ID_MALFORMED");
    REQUIRE(f != nullptr);
    CHECK(f->field == "row 28 col 5");    // item index 2 -> row 26+2
    CHECK(f->actual == "'banana'");
    CHECK(f->id == "A");
}

TEST_CASE("variant-csv: unresolvable / out-of-range ids -> VARC_COMPONENT_ID_UNKNOWN; 255 sentinel is fine") {
    std::set<int> ids = parseCompbasPopulatedIds(kCompbas);
    SUBCASE("id not populated in compbas (free ordinal 39)") {
        std::string csv = makeChassisCsv({ { "A", { {0, "39"} } } });
        auto fs = validateVariantCsvAdd(parseVariantCsv(csv), "atlas.csv", "m", &ids);
        const ModderFinding* f = findReason(fs, "VARC_COMPONENT_ID_UNKNOWN");
        REQUIRE(f != nullptr);
        CHECK(f->field == "row 26 col 5");
        CHECK(f->actual == "39");
    }
    SUBCASE("id beyond 254 is out of range even without compbas") {
        std::string csv = makeChassisCsv({ { "A", { {0, "300"} } } });
        auto fs = validateVariantCsvAdd(parseVariantCsv(csv), "atlas.csv", "m", nullptr);
        CHECK(hasReason(fs, "VARC_COMPONENT_ID_UNKNOWN"));
    }
    SUBCASE("255 (0xff empty-slot sentinel) never flagged") {
        std::string csv = makeChassisCsv({ { "A", { {0, "255"} } } });
        auto fs = validateVariantCsvAdd(parseVariantCsv(csv), "atlas.csv", "m", &ids);
        CHECK(fs.empty());
    }
    SUBCASE("no compbas provided -> in-range unresolved ids pass (check skipped)") {
        std::string csv = makeChassisCsv({ { "A", { {0, "39"} } } });
        auto fs = validateVariantCsvAdd(parseVariantCsv(csv), "atlas.csv", "m", nullptr);
        CHECK(fs.empty());
    }
}

TEST_CASE("variant-csv: block cut off before its armor row -> VARC_BLOCK_TRUNCATED") {
    // Two blocks, file truncated at row 200 (< block 1's armor row 212).
    std::string csv = makeChassisCsv({ { "A", {} }, { "B", {} } }, 200);
    auto fs = validateVariantCsvAdd(parseVariantCsv(csv), "atlas.csv", "m", nullptr);
    const ModderFinding* f = findReason(fs, "VARC_BLOCK_TRUNCATED");
    REQUIRE(f != nullptr);
    CHECK(f->id == "B");
    CHECK(f->field.find("row 212") != std::string::npos);  // 115 + 97
    CHECK(f->actual.find("200") != std::string::npos);
}

TEST_CASE("variant-csv: formatter matches sibling shape (OK line / findings block)") {
    std::vector<ModderFinding> none;
    CHECK(formatVariantCsvFindings("atlas.csv", none, "modid")
          == "chassis-csv atlas.csv [modid]: OK (0 findings)\n");
    ModderFinding f;
    f.reason = "VARC_NO_VARIANT_BLOCK";
    f.message = "boom";
    std::string out = formatVariantCsvFindings("atlas.csv", { f });
    CHECK(out.find("1 finding(s)") != std::string::npos);
    CHECK(out.find("[VARC_NO_VARIANT_BLOCK] boom") != std::string::npos);
}
