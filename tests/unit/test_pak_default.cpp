// Pak-default carry: digest memo, registered-but-missing FST is absent, the two direct FST probes learn the pak.
#include "doctest.h"
#include "pak_zip.h"
#include "vfs.h"
#include "file.h"
#include "ffile.h"
#include "fastfile.h"
#include "platform_windows.h"
#include <filesystem>
#include <cstdlib>
#include <chrono>
#ifdef _WIN32
#include <process.h>
#define PAKDEF_PID _getpid()
#else
#include <unistd.h>
#define PAKDEF_PID getpid()
#endif
namespace fs = std::filesystem;
namespace {
const fs::path fixtures = fs::path(MC2_REPO_ROOT) / "tests/fixtures/pak";
const char* const kKey = "data/pak/store.bin";
struct Scratch {
    fs::path old = fs::current_path(), root = fs::temp_directory_path() / ("mc2pakdef-unit-" + std::to_string(PAKDEF_PID) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    explicit Scratch(bool firstPresent = true) {
        { std::error_code ec; fs::remove_all(root, ec); }
        fs::create_directories(root / "paks"); fs::current_path(root);
        maxFastFiles = 16; numFastFiles = 0; fastFiles = static_cast<FastFile**>(std::calloc(16, sizeof(FastFile*)));
        if (firstPresent) fs::copy_file(fixtures / "source-fst.bin", "first.fst");
        fs::copy_file(fixtures / "source-fst.bin", "second.fst");
        fs::copy_file(fixtures / "stock.mc2pak", "paks/stock.mc2pak");
        FastFileInit("first.fst"); FastFileInit("second.fst");   // first.fst may be registered while missing on disk
        mc2::vfs::rebuild();
    }
    ~Scratch() { FastFileFini(); fs::current_path(old); mc2::vfs::rebuild(); std::error_code ec; fs::remove_all(root, ec); }
};
size_t pakCandidates(const char* key) {
    size_t n = 0;
    for (const auto& c : mc2::vfs::candidates(key)) n += c.provider == mc2::vfs::Provider::Pak;
    return n;
}
}
TEST_CASE("pakdefault/digest of a pak winner is memoized by the pak stamp") {
    Scratch s;
    REQUIRE(mc2::vfs::candidates(kKey)[0].provider == mc2::vfs::Provider::Pak);
    const auto first = mc2::vfs::contentDigest(kKey);
    REQUIRE(first != 0);
    const auto opens = mc2::vfs::pakIo.pakOpens.load();
    CHECK(mc2::vfs::contentDigest(kKey) == first);
    CHECK(mc2::vfs::pakIo.pakOpens.load() == opens);          // the hit decoded nothing
    CHECK(mc2::vfs::contentDigest(kKey, true) == first);      // verify bypasses the memo
    CHECK(mc2::vfs::pakIo.pakOpens.load() == opens + 1);
    // A changed pak stamp (mtime) invalidates the memo: the digest is recomputed from the served bytes.
    fs::last_write_time("paks/stock.mc2pak", fs::last_write_time("paks/stock.mc2pak") + std::chrono::seconds(7));
    CHECK(mc2::vfs::contentDigest(kKey) == first);
    CHECK(mc2::vfs::pakIo.pakOpens.load() == opens + 2);
}
TEST_CASE("pakdefault/a registered FST missing on disk is absent, not modified") {
    Scratch s(false);
    CHECK(pakCandidates(kKey) == 1);   // before: state=modified skipped every entry of the source
}
TEST_CASE("pakdefault/LookupArchivePath names the source FST, never the pak") {
    { Scratch s;
      const auto path = LookupArchivePath(kKey);
      CHECK(fs::path(path).filename() == "first.fst");
      CHECK(path.find("mc2pak") == std::string::npos); }
    { Scratch s(false);   // pak sits below every registered FST when its source is absent: second.fst is the real winner
      CHECK(fs::path(LookupArchivePath(kKey)).filename() == "second.fst"); }
}
TEST_CASE("pakdefault/an extra root FST ranked ahead of the stock pak answers both probes") {
    Scratch s;
    fs::copy_file(fixtures / "source-fst.bin", "extra.fst");
    FastFileFini(); maxFastFiles = 16; numFastFiles = 0; fastFiles = static_cast<FastFile**>(std::calloc(16, sizeof(FastFile*)));
    FastFileInit("extra.fst"); FastFileInit("first.fst"); FastFileInit("second.fst"); mc2::vfs::rebuild();   // extra root FST registered ahead of the stock source
    REQUIRE(mc2::vfs::candidates(kKey)[0].provider == mc2::vfs::Provider::Fst);
    CHECK(fs::path(LookupArchivePath(kKey)).filename() == "extra.fst");   // before: the pak's source, first.fst
    const auto opens = mc2::vfs::pakIo.pakOpens.load();
    CHECK(ReadArchiveBytes(kKey, 1024).size() == 3);
    CHECK(mc2::vfs::pakIo.pakOpens.load() == opens);                      // served by the FST, not the pak
}
TEST_CASE("pakdefault/ReadArchiveBytes returns the pak bytes") {
    Scratch s;
    const auto opens = mc2::vfs::pakIo.pakOpens.load();
    const auto bytes = ReadArchiveBytes(kKey, 1024);
    CHECK(std::string(bytes.begin(), bytes.end()) == "abc");
    CHECK(mc2::vfs::pakIo.pakOpens.load() == opens + 1);
    CHECK(ReadArchiveBytes(kKey, 2).empty());                 // over the caller's cap
}
TEST_CASE("pakdefault/a pak-backed File cannot be a parent") {
    Scratch s;
    File parent; REQUIRE(parent.open(kKey) == NO_ERR);
    File child; CHECK(child.open(&parent, 1, -1) != NO_ERR);
}
