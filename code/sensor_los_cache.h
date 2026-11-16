#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc2 {
template<class Object, class Position, class Epoch = std::uint32_t>
class SensorLosCache {
	struct Entry {
		Object object{};
		Position position{};
		Epoch epoch = 0;
	};
	std::vector<Entry> entries;
	Epoch epoch = 0;
public:
	void beginScan(std::size_t count) {
		// resize is a no-op for the usual, unchanged mover capacity.
		entries.resize(count);
		if (++epoch == 0) {
			// Invalidate every tag before reusing epoch one. Positions need no clear.
			for (auto& entry : entries) entry.epoch = 0;
			epoch = 1;
		}
	}
	template<class Load>
	Position get(std::size_t handle, Object object, bool eligible, Load load) {
		if (!eligible || handle >= entries.size()) return load();
		auto& entry = entries[handle];
		if (entry.epoch != epoch || entry.object != object) {
			entry.position = load();
			entry.object = object;
			entry.epoch = epoch;
		}
		return entry.position;
	}
};
}
