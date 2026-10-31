// mc2format.h -- platform-independent printf-compatible formatting on std::format (step 3 of the C++23 plan,
// .claude/cleanup/cxx23-step3-logging-plan.md).
//
// WHAT: to_buf / to_buf_n are snprintf with a compile-time-checked format string; safe() and the as_*
// helpers reproduce what a printf argument means (null %s prints "(null)", %u reinterprets a signed int as unsigned,
// %ld is 32-bit on Windows) so a mechanical conversion keeps the output bytes.
// WHY: sprintf into a stack array has no bound and no argument checking. to_buf takes the bound from the array type
// and std::format_string rejects a specifier/argument mismatch at compile time.
// Owned by mclib: simulation code and standalone tools must not depend on GameOS for formatting.
// HEADER-ONLY on purpose: mclib is linked by standalone tools that do not link GameOS, so nothing here may need a
// symbol from another library.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace mc2::fmt {

// printf("%s", nullptr) prints "(null)" on the MSVC CRT; std::format of a null const char* is undefined.
template <class P> requires std::convertible_to<P, const char*>
inline const char* safe(P s) noexcept {
    const char* p = s;
    return p ? p : "(null)";
}

// The varargs meaning of an integer conversion. Anything that is not an integer or an enum does not compile:
// a double passed to %d is a printf bug and must surface as a finding, not be converted silently.
template <class T> concept PrintfInteger = std::integral<T> || std::is_enum_v<T>;
template <PrintfInteger T> constexpr int32_t as_i32(T v) noexcept { return static_cast<int32_t>(v); }
template <PrintfInteger T> constexpr uint32_t as_u32(T v) noexcept { return static_cast<uint32_t>(v); }
template <PrintfInteger T> constexpr int64_t as_i64(T v) noexcept { return static_cast<int64_t>(v); }
template <PrintfInteger T> constexpr uint64_t as_u64(T v) noexcept { return static_cast<uint64_t>(v); }
template <PrintfInteger T> constexpr size_t as_size(T v) noexcept { return static_cast<size_t>(v); }
template <PrintfInteger T> constexpr char as_char(T v) noexcept { return static_cast<char>(v); }
// %p on the MSVC CRT prints the address as 16 upper-case hex digits without a 0x prefix: format it as {:016X}.
template <class T> inline uintptr_t as_ptr(T* p) noexcept { return reinterpret_cast<uintptr_t>(p); }
inline uintptr_t as_ptr(std::nullptr_t) noexcept { return 0; }

inline constexpr int kFormatError = -1;     // what snprintf returns when it cannot format

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
        return kFormatError;
    }
}

// sprintf/snprintf into an array: the bound is the array's extent, so it cannot be wrong.
template <size_t N, class... A>
int to_buf(char (&buf)[N], std::format_string<A...> f, A&&... a) noexcept {
    return to_buf_n(buf, N, f, std::forward<A>(a)...);
}

// printf to a stream, as one fwrite of the formatted text. The stream is whatever FILE* the call site used, so the MC2_LOG-unset redirect of
// stdout/stderr to NUL and the call site's own fflush() keep working; nothing is added to the text (no level, no newline, no prefix).
// Not for crash or allocator paths: the text is built in a per-thread std::string.
namespace detail {
// Never destroyed: handlers registered with atexit run after the main thread's thread_local objects are gone, and several of them print
// summary lines the smoke gates read. The string lives in raw per-thread storage that no destructor ever runs on.
inline std::string& scratch() {
    alignas(std::string) thread_local unsigned char storage[sizeof(std::string)];
    thread_local std::string* const buf = std::construct_at(reinterpret_cast<std::string*>(storage));
    return *buf;
}
}  // namespace detail

// Returns what printf returns: the number of characters written, or a negative value when formatting or the write failed.
template <class... A>
int print(std::FILE* f, std::format_string<A...> fmt, A&&... a) noexcept {
    try {
        std::string& buf = detail::scratch();
        buf.clear();
        std::vformat_to(std::back_inserter(buf), fmt.get(), std::make_format_args(a...));
        const size_t written = std::fwrite(buf.data(), 1, buf.size(), f);
        return written == buf.size() ? static_cast<int>(written) : kFormatError;
    } catch (...) {
        return kFormatError;
    }
}
template <class... A> int print_err(std::format_string<A...> fmt, A&&... a) noexcept { return mc2::fmt::print(stderr, fmt, std::forward<A>(a)...); }
template <class... A> int print_out(std::format_string<A...> fmt, A&&... a) noexcept { return mc2::fmt::print(stdout, fmt, std::forward<A>(a)...); }

}  // namespace mc2::fmt
