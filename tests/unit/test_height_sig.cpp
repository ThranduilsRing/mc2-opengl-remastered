// tests/unit/test_height_sig.cpp
// The gameplay_height.sig hash must equal tools/terrain_beautify/height_sig.py (the sidecar writer).
#include "doctest.h"
#include "gameos/height_sig.h"

TEST_CASE("height sig: matches the Python writer's test vector")
{
    // Python: height_sig([[0,1.5,-2.25],[100,3.75,0.125],[7,8,9.5]] as <f4) == "3 08878b3b0a6edae3"
    const float elev[9] = {0.0f, 1.5f, -2.25f, 100.0f, 3.75f, 0.125f, 7.0f, 8.0f, 9.5f};
    CHECK(mc2terrain::gameplayHeightSig(3, elev) == "3 08878b3b0a6edae3");
}

TEST_CASE("height sig: one edited cell changes the hash")
{
    float elev[9] = {0.0f, 1.5f, -2.25f, 100.0f, 3.75f, 0.125f, 7.0f, 8.0f, 9.5f};
    const std::string a = mc2terrain::gameplayHeightSig(3, elev);
    elev[4] += 1.0f;
    CHECK(a != mc2terrain::gameplayHeightSig(3, elev));
}
