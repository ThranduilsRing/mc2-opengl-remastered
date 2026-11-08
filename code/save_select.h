// code/save_select.h -- pure "which save is newest" rule (header-only, game-free, doctest-covered).
//
// Rank by (group, completedMissions); equal rank breaks on the case-insensitive file name, greatest
// wins. The result must not depend on the order the saves are enumerated (FindFirstFile order is
// filesystem-defined), so the tie rule is explicit instead of "last one seen wins".
#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace mc2 {

struct NewestSave {
	long group = -1;
	long missions = -1;
	std::string name;

	bool valid() const { return !name.empty(); }

	static int compareNames(std::string_view a, std::string_view b)
	{
		const size_t n = a.size() < b.size() ? a.size() : b.size();
		for (size_t i = 0; i < n; ++i) {
			const int x = std::tolower(static_cast<unsigned char>(a[i]));
			const int y = std::tolower(static_cast<unsigned char>(b[i]));
			if (x != y) return x < y ? -1 : 1;
		}
		return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
	}

	// True when (g, m, n) outranks the current best.
	bool beats(long g, long m, std::string_view n) const
	{
		if (!valid()) return true;
		if (g != group) return g > group;
		if (m != missions) return m > missions;
		return compareNames(n, name) > 0;
	}

	void consider(long g, long m, std::string_view n)
	{
		if (!beats(g, m, n)) return;
		group = g; missions = m; name = std::string(n);
	}
};

} // namespace mc2
