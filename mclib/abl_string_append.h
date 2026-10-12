// WHAT: the bounded append behind ABL's concat(). WHY: a script's char[N] variable is a heap block of exactly N bytes,
// and the strcat that used to implement concat ran past it whenever the text grew beyond N-1 characters (mc2_15:
// corebrain's coreEscort builds "coreEscort: <id> (<x>,<y>,<z>)" in a char[50]; two 19-character positions made it 65
// characters) and corrupted the neighbouring heap block.
#pragma once

#include <cstddef>
#include <cstring>

namespace abl {

// Appends item to the NUL-terminated text in dest, whose capacity is cap bytes including the NUL, never writing past
// cap. An unterminated dest counts as full. Returns the number of characters that did not fit (0 = appended whole).
inline size_t appendBounded(char* dest, size_t cap, const char* item) noexcept {
	if (!item) return 0;
	const size_t itemLen = std::strlen(item);
	if (!dest || cap == 0) return itemLen;
	size_t len = ::strnlen(dest, cap);
	if (len >= cap) { len = cap - 1; dest[len] = '\0'; }
	const size_t room = cap - 1 - len;
	const size_t n = itemLen < room ? itemLen : room;
	std::memcpy(dest + len, item, n);
	dest[len + n] = '\0';
	return itemLen - n;
}

}  // namespace abl
