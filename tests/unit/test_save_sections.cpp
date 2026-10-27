#include "doctest.h"
#include "save_sections.h"

#include <string>

namespace {
using namespace mc2::save;

// Shared with tools/test_save_inspect.py: the Python --upgrade writer must produce these exact bytes.
constexpr const char* goldenHex =
    "4d43325345435453" "02000000" "00000000" "00000000" "02000000"
    "414e494d" "03000000" "01000000" "00000000" "01000000" "aa"
    "4f524451" "01000000" "01000000" "00000000" "02000000" "0102";

std::string hex(const Bytes& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (auto b : bytes) { out += digits[b >> 4]; out += digits[b & 15]; }
    return out;
}

SectionContainer sample() {
    SectionContainer c;
    c.add(tagOrders, 1, true, {1, 2});        // added out of tag order: the writer sorts
    c.add(tagAnimation, 3, true, {0xaa});
    return c;
}

Bytes encoded(const SectionContainer& c) {
    Bytes out;
    REQUIRE(encodeSections(c, out));
    return out;
}

Bytes r2Packet(std::uint32_t version, const Bytes& animation, const Bytes& queue) {
    Archive a;
    std::uint32_t tag = legacyR2Tag, length = static_cast<std::uint32_t>(animation.size());
    a(tag, version, length);
    Bytes anim = animation, rest = queue;
    a.raw(anim);
    a.raw(rest);
    return a.bytes();
}
} // namespace

TEST_CASE("save sections: golden wire bytes, sorted and byte-stable") {
    const auto bytes = encoded(sample());
    CHECK(hex(bytes) == goldenHex);
    CHECK(encoded(sample()) == bytes);
    SectionContainer back;
    REQUIRE(decodeSections(bytes, back));
    REQUIRE(back.sections.size() == 2);
    CHECK(back.sections[0].tag == tagAnimation);
    CHECK(back.find(tagOrders)->payload == Bytes{1, 2});
    CHECK(encoded(back) == bytes);
}

TEST_CASE("save sections: rejects malformed, newer and unknown required sections") {
    std::string why;
    SectionContainer out;
    auto bytes = encoded(sample());
    for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
        Bytes truncated(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(decodeSections(truncated, out));
    }
    auto trailing = bytes; trailing.push_back(0);
    CHECK_FALSE(decodeSections(trailing, out, &why)); CHECK(why == "trailing_bytes");
    auto magic = bytes; magic[0] ^= 1;
    CHECK_FALSE(decodeSections(magic, out, &why)); CHECK(why == "section_magic");

    SectionContainer newer; newer.add(tagAnimation, 4, true, {});
    CHECK_FALSE(decodeSections(encoded(newer), out, &why)); CHECK(why == "section_version");

    SectionContainer unknownRequired; unknownRequired.add(fourcc('Z', 'Z', 'Z', 'Z'), 1, true, {7});
    CHECK_FALSE(decodeSections(encoded(unknownRequired), out, &why)); CHECK(why == "unknown_required_section");

    SectionContainer unknownOptional = sample(); unknownOptional.add(tagBrain, 1, false, {7});
    REQUIRE(decodeSections(encoded(unknownOptional), out));
    CHECK(out == SectionContainer{{{tagAnimation, 3, 1, {0xaa}}, {tagOrders, 1, 1, {1, 2}}}});

    SectionContainer duplicate = sample(); duplicate.add(tagOrders, 1, true, {});
    Bytes ignored;
    CHECK_FALSE(encodeSections(duplicate, ignored, &why)); CHECK(why == "duplicate_section");
    // An unsorted container on the wire: swap the two section records of the golden bytes.
    const std::size_t head = 8 + 16, first = 20 + 1;
    Bytes swapped(bytes.begin(), bytes.begin() + head);
    swapped.insert(swapped.end(), bytes.begin() + head + first, bytes.end());
    swapped.insert(swapped.end(), bytes.begin() + head, bytes.begin() + head + first);
    CHECK_FALSE(decodeSections(swapped, out, &why)); CHECK(why == "section_order");
}

TEST_CASE("save sections: a v1 R2 packet upgrades to ANIM + ORDQ; no packet means defaults") {
    SectionContainer out;
    REQUIRE(upgradeLegacyR2(r2Packet(3, {0xaa}, {1, 2}), out));
    CHECK(out == SectionContainer{{{tagAnimation, 3, 1, {0xaa}}, {tagOrders, 1, 1, {1, 2}}}});
    CHECK(hex(encoded(out)) == goldenHex);
    REQUIRE(upgradeLegacyR2(r2Packet(2, {}, {}), out));
    CHECK(out.find(tagAnimation)->version == 2);
    CHECK(out.find(tagOrders)->payload.empty());
    REQUIRE(upgradeLegacyR2({}, out));
    CHECK(out.sections.empty());
    std::string why;
    CHECK_FALSE(upgradeLegacyR2(r2Packet(4, {}, {}), out, &why)); CHECK(why == "r2_version");
    auto badTag = r2Packet(3, {}, {}); badTag[0] ^= 1;
    CHECK_FALSE(upgradeLegacyR2(badTag, out, &why)); CHECK(why == "r2_tag");
    auto shortBlob = r2Packet(3, {1, 2, 3}, {}); shortBlob.pop_back();
    CHECK_FALSE(upgradeLegacyR2(shortBlob, out));
}
