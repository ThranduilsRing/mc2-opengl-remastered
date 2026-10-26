#pragma once
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>
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
    for (const char* arg : args) {
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, arg, -1, nullptr, 0);
        if (!size) return -1;
        std::wstring wide(size, L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, arg, -1, wide.data(), size)) return -1;
        wide.pop_back();
        command += L" \"";
        size_t slashes = 0;
        for (wchar_t ch : wide) {
            if (ch == L'\\') { ++slashes; continue; }
            command.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
            if (ch == L'"') command += L'\\';
            command += ch;
            slashes = 0;
        }
        command.append(slashes * 2, L'\\');
        command += L'"';
    }
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
    std::vector<const char*> argv{executable.c_str()};
    argv.insert(argv.end(), args.begin(), args.end());
    argv.push_back(nullptr);
    const auto child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        execv(executable.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    int status = 0;
    for (;;) {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
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
