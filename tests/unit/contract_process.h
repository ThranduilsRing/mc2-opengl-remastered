#pragma once
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

inline std::filesystem::path contractExecutable(const char* name)
{
#ifdef _WIN32
    wchar_t module[32768];
    if (!GetModuleFileNameW(nullptr, module, 32768)) return {};
    return std::filesystem::path(module).parent_path() / (std::string(name) + ".exe");
#else
    char module[4096];
    const auto size = readlink("/proc/self/exe", module, sizeof(module) - 1);
    if (size < 0) return {};
    module[size] = 0;
    return std::filesystem::path(module).parent_path() / name;
#endif
}

// Every probe owns its child and waits for it, including the timeout path.
inline int runContractChild(const std::filesystem::path& executable,
                            std::initializer_list<const char*> args)
{
#ifdef _WIN32
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    for (const char* arg : args) { command += L' '; while (*arg) command += wchar_t(*arg++); }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return -1;
    const auto wait = WaitForSingleObject(process.hProcess, 60000);
    DWORD result = 1;
    const bool exited = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &result);
    if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, INFINITE); }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exited ? int(result) : -1;
#else
    const auto child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        const char* argv[16]{};
        unsigned index = 0;
        argv[index++] = executable.c_str();
        for (const auto* arg : args) { if (index >= 15) _exit(127); argv[index++] = arg; }
        execv(executable.c_str(), const_cast<char* const*>(argv));
        _exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    int status = 0;
    for (;;) {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (result < 0 && errno != EINTR) return -1;
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
}
