// B7o.1: TG_TypeNode has a virtual destructor, so a TG_TypeShape-like derived node deleted through a TG_TypeNode* runs its own
// destructor once, and the explicit destroy() (idempotent) plus the destructor's destroy() release the stores exactly once.
#include "doctest.h"
#include "tgl.h"

#include <type_traits>
#include <vector>

// The base's out-of-line virtuals live in tgl_types.cpp (GL/GameOS dependent); this binary does not link it.
void TG_TypeNode::destroy (void) { init(); }
long TG_TypeNode::MakeFromHelper (BYTE*, const char*) { return 0; }
TG_Shape* TG_TypeNode::CreateFrom (void) { return nullptr; }
bool TG_TypeNode::LoadBinaryCopy (File&) { return true; }
void TG_TypeNode::SaveBinaryCopy (File&) {}

namespace {
int g_derivedDtors = 0;
int g_releases = 0;       // times destroy() found something to release
int g_destroyCalls = 0;

// Mirrors TG_TypeShape: owns vector stores, destroy() swaps them out (idempotent), the destructor calls destroy().
class CountingShape : public TG_TypeNode
{
	std::vector<int> store{1, 2, 3};
public:
	void destroy (void) override
	{
		++g_destroyCalls;
		if (!store.empty()) ++g_releases;
		std::vector<int>().swap(store);
	}
	~CountingShape (void) override { destroy(); ++g_derivedDtors; }
};
void reset() { g_derivedDtors = g_releases = g_destroyCalls = 0; }
}

static_assert(std::has_virtual_destructor_v<TG_TypeNode>, "TG_TypeNode must be deletable through the base pointer");
static_assert(std::has_virtual_destructor_v<TG_TypeShape>, "TG_TypeShape inherits the virtual destructor");

TEST_CASE("TG_TypeNode: delete through the base runs the derived destructor once")
{
	reset();
	TG_TypeNode* n = new CountingShape;
	delete n;
	CHECK(g_derivedDtors == 1);
	CHECK(g_destroyCalls == 1);
	CHECK(g_releases == 1);
}

TEST_CASE("TG_TypeNode: explicit destroy() before delete releases once (idempotent)")
{
	reset();
	TG_TypeNode* n = new CountingShape;
	n->destroy();
	CHECK(g_releases == 1);
	delete n;
	CHECK(g_derivedDtors == 1);
	CHECK(g_destroyCalls == 2);  // the old msl_load pattern: both calls run, the second finds nothing
	CHECK(g_releases == 1);      // the stores were released exactly once
}

TEST_CASE("TG_TypeNode: a bare helper node deletes through the base without a derived destructor")
{
	TG_TypeNode* n = new TG_TypeNode;
	n->init();
	delete n;
	CHECK(true);
}
