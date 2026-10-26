#include "doctest.h"
#include "platform_windows.h"
#include <filesystem>
#include <fstream>

TEST_CASE("platform/Win32 data widths and monotonic counter")
{
    CHECK(sizeof(DWORD) == 4);
    CHECK(sizeof(LONG) == 4);
    CHECK(sizeof(INT_PTR) == sizeof(void*));
    LARGE_INTEGER before{}, after{}, frequency{};
    REQUIRE(QueryPerformanceFrequency(&frequency));
    REQUIRE(QueryPerformanceCounter(&before));
    const auto milliseconds = GetTickCount64();
    Sleep(1);
    REQUIRE(QueryPerformanceCounter(&after));
    CHECK(frequency.QuadPart > 0);
    CHECK(after.QuadPart > before.QuadPart);
    CHECK(GetTickCount64() >= milliseconds);
}

TEST_CASE("platform/file handles and atomic replacement preserve bytes")
{
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("mc2-platform-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(dir);
    struct Cleanup { fs::path dir; ~Cleanup() { std::error_code ec; fs::remove_all(dir, ec); } } cleanup{dir};
    const auto source = dir / "source.bin", destination = dir / "Destination.bin";
    const auto sourceText = source.string();
    struct File {
        HANDLE value;
        ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
    {
        File file{CreateFileA(sourceText.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        REQUIRE(file.value != INVALID_HANDLE_VALUE);
        DWORD count = 0;
        REQUIRE(WriteFile(file.value, "hello", 5, &count, nullptr));
        CHECK(count == 5);
    }
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    REQUIRE(GetFileAttributesExA(sourceText.c_str(), GetFileExInfoStandard, &attributes));
    CHECK(attributes.nFileSizeLow == 5);
    CHECK(attributes.nFileSizeHigh == 0);
    { std::ofstream out(destination); out << "old"; }
    REQUIRE(MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    CHECK_FALSE(fs::exists(source));
    WIN32_FIND_DATAA found{};
    const auto search = FindFirstFileA((dir / "*.bin").string().c_str(), &found);
    REQUIRE(search != INVALID_HANDLE_VALUE);
    CHECK(found.nFileSizeLow == 5);
    CHECK(found.nFileSizeHigh == 0);
    FindClose(search);
    const auto alias = (dir / "destination.BIN").string();
    CHECK(GetFileAttributesA(alias.c_str()) != INVALID_FILE_ATTRIBUTES);
    File file{CreateFileA(alias.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(file.value != INVALID_HANDLE_VALUE);
    char bytes[16]{};
    DWORD count = 0;
    REQUIRE(ReadFile(file.value, bytes, sizeof(bytes), &count, nullptr));
    CHECK(count == 5);
    CHECK(std::string(bytes, count) == "hello");
    REQUIRE(ReadFile(file.value, bytes, sizeof(bytes), &count, nullptr));
    CHECK(count == 0);
}
