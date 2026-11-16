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
    REQUIRE(first.seek(-1, SEEK_CUR) == NO_ERR);
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
}

TEST_CASE("carbon_mclib/short paths append a longer extension once")
{
    FullPathFileName shortName("", "a", ".fit"), suffixed("", "a.fit", ".fit");
    CHECK(std::strcmp(shortName, "a.fit") == 0);
    CHECK(std::strcmp(suffixed, "a.fit") == 0);
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
	// blockId is char[50]. The engine only LOGS the 'BlockId To large' STOP and carries on; the shim does the
	// same here. What must hold for any length: no write past the node, and the next block still seeks.
	struct Case { size_t len; bool seekable_by_full_name; bool stops; };
	const Case cases[] = {
		{47, true, false},    // longest accepted without a STOP
		{49, true, true},     // fills blockId[0..48] + NUL (the old code stored it whole); STOP logged
		{300, false, true},   // mod-sized hostile name: used to overwrite ~250 bytes of heap; kept as its first 49
	};

	for (const Case &c : cases)
	{
		INFO("block id length " << c.len);
		g_mc2TestStopFatal = false;
		g_mc2TestStopCount = 0;

		const std::string id(c.len, 'b');
		std::string text = fitWithBlock(id);

		FitIniFile fit;
		REQUIRE(fit.open(text.data(), (int)text.size()) == NO_ERR);

		CHECK((g_mc2TestStopCount >= 1) == c.stops);          // the guard fired exactly for the over-long names
		CHECK((fit.seekBlock(id.c_str()) == NO_ERR) == c.seekable_by_full_name);

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
