#include "doctest.h"
#include "contract_test_support.h"
#include <filesystem>
#include <string>
#define NOMINMAX
#include <windows.h>

TEST_CASE("vfx_trace/combat trace assertions pass in an isolated diagnostic sink")
{
    wchar_t module[32768];
    REQUIRE(GetModuleFileNameW(nullptr, module, 32768) != 0);
    const auto executable = std::filesystem::path(module).parent_path() / L"vfx_combat_trace_tests.exe";
    REQUIRE(std::filesystem::is_regular_file(executable));
    ContractEnvironment sanitizer{"ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1:print_stacktrace=1"};
    std::wstring command = L"\"" + executable.wstring() + L"\" --no-breaks --test-case=vfx_trace/* --no-skip";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    REQUIRE(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    const auto wait = WaitForSingleObject(process.hProcess, 60000);
    CAPTURE(wait);
    DWORD result = 1;
    const bool exited = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &result);
    if (wait == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CHECK_MESSAGE(wait != WAIT_TIMEOUT, "combat-trace subprocess timed out after 60000 ms");
    REQUIRE(exited);
    CHECK(result == 0);
}

TEST_CASE("vfx_frozen/fresh processes cover MLR and both GPU harvest gate vectors")
{
    wchar_t module[32768];
    REQUIRE(GetModuleFileNameW(nullptr, module, 32768) != 0);
    const auto executable = std::filesystem::path(module).parent_path() / L"vfx_lifecycle_tests.exe";
    REQUIRE(std::filesystem::is_regular_file(executable));
    ContractEnvironment tube{"MC2_VFX_ORACLE_TUBE", "0"};
    ContractEnvironment shape{"MC2_VFX_ORACLE_SHAPE", "0"};
    ContractEnvironment compare{"MC2_VFX_GPU_SIM_COMPARE", "0"};
    ContractEnvironment sanitizer{"ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1:print_stacktrace=1"};
    struct Arm { const char* gpu; const char* oracle; const char* log; const char* gpuSim; };
    for (const auto arm : {Arm{"0", "0", "0", "0"}, Arm{"1", "0", "0", "0"},
                           Arm{"1", "1", "1", "0"}, Arm{"1", "0", "0", "1"}}) {
        INFO("gpu=" << std::string(arm.gpu) << " oracle=" << std::string(arm.oracle)
            << " log=" << std::string(arm.log) << " gpuSim=" << std::string(arm.gpuSim));
        ContractEnvironment gpu{"MC2_GPU_PARTICLES", arm.gpu};
        ContractEnvironment oracle{"MC2_VFX_ORACLE_RENDER", arm.oracle};
        ContractEnvironment log{"MC2_GPU_PARTICLES_LOG", arm.log};
        ContractEnvironment gpuSim{"MC2_VFX_GPU_SIM_CARDCLOUD", arm.gpuSim};
        std::wstring command = L"\"" + executable.wstring() + L"\" --no-breaks --test-case=vfx_frozen/*";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION process{};
        REQUIRE(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
        const auto wait = WaitForSingleObject(process.hProcess, 60000);
        CAPTURE(wait);
        DWORD result = 1;
        const bool exited = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &result);
        if (wait == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CHECK_MESSAGE(wait != WAIT_TIMEOUT, "frozen-cloud subprocess timed out after 60000 ms");
        REQUIRE(exited);
        CHECK(result == 0);
    }
}
