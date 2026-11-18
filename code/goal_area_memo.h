#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace mc2 {
// Call-local, direct-mapped cache. Collisions only cause provider re-reads.
template<class Provider>
class GoalAreaMemo {
	struct Entry {
		long area = -1;
		unsigned char value = 0;
		std::uint32_t epoch = 0;
	};
	Provider& provider;
	long from;
	std::array<Entry, 64> entries{};
	std::uint32_t epoch = 1;
public:
	GoalAreaMemo(Provider& source, long fromArea) : provider(source), from(fromArea) {}
	unsigned char get(long area) {
		auto& entry = entries[static_cast<std::size_t>(area) % entries.size()];
		if (entry.epoch != epoch || entry.area != area) {
			entry.value = provider.getPathExists(from, area);
			entry.area = area;
			entry.epoch = epoch;
		}
		return entry.value;
	}
	void set(long fromArea, long toArea, unsigned char value) {
		provider.setPathExists(fromArea, toArea, value);
		// A path can write pairs involving any previously cached destination.
		// Invalidate after EVERY write, including writes with a different source.
		if (++epoch == 0) {
			entries = {};
			epoch = 1;
		}
	}
};
}
