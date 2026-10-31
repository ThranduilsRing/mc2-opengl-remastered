#include "save_field_probe.h"
#include "watch_list_save_data.h"
#include "saved_objects_check.h"

TEST_CASE("save fields: watch list independent census") {
    using T = mc2::save::SavedWatchList;
    auto sample = save_probe::patterned<T>(&T::indices);
    SAVE_FIELD_SHAPE(sample, indices);
    CHECK(save_probe::memberBytes<T>(&T::indices) == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, &T::indices));
    sample.indices = {0, -1, INT32_MIN, INT32_MAX, 7};
    auto bytes = mc2::save::encodeTaggedRecord(sample, mc2::save::watchListTag);
    CHECK(bytes == mc2::save::Bytes{'W','A','T','C',1,0,0,0,5,0,0,0,
        0,0,0,0,255,255,255,255,0,0,0,128,255,255,255,127,7,0,0,0});
    T decoded;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::watchListTag));
    CHECK(decoded.indices == sample.indices);
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::tagObjectManager));
    bytes.pop_back();
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::watchListTag));
    CHECK(decoded.indices == sample.indices);
    sample.indices.resize(mc2::save::maxSequenceLength);
    for (std::size_t i = 0; i < sample.indices.size(); ++i) sample.indices[i] = i;
    bytes = mc2::save::encodeTaggedRecord(sample, mc2::save::watchListTag);
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::watchListTag));
    CHECK(decoded.indices == sample.indices);
    ++bytes[10]; // Oversized count without allocating its claimed elements.
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::watchListTag));
    sample.indices.push_back(1);
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::watchListTag).empty());
}

TEST_CASE("saved objects: tagged watch size leaves object dispatch unchanged") {
    using namespace mc2_saved_objects;
    HeaderCounts h{};
    h.byClass[kMech] = 1;
    RecordSizes r{};
    r.byClass[kMech] = 3992;
    r.pureObject = 372; r.pureMover = 3288;
    std::array<std::uint32_t, 4> packets{68, 0, 3992, 20};
    auto sizeAt = [&](auto i) { return packets[i]; };
    CHECK_FALSE(checkRun(h, r, 0, 4, sizeAt, true));
    CHECK(checkRun(h, r, 0, 4, sizeAt) == "bad_watch_table");
    packets[2] += 8;
    CHECK(checkRun(h, r, 0, 4, sizeAt, true) == "unknown_record_size");
}
