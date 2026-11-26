#include "doctest.h"
#include "contract_test_support.h"
#include "Gates/gate_cold.h"
#include "contract_process.h"

TEST_CASE("vfx_oracle/CardCloud lifecycle in an isolated platform")
{
    const auto executable = contractExecutable("vfx_lifecycle_tests");
    REQUIRE(std::filesystem::is_regular_file(executable));
    mc2::gate::ScopedOverride<"MC2_VFX_ORACLE_TUBE"> tube{"0"};
    mc2::gate::ScopedOverride<"MC2_VFX_ORACLE_SHAPE"> shape{"0"};
    mc2::gate::ScopedOverride<"MC2_GPU_PARTICLES_LOG"> log{"0"};
    ContractEnvironment sanitizer{"ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1:print_stacktrace=1"};
    for (const auto* arm : {"0", "1"}) {
        CAPTURE(arm);
        mc2::gate::ScopedOverride<"MC2_GPU_PARTICLES"> gpu{arm};
        CHECK(runContractChild(executable, {"--no-breaks", "--test-case-exclude=fx_playback/*"}) == 0);
    }
}
