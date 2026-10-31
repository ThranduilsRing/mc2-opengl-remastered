#include "save_field_probe.h"
#include "abl_value_archive.h"

TEST_CASE("save fields: ABL variable independent census") {
    using T = mc2::save::AblVariableRecord;
    T scalar{0x7fc12345u, {}}, array{0, {0x13, 0x24, 0x35}};
    SAVE_FIELD_SHAPE(scalar, bits, elements);
    for (auto sample : {scalar, array}) {
        CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, &T::bits, &T::elements));
        // Negative controls: either field disappearing must break the census.
    }
    CHECK_FALSE(save_probe::roundtrip(scalar, [](auto& a, auto& v) { a(v.elements); }, &T::bits, &T::elements));
    CHECK_FALSE(save_probe::roundtrip(array, [](auto& a, auto& v) { a(v.bits); }, &T::bits, &T::elements));
    auto bytes = mc2::save::encodeTaggedRecord(array, mc2::save::tagAblVariable);
    T decoded;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::tagAblVariable));
    CHECK(decoded.elements == array.elements);
    bytes.pop_back();
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::tagAblVariable));
    CHECK(decoded.elements == array.elements);
    CHECK(mc2::save::encodeTaggedRecord(T{1, {2}}, mc2::save::tagAblVariable).empty());
}

TEST_CASE("ABL value restore rebuilds addresses and discards scalar pointer bytes") {
    // Same union arms as native StackItem, independently exercised without VM globals.
    union Slot { int integer; float real; unsigned char byte; char* address; };
    Slot source, target;
    std::memset(&source, 0xab, sizeof(source));
    source.integer = 0x12345678;
    const auto value = mc2::save::captureAblVariable(source, 0);
    REQUIRE(mc2::save::restoreAblVariable(value, target, 0));
    CHECK(target.integer == source.integer);
    for (std::size_t i = 4; i < sizeof(target); ++i)
        CHECK(reinterpret_cast<unsigned char*>(&target)[i] == 0);
    char from[]{1, 2, 3}, to[]{0, 0, 0};
    source.address = from;
    target.address = to;
    const auto array = mc2::save::captureAblVariable(source, sizeof(from));
    REQUIRE(mc2::save::restoreAblVariable(array, target, sizeof(to)));
    CHECK(target.address == to);
    CHECK(std::memcmp(from, to, sizeof(to)) == 0);
    CHECK_FALSE(mc2::save::restoreAblVariable(array, target, sizeof(to) - 1));
}
