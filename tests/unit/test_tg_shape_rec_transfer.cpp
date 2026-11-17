// B7o.6a/6b: TG_ShapeRec::transferFrom moves a record's node and applies the resets the copy assignment applied (Detach and the
// shape-compaction loop use it), and rebuildShapeRecParents re-points every parent into the array it is given, so after a
// detach or a compaction no parent points into a released array.
#include "doctest.h"
#include "tgl.h"
#include "alloc_probe.h"
#include "../../GameOS/gameos/vulkan_legacy_shape_island.h"

// Trigger: legacy island provenance and draw accounting headers.
TEST_CASE("legacy island mixed-node ownership preserves source vertices") {
    Mc2LegacyMlrDraw draw{};
    draw.vertexCount = 15;
    draw.sourceVertices[MC2_LEGACY_VEHICLE_FADING] = 6;
    draw.sourceVertices[MC2_LEGACY_SHAPE] = 9;
    CHECK(mc2LegacyNodeOwner(draw) == MC2_LEGACY_SHAPE);
    uint32_t sum = 0;
    for (auto count : draw.sourceVertices) sum += count;
    CHECK(sum == draw.vertexCount);
    draw.sourceVertices[MC2_LEGACY_SHAPE] = 6;
    CHECK(mc2LegacyNodeOwner(draw) == MC2_LEGACY_VEHICLE_FADING);
    CHECK(mc2LegacyVehicleSource == MC2_LEGACY_SHAPE);
    { const Mc2LegacyVehicleScope outer(MC2_LEGACY_VEHICLE_FADING);
      { const Mc2LegacyVehicleScope inner(MC2_LEGACY_VEHICLE_UNREGISTERED);
        CHECK(mc2LegacyVehicleSource == MC2_LEGACY_VEHICLE_UNREGISTERED); }
      CHECK(mc2LegacyVehicleSource == MC2_LEGACY_VEHICLE_FADING); }
    CHECK(mc2LegacyVehicleSource == MC2_LEGACY_SHAPE);
}

#include <type_traits>

#include <cstring>
#include <memory>
#include <vector>

TEST_CASE("TG_Light/source color and intensity stay separate") {
    TG_Light light;
    light.init(TG_LIGHT_TERRAIN);
    light.SetaRGB(0xffe8c870);
    light.SetIntensity(0.25f);
    CHECK(light.GetSourceRGB()==0xffe8c870);
    CHECK(light.intensity==0.25f);
    CHECK(light.GetaRGB()==0xff3a321c);
    light.SetIntensity(0.0f);
    CHECK(light.intensity==0);
    CHECK(light.GetaRGB()==0xff000000);
    CHECK(light.GetSourceRGB()==0xffe8c870);
    light.SetaRGB(0xff804020);
    CHECK(light.intensity==1);
    CHECK(light.GetaRGB()==light.GetSourceRGB());
}

// ~TG_Shape calls the out-of-line destroy() (tgl.cpp, GL dependent); here it counts, so the real-node tests below see every
// TG_Shape the records destroy (the fake-node tests release their dummies before any record goes, so it never runs for them).
extern int g_shapeDestroys;

namespace {
// The node is only moved, never dereferenced, by transferFrom: an aligned dummy stands in for a TG_Shape. Since B7o.6b the record
// OWNS its node (unique_ptr), so every test releases the dummies before the records go.
TG_Shape* fakeNode(int i)
{
	static std::vector<std::unique_ptr<unsigned char[]>> blocks;
	blocks.emplace_back(new unsigned char[16]());
	return reinterpret_cast<TG_Shape*>(blocks.back().get() + (i & 0));
}

struct Ids
{
	const char* id;
	const char* parent;
};
}

TEST_CASE("TG_ShapeRec::transferFrom moves the node and resets the per-frame fields like the copy assignment")
{
	TG_ShapeRec parent{};
	TG_ShapeRec src{};
	TG_Animation anim;
	src.node.reset(fakeNode(1));
	src.calcedThisFrame = 77;
	src.processMe = false;
	src.currentAnimation = &anim;
	src.parentNode = &parent;
	src.baseRotation.w = 0.5f;
	src.baseRotation.x = 0.25f;
	src.baseRotation.y = -0.25f;
	src.baseRotation.z = 0.75f;
	TG_Shape* const node = src.node.get();

	TG_ShapeRec dst{};
	dst.calcedThisFrame = 5;
	dst.processMe = false;
	dst.transferFrom(src);

	CHECK(dst.node.get() == node);
	CHECK(src.node == nullptr);
	CHECK(dst.calcedThisFrame == -1);
	CHECK(dst.processMe == true);
	CHECK(dst.currentAnimation == nullptr);
	CHECK(dst.parentNode == &parent);
	CHECK(dst.baseRotation.w == 0.5f);
	CHECK(dst.baseRotation.x == 0.25f);
	CHECK(dst.baseRotation.y == -0.25f);
	CHECK(dst.baseRotation.z == 0.75f);
	(void)dst.node.release();
}

TEST_CASE("TG_ShapeRec: compaction by transfer, then the parent rebuild points every parent into the new array")
{
	// root <- a <- b, root <- c; detach "a" (and its child b): root and c stay, compacted into a new array.
	const std::vector<Ids> ids = {{"root", ""}, {"a", "root"}, {"b", "a"}, {"c", "root"}};
	std::vector<TG_ShapeRec> old(ids.size());
	std::vector<TG_Shape*> nodes;
	for (size_t i = 0; i < ids.size(); ++i)
	{
		old[i].node.reset(fakeNode(static_cast<int>(i)));
		nodes.push_back(old[i].node.get());
	}
	auto idOf = [&](const TG_ShapeRec& r) {
		for (size_t i = 0; i < nodes.size(); ++i)
			if (nodes[i] == r.node.get())
				return ids[i].id;
		return "?";
	};
	auto parentOf = [&](const TG_ShapeRec& r) {
		for (size_t i = 0; i < nodes.size(); ++i)
			if (nodes[i] == r.node.get())
				return ids[i].parent;
		return "?";
	};
	rebuildShapeRecParents(old.data(), static_cast<long>(old.size()), idOf, parentOf);
	CHECK(old[0].parentNode == nullptr);
	CHECK(old[1].parentNode == &old[0]);
	CHECK(old[2].parentNode == &old[1]);
	CHECK(old[3].parentNode == &old[0]);

	// Detach a and b into their own array (as TG_MultiShape::Detach does), a becomes the new root.
	std::vector<TG_ShapeRec> detached(2);
	detached[0].transferFrom(old[1]);
	detached[1].transferFrom(old[2]);
	detached[0].parentNode = nullptr;
	rebuildShapeRecParents(detached.data(), 2, idOf, parentOf);
	CHECK(detached[0].parentNode == nullptr);
	CHECK(detached[1].parentNode == &detached[0]);

	// Compact the remaining records into a fresh array, then release the old one.
	std::vector<TG_ShapeRec> kept(2);
	long k = 0;
	for (auto& r : old)
		if (r.node != nullptr)
			kept[k++].transferFrom(r);
	REQUIRE(k == 2);
	rebuildShapeRecParents(kept.data(), k, idOf, parentOf);
	const TG_ShapeRec* lo = kept.data();
	const TG_ShapeRec* hi = kept.data() + kept.size();
	for (auto& r : kept)
	{
		CHECK(r.calcedThisFrame == -1);
		CHECK(r.processMe == true);
		CHECK((r.parentNode == nullptr || (r.parentNode >= lo && r.parentNode < hi)));
	}
	CHECK(kept[0].node.get() == nodes[0]);
	CHECK(kept[1].node.get() == nodes[3]);
	CHECK(kept[1].parentNode == &kept[0]);
	for (auto& r : old)
		CHECK(r.node == nullptr);
	for (auto& r : kept)
		(void)r.node.release();
	for (auto& r : detached)
		(void)r.node.release();
}

static_assert(!std::is_copy_assignable_v<TG_ShapeRec> && !std::is_copy_constructible_v<TG_ShapeRec>, "a record owns its node: never copied (B7o.6b)");
static_assert(sizeof(std::unique_ptr<TG_Shape>) == sizeof(TG_Shape*), "the node owner is one pointer (default deleter)");

// Review follow-up (B7 proof gaps): REAL TG_Shape nodes, owned by the records, through the same detach + compaction sequence as
// TG_MultiShape::Detach (msl_shape.cpp:445) and its compaction loop (:476). Every node is destroyed exactly once, only when its
// final owner goes; a second teardown of the same arrays destroys nothing; the cycle repeated leaves the heap where it found it.
namespace {
int detachAndCompactCycle()
{
	const std::vector<Ids> ids = {{"root", ""}, {"a", "root"}, {"b", "a"}, {"c", "root"}};
	auto oldArray = std::make_unique<std::vector<TG_ShapeRec>>(ids.size());
	auto& old = *oldArray;
	std::vector<TG_Shape*> nodes;
	for (auto& r : old)
	{
		r.node = std::make_unique<TG_Shape>();
		nodes.push_back(r.node.get());
	}
	auto idOf = [&](const TG_ShapeRec& r) {
		for (size_t i = 0; i < nodes.size(); ++i)
			if (nodes[i] == r.node.get())
				return ids[i].id;
		return "?";
	};
	auto parentOf = [&](const TG_ShapeRec& r) {
		for (size_t i = 0; i < nodes.size(); ++i)
			if (nodes[i] == r.node.get())
				return ids[i].parent;
		return "?";
	};
	rebuildShapeRecParents(old.data(), static_cast<long>(old.size()), idOf, parentOf);

	auto detached = std::make_unique<std::vector<TG_ShapeRec>>(2);
	(*detached)[0].transferFrom(old[1]);
	(*detached)[1].transferFrom(old[2]);
	(*detached)[0].parentNode = nullptr;
	rebuildShapeRecParents(detached->data(), 2, idOf, parentOf);

	auto kept = std::make_unique<std::vector<TG_ShapeRec>>(2);
	long k = 0;
	for (auto& r : old)
		if (r.node != nullptr)
			(*kept)[k++].transferFrom(r);
	rebuildShapeRecParents(kept->data(), k, idOf, parentOf);

	// The old array goes first (as the compaction frees listOfShapes): it owns nothing any more, so no node dies.
	const int before = g_shapeDestroys;
	oldArray.reset();
	CHECK(g_shapeDestroys == before);
	CHECK(k == 2);
	CHECK((*kept)[1].parentNode == &(*kept)[0]);
	CHECK((*detached)[1].parentNode == &(*detached)[0]);

	// The detached shape is released, then the compacted one: two nodes each.
	detached.reset();
	CHECK(g_shapeDestroys == before + 2);
	kept.reset();
	CHECK(g_shapeDestroys == before + 4);
	return g_shapeDestroys - before;
}
}

TEST_CASE("TG_ShapeRec: real nodes through detach and compaction are destroyed exactly once by their final owner")
{
	ScopedLeakCheck leak;
	g_shapeDestroys = 0;
	CHECK(detachAndCompactCycle() == 4);
	CHECK(g_shapeDestroys == 4);
	CHECK(leak.delta() == 0);
}

TEST_CASE("TG_ShapeRec: repeated teardown destroys nothing twice and the repeated cycle does not leak")
{
	ScopedLeakCheck leak;
	g_shapeDestroys = 0;
	for (int cycle = 0; cycle < 8; ++cycle)
		CHECK(detachAndCompactCycle() == 4);
	CHECK(g_shapeDestroys == 32);

	// Tearing the same record down twice: the second reset finds an empty owner.
	TG_ShapeRec rec{};
	rec.node = std::make_unique<TG_Shape>();
	rec.node.reset();
	rec.node.reset();
	CHECK(g_shapeDestroys == 33);
	// A record moved from and then destroyed releases nothing; the receiver releases the node once.
	{
		TG_ShapeRec src{};
		src.node = std::make_unique<TG_Shape>();
		TG_ShapeRec dst{};
		dst.transferFrom(src);
		dst.transferFrom(src);	// a second transfer from an empty record clears the receiver's node: it is released here, once
		CHECK(g_shapeDestroys == 34);
	}
	CHECK(g_shapeDestroys == 34);
	CHECK(leak.delta() == 0);
}


namespace {
struct TextureBorrowProbe : TG_Shape {
    static const TG_TinyTexture*& slots(TG_Shape& shape) { return shape.*&TextureBorrowProbe::instanceTextures_; }
};
}

TEST_CASE("TG_ShapeRec: detach immediately borrows the destination texture slots")
{
    std::vector<TG_TinyTexture> source(1), detached;
    source[0].mcTextureNodeIndex=7;
    TG_ShapeRec from{}, to{};
    from.node=std::make_unique<TG_Shape>();
    TextureBorrowProbe::slots(*from.node)=source.data();
    detached=source;
    to.transferFrom(from,detached.data()); // the production Detach transfer
    source.clear(); source.shrink_to_fit();
    REQUIRE(TextureBorrowProbe::slots(*to.node)==detached.data());
    CHECK(TextureBorrowProbe::slots(*to.node)[0].mcTextureNodeIndex==7);
    detached[0].mcTextureNodeIndex=19;
    CHECK(TextureBorrowProbe::slots(*to.node)[0].mcTextureNodeIndex==19);
    CHECK_FALSE(from.node);
}

// RENDER-THREAD-R2 item 1: a gather reads its own TG_LightContext. TG_Shape carries no light-list statics, two
// contexts never alias, and building or editing one writes nothing shared.
template <class T> concept HasLightStatics = requires { T::s_listOfLights; } || requires { T::s_numLights; };
static_assert(!HasLightStatics<TG_Shape>, "the record path must not read mutable TG_Shape light statics");

TEST_CASE("TG_LightContext/contexts do not alias and editing one leaves the other alone")
{
    TG_Light a, b;
    a.init(TG_LIGHT_AMBIENT);
    b.init(TG_LIGHT_AMBIENT);
    TG_LightPtr listA[] = { &a };
    TG_LightPtr listB[] = { &b, &a };

    const TG_LightContext passA = TG_LightContext::make(std::span<TG_LightPtr>(listA, 1));
    TG_LightContext passB = TG_LightContext::make(std::span<TG_LightPtr>(listB, 2));
    CHECK(passA.list == listA);
    CHECK(passA.count == 1);
    CHECK(passB.list == listB);
    CHECK(passB.count == 2);
    CHECK(&passA != &passB);
    CHECK(passA.list != passB.list);

    passB.count = 0;
    passB.list = nullptr;
    CHECK(passA.list == listA);
    CHECK(passA.count == 1);

    // A null list normalises to a zero count.
    const TG_LightContext none = TG_LightContext::make(std::span<TG_LightPtr>());
    CHECK(none.list == nullptr);
    CHECK(none.count == 0);
}
