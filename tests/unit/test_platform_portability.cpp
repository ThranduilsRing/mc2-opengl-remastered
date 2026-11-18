#include "doctest.h"
#include "platform_windows.h"
#include <filesystem>
#include <fstream>
#include <type_traits>
#include "platform_str.h"
#include "../../Platform/FileSystem.cpp"

TEST_CASE("platform/user directory rejects an empty span") {
    CHECK_FALSE(mc2::platform::GetUserDataDirectory({}));
}

static_assert(std::is_same_v<PINT_PTR, INT_PTR*>);
static_assert(std::is_same_v<PUINT_PTR, UINT_PTR*>);
static_assert(std::is_same_v<PLONG_PTR, LONG_PTR*>);
static_assert(std::is_same_v<PULONG_PTR, ULONG_PTR*>);
static_assert(std::is_same_v<PULONG, ULONG*>);
static_assert(std::is_same_v<LPLONG, LONG*>);

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

#ifndef _WIN32
TEST_CASE("platform/native path preserves parents for new and ambiguous children")
{
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("mc2-case-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(dir / "Data/Saves");
    struct Cleanup { fs::path p; ~Cleanup() { fs::remove_all(p); } } cleanup{dir};
    std::ofstream(dir / "Data/Saves/Slot1.fit") << "upper";
    std::ofstream(dir / "Data/Saves/slot1.fit") << "lower";
    CHECK(mc2_native_path((dir / "data/saves/Slot1.fit").string()) == (dir / "Data/Saves/Slot1.fit").string());
    CHECK(mc2_native_path((dir / "data/saves/SLOT1.FIT").string()) == (dir / "Data/Saves/SLOT1.FIT").string());
    CHECK(mc2_native_path((dir / "data/saves/New.fit").string()) == (dir / "Data/Saves/New.fit").string());
    fs::create_directories(dir / "Data/Saves/Alias");
    fs::create_directories(dir / "Data/Saves/alias");
    CHECK(mc2_native_path((dir / "data/saves/ALIAS/New.fit").string()) == (dir / "Data/Saves/ALIAS/New.fit").string());
}

TEST_CASE("platform/file dispositions and failures report native errors")
{
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("mc2-disposition-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(dir);
    struct Cleanup { fs::path p; ~Cleanup() { fs::remove_all(p); } } cleanup{dir};
    const auto path = (dir / "save.fit").string();
    auto open = [&](DWORD disposition) {
        return CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    auto file = open(OPEN_ALWAYS);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    DWORD count = 0;
    REQUIRE(WriteFile(file, "hello", 5, &count, nullptr));
    REQUIRE(CloseHandle(file));
    CHECK(open(CREATE_NEW) == INVALID_HANDLE_VALUE);
    CHECK(GetLastError() == EEXIST);
    file = open(OPEN_ALWAYS);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    CHECK(fs::file_size(path) == 5);
    REQUIRE(CloseHandle(file));
    file = open(TRUNCATE_EXISTING);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    CHECK(fs::file_size(path) == 0);
    REQUIRE(CloseHandle(file));
    fs::remove(path);
    CHECK(open(TRUNCATE_EXISTING) == INVALID_HANDLE_VALUE);
    CHECK(GetLastError() == ENOENT);
    file = open(CREATE_NEW);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    REQUIRE(CloseHandle(file));
    char byte{};
    CHECK_FALSE(ReadFile(INVALID_HANDLE_VALUE, &byte, 1, &count, nullptr));
    CHECK(count == 0);
    CHECK(GetLastError() == EBADF);
    CHECK_FALSE(WriteFile(INVALID_HANDLE_VALUE, "x", 1, &count, nullptr));
    CHECK(count == 0);
    CHECK(GetLastError() == EBADF);
    CHECK_FALSE(MoveFileExW((dir / "missing").wstring().c_str(), (dir / "renamed").wstring().c_str(), 0));
    CHECK(GetLastError() == ENOENT);
    CHECK_FALSE(MoveFileExW((dir / "missing").wstring().c_str(), fs::path(path).wstring().c_str(), 0));
    CHECK(GetLastError() == EEXIST);
}
#endif

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

TEST_CASE("platform/S_strcmp and S_strncmp order both ways (vehicle-chassis false positive)")
{
    CHECK(S_strncmp("Fusion Engine", "vehicle", 7) != 0);   // 'F' < 'v' used to compare equal
    CHECK(S_strncmp("1", "vehicle", 7) < 0);
    CHECK(S_strncmp("zeta", "vehicle", 7) > 0);
    CHECK(S_strncmp("vehicle sensors", "vehicle", 7) == 0);
    CHECK(S_strcmp("abc", "abd") < 0);
    CHECK(S_strcmp("abd", "abc") > 0);
    CHECK(S_strcmp("abc", "abc") == 0);
    CHECK(S_strcmp("abc", "abcd") < 0);                      // strict prefix
    CHECK(S_strcmp("abcd", "abc") > 0);
    CHECK(S_strcmp("", "a") < 0);
    CHECK(S_strncmp("veh", "vehicle", 7) < 0);               // prefix shorter than n
    CHECK(S_strncmp("", "vehicle", 7) < 0);
    CHECK(S_strncmp("vehicle", "veh", 7) > 0);
    CHECK(S_strncmp("zzz", "aaa", 0) == 0);                  // n == 0 compares nothing
    CHECK(S_strncmp("vehicle", "vehicle", 100) == 0);        // n longer than both strings
    CHECK(S_strncmp("abc", "abd", 100) < 0);
    CHECK(S_strncmp("Vehicle", "vehicle", 7) < 0);           // case sensitive ('V' < 'v')
    CHECK(S_strncmp("vehicle", "Vehicle", 7) > 0);
    CHECK(S_strcmp("\xe9", "a") > 0);                        // unsigned char ordering
}
