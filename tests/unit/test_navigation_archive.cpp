#include "save_field_probe.h"
#include "move.h"

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
