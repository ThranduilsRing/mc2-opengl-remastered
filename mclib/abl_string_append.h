// WHAT: the bounded string writes behind ABL's concat() and the standard routines that fill a script char[N] variable.
// WHY: a script's char[N] variable is a heap block of exactly N bytes, and the unbounded strcat/strcpy that used to fill it
// ran past it whenever the text outgrew N-1 characters (mc2_15: corebrain's coreEscort builds "coreEscort: <id> (<x>,<y>,<z>)"
// in a char[50]; two 19-character positions made it 65 characters) and corrupted the neighbouring heap block.
#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

namespace abl {

// A possibly-null C string as a view (null is the empty text).
inline std::string_view textOf(const char* s) noexcept {
	return s ? std::string_view{s} : std::string_view{};
}

// Appends item to the NUL-terminated text in dest, whose size is the variable's capacity including the NUL, never writing
// past it. An unterminated dest counts as full. An empty dest (size unknown) takes nothing. Returns the number of
// characters that did not fit (0 = appended whole).
inline size_t appendBounded(std::span<char> dest, std::string_view item) noexcept {
	if (dest.empty()) return item.size();
	size_t len = static_cast<size_t>(std::ranges::find(dest, '\0') - dest.begin());
	if (len >= dest.size()) { len = dest.size() - 1; dest[len] = '\0'; }
	const size_t room = dest.size() - 1 - len;
	const size_t n = std::min(item.size(), room);
	std::copy_n(item.data(), n, dest.data() + len);
	dest[len + n] = '\0';
	return item.size() - n;
}

// Replaces the text in dest with src, cut to fit (capacity including the NUL). An empty dest takes nothing. Returns the
// number of characters that did not fit.
inline size_t copyBounded(std::span<char> dest, std::string_view src) noexcept {
	if (dest.empty()) return src.size();
	const size_t n = std::min(src.size(), dest.size() - 1);
	std::copy_n(src.data(), n, dest.data());
	dest[n] = '\0';
	return src.size() - n;
}

}  // namespace abl
