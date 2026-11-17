// gosRenderer::drawText bounded-region fit math (GameOS/gameos/text_fit.h).
#include "doctest.h"
#include "text_fit.h"

using mc2::textfit::fitLines;

TEST_CASE("text fit: text that fits is unchanged")
{
    auto f = fitLines(3, 14, 60);
    CHECK(f.pitch == 14); CHECK(f.visibleLines == 3); CHECK_FALSE(f.ellipsize);
    f = fitLines(5, 14, 0);   // no usable region
    CHECK(f.pitch == 14); CHECK(f.visibleLines == 5); CHECK_FALSE(f.ellipsize);
    f = fitLines(1, 14, 5);
    CHECK(f.visibleLines == 1); CHECK_FALSE(f.ellipsize);
}

TEST_CASE("text fit: modest overflow tightens the pitch, never below 85%")
{
    auto f = fitLines(5, 14, 65);   // (65-14)/4 = pitch 12 >= floor 11
    CHECK(f.pitch == 12); CHECK(f.visibleLines == 5); CHECK_FALSE(f.ellipsize);
    CHECK((f.visibleLines - 1) * f.pitch + 14 <= 65);
    f = fitLines(5, 14, 58);        // five lines at floor pitch 11 fit exactly
    CHECK(f.pitch == 11); CHECK(f.visibleLines == 5); CHECK_FALSE(f.ellipsize);
    CHECK((f.visibleLines - 1) * f.pitch + 14 == 58);
    f = fitLines(5, 14, 55);        // five lines would occupy 58 pixels: drop one
    CHECK(f.pitch == 11); CHECK(f.visibleLines == 4); CHECK(f.ellipsize);
    CHECK((f.visibleLines - 1) * f.pitch + 14 <= 55);
}

TEST_CASE("text fit: hard overflow clips to the lines that fit and ellipsizes")
{
    const auto f = fitLines(5, 14, 40);   // floor pitch 11: (40-14)/11+1 = 3 lines
    CHECK(f.pitch == 11); CHECK(f.visibleLines == 3); CHECK(f.ellipsize);
    CHECK((f.visibleLines - 1) * f.pitch + 14 <= 40);
    const auto g = fitLines(4, 14, 14);   // exactly one line of room
    CHECK(g.visibleLines == 1); CHECK(g.ellipsize);
}
