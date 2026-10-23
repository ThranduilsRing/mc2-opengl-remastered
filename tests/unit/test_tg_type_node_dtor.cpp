// B7o.1 (+ B7o.6b real TG_TypeShape cleanup at the end): TG_TypeNode has a virtual destructor, so a TG_TypeShape-like derived node deleted through a TG_TypeNode* runs its own
// destructor once, and the explicit destroy() (idempotent) plus the destructor's destroy() release the stores exactly once.
#include "doctest.h"
#include "tgl.h"

#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

// The base's out-of-line virtuals live in tgl_types.cpp (GL/GameOS dependent); this binary does not link it.
void TG_TypeNode::destroy (void) { init(); }
long TG_TypeNode::MakeFromHelper (BYTE*, const char*) { return 0; }
std::unique_ptr<TG_Shape> TG_TypeNode::CreateFrom (void) { return nullptr; }
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

// ---------------------------------------------------------------------------------------------------------------------------
// B7o.6b: REAL TG_TypeShape cleanup. TG_TypeShape::destroy() is the production body (mclib/tgl_typeshape_destroy.cpp, linked
// into this binary); the two gos release calls are counted by the stubs below. A probe subclass only fills the protected stores
// and handles; the node is owned by a unique_ptr<TG_TypeNode> exactly as TG_TypeMultiShape::typeNodeStore owns it.
namespace {
int g_bufferReleases = 0;
int g_declReleases = 0;

class TypeShapeProbe : public TG_TypeShape
{
public:
	static TypeShapeProbe* last;
	TypeShapeProbe()
	{
		last = this;
		typeVertexStore.resize(4);
		listOfTypeVertices = typeVertexStore.data();
		typeTriangleStore.resize(2);
		listOfTypeTriangles = typeTriangleStore.data();
		tinyTextureStore.resize(1);
		listOfTextures = tinyTextureStore.data();
		numTypeVertices = 4;
		numTypeTriangles = 2;
		numTextures = 1;
		vb_ = reinterpret_cast<HGOSBUFFER>(static_cast<uintptr_t>(0x10));
		ib_ = reinterpret_cast<HGOSBUFFER>(static_cast<uintptr_t>(0x20));
		vdecl_ = reinterpret_cast<HGOSVERTEXDECLARATION>(static_cast<uintptr_t>(0x30));
	}
	bool released() const
	{
		return typeVertexStore.empty() && typeVertexStore.capacity() == 0 && typeTriangleStore.capacity() == 0 && tinyTextureStore.capacity() == 0 &&
			listOfTypeVertices == nullptr && listOfTypeTriangles == nullptr && listOfTextures == nullptr && numTypeVertices == 0 && numTypeTriangles == 0 &&
			numTextures == 0 && !vb_ && !ib_ && !vdecl_;
	}
};
TypeShapeProbe* TypeShapeProbe::last = nullptr;
}

void __stdcall gos_DestroyBuffer (HGOSBUFFER) { ++g_bufferReleases; }
void __stdcall gos_DestroyVertexDeclaration (HGOSVERTEXDECLARATION) { ++g_declReleases; }

// The other TG_TypeShape virtuals live in tgl_types.cpp (GL/GameOS dependent); they are never called here.
long TG_TypeShape::ParseASEFile (BYTE*, const char*) { return 0; }
long TG_TypeShape::MakeFromHelper (BYTE*, const char*) { return 0; }
long TG_TypeShape::SetTextureHandle (DWORD, DWORD) { return 0; }
long TG_TypeShape::SetTextureAlpha (DWORD, bool) { return 0; }
void TG_TypeShape::CreateListOfTextures (TG_TexturePtr, DWORD) {}
void TG_TypeShape::movePosRelativeCenterNode (void) {}
std::unique_ptr<TG_Shape> TG_TypeShape::CreateFrom (void) { return nullptr; }
bool TG_TypeShape::LoadBinaryCopy (File&) { return true; }
void TG_TypeShape::SaveBinaryCopy (File&) {}

TEST_CASE("TG_TypeShape: the real destroy() releases stores and GPU handles once, through a unique_ptr<TG_TypeNode>")
{
	g_bufferReleases = g_declReleases = 0;
	{
		std::unique_ptr<TG_TypeNode> owner = std::make_unique<TypeShapeProbe>();
		TypeShapeProbe* probe = TypeShapeProbe::last;
		REQUIRE(probe != nullptr);
		CHECK_FALSE(probe->released());

		// A second destroy() is a no-op: the stores are already empty and the handles null.
		owner->destroy();
		CHECK(probe->released());
		CHECK(g_bufferReleases == 2);
		CHECK(g_declReleases == 1);
		owner->destroy();
		CHECK(probe->released());
		CHECK(g_bufferReleases == 2);
		CHECK(g_declReleases == 1);
	}
	// The owner's destructor ran ~TG_TypeShape -> destroy() a third time: still nothing released twice.
	CHECK(g_bufferReleases == 2);
	CHECK(g_declReleases == 1);

	g_bufferReleases = g_declReleases = 0;
	{
		std::vector<std::unique_ptr<TG_TypeNode>> store;
		store.push_back(std::make_unique<TypeShapeProbe>());
		store.push_back(std::make_unique<TG_TypeNode>());
		store.push_back(nullptr);  // an aborted load leaves empty slots
		std::vector<std::unique_ptr<TG_TypeNode>>().swap(store);  // TG_TypeMultiShape::destroy releases the owners this way
	}
	CHECK(g_bufferReleases == 2);
	CHECK(g_declReleases == 1);
}
