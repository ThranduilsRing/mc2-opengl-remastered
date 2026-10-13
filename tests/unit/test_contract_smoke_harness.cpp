#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

TEST_CASE("contract_smoke_harness/pass") {
    CHECK((1 + 1 == 2));
    CHECK((std::string("abc").size() == 3));
    return;
}

TEST_CASE("contract_smoke_harness/fail" * doctest::should_fail()) {
    CHECK((2 + 2 == 5));
    return;
}

TEST_CASE("contract_smoke_harness/seeded") {
    std::uint64_t x = std::uint64_t{0} * 2654435761ull + 1u;
    CHECK(((x & 1u) == 1u));
    return;
}

}
