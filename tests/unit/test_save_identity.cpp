#include "doctest.h"
#include "abl_value_archive.h"
#include "save_identity.h"

namespace {
const std::string kMissionA = "st MissionName = \"data\\missions\\mc2_01\"\r\nst ScenarioScript = \"a\"\r\n";
const std::string kCampaign = "[General]\r\nst ActiveMod = \"-\"\r\nst ModDeps = \"-\"\r\n";
}

TEST_CASE("quickload identity: a save from another mission or mod is refused before teardown") {
    using namespace mc2::save;
    CHECK(fitStringValue(kMissionA, "MissionName") == "data\\missions\\mc2_01");
    CHECK_FALSE(fitStringValue(kMissionA, "Missing").has_value());
    CHECK_FALSE(fitStringValue("st MissionName = \"unterminated", "MissionName").has_value());
    // quick-save on mission A, quick-load while on mission B: refused; on A: allowed.
    CHECK(savedIdentityMatches(kMissionA, "data\\missions\\MC2_01", kCampaign, "-", "-"));
    CHECK_FALSE(savedIdentityMatches(kMissionA, "data\\missions\\mc2_10", kCampaign, "-", "-"));
    CHECK_FALSE(savedIdentityMatches(kMissionA, "data\\missions\\mc2_01", kCampaign, "somemod", "-"));
    CHECK_FALSE(savedIdentityMatches(kMissionA, "data\\missions\\mc2_01", kCampaign, "-", "changed-dependency"));
    CHECK(savedIdentityMatches(kMissionA, "data\\missions\\mc2_01",
        "st ActiveMod = \"mod\"\nst ModDeps = \"base,extra\"\n", "MOD", "BASE,EXTRA"));
    // an older save without the keys, or an unreadable packet, is not refused on identity alone.
    CHECK(savedIdentityMatches("", "mc2_10", "", "somemod", "-"));
}

TEST_CASE("ABL value reader: record sizes are validated against the remaining stream") {
    using namespace mc2::save;
    CHECK(ablRecordSizeValid(8, 8));
    CHECK(ablRecordSizeValid(8, -1));
    CHECK_FALSE(ablRecordSizeValid(7, 100));
    CHECK_FALSE(ablRecordSizeValid(-1, 100));
    CHECK_FALSE(ablRecordSizeValid(9, 8));
    CHECK_FALSE(ablRecordSizeValid(static_cast<std::int64_t>(maxFileBytes) + 1, -1));
    CHECK_FALSE(ablRecordSizeValid(0x7fffffff, -1));
}

TEST_CASE("ABL value reader: short and damaged variable records fail without throwing") {
    using namespace mc2::save;
    AblVariableRecord sample;
    sample.elements.assign(40, 0x5a);
    const auto bytes = encodeTaggedRecord(sample, tagAblVariable);
    AblVariableRecord out;
    REQUIRE(decodeTaggedRecord(bytes, out, tagAblVariable));
    // every truncation of a valid record
    for (size_t n = 0; n < bytes.size(); ++n) {
        Bytes cut(bytes.begin(), bytes.begin() + n);
        AblVariableRecord v;
        CHECK_NOTHROW(CHECK_FALSE(decodeTaggedRecord(cut, v, tagAblVariable)));
    }
    // every single-byte corruption, including a huge element count: no throw, no giant allocation
    for (size_t i = 0; i < bytes.size(); ++i)
        for (std::uint8_t b : {std::uint8_t{0}, std::uint8_t{0xff}, std::uint8_t{0x7f}}) {
            Bytes bad = bytes;
            bad[i] = b;
            AblVariableRecord v;
            CHECK_NOTHROW(decodeTaggedRecord(bad, v, tagAblVariable));
        }
}
