#include "save_field_probe.h"
#include "abl_module_archive.h"

TEST_CASE("save fields: ABL module independent census") {
    using T = mc2::save::AblModuleRecord;
    auto sample = save_probe::patterned<T>(&T::name, &T::handle, &T::previousState,
        &T::state, &T::initCalled, &T::statics, &T::orderFlags);
    sample.statics = 23;
    SAVE_FIELD_SHAPE(sample, name, handle, previousState, state, initCalled, statics, orderFlags);
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, &T::name,
        &T::handle, &T::previousState, &T::state, &T::initCalled, &T::statics, &T::orderFlags));
    auto bytes = mc2::save::encodeTaggedRecord(sample, mc2::save::tagAblModule);
    T decoded;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::tagAblModule));
    CHECK(decoded.orderFlags == sample.orderFlags);
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::tagTeam));
    bytes.push_back(0);
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::tagAblModule));
    sample.handle = -1;
    CHECK(mc2::save::encodeTaggedRecord(sample, mc2::save::tagAblModule).empty());
}
