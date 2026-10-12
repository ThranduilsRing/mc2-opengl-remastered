// Allocation accounting seam; see alloc_probe.h.
#include "alloc_probe.h"

#include <atomic>
#include <cstdlib>

namespace mc2_alloc_probe {

namespace {

// The header keeps the block 16-byte aligned, which is what malloc promises on every platform we build for.
constexpr size_t kHeader = 16;

std::atomic<uint64_t> s_live{0};
std::atomic<uint64_t> s_bytes{0};
std::atomic<uint64_t> s_total{0};

} // namespace

void* allocate(size_t bytes)
{
	unsigned char* raw = static_cast<unsigned char*>(std::malloc(kHeader + bytes));
	if (!raw)
		return nullptr;
	*reinterpret_cast<uint64_t*>(raw) = bytes;
	s_live.fetch_add(1, std::memory_order_relaxed);
	s_bytes.fetch_add(bytes, std::memory_order_relaxed);
	s_total.fetch_add(1, std::memory_order_relaxed);
	return raw + kHeader;
}

void release(void* p)
{
	if (!p)
		return;
	unsigned char* raw = static_cast<unsigned char*>(p) - kHeader;
	const uint64_t bytes = *reinterpret_cast<uint64_t*>(raw);
	s_live.fetch_sub(1, std::memory_order_relaxed);
	s_bytes.fetch_sub(bytes, std::memory_order_relaxed);
	std::free(raw);
}

Snapshot snapshot()
{
	return {s_live.load(std::memory_order_relaxed), s_bytes.load(std::memory_order_relaxed), s_total.load(std::memory_order_relaxed)};
}

} // namespace mc2_alloc_probe
