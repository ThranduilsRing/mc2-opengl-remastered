// tests/unit/test_file_family_contract.cpp
//
// What the File family promises to its callers, pinned before the conversion that moved the name, the child list, the in-RAM image and the seek
// tables into standard containers. Every case here passes on the old classes and must keep passing, unchanged, after the conversion; the cases that
// need NEW behaviour (malformed packet headers, an empty fit file) arrive with the commit that makes them true.
//
// WHAT each case pins and WHY a conversion could break it:
//   FC1 the name is null while closed and the opened path while open (mech_loadout.cpp tests getFilename() for null).
//   FC2 a child reads its parent's bytes at the parent's position, reports the parent's name, and is closed by the parent's close (the owner of a
//       packet file closes it without closing the fit views it opened on its packets first).
//   FC3 closing a file opened on a caller's buffer never frees or alters that buffer (the in-RAM image is owned only for FastFile and child files).
//   FC4 a closed file can be opened again and reports the new name (a loader object is reused across missions).
//   FC5 many children: the child list holds the default 50 and a child that closes itself leaves its slot free for the next one.

#include "doctest.h"
#include "Gates/gate_name.h"

#include "file.h"
#include "inifile.h"
#include "packet.h"
#include "ablscan.h"

#include "../../GameOS/gameos/utils/mc2log.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <climits>
#include <type_traits>

extern bool g_mc2TestStopFatal;   // tests/unit/heap_shim.cpp: STOP counts and continues instead of aborting
extern int  g_mc2TestStopCount;

namespace
{

// Lowercase on purpose: File::open lowercases a path before it reaches the OS.
std::string scratch (const char *name)
{
	return std::string("ffc_scratch_") + name;
}

void writeBytes (const std::string &path, const char *bytes, size_t n)
{
	File out;
	REQUIRE(out.create(path.c_str()) == NO_ERR);
	REQUIRE(out.write((MemoryPtr)bytes, n) == (long)n);
	out.close();
}

} // namespace

TEST_SUITE("FileFamilyContract")
{

TEST_CASE("FC1 the name is null while closed and the opened path while open")
{
	const std::string path = scratch("name.bin");
	writeBytes(path, "0123456789", 10);

	File f;
	CHECK(f.getFilename() == nullptr);
	REQUIRE(f.open(path.c_str()) == NO_ERR);
	REQUIRE(f.getFilename() != nullptr);
	CHECK(std::string(f.getFilename()) == path);
	CHECK(f.getLength() == 10);
	f.close();
	CHECK(f.getFilename() == nullptr);
	CHECK(!f.isOpen());

	remove(path.c_str());
}

TEST_CASE("FC2 a child reads its parent's bytes, shares its name and is closed with it")
{
	const std::string path = scratch("child.bin");
	writeBytes(path, "ABCDEFGHIJ", 10);

	File parent;
	REQUIRE(parent.open(path.c_str()) == NO_ERR);
	parent.seek(2);

	File child;
	REQUIRE(child.open(&parent, 4) == NO_ERR);
	CHECK(child.getParent() == &parent);
	REQUIRE(child.getFilename() != nullptr);
	CHECK(std::string(child.getFilename()) == path);

	unsigned char got[5] = {0};
	CHECK(child.read(got, 4) == 4);
	CHECK(memcmp(got, "CDEF", 4) == 0);

	// Closing the parent first closes every child that is still open.
	parent.close();
	CHECK(!child.isOpen());
	child.close();

	remove(path.c_str());
}

TEST_CASE("FC3 closing a file opened on a caller's buffer leaves the buffer alone")
{
	char buffer[] = "caller-owned bytes";
	const size_t n = sizeof(buffer) - 1;

	File f;
	REQUIRE(f.open(buffer, (int)n) == NO_ERR);

	char got[8] = {0};
	CHECK(f.read((MemoryPtr)got, 6) == 6);
	CHECK(memcmp(got, "caller", 6) == 0);
	f.close();

	CHECK(std::string(buffer) == "caller-owned bytes");
}

// FC6 byte views: a borrowed read-only view cannot be written (compile time: its span holds const bytes; run time: File::write fails and the
// caller's memory is untouched); a borrowed writable view is written in place and never grown.
static_assert(!std::is_assignable_v<File::ReadOnlyBytes::reference, unsigned char>, "a read-only byte view must not be writable");
static_assert(std::is_assignable_v<File::WritableBytes::reference, unsigned char>);
static_assert(!std::is_constructible_v<File::WritableBytes, const unsigned char*, size_t>, "const memory must not become a writable view");

TEST_CASE("FC6 read-only borrow rejects writes; writable borrow writes in place")
{
	const unsigned char ro[] = { 'a', 'b', 'c', 'd' };
	File r;
	REQUIRE(r.openReadOnly(File::ReadOnlyBytes(ro)) == NO_ERR);
	CHECK(r.getFileMode() == READ);
	unsigned char x = 'Z';
	CHECK(r.write(&x, 1) == static_cast<long>(BAD_WRITE_ERR));
	CHECK(r.readByte() == 'a');
	r.close();
	CHECK(memcmp(ro, "abcd", 4) == 0);

	// open(buffer, length) is the read-only borrow.
	const char legacy[] = "wxyz";
	File l;
	REQUIRE(l.open(legacy, 4) == NO_ERR);
	CHECK(l.write(&x, 1) == static_cast<long>(BAD_WRITE_ERR));
	l.close();
	CHECK(std::string(legacy) == "wxyz");

	unsigned char rw[] = { '1', '2', '3' };
	File w;
	REQUIRE(w.openWritable(File::WritableBytes(rw)) == NO_ERR);
	CHECK(w.write(&x, 1) == 1);
	CHECK(w.writeByte('Y') == NO_ERR);
	CHECK(w.writeByte('Q') == NO_ERR);
	CHECK(w.writeByte('!') == static_cast<long>(BAD_WRITE_ERR));   // never grown past the view
	w.close();
	CHECK(memcmp(rw, "ZYQ", 3) == 0);

	File e;
	CHECK(e.openReadOnly({}) == static_cast<long>(FILE_NOT_OPEN));
	CHECK(e.openWritable({}) == static_cast<long>(FILE_NOT_OPEN));
}

TEST_CASE("FC4 a closed file opens again and reports the new name")
{
	const std::string a = scratch("again_a.bin");
	const std::string b = scratch("again_b.bin");
	writeBytes(a, "aaaa", 4);
	writeBytes(b, "bbbbbbbb", 8);

	File f;
	REQUIRE(f.open(a.c_str()) == NO_ERR);
	CHECK(std::string(f.getFilename()) == a);
	f.close();
	REQUIRE(f.open(b.c_str()) == NO_ERR);
	CHECK(std::string(f.getFilename()) == b);
	CHECK(f.getLength() == 8);
	f.close();

	remove(a.c_str());
	remove(b.c_str());
}

TEST_CASE("FC5 a parent holds its default 50 children and a closed child frees its slot")
{
	const std::string path = scratch("many.bin");
	writeBytes(path, "0123456789", 10);

	File parent;
	REQUIRE(parent.open(path.c_str()) == NO_ERR);

	File children[51];
	for (int i = 0; i < 50; i++)
		REQUIRE(children[i].open(&parent, 1) == NO_ERR);
	// The 51st child does not fit.
	CHECK(children[50].open(&parent, 1) != NO_ERR);

	children[7].close();
	CHECK(children[50].open(&parent, 1) == NO_ERR);

	parent.close();
	for (File &c : children)
		c.close();

	remove(path.c_str());
}

// ---- Behaviour that is NEW with the standard-container internals (they fail on the old classes) ----

TEST_CASE("FC6 a child closed after its parent was destroyed does not touch the parent")
{
	const std::string path = scratch("orphan.bin");
	writeBytes(path, "0123456789", 10);

	File *parent = new File;
	REQUIRE(parent->open(path.c_str()) == NO_ERR);
	File child;
	REQUIRE(child.open(parent, 4) == NO_ERR);

	// The parent closes (and so closes the child) and is destroyed; the child's own close, then its destructor, run later.
	parent->close();
	delete parent;
	child.close();

	remove(path.c_str());
}

TEST_CASE("FC7 a packet file whose header claims more packets than the file holds is rejected")
{
	const std::string path = scratch("lying.pak");
	// version, first-packet offset 0x7FFFFFF8 (claims about 536 million packets), and 8 more bytes: 16 bytes in all.
	const unsigned char bytes[16] = {0xCE, 0xFA, 0xED, 0xFE, 0xF8, 0xFF, 0xFF, 0x7F, 0, 0, 0, 0, 0, 0, 0, 0};
	writeBytes(path, (const char *)bytes, sizeof(bytes));

	// The rejection is one [PACKET_HEADER_BAD] log line per process and never a STOP: malformed content is rejected with a log line, never a crash.
	g_mc2TestStopFatal = false;
	g_mc2TestStopCount = 0;
	std::FILE *capture = std::tmpfile();
	REQUIRE(capture != nullptr);
	mc2::log::setSpec("*:info");
	mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, capture);

	PacketFile in;
	CHECK(in.open(path.c_str()) == PACKET_OUT_OF_RANGE);
	CHECK(g_mc2TestStopCount == 0);
	CHECK(in.getNumPackets() == 0);
	in.close();

	// A negative offset gives a negative count.
	const unsigned char negative[16] = {0xCE, 0xFA, 0xED, 0xFE, 0xF0, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0};
	writeBytes(path, (const char *)negative, sizeof(negative));
	PacketFile in2;
	CHECK(in2.open(path.c_str()) == PACKET_OUT_OF_RANGE);
	CHECK(in2.getNumPackets() == 0);
	in2.close();
	CHECK(g_mc2TestStopCount == 0);

	std::fflush(capture);
	std::rewind(capture);
	std::string logged;
	char buf[512];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, capture)) > 0) logged.append(buf, n);
	mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, nullptr);
	std::fclose(capture);
	const size_t first = logged.find("[PACKET_HEADER_BAD]");
	CHECK(first != std::string::npos);
	CHECK(logged.find("[PACKET_HEADER_BAD]", first + 1) == std::string::npos);   // one line per process, not one per bad file
	g_mc2TestStopFatal = true;

	remove(path.c_str());
}

TEST_CASE("FC8 a fit file with no blocks opens and finds no block")
{
	const char text[] = "FITini\r\n\r\nFITend\r\n";
	FitIniFile fit;
	REQUIRE(fit.open(text, (int)(sizeof(text) - 1)) == NO_ERR);
	CHECK(fit.seekBlock("Anything") != NO_ERR);
	fit.close();
}

} // TEST_SUITE

namespace {
struct AblMemory {
	unsigned char bytes[4] = {42, 0, 0, 0};
	long length = 4;
	long position = 0;
	int reads = 0;
	int closes = 0;
};

struct AblCallbacks {
	decltype(ABLFile::remainingCB) remaining = ABLFile::remainingCB;
	decltype(ABLFile::readCB) read = ABLFile::readCB;
	decltype(ABLFile::openCB) open = ABLFile::openCB;
	decltype(ABLFile::closeCB) close = ABLFile::closeCB;

	AblCallbacks() {
		ABLFile::remainingCB = [](void* h) { auto& m = *static_cast<AblMemory*>(h); return m.length - m.position; };
		ABLFile::readCB = [](void* h, unsigned char* out, long n) {
			auto& m = *static_cast<AblMemory*>(h);
			++m.reads;
			memcpy(out, m.bytes + m.position, n);
			m.position += n;
			return n;
		};
		ABLFile::openCB = [](void** h, const char*) { *h = nullptr; return -7L; };
		ABLFile::closeCB = [](void** h) { ++static_cast<AblMemory*>(*h)->closes; *h = nullptr; return 0L; };
	}
	~AblCallbacks() {
		ABLFile::remainingCB = remaining; ABLFile::readCB = read;
		ABLFile::openCB = open; ABLFile::closeCB = close;
	}
};

struct AblRamCallbacks {
	decltype(ABLFile::remainingCB) remaining = ABLFile::remainingCB;
	decltype(ABLFile::readCB) read = ABLFile::readCB;
	decltype(ABLFile::readStringCB) string = ABLFile::readStringCB;
	decltype(ABLFile::readLineExCB) line = ABLFile::readLineExCB;
	decltype(ABLFile::closeCB) close = ABLFile::closeCB;
	AblRamCallbacks() {
		ABLFile::remainingCB = [](void* h) -> long { auto& f = *static_cast<File*>(h); return f.getLength() - f.getLogicalPosition(); };
		ABLFile::readCB = [](void* h, unsigned char* out, long n) { return static_cast<File*>(h)->read(out, n); };
		ABLFile::readStringCB = [](void* h, unsigned char* out) { return static_cast<File*>(h)->readString(out); };
		ABLFile::readLineExCB = [](void* h, unsigned char* out, long n) { return static_cast<File*>(h)->readLineEx(out, n); };
		ABLFile::closeCB = [](void**) { return 0L; };
	}
	~AblRamCallbacks() {
		ABLFile::remainingCB = remaining; ABLFile::readCB = read;
		ABLFile::readStringCB = string; ABLFile::readLineExCB = line; ABLFile::closeCB = close;
	}
};

struct AblReadLog {
	std::FILE* capture = std::tmpfile();
	AblReadLog() {
		REQUIRE(capture != nullptr);
		mc2::log::setSpec("*:info");
		mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, capture);
	}
	~AblReadLog() {
		mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, nullptr);
		std::fclose(capture);
	}
	void checkOne(const char* tag) {
		std::rewind(capture);
		std::string logged;
		char line[512];
		while (std::fgets(line, sizeof(line), capture)) logged += line;
		const auto first = logged.find(tag);
		REQUIRE(first != std::string::npos);
		CHECK(logged.find(tag, first + 1) == std::string::npos);
	}
};
}

TEST_CASE("FileFamilyContract/ABL final line stops at the RAM image boundary") {
	AblRamCallbacks callbacks;
	char image[] = "first\nlastX\n";
	File ram;
	REQUIRE(ram.open(image, 10) == NO_ERR);
	ABLFile file;
	file.set(&ram);
	unsigned char out[32] = {};
	CHECK(file.readLineEx(out, sizeof(out)) == 7);
	CHECK(std::string(reinterpret_cast<char*>(out)) == "first\n");
	CHECK(file.readLineEx(out, sizeof(out)) == 5);
	CHECK(std::string(reinterpret_cast<char*>(out)) == "last");
	CHECK(ram.getLogicalPosition() == 10);
}

TEST_CASE("FileFamilyContract/ABL overlong line cannot overwrite its destination") {
	AblRamCallbacks callbacks;
	AblReadLog log;
	std::string image(MAXLEN_SOURCELINE, 'a');
	image += "\n";
	File ram;
	REQUIRE(ram.open(image.data(), static_cast<int>(image.size())) == NO_ERR);
	ABLFile file;
	file.set(&ram);
	unsigned char out[MAXLEN_SOURCELINE + 2] = {};
	out[MAXLEN_SOURCELINE] = 0x5a;
	out[MAXLEN_SOURCELINE + 1] = 0x5a;
	CHECK(file.readLineEx(out, MAXLEN_SOURCELINE) == 0);
	CHECK(out[MAXLEN_SOURCELINE] == 0x5a);
	CHECK(out[MAXLEN_SOURCELINE + 1] == 0x5a);
	log.checkOne("[ABL_FILE_LINE_BAD]");
	image.resize(MAXLEN_SOURCELINE - 1);
	ram.close();
	REQUIRE(ram.open(image.data(), static_cast<int>(image.size())) == NO_ERR);
	CHECK(file.readLineEx(out, MAXLEN_SOURCELINE) == MAXLEN_SOURCELINE);
	CHECK(std::string(reinterpret_cast<char*>(out)) == image);
}

TEST_CASE("FileFamilyContract/ABL oversized or unterminated string is rejected") {
	AblRamCallbacks callbacks;
	for (const bool terminated : {true, false}) {
		AblReadLog log;
		std::string image(terminated ? 1024 : 3, 'a');
		image += '\0';
		File ram;
		REQUIRE(ram.open(image.data(), static_cast<int>(image.size() - (terminated ? 0 : 1))) == NO_ERR);
		ABLFile file;
		file.set(&ram);
		unsigned char out[1026] = {};
		out[1024] = 0x5a;
		CHECK(file.readString(out, 1024) == -1);
		CHECK(out[1024] == 0x5a);
		CHECK(ram.getLogicalPosition() <= ram.getLength());
		log.checkOne("[ABL_FILE_STRING_BAD]");
	}
	std::string image(1023, 'a');
	image += '\0';
	File ram;
	REQUIRE(ram.open(image.data(), static_cast<int>(image.size())) == NO_ERR);
	ABLFile file;
	file.set(&ram);
	unsigned char out[1024] = {};
	CHECK(file.readString(out, sizeof(out)) == 1023);
	CHECK(std::memcmp(out, image.data(), image.size()) == 0);
}

TEST_CASE("FileFamilyContract/ABL truncation and oversized reads never reach the unchecked callback") {
	AblCallbacks callbacks;
	AblMemory complete, truncated;
	truncated.length = 3;
	std::FILE* capture = std::tmpfile();
	REQUIRE(capture != nullptr);
	mc2::log::setSpec("*:info");
	mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, capture);
	const int stops = g_mc2TestStopCount;
	{
		ABLFile valid, bad, missing;
		valid.set(&complete);
		CHECK(valid.readInt() == 42);
		CHECK(complete.reads == 1);
		bad.set(&truncated);
		CHECK(bad.readInt() == -1);
		unsigned char guard[4] = {1, 2, 3, 4};
		CHECK(bad.read(guard, LONG_MAX) == 0);
		CHECK(truncated.reads == 0);
		CHECK(guard[0] == 1);
		CHECK(guard[3] == 4);
		CHECK(missing.open("missing.abl") == -7);
		CHECK(missing.get() == nullptr);
	}
	CHECK(complete.closes == 1);
	CHECK(truncated.closes == 1);
	CHECK(g_mc2TestStopCount == stops);
	mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, nullptr);
	std::rewind(capture);
	std::string logged;
	char line[512];
	while (std::fgets(line, sizeof(line), capture)) logged += line;
	std::fclose(capture);
	const auto first = logged.find("[ABL_FILE_READ_BAD]");
	REQUIRE(first != std::string::npos);
	const auto second = logged.find("[ABL_FILE_READ_BAD]", first + 1);
	REQUIRE(second != std::string::npos);
	CHECK(logged.find("[ABL_FILE_READ_BAD]", second + 1) == std::string::npos);
	CHECK(logged.find("[ABL_FILE_OPEN_BAD]") != std::string::npos);
}

// FC9..FC11 -- V1 resolver conformance corpus (tests/fixtures/vfs_resolution.json).
//
// One specification drives three harnesses: this engine ladder, the editor
// resolver (editor/tests/test_catalog.cpp) and the static resolver
// (tests/mod_install/test_vfs_resolution_corpus.py).  Each materializes the
// same tree, mounts with the same config and must agree on the PROVIDER and on
// the BYTES every request resolves to; "it exists" is not evidence.
//
// This arm runs the real ladder: InitModSearchPaths reads MC2_ACTIVE_MOD /
// MC2_MOD_DEPS and the mod.json dependencies, File::open walks mod index ->
// loose disk -> FastFileFind, and FastFileInit/FastFileFind/readFast supply
// the archive members (decoded), so the expected bytes are what the game hands
// a caller.  The editor arm instead reads system.cfg's FastFiles list, and the
// python arm reads <archive>.fst.txt sidecars.
//
// Rows the editor resolver cannot express stay out of the shared fixture (it
// has no numeric-size-subdir strip and no loose-over-archive step): those are
// pinned by test_paths_mod.cpp, test_fst_roundtrip.cpp,
// editor/tests/test_catalog.cpp and tests/mod_install/test_resolver.py.

#include "doctest.h"

#include "file.h"
#include "ffile.h"
#include "fastfile.h"
#include "fst_hash.h"

#include "../../tools/mc2_modlint/modlint_input.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef MC2_REPO_ROOT
#error "MC2_REPO_ROOT must be defined by tests/unit/CMakeLists.txt"
#endif

namespace
{

namespace fs = std::filesystem;

const MlJson *need (const MlJson &object, const char *key)
{
	const MlJson *value = object.find(key);
	REQUIRE(value != nullptr);
	return value;
}

void putEnv (const char *name, const std::string &value) { _putenv_s(name, value.c_str()); }

// Environment restored on every exit path (the ladder reads it on each mount).
struct EnvRestore
{
	const char *name;
	std::string saved;
	bool had;
	explicit EnvRestore (const char *n) : name(n)
	{
		const char *value = getenv(n);
		had = value != nullptr;
		saved = had ? value : "";
	}
	~EnvRestore () { _putenv_s(name, had ? saved.c_str() : ""); }
	EnvRestore (const EnvRestore &) = delete;
	EnvRestore &operator= (const EnvRestore &) = delete;
};

struct CwdRestore
{
	fs::path saved;
	CwdRestore () : saved(fs::current_path()) {}
	~CwdRestore ()
	{
		std::error_code ignored;
		fs::current_path(saved, ignored);
	}
	CwdRestore (const CwdRestore &) = delete;
	CwdRestore &operator= (const CwdRestore &) = delete;
};

void writeWholeFile (const fs::path &inputPath, const std::string &bytes)
{
    fs::path path = inputPath;
#ifndef _WIN32
    // Materialize the corpus as Windows does: Data/ and data/ share a directory.
    path = mc2_native_path(path.string().c_str());
#endif
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	REQUIRE(out.good());
	out.write(bytes.data(), (std::streamsize)bytes.size());
	REQUIRE(out.good());
}

void setWord (std::string &bytes, size_t at, unsigned value)
{
	for (unsigned i = 0; i < 4; ++i) bytes[at + i] = (char)(value >> (8 * i));
}

// A stock-shaped archive: zlib entries in the fixed 266-byte table (what
// FastFile::writeFast writes with compressed=true), names stored as specified.
void writeFst (const fs::path &path, const std::vector<std::pair<std::string, std::string>> &entries)
{
	std::string archive(8 + 266 * entries.size(), '\0');
	setWord(archive, 0, FASTFILE_VERSION);
	setWord(archive, 4, (unsigned)entries.size());
	unsigned offset = (unsigned)archive.size();
	for (size_t i = 0; i < entries.size(); ++i) {
		const std::string &name = entries[i].first;
		const std::string &plain = entries[i].second;
		REQUIRE(name.size() < MAX_FILENAME_SIZE);
		uLongf size = compressBound((uLong)plain.size());
		std::string encoded(size, '\0');
		REQUIRE(compress2((Bytef *)encoded.data(), &size, (const Bytef *)plain.data(),
			(uLong)plain.size(), 6) == Z_OK);
		encoded.resize(size);
		setWord(archive, 8 + i * 266, offset);
		setWord(archive, 8 + i * 266 + 4, (unsigned)encoded.size());
		setWord(archive, 8 + i * 266 + 8, (unsigned)plain.size());
		setWord(archive, 8 + i * 266 + 12, (unsigned)elfHash(name.c_str()));
		std::memcpy(archive.data() + 8 + i * 266 + 16, name.data(), name.size());
		archive += encoded;
		offset += (unsigned)encoded.size();
	}
	writeWholeFile(path, archive);
}

// The fixture deploy, materialized under one CWD-relative scratch root: the
// ladder opens CWD-relative names, and no tempfile directory can be populated
// under the test sandbox.
struct Deploy
{
	fs::path root;
	std::vector<std::string> archives;

	explicit Deploy (const MlJson &fixture)
	{
		root = fs::absolute("vfs_resolution_scratch");
		std::error_code ec;
		fs::remove_all(root, ec);
		fs::create_directories(root, ec);
		REQUIRE(!ec);
		for (const auto &file : need(fixture, "files")->arr)
			writeWholeFile(root / need(file, "path")->s, need(file, "bytes")->s);
		for (const auto &archive : need(fixture, "fst_files")->arr) {
			std::vector<std::pair<std::string, std::string>> entries;
			for (const auto &entry : need(archive, "entries")->arr)
				entries.emplace_back(need(entry, "name")->s, need(entry, "bytes")->s);
			const fs::path path = root / need(archive, "path")->s;
			writeFst(path, entries);
			archives.push_back(path.string());
		}
	}
	~Deploy ()
	{
		std::error_code ec;
		fs::remove_all(root, ec);
	}
	Deploy (const Deploy &) = delete;
	Deploy &operator= (const Deploy &) = delete;
};

struct FstRegistry
{
	explicit FstRegistry (const std::vector<std::string> &archives)
	{
		maxFastFiles = (long)archives.size();
		numFastFiles = 0;
		fastFiles = (FastFile **)std::calloc(archives.size(), sizeof(FastFile *));
		REQUIRE(fastFiles != nullptr);
		for (const auto &archive : archives) REQUIRE(FastFileInit(archive.c_str()));
	}
	~FstRegistry () { FastFileFini(); }
	FstRegistry (const FstRegistry &) = delete;
	FstRegistry &operator= (const FstRegistry &) = delete;
};

struct Resolved
{
	bool opened = false;
	std::string bytes;
};

Resolved resolveWithLadder (const std::string &request)
{
	Resolved resolved;
	File file;
	if (file.open(request.c_str()) == NO_ERR) {
		resolved.opened = true;
		const unsigned long length = file.getLength();
		resolved.bytes.assign(length, '\0');
		if (length) REQUIRE(file.read((MemoryPtr)resolved.bytes.data(), (long)length) == (long)length);
	}
	file.close();
	return resolved;
}

struct Corpus
{
	MlJson fixture;
	std::string active, mods, deps;
	std::vector<const MlJson *> rows;

	explicit Corpus (const char *name)
	{
		std::ifstream input(fs::path(MC2_REPO_ROOT) / "tests/fixtures" / name);
		REQUIRE(input.good());
		std::ostringstream text;
		text << input.rdbuf();
		bool ok = false;
		fixture = parseModlintJson(text.str(), &ok);
		REQUIRE(ok);
		const MlJson &config = *need(fixture, "config");
		active = need(config, "active_mod")->s;
		for (const auto &dep : need(config, "dependencies")->arr)
			deps += (deps.empty() ? "" : ",") + dep.s;
		for (const auto &row : need(fixture, "requests")->arr) rows.push_back(&row);
	}

	void mount (const Deploy &deploy)
	{
		mods = (deploy.root / "mods").string();
		putEnv(mc2::gate::name<"MC2_ACTIVE_MOD">(),active);
		putEnv(mc2::gate::name<"MC2_MOD_DEPS">(),deps);
		putEnv(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">(),"1");
		InitModSearchPaths(mods.c_str());
	}
};

// The bytes the fixture's provider path actually holds on disk.
std::string readWholeFile (const fs::path &path)
{
	std::ifstream input(path, std::ios::binary);
	REQUIRE(input.good());
	std::ostringstream text;
	text << input.rdbuf();
	return text.str();
}

// Root-relative provider of a row: mods/<id>/data/... or data/... .
std::string expectedProvider (const std::string &layer, const std::string &request)
{
	const std::string key = fst_normalize_loose_key(request.c_str());
	if (layer.starts_with("mod:")) {
		std::string id = layer.substr(4);
		if (id.ends_with("#scratch")) return "mods/" + id.substr(0, id.size() - 8) + "/.scratch/" + key;
		return "mods/" + id + "/" + key;
	}
	return key;
}

} // namespace

TEST_CASE("FC9 the resolver corpus hands back the provider and bytes it names")
{
	Corpus corpus("vfs_resolution.json");
	Deploy deploy(corpus.fixture);
	EnvRestore activeEnv(mc2::gate::name<"MC2_ACTIVE_MOD">()), depsEnv(mc2::gate::name<"MC2_MOD_DEPS">()), rebuildEnv(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">());
	CwdRestore cwd;
	fs::current_path(deploy.root);
	corpus.mount(deploy);
	FstRegistry archives(deploy.archives);   // fixture order == registration order

	for (const MlJson *row : corpus.rows) {
		const std::string request = need(*row, "req")->s;
		const std::string layer = need(*row, "layer")->s;
		const std::string path = need(*row, "path")->s;
		const MlJson *bytes = row->find("bytes");
		INFO("request " << request << " expects layer " << layer);

		const Resolved resolved = resolveWithLadder(request);
		CHECK(resolved.opened == (layer != "MISS"));
		if (layer == "MISS") {
			CHECK(resolved.bytes.empty());
			CHECK(fileExists(request.c_str()) == 0);
			continue;
		}
		// Every non-miss row names its content; only a miss may be null.
		REQUIRE(bytes != nullptr);
		REQUIRE(bytes->type == MlJson::Str);
		// Bytes first: the ladder must hand back the content the fixture names.
		CHECK(resolved.bytes == bytes->s);
		if (layer != "fastfile") {
			CHECK(fs::path(fst_normalize_loose_key(path.c_str())).lexically_normal() ==
				fs::path(expectedProvider(layer, request)));
		}
		if (layer == "fastfile") {
			// An archive member has no disk path: its decoded bytes above are
			// the provider check, and the probe must agree it is packed.
			CHECK(path.empty());
			CHECK(fileExists(request.c_str()) == FILE_ON_FST);
			continue;
		}
		// Loose provider: the probe reports disk, the named file exists under
		// the deploy root and holds exactly the bytes the ladder returned.
		CHECK(fileExists(request.c_str()) == FILE_ON_DISK);
		fs::path provider = deploy.root / path;
#ifndef _WIN32
        provider = mc2_native_path(provider.string().c_str());
#endif
		CHECK(fs::exists(provider));
		CHECK(readWholeFile(provider) == resolved.bytes);
	}
	putEnv(mc2::gate::name<"MC2_ACTIVE_MOD">(),"");
	putEnv(mc2::gate::name<"MC2_MOD_DEPS">(),"");
	InitModSearchPaths("");
}

TEST_CASE("FC10 the resolver corpus attributes each mod hit to the layer the fixture names")
{
	Corpus corpus("vfs_resolution.json");
	Deploy deploy(corpus.fixture);
	EnvRestore activeEnv(mc2::gate::name<"MC2_ACTIVE_MOD">()), depsEnv(mc2::gate::name<"MC2_MOD_DEPS">()), rebuildEnv(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">());
	CwdRestore cwd;
	fs::current_path(deploy.root);
	corpus.mount(deploy);

	for (const MlJson *row : corpus.rows) {
		const std::string request = need(*row, "req")->s;
		const std::string layer = need(*row, "layer")->s;
		INFO("request " << request);
		const char *owner = LookupModOwner(request.c_str());
		if (layer.starts_with("mod:")) {
			REQUIRE(owner != nullptr);
			CHECK(std::string(owner) == layer.substr(4));
		} else {
			// Dot-prefixed entries and unlisted mods are never indexed.
			CHECK(owner == nullptr);
		}
	}

	// Stock mode: the same requests now see no mod layer at all.
	putEnv(mc2::gate::name<"MC2_ACTIVE_MOD">(),"");
	putEnv(mc2::gate::name<"MC2_MOD_DEPS">(),"");
	InitModSearchPaths(corpus.mods.c_str());
	for (const MlJson *row : corpus.rows) {
		const std::string layer = need(*row, "layer")->s;
		if (layer.starts_with("mod:")) CHECK(LookupModOwner(need(*row, "req")->s.c_str()) == nullptr);
	}
	InitModSearchPaths("");
}

TEST_CASE("FC11 the resolver corpus does not depend on request order")
{
	Corpus corpus("vfs_resolution.json");
	Deploy deploy(corpus.fixture);
	EnvRestore activeEnv(mc2::gate::name<"MC2_ACTIVE_MOD">()), depsEnv(mc2::gate::name<"MC2_MOD_DEPS">()), rebuildEnv(mc2::gate::name<"MC2_REBUILD_MOD_CACHE">());
	CwdRestore cwd;
	fs::current_path(deploy.root);
	corpus.mount(deploy);
	FstRegistry archives(deploy.archives);

	for (auto it = corpus.rows.rbegin(); it != corpus.rows.rend(); ++it) {
		const std::string request = need(**it, "req")->s;
		const std::string layer = need(**it, "layer")->s;
		const MlJson *bytes = (*it)->find("bytes");
		INFO("request " << request << " expects layer " << layer);
		const Resolved resolved = resolveWithLadder(request);
		CHECK(resolved.opened == (layer != "MISS"));
		REQUIRE(bytes != nullptr);
		if (layer != "MISS") {
			REQUIRE(bytes->type == MlJson::Str);
			CHECK(resolved.bytes == bytes->s);
		}
	}
	putEnv(mc2::gate::name<"MC2_ACTIVE_MOD">(),"");
	putEnv(mc2::gate::name<"MC2_MOD_DEPS">(),"");
	InitModSearchPaths("");
}
