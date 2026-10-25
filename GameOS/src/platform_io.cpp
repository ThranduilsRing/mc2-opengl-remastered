#ifndef PLATFORM_WINDOWS

#include "platform_io.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cerrno>

std::string mc2_native_path(const char* input)
{
    std::string normalized(input);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    const std::filesystem::path path(normalized);
    std::filesystem::path result = path.root_path();
    auto equal = [](const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
            return std::tolower(x) == std::tolower(y);
        });
    };
    for (const auto& part : path.relative_path()) {
        std::error_code error;
        if (std::filesystem::exists(result / part, error)) { result /= part; continue; }
        std::filesystem::path found;
        for (const auto& entry : std::filesystem::directory_iterator(result.empty() ? "." : result, error)) {
            if (!equal(entry.path().filename().string(), part.string())) continue;
            // A Windows-derived install cannot contain ambiguous case aliases.
            if (!found.empty()) return normalized;
            found = entry.path().filename();
        }
        result /= found.empty() ? part : found;
    }
    return result.string();
}

int mc2_open(const char* path, int flags, mode_t mode)
{
    if (!(flags & O_CREAT)) {
        const int fd = ::open(path, flags, mode);
        if (fd >= 0 || errno != ENOENT) return fd;
    }
    return ::open(mc2_native_path(path).c_str(), flags, mode);
}

int mc2_stat(const char* path, struct stat* info)
{
    if (::stat(path, info) == 0) return 0;
    if (errno != ENOENT) return -1;
    return ::stat(mc2_native_path(path).c_str(), info);
}

long _filelength(int fd) {
    struct stat buf;
    fstat(fd, &buf);
    return buf.st_size;
}

#endif // PLATFORM_WINDOWS
