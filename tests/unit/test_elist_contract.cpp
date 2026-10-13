// tests/unit/test_elist_contract.cpp
//
// Characterization of mclib/elist.h AS IT IS TODAY (step 4 batch B2.0, .claude/cleanup/cxx23-step4-b2-notes.md section 2, HE1-HE12).
// WHY: B2.2/B2.3 replace EList by std::vector / std::deque / composition. Call sites rely on properties the class never documented (element addresses
// that stay valid across Append and Delete of OTHER elements, End() being the tail element and not past-the-end, postfix ++ returning the new position,
// Insert-before, [a,b) ranges, deep copy of values). Each case states one property; the cases that name a std container run the same body on it so the
// difference (the property `vector` loses) is on record.

#include "doctest.h"

#include "elist.h"

#include <deque>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
using IntList = EList<int, const int>;

IntList make(std::initializer_list<int> values)
{
	IntList list;
	for (int v : values)
		list.Append(v);
	return list;
}

std::vector<int> toVector(const IntList& list)
{
	std::vector<int> out;
	for (auto it = list.Begin(); !it.IsDone(); it++)
		out.push_back(*it);
	return out;
}

// A value type that records its copies and the order of its destructors (HE7, HE12).
struct Tracked
{
	int v;
	static int copies;
	static std::vector<int>* destroyed;
	explicit Tracked(int x) : v(x) {}
	Tracked(const Tracked& o) : v(o.v) { ++copies; }
	~Tracked()
	{
		if (destroyed)
			destroyed->push_back(v);
	}
	bool operator==(const Tracked& o) const { return v == o.v; }
};
int Tracked::copies = 0;
std::vector<int>* Tracked::destroyed = nullptr;
using TrackedList = EList<Tracked, const Tracked&>;
} // namespace

TEST_SUITE("elist_contract")
{

TEST_CASE("HE1 an empty list: Begin, End and Find are INVALID (IsDone), Count is 0")
{
	IntList list;
	CHECK(list.Count() == 0);
	CHECK(list.IsEmpty());
	CHECK(list.Begin().IsDone());
	CHECK(list.End().IsDone());
	CHECK(list.Find(5).IsDone());
}

TEST_CASE("HE2 End() is the TAIL ELEMENT, and a reverse loop with -- stops through IsDone")
{
	IntList list = make({1, 2, 3});
	CHECK(*list.End() == 3);
	std::vector<int> reverse;
	for (auto it = list.End(); !it.IsDone(); it--)
		reverse.push_back(*it);
	CHECK(reverse == std::vector<int>{3, 2, 1});
	// The forward loop ends when the iterator becomes null, not at End().
	CHECK(toVector(list) == std::vector<int>{1, 2, 3});
}

TEST_CASE("HE3 postfix ++ and -- return the NEW position")
{
	IntList list = make({10, 20, 30});
	auto it = list.Begin();
	CHECK((it++).Item() == 20);
	CHECK(it.Item() == 20);
	CHECK((it--).Item() == 10);
	CHECK(it.Item() == 10);
	auto jump = list.Begin();
	jump += 2;
	CHECK(jump.Item() == 30);
	jump -= 1;
	CHECK(jump.Item() == 20);
}

TEST_CASE("HE4 Insert goes BEFORE the position; Insert(x, Pos) is false for Pos >= Count, even on an empty list")
{
	IntList list = make({1, 3});
	CHECK(list.Insert(2, list.Iterator(1)));
	CHECK(toVector(list) == std::vector<int>{1, 2, 3});
	CHECK(list.Insert(0, 0ul));
	CHECK(toVector(list) == std::vector<int>{0, 1, 2, 3});
	CHECK_FALSE(list.Insert(9, 4ul)); // Pos == Count
	CHECK_FALSE(list.Insert(9, 99ul));
	CHECK(list.Count() == 4);

	IntList empty;
	CHECK_FALSE(empty.Insert(1, 0ul));
	CHECK(empty.Count() == 0);

	// Insert(list, iter): the source keeps its order, before the iterator.
	IntList base = make({1, 5});
	IntList src = make({2, 3, 4});
	CHECK(base.Insert(src, base.Iterator(1)));
	CHECK(toVector(base) == std::vector<int>{1, 2, 3, 4, 5});
	CHECK(toVector(src) == std::vector<int>{2, 3, 4});
}

TEST_CASE("HE4b Prepend, Append(list), DeleteHead, DeleteTail, GetHead, GetTail, Replace")
{
	IntList list = make({2, 3});
	list.Prepend(1);
	CHECK(list.GetHead() == 1);
	CHECK(list.GetTail() == 3);
	IntList more = make({4, 5});
	list.Append(more);
	CHECK(toVector(list) == std::vector<int>{1, 2, 3, 4, 5});
	list.DeleteHead();
	list.DeleteTail();
	CHECK(toVector(list) == std::vector<int>{2, 3, 4});
	list.Replace(30, list.Iterator(1));
	CHECK(toVector(list) == std::vector<int>{2, 30, 4});
}

TEST_CASE("HE5 Delete(a, b) removes [a, b) although its comment says last; Delete(a, a) removes nothing")
{
	IntList list = make({1, 2, 3, 4, 5});
	CHECK(list.Delete(list.Iterator(1), list.Iterator(3)));
	CHECK(toVector(list) == std::vector<int>{1, 4, 5});

	IntList same = make({1, 2, 3});
	auto it = same.Iterator(1);
	CHECK(same.Delete(it, it)); // asserts in a debug build (gosASSERT); in release it is a no-op that reports success
	CHECK(same.Count() == 3);

	// Delete(iter) frees that node: the neighbours link up.
	IntList three = make({1, 2, 3});
	CHECK(three.Delete(three.Iterator(1)));
	CHECK(toVector(three) == std::vector<int>{1, 3});
	CHECK(three.Delete(0ul));
	CHECK(toVector(three) == std::vector<int>{3});
	CHECK(three.Delete(three.Begin()));
	CHECK(three.IsEmpty());
}

TEST_CASE("HE6 ADDRESS STABILITY: a reference taken before Append or a Delete of ANOTHER element stays valid")
{
	// This is the property std::vector loses and the reason by-value lists whose addresses escape become std::deque.
	EList<int, const int> list;
	list.Append(1);
	list.Append(2);
	list.Append(3);
	const int* second = &list.Iterator(1).Item();
	for (int i = 0; i < 1000; ++i)
		list.Append(i);
	CHECK(second == &list.Iterator(1).Item());
	CHECK(*second == 2);
	list.Delete(list.Iterator(0));
	CHECK(second == &list.Iterator(0).Item()); // the node did not move; its position did
	CHECK(*second == 2);

	// std::deque: push_back and pop_back keep references (all the escaping lists need); an erase in the middle does not.
	std::deque<int> d = {1, 2, 3};
	const int* dsecond = &d[1];
	for (int i = 0; i < 1000; ++i)
		d.push_back(i);
	CHECK(dsecond == &d[1]);
	d.pop_back();
	CHECK(dsecond == &d[1]);
}

TEST_CASE("HE7 copy and assignment are deep for values, shallow for pointers; self-assignment EMPTIES the list")
{
	Tracked::copies = 0;
	TrackedList a;
	a.Append(Tracked(1));
	a.Append(Tracked(2));
	const int before = Tracked::copies;
	TrackedList b = a;
	CHECK(Tracked::copies == before + 2); // every element copied
	CHECK(b.Count() == 2);
	CHECK(&b.GetHead() != &a.GetHead());

	int x = 1, y = 2;
	EList<int*, int*> pa;
	pa.Append(&x);
	pa.Append(&y);
	EList<int*, int*> pb = pa;
	CHECK(pb.GetHead() == &x); // the pointers are shared, the pointees are not copied

	IntList c = make({1, 2, 3});
	IntList& alias = c;
	c = alias;
	CHECK(c.Count() == 0);
}

TEST_CASE("HE8 Find uses operator==, returns the first match, and honours Start_Index")
{
	IntList list = make({4, 7, 4, 9});
	CHECK(list.Find(4).Item() == 4);
	CHECK((list.Find(4) == list.Begin()));
	auto second4 = list.Find(4, 1ul);
	REQUIRE(second4.IsValid());
	CHECK((second4 == list.Iterator(2)));
	CHECK(list.Find(5).IsDone());
	CHECK((list.Find(9, list.Iterator(1)) == list.Iterator(3)));
}

TEST_CASE("HE9 operator[] and Iterator(Pos) are positional")
{
	IntList list = make({5, 6, 7});
	CHECK(list[0ul] == 5);
	CHECK(list[2ul] == 7);
	CHECK(list.Iterator(1).Item() == 6);
	CHECK(std::as_const(list).Get(1ul) == 6);
	CHECK(list.Exists(2));
	CHECK_FALSE(list.Exists(3));
}

TEST_CASE("HE10 Count() is 32-bit unsigned (unsigned long on this ABI)")
{
	IntList list;
	static_assert(std::is_same_v<decltype(list.Count()), unsigned long>);
	CHECK(sizeof(list.Count()) == 4);
}

TEST_CASE("HE12 element destructors run head to tail on Clear and on destruction")
{
	std::vector<int> order;
	Tracked::destroyed = &order;
	{
		TrackedList list;
		list.Append(Tracked(1));
		list.Append(Tracked(2));
		list.Append(Tracked(3));
		order.clear(); // the temporaries of Append
		list.Clear();
		CHECK(order == std::vector<int>{1, 2, 3});
		list.Append(Tracked(4));
		list.Append(Tracked(5));
		order.clear();
	}
	CHECK(order == std::vector<int>{4, 5});
	Tracked::destroyed = nullptr;
}

TEST_CASE("HE12b DeleteHead / Delete(iter) destroy exactly the removed element")
{
	std::vector<int> order;
	Tracked::destroyed = &order;
	TrackedList list;
	list.Append(Tracked(1));
	list.Append(Tracked(2));
	list.Append(Tracked(3));
	order.clear();
	list.Delete(list.Iterator(1));
	CHECK(order == std::vector<int>{2});
	Tracked::destroyed = nullptr;
}

} // TEST_SUITE
