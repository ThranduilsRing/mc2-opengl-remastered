#include "doctest.h"
#include "save_sections.h"
#include "combat_clock_archive.h"
#include "save_field_probe.h"
#include "unit_anim_state.h"

#include <string>

TEST_CASE("save sections: animation legacy integers use the Windows wire width") {
    using namespace mc2::save;
    mc2::sim::UnitAnimState state;
    state.currentStateGoal = -1;
    state.currentGestureId = 0x12345678;
    state.nodeUsed = {-1, 0x12345678};
    Archive output;
    output(state);
    REQUIRE(output.good());
    // Five state/gesture words follow float + eleven bools + velocity.
    REQUIRE(output.bytes().size() >= 27);
    CHECK(output.bytes()[19] == 0xff);
    CHECK(output.bytes()[22] == 0xff);
    CHECK(output.bytes()[23] == 0x78);
    CHECK(output.bytes()[26] == 0x12);
    auto empty = state;
    empty.nodeUsed.clear();
    Archive baseline;
    baseline(empty);
    CHECK(output.bytes().size() == baseline.bytes().size() + 8);
    mc2::sim::UnitAnimState restored;
    Archive input(output.bytes());
    input(restored);
    REQUIRE(input.finish());
    CHECK(restored.currentStateGoal == -1);
    CHECK(restored.currentGestureId == 0x12345678);
    CHECK(restored.nodeUsed == state.nodeUsed);
    std::uint64_t tick = 0x123456789abcdef0ull;
    Archive wide;
    wide(tick);
    CHECK(wide.bytes().size() == 8);
}

TEST_CASE("save sections: WRLD requires complete unique classified mover membership") {
    using mc2::save::SavedMoverLists;
    const SavedMoverLists rebuilt{{}, {}, {2, 4, 6, 8}, {2}, {4, 6}}; // 8 is neutral.
    auto saved = rebuilt;
    saved.movers = {8, 6, 4, 2};
    saved.hostile = {6, 4};
    CHECK(saved.sameMoverMembership(rebuilt));
    SUBCASE("duplicate mover") { saved.movers = {2, 4, 6, 6}; }
    SUBCASE("missing mover") { saved.movers.pop_back(); }
    SUBCASE("extra mover") { saved.movers.push_back(10); }
    SUBCASE("duplicate friendly") { saved.friendly.push_back(2); }
    SUBCASE("duplicate hostile") { saved.hostile.push_back(4); }
    SUBCASE("overlapping sides") { saved.friendly.push_back(4); }
    SUBCASE("missing hostile") { saved.hostile.pop_back(); }
    SUBCASE("wrong side") { saved.friendly = {4}; saved.hostile = {2, 6}; }
    SUBCASE("neutral assigned a side") { saved.friendly.push_back(8); }
    CHECK_FALSE(saved.sameMoverMembership(rebuilt));
}

TEST_CASE("save fields: compacted mover pools and registration order round trip") {
    using namespace mc2::save;
    const SavedMoverLists expected{{7, 2, 5}, {9, 6, 8}, {2, 9, 7}, {7}, {2, 9}, 1300};
    auto source = expected;
    SAVE_FIELD_SHAPE(source, mechs, vehicles, movers, friendly, hostile, turn);
    CHECK(save_probe::memberBytes<SavedMoverLists>(&SavedMoverLists::mechs, &SavedMoverLists::vehicles,
        &SavedMoverLists::movers, &SavedMoverLists::friendly, &SavedMoverLists::hostile, &SavedMoverLists::turn)
        > sizeof(SavedMoverLists) - alignof(SavedMoverLists));
    Archive output;
    source.serialize(output);
    REQUIRE(output.good());
    SavedMoverLists restored;
    Archive input(output.bytes());
    restored.serialize(input);
    REQUIRE(input.finish());
    CHECK(restored == expected);
    Archive legacyOutput;
    source.serialize(legacyOutput, 1);
    SavedMoverLists legacyRestored;
    Archive legacyInput(legacyOutput.bytes());
    legacyRestored.serialize(legacyInput, 1);
    REQUIRE(legacyInput.finish());
    auto legacyExpected = expected;
    legacyExpected.turn = 0;
    CHECK(legacyRestored == legacyExpected);
    auto truncated = output.bytes();
    truncated.pop_back();
    Archive bad(truncated);
    restored.serialize(bad);
    CHECK_FALSE(bad.finish());
}

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
    SavedSection shape;
    SAVE_FIELD_SHAPE(shape, tag, version, flags, payload);
    CHECK(save_probe::memberBytes<SavedSection>(&SavedSection::tag, &SavedSection::version,
        &SavedSection::flags, &SavedSection::payload) + 4 == sizeof(SavedSection));
    SectionContainer containerShape;
    SAVE_FIELD_SHAPE(containerShape, sections);
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

TEST_CASE("save sections: weapon nodes beyond the legacy cap round trip through ANIM") {
    mc2::sim::UnitAnimState state;
    for (long i = 0; i < 32; ++i) {
        state.nodeUsed.push_back(i * 7 + 1);
        state.nodeRecycle.push_back(static_cast<float>(i) * 0.25f);
    }
    Archive write;
    write(state);
    REQUIRE(write.good());
    SectionContainer container;
    container.add(tagAnimation, 4, true, write.bytes());
    container.add(tagWeaponNodes, 1, true, {});
    SectionContainer back;
    REQUIRE(decodeSections(encoded(container), back));
    REQUIRE(back.find(tagWeaponNodes));
    CHECK(back.find(tagWeaponNodes)->flags == sectionRequired);
    REQUIRE(back.find(tagAnimation));
    mc2::sim::UnitAnimState restored;
    Archive read(back.find(tagAnimation)->payload);
    read(restored);
    REQUIRE(read.finish());
    CHECK(restored.nodeUsed == state.nodeUsed);
    CHECK(restored.nodeRecycle == state.nodeRecycle);
    Archive canonical;
    canonical(restored);
    CHECK(canonical.bytes() == write.bytes());
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

    SectionContainer newer; newer.add(tagAnimation, 5, true, {});
    CHECK_FALSE(decodeSections(encoded(newer), out, &why)); CHECK(why == "section_version");

    SectionContainer terrain; terrain.add(tagObjectSlots, objectSlotsVersion, true, {});
    REQUIRE(decodeSections(encoded(terrain), out, &why));
    CHECK(out.find(tagObjectSlots)->version == 2);
    SectionContainer futureTerrain; futureTerrain.add(tagObjectSlots, objectSlotsVersion + 1, true, {});
    CHECK_FALSE(decodeSections(encoded(futureTerrain), out, &why)); CHECK(why == "section_version");

    SectionContainer unknownRequired; unknownRequired.add(fourcc('Z', 'Z', 'Z', 'Z'), 1, true, {7});
    CHECK_FALSE(decodeSections(encoded(unknownRequired), out, &why)); CHECK(why == "unknown_required_section");

    SectionContainer unknownOptional = sample(); unknownOptional.add(tagPath, 1, false, {7});  // PATH is reserved: unknown to this build
    REQUIRE(decodeSections(encoded(unknownOptional), out));
    CHECK(out == SectionContainer{{{tagAnimation, 3, 1, {0xaa}}, {tagOrders, 1, 1, {1, 2}}}});

    SectionContainer brain = sample(); brain.add(tagBrain, 1, false, {7});
    REQUIRE(decodeSections(encoded(brain), out));
    CHECK(out.find(tagBrain)->payload == Bytes{7});  // BRN1 is known: kept, not dropped
    SectionContainer brainNewer = sample(); brainNewer.add(tagBrain, 3, false, {7});
    CHECK_FALSE(decodeSections(encoded(brainNewer), out, &why)); CHECK(why == "section_version");

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

TEST_CASE("save sections: combat clock value bytes and malformed corpus") {
    CombatClockValue sample{0x0102030405060708ull, 0.5};
    Archive archive;
    archive(sample);
    REQUIRE(archive.good());
    CHECK(hex(archive.bytes()) == "0807060504030201000000000000e03f");
    for (const auto frac : {0.0, -0.0, std::numeric_limits<double>::denorm_min(),
                           0.875, std::nextafter(1.0, 0.0)}) {
        CombatClockValue value{mc2::combat::invalidTick - 1, frac};
        Archive writer;
        writer(value);
        REQUIRE(writer.good());
        CombatClockValue restored;
        REQUIRE(decodeRecord(writer.bytes(), restored));
        CHECK(restored.ticks == value.ticks);
        CHECK(std::bit_cast<std::uint64_t>(restored.frac) == std::bit_cast<std::uint64_t>(frac));
        mc2::combat::SavedCombatClock legacy{value.ticks, frac};
        REQUIRE(sizeof(legacy) == 16);
        CHECK(std::memcmp(writer.bytes().data(), &legacy, 16) == 0);
    }
    for (std::size_t size = 0; size < archive.bytes().size(); ++size) {
        CombatClockValue previous{7, 0.25};
        CHECK_FALSE(decodeRecord(std::span(archive.bytes()).first(size), previous));
        CHECK(previous.ticks == 7);
        CHECK(previous.frac == 0.25);
    }
    auto extra = archive.bytes();
    extra.push_back(0);
    CombatClockValue previous{7, 0.25};
    CHECK_FALSE(decodeRecord(extra, previous));
    for (const auto value : {CombatClockValue{mc2::combat::invalidTick, 0}, {0, -0.1}, {0, 1},
                            {0, std::numeric_limits<double>::infinity()},
                            {0, std::numeric_limits<double>::quiet_NaN()}}) {
        auto candidate = value;
        Archive writer;
        writer(candidate);
        CHECK_FALSE(writer.good());
        // Raw scalars bypass the writer validator to exercise hostile input too.
        Archive malformed;
        malformed(candidate.ticks, candidate.frac);
        CHECK_FALSE(decodeRecord(malformed.bytes(), previous));
        CHECK(previous.ticks == 7);
        CHECK(previous.frac == 0.25);
    }
}

TEST_CASE("save fields: clock omission catcher rejects a dropped member") {
    using T = CombatClockValue;
    auto sample = save_probe::patterned<T>(&T::ticks, &T::frac);
    SAVE_FIELD_SHAPE(sample, ticks, frac);
    sample.frac = std::bit_cast<double>(0x3fe2020202020202ull); // Valid fraction; every byte is nonzero.
    CHECK(save_probe::memberBytes<T>(&T::ticks, &T::frac) == sizeof(T));
    CHECK(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v); }, &T::ticks, &T::frac));
    CHECK_FALSE(save_probe::roundtrip(sample, [](auto& a, auto& v) { a(v.ticks); }, &T::ticks, &T::frac));
}

TEST_CASE("save sections: V2 clock and legacy clock preserve the packet cursor") {
    struct Packets {
        std::array<Bytes, 4> data;
        int cursor = 2;
        int getCurrentPacket() const { return cursor; }
        int getNumPackets() const { return static_cast<int>(data.size()); }
        int seekPacket(int index) { cursor = index; return 0; }
        int getPacketSize() const { return static_cast<int>(data[cursor].size()); }
        int readPacket(int index, unsigned char* output) {
            cursor = index;
            std::memcpy(output, data[index].data(), data[index].size());
            return getPacketSize();
        }
    } file;
    mc2::combat::SavedCombatClock legacy{1234, 0.875};
    const auto* native = reinterpret_cast<const std::uint8_t*>(&legacy);
    file.data[3].assign(native, native + sizeof(legacy));
    // Pre-V2 stamp takes the unchanged native reader.
    file.data[0] = {'o', 'l', 'd', 0};
    auto restored = readSavedCombatClock(file, false);
    REQUIRE(restored);
    CHECK(restored->ticks == legacy.ticks);
    CHECK(restored->frac == legacy.frac);
    CHECK(file.cursor == 2);
    file.data[0].assign(saveStampV2, saveStampV2 + sizeof(saveStampV2));
    restored = readSavedCombatClock(file, true);
    REQUIRE(restored);
    CHECK(restored->ticks == legacy.ticks);
    CHECK(restored->frac == legacy.frac);
    CHECK(file.cursor == 2);
    file.data[3].back() = 0x7f; // non-finite/out-of-range binary64
    CHECK_FALSE(readSavedCombatClock(file, true));
    CHECK(file.cursor == 2);
    file.data[3].resize(8);
    CHECK_FALSE(readSavedCombatClock(file, true));
    CHECK(file.cursor == 2);
}

TEST_CASE("save parts: an explicit PartsFormat tag makes legacy detection unambiguous") {
    using mc2::save::partsAreShiftedLegacy;
    // New save whose Part1 is an omitted alternative (ObjectNumber 0, WID 0): the tag keeps it non-legacy, so WID/SquadId/Variant load.
    CHECK_FALSE(partsAreShiftedLegacy(true, true, 0, 0));
    CHECK_FALSE(partsAreShiftedLegacy(true, true, 41, 7));
    CHECK_FALSE(partsAreShiftedLegacy(true, false, 0, 0));
    // Untagged legacy save: blank Part1 (no WID key, or WID 0 with ObjectNumber 0) means every part is shifted.
    CHECK(partsAreShiftedLegacy(false, false, 0, 0));
    CHECK(partsAreShiftedLegacy(false, true, 0, 0));
    // Untagged save from the Parts 1..N era with a real Part1 still loads its WIDs.
    CHECK_FALSE(partsAreShiftedLegacy(false, true, 41, 7));
    CHECK_FALSE(partsAreShiftedLegacy(false, true, 0, 7));
    CHECK(mc2::save::kPartsFormatCurrent == 2);
}

#include "save_fix_section.h"

TEST_CASE("save sections: FIX1 round trips music, jump backoff and ordered path requests") {
    using namespace mc2::save;
    FixSection in;
    in.music = {7, 12.5, false};
    in.backoffs = {{3, 40, 52, 11}, {9, -1, 0, 0}};
    in.paths = {{5, 4, 2, 0x11u}, {3, 0, -1, 0u}, {5, 49, 7, 0xFFFFFFFFu}};
    const Bytes bytes = encodeFixSection(in);
    REQUIRE_FALSE(bytes.empty());
    const auto out = decodeFixSection(bytes);
    REQUIRE(out.has_value());
    CHECK(*out == in);
    CHECK(out->valid(10));
    CHECK_FALSE(out->valid(9));  // warrior index 9 out of range
    for (std::size_t cut = 0; cut < bytes.size(); ++cut)
        CHECK_FALSE(decodeFixSection(std::span(bytes).first(cut)).has_value());
    Bytes trailing = bytes;
    trailing.push_back(0);
    CHECK_FALSE(decodeFixSection(trailing).has_value());
    FixSection badSource = in;
    badSource.paths[0].source = fixMaxPathSource;
    CHECK_FALSE(badSource.valid(10));
}

TEST_CASE("save sections: FIX1 validation refuses non-finite music time, out-of-table ids and bad versions") {
    using namespace mc2::save;
    FixSection ok;
    ok.music = {3, 5.0, false};
    ok.backoffs = {{1, 2, 3, 4}};
    CHECK(ok.valid(10, 8));
    FixSection nan = ok;
    nan.music.remaining = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(nan.valid(10, 8));
    FixSection inf = ok;
    inf.music.remaining = std::numeric_limits<double>::infinity();
    CHECK_FALSE(inf.valid(10, 8));
    FixSection high = ok;
    high.music.id = 8;  // table has ids 0..7
    CHECK_FALSE(high.valid(10, 8));
    CHECK(high.valid(10, 9));
    FixSection silent = ok;
    silent.music.id = -1;
    CHECK(silent.valid(10, 8));
    // The one-step helper the loader uses: any failure is a single nullopt (one reject line, defaults kept).
    const Bytes good = encodeFixSection(ok);
    CHECK(decodeValidFixSection(fixSectionVersion, good, 10, 8).has_value());
    CHECK_FALSE(decodeValidFixSection(fixSectionVersion + 1, good, 10, 8).has_value());
    CHECK_FALSE(decodeValidFixSection(fixSectionVersion, encodeFixSection(nan), 10, 8).has_value());
    CHECK_FALSE(decodeValidFixSection(fixSectionVersion, std::span(good).first(good.size() - 1), 10, 8).has_value());
}

TEST_CASE("save sections: FIX1 is an optional known section a container carries") {
    using namespace mc2::save;
    SectionContainer c;
    c.add(tagFix, fixSectionVersion, false, encodeFixSection(FixSection{}));
    Bytes wire;
    REQUIRE(encodeSections(c, wire));
    SectionContainer back;
    REQUIRE(decodeSections(wire, back));
    REQUIRE(back.find(tagFix) != nullptr);
    const auto out = decodeFixSection(back.find(tagFix)->payload);
    REQUIRE(out.has_value());
    CHECK(*out == FixSection{});
}
