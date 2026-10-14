#include "doctest.h"
#include "save_archive.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>

namespace {
using namespace mc2::save;

struct MissionParts {
    std::vector<Part> parts;
    void serialize(Archive& a) { a(parts); }
    bool operator==(const MissionParts&) const = default;
};

struct TaggedValue {
    std::optional<std::int32_t> optional;
    std::variant<std::int32_t, Text, double> value;
    void serialize(Archive& a) { a(optional, value); }
};

Part samplePart() {
    Part part;
    part.objectWID = -1;
    part.objNumber = 0x01020304;
    part.baseColor = 0xaabbccdd;
    part.highlightColor1 = 7;
    part.highlightColor2 = 8;
    part.active = -2;
    part.exists = 1;
    part.destroyed = true;
    part.position = {-0.0f, 12.5f, -7.25f};
    part.velocity = 3.5f;
    part.rotation = -2.75f;
    part.gestureId = 19;
    part.alignment = -1;
    part.teamId = 7;
    part.commanderID = -1;
    part.squadId = 31;
    part.myIcon = -1;
    part.controlType = 2;
    part.controlDataType = 3;
    part.profileName = {2, "profile"};
    part.pilot = 42;
    part.captureable = true;
    part.variantNum = 6;
    return part;
}

Document mission() {
    Document doc;
    doc.header.releaseLine.bytes = "development";
    doc.header.originBuildSha.bytes = std::string(40, 'a');
    doc.header.dataFingerprint[0] = 7;
    doc.header.simRulesFingerprint[31] = 9;
    Archive record;
    auto part = samplePart();
    record(part);
    for (std::uint32_t tag = 1; tag <= 15; ++tag)
        doc.sections.push_back({tag, tag != 12, tag == 10 ? record.bytes() : Bytes{0}});
    return doc;
}

Compatibility compatibility(const Document& doc) {
    return {doc.header.releaseLine.bytes, doc.header.dataFingerprint, doc.header.simRulesFingerprint};
}

std::string hex(const Digest& digest) {
    std::string result;
    for (auto byte : digest) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}
} // namespace

TEST_SUITE("SaveArchive") {
TEST_CASE("SHA256 known wire digests and block boundaries") {
    CHECK(hex(sha256({})) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex(sha256(Bytes{'a','b','c'})) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string longText = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(hex(sha256(Bytes(longText.begin(), longText.end()))) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hex(sha256(Bytes(1000000, 'a'))) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("Part fixed field order is independent of native layout") {
    auto part = samplePart();
    Archive writer;
    writer(part);
    REQUIRE(writer.finish());
    const Bytes golden{
        255,255,255,255, 4,3,2,1, 221,204,187,170, 7,0,0,0, 8,0,0,0,
        254,255,255,255, 1,0,0,0, 1,
        0,0,0,128, 0,0,72,65, 0,0,232,192, 0,0,96,64, 0,0,48,192,
        19,0,0,0, 255,7, 255,255,255,255, 31,0,0,0, 255,
        2,0,0,0, 3,0,0,0, 2,7,0,0,0, 'p','r','o','f','i','l','e',
        42,0,0,0, 1, 6,0,0,0};
    CHECK(writer.bytes() == golden);
    Part decoded;
    REQUIRE(decodeRecord(golden, decoded));
    CHECK(decoded == part);
    CHECK(std::bit_cast<std::uint32_t>(decoded.position.x) == 0x80000000);
}

TEST_CASE("nested live list and text bytes round trip without reordering") {
    MissionParts original{{samplePart(), samplePart()}};
    original.parts[1].profileName.bytes = "second";
    original.parts[1].pilot = 17;
    Archive writer;
    writer(original);
    REQUIRE(writer.finish());
    MissionParts restored;
    REQUIRE(decodeRecord(writer.bytes(), restored));
    CHECK(restored == original);
    Archive resave;
    resave(restored);
    CHECK(resave.bytes() == writer.bytes());
    MissionParts empty;
    Archive emptyWriter;
    emptyWriter(empty);
    CHECK(emptyWriter.bytes() == Bytes{0,0,0,0});
    REQUIRE(decodeRecord(emptyWriter.bytes(), restored));
    CHECK(restored.parts.empty());
    Text multiline{1, "one\ntwo\r\n"};
    Archive textWriter;
    textWriter(multiline);
    Text restoredText;
    REQUIRE(decodeRecord(textWriter.bytes(), restoredText));
    CHECK(restoredText == multiline);
    std::vector<std::array<std::uint8_t, 0>> emptyElements(4);
    Archive emptyElementsWriter;
    emptyElementsWriter(emptyElements);
    REQUIRE(emptyElementsWriter.good());
    decltype(emptyElements) restoredElements;
    REQUIRE(decodeRecord(emptyElementsWriter.bytes(), restoredElements));
    CHECK(restoredElements == emptyElements);
}

TEST_CASE("header is written once and provenance survives re-save") {
    auto original = mission();
    Bytes bytes;
    REQUIRE(encode(original, bytes));
    CHECK(std::string(bytes.begin(), bytes.begin() + 8) == "MC2SAVE1");
    CHECK(Bytes(bytes.begin() + 8, bytes.begin() + 12) == Bytes{1,0,0,0});
    CHECK(Bytes(bytes.begin() + 12, bytes.begin() + 17) == Bytes{1,11,0,0,0});
    Document restored;
    REQUIRE(decode(bytes, compatibility(original), restored));
    CHECK(restored.sections == original.sections);
    CHECK(restored.header.originBuildSha == original.header.originBuildSha);
    Bytes resaved;
    REQUIRE(encode(restored, resaved));
    CHECK(resaved == bytes);
    Part part;
    REQUIRE(decodeRecord(restored.sections[9].payload, part));
    CHECK(part == samplePart());
}

TEST_CASE("every truncated envelope is rejected once and preserves previous state") {
    const auto original = mission();
    Bytes bytes;
    REQUIRE(encode(original, bytes));
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CAPTURE(length);
        Document previous = original;
        std::vector<std::string> logs;
        CHECK_FALSE(decode(std::span(bytes).first(length), compatibility(original), previous,
                           [&](std::string_view line) { logs.emplace_back(line); }));
        REQUIRE(logs.size() == 1);
        CHECK(logs[0].starts_with("[SAVE v1] reject reason="));
        CHECK(previous == original);
    }
}

TEST_CASE("every single-byte envelope mutation rejects with one log") {
    const auto original = mission();
    Bytes bytes;
    REQUIRE(encode(original, bytes));
    for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
        // Origin SHA is provenance rather than compatibility; another valid SHA is accepted.
        if (offset >= 33 && offset < 73) continue;
        CAPTURE(offset);
        auto corrupted = bytes;
        corrupted[offset] ^= 0x80;
        Document previous = original;
        std::vector<std::string> logs;
        CHECK_FALSE(decode(corrupted, compatibility(original), previous,
                           [&](std::string_view line) { logs.emplace_back(line); }));
        CHECK(logs.size() == 1);
        CHECK(previous == original);
    }
}

TEST_CASE("schema mismatch legacy bytes and fingerprint mismatch reject once") {
    const auto original = mission();
    Bytes bytes;
    REQUIRE(encode(original, bytes));
    bytes[8] = 2;
    Document output;
    std::vector<std::string> logs;
    auto sink = [&](std::string_view line) { logs.emplace_back(line); };
    CHECK_FALSE(decode(bytes, compatibility(original), output, sink));
    REQUIRE(logs.size() == 1);
    CHECK(logs[0] == "[SAVE v1] reject reason=schema_version");
    logs.clear();
    CHECK_FALSE(decode(Bytes{'0','.','0','6','.','0','1',0}, compatibility(original), output, sink));
    CHECK(logs.size() == 1);
    logs.clear();
    REQUIRE(encode(original, bytes));
    auto incompatible = compatibility(original);
    incompatible.dataFingerprint[0] ^= 1;
    CHECK_FALSE(decode(bytes, incompatible, output, sink));
    REQUIRE(logs.size() == 1);
    CHECK(logs[0] == "[SAVE v1] reject reason=fingerprint");
}

TEST_CASE("scalar text sequence and variant malformed corpus rejects before allocation") {
    std::vector<std::string> logs;
    auto sink = [&](std::string_view line) { logs.emplace_back(line); };
    auto rejects = [&]<class T>(const Bytes& bytes, T previous) {
        logs.clear();
        CHECK_FALSE(decodeRecord(bytes, previous, sink));
        REQUIRE(logs.size() == 1);
    };
    rejects(Bytes{2}, false);
    rejects(Bytes{0,0,128,127}, 1.0f);
    rejects(Bytes{0,0,192,127}, 1.0f);
    rejects(Bytes{3,0,0,0,0}, Text{});
    rejects(Bytes{1,255,255,255,255}, Text{});
    rejects(Bytes{1,2,0,0,0,192,128}, Text{});
    rejects(Bytes{255,255,255,255}, MissionParts{});
    rejects(Bytes{2,0,0,0}, MissionParts{});
    rejects(Bytes{0,0}, TaggedValue{});
    rejects(Bytes{2}, TaggedValue{});
    for (auto value : {TaggedValue{17, std::int32_t{-7}}, TaggedValue{{}, Text{2, "legacy"}},
                       TaggedValue{0, -0.0}}) {
        Archive writer;
        writer(value);
        REQUIRE(writer.finish());
        TaggedValue output;
        REQUIRE(decodeRecord(writer.bytes(), output));
        CHECK(output.optional == value.optional);
        CHECK(output.value == value.value);
        Archive resave;
        resave(output);
        CHECK(resave.bytes() == writer.bytes());
    }
}

TEST_CASE("ordered required section table cannot omit or duplicate state") {
    for (int defect = 0; defect < 4; ++defect) {
        auto doc = mission();
        if (defect == 0) doc.sections.erase(doc.sections.begin());
        if (defect == 1) doc.sections[1].tag = 1;
        if (defect == 2) std::swap(doc.sections[0], doc.sections[1]);
        if (defect == 3) doc.sections[0].required = false;
        Bytes output{42};
        std::vector<std::string> logs;
        CHECK_FALSE(encode(doc, output, [&](std::string_view line) { logs.emplace_back(line); }));
        CHECK(logs.size() == 1);
        CHECK(output == Bytes{42});
    }
    auto doc = mission();
    doc.sections.erase(doc.sections.begin() + 11);
    Bytes output;
    REQUIRE(encode(doc, output));
    Document restored;
    REQUIRE(decode(output, compatibility(doc), restored));
    doc.header.saveKind = 2;
    doc.sections = {{2, true, {0}}, {11, true, {0}}};
    REQUIRE(encode(doc, output));
    REQUIRE(decode(output, compatibility(doc), restored));
}

TEST_CASE("section corruption is rejected even with a recomputed outer hash") {
    const auto original = mission();
    Bytes bytes;
    REQUIRE(encode(original, bytes));
    Archive reader(bytes);
    Header header;
    reader(header);
    REQUIRE(reader.good());
    const auto offset = bytes.size() - reader.remaining();
    for (int defect = 0; defect < 5; ++defect) {
        Bytes payload(bytes.begin() + offset, bytes.end());
        if (defect == 0) payload[4] = 2;
        if (defect == 1) payload[5] = 255;
        if (defect == 2) payload[13] ^= 1;
        if (defect == 3) payload[46] = 1;
        if (defect == 4) payload[0] = 16;
        header.payloadSha256 = sha256(payload);
        Archive writer;
        writer(header);
        writer.raw(payload);
        REQUIRE(writer.good());
        std::vector<std::string> logs;
        Document previous = original;
        CHECK_FALSE(decode(writer.bytes(), compatibility(original), previous,
                           [&](std::string_view line) { logs.emplace_back(line); }));
        CHECK(logs.size() == 1);
        CHECK(previous == original);
    }
}

TEST_CASE("schema text limits and allocation budget reject before publishing") {
    auto part = samplePart();
    part.profileName.bytes = "123456789";
    std::vector<std::string> logs;
    Archive writer([&](std::string_view line) { logs.emplace_back(line); });
    writer(part);
    CHECK_FALSE(writer.finish());
    REQUIRE(logs.size() == 1);
    CHECK(logs[0] == "[SAVE v1] reject reason=text_length");
    logs.clear();
    const Bytes input{0};
    Archive budget(input, [&](std::string_view line) { logs.emplace_back(line); });
    CHECK_FALSE(budget.allocate(std::numeric_limits<std::size_t>::max(), 2));
    REQUIRE(logs.size() == 1);
    CHECK(logs[0] == "[SAVE v1] reject reason=decoded_budget");
}

TEST_CASE("Part varied values and maximum sequence retain float bits and order") {
    MissionParts original;
    original.parts.resize(maxSequenceLength);
    std::uint32_t state = 0x12345678;
    for (auto& part : original.parts) {
        state = state * 1664525u + 1013904223u;
        part = samplePart();
        part.objectWID = std::bit_cast<std::int32_t>(state);
        part.objNumber = state ^ 0x7fffffff;
        part.position.y = static_cast<float>(state & 0xffff) / 256;
        part.velocity = -static_cast<float>(state & 0x7fff) / 16;
        part.profileName.bytes = std::to_string(state % 100000000);
    }
    Archive writer;
    writer(original);
    REQUIRE(writer.finish());
    MissionParts restored;
    REQUIRE(decodeRecord(writer.bytes(), restored));
    CHECK(restored == original);
    Archive resave;
    resave(restored);
    CHECK(resave.bytes() == writer.bytes());
    original.parts.emplace_back();
    std::vector<std::string> logs;
    Archive tooMany([&](std::string_view line) { logs.emplace_back(line); });
    tooMany(original);
    CHECK_FALSE(tooMany.good());
    CHECK(logs.size() == 1);
}

TEST_CASE("every truncated Part field and unknown tag rejects once without changing the record") {
    const auto original = samplePart();
    auto part = original;
    Archive writer;
    writer(part);
    REQUIRE(writer.good());
    for (std::size_t length = 0; length < writer.bytes().size(); ++length) {
        std::vector<std::string> logs;
        part = original;
        CHECK_FALSE(decodeRecord(std::span(writer.bytes()).first(length), part,
                                [&](std::string_view line) { logs.emplace_back(line); }));
        CHECK(logs.size() == 1);
        CHECK(part == original);
    }
    for (int defect = 0; defect < 4; ++defect) {
        part = original;
        if (defect == 0) part.controlType = 4;
        if (defect == 1) part.controlDataType = 4;
        if (defect == 2) part.profileName.bytes = "C:/file";
        if (defect == 3) part.profileName.bytes = "../file";
        std::vector<std::string> logs;
        Archive invalid([&](std::string_view line) { logs.emplace_back(line); });
        invalid(part);
        CHECK_FALSE(invalid.good());
        CHECK(logs.size() == 1);
    }
    auto invalid = writer.bytes();
    invalid[64] = 4;
    std::vector<std::string> logs;
    CHECK_FALSE(decodeRecord(invalid, part, [&](std::string_view line) { logs.emplace_back(line); }));
    REQUIRE(logs.size() == 1);
    CHECK(logs[0] == "[SAVE v1] reject reason=part_control_tag");
}

TEST_CASE("Part serialization ignores native padding with two heap fill patterns") {
    Bytes outputs[2];
    for (int i = 0; i < 2; ++i) {
        alignas(Part) std::array<std::uint8_t, sizeof(Part)> storage;
        storage.fill(i == 0 ? 0x55 : 0xaa);
        auto* part = std::construct_at(reinterpret_cast<Part*>(storage.data()));
        *part = samplePart();
        Archive writer;
        writer(*part);
        REQUIRE(writer.good());
        outputs[i] = writer.bytes();
        std::destroy_at(part);
    }
    CHECK(outputs[0] == outputs[1]);
}

TEST_CASE("atomic file replacement and rejected read preserve previous save and world") {
    namespace fs = std::filesystem;
    struct Files {
        fs::path directory = fs::temp_directory_path() /
            ("mc2_save_archive_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Files() { fs::create_directory(directory); }
        ~Files() {
            std::error_code ignored;
            fs::remove(directory / "save", ignored);
            fs::remove(directory / "save.pending", ignored);
            fs::remove(directory, ignored);
        }
    } files;
    const auto path = files.directory / "save";
    auto doc = mission();
    REQUIRE(writeFile(path, doc));
    Document previous;
    REQUIRE(readFile(path, compatibility(doc), previous));
    doc.header.originBuildSha.bytes.assign(40, 'b');
    REQUIRE(writeFile(path, doc));
    Document replacement;
    REQUIRE(readFile(path, compatibility(doc), replacement));
    CHECK(replacement.header.originBuildSha.bytes == std::string(40, 'b'));
    CHECK_FALSE(fs::exists(files.directory / "save.pending"));
    std::vector<std::string> logs;
    auto sink = [&](std::string_view line) { logs.emplace_back(line); };
    auto invalid = doc;
    invalid.header.schemaVersion = 2;
    CHECK_FALSE(writeFile(path, invalid, sink));
    CHECK(logs.size() == 1);
    Document preserved;
    REQUIRE(readFile(path, compatibility(doc), preserved));
    CHECK(preserved == replacement);
    {
        std::ofstream pending(files.directory / "save.pending");
        pending << "another writer";
    }
    logs.clear();
    CHECK_FALSE(writeFile(path, doc, sink));
    CHECK(logs.size() == 1);
    CHECK(fs::file_size(files.directory / "save.pending") == 14);
    REQUIRE(readFile(path, compatibility(doc), preserved));
    CHECK(preserved == replacement);
    {
        std::ofstream truncated(path, std::ios::binary | std::ios::trunc);
        truncated << "MC2SAVE1";
    }
    logs.clear();
    CHECK_FALSE(readFile(path, compatibility(doc), preserved, sink));
    CHECK(logs.size() == 1);
    CHECK(preserved == replacement);
}
}
