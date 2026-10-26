#pragma once
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

// The conversion corpus preserves MSVC's diagnostic text on every host.
// glibc's %p syntax and null-string precision are different CRT conventions.
namespace printf_reference {
template<class T> auto argument(T value) {
    if constexpr (std::is_same_v<T, void*> || std::is_same_v<T, const void*>)
        return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(value));
    else if constexpr (std::is_same_v<T, char*> || std::is_same_v<T, const char*>)
        return value ? value : "(null)";
    else return value;
}
}
template<class... Args>
int corpusSnprintf(char* output, size_t capacity, const char* format, Args... args) {
#ifdef _WIN32
    return std::snprintf(output, capacity, format, args...);
#else
    std::string reference(format);
    for (size_t pos = 0; pos + 1 < reference.size(); ++pos) {
        if (reference[pos] != '%') continue;
        if (reference[pos + 1] == '%') { ++pos; continue; }
        size_t end = pos + 1;
        while (end < reference.size() && std::string_view("-+ #0").find(reference[end]) != std::string_view::npos) ++end;
        while (end < reference.size() && ((reference[end] >= '0' && reference[end] <= '9') || reference[end] == '*')) ++end;
        const bool precision = end < reference.size() && reference[end] == '.';
        if (precision) {
            ++end;
            while (end < reference.size() && ((reference[end] >= '0' && reference[end] <= '9') || reference[end] == '*')) ++end;
        }
        if (end < reference.size() && reference[end] == 'p') {
            const std::string conversion = precision ? "llX" : ".16llX";
            reference.replace(end, 1, conversion);
            pos = end + conversion.size() - 1;
        }
    }
    return std::snprintf(output, capacity, reference.c_str(), printf_reference::argument(args)...);
#endif
}
