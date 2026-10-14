#include "doctest.h"
#include "contract_test_support.h"
#include <filesystem>
#include <string>
#define NOMINMAX
#include <windows.h>

TEST_CASE("vfx_oracle/CardCloud lifecycle in an isolated platform")
{
    wchar_t module[32768];
    REQUIRE(GetModuleFileNameW(nullptr, module, 32768) != 0);
    const auto executable = std::filesystem::path(module).parent_path() / L"vfx_lifecycle_tests.exe";
    REQUIRE(std::filesystem::is_regular_file(executable));
    ContractEnvironment oracle{"MC2_VFX_ORACLE_RENDER", "0"};
    ContractEnvironment tube{"MC2_VFX_ORACLE_TUBE", "0"};
    ContractEnvironment shape{"MC2_VFX_ORACLE_SHAPE", "0"};
    ContractEnvironment gpuSim{"MC2_VFX_GPU_SIM_CARDCLOUD", "0"};
    ContractEnvironment log{"MC2_GPU_PARTICLES_LOG", "0"};
    ContractEnvironment sanitizer{"ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1:print_stacktrace=1"};
    for (const auto* arm : {"0", "1"}) {
        CAPTURE(arm);
        ContractEnvironment gpu{"MC2_GPU_PARTICLES", arm};
        std::wstring command = L"\"" + executable.wstring() + L"\" --no-breaks";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        REQUIRE(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
        const auto wait = WaitForSingleObject(process.hProcess, 60000);
        DWORD result = 1;
        const bool exited = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &result);
        if (wait == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        REQUIRE(exited);
        CHECK(result == 0);
    }
}
