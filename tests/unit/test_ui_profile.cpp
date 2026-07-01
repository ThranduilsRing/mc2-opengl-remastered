// tests/unit/test_ui_profile.cpp
// UI-PROFILE-1 (Phase 2, slice 6 of the UNIT-PROFILE arc).
//
// Pure-logic doctest coverage for ui_profile.h (the UI facet + accessor-override
// helpers) and the UPV_UI_ASSET_MISSING validator check in
// unit_profile_validator.h. PURE -- no engine behavior, no atlas blit math.
// Covers: declared value overrides legacy, empty/unset falls back to legacy
// (the stock-parity guarantee), and the report-only missing-asset finding.
// Header-only, drags no engine/GL/IO. Mirrors test_hardpoint.cpp shape.
#include "doctest.h"
#include "ui_profile.h"
#include "unit_profile_validator.h"
#include <string>

// -------------------------------------------------------------------------
// chooseUIAsset: declared filename overrides legacy; empty falls back.
// -------------------------------------------------------------------------

TEST_CASE("ui asset: declared non-empty overrides the legacy filename") {
    const UIAssetName declared("hunchback_icon.tga");
    CHECK(std::string(chooseUIAsset(declared, "legacy_slot.tga"))
          == "hunchback_icon.tga");
}

TEST_CASE("ui asset: empty declaration falls back to legacy (stock parity)") {
    const UIAssetName unmapped;   // stock unit: declares nothing
    CHECK(unmapped.empty());
    CHECK(std::string(chooseUIAsset(unmapped, "legacy_slot.tga"))
          == "legacy_slot.tga");
    // A default-constructed UIProfile leaves EVERY slot empty -> every override is
    // the legacy value -> byte-identical legacy behavior.
    const UIProfile p;
    CHECK(p.icon.empty());
    CHECK(p.portrait.empty());
    CHECK(p.minimap.empty());
    CHECK(p.silhouette.empty());
    CHECK(std::string(chooseUIAsset(p.icon, "leg0.tga"))       == "leg0.tga");
    CHECK(std::string(chooseUIAsset(p.silhouette, "leg1.tga")) == "leg1.tga");
    CHECK(std::string(chooseUIAsset(p.portrait, "leg2.tga"))   == "leg2.tga");
}

// -------------------------------------------------------------------------
// chooseUIStringId: declared id overrides; UNSET (-1) falls back.
// -------------------------------------------------------------------------

TEST_CASE("ui string id: declared id overrides the legacy resource id") {
    CHECK(chooseUIStringId(4200, /*legacy*/ 137) == 4200);
    // id 0 is a valid IDS_ value and must NOT be treated as unset.
    CHECK(chooseUIStringId(0, /*legacy*/ 137) == 0);
}

TEST_CASE("ui string id: UNSET falls back to the legacy id (stock parity)") {
    const UIProfile p;   // stock: both ids UNSET
    CHECK(p.nameStringId == UI_STRING_ID_UNSET);
    CHECK(p.encyclopediaStringId == UI_STRING_ID_UNSET);
    CHECK(chooseUIStringId(p.nameStringId, /*legacy*/ 137) == 137);
    CHECK(chooseUIStringId(p.encyclopediaStringId, /*legacy*/ 909) == 909);
}

// -------------------------------------------------------------------------
// validateDeclaredUIAssets: report-only missing-asset finding.
// -------------------------------------------------------------------------

TEST_CASE("ui validate: empty declaration list => ZERO findings (stock parity)") {
    std::vector<DeclaredUIAssetCheck> none;
    CHECK(validateDeclaredUIAssets(none).empty());
}

TEST_CASE("ui validate: every declared asset resolves => ZERO findings") {
    std::vector<DeclaredUIAssetCheck> all;
    DeclaredUIAssetCheck a; a.resolves = true;
    DeclaredUIAssetCheck b; b.resolves = true;
    all.push_back(a); all.push_back(b);
    CHECK(validateDeclaredUIAssets(all).empty());
}

TEST_CASE("ui validate: a missing declared asset => one UPV_UI_ASSET_MISSING") {
    std::vector<DeclaredUIAssetCheck> mixed;
    DeclaredUIAssetCheck present; present.resolves = true;
    DeclaredUIAssetCheck missing; missing.resolves = false;
    mixed.push_back(present);
    mixed.push_back(missing);
    const std::vector<UnitProfileFinding> f = validateDeclaredUIAssets(mixed);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_UI_ASSET_MISSING);
}

TEST_CASE("ui validate: reason code is appended, not renumbered") {
    // UI-PROFILE-1 added its code LAST; the earlier LIVE codes keep their values
    // so logs / mod tooling keying on the number never break.
    CHECK(UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR == 0);
    CHECK(UPV_UI_ASSET_MISSING < UPV__COUNT);
    CHECK(UPV_UI_ASSET_MISSING > UPV_HARDPOINT_WEAPON_CLASS_REJECTED);
}
