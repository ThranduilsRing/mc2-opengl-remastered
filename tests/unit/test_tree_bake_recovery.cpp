#include "doctest.h"
#include <algorithm>
#include <cstddef>

namespace {
bool nvidia = true, flagSet = false, flagValue = false;
bool gos_IsNvidiaGPU() { return nvidia; }
namespace mc2::gate {
template<std::size_t N> struct Name {
    char value[N];
    constexpr Name(const char (&text)[N]) { std::copy_n(text, N, value); }
};
template<Name> bool isSet() { return flagSet; }
template<Name> bool get() { return flagSet && flagValue; }
}
#include "tree_dynamic_default.inc"
template<int Arm> bool forcedRecovery() {
#include "tree_recovery_gate.inc"
    return s_forceDynamicTrees;
}
}

TEST_CASE("tree_bake_recovery/NVIDIA keeps registered baked trees static by default")
{
    nvidia = true;
    flagSet = false;
    flagValue = false;
    // A valid light slot must survive render: vendor detection must not cause
    // invalidate -> submit -> register -> needsFullBakeNextFrame every frame.
    CHECK_FALSE(forcedRecovery<0>());
    nvidia = false;
    CHECK_FALSE(forcedRecovery<1>());
    flagSet = true;
    flagValue = true;
    CHECK(forcedRecovery<2>()); // explicit diagnostic fallback is retained
    nvidia = true;
    flagValue = false;
    CHECK_FALSE(forcedRecovery<3>());
}
