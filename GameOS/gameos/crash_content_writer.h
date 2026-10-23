#pragma once
// Private leaf shared with the writer doctests. Only normal-runtime methods allocate.
#include <windows.h>
#include <io.h>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace mc2::crash {
inline uint64_t hash(const void* data, size_t size, uint64_t h = 14695981039346656037ull) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
inline std::string quote(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32) { char esc[7]; std::snprintf(esc, sizeof(esc), "\\u%04x", c); out += esc; }
        else out += char(c);
    }
    return out + '"';
}
class ContentWriter {
    HANDLE journal_ = INVALID_HANDLE_VALUE;
    char path_[MAX_PATH]{};
    std::mutex mutex_;
    std::unordered_set<std::string> seen_;
    void append(const std::string& line) {
        if (journal_ == INVALID_HANDLE_VALUE) return;
        DWORD wrote = 0;
        const std::string record = line + '\n';
        WriteFile(journal_, record.data(), DWORD(record.size()), &wrote, nullptr);
    }
public:
    ~ContentWriter() { if (journal_ != INVALID_HANDLE_VALUE) CloseHandle(journal_); }
    bool init(const char* path) {
        strcpy_s(path_, path);
        journal_ = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (journal_ == INVALID_HANDLE_VALUE) return false;
        append("{\"kind\":\"schema\",\"version\":1,\"hash_algorithm\":\"fnv1a64\",\"scope\":\"loaded File content; GPU shader identities in gpu_breadcrumbs.txt\"}");
        savePoint("none", "no save loaded or written this session");
        return true;
    }
    void mod(const char* id, int priority) {
        std::lock_guard lock(mutex_);
        append("{\"kind\":\"mod\",\"id\":" + quote(id) + ",\"priority\":" + std::to_string(priority) + "}");
    }
    void savePoint(const char* op, const char* path) {
        std::lock_guard lock(mutex_);
        append("{\"kind\":\"save_point\",\"operation\":" + quote(op) + ",\"path\":" + quote(path ? path : "") + "}");
    }
    void identity(const char* name, const char* source, uint64_t h, uint64_t size) {
        char hex[17]; std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
        append("{\"kind\":\"content\",\"path\":" + quote(name) + ",\"source\":" + quote(source) +
               ",\"hash\":\"" + hex + "\",\"bytes\":" + std::to_string(size) + "}");
    }
    void image(const char* name, const void* data, size_t size) {
        std::lock_guard lock(mutex_);
        const auto h = hash(data, size);
        const auto key = std::string(name) + ':' + std::to_string(h);
        if (seen_.insert(key).second) identity(name, "fst-member", h, size);
    }
    void fd(int fd, const char* logical, const char* owner) {
        std::lock_guard lock(mutex_);
        char physical[MAX_PATH]{};
        const auto native = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        const DWORD pathLength = GetFinalPathNameByHandleA(native, physical, MAX_PATH, FILE_NAME_NORMALIZED);
        if (!pathLength || pathLength >= MAX_PATH) return;
        // Read a separate handle, so the loader's file offset and CRT buffering stay untouched.
        HANDLE file = CreateFileA(physical, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file, &info)) { CloseHandle(file); return; }
        const auto key = std::string(physical) + ':' + std::to_string(info.nFileSizeHigh) + ':' +
                         std::to_string(info.nFileSizeLow) + ':' + std::to_string(info.ftLastWriteTime.dwHighDateTime) + ':' +
                         std::to_string(info.ftLastWriteTime.dwLowDateTime);
        if (seen_.contains(key)) { CloseHandle(file); return; }
        char bytes[65536]; DWORD got = 0; uint64_t h = hash(nullptr, 0), size = 0;
        BOOL ok;
        while ((ok = ReadFile(file, bytes, sizeof(bytes), &got, nullptr)) && got) {
            h = hash(bytes, got, h); size += got;
        }
        CloseHandle(file);
        if (ok) {
            seen_.insert(key);
            identity(logical, owner, h, size);
            append("{\"kind\":\"physical\",\"logical\":" + quote(logical) + ",\"path\":" + quote(physical) + "}");
        }
    }
    // Exception path: no mutex, allocation, CRT streams or traversal. A concurrent final
    // journal record can be incomplete; the summary reports it rather than inventing data.
    bool snapshot(const char* folder) const {
        char dst[MAX_PATH];
        if (sprintf_s(dst, "%s\\content.jsonl", folder) < 0) return false;
        return CopyFileA(path_, dst, FALSE) != FALSE;
    }
};
}
