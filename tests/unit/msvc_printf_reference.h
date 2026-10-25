#pragma once
#include <cstdio>
#include <cstdint>
#include <string>
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
        if (reference[pos + 1] == 'p') { reference.replace(pos, 2, "%016llX"); pos += 6; }
    }
    return std::snprintf(output, capacity, reference.c_str(), printf_reference::argument(args)...);
#endif
}
