// tests/unit/test_unit_source_flip.cpp
// UNIT-SOURCE-FLIP-SWITCH-1: pure logic for the MC2_UNIT_DEFINITION_SOURCE gate.
// Covers (a) the env parser (generated ONLY on the exact token; everything else
// legacy), (b) the variant-slug mirror of the converter's extract.py::_variant_slug,
// and (c) the token-apply helpers (generated-present -> facet sourced from the
// generated token; empty/absent -> facet unchanged == legacy sentinel). Pure,
// offline: no file I/O, no getenv (the cached accessor is exercised in-engine).
#include "doctest.h"
#include "unitprofile.h"
#include "locomotion_facet.h"
#include "damage_schema.h"
#include <cstring>

TEST_CASE("source gate: generated ONLY on exact token") {
    // The one true value.
    CHECK(mc2_unit_definition_source_is_generated("generated"));
    CHECK(mc2_unit_definition_source_is_generated("GENERATED"));   // case-insensitive
    CHECK(mc2_unit_definition_source_is_generated("Generated"));
    // Everything else -> legacy (default), so unset/typo/legacy stays byte-identical.
    CHECK_FALSE(mc2_unit_definition_source_is_generated(nullptr));
    CHECK_FALSE(mc2_unit_definition_source_is_generated(""));
    CHECK_FALSE(mc2_unit_definition_source_is_generated("legacy"));
    CHECK_FALSE(mc2_unit_definition_source_is_generated("0"));
    CHECK_FALSE(mc2_unit_definition_source_is_generated("gen"));        // prefix, not full
    CHECK_FALSE(mc2_unit_definition_source_is_generated("generatedx")); // trailing junk
    CHECK_FALSE(mc2_unit_definition_source_is_generated("1"));
}

TEST_CASE("variant slug mirrors converter _variant_slug") {
    char buf[64];
    // "Atlas Prime" -> "atlas_prime"
    CHECK(std::strcmp(mc2GeneratedVariantSlug("Atlas Prime", 0, buf, sizeof(buf)),
                      "atlas_prime") == 0);
    // apostrophes dropped (not turned to '_'): "O'Neil MkII" -> "oneil_mkii"
    CHECK(std::strcmp(mc2GeneratedVariantSlug("O'Neil MkII", 0, buf, sizeof(buf)),
                      "oneil_mkii") == 0);
    // runs of separators collapse to one '_'; leading/trailing stripped
    CHECK(std::strcmp(mc2GeneratedVariantSlug("  Mad-Cat / MkII  ", 0, buf, sizeof(buf)),
                      "mad_cat_mkii") == 0);
    // empty / all-separator -> "v<index>"
    CHECK(std::strcmp(mc2GeneratedVariantSlug("", 3, buf, sizeof(buf)), "v3") == 0);
    CHECK(std::strcmp(mc2GeneratedVariantSlug("---", 7, buf, sizeof(buf)), "v7") == 0);
    CHECK(std::strcmp(mc2GeneratedVariantSlug(nullptr, 2, buf, sizeof(buf)), "v2") == 0);
}

TEST_CASE("apply generated tokens: present -> sourced, absent -> legacy sentinel") {
    // Present, recognized generated token overrides the field.
    UnitProfile p;
    p.locomotion   = LOCO_WHEELED;          // legacy sentinel to detect a write
    p.damageSchema = DMG_VEHICLE_FACINGS;
    CHECK(mc2ApplyGeneratedLocomotion("MechWalker", p));
    CHECK(p.locomotion == LOCO_MECH_WALKER);   // sourced from generated
    CHECK(mc2ApplyGeneratedDamageSchema("Mech8Part", p));
    CHECK(p.damageSchema == DMG_MECH_8PART);

    // Absent (null/empty) token -> no write, field keeps legacy value, returns false.
    UnitProfile q;
    q.locomotion   = LOCO_TRACKED;
    q.damageSchema = DMG_SQUAD;
    CHECK_FALSE(mc2ApplyGeneratedLocomotion(nullptr, q));
    CHECK_FALSE(mc2ApplyGeneratedLocomotion("", q));
    CHECK(q.locomotion == LOCO_TRACKED);       // untouched -> legacy fallback
    CHECK_FALSE(mc2ApplyGeneratedDamageSchema(nullptr, q));
    CHECK(q.damageSchema == DMG_SQUAD);
}
