// Per-VFS-generation memo for texture existence probes. A miss walks the whole VFS ladder (loose, FST, stat), so the
// answer is remembered; the load path primes it so destroy-time swaps (mech fall shapes, tree damage shapes) never probe.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mc2 {
class TextureProbeMemo {
public:
	// probe(path) runs only on a memo miss (outside the lock); generation is mc2::vfs::generation().
	template <class Probe>
	bool get(const char* path, uint64_t generation, Probe&& probe) {
		{
			const std::lock_guard lock(mutex_);
			if (generation != generation_) { memo_.clear(); generation_ = generation; }
			if (auto it = memo_.find(path); it != memo_.end()) return it->second;
		}
		const bool exists = probe(path);
		{
			const std::lock_guard lock(mutex_);
			++uncached_;
			if (generation == generation_) memo_.insert_or_assign(path, exists);
		}
		return exists;
	}
	// Uncached probes performed so far (a test or a diagnostic reads the delta across a phase).
	uint64_t uncachedCount() const { const std::lock_guard lock(mutex_); return uncached_; }
private:
	mutable std::mutex mutex_;
	std::unordered_map<std::string, bool> memo_;
	uint64_t generation_ = ~uint64_t{0};
	uint64_t uncached_ = 0;
};
}

#include "mc2_czstring.h"
// Defined in bdactor.cpp. Memoised per VFS generation; textureProbeUncachedCount is the sum of uncached probes of both memos.
bool textureTgaExistsMemo(mc2::czstring tgaPath);
uint64_t textureProbeUncachedCount();
