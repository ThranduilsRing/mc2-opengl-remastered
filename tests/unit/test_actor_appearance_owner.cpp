// B7o.3: the actor appearance owner protocol (code/appearance_owner.h, the bodies of GameObject::adoptAppearance /
// releaseAppearance). GameObject cannot be linked here, so a small actor model uses the PRODUCTION helpers with the same member
// shape (owner + raw view) and the same destroy()-then-backstop teardown. Counts and event order are what is pinned.
#include "doctest.h"
#include "appearance_owner.h"

#include <memory>
#include <string>
#include <vector>

namespace {
std::vector<std::string> g_events;
int g_live = 0;
int g_dtors = 0;

struct App {
	int id;
	explicit App (int i) : id(i) { ++g_live; }
	virtual ~App (void) { ++g_dtors; --g_live; g_events.push_back("app" + std::to_string(id) + "-dtor"); }
};

struct Actor {
	App* appearance = nullptr;               // view
	std::unique_ptr<App> appearanceOwner;    // owner
	void adopt (App* a) { mc2::adoptOwnedView(appearanceOwner, appearance, a); }
	void release (void) { mc2::releaseOwnedView(appearanceOwner, appearance); }
	virtual void destroy (void) { release(); }
	virtual ~Actor (void) { Actor::destroy(); }
};

// Mech: retires its render handle BEFORE the appearance goes, in destroy and in the replacement path.
struct MechLike : Actor {
	void retireHandle (void) { g_events.push_back("handle-retire"); }
	void destroy (void) override { retireHandle(); Actor::destroy(); }
	void reinit (App* a) { retireHandle(); release(); adopt(a); }
};
// Mover: appearance before the contacts.
struct MoverLike : Actor {
	void destroy (void) override { Actor::destroy(); g_events.push_back("contacts-free"); }
};
// TerrainObject: buffers first, then the appearance.
struct TerrainLike : Actor {
	void destroy (void) override { g_events.push_back("buffers-free"); Actor::destroy(); }
};

void reset () { g_events.clear(); g_live = 0; g_dtors = 0; }
}

static_assert(sizeof(std::unique_ptr<App>) == sizeof(App*), "default deleter keeps the owner pointer-sized");

TEST_CASE("actor appearance owner: adopt, view follows, destructor frees once")
{
	reset();
	{
		Actor a;
		a.adopt(new App(1));
		CHECK(a.appearance == a.appearanceOwner.get());
		CHECK(g_live == 1);
	}
	CHECK(g_dtors == 1);
	CHECK(g_live == 0);
}

TEST_CASE("actor appearance owner: adopting over a live appearance frees the previous one (init replacement paths)")
{
	reset();
	Actor a;
	a.adopt(new App(1));
	a.adopt(new App(2));      // bldng/gate/turret/terrobj/gvehicl init replacement
	CHECK(g_dtors == 1);
	CHECK(g_events.back() == "app1-dtor");
	CHECK(a.appearance->id == 2);
	CHECK_FALSE(mc2::ownedViewDiverged(a.appearanceOwner, a.appearance));
	a.adopt(nullptr);
	CHECK(g_dtors == 2);
	CHECK(a.appearance == nullptr);
}

TEST_CASE("actor appearance owner: release nulls the view and repeated teardown frees nothing twice")
{
	reset();
	Actor a;
	a.adopt(new App(1));
	a.release();
	a.release();
	a.destroy();
	a.destroy();
	CHECK(g_dtors == 1);
	CHECK(a.appearance == nullptr);
	CHECK(a.appearanceOwner == nullptr);
}

TEST_CASE("actor appearance owner: owner destructor is the backstop, a raw view write is detected")
{
	reset();
	{
		std::unique_ptr<App> owner(new App(8));   // never released explicitly
		App* view = owner.get();
		CHECK_FALSE(mc2::ownedViewDiverged(owner, view));
		view = nullptr;
		CHECK(mc2::ownedViewDiverged(owner, view));
	}
	CHECK(g_dtors == 1);
	CHECK(g_live == 0);
}

TEST_CASE("actor appearance owner: per-class release order is unchanged")
{
	reset();
	{ MechLike m; m.adopt(new App(1)); m.destroy(); }
	REQUIRE(g_events.size() >= 2);
	CHECK(g_events[0] == "handle-retire");
	CHECK(g_events[1] == "app1-dtor");
	CHECK(g_dtors == 1);

	reset();
	{ MechLike m; m.adopt(new App(1)); m.reinit(new App(2)); }   // replacement path
	CHECK(g_events[0] == "handle-retire");
	CHECK(g_events[1] == "app1-dtor");
	CHECK(g_dtors == 2);

	reset();
	{ MoverLike m; m.adopt(new App(3)); m.destroy(); }
	CHECK(g_events[0] == "app3-dtor");
	CHECK(g_events[1] == "contacts-free");

	reset();
	{ TerrainLike t; t.adopt(new App(4)); t.destroy(); }
	CHECK(g_events[0] == "buffers-free");
	CHECK(g_events[1] == "app4-dtor");
}
