#include "doctest.h"
#include "contract_test_support.h"
#include "contract_process.h"

TEST_CASE("vfx_trace/combat trace assertions pass in an isolated diagnostic sink")
{
    const auto executable = contractExecutable("vfx_combat_trace_tests");
    REQUIRE(std::filesystem::is_regular_file(executable));
    ContractEnvironment sanitizer{"ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1:print_stacktrace=1"};
    CHECK(runContractChild(executable, {"--no-breaks", "--test-case=vfx_trace/*", "--no-skip"}) == 0);
}

TEST_CASE("vfx_frozen/fresh processes cover MLR and the GPU harvest gate vectors")
{
    const auto executable = contractExecutable("vfx_lifecycle_tests");
    REQUIRE(std::filesystem::is_regular_file(executable));
    ContractEnvironment tube{"MC2_VFX_ORACLE_TUBE", "0"};
    ContractEnvironment shape{"MC2_VFX_ORACLE_SHAPE", "0"};
    ContractEnvironment sanitizer{"ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1:print_stacktrace=1"};
    struct Arm { const char* gpu; const char* log; };
    for (const auto arm : {Arm{"0", "0"}, Arm{"1", "1"}}) {
        INFO("gpu=" << std::string(arm.gpu)
            << " log=" << std::string(arm.log));
        ContractEnvironment gpu{"MC2_GPU_PARTICLES", arm.gpu};
        ContractEnvironment log{"MC2_GPU_PARTICLES_LOG", arm.log};
        CHECK(runContractChild(executable, {"--no-breaks", "--test-case=vfx_frozen/*"}) == 0);
    }
}
