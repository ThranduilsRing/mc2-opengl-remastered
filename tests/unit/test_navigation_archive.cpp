#include "save_field_probe.h"
#include "move.h"

#define AREA_MEMBERS &T::sectorR, &T::sectorC, &T::doors_Legacy32bitPtr, &T::type, \
    &T::numDoors, &T::ownerWID, &T::teamID, &T::offMap, &T::open, &T::cellsCovered_Legacy32bitPtr

TEST_CASE("save fields: map areas independent census") {
    using T = GlobalMapArea;
    auto sample = save_probe::patterned<T>(AREA_MEMBERS);
    SAVE_FIELD_SHAPE(sample, sectorR, sectorC, doors, type, numDoors, ownerWID, teamID, offMap, open, cells);
    CHECK(save_probe::memberBytes<T>(AREA_MEMBERS) == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, AREA_MEMBERS));
    std::array<T, 1> values{sample}, decoded{};
    auto bytes = mc2::save::encodeNavigationRecord(std::span(values), mc2::save::tagMapAreas);
    CHECK(bytes.size() == 12 + 25);
    REQUIRE(mc2::save::decodeNavigationRecord(bytes, std::span(decoded), mc2::save::tagMapAreas));
    CHECK(std::memcmp(values.data(), decoded.data(), sizeof(T)) == 0);
}

TEST_CASE("save fields: map cells independent census") {
    using T = MapCell;
    auto sample = save_probe::patterned<T>(&T::data);
    SAVE_FIELD_SHAPE(sample, data);
    CHECK(save_probe::memberBytes<T>(&T::data) == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, &T::data));
    std::vector<T> cells(70000, sample); // Real grids exceed the ordinary sequence cap.
    auto bytes = mc2::save::encodeNavigationRecord(std::span(cells), mc2::save::tagMapCells);
    CHECK(bytes.size() == 12 + 4 * cells.size());
    std::vector<T> decoded(cells.size());
    REQUIRE(mc2::save::decodeNavigationRecord(bytes, std::span(decoded), mc2::save::tagMapCells));
    CHECK(decoded.back().data == sample.data);
    bytes.pop_back();
    CHECK_FALSE(mc2::save::decodeNavigationRecord(bytes, std::span(decoded), mc2::save::tagMapCells));
    CHECK(decoded.back().data == sample.data);
    bytes.push_back(1);
    ++bytes[8];
    CHECK_FALSE(mc2::save::decodeNavigationRecord(bytes, std::span(decoded), mc2::save::tagMapCells));
}
