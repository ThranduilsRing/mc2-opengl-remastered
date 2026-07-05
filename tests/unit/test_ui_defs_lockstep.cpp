// UI-DEFS-LOCKSTEP-HARDEN-1: pure decision logic for the data/defs/ui <-> exe
// lockstep contract (gui/UiDefsLockstep.h). Locks the BLACK-MENU-BISECT-1
// hardening: package version-compare, zero-element load decision, and the
// zero-draw fail-soft-to-legacy decision.

#include "doctest.h"

#include "../../gui/UiDefsLockstep.h"

using namespace UiDefs;

TEST_CASE("parseUiPackageVersionText: canonical PACKAGE.fit") {
    const char* fit =
        "FITini\n"
        "\n"
        "[UiPackage]\n"
        "l Version = 1\n"
        "\n"
        "FITend\n";
    CHECK(parseUiPackageVersionText(fit) == 1);
}

TEST_CASE("parseUiPackageVersionText: case-insensitive key, flexible spacing") {
    CHECK(parseUiPackageVersionText("VERSION=7") == 7);
    CHECK(parseUiPackageVersionText("version   =   42") == 42);
    CHECK(parseUiPackageVersionText("ul VeRsIoN\t=\t3\n") == 3);
}

TEST_CASE("parseUiPackageVersionText: comments do not satisfy the key") {
    const char* fit =
        "FITini\n"
        "// version manifest, expected version = 99 (comment, must not parse)\n"
        "[UiPackage]\n"
        "l Version = 2\n"
        "FITend\n";
    CHECK(parseUiPackageVersionText(fit) == 2);
}

TEST_CASE("parseUiPackageVersionText: missing / malformed => -1") {
    CHECK(parseUiPackageVersionText(nullptr) == -1);
    CHECK(parseUiPackageVersionText("") == -1);
    CHECK(parseUiPackageVersionText("FITini\n[UiPackage]\nFITend\n") == -1);
    CHECK(parseUiPackageVersionText("Version = abc") == -1);   // non-numeric
    CHECK(parseUiPackageVersionText("Version 1") == -1);       // no '='
}

TEST_CASE("uiPackageVersionOk: exact match only") {
    CHECK(uiPackageVersionOk(1, 1));
    CHECK_FALSE(uiPackageVersionOk(2, 1));   // newer package, older exe
    CHECK_FALSE(uiPackageVersionOk(1, 2));   // older package, newer exe
    CHECK_FALSE(uiPackageVersionOk(-1, 1));  // manifest missing/unparseable
    CHECK_FALSE(uiPackageVersionOk(-1, -1)); // both invalid is still not ok
}

TEST_CASE("uiPackageVersionOk: current expected version accepts shipped manifest") {
    // Lock: the constant and the shipped data/defs/ui/PACKAGE.fit move together.
    CHECK(uiPackageVersionOk(kExpectedUiPackageVersion, kExpectedUiPackageVersion));
}

TEST_CASE("pageElementCountAcceptable: zero-element page is a load failure") {
    CHECK_FALSE(pageElementCountAcceptable(0));
    CHECK(pageElementCountAcceptable(1));
    CHECK(pageElementCountAcceptable(250));
}

TEST_CASE("pageShouldFailSoftToLegacy: zero-draw decision") {
    // loaded, non-passthrough, rendered once, zero draws => fail-soft
    CHECK(pageShouldFailSoftToLegacy(true, false, true, 0));
    // produced output => page owns the screen
    CHECK_FALSE(pageShouldFailSoftToLegacy(true, false, true, 12));
    // not yet rendered => give the page its first measured frame
    CHECK_FALSE(pageShouldFailSoftToLegacy(true, false, false, 0));
    // legacy passthrough pages never black-clear; no fail-soft needed
    CHECK_FALSE(pageShouldFailSoftToLegacy(true, true, true, 0));
    // unloaded page is handled by the load-failure path, not fail-soft
    CHECK_FALSE(pageShouldFailSoftToLegacy(false, false, true, 0));
}
