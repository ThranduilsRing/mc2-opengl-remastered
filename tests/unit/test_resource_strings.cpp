#include "doctest.h"
#include "utilities.h"
#include "gameos_resource.hpp"
#include "utils/mc2log.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// A scripted provider boundary; both public loaders link from the production TU.
struct gos_StringRes {
    const char* text = "";
    unsigned int calls = 0;
    DWORD lastId = 0;
    bool present = true;
};

std::optional<std::size_t> gos_ResourceStringLength(HSTRRES handle, DWORD)
{
    return handle->present ? std::optional<std::size_t>(handle->text ? std::strlen(handle->text) : 0) : std::nullopt;
}

const char* __stdcall gos_GetResourceString(HSTRRES handle, DWORD id)
{
    ++handle->calls;
    handle->lastId = id;
    return handle->text;
}

namespace {

struct Fixture {
    gos_StringRes table;
    HSTRRES previous = gosResourceHandle;
    std::FILE* log = std::tmpfile();

    explicit Fixture(const char* text)
    {
        REQUIRE(log != nullptr);
        table.text = text;
        gosResourceHandle = &table;
        mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, log);
    }

    ~Fixture()
    {
        mc2::log::setStreamOverrideForTest(mc2::log::Stream::Err, nullptr);
        gosResourceHandle = previous;
        if (log)
            std::fclose(log);
    }

    std::string diagnostics()
    {
        std::fflush(log);
        std::rewind(log);
        std::string result;
        std::array<char, 256> bytes;
        while (const size_t count = std::fread(bytes.data(), 1, bytes.size(), log))
            result.append(bytes.data(), count);
        return result;
    }
};

template<size_t Capacity>
struct Guarded {
    std::array<char, 8> before;
    std::array<char, Capacity> text;
    std::array<char, 8> after;

    Guarded()
    {
        before.fill('B');
        text.fill('X');
        after.fill('A');
    }

    void checkGuards() const
    {
        CHECK(std::all_of(before.begin(), before.end(), [](char c) { return c == 'B'; }));
        CHECK(std::all_of(after.begin(), after.end(), [](char c) { return c == 'A'; }));
    }
};

} // namespace

TEST_CASE("resource_strings/exact fit retains the complete value in both APIs")
{
    Fixture fixture("exact");
    Guarded<6> output;
    CHECK(cLoadString(42, output.text.data(), 6) == 5);
    CHECK(output.text[5] == '\0');
    CHECK(std::string(output.text.data()) == "exact");
    output.checkGuards();

    const std::string value = loadResourceString(42);
    CHECK(value == "exact");
    CHECK(value.c_str()[value.size()] == '\0');
    CHECK(fixture.table.calls == 2);
    CHECK(fixture.table.lastId == 42);
    CHECK(fixture.diagnostics().empty());
}

TEST_CASE("resource_strings/one over rejects only the bounded API with one diagnostic")
{
    Fixture fixture("exact!");
    Guarded<6> output;
    CHECK(cLoadString(43, output.text.data(), 6) == 0);
    CHECK(std::all_of(output.text.begin(), output.text.end(), [](char c) { return c == '\0'; }));
    output.checkGuards();
    const std::string value = loadResourceString(43);
    CHECK(value == "exact!");
    CHECK(value.c_str()[value.size()] == '\0');
    CHECK(fixture.diagnostics() ==
        "[RESOURCE_STRING v1] event=too_long id=43 source_length=6 capacity=6 copied=0\n");
}

TEST_CASE("resource_strings/mod sized text is owned and bounded rejections preserve canaries")
{
    std::string modText(65536, 'M');
    modText.replace(modText.size() - 12, 12, "F4_END_65536");
    Fixture fixture(modText.c_str());
    Guarded<256> output;
    CHECK(cLoadString(45208, output.text.data(), 256) == 0);
    CHECK(std::all_of(output.text.begin(), output.text.end(), [](char c) { return c == '\0'; }));
    output.checkGuards();
    const std::string value = loadResourceString(45208);
    CHECK(value == modText);
    CHECK(value.c_str()[65536] == '\0');
    CHECK(fixture.diagnostics() ==
        "[RESOURCE_STRING v1] event=too_long id=45208 source_length=65536 capacity=256 copied=0\n");

    modText[0] = 'Z';
    CHECK(value[0] == 'M');
}

TEST_CASE("resource_strings/valid empty is silent and clears the destination tail")
{
    Fixture fixture("");
    Guarded<8> output;
    CHECK(cLoadString(44, output.text.data(), 8) == 0);
    CHECK(std::all_of(output.text.begin(), output.text.end(), [](char c) { return c == '\0'; }));
    output.checkGuards();
    const std::string value = loadResourceString(44);
    CHECK(value.empty());
    CHECK(value.c_str()[0] == '\0');
    CHECK(fixture.diagnostics().empty());
}

TEST_CASE("resource_strings/marker looking valid content is preserved without a diagnostic")
{
    Fixture fixture("<missing-string:legacy:999999>");
    Guarded<64> output;
    CHECK(cLoadString(999999, output.text.data(), 64) == 30);
    CHECK(std::string(output.text.data()) == fixture.table.text);
    CHECK(output.text[30] == '\0');
    output.checkGuards();
    const std::string value = loadResourceString(999999);
    CHECK(value == fixture.table.text);
    CHECK(value.c_str()[value.size()] == '\0');
    CHECK(fixture.diagnostics().empty());
}

TEST_CASE("resource_strings/missing ID rejects provider marker with one diagnostic per API")
{
    Fixture fixture("<missing-string:legacy:999999>");
    fixture.table.present = false;
    Guarded<64> output;
    CHECK(cLoadString(999999, output.text.data(), 64) == 0);
    CHECK(std::all_of(output.text.begin(), output.text.end(), [](char c) { return c == '\0'; }));
    output.checkGuards();
    CHECK(loadResourceString(999999).empty());
    CHECK(fixture.table.calls == 0);
    CHECK(fixture.diagnostics() ==
        "[RESOURCE_STRING v1] event=missing_id id=999999 source_length=0 capacity=64 copied=0\n"
        "[RESOURCE_STRING v1] event=missing_id id=999999 source_length=0 capacity=0 copied=0\n");
    CHECK(std::string(gos_GetResourceString(&fixture.table, 999999)) == fixture.table.text);
}

TEST_CASE("resource_strings/null provider result is rejected once per API")
{
    Fixture fixture(nullptr);
    Guarded<8> output;
    CHECK(cLoadString(45, output.text.data(), 8) == 0);
    CHECK(std::all_of(output.text.begin(), output.text.end(), [](char c) { return c == '\0'; }));
    output.checkGuards();
    const std::string value = loadResourceString(45);
    CHECK(value.empty());
    CHECK(value.c_str()[0] == '\0');
    CHECK(fixture.table.calls == 2);
    CHECK(fixture.diagnostics() ==
        "[RESOURCE_STRING v1] event=null_provider_result id=45 source_length=0 capacity=8 copied=0\n"
        "[RESOURCE_STRING v1] event=null_provider_result id=45 source_length=0 capacity=0 copied=0\n");
}

TEST_CASE("resource_strings/invalid destinations are untouched and never reach the provider")
{
    Fixture fixture("content");
    Guarded<8> output;
    CHECK(cLoadString(46, nullptr, 8) == 0);
    CHECK(cLoadString(46, output.text.data(), 0) == 0);
    CHECK(cLoadString(46, output.text.data(), -7) == 0);
    CHECK(std::all_of(output.text.begin(), output.text.end(), [](char c) { return c == 'X'; }));
    output.checkGuards();
    CHECK(fixture.table.calls == 0);
    CHECK(fixture.diagnostics() ==
        "[RESOURCE_STRING v1] event=invalid_destination id=46 source_length=0 capacity=8 copied=0\n"
        "[RESOURCE_STRING v1] event=invalid_destination id=46 source_length=0 capacity=0 copied=0\n"
        "[RESOURCE_STRING v1] event=invalid_destination id=46 source_length=0 capacity=-7 copied=0\n");
}

TEST_CASE("resource_strings/capacity one writes only NUL and rejects nonempty content")
{
    Fixture fixture("content");
    Guarded<1> output;
    CHECK(cLoadString(47, output.text.data(), 1) == 0);
    CHECK(output.text[0] == '\0');
    output.checkGuards();
    CHECK(fixture.diagnostics() ==
        "[RESOURCE_STRING v1] event=too_long id=47 source_length=7 capacity=1 copied=0\n");
}

TEST_CASE("resource_strings/capacity one accepts valid empty without a diagnostic")
{
    Fixture fixture("");
    Guarded<1> output;
    CHECK(cLoadString(48, output.text.data(), 1) == 0);
    CHECK(output.text[0] == '\0');
    output.checkGuards();
    CHECK(fixture.diagnostics().empty());
}

TEST_CASE("resource_strings/null handles reject before the provider for both APIs")
{
    Fixture fixture("content");
    Guarded<8> output;
    CHECK(cLoadString(49, output.text.data(), 8, nullptr) == 0);
    CHECK(std::all_of(output.text.begin(), output.text.end(), [](char c) { return c == '\0'; }));
    output.checkGuards();
    const std::string value = loadResourceString(49, nullptr);
    CHECK(value.empty());
    CHECK(value.c_str()[0] == '\0');
    CHECK(fixture.table.calls == 0);
    CHECK(fixture.diagnostics() ==
        "[RESOURCE_STRING v1] event=null_handle id=49 source_length=0 capacity=8 copied=0\n"
        "[RESOURCE_STRING v1] event=null_handle id=49 source_length=0 capacity=0 copied=0\n");
}

TEST_CASE("resource_strings/explicit handles select their table without changing the default")
{
    Fixture fixture("default");
    gos_StringRes explicitTable{"explicit"};
    Guarded<9> output;
    CHECK(cLoadString(50, output.text.data(), 9, &explicitTable) == 8);
    CHECK(output.text[8] == '\0');
    CHECK(std::string(output.text.data()) == "explicit");
    output.checkGuards();
    CHECK(loadResourceString(51, &explicitTable) == "explicit");
    CHECK(loadResourceString(52) == "default");
    CHECK(explicitTable.calls == 2);
    CHECK(explicitTable.lastId == 51);
    CHECK(fixture.table.calls == 1);
    CHECK(fixture.table.lastId == 52);
    CHECK(gosResourceHandle == &fixture.table);
    CHECK(fixture.diagnostics().empty());
}

TEST_CASE("resource_strings/negative control old copy violates the same mod fixture contract")
{
    const std::string modText(65536, 'M');
    Fixture fixture(modText.c_str());
    Guarded<256> output;
    CHECK(cLoadString(45208, output.text.data(), 256) == 0);
    CHECK(std::all_of(output.text.begin(), output.text.end(), [](char c) { return c == '\0'; }));
    output.checkGuards();
    CHECK(fixture.diagnostics() ==
        "[RESOURCE_STRING v1] event=too_long id=45208 source_length=65536 capacity=256 copied=0\n");

    // The old copy violates the rejection and canary contract; oversized backing
    // storage keeps this negative control free of undefined behavior.
    std::vector<char> oldBacking(modText.size() + 1, 'A');
    std::memset(oldBacking.data(), 0, 256);
    std::memcpy(oldBacking.data(), modText.c_str(), modText.size());
    CHECK(oldBacking[0] != '\0');
    CHECK(oldBacking[255] != '\0');
    CHECK(oldBacking[256] != 'A');
    CHECK(oldBacking[modText.size() - 1] == 'M');
}
