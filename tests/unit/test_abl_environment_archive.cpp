#include "save_field_probe.h"
#include "abl_environment_archive.h"

TEST_CASE("save fields: ABL environment independent census") {
    using T = mc2::save::AblEnvironmentRecord;
    T sample{1, 3, 7, {"library.abl", "mission.abl"}};
    SAVE_FIELD_SHAPE(sample, libraries, modules, eternals, files);
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); },
        &T::libraries, &T::modules, &T::eternals, &T::files));
    auto bytes = mc2::save::encodeTaggedRecord(sample, mc2::save::tagAblEnvironment);
    T decoded;
    REQUIRE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::tagAblEnvironment));
    CHECK(decoded.files == sample.files);
    bytes[4] = 2;
    CHECK_FALSE(mc2::save::decodeTaggedRecord(bytes, decoded, mc2::save::tagAblEnvironment));
    CHECK(decoded.files == sample.files);
    sample.libraries = 3;
    CHECK(mc2::save::encodeTaggedRecord(sample, 0).empty());
    sample = {0, 0, 0, {std::string("bad\0name", 8)}};
    CHECK(mc2::save::encodeTaggedRecord(sample, 0).empty());
}
