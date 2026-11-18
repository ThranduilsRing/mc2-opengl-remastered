// In-mission HUD ImGui tabs (MC2_HUD_IMGUI_TABS):
// HudTabs::Registry order/cycle/lookup invariants. Pure logic, offline.
//
// The load-bearing invariant: registration order == hotkey cycle order, and
// the stock registration (tacmap, vehicle, info) reproduces the legacy
// hand-written switchTabs table exactly:
//   dir > 0 : TACMAP -> VEHICLE -> INFO -> TACMAP
//   dir < 0 : TACMAP -> INFO -> VEHICLE -> TACMAP

#include "doctest.h"

#include "hudtabregistry.h"
#include "../../code/hud_join.h"
#include <array>
#include <random>
#include <vector>

TEST_CASE("HUDJoinDifferential")
{
    struct Other { virtual ~Other() = default; };
    struct Object { virtual ~Object() = default; };
    struct Mover : Other, Object {};
    struct View { const Object* obj; int mode; float barStatus; };
    struct Row { const Object* object; };
    std::array<Mover, 8> objects{};
    const auto compare = [&](const std::vector<View>& views, const std::vector<Row>& rows) {
        const auto bars = mc2::hud_join::barStatusIndex(views, 1, 2);
        const auto movers = mc2::hud_join::moverIndex(rows);
        std::vector<std::size_t> expectedRoster, actualRoster;
        // Null and absent objects are roster entries too; preserve roster order.
        for (int i = -1; i < 8; ++i) {
            const auto* object = i < 0 ? nullptr : &objects[i];
            float expected = -1, actual = -1;
            for (const auto& view : views)
                if (view.obj == object && (view.mode == 1 || view.mode == 2)) {
                    expected = view.barStatus; break;
                }
            if (const auto found = bars.find(object); found != bars.end()) {
                const auto& [key, value] = *found;
                actual = value;
            }
            CHECK(actual == expected);
            for (std::size_t j = 0; j < rows.size(); ++j)
                if (rows[j].object == object) { expectedRoster.push_back(j); break; }
            if (const auto found = movers.find(object); found != movers.end()) {
                const auto& [key, value] = *found;
                actualRoster.push_back(value);
            }
        }
        CHECK(actualRoster == expectedRoster);
    };
    compare({}, {});
    compare({{&objects[0], 0, 9}, {&objects[0], 1, 3}, {&objects[0], 2, 7},
             {nullptr, 2, 4}}, {{&objects[0]}, {&objects[0]}, {nullptr}, {nullptr}});
    std::mt19937 random(0x485544);
    const auto pointer = [&]() -> const Object* {
        const auto i = random() % 9;
        return i == 8 ? nullptr : &objects[i];
    };
    for (int trial = 0; trial < 400; ++trial) {
        std::vector<View> views;
        std::vector<Row> rows;
        const auto count = random() % 64;
        for (unsigned i = 0; i < count; ++i)
            views.push_back({pointer(), static_cast<int>(random() % 4), static_cast<float>(random() % 100)});
        for (unsigned i = 0, n = random() % 64; i < n; ++i) rows.push_back({pointer()});
        compare(views, rows);
    }
}

namespace
{
	// stand-ins for the ControlGui::Commands ids (values irrelevant, distinct)
	enum { TACMAP_TAB = 12, INFO_TAB = 13, VEHICLE_TAB = 14 };

	HudTabs::Tab makeTab( const char* id, int buttonCommand )
	{
		HudTabs::Tab t;
		t.id = id;
		t.buttonCommand = buttonCommand;
		t.render = 0;
		t.update = 0;
		t.helpTextHeaderID = 0;
		t.helpTextID = 0;
		t.countsAsTacMap = false;
		return t;
	}

	// register the stock 3 in the engine's cycle order (controlgui.cpp
	// initHudTabRegistry)
	void registerStock( HudTabs::Registry& r )
	{
		r.add( makeTab( "tacmap", TACMAP_TAB ) );
		r.add( makeTab( "vehicle", VEHICLE_TAB ) );
		r.add( makeTab( "info", INFO_TAB ) );
	}
}

TEST_SUITE( "HudTabRegistry" )
{

TEST_CASE( "registration order and lookup" )
{
	HudTabs::Registry r;
	CHECK( r.count() == 0 );
	CHECK( r.activeTab() == nullptr );

	registerStock( r );
	REQUIRE( r.count() == 3 );

	CHECK( r.indexOfId( "tacmap" ) == 0 );
	CHECK( r.indexOfId( "vehicle" ) == 1 );
	CHECK( r.indexOfId( "info" ) == 2 );
	CHECK( r.indexOfId( "nope" ) == -1 );
	CHECK( r.indexOfId( nullptr ) == -1 );

	CHECK( r.indexOfButton( TACMAP_TAB ) == 0 );
	CHECK( r.indexOfButton( VEHICLE_TAB ) == 1 );
	CHECK( r.indexOfButton( INFO_TAB ) == 2 );
	CHECK( r.indexOfButton( -1 ) == -1 );   // ImGui-drawn tabs never match
	CHECK( r.indexOfButton( 999 ) == -1 );

	// first registered tab is active by default (legacy: TACMAP_TAB pressed)
	CHECK( r.activeIndex() == 0 );
	REQUIRE( r.activeTab() != nullptr );
	CHECK( r.activeTab()->buttonCommand == TACMAP_TAB );
}

TEST_CASE( "cycle reproduces the legacy switchTabs table" )
{
	HudTabs::Registry r;
	registerStock( r );

	// dir > 0: TACMAP -> VEHICLE -> INFO -> TACMAP
	CHECK( r.cycle( 1 ) == 1 );          // tacmap -> vehicle
	r.setActive( 1 );
	CHECK( r.cycle( 1 ) == 2 );          // vehicle -> info
	r.setActive( 2 );
	CHECK( r.cycle( 1 ) == 0 );          // info -> tacmap (wrap)

	// dir < 0: TACMAP -> INFO -> VEHICLE -> TACMAP
	r.setActive( 0 );
	CHECK( r.cycle( -1 ) == 2 );         // tacmap -> info (wrap)
	r.setActive( 2 );
	CHECK( r.cycle( -1 ) == 1 );         // info -> vehicle
	r.setActive( 1 );
	CHECK( r.cycle( -1 ) == 0 );         // vehicle -> tacmap

	// cycle() must not mutate the active index
	r.setActive( 1 );
	(void)r.cycle( 1 );
	CHECK( r.activeIndex() == 1 );
}

TEST_CASE( "4th tab extends the cycle without touching the stock 3" )
{
	HudTabs::Registry r;
	registerStock( r );
	const int testIdx = r.add( makeTab( "test", -1 ) );   // no legacy button
	REQUIRE( testIdx == 3 );
	REQUIRE( r.count() == 4 );

	// stock lookups unchanged
	CHECK( r.indexOfButton( TACMAP_TAB ) == 0 );
	CHECK( r.indexOfId( "info" ) == 2 );

	// cycle now visits the 4th tab between info and tacmap
	r.setActive( 2 );                    // info
	CHECK( r.cycle( 1 ) == 3 );          // info -> test
	r.setActive( 3 );
	CHECK( r.cycle( 1 ) == 0 );          // test -> tacmap
	CHECK( r.cycle( -1 ) == 2 );         // test -> info

	CHECK( r.at( 3 )->buttonCommand == -1 );
}

TEST_CASE( "bounds, capacity, clear" )
{
	HudTabs::Registry r;
	CHECK( r.at( 0 ) == nullptr );
	CHECK( r.cycle( 1 ) == -1 );         // empty registry

	// null id rejected
	CHECK( r.add( makeTab( nullptr, 0 ) ) == -1 );

	static const char* names[] = { "a","b","c","d","e","f","g","h","i","j" };
	for ( int i = 0; i < 10; ++i )
	{
		const int idx = r.add( makeTab( names[i], i ) );
		if ( i < HudTabs::Registry::MAX_TABS )
			CHECK( idx == i );
		else
			CHECK( idx == -1 );          // full: rejected, not clobbered
	}
	CHECK( r.count() == HudTabs::Registry::MAX_TABS );

	// setActive ignores out-of-range
	r.setActive( 3 );
	r.setActive( -1 );
	r.setActive( 99 );
	CHECK( r.activeIndex() == 3 );

	r.clear();
	CHECK( r.count() == 0 );
	CHECK( r.activeIndex() == 0 );
	CHECK( r.activeTab() == nullptr );
}

} // TEST_SUITE
