// B7o.4: queued waypoint markers (code/warrior_queue_markers.h, the helpers MechWarrior's queue API calls).
// MechWarrior cannot be linked into mc2_tests, so a model warrior drives the PRODUCTION helpers with the same sequence of record
// writes the production functions perform (addQueuedTacOrder / removeQueuedTacOrder / clearTacOrderQueue / enableTacOrderQueue /
// copyToData / copyFromData). After EACH operation the whole pool image (every byte-field of every record, including two guard
// records around the slices) is compared with an INDEPENDENT oracle: the legacy algorithm run on a plain array that has a safely
// allocated record after the last slice, so the legacy slot-16 read is executed in bounds, plus the one documented bounds-fix
// exception (a full-capacity dequeue leaves slot 15 as it was). A second, pure-legacy oracle proves exact equality until that
// exception first fires, and that the difference is then exactly slot 15 of the slice.
#include "doctest.h"
#include "warrior_queue_markers.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int N = 16;       // MAX_QUEUED_TACORDERS_PER_WARRIOR
constexpr int SLICES = 3;
constexpr int CAP = SLICES * N;   // MAX_QUEUED_TACORDERS (model)

struct Rec {                // same fields and order as QueuedTacOrder
	long id;
	float point[3];
	unsigned long packedData[2];
	unsigned char tactic;
	void* marker;
	unsigned char moveMode;
};

using Bytes = std::vector<unsigned char>;
void put (Bytes& b, const void* p, size_t n) { const unsigned char* c = static_cast<const unsigned char*>(p); b.insert(b.end(), c, c + n); }
// Field-wise image: padding bytes are not part of the comparison (assignment need not copy them).
void putRec (Bytes& b, const Rec& r)
{
	put(b, &r.id, sizeof r.id); put(b, r.point, sizeof r.point); put(b, r.packedData, sizeof r.packedData);
	put(b, &r.tactic, 1); put(b, &r.marker, sizeof r.marker); put(b, &r.moveMode, 1);
}
bool sameRec (const Rec& a, const Rec& b) { Bytes x, y; putRec(x, a); putRec(y, b); return x == y; }

// Distinct nonzero record for a seed.
Rec makeRec (int seed)
{
	Rec r;
	std::memset(&r, 0, sizeof r);
	r.id = 5000 + seed;
	r.point[0] = 1.5f + (float)seed; r.point[1] = 2.25f + (float)seed * 2.0f; r.point[2] = 3.125f + (float)seed * 3.0f;
	r.packedData[0] = 0x1000u + (unsigned)seed * 7u; r.packedData[1] = 0x2000u + (unsigned)seed * 13u;
	r.tactic = (unsigned char)(1 + seed % 200);
	r.marker = nullptr;
	r.moveMode = (unsigned char)(seed % 4);     // 0..2 own a marker, 3 owns none
	return r;
}

// ---- fake marker with registry (destructor counts, identities, double-free detection)
std::map<int, int> g_live;   // uid -> kind
int g_created = 0, g_destroyed = 0, g_nextUid = 1;
struct Mk {
	int uid, kind;
	Mk (int k) : uid(g_nextUid++), kind(k) { g_live[uid] = k; ++g_created; }
	~Mk (void) { CHECK(g_live.erase(uid) == 1); ++g_destroyed; }
};
using MkPtr = std::unique_ptr<Mk>;
void resetRegistry (void) { CHECK(g_live.empty()); g_live.clear(); g_created = g_destroyed = 0; g_nextUid = 1; }
MkPtr makeMarker (const Rec& r) { return r.moveMode < 3 ? MkPtr(new Mk(r.moveMode)) : MkPtr(); }
int uidOf (const MkPtr& p) { return p ? p->uid : 0; }

// ---- the production side: records in a pool buffer, owners beside them
struct Prod {
	Rec* pool;                  // [guard][SLICES slices][guard]
	Rec* queue = nullptr;
	char count = 0;
	long pos = 0;
	mc2::QueueMarkerArray<Mk, N> markers;
	explicit Prod (Rec* p) : pool(p) {}

	int enqueue (const Rec& src)    // addQueuedTacOrder: moveMode, point, tactic, packedData; the id is NOT written
	{
		if (count == N) return 2;
		Rec& r = queue[(int)count];
		r.moveMode = src.moveMode;
		if (src.moveMode < 3) markers[(size_t)count] = MkPtr(new Mk(src.moveMode));
		std::memcpy(r.point, src.point, sizeof r.point);
		r.tactic = src.tactic; r.packedData[0] = src.packedData[0]; r.packedData[1] = src.packedData[1];
		count++;
		return 0;
	}
	int dequeue (Rec& out)          // removeQueuedTacOrder
	{
		if (!queue) return 1;
		if (count == 0) return 2;
		out = queue[0];
		mc2::shiftQueueDown(queue, markers, (long)count);
		count--;
		return 0;
	}
	void clear (void) { mc2::resetQueueMarkers(markers); count = 0; }
	bool rebind (void) { return mc2::rebindQueue(queue, pool + 1, CAP, pos, count, markers); }
	std::vector<Rec> save (void)    // copyToData: all 16 records, marker nulled
	{
		std::vector<Rec> img(queue, queue + N);
		for (auto& r : img) r.marker = nullptr;
		return img;
	}
	void restore (const std::vector<Rec>& img, char savedCount)   // copyFromData
	{
		count = savedCount;
		for (int i = 0; i < N; i++) queue[i] = img[(size_t)i];
		for (int i = 0; i < N; i++) queue[i].marker = nullptr;
		mc2::restoreQueueMarkers(queue, markers, (long)count, [](const Rec& r) { return makeMarker(r); });
	}
};

// ---- the oracle: legacy algorithm on a plain array with a safe record after the last slice; markers as a logical uid list
struct Oracle {
	std::vector<Rec> a;         // [guard][SLICES*N][guard]
	long off = -1;              // absolute index of slot 0 of the bound slice, -1 = unbound
	char count = 0;
	long pos = 0;
	std::vector<int> list;      // marker uid or 0 per active slot
	bool exceptionApplied;
	explicit Oracle (bool applyException) : a((size_t)CAP + 2), exceptionApplied(applyException) {}

	bool rebind (void)
	{
		if (CAP - pos < N) return false;
		list.clear();
		off = 1 + pos; pos += N; count = 0;
		return true;
	}
	int enqueue (const Rec& src, int uid)
	{
		if (count == N) return 2;
		Rec& r = a[(size_t)(off + count)];
		r.moveMode = src.moveMode;
		std::memcpy(r.point, src.point, sizeof r.point);
		r.tactic = src.tactic; r.packedData[0] = src.packedData[0]; r.packedData[1] = src.packedData[1];
		list.push_back(uid);
		count++;
		return 0;
	}
	int dequeue (Rec& out)
	{
		if (off < 0) return 1;
		if (count == 0) return 2;
		out = a[(size_t)off];
		const Rec before15 = a[(size_t)(off + N - 1)];
		for (long i = 0; i < count; i++)            // the legacy loop, verbatim; slot N is in bounds (neighbour or trailing guard)
			a[(size_t)(off + i)] = a[(size_t)(off + i + 1)];
		if (exceptionApplied && count == N)
			a[(size_t)(off + N - 1)] = before15;    // the declared bounds-fix exception
		list.erase(list.begin());
		count--;
		return 0;
	}
	void clear (void) { list.clear(); count = 0; }
};

struct Rig {
	std::unique_ptr<Rec[]> poolStore;   // heap block of exactly the model size: ASan redzones sit right after the trailing guard
	Prod prod;
	Oracle exact{true};
	Oracle pure{false};
	bool diverged = false;              // pure legacy no longer equals production (the exception fired)
	std::vector<Rec> savedImage; char savedCount = 0;
	int enq = 0;                        // seed counter
	Rig (void) : poolStore(new Rec[(size_t)CAP + 2]), prod(poolStore.get())
	{
		for (int k = 0; k < CAP + 2; k++) {         // neighbours: distinct, nonzero, identical in all three images
			poolStore[(size_t)k] = makeRec(900 + k);
			exact.a[(size_t)k] = pure.a[(size_t)k] = poolStore[(size_t)k];
		}
	}
	static Bytes image (const Rec* r, size_t n) { Bytes b; for (size_t i = 0; i < n; i++) putRec(b, r[i]); return b; }

	void bind (long startPos)
	{
		prod.pos = exact.pos = pure.pos = startPos;
		CHECK(prod.rebind()); CHECK(exact.rebind()); CHECK(pure.rebind());
		check("bind");
	}

	std::vector<int> prodList (void) { std::vector<int> v; for (int i = 0; i < prod.count; i++) v.push_back(uidOf(prod.markers[(size_t)i])); return v; }

	void check (const char* what)
	{
		INFO("after " << what);
		const size_t P = (size_t)CAP + 2;
		const Bytes pi = image(prod.pool, P);
		CHECK(pi == image(exact.a.data(), P));                  // whole pool: slices, neighbours, guards
		if (!diverged) CHECK(pi == image(pure.a.data(), P));    // exact legacy equality until the exception first fires
		CHECK(prod.count == exact.count);
		CHECK(prod.pos == exact.pos);
		// owners: slot < count matches the oracle list (identity), every other slot is null (tail owner null)
		CHECK(prodList() == exact.list);
		int nonNull = 0;
		for (int i = 0; i < N; i++) {
			if (i >= prod.count) CHECK(!prod.markers[(size_t)i]);
			else if (prod.markers[(size_t)i]) { CHECK(prod.markers[(size_t)i]->kind == prod.queue[i].moveMode); ++nonNull; }
		}
		CHECK((int)g_live.size() == nonNull);                   // nothing leaked, nothing orphaned
		// the stale id: enqueue never writes it, peek reads it
		if (prod.count > 0) {
			CHECK(prod.queue[0].id == exact.a[(size_t)exact.off].id);
			CHECK(prod.queue[(int)prod.count - 1].id == exact.a[(size_t)(exact.off + exact.count - 1)].id);
		}
	}

	void op (char c)
	{
		Rec o1, o2, o3;
		const bool usePure = !diverged;     // after the exception fires only the exception oracle is exact
		switch (c) {
		case 'E': {
			const Rec r = makeRec(enq++);
			const int rp = prod.enqueue(r);
			const int uid = (rp == 0 && r.moveMode < 3) ? uidOf(prod.markers[(size_t)prod.count - 1]) : 0;
			CHECK(exact.enqueue(r, uid) == rp);
			if (usePure) CHECK(pure.enqueue(r, uid) == rp);
			check("enqueue");
			break; }
		case 'D': {
			const bool full = prod.count == N;
			const Rec nb = exact.off >= 0 ? exact.a[(size_t)(exact.off + N)] : Rec{};
			const Rec s15 = exact.off >= 0 ? exact.a[(size_t)(exact.off + N - 1)] : Rec{};
			const int rp = prod.dequeue(o1);
			CHECK(exact.dequeue(o2) == rp);
			if (usePure) CHECK(pure.dequeue(o3) == rp);
			if (rp == 0) { CHECK(sameRec(o1, o2)); if (usePure) CHECK(sameRec(o1, o3)); }   // unchanged active-order behaviour
			if (full && usePure) {
				// The exception fires here: production and pure legacy differ in exactly slot 15 of the slice.
				diverged = true;
				CHECK(!sameRec(nb, s15));                                        // the sentinel is distinguishable
				CHECK(sameRec(pure.a[(size_t)(pure.off + N - 1)], nb));          // legacy copied the neighbour in
				CHECK(sameRec(prod.queue[N - 1], s15));                          // production leaves slot 15 alone
				for (size_t i = 0; i < (size_t)CAP + 2; i++)
					if (i != (size_t)(exact.off + N - 1)) CHECK(sameRec(prod.pool[i], pure.a[i]));
			}
			check("dequeue");
			break; }
		case 'C': prod.clear(); exact.clear(); if (usePure) pure.clear(); check("clear"); break;
		case 'R': {
			const bool rp = prod.rebind();
			CHECK(exact.rebind() == rp); if (usePure) CHECK(pure.rebind() == rp);
			check(rp ? "rebind" : "rebind (refused)");
			break; }
		case 'S':
			savedImage = prod.save(); savedCount = prod.count;
			CHECK(image(savedImage.data(), N) == image(exact.a.data() + exact.off, N));   // the file carries all 16 records
			for (auto& r : savedImage) CHECK(r.marker == nullptr);
			break;
		case 'L': {
			prod.restore(savedImage, savedCount);
			for (int i = 0; i < N; i++) { exact.a[(size_t)(exact.off + i)] = savedImage[(size_t)i]; if (usePure) pure.a[(size_t)(pure.off + i)] = savedImage[(size_t)i]; }
			exact.count = savedCount; pure.count = savedCount;
			exact.list.clear(); pure.list.clear();
			for (int i = 0; i < savedCount; i++) { exact.list.push_back(uidOf(prod.markers[(size_t)i])); pure.list.push_back(uidOf(prod.markers[(size_t)i])); }
			check("restore");
			break; }
		}
	}

	// "E*16 D C R S L"
	void run (const std::string& history)
	{
		std::istringstream in(history);
		std::string tok;
		while (in >> tok) {
			int times = 1;
			const size_t star = tok.find('*');
			if (star != std::string::npos) times = std::atoi(tok.c_str() + star + 1);
			for (int i = 0; i < times; i++) op(tok[0]);
		}
	}
};

const char* const HISTORIES[] = {
	"E*3 D S C E L D*2 D",                          // basic enqueue, dequeue, save, clear, restore, empty dequeue
	"E*16 D*16",                                    // full capacity, then to empty
	"E*16 D E*3 D*2 C E*2 D*3",                     // exception, then re-enqueue/dequeue (the changed slot 15 propagates)
	"E*16 D C E*2 S D D E*16 D S L D*5",            // clear after the exception, save/restore across it
	"E*16 D R E*4 D S R E*2 L D*4",                 // rebind after the exception, restore over live markers
	"E*5 S D*2 L C L E*16 D*3 L",                   // restore round trips, restore into a full queue
	"E*16 D*2 E*2 D*16 E*16 D*16",                  // repeated full-capacity cycles
	"E*17 D R R R E*3 D C",                         // enqueue refused at capacity, rebinds to and past the last slice
};

} // namespace

TEST_CASE("queue markers: byte-level oracle histories on the first slice")
{
	for (const char* h : HISTORIES) {
		INFO("history: " << std::string(h));
		resetRegistry();
		{
			Rig rig;
			rig.bind(0);
			rig.run(h);
		}
		CHECK(g_live.empty());                         // destructor count == created count
		CHECK(g_created == g_destroyed);
	}
}

TEST_CASE("queue markers: byte-level oracle histories on a MIDDLE slice (neighbour slices hold live-looking records)")
{
	for (const char* h : HISTORIES) {
		INFO("history: " << std::string(h));
		resetRegistry();
		{
			Rig rig;
			rig.bind(N);
			rig.run(h);
		}
		CHECK(g_live.empty());
		CHECK(g_created == g_destroyed);
	}
}

TEST_CASE("queue markers: the FINAL pool slice (the legacy slot-16 read would leave the array); run under ASan")
{
	for (const char* h : HISTORIES) {
		INFO("history: " << std::string(h));
		resetRegistry();
		{
			Rig rig;
			rig.bind(CAP - N);                         // the last slice: slot 16 would be the trailing guard
			rig.run(h);
			CHECK(sameRec(rig.prod.pool[CAP + 1], makeRec(900 + CAP + 1)));       // trailing guard untouched
			CHECK(sameRec(rig.prod.pool[0], makeRec(900)));                       // leading guard untouched
		}
		CHECK(g_live.empty());
		CHECK(g_created == g_destroyed);
	}
}

TEST_CASE("queue markers: shifted markers keep order and identity; the tail owner is null; the head is destroyed once")
{
	resetRegistry();
	Rig rig;
	rig.bind(0);
	rig.run("E*16");
	std::vector<int> uids;
	for (int i = 0; i < 16; i++) uids.push_back(uidOf(rig.prod.markers[(size_t)i]));
	const int d0 = g_destroyed;
	Rec out;
	CHECK(rig.prod.dequeue(out) == 0);
	CHECK(g_destroyed == d0 + (uids[0] ? 1 : 0));
	for (int i = 0; i < 15; i++) CHECK(uidOf(rig.prod.markers[(size_t)i]) == uids[(size_t)i + 1]);
	CHECK(!rig.prod.markers[15]);
	rig.prod.clear();
	CHECK(g_live.empty());
}

TEST_CASE("queue markers: failed rebind (exhausted pool) preserves records, count, pointer and every owner")
{
	resetRegistry();
	{
		Rig rig;
		rig.bind(CAP - N);                              // the last slice; the cursor is now at capacity
		rig.run("E*4 D");
		const Bytes before = Rig::image(rig.prod.pool, (size_t)CAP + 2);
		const std::vector<int> owners = rig.prodList();
		Rec* q = rig.prod.queue;
		const int d0 = g_destroyed;
		CHECK_FALSE(rig.prod.rebind());
		CHECK(g_destroyed == d0);                       // destructor count 0
		CHECK(rig.prod.queue == q);
		CHECK(rig.prod.count == 3);
		CHECK(rig.prodList() == owners);                // order and identity
		CHECK(Rig::image(rig.prod.pool, (size_t)CAP + 2) == before);
		rig.run("D C E*2 D");                           // a normal dequeue and clear still work
	}
	CHECK(g_live.empty());
}

TEST_CASE("queue markers: successful rebind destroys the first binding's markers once and leaves the new slice's records alone")
{
	resetRegistry();
	{
		Rig rig;
		rig.bind(0);
		rig.run("E*3");
		const int live = (int)g_live.size();
		const int d0 = g_destroyed;
		const Bytes slice1 = Rig::image(rig.prod.pool + 1 + N, N);
		CHECK(rig.prod.rebind());
		CHECK(g_destroyed == d0 + live);
		CHECK(Rig::image(rig.prod.queue, N) == slice1);
		CHECK(rig.prod.count == 0);
	}
	CHECK(g_live.empty());
}

TEST_CASE("queue markers: restore over live markers releases each once and rebuilds only queued slots")
{
	resetRegistry();
	{
		Rig rig;
		rig.bind(0);
		rig.run("E*4 S E*3");
		const int liveBefore = (int)g_live.size();
		const int d0 = g_destroyed;
		rig.op('L');
		CHECK(g_destroyed == d0 + liveBefore);
		int owned = 0;
		for (int i = 0; i < 4; i++) owned += rig.prod.queue[i].moveMode < 3 ? 1 : 0;
		CHECK((int)g_live.size() == owned);
		for (int i = 4; i < N; i++) CHECK(!rig.prod.markers[(size_t)i]);
		rig.run("C");
		CHECK(g_live.empty());
	}
}

TEST_CASE("queue markers: stale id - enqueue leaves it, the record shift moves it with the record")
{
	resetRegistry();
	Rig rig;
	rig.bind(0);
	const long idSlot0 = rig.prod.queue[0].id, idSlot1 = rig.prod.queue[1].id;
	rig.run("E*2");
	CHECK(rig.prod.queue[0].id == idSlot0);             // enqueue did not write it
	CHECK(rig.prod.queue[1].id == idSlot1);
	rig.run("D");
	CHECK(rig.prod.queue[0].id == idSlot1);             // peek of the new head now reads the old second slot's id
	rig.prod.clear();
}
