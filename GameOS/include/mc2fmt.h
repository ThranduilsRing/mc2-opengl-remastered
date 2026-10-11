// mc2fmt.h -- printf-compatible buffer formatting on std::format (step 3 of the C++23 plan,
// .claude/cleanup/cxx23-step3-logging-plan.md).
//
// WHAT: to_buf / to_buf_n are snprintf with a compile-time-checked format string; safe() and the as_*
// helpers reproduce what a printf argument means (null %s prints "(null)", %u reinterprets a signed int as unsigned,
// %ld is 32-bit on Windows) so a mechanical conversion keeps the output bytes.
// WHY: sprintf into a stack array has no bound and no argument checking. to_buf takes the bound from the array type
// and std::format_string rejects a specifier/argument mismatch at compile time.
// HEADER-ONLY on purpose: mclib is linked by standalone tools that do not link GameOS, so nothing here may need a
// symbol from another library.
#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <type_traits>
#include <utility>

namespace mc2::fmt {

// printf("%s", nullptr) prints "(null)" on the MSVC CRT; std::format of a null const char* is undefined.
inline const char* safe(const char* s) noexcept { return s ? s : "(null)"; }

// The varargs meaning of an integer conversion. Anything that is not an integer or an enum does not compile:
// a double passed to %d is a printf bug and must surface as a finding, not be converted silently.
template <class T> constexpr int32_t as_i32(T v) noexcept {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "printf %d/%i needs an integer argument");
    return static_cast<int32_t>(v);
}
template <class T> constexpr uint32_t as_u32(T v) noexcept {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "printf %u/%x/%X/%o needs an integer argument");
    return static_cast<uint32_t>(v);
}
template <class T> constexpr int64_t as_i64(T v) noexcept {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "printf %lld needs an integer argument");
    return static_cast<int64_t>(v);
}
template <class T> constexpr uint64_t as_u64(T v) noexcept {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "printf %llu/%llx needs an integer argument");
    return static_cast<uint64_t>(v);
}
template <class T> constexpr size_t as_size(T v) noexcept {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "printf %zu needs an integer argument");
    return static_cast<size_t>(v);
}
template <class T> constexpr char as_char(T v) noexcept {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "printf %c needs an integer argument");
    return static_cast<char>(v);
}
// %p on the MSVC CRT prints the address as 16 upper-case hex digits without a 0x prefix: format it as {:016X}.
template <class T> inline uintptr_t as_ptr(T* p) noexcept { return reinterpret_cast<uintptr_t>(p); }
inline uintptr_t as_ptr(std::nullptr_t) noexcept { return 0; }

// snprintf: at most cap-1 characters and a NUL (nothing at all when cap is 0), returns the length the full
// output would have had, or -1 when formatting itself fails.
template <class... A>
int to_buf_n(char* buf, size_t cap, std::format_string<A...> f, A&&... a) noexcept {
    try {
        if (cap == 0) return static_cast<int>(std::formatted_size(f, std::forward<A>(a)...));
        auto r = std::format_to_n(buf, static_cast<std::ptrdiff_t>(cap - 1), f, std::forward<A>(a)...);
        *r.out = '\0';
        return static_cast<int>(r.size);
    } catch (...) {
        if (cap) buf[0] = '\0';
        return -1;
    }
}

// sprintf/snprintf into an array: the bound is the array's extent, so it cannot be wrong.
template <size_t N, class... A>
int to_buf(char (&buf)[N], std::format_string<A...> f, A&&... a) noexcept {
    return to_buf_n(buf, N, f, std::forward<A>(a)...);
}

}  // namespace mc2::fmt
