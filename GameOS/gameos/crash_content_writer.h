#pragma once
// Private leaf shared with the writer doctests. Only normal-runtime methods allocate.
#include <windows.h>
#include <io.h>
#include <cstdint>
#include <cstdio>
#include <format>
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
    std::string profile_;   // user profile directory, replaced by %USERPROFILE% in recorded paths
    // Bundles get attached to reports: drop the \\?\ prefix and the user profile directory from recorded paths.
    std::string scrub(std::string_view p) const {
        if (p.starts_with("\\\\?\\")) p.remove_prefix(4);
        if (!profile_.empty() && p.size() >= profile_.size() &&
            _strnicmp(p.data(), profile_.data(), profile_.size()) == 0)
            return "%USERPROFILE%" + std::string(p.substr(profile_.size()));
        return std::string(p);
    }
    void append(std::string_view line) {
        if (journal_ == INVALID_HANDLE_VALUE) return;
        DWORD wrote = 0;
        std::string record(line);
        record += '\n';
        WriteFile(journal_, record.data(), DWORD(record.size()), &wrote, nullptr);
    }
public:
    ~ContentWriter() { if (journal_ != INVALID_HANDLE_VALUE) CloseHandle(journal_); }
    bool init(std::string_view path) {
        const std::string nul(path);
        if (nul.size() >= MAX_PATH) return false;   // no journal beats an invalid-parameter abort on a deep install path
        memcpy(path_, nul.c_str(), nul.size() + 1);
        char prof[MAX_PATH]{};
        const DWORD pl = GetEnvironmentVariableA("USERPROFILE", prof, MAX_PATH);
        if (pl && pl < MAX_PATH) profile_.assign(prof, pl);
        journal_ = CreateFileA(nul.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (journal_ == INVALID_HANDLE_VALUE) return false;
        append("{\"kind\":\"schema\",\"version\":1,\"hash_algorithm\":\"fnv1a64\",\"hashed_prefix_bytes\":65536,\"scope\":\"loaded File content; GPU shader identities in gpu_breadcrumbs.txt\"}");
        savePoint("none", "no save loaded or written this session");
        return true;
    }
    void mod(std::string_view id, int priority) {
        std::lock_guard lock(mutex_);
        append("{\"kind\":\"mod\",\"id\":" + quote(id) + ",\"priority\":" + std::to_string(priority) + "}");
    }
    void savePoint(std::string_view op, std::string_view path) {
        std::lock_guard lock(mutex_);
        append("{\"kind\":\"save_point\",\"operation\":" + quote(op) + ",\"path\":" + quote(scrub(path)) + "}");
    }
    void identity(std::string_view name, std::string_view source, uint64_t h, uint64_t size) {
        append(std::format("{{\"kind\":\"content\",\"path\":{},\"source\":{},\"hash\":\"{:016x}\",\"bytes\":{}}}",
                           quote(name), quote(source), h, size));
    }
    void image(std::string_view name, const void* data, size_t size) {
        std::lock_guard lock(mutex_);
        const auto h = hash(data, size);
        const auto key = std::string(name) + ':' + std::to_string(h);
        if (auto [it, fresh] = seen_.insert(key); fresh) identity(name, "fst-member", h, size);
    }
    void fd(int fd, std::string_view logical, std::string_view owner) {
        char physical[MAX_PATH]{};
        const auto native = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        const DWORD pathLength = GetFinalPathNameByHandleA(native, physical, MAX_PATH, FILE_NAME_NORMALIZED);
        if (!pathLength || pathLength >= MAX_PATH) return;
        // Size and mtime come from the loader's own handle, so a repeat open costs no second handle and no mutex-held I/O.
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(native, &info)) return;
        const uint64_t fileSize = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        const auto key = std::string(physical) + ':' + std::to_string(fileSize) + ':' +
                         std::to_string(info.ftLastWriteTime.dwHighDateTime) + ':' + std::to_string(info.ftLastWriteTime.dwLowDateTime);
        { std::lock_guard lock(mutex_); if (!seen_.insert(key).second) return; }
        // Identity is size + mtime + the first 64 KiB, read on a separate handle (the loader's file offset and CRT buffering stay untouched), outside the lock.
        HANDLE file = CreateFileA(physical, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        char bytes[65536]; DWORD got = 0;
        const BOOL ok = ReadFile(file, bytes, sizeof(bytes), &got, nullptr);
        CloseHandle(file);
        if (!ok) return;
        const uint64_t h = hash(bytes, got);
        std::lock_guard lock(mutex_);
        identity(logical, owner, h, fileSize);
        append("{\"kind\":\"physical\",\"logical\":" + quote(logical) + ",\"path\":" + quote(scrub(physical)) + "}");
    }
    // Exception path: no mutex, allocation, CRT streams or traversal. A concurrent final
    // journal record can be incomplete; the summary reports it rather than inventing data.
    bool snapshot(std::string_view folder) const {
        char dst[MAX_PATH];
        const auto r = std::format_to_n(dst, MAX_PATH - 1, "{}\\content.jsonl", folder);
        if (r.size >= MAX_PATH) return false;
        *r.out = '\0';
        return CopyFileA(path_, dst, FALSE) != FALSE;
    }
};
}
