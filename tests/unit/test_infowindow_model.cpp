// HUD-IMGUI slice D (MC2_HUD_IMGUI_INFOWINDOW): pure component-list
// aggregation kernel shared by InfoWindow::renderImGuiContent. Mirrors the
// legacy drawScrollingStuff dedupe loop contract exactly.
#include "doctest.h"
#include "infowindowmodel.h"

using namespace InfoWindowModel;

static WeaponRec rec( unsigned char id, bool working, long ammo, int range )
{
	WeaponRec r;
	r.masterID = id;
	r.working = working;
	r.ammo = ammo;
	r.range = range;
	return r;
}

TEST_SUITE( "InfoWindowModel" )
{
	TEST_CASE( "empty / null inputs" )
	{
		ComponentRow out[4];
		CHECK( aggregateWeapons( {}, out ) == 0 );
		WeaponRec r = rec( 1, true, 5, 0 );
		CHECK( aggregateWeapons( {&r, 1}, {} ) == 0 );
		CHECK( aggregateWeapons( {&r, 1}, std::span(out).first(0) ) == 0 );
		CHECK( aggregateWeapons( {&r, 0}, out ) == 0 );
	}

	TEST_CASE( "dedupe keeps first-seen order and counts occurrences" )
	{
		WeaponRec recs[] = {
			rec( 10, true, 8, 1 ),
			rec( 20, false, -1, 2 ),
			rec( 10, false, 8, 1 ),
			rec( 10, true, 8, 1 ),
			rec( 20, true, -1, 2 ),
		};
		ComponentRow out[8];
		const int n = aggregateWeapons( recs, out );
		REQUIRE( n == 2 );
		CHECK( out[0].masterID == 10 );
		CHECK( out[0].totalCount == 3 );
		CHECK( out[0].workingCount == 2 );
		CHECK( out[0].range == 1 );
		CHECK( out[1].masterID == 20 );
		CHECK( out[1].totalCount == 2 );
		CHECK( out[1].workingCount == 1 );
		CHECK( out[1].range == 2 );
	}

	TEST_CASE( "ammo comes from FIRST occurrence only (legacy combined-list rule)" )
	{
		WeaponRec recs[] = {
			rec( 7, true, 12, 0 ),
			rec( 7, true, 99, 0 ),   // duplicate ammo must NOT accumulate
		};
		ComponentRow out[2];
		REQUIRE( aggregateWeapons( recs, out ) == 1 );
		CHECK( out[0].ammo == 12 );
	}

	TEST_CASE( "no-ammo sentinel passes through" )
	{
		WeaponRec r = rec( 3, true, -1, 2 );
		ComponentRow out[1];
		REQUIRE( aggregateWeapons( {&r, 1}, out ) == 1 );
		CHECK( out[0].ammo == -1 );
	}

	TEST_CASE( "maxOut clamps new rows but duplicates still count" )
	{
		WeaponRec recs[] = {
			rec( 1, true, -1, 0 ),
			rec( 2, true, -1, 0 ),   // dropped: out is full
			rec( 1, false, -1, 0 ),  // still counted onto row 0
		};
		ComponentRow out[1];
		const int n = aggregateWeapons( recs, out );
		REQUIRE( n == 1 );
		CHECK( out[0].masterID == 1 );
		CHECK( out[0].totalCount == 2 );
		CHECK( out[0].workingCount == 1 );
	}

	TEST_CASE( "Clan-range masterID >= 128 survives (unsigned char contract)" )
	{
		WeaponRec r = rec( (unsigned char)200, true, 4, 1 );
		ComponentRow out[1];
		REQUIRE( aggregateWeapons( {&r, 1}, out ) == 1 );
		CHECK( (int)out[0].masterID == 200 );
	}

	TEST_CASE( "skillBarCount: legacy division + bad-divisor guards" )
	{
		CHECK( skillBarCount( 50, 5 ) == 10 );
		CHECK( skillBarCount( 49, 5 ) == 9 );
		CHECK( skillBarCount( 0, 5 ) == 0 );
		CHECK( skillBarCount( -3, 5 ) == 0 );
		CHECK( skillBarCount( 50, 0 ) == 0 );
		CHECK( skillBarCount( 50, -1 ) == 0 );
	}
}
