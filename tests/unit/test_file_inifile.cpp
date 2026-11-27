// tests/unit/test_file_inifile.cpp
//
// First tests that run REAL mclib file-stack TUs
// (file.cpp / inifile.cpp / packet.cpp) game-free, on top of the malloc-backed
// heap shim (heap_shim.cpp).
//
// What this proves:
//   * FitIniFile disk parse -- the game's config spine, offline-verifiable.
//   * FitIniFile::open(buffer,len) -- the in-memory open seam previously had
//     ZERO callers and no runtime proof. These are its first runtime
//     assertions.
//   * PacketFile write/read round-trip -- the .pak container used by object2.pak
//     et al., exercised end-to-end with no external data dependency.
//   * File loose-disk resolution (base-loose branch of File::open).

#include "doctest.h"

#include "file.h"
#include "inifile.h"
#include "packet.h"
#include "cident.h"
#include "../../code/model_credit.h"
#include "../../code/purchase_catalog.h"
#include "../../code/objective_text.h"
#include "../../code/logistics_csv.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifndef MC2_TEST_FIXTURE_DIR
#error "MC2_TEST_FIXTURE_DIR must be defined by tests/unit/CMakeLists.txt"
#endif

namespace
{

std::string fixturePath (const char *name)
{
	std::string p(MC2_TEST_FIXTURE_DIR);
	p += "/";
	p += name;
	return p;
}

// Scratch-file path inside the build dir (cwd for `check`) or wherever the
// test binary is launched from. Lowercase on purpose: File::open lowercases
// paths (S_strlwr, file.cpp) before hitting the OS.
std::string scratchPath (const char *name)
{
	std::string p("mvh_scratch_");
	p += name;
	return p;
}

const char kFitInMemText[] =
	"FITini\r\n"
	"\r\n"
	"[MemBlock]\r\n"
	"l Alpha = 5\r\n"
	"f Beta = 1.25\r\n"
	"st Name = \"inmem\"\r\n"
	"\r\n"
	"FITend\r\n";

} // namespace

TEST_SUITE("FileStack")
{
TEST_CASE("pilots/blank CSV rows do not terminate the roster")
{
    char csv[] = "\r\n \t\r\n alpha,\r\nb\r\n,\r\ncharlie";
    File file;
    REQUIRE(file.open(csv, sizeof(csv) - 1) == NO_ERR);
    unsigned char row[256] = {};
    std::string names;
    int loaded = 0, skipped = 0;
    while (!file.eof()) {
        file.readLine(row, 255);
        const char* name = logistics_csv::pilotFileName(reinterpret_cast<char*>(row));
        if (!*name) { ++skipped; continue; }
        names += std::string(name) + ";";
        ++loaded;
    }
    CHECK(names == "alpha;b;charlie;");
    CHECK(loaded == 3);
    CHECK(skipped == 3);
}

TEST_CASE("carbon_mclib/RAM seek is relative to each file after reads")
{
    char a[] = "abcdef", b[] = "uvwxyz";
    File first, second;
    REQUIRE(first.open(a, 6) == NO_ERR);
    REQUIRE(second.open(b, 6) == NO_ERR);
    REQUIRE(first.seek(1, SEEK_SET) == NO_ERR);
    unsigned char byte = 0;
    REQUIRE(first.read(&byte, 1) == 1);
    REQUIRE(second.seek(4, SEEK_SET) == NO_ERR);
    REQUIRE(first.seek(1, SEEK_CUR) == NO_ERR);
    REQUIRE(first.read(&byte, 1) == 1);
    CHECK(byte == 'd');
}

TEST_CASE("carbon_mclib/RAM line with a final delimiter stops at EOF")
{
    char text[] = {'a', '\r'};
    File file;
    REQUIRE(file.open(text, sizeof text) == NO_ERR);
    unsigned char line[8] = {};
    CHECK(file.readLine(line, 7) == 2);
    CHECK(std::strcmp(reinterpret_cast<char*>(line), "a") == 0);
    CHECK(file.getLogicalPosition() == sizeof text);
}

TEST_CASE("carbon_mclib/FIT strings reject missing quotes")
{
    struct Parser : FitIniFile { using FitIniFile::copyString; } parser;
    char out[16] = {}, noQuote[] = "plain", noEnd[] = "\"unfinished", valid[] = "\"ok\"";
    CHECK(parser.copyString(out, noQuote, sizeof out) == SYNTAX_ERROR);
    CHECK(parser.copyString(out, noEnd, sizeof out) == SYNTAX_ERROR);
    CHECK(parser.copyString(out, valid, sizeof out) == NO_ERR);
    CHECK(std::strcmp(out, "ok") == 0);
    char quoted[] = "\"abc\"";
    for (unsigned long capacity : {0ul, 1ul, 3ul, 4ul}) {
        char guarded[] = "ZZZZZ";
        CHECK(parser.copyString(guarded, quoted, capacity) == (capacity < 4 ? BUFFER_TOO_SMALL : NO_ERR));
        CHECK(guarded[capacity] == 'Z');
        if (capacity == 4) CHECK(std::strcmp(guarded, "abc") == 0);
    }
}

TEST_CASE("carbon_mclib/short paths append a longer extension once")
{
    FullPathFileName shortName("", "a", ".fit"), suffixed("", "a.fit", ".fit");
    CHECK(std::strcmp(shortName, "a.fit") == 0);
    CHECK(std::strcmp(suffixed, "a.fit") == 0);
}

namespace vehicle_results {
struct Variant {
    bool vehicle = true;
    const Variant* getChassis() const { return this; }
    bool isVehicle() const { return vehicle; }
    std::string getName() const { return "Hunter"; }
    std::string getFileName() const { return "hunter.csv"; }
    bool isDesignerMech() const { return true; }
};
struct { unsigned long baseColor = 1, highlightColor = 2; } prefs;
struct LogisticsData {
    struct Snapshot { Variant* pVariant; long fitID; bool recovered = false; };
    std::vector<Snapshot> deployedMechSnapshot;
    std::vector<Variant*> inventory;
    void* getVehiclePilot() { return this; }
    void addMechToInventory(Variant* v, int, void*, unsigned long, unsigned long, unsigned long) { inventory.push_back(v); }
    void recoverDeployedVehicle(long, bool, int&);
};
#include "vehicle_recovery.inc"
}

TEST_CASE("purchase_catalog/surviving purchased vehicles return through results and FIT reload") {
    vehicle_results::Variant hunter, mech{false};
    vehicle_results::LogisticsData data;
    data.deployedMechSnapshot = {{&hunter, 7}, {&hunter, 7}, {&mech, 8}};
    int group = 1;
    data.recoverDeployedVehicle(7, false, group);
    data.recoverDeployedVehicle(7, true, group);
    data.recoverDeployedVehicle(7, true, group);
    data.recoverDeployedVehicle(8, true, group);
    data.recoverDeployedVehicle(9, true, group);
    REQUIRE(data.inventory.size() == 1);
    CHECK(group == 2);
    const auto path = scratchPath("vehicle_results.fit");
    {
        FitIniFile out;
        REQUIRE(out.create(path.c_str()) == NO_ERR);
        out.writeBlock("Inventory0");
        auto* v = data.inventory.front();
        mc2purchase::saveInventoryIdentity(out, "", v->getName(), v->getFileName(), true, "PMWScout");
        out.close();
    }
    FitIniFile in;
    REQUIRE(in.open(path.c_str()) == NO_ERR);
    REQUIRE(in.seekBlock("Inventory0") == NO_ERR);
    std::vector<vehicle_results::Variant*> catalog{&hunter};
    REQUIRE(mc2purchase::loadInventoryVariant(in, catalog, [](const auto&) { return std::string(); }) == catalog.begin());
    in.close();
    remove(path.c_str());
}

// Trigger paths: code/purchase_catalog.h, code/logisticsmech.cpp, code/logisticsdata_save.cpp.
TEST_CASE("purchase_catalog/full inventory identity survives real FIT save reload")
{
    struct Variant {
        std::string chassis;
        bool designer;
        std::string getName() const { return "Atlas"; }
        const std::string& getFileName() const { return chassis; }
        bool isDesignerMech() const { return designer; }
    } other{"other.csv", false}, stock{"atlas.csv", true}, custom{"atlas.csv", false};
    std::vector<Variant*> variants{&other, &stock, &custom};
    const auto source = [](const auto&) { return std::string("mco"); };
    const auto path = scratchPath("inventory_identity.fit");
    for (const bool designer : {false, true}) {
        {
            FitIniFile out;
            REQUIRE(out.create(path.c_str()) == NO_ERR);
            out.writeBlock("Inventory0");
            mc2purchase::saveInventoryIdentity(out, "MCO", "ATLAS", "ATLAS.CSV", designer);
            out.writeBlock("Legacy");
            mc2purchase::saveIdentity(out, "mco", "Atlas");
            out.writeBlock("NoPack");
            out.writeIdString("Variant", "Atlas");
            out.writeBlock("Fallback");
            mc2purchase::saveInventoryIdentity(out, "mco", "Atlas", "missing.csv", designer);
            out.writeBlock("Missing");
            mc2purchase::saveIdentity(out, "missing-pack", "Atlas");
            out.close();
        }
        FitIniFile in;
        REQUIRE(in.open(path.c_str()) == NO_ERR);
        REQUIRE(in.seekBlock("Inventory0") == NO_ERR);
        auto restored = mc2purchase::loadInventoryVariant(in, variants, source);
        REQUIRE(restored != variants.end());
        CHECK(*restored == (designer ? &stock : &custom));
        REQUIRE(in.seekBlock("Legacy") == NO_ERR);
        CHECK(*mc2purchase::loadInventoryVariant(in, variants, source) == &other);
        REQUIRE(in.seekBlock("NoPack") == NO_ERR);
        CHECK(*mc2purchase::loadInventoryVariant(in, variants, source) == &other);
        REQUIRE(in.seekBlock("Fallback") == NO_ERR);
        CHECK(*mc2purchase::loadInventoryVariant(in, variants, source) == &other);
        REQUIRE(in.seekBlock("Missing") == NO_ERR);
        CHECK(mc2purchase::loadInventoryVariant(in, variants, source) == variants.end());
        in.close();
    }
    remove(path.c_str());
}

// Trigger paths: code/purchase_catalog.h, code/logisticsmech.cpp, code/logisticsdata_save.cpp.
TEST_CASE("purchase_catalog/pack purchase survives real FIT save reload with duplicate names")
{
    const auto path = scratchPath("purchase.fit");
    {
        FitIniFile out;
        REQUIRE(out.create(path.c_str()) == NO_ERR);
        out.writeBlock("Inventory0");
        mc2purchase::saveIdentity(out, "bt2018", "Atlas");
        out.close();
    }
    {
        FitIniFile in;
        REQUIRE(in.open(path.c_str()) == NO_ERR);
        REQUIRE(in.seekBlock("Inventory0") == NO_ERR);
        const auto pack = objective_text::readPlain(in, "PurchasePack");
        const auto name = objective_text::readPlain(in, "Variant");
        REQUIRE(pack == "bt2018");
        REQUIRE(name == "Atlas");
        const std::vector<mc2purchase::Item> variants = {{"", "Atlas"}, {"other", "Atlas"}, {"bt2018", "Atlas"}};
        const auto restored = std::find_if(variants.begin(), variants.end(), [&](const auto& variant) {
            return mc2purchase::identityMatches(variant.source, variant.name, pack, *name);
        });
        REQUIRE(restored != variants.end());
        CHECK(restored->source == "bt2018");
        in.close();
    }
    remove(path.c_str());
}

// Trigger paths: code/model_credit.h and code/simplecamera.cpp.
TEST_CASE("ModelCredit/appearance manifest requires real author and source")
{
    auto read = [](const char* fields) {
        std::string text = std::string("FITini\r\n[ModelCredit]\r\n") + fields + "FITend\r\n";
        FitIniFile fit;
        REQUIRE(fit.open(text.data(), static_cast<int>(text.size())) == NO_ERR);
        return readModelCredit(fit);
    };
    CHECK(read("st Author = \"Karl\"\r\nst Source = \"Wolfman TCE p.4\"\r\n") ==
          "Model by: Karl (Wolfman TCE p.4)");
    CHECK(read("st Author = \"Wolfman\"\r\n").empty());
    CHECK(read("st Source = \"TCE\"\r\n").empty());
    CHECK(read("st Author = \"\"\r\nst Source = \"TCE\"\r\n").empty());
    std::string stock = "FITini\r\n[TGLData]\r\nst FileName = \"Atlas\"\r\nFITend\r\n";
    FitIniFile fit;
    REQUIRE(fit.open(stock.data(), static_cast<int>(stock.size())) == NO_ERR);
    CHECK(readModelCredit(fit).empty());
}

TEST_CASE("FitIniFile parses a .fit fixture from disk")
{
	FitIniFile fit;
	const std::string path = fixturePath("harness_smoke.fit");
	REQUIRE(fit.open(path.c_str()) == NO_ERR);

	SUBCASE("block seek + typed reads")
	{
		REQUIRE(fit.seekBlock("TestBlock") == NO_ERR);

		long l = 0;
		CHECK(fit.readIdLong("TestLong", l) == NO_ERR);
		CHECK(l == 42);

		float f = 0.0f;
		CHECK(fit.readIdFloat("TestFloat", f) == NO_ERR);
		CHECK(f == doctest::Approx(3.5f));

		char s[64] = {0};
		CHECK(fit.readIdString("TestString", s, sizeof(s) - 1) == NO_ERR);
		CHECK(strcmp(s, "hello harness") == 0);
	}

	SUBCASE("second block is independently addressable")
	{
		REQUIRE(fit.seekBlock("SecondBlock") == NO_ERR);
		long v = 0;
		CHECK(fit.readIdLong("Another", v) == NO_ERR);
		CHECK(v == 7);
	}

	SUBCASE("missing block / missing var fail cleanly")
	{
		CHECK((unsigned long)fit.seekBlock("NoSuchBlock") == BLOCK_NOT_FOUND);
		REQUIRE(fit.seekBlock("TestBlock") == NO_ERR);
		long v = 0;
		CHECK(fit.readIdLong("NoSuchVar", v) != NO_ERR);
	}

	fit.close();
	CHECK(!fit.isOpen());
}

TEST_CASE("FitIniFile::open(buffer,len) parses in-memory .fit text (FITINI-INMEM)")
{
	// Caller-owned mutable buffer: File::open(buffer,len) aliases the memory
	// (no copy) and close() must NOT free it (file.cpp close(): fileImage is
	// only freed when bFast||parent -- 'don't want to delete memFiles').
	std::string text(kFitInMemText);

	FitIniFile fit;
	REQUIRE(fit.open(text.data(), (int)text.size()) == NO_ERR);

	REQUIRE(fit.seekBlock("MemBlock") == NO_ERR);

	long l = 0;
	CHECK(fit.readIdLong("Alpha", l) == NO_ERR);
	CHECK(l == 5);

	float f = 0.0f;
	CHECK(fit.readIdFloat("Beta", f) == NO_ERR);
	CHECK(f == doctest::Approx(1.25f));

	char s[32] = {0};
	CHECK(fit.readIdString("Name", s, sizeof(s) - 1) == NO_ERR);
	CHECK(strcmp(s, "inmem") == 0);

	fit.close();

	// Buffer is caller-owned and must be intact after close().
	CHECK(text == kFitInMemText);
}

TEST_CASE("FitIniFile::open(buffer,len) rejects NULL / empty input")
{
	FitIniFile fit;
	CHECK(fit.open((const char *)NULL, 16) != NO_ERR);
	char dummy[4] = "x";
	CHECK(fit.open(dummy, 0) != NO_ERR);
}

TEST_CASE("PacketFile write/read round-trip")
{
	const std::string path = scratchPath("roundtrip.pak");
	const char p0[] = "alpha";
	const char p1[] = "bravo-longer-payload";
	const char p2[] = "c";

	// --- write ---
	{
		PacketFile out;
		REQUIRE(out.create(path.c_str()) == NO_ERR);
		out.reserve(3);
		// writePacket returns bytes written, not NO_ERR.
		CHECK(out.writePacket(0, (MemoryPtr)p0, sizeof(p0)) == (int)sizeof(p0));
		CHECK(out.writePacket(1, (MemoryPtr)p1, sizeof(p1)) == (int)sizeof(p1));
		CHECK(out.writePacket(2, (MemoryPtr)p2, sizeof(p2)) == (int)sizeof(p2));
		out.close();
	}

	// --- read back ---
	{
		PacketFile in;
		REQUIRE(in.open(path.c_str()) == NO_ERR);
		CHECK(in.getNumPackets() == 3);

		REQUIRE(in.seekPacket(1) == NO_ERR);
		CHECK(in.getPacketSize() == (int)sizeof(p1));

		char buf[64] = {0};
		REQUIRE((size_t)in.getPacketSize() <= sizeof(buf));
		in.readPacket(1, (unsigned char *)buf);
		CHECK(strcmp(buf, p1) == 0);

		REQUIRE(in.seekPacket(0) == NO_ERR);
		CHECK(in.getPacketSize() == (int)sizeof(p0));
		memset(buf, 0, sizeof(buf));
		in.readPacket(0, (unsigned char *)buf);
		CHECK(strcmp(buf, p0) == 0);

		in.close();
	}

	remove(path.c_str());
}

TEST_CASE("File resolves and reads a loose disk file (base-loose branch)")
{
	File f;
	const std::string path = fixturePath("harness_smoke.fit");
	REQUIRE(f.open(path.c_str()) == NO_ERR);
	CHECK(f.getLength() > 0);

	char header[8] = {0};
	CHECK(f.read((MemoryPtr)header, 6) == 6);
	CHECK(strncmp(header, "FITini", 6) == 0);

	f.close();
	CHECK(!f.isOpen());
}

} // TEST_SUITE("FileStack")

extern bool g_mc2TestStopFatal;   // tests/unit/heap_shim.cpp: STOP counts and continues instead of aborting
extern int  g_mc2TestStopCount;

namespace
{

// A .fit with a block named `id`, followed by [Second] { l Beta = 9 }.
std::string fitWithBlock (const std::string &id)
{
	return "FITini\r\n\r\n[" + id + "]\r\nl Alpha = 7\r\n\r\n[Second]\r\nl Beta = 9\r\n\r\nFITend\r\n";
}

} // namespace

TEST_CASE("FitIniFile block ids: the block-id copy is bounded for every header length")
{
	struct Case { size_t len; bool accepted; };
	const Case cases[] = {
		{47, true}, {49, true}, {300, false},
	};

	for (const Case &c : cases)
	{
		INFO("block id length " << c.len);
		g_mc2TestStopFatal = false;
		g_mc2TestStopCount = 0;

		const std::string id(c.len, 'b');
		std::string text = fitWithBlock(id);

		FitIniFile fit;
		CHECK((fit.open(text.data(), (int)text.size()) == NO_ERR) == c.accepted);
		CHECK(g_mc2TestStopCount == 0);
		if (!c.accepted) { g_mc2TestStopFatal = true; continue; }
		CHECK(fit.seekBlock(id.c_str()) == NO_ERR);

		long l = 0;
		REQUIRE(fit.seekBlock("Second") == NO_ERR);          // the block after the long one is intact
		CHECK(fit.readIdLong("Beta", l) == NO_ERR);
		CHECK(l == 9);
		fit.close();

		g_mc2TestStopFatal = true;
	}
}

TEST_CASE("PacketFile rejects a negative packet index instead of reading seekTable[-n]")
{
	const std::string path = scratchPath("negindex.pak");
	const char p0[] = "alpha";
	{
		PacketFile out;
		REQUIRE(out.create(path.c_str()) == NO_ERR);
		out.reserve(1);
		CHECK(out.writePacket(0, (MemoryPtr)p0, sizeof(p0)) == (int)sizeof(p0));
		out.close();
	}
	{
		PacketFile in;
		REQUIRE(in.open(path.c_str()) == NO_ERR);
		CHECK(in.readPacketOffset(-1) == -1);
		CHECK(in.readPacketOffset(-1000) == -1);
		CHECK(in.seekPacket(-1) == PACKET_OUT_OF_RANGE);
		CHECK(in.seekPacket(-7) == PACKET_OUT_OF_RANGE);
		REQUIRE(in.seekPacket(0) == NO_ERR);        // a valid index still works afterwards
		CHECK(in.getPacketSize() == (int)sizeof(p0));
		in.close();
	}
	remove(path.c_str());
}

TEST_CASE("MPJoin/FIT hostile strings arrays truncation and garbage stay bounded") {
    const std::string corpus[] = {
        "garbage", "FITini\n[", "FITini\n[" + std::string(300, 'x') + "]\nFITend\n",
        "FITini\n[B]\nl[999999999] Values = 1\nl End = 0\nFITend\n",
        "FITini\n[B]\n] Values = l[1\nl End = 0\nFITend\n",
        "FITini\n[B]\nst Name = \"" + std::string(2000, 'x') + "\n" + std::string(2000, 'y') + "\"\nl End = 0\nFITend\n"
    };
    for (const auto& text : corpus) {
        for (const auto size : {std::size_t{1}, text.size() / 2, text.size()}) {
            FitIniFile fit;
            if (fit.open(text.data(), static_cast<int>(size)) != NO_ERR || fit.seekBlock("B") != NO_ERR) continue;
            struct { char text[16]{}; unsigned guard = 0x12345678; } name;
            fit.readIdString("Name", name.text, sizeof(name.text));
            CHECK(name.guard == 0x12345678);
            struct { long values[2]{}; unsigned guard = 0x12345678; } array;
            CHECK(fit.readIdLongArray("Values", array.values, 2) != NO_ERR);
            CHECK(array.guard == 0x12345678);
        }
    }
}

TEST_CASE("MPJoin/match RAM reads cannot escape the supplied image") {
    const char bytes[] = {1, 2, 3};
    File ram;
    REQUIRE(ram.open(bytes, 3) == NO_ERR);
    unsigned char out[8]{};
    CHECK(ram.read(out, -1) == 0);
    CHECK(ram.read(100, out, 4) == 0);
    CHECK(ram.read(out, 8) == 3);
    CHECK(ram.readByte() == 0);
    CHECK(ram.readInt() == 0);
    CHECK(ram.getLogicalPosition() == 3);
    ram.seek(2);
    CHECK(ram.readInt() == 0);
    CHECK(ram.getLogicalPosition() == 3);
}

TEST_CASE("MPJoin/packet corpus rejects truncated tables offsets and oversized unpacked counts") {
    const std::vector<std::vector<unsigned>> corpus = {
        {PACKET_FILE_VERSION, 0xfffffffcu, 0},
        {PACKET_FILE_VERSION, 13, 12, 0},
        {PACKET_FILE_VERSION, 16, 16, 12, 0},
        {PACKET_FILE_VERSION, 12, 0x4000000c, 0x7fffffffu},
        {PACKET_FILE_VERSION, 12, 0x2000000c, 0x7fffffffu},
        {PACKET_FILE_VERSION, 12, 0x4000000c, 0xffffffffu},
        {PACKET_FILE_VERSION, 12, 0x4000000c},
        {0x12345678, 0x87654321, 0x12345678}
    };
    for (const auto& words : corpus) {
        std::vector<char> bytes;
        for (auto word : words) for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>(word >> shift));
        for (std::size_t n = 1; n <= bytes.size(); ++n) {
            File ram;
            REQUIRE(ram.open(bytes.data(), static_cast<int>(n)) == NO_ERR);
            PacketFile packet;
            const auto result = packet.open(&ram, static_cast<unsigned long>(n));
            if (result == NO_ERR) CHECK(packet.seekPacket(0) != NO_ERR);
        }
    }
}
