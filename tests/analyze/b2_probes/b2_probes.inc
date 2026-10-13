// ===== STEP 4 B2 PROBES P1 (sort ties) and P2 (A* open list) -- appended to code/sim_identity.cpp by apply_probes.py =====
// An UNCOMMITTED patch on a scratch worktree: it observes the OLD SortList / PriorityQueue and never changes what they return.
// Output goes to stdout in the same stream as the [SIMID v1] lines, so a probe line belongs to the sim frame of the NEXT [SIMID v1] line.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

extern "C" {
const char* g_b2Site = "unset";
const char* g_b2Solver = "unset";
}

namespace b2probe
{
struct P1Site
{
	long calls = 0, maxN = 0, tieCalls = 0, diffCalls = 0, nanCalls = 0, infCalls = 0, leakCalls = 0, cap = 0;
};
std::map<std::string, P1Site>& p1Sites()
{
	static std::map<std::string, P1Site> sites;
	return sites;
}
std::vector<std::string>& p1Dirty()
{
	static std::vector<std::string> dirty;
	return dirty;
}
int g_p1Events = 0;

struct B2Node
{
	float value;
	unsigned long id;
};

std::string describe(const B2Node* nodes, size_t count)
{
	std::string out;
	for (size_t i = 0; i < count && i < 12; ++i)
	{
		char buf[48];
		std::snprintf(buf, sizeof buf, "%s%lu:%g", i ? "," : "", nodes[i].id, double(nodes[i].value));
		out += buf;
	}
	return out;
}

struct Fnv
{
	uint64_t h = 1469598103934665603ull;
	void word(uint32_t w)
	{
		for (int i = 0; i < 4; ++i)
		{
			h ^= (w >> (8 * i)) & 0xff;
			h *= 1099511628211ull;
		}
	}
};

struct P2Session
{
	const char* solver = "unset";
	bool active = false;
	long inserts = 0, removes = 0, changes = 0, finds = 0, find0 = 0, maxOpen = 0, chgMaxIdx = 0, chgPastN = 0, chgPastCap = 0;
	int32_t minKey = INT32_MAX;
	long cap = 0;
	Fnv ops;
};
P2Session g_p2;
long g_p2Hits = 0;

void p2Flush()
{
	if (!g_p2.active || (g_p2.inserts == 0 && g_p2.removes == 0))
		return;
	std::printf("[B2P2] solver=%s inserts=%ld removes=%ld changes=%ld finds=%ld find0=%ld maxopen=%ld chgmax=%ld chg_gt_n=%ld chg_ge_cap=%ld minkey=%d cap=%ld ophash=%016llx\n", g_p2.solver,
		g_p2.inserts, g_p2.removes, g_p2.changes, g_p2.finds, g_p2.find0, g_p2.maxOpen, g_p2.chgMaxIdx, g_p2.chgPastN, g_p2.chgPastCap, g_p2.minKey == INT32_MAX ? 0 : g_p2.minKey, g_p2.cap,
		static_cast<unsigned long long>(g_p2.ops.h));
}
} // namespace b2probe

// qsort with the OLD comparator, observed. WHY the filled prefix is "the leading run of non-sentinel values": every live site fills slots 0..k-1 after
// clear(), whose sentinel matches the direction.
extern "C" void b2P1Sort(void* base, size_t n, size_t /*width*/, int (*cmp)(const void*, const void*), int descending)
{
	using namespace b2probe;
	B2Node* list = static_cast<B2Node*>(base);
	const float sentinel = descending ? -3.4E38f : 3.4E38f;
	size_t k = 0;
	while (k < n && list[k].value != sentinel)
		++k;
	std::vector<B2Node> ref(list, list + k);
	std::stable_sort(ref.begin(), ref.end(), [&](const B2Node& a, const B2Node& b) {
		const bool na = std::isnan(a.value), nb = std::isnan(b.value);
		if (na || nb)
			return !na && nb;
		return descending ? a.value > b.value : a.value < b.value;
	});
	bool ties = false;
	long nanCount = 0, infCount = 0;
	for (size_t i = 0; i < k; ++i)
	{
		if (std::isnan(ref[i].value))
			++nanCount;
		else if (std::isinf(ref[i].value))
			++infCount;
		if (i && !std::isnan(ref[i].value) && ref[i].value == ref[i - 1].value)
			ties = true;
	}
	std::vector<B2Node> before(list, list + (k < 12 ? k : 12));
	std::qsort(base, n, sizeof(B2Node), cmp);
	bool diff = false, leak = false;
	for (size_t i = 0; i < k; ++i)
	{
		const bool bothNaN = std::isnan(list[i].value) && std::isnan(ref[i].value);
		if (list[i].id != ref[i].id || (list[i].value != ref[i].value && !bothNaN))
			diff = true;
		if (list[i].value == sentinel)
			leak = true;
	}
	P1Site& s = p1Sites()[g_b2Site];
	++s.calls;
	s.cap = static_cast<long>(n);
	if (long(k) > s.maxN)
		s.maxN = long(k);
	s.tieCalls += ties;
	s.diffCalls += diff;
	s.nanCalls += nanCount > 0;
	s.infCalls += infCount > 0;
	s.leakCalls += leak;
	std::vector<std::string>& dirty = p1Dirty();
	if (std::find(dirty.begin(), dirty.end(), g_b2Site) == dirty.end())
		dirty.push_back(g_b2Site);
	if (diff && g_p1Events < 60)
	{
		++g_p1Events;
		std::printf("[B2P1 event] site=%s n=%zu cap=%zu desc=%d qsort=%s rule=%s input=%s\n", g_b2Site, k, n, descending, describe(list, k).c_str(), describe(ref.data(), k).c_str(),
			describe(before.data(), before.size()).c_str());
	}
}

// One cumulative row per site whose counters moved in the frame just finished, ahead of that frame's [SIMID v1] line.
extern "C" void b2P1Frame(long simFrame)
{
	using namespace b2probe;
	for (const std::string& name : p1Dirty())
	{
		const P1Site& s = p1Sites()[name];
		std::printf("[B2P1] simframe=%ld site=%s calls=%ld maxn=%ld ties=%ld diff=%ld nan=%ld inf=%ld leak=%ld cap=%ld\n", simFrame, name.c_str(), s.calls, s.maxN, s.tieCalls, s.diffCalls, s.nanCalls,
			s.infCalls, s.leakCalls, s.cap);
	}
	p1Dirty().clear();
	if (simFrame % 600 == 0)
		p2Flush();
}

extern "C" void b2P2Clear(void)
{
	using namespace b2probe;
	p2Flush();
	g_p2 = P2Session();
	g_p2.solver = g_b2Solver;
	g_p2.active = true;
}
extern "C" void b2P2Insert(uint32_t id, int32_t key, int numItems, int maxItems)
{
	using namespace b2probe;
	++g_p2.inserts;
	g_p2.cap = maxItems;
	g_p2.maxOpen = numItems > g_p2.maxOpen ? numItems : g_p2.maxOpen;
	g_p2.minKey = key < g_p2.minKey ? key : g_p2.minKey;
	g_p2.ops.word(1);
	g_p2.ops.word(id);
	g_p2.ops.word(uint32_t(key));
}
extern "C" void b2P2Remove(uint32_t id, int32_t key)
{
	using namespace b2probe;
	++g_p2.removes;
	g_p2.ops.word(2);
	g_p2.ops.word(id);
	g_p2.ops.word(uint32_t(key));
}
extern "C" void b2P2Change(int index, int newKey, int numItems, int maxItems)
{
	using namespace b2probe;
	++g_p2.changes;
	g_p2.chgMaxIdx = index > g_p2.chgMaxIdx ? index : g_p2.chgMaxIdx;
	g_p2.chgPastN += index > numItems;
	g_p2.chgPastCap += index >= maxItems;
	g_p2.minKey = newKey < g_p2.minKey ? newKey : g_p2.minKey;
	g_p2.ops.word(3);
	g_p2.ops.word(uint32_t(index));
	g_p2.ops.word(uint32_t(newKey));
}
extern "C" void b2P2Find(int found)
{
	using namespace b2probe;
	++g_p2.finds;
	g_p2.find0 += found == 0;
}
// GlobalMap::propogateCost: the open branch re-keys slot `door` instead of slot `openIndex`.
extern "C" void b2P2Hit(long door, long openIndex, int numItems)
{
	using namespace b2probe;
	if (door == openIndex)
		return;
	++g_p2Hits;
	std::printf("[B2P2 hit] solver=%s door=%ld openIndex=%ld numItems=%d stale=%d past5002=%d total=%ld\n", g_b2Solver, door, openIndex, numItems, door > numItems, door >= 5002, g_p2Hits);
}
// ===== end of B2 probes =====
