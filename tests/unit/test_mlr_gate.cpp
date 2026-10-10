// tests/unit/test_mlr_gate.cpp
//
// mc2::mlr_gate::is_disabled() latches MC2_DISABLE_GOSFX on first use. The latch is a function-local static (thread-safe
// initialisation), replacing a double-checked flag that was read without synchronisation. This process calls it first
// here, so the test sets the variable before that first call.

#include "doctest.h"

#include "Gates/gate_cold.h"
#include "mlr/mlr_gate.h"

#include <cstdlib>
#include <thread>
#include <vector>

TEST_CASE("mlr_gate latches the env value on first use and every thread sees it")
{
    _putenv_s("MC2_DISABLE_GOSFX", "1");
    mc2::gate::invalidateAll();

    CHECK(mc2::mlr_gate::is_disabled());

    _putenv_s("MC2_DISABLE_GOSFX", "0");            // after the first call the value no longer moves
    mc2::gate::invalidateAll();
    CHECK(mc2::mlr_gate::is_disabled());

    std::vector<std::thread> threads;
    std::vector<int> seen(8, 0);
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&seen, i] { seen[i] = mc2::mlr_gate::is_disabled() ? 1 : 0; });
    for (auto &t : threads) t.join();
    for (int v : seen) CHECK(v == 1);

    _putenv_s("MC2_DISABLE_GOSFX", "");
    mc2::gate::invalidateAll();
}
