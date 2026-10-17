// B7o.6a: TG_ShapeRec::transferFrom moves a record's node and applies the resets the copy assignment applied (Detach and the
// shape-compaction loop use it), and rebuildShapeRecParents re-points every parent into the array it is given, so after a
// detach or a compaction no parent points into a released array.
#include "doctest.h"
#include "tgl.h"

#include <cstring>
#include <memory>
#include <vector>

namespace {
// The node is only moved, never dereferenced, by transferFrom: an aligned dummy stands in for a TG_Shape.
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
	src.node = fakeNode(1);
	src.calcedThisFrame = 77;
	src.processMe = false;
	src.currentAnimation = &anim;
	src.parentNode = &parent;
	src.baseRotation.w = 0.5f;
	src.baseRotation.x = 0.25f;
	src.baseRotation.y = -0.25f;
	src.baseRotation.z = 0.75f;
	TG_Shape* const node = src.node;

	TG_ShapeRec dst{};
	dst.calcedThisFrame = 5;
	dst.processMe = false;
	dst.transferFrom(src);

	CHECK(dst.node == node);
	CHECK(src.node == nullptr);
	CHECK(dst.calcedThisFrame == -1);
	CHECK(dst.processMe == true);
	CHECK(dst.currentAnimation == nullptr);
	CHECK(dst.parentNode == &parent);
	CHECK(dst.baseRotation.w == 0.5f);
	CHECK(dst.baseRotation.x == 0.25f);
	CHECK(dst.baseRotation.y == -0.25f);
	CHECK(dst.baseRotation.z == 0.75f);
}

TEST_CASE("TG_ShapeRec: compaction by transfer, then the parent rebuild points every parent into the new array")
{
	// root <- a <- b, root <- c; detach "a" (and its child b): root and c stay, compacted into a new array.
	const std::vector<Ids> ids = {{"root", ""}, {"a", "root"}, {"b", "a"}, {"c", "root"}};
	std::vector<TG_ShapeRec> old(ids.size());
	std::vector<TG_Shape*> nodes;
	for (size_t i = 0; i < ids.size(); ++i)
	{
		old[i].node = fakeNode(static_cast<int>(i));
		nodes.push_back(old[i].node);
	}
	auto idOf = [&](const TG_ShapeRec& r) {
		for (size_t i = 0; i < nodes.size(); ++i)
			if (nodes[i] == r.node)
				return ids[i].id;
		return "?";
	};
	auto parentOf = [&](const TG_ShapeRec& r) {
		for (size_t i = 0; i < nodes.size(); ++i)
			if (nodes[i] == r.node)
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
	CHECK(kept[0].node == nodes[0]);
	CHECK(kept[1].node == nodes[3]);
	CHECK(kept[1].parentNode == &kept[0]);
	for (auto& r : old)
		CHECK(r.node == nullptr);
}
