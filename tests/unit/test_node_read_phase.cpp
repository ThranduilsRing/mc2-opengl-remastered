// G1 (sim/render boundary): Appearance::simNode*/drawNode* force the phase half for the call and restore the caller's phase.
// The wrappers are one NodeReadPhase guard around the legacy virtual, so the guard is what is pinned here.
#include "doctest.h"
#include "presentation_phase.h"

thread_local bool simPresentationPass = false;

namespace {
// Model of a legacy node getter: the sim pose half when the flag is clear, the draw half when it is set.
int legacyGetter() { return simPresentationPass ? 2 : 1; }
int simNode() { mc2::sim::NodeReadPhase p(false); return legacyGetter(); }
int drawNode() { mc2::sim::NodeReadPhase p(true); return legacyGetter(); }
}

TEST_CASE("simNode reads the sim half even inside a presentation pass") {
	simPresentationPass = false;
	mc2::sim::PresentationScope presenting;
	CHECK(legacyGetter() == 2);
	CHECK(simNode() == 1);
	CHECK(simPresentationPass);
}

TEST_CASE("drawNode reads the draw half even inside the tick") {
	simPresentationPass = false;
	CHECK(legacyGetter() == 1);
	CHECK(drawNode() == 2);
	CHECK_FALSE(simPresentationPass);
}
