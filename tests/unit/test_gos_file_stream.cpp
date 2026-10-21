// V4: the GOS stream handle (gosFileStream) and its gos_* entry points.
// Disk-only: archive and mod resolution live above this layer, so this
// fixture pins the seek/read/close contract the layers above rely on.

#include "gameos.hpp"
#include "toolos.hpp"
#include "fileio.hpp"

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

class TempFile {
public:
    explicit TempFile(const std::string& contents)
    {
        static unsigned counter = 0;
        m_path = std::filesystem::temp_directory_path() /
                 ("mc2-gos-stream-" + std::to_string(++counter) + ".bin");
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    std::string path() const { return m_path.string(); }

private:
    std::filesystem::path m_path;
};

std::string readExactly(gosFileStream* stream, DWORD count)
{
    std::vector<char> buffer(count);
    const DWORD got = gos_ReadFile(stream, buffer.data(), count);
    REQUIRE(got == count);
    return std::string(buffer.data(), buffer.size());
}

} // namespace

TEST_CASE("gosFileStream: open, read, seek and close round-trip")
{
    TempFile file("0123456789");

    gosFileStream* stream = nullptr;
    gos_OpenFile(&stream, file.path().c_str(), READONLY);
    REQUIRE(stream != nullptr);

    CHECK(readExactly(stream, 4) == "0123");

    CHECK(stream->Seek(0, FROMSTART) == 0);
    CHECK(readExactly(stream, 10) == "0123456789");

    CHECK(stream->Seek(-3, FROMEND) == 0);
    CHECK(readExactly(stream, 3) == "789");

    CHECK(stream->Seek(0, FROMSTART) == 0);
    CHECK(stream->Seek(2, FROMCURRENT) == 0);
    CHECK(readExactly(stream, 2) == "23");

    gos_CloseFile(stream);
}

TEST_CASE("gos_FileSize and gos_FileExists agree with the stream")
{
    TempFile file("0123456789");

    CHECK(gos_FileSize(file.path().c_str()) == 10);
    CHECK(gos_FileExists(file.path().c_str()));
    CHECK_FALSE(gos_FileExists((file.path() + ".missing").c_str()));
}
