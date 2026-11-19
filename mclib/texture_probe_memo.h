// Per-VFS-generation memo for texture existence probes. A miss walks the whole VFS ladder (loose, FST, stat), so the
// answer is remembered; the load path primes it so destroy-time swaps (mech fall shapes, tree damage shapes) never probe.
#pragma once
#include "mc2_czstring.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mc2 {
class TextureProbeMemo {
public:
	// probe(path) runs only on a memo miss (outside the lock); generation is mc2::vfs::generation().
	template <class Probe>
	bool resolve(mc2::czstring path, uint64_t generation, const Probe& probe) {
		{
			const std::lock_guard lock(mutex_);
			if (generation != generation_) { memo_.clear(); generation_ = generation; }
			if (memo_.contains(path)) return memo_.at(path);
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

// Defined in bdactor.cpp. Memoised per VFS generation; textureProbeUncachedCount is the sum of uncached probes of both memos.
bool textureTgaExistsMemo(mc2::czstring tgaPath);
uint64_t textureProbeUncachedCount();

class TG_TypeMultiShape;
// Defined in bdactor.cpp. Loads (variant 0) every texture of a damage/fall shape at type init; ktxFallback accepts a .ktx2 sidecar.
void preloadDamageShapeTextures(TG_TypeMultiShape* shape, bool ktxFallback);
