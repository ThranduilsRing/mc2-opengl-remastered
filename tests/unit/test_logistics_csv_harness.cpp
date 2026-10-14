#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "logistics_csv.h"

#include <cstring>
#include <string>
#include <vector>

namespace {
namespace lc = logistics_csv;

static std::vector<std::string> extractFields(const char* row, int n) {

    std::vector<char> buf(strlen(row) + 64, '\0');
    memcpy(buf.data(), row, strlen(row));
    char* cur = buf.data();
    std::vector<std::string> out;
    for (int i = 0; i < n; ++i) {
        char field[1024];
        lc::extractField(cur, field, 1024);
        out.emplace_back(field);
    }
    return out;
}

TEST_CASE("logistics_csv_harness/wellformed_row_parity") {
    const char* row = "7,Weapon,Medium Laser,1,4.0,250,5";
    auto f = extractFields(row, 7);
    const char* want[] = {"7", "Weapon", "Medium Laser", "1", "4.0", "250", "5"};
    for (int i = 0; i < 7; ++i) CHECK((f[(size_t)i] == want[i]));
    return;
}

TEST_CASE("logistics_csv_harness/cursor_parks_on_nul_at_end") {
    char buf[] = "a,b,c";
    char* cur = buf;
    char field[8];
    lc::extractField(cur, field, 8);
    lc::extractField(cur, field, 8);
    lc::extractField(cur, field, 8);
    CHECK((*cur == '\0'));
    CHECK((cur == buf + 5));

    int extra = lc::extractField(cur, field, 8);
    CHECK((extra == 0 && field[0] == '\0'));
    CHECK((cur == buf + 5));
    return;
}

TEST_CASE("logistics_csv_harness/truncated_row_no_past_nul_walk") {
    char buf[] = "7,Weapon,Laser";
    char* cur = buf;
    const char* end = buf + strlen(buf);
    char field[64];
    for (int i = 0; i < 24; ++i) {
        lc::extractField(cur, field, 64);
        CHECK((cur <= end));
    }
    CHECK((cur == end));
    return;
}

TEST_CASE("logistics_csv_harness/empty_fields") {
    char buf[] = "a,,b";
    char* cur = buf;
    char field[8];
    CHECK((lc::extractField(cur, field, 8) == 1));
    CHECK((lc::extractField(cur, field, 8) == 0));
    CHECK((std::string(field).empty()));
    CHECK((lc::extractField(cur, field, 8) == 1));
    char b2[] = ",5";
    char* c2 = b2;
    CHECK((lc::extractInt(c2) == -1));
    CHECK((lc::extractInt(c2) == 5));
    char b3[] = ",2.5";
    char* c3 = b3;
    CHECK((lc::extractFloat(c3) == -1.0f));
    return;
}

TEST_CASE("logistics_csv_harness/no_trailing_delimiter") {
    char buf[] = "alpha,42";
    char* cur = buf;
    char field[16];
    lc::extractField(cur, field, 16);
    int n = lc::extractField(cur, field, 16);
    CHECK((n == 2 && std::string(field) == "42"));
    CHECK((*cur == '\0'));
    return;
}

TEST_CASE("logistics_csv_harness/overlong_field_clamped_no_overrun") {
    std::string big(300, 'X');
    big += ",tail";
    std::vector<char> buf(big.size() + 1, '\0');
    memcpy(buf.data(), big.c_str(), big.size());
    char* cur = buf.data();
    char small[16];

    char canary = 0x7E;
    int n = lc::extractField(cur, small, 16);
    CHECK((n == 15));
    CHECK((small[15] == '\0'));
    CHECK((strlen(small) == 15));
    CHECK((canary == 0x7E));
    return;
}

TEST_CASE("logistics_csv_harness/demo_intentional_fail" * doctest::should_fail()) {
    char buf[] = "a,b";
    char* cur = buf;
    char field[8];
    lc::extractField(cur, field, 8);
    CHECK((std::string(field) == "WRONG"));
    return;
}

}
