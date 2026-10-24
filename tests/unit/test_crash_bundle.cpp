#include "doctest.h"
#ifdef _WIN32
#include "../../GameOS/gameos/crash_content_writer.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <fcntl.h>

TEST_CASE("crash-bundle/writer preserves identities and snapshots without the live source") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("mc2-crash-test-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(dir / "bundle");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{dir};
    const auto journal = dir / "journal.jsonl";
    // A legal Windows path; hostile logical names still must survive JSON encoding.
    const auto input = dir / "input.bin";
    { std::ofstream out(input, std::ios::binary); out << "hello"; }
    {
        mc2::crash::ContentWriter writer;
        REQUIRE(writer.init(journal.string().c_str()));
        writer.mod("pack\"\\\n", 2);
        writer.savePoint("load_begin", "save\\slot\"1.fit");
        writer.image("bad\"\\\n.fit", "hello", 5);
        writer.image("bad\"\\\n.fit", "hello", 5); // repeat content is deduplicated
        int fd = _open(input.string().c_str(), _O_RDONLY | _O_BINARY);
        REQUIRE(fd >= 0);
        REQUIRE(_lseek(fd, 2, SEEK_SET) == 2);
        writer.fd(fd, "data/input.bin", "base");
        CHECK(_tell(fd) == 2);
        _close(fd);
        fs::remove(input); // snapshot must use the captured bytes, not reopen the content
        REQUIRE(writer.snapshot((dir / "bundle").string().c_str()));
        CHECK_FALSE(writer.snapshot((dir / "missing" / "bundle").string().c_str()));
    }
    std::ifstream in(dir / "bundle" / "content.jsonl", std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    CHECK(text.find("fnv1a64") != std::string::npos);
    CHECK(text.find("a430d84680aabd0b") != std::string::npos); // known FNV-1a vector
    CHECK(text.find("pack\\\"\\\\\\u000a") != std::string::npos);
    CHECK(text.find("save\\\\slot\\\"1.fit") != std::string::npos);
    const auto first = text.find("fst-member");
    CHECK(first != std::string::npos);
    CHECK(text.find("fst-member", first + 1) == std::string::npos);
    CHECK(text.find("data/input.bin") != std::string::npos);
    CHECK(mc2::crash::hash("hellp", 5) != mc2::crash::hash("hello", 5));
}

TEST_CASE("crash-bundle/profile scrub stops at a path separator") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("mc2-crash-scrub-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(dir);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{dir};
    SetEnvironmentVariableA("USERPROFILE", "C:\\Users\\Joe");
    {
        mc2::crash::ContentWriter writer;
        REQUIRE(writer.init((dir / "j.jsonl").string()));
        writer.savePoint("a", "C:\\Users\\Joe\\save.fit");
        writer.savePoint("b", "C:\\Users\\Joel\\save.fit");
    }
    std::ifstream in(dir / "j.jsonl", std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    CHECK(text.find("%USERPROFILE%\\\\save.fit") != std::string::npos);
    CHECK(text.find("C:\\\\Users\\\\Joel\\\\save.fit") != std::string::npos);
}
#endif
