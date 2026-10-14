#include "../../tests/unit/doctest.h"
#include "ObjectCatalog.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <zlib.h>

using namespace editor::document;
namespace {
std::filesystem::path scratch()
{
    const auto root = std::filesystem::path("scratch/e5-catalog-tests") / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    return root;
}
void write(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}
void word(std::string& bytes, std::size_t p, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[p + i] = static_cast<char>(value >> (8 * i));
}
std::string definition(int runtime, std::string_view appearance = "appearance")
{
    return "FITini\n[ObjectClass]\nl ObjectTypeNum = " + std::to_string(runtime) +
        "\n[ObjectType]\nst AppearanceName = \"" + std::string(appearance) + "\"\n[Footprint]\nul LowTemplate = 1\nul HighTemplate = 2\nb BlocksLineOfFire = true\nFITend\n";
}
std::string pak(const std::vector<std::string>& packets)
{
    const auto start = 8 + 4 * packets.size();
    std::string bytes(start, '\0');
    word(bytes, 0, 0xfeedface); word(bytes, 4, static_cast<std::uint32_t>(start));
    for (std::size_t i = 0; i < packets.size(); ++i) {
        word(bytes, 8 + i * 4, static_cast<std::uint32_t>(bytes.size()));
        bytes += packets[i];
    }
    return bytes;
}
std::string row(std::string_view stem, unsigned id, std::string_view type = "BUILDING", std::string_view special = "")
{
    return std::string(stem) + ",2,30400," + std::string(type) + "," + std::to_string(id) + "," + std::string(special) + ",1,0,1,icon,,1.5\n";
}
void base(const std::filesystem::path& root)
{
    write(root / "system.cfg", "FITini\n[FastFiles]\nl NumFastFiles = 0\nFITend\n");
    write(root / "data/art/Buildings.csv", "File Name,Group ID,NameID,Type,FitID,SPECIAL TYPE,ALIGNMENT?,CAPTURABLE,Draw Building On TacMap,TGA Filename,Mechlopedia Scale,Mission Briefing scale\n" + row("house", 0) + row("mech", 1, "MECH"));
    write(root / "data/objects/object2.pak", pak({definition(1), definition(2)}));
}
std::string bytes(const ResolvedFile& file)
{
    return {reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()};
}
std::string fst(std::string_view key, std::string_view plain, bool compress)
{
    std::string encoded(plain);
    if (compress) {
        uLongf size = compressBound(static_cast<uLong>(plain.size()));
        encoded.resize(size);
        REQUIRE(compress2(reinterpret_cast<Bytef*>(encoded.data()), &size, reinterpret_cast<const Bytef*>(plain.data()), static_cast<uLong>(plain.size()), 6) == Z_OK);
        encoded.resize(size);
    }
    std::string archive(274, '\0');
    word(archive, 0, 0xfaddecaf); word(archive, 4, 1);
    word(archive, 8, 274); word(archive, 12, static_cast<std::uint32_t>(encoded.size())); word(archive, 16, static_cast<std::uint32_t>(plain.size()));
    std::uint32_t hash{};
    for (const unsigned char c : FileResolver::normalize(key)) { hash = (hash << 4) + c; const auto high = hash & 0xf0000000; if (high) hash ^= high >> 24; hash &= ~high; }
    word(archive, 20, hash);
    archive.replace(24, key.size(), key);
    return archive + encoded;
}
}

TEST_CASE("owned stock catalog preserves production columns, packets and variants")
{
    const auto root = scratch(); base(root);
    std::string variants;
    for (unsigned i = 0; i < 121; ++i) variants += i == 22 ? "Variant,Stock Variant\n" : i == 119 ? "Variant,Second Variant\n" : ",\n";
    write(root / "data/objects/mech.csv", variants);
    auto resolver = FileResolver::mount({root}); REQUIRE(resolver);
    std::vector<std::string> logs;
    auto catalog = ObjectCatalog::load(*resolver, [&](const auto& s) { logs.push_back(s); }, [](auto) { return std::optional<std::string>("Localized"); });
    REQUIRE(catalog.entries().size() == 2); CHECK(logs.empty()); CHECK(catalog.rejections() == 0);
    const auto& house = catalog.entries()[0];
    CHECK(house.id == 0); CHECK(house.family == ObjectFamily::building); CHECK(house.scale == 1.5);
    CHECK(house.capturable); CHECK_FALSE(house.alignable); CHECK(house.onTacMap);
    CHECK(house.impassability == 0x200000001ULL); CHECK(house.blocksLineOfFire); CHECK(house.displayName == "Localized");
    CHECK(house.definition.layer == "base-loose");
    REQUIRE(catalog.entries()[1].variants.size() == 2); CHECK(catalog.entries()[1].variants[1].index == 1);
    CHECK(catalog.entries()[1].variants[1].name == "Second Variant");
    auto again = ObjectCatalog::load(*resolver, {}); CHECK(again.entries()[0].displayName == "house_30400");
}

TEST_CASE("catalog accepts file-within-file raw definition packets")
{
    const auto root = scratch(); base(root);
    const std::vector definitions{definition(1), definition(2)};
    auto archive = pak(definitions);
    word(archive, 8, (8 + 4 * static_cast<std::uint32_t>(definitions.size())) | (1U << 29));
    word(archive, 12, (8 + 4 * static_cast<std::uint32_t>(definitions.size()) + static_cast<std::uint32_t>(definitions[0].size())) | (1U << 29));
    write(root / "data/objects/object2.pak", archive);
    auto resolver = FileResolver::mount({root}); REQUIRE(resolver);
    std::vector<std::string> logs;
    const auto catalog = ObjectCatalog::load(*resolver, [&](const auto& line) { logs.push_back(line); });
    REQUIRE(catalog.entries().size() == 2);
    CHECK(catalog.entries()[0].family == ObjectFamily::building);
    CHECK(catalog.entries()[1].family == ObjectFamily::mech);
    CHECK(logs.empty()); CHECK(catalog.rejections() == 0);
}

TEST_CASE("resource blocks accept indented and same-line endings outside quoted text")
{
    const auto root = scratch(); base(root);
    write(root / "data/defs/text/en_us/formats.fit",
        "String {\n key = \"indented\"\n text = \"Name } { with \\\"quotes\\\"\"\n legacyId = 30400\n  }\n"
        "String { key = \"inline\"; text = \"Inline\"; legacyId = 30401; } String { key = \"following\"; text = \"Following\"; legacyId = 30402; }\n"
        "LegacyTextAlias { legacyId = 30403; textKey = \"inline\"; }\n"
        "String {\n key = \"last\"\n text = \"Last\"\n legacyId = 30404\n}\n");
    auto resolver = FileResolver::mount({root}); REQUIRE(resolver);
    std::vector<std::string> logs;
    const auto names = resourceNames(*resolver, [&](const auto& line) { logs.push_back(line); });
    CHECK(names(30400) == std::optional<std::string>("Name } { with \"quotes\""));
    CHECK(names(30401) == std::optional<std::string>("Inline"));
    CHECK(names(30402) == std::optional<std::string>("Following"));
    CHECK(names(30403) == std::optional<std::string>("Inline"));
    CHECK(names(30404) == std::optional<std::string>("Last"));
    CHECK(logs.empty());
}

TEST_CASE("catalog rejects each malformed entry once and retains following healthy rows")
{
    const auto root = scratch(); base(root);
    write(root / "data/objects/object2.pak", pak({definition(1), "FITini\n[ObjectClass]\nl ObjectTypeNum = nope\nFITend\n", definition(4), definition(3)}));
    write(root / "data/art/Buildings.csv", "header\n" + row("house", 0) + row("badfit", 1) + row("effect", 2) + row("missing", 99) +
        "malformed\n" + row("house", 0) + "nan,1,1,BUILDING,3,,0,0,0,icon,,nan\n" + row("last", 3, "VEHICLE"));
    auto resolver = FileResolver::mount({root}); REQUIRE(resolver);
    std::vector<std::string> logs;
    const auto catalog = ObjectCatalog::load(*resolver, [&](const auto& s) { logs.push_back(s); });
    REQUIRE(catalog.entries().size() == 2); CHECK(catalog.entries().back().stem == "last");
    CHECK(catalog.rejections() == 6); REQUIRE(logs.size() == 6);
    for (const auto& line : logs) { CHECK(line.find('\n') == line.npos); CHECK(line.starts_with("[EDITOR_CATALOG_REJECT v1]")); }
}

TEST_CASE("owned resolver uses active, declared dependencies, base and ordered FastFiles without row merging")
{
    const auto root = scratch(); base(root);
    const auto mods = root / "mods";
    write(mods / "active/mod.json", "{\"id\":\"active\",\"dependencies\":[\"first\",\"second\"]}");
    write(mods / "active/data/ART/BUILDINGS.CSV", "header\n" + row("only", 1, "MECH"));
    write(mods / "first/data/objects/object2.pak", pak({definition(1), definition(2, "first")}));
    write(mods / "second/data/objects/object2.pak", pak({definition(1), definition(2, "second")}));
    write(mods / "extra/data/objects/object2.pak", "broken");
    write(mods / "active/data/.hidden/hidden.fit", "hidden");
    auto resolver = FileResolver::mount({root, mods, "active", {"first", "extra"}}); REQUIRE(resolver);
    CHECK((resolver->config().dependencies == std::vector<std::string>{"first", "second", "extra"}));
    auto catalog = ObjectCatalog::load(*resolver, {});
    REQUIRE(catalog.entries().size() == 1); CHECK(catalog.entries()[0].stem == "only");
    CHECK(catalog.entries()[0].appearance == "first"); CHECK(catalog.entries()[0].enumeration.layer == "mod:active");
    CHECK(catalog.entries()[0].definition.layer == "mod:first");
    const auto normalized = resolver->read("DATA\\art\\Buildings.CSV"); REQUIRE(normalized); CHECK(normalized->layer == "mod:active");
    CHECK_FALSE(resolver->read("data/.hidden/hidden.fit")); CHECK_FALSE(resolver->read("../system.cfg"));
    write(root / "system.cfg", "FITini\n[FastFiles]\nl NumFastFiles = 2\nst File0 = \"first.fst\"\nst File1 = \"second.fst\"\nFITend\n");
    write(root / "first.fst", fst("data/test.fit", std::string(300, 'a'), true));
    write(root / "second.fst", fst("data/test.fit", "second archive", false));
    resolver = FileResolver::mount({root}); REQUIRE(resolver);
    auto file = resolver->read("data/test.fit"); REQUIRE(file); CHECK(bytes(*file) == std::string(300, 'a')); CHECK(file->source.filename() == "first.fst");
    write(root / "data/test.fit", "loose");
    auto looseResolver = FileResolver::mount({root}); REQUIRE(looseResolver);
    auto loose = looseResolver->read("data/test.fit"); REQUIRE(loose); CHECK(bytes(*loose) == "loose");
    CHECK(bytes(*resolver->read("data/test.fit")) == std::string(300, 'a'));
    write(root / "data/test.fit", "broken override");
    CHECK(bytes(*looseResolver->read("data/test.fit")) == "broken override");
}

TEST_CASE("missing catalog and corrupt archives fail with bounded diagnostics")
{
    const auto root = scratch(); write(root / "system.cfg", "FITini\n[FastFiles]\nl NumFastFiles = 0\nFITend\n");
    auto resolver = FileResolver::mount({root}); REQUIRE(resolver);
    std::vector<std::string> logs;
    auto catalog = ObjectCatalog::load(*resolver, [&](const auto& s) { logs.push_back(s); });
    CHECK(catalog.entries().empty()); CHECK(catalog.rejections() == 1); CHECK(logs.size() == 1);
    write(root / "system.cfg", "FITini\n[FastFiles]\nl NumFastFiles = 1\nst File0 = \"bad.fst\"\nFITend\n");
    write(root / "bad.fst", "short"); CHECK_FALSE(FileResolver::mount({root}));
    CHECK_FALSE(FileResolver::mount({root, {}, "../escape"}));
}

TEST_CASE("resource names resolve numeric strings and clean mod aliases with owned lifetime")
{
    const auto root = scratch(); base(root);
    write(root / "data/defs/text/en_us/a.fit", "/* header */\nString {\n key = \"legacy.name\"\n text = \"Stock Name\"\n legacyId = 30400\n source = \"mc2res\"\n}\n");
    write(root / "mods/active/data/defs/text/en_us/z.fit", "String {\n key = \"mod.name\"\n text = \"Mod \\\"Name\\\"\"\n}\nLegacyTextAlias {\n legacyId = 30400\n textKey = \"mod.name\"\n}\n");
    auto resolver = FileResolver::mount({root, {}, "active"}); REQUIRE(resolver);
    auto names = resourceNames(*resolver);
    CHECK(names(30400) == std::optional<std::string>("Mod \"Name\""));
    CHECK_FALSE(names(99));
    const auto catalog = ObjectCatalog::load(*resolver, {});
    REQUIRE(catalog.entries().size() == 2); CHECK(catalog.entries()[0].displayName == "Mod \"Name\"");
    resolver = FileResolver::mount({root}); REQUIRE(resolver);
    CHECK(names(30400) == std::optional<std::string>("Mod \"Name\""));
    CHECK(resourceNames(*resolver)(30400) == std::optional<std::string>("Stock Name"));
}

TEST_CASE("catalog accepts legacy boolean literals and unsigned footprint bits")
{
    const auto root = scratch(); base(root);
    write(root / "data/objects/object2.pak", pak({definition(1),
        "FITini\n[ObjectClass]\nl ObjectTypeNum = 3\n[ObjectType]\nst AppearanceName = \"hover\"\n[General]\nb HoverCraft = t // legacy FIT spelling\n[Footprint]\nl LowTemplate = 0xffffffff\nl HighTemplate = 1\nFITend\n"}));
    auto resolver = FileResolver::mount({root}); REQUIRE(resolver);
    const auto catalog = ObjectCatalog::load(*resolver, {});
    REQUIRE(catalog.entries().size() == 2); CHECK(catalog.entries()[1].hover);
    CHECK(catalog.entries()[1].impassability == 0x1ffffffffULL);
}

TEST_CASE("FastFile legacy LZW literals and stored hash agree with normalized keys")
{
    const auto root = scratch(); base(root);
    write(root / "system.cfg", "FITini\n[FastFiles]\nl NumFastFiles = 1\nst File0 = \"literal.fst\"\nFITend\n");
    auto archive = fst("data/literal.fit", "ABC", false);
    archive.resize(274);
    std::uint32_t buffer{}; unsigned available{};
    for (unsigned code : {256U, 65U, 66U, 67U, 257U}) {
        buffer |= code << available; available += 9;
        while (available >= 8) { archive += static_cast<char>(buffer); buffer >>= 8; available -= 8; }
    }
    if (available) archive += static_cast<char>(buffer);
    word(archive, 12, static_cast<std::uint32_t>(archive.size() - 274));
    write(root / "literal.fst", archive);
    auto resolver = FileResolver::mount({root}); REQUIRE(resolver);
    auto decoded = resolver->read("DATA\\LITERAL.FIT"); REQUIRE(decoded); CHECK(bytes(*decoded) == "ABC");
    word(archive, 20, 1); write(root / "literal.fst", archive);
    resolver = FileResolver::mount({root}); REQUIRE(resolver); CHECK_FALSE(resolver->read("data/literal.fit"));
    archive = fst("data/literal.fit", "ABAB", false); archive.resize(274);
    buffer = 0; available = 0;
    for (unsigned code : {65U, 66U, 258U, 257U}) {
        buffer |= code << available; available += 9;
        while (available >= 8) { archive += static_cast<char>(buffer); buffer >>= 8; available -= 8; }
    }
    if (available) archive += static_cast<char>(buffer);
    word(archive, 12, static_cast<std::uint32_t>(archive.size() - 274));
    write(root / "literal.fst", archive);
    resolver = FileResolver::mount({root}); REQUIRE(resolver);
    decoded = resolver->read("data/literal.fit"); REQUIRE(decoded); CHECK(bytes(*decoded) == "ABAB");
}

TEST_CASE("FastFiles sniff zlib when encoded and decoded lengths are equal")
{
    const auto root = scratch(); base(root);
    write(root / "system.cfg", "FITini\n[FastFiles]\nl NumFastFiles = 1\nst File0 = \"equal.fst\"\nFITend\n");
    std::string plain = "AAAAAAAAAAA";
    auto archive = fst("data/equal.fit", plain, true);
    REQUIRE(archive.size() - 274 == plain.size());
    write(root / "equal.fst", archive);
    auto resolver = FileResolver::mount({root}); REQUIRE(resolver);
    auto decoded = resolver->read("data/equal.fit"); REQUIRE(decoded); CHECK(bytes(*decoded) == plain);
    archive = fst("data/equal.fit", std::string("\x78\x9c raw bytes"), false);
    write(root / "equal.fst", archive);
    resolver = FileResolver::mount({root}); REQUIRE(resolver);
    decoded = resolver->read("data/equal.fit"); REQUIRE(decoded); CHECK(bytes(*decoded) == std::string("\x78\x9c raw bytes"));
}

TEST_CASE("catalog isolates damaged packets and rejects mistyped facts")
{
    const auto root = scratch(); base(root);
    auto definitions = pak({definition(1), "FITini\n[ObjectClass]\nl ObjectTypeNum = 3\n[General]\nst HoverCraft = \"true\"\nFITend\n", definition(21), definition(2)});
    const auto raw = static_cast<std::uint32_t>(static_cast<unsigned char>(definitions[16])) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(definitions[17])) << 8);
    word(definitions, 16, raw | (5U << 29));
    write(root / "data/objects/object2.pak", definitions);
    write(root / "data/art/Buildings.csv", "header\n" + row("a", 0) + row("wrongtype", 1) + row("badpacket", 2) + row("last", 3));
    auto resolver = FileResolver::mount({root}); REQUIRE(resolver);
    std::vector<std::string> logs;
    const auto catalog = ObjectCatalog::load(*resolver, [&](const auto& s) { logs.push_back(s); });
    REQUIRE(catalog.entries().size() == 2); CHECK(catalog.entries().back().id == 3);
    CHECK(logs.size() == 2); CHECK(catalog.rejections() == 2);
}

TEST_CASE("catalog runtime classes cover every supported legacy placement family")
{
    const auto root = scratch(); base(root);
    const std::array classes{2, 3, 1, 21, 0, 22, 1, 21, 1};
    const std::array families{ObjectFamily::mech, ObjectFamily::vehicle, ObjectFamily::building, ObjectFamily::turret, ObjectFamily::tree,
        ObjectFamily::gate, ObjectFamily::bridge, ObjectFamily::light, ObjectFamily::marker};
    const std::array<std::string_view, 9> special{"", "", "", "TURRET", "TREE3", "GATE", "BRIDGE", "SPOTLIGHT", "NAVMARKER"};
    std::vector<std::string> definitions;
    std::string rows = "header\n";
    for (unsigned i = 0; i < classes.size(); ++i) {
        definitions.push_back(definition(classes[i]));
        rows += row("object" + std::to_string(i), i, "BUILDING", special[i]);
    }
    write(root / "data/objects/object2.pak", pak(definitions)); write(root / "data/art/Buildings.csv", rows);
    auto resolver = FileResolver::mount({root}); REQUIRE(resolver);
    const auto catalog = ObjectCatalog::load(*resolver, {});
    REQUIRE(catalog.entries().size() == 9);
    for (unsigned i = 0; i < families.size(); ++i) CHECK(catalog.entries()[i].family == families[i]);
    CHECK(catalog.entries()[4].forestGroup == 3);
}
