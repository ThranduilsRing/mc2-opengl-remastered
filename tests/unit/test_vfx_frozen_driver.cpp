#include "doctest.h"
#include "contract_test_support.h"
#include "contract_process.h"
#include "Gates/gate_name.h"
#include <fstream>

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
    ContractEnvironment tube{mc2::gate::name<"MC2_VFX_ORACLE_TUBE">(), "0"};
    ContractEnvironment shape{mc2::gate::name<"MC2_VFX_ORACLE_SHAPE">(), "0"};
    ContractEnvironment sanitizer{"ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1:print_stacktrace=1"};
    struct Arm { const char* gpu; const char* log; };
    for (const auto arm : {Arm{"0", "0"}, Arm{"1", "1"}}) {
        INFO("gpu=" << std::string(arm.gpu)
            << " log=" << std::string(arm.log));
        ContractEnvironment gpu{mc2::gate::name<"MC2_GPU_PARTICLES">(), arm.gpu};
        ContractEnvironment log{mc2::gate::name<"MC2_GPU_PARTICLES_LOG">(), arm.log};
        CHECK(runContractChild(executable, {"--no-breaks", "--test-case=vfx_frozen/*"}) == 0);
    }
    CHECK(runContractChild(executable, {"--no-breaks", "--test-case=vfx_mesh/*"}) == 0);
    {
        ContractEnvironment nativePert{mc2::gate::name<"MC2_VFX_ORACLE_SHAPE">(), "1"};
        ContractEnvironment gpu{mc2::gate::name<"MC2_GPU_PARTICLES">(), "1"};
        CHECK(runContractChild(executable, {"--no-breaks", "--test-case=vfx_frozen/PertCloud*"}) == 0);
    }
}


TEST_CASE("fx_playback/native playback matches the isolated legacy oracle")
{
    const auto executable = contractExecutable("vfx_lifecycle_tests");
    REQUIRE(std::filesystem::is_regular_file(executable));
    ContractEnvironment sanitizer{"ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1:print_stacktrace=1"};
    CHECK(runContractChild(executable, {"--no-breaks", "--test-case=fx_playback/controlled*", "--abort-after=1"}) == 0);
    // Keep each owned child within contract_process's existing 60-second limit.
    for (const auto* quarter : {"--subcase=quarter1", "--subcase=quarter2", "--subcase=quarter3", "--subcase=quarter4"}) {
        CAPTURE(quarter);
        CHECK(runContractChild(executable, {"--no-breaks", "--test-case=fx_playback/stock*", quarter, "--abort-after=1"}) == 0);
    }
}

TEST_CASE("contract process/quoted \"arguments\" and long argv")
{
    const auto path = std::filesystem::temp_directory_path() /
        ("mc2 child " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::filesystem::remove(p); } } cleanup{path};
    const auto output = "--out=" + path.string();
    CHECK(runContractChild(contractExecutable("mc2_tests"), {"--count", "--no-colors",
        "--test-case=contract process/quoted \"arguments\" and long argv", output.c_str(),
        "--no-breaks", "--no-breaks", "--no-breaks", "--no-breaks", "--no-breaks", "--no-breaks",
        "--no-breaks", "--no-breaks", "--no-breaks", "--no-breaks", "--no-breaks", "--no-breaks", "--no-breaks"}) == 0);
    std::ifstream in(path);
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    CHECK(text.find("test cases passing the current filters: 1") != std::string::npos);
#ifndef _WIN32
    CHECK(runContractChild("/bin/sh", {"-c", "kill -TERM $$"}) == 128 + SIGTERM);
#endif
}
