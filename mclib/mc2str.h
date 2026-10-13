// mc2str.h -- the string helpers that replace the behaviour EString gave for free (step 4 batch B1,
// .claude/cleanup/cxx23-step4-b1-notes.md).
//
// WHAT: from_c turns a possibly-null C string into a std::string; iequals / icompare compare names that
// come from data (file names, mech / pilot / variant / mission names, flag IDs) ignoring ASCII case; ci_total_less is the
// total order for sorted lists; ascii_lower / ascii_upper change case of ASCII letters only; file_stem cuts the directory and the extension
// off a file name; erase_first cuts the first occurrence of a substring.
// WHY: std::string compares case-exactly and std::string(nullptr) is undefined, while names in the shipped data differ in case
// from the names that look them up and legacy callers pass null for "no text". Identity is therefore case-insensitive, as a fixed
// ASCII fold that gives the same answer on every host and locale (the Windows lstrcmpi collation is locale-dependent and absent
// on other platforms). Bytes >= 0x80 are never folded: they compare and order by value, which is defined and deterministic.
// HEADER-ONLY on purpose (mclib is linked by standalone tools that do not link GameOS).
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace mc2::str {

// A legacy `const char*` may be null, which means "no text"; std::string(nullptr) is undefined behaviour.
// Every pointer that enters a std::string from older code passes through here.
inline std::string from_c(const char* p) { return p ? std::string(p) : std::string(); }

namespace detail {
constexpr unsigned char fold(unsigned char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c; }
constexpr unsigned char upper(unsigned char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<unsigned char>(c - ('a' - 'A')) : c; }
}  // namespace detail

// Lexicographic comparison of the ASCII-folded bytes: -1, 0 or +1. Zero exactly when iequals is true.
constexpr int icompare(std::string_view a, std::string_view b) noexcept {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char x = detail::fold(static_cast<unsigned char>(a[i]));
        const unsigned char y = detail::fold(static_cast<unsigned char>(b[i]));
        if (x != y) return x < y ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

constexpr bool iequals(std::string_view a, std::string_view b) noexcept { return a.size() == b.size() && icompare(a, b) == 0; }

// Total order for a sorted list: folded lexicographic, then the raw bytes, so two names that differ only in case keep a
// defined order instead of the order the sort happened to see them in.
struct ci_total_less {
    using is_transparent = void;
    constexpr bool operator()(std::string_view a, std::string_view b) const noexcept {
        const int c = icompare(a, b);
        return c != 0 ? c < 0 : a < b;
    }
};

inline std::string ascii_lower(std::string_view s) {
    std::string r(s);
    std::ranges::transform(r, r.begin(), [](unsigned char c) { return static_cast<char>(detail::fold(c)); });
    return r;
}

inline std::string ascii_upper(std::string_view s) {
    std::string r(s);
    std::ranges::transform(r, r.begin(), [](unsigned char c) { return static_cast<char>(detail::upper(c)); });
    return r;
}

// `s` without the first occurrence of `part` (compared exactly, case included); `s` itself when `part` is empty or absent. The campaign code
// uses it to cut ".fit" out of a mission file name.
inline std::string erase_first(std::string_view s, std::string_view part) {
    const std::size_t at = part.empty() ? std::string_view::npos : s.find(part);
    if (at == std::string_view::npos) return std::string(s);
    std::string r(s);
    r.erase(at, part.size());
    return r;
}

// The name of a file without its directory and extension: "data\objects\madcat.csv" -> "madcat". Either path separator counts; the
// extension starts at the first '.' of the last path component, and a name without one is returned whole. Defined on every input: a path that
// ends in a separator or is empty gives an empty name, and ".x" gives an empty name (the dot starts the extension). The call sites it replaced
// cut at the first dot of the WHOLE path, so a dotted directory ("a.b\x.csv") gave "a"; that is a bug there and not reproduced here.
inline std::string file_stem(std::string_view path) {
    const std::size_t sep = path.find_last_of("/\\");
    const std::string_view name = sep == std::string_view::npos ? path : path.substr(sep + 1);
    return std::string(name.substr(0, name.find('.')));
}

}  // namespace mc2::str
