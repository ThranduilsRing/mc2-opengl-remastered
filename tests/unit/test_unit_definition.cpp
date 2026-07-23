// SINGLE-SOURCE-UNIT-DEFINITION-1 (Phase 2, slice 7): offline doctest for the
// UnitDefinition facade + the single UI-facet resolution point (mergeUIProfile).
// Pure logic only -- no engine/GL/IO. Proves: (a) the facade is a non-owning view
// that reads facets straight off the UnitProfile source of truth, (b) mergeUIProfile
// is the ONE deterministic merge both readers share (registry over inline, unset
// falls through), (c) an all-unset result yields legacy at every accessor (stock
// parity), fixing the slice-6 split where the two readers resolved UI separately.
#include "doctest.h"
#include "unit_definition.h"
#include <string>

// --- The facade is a VIEW: accessors read the live UnitProfile fields. --------
TEST_CASE("UnitDefinition aggregates the facets by reference (no copy)") {
    UnitProfile prof;                         // stock defaults
    prof.baselineCapabilities.set(CAP_JUMP, true);
    prof.locomotion   = LOCO_MECH_WALKER;
    prof.damageSchema = DMG_MECH_8PART;
    prof.ui.icon      = UIAssetName("hbk_small.tga");

    UnitDefinition def("mech_hbk4g.fit", prof);

    CHECK(std::string(def.identity()) == "mech_hbk4g.fit");
    CHECK(def.capabilities().has(CAP_JUMP));
    CHECK(def.locomotion()   == LOCO_MECH_WALKER);
    CHECK(def.damageSchema() == DMG_MECH_8PART);
    CHECK(std::string(def.ui().icon.c_str()) == "hbk_small.tga");

    // VIEW semantics: mutate the source AFTER construction -> facade reflects it,
    // proving it references (not copies) the single source of truth.
    prof.locomotion = LOCO_TRACKED;
    CHECK(def.locomotion() == LOCO_TRACKED);
}

// --- The accessor helpers put the declared-else-legacy verdict in one place. --
TEST_CASE("UnitDefinition accessors resolve declared UI else legacy") {
    UnitProfile prof;                         // all UI unset (stock)
    UnitDefinition stock("v_lctank.fit", prof);
    // Stock: every field unset -> every accessor returns the legacy slot value.
    CHECK(std::string(stock.iconAsset("leg_icon.tga"))       == "leg_icon.tga");
    CHECK(std::string(stock.portraitAsset("leg_port.tga"))   == "leg_port.tga");
    CHECK(std::string(stock.silhouetteAsset("leg_sil.tga"))  == "leg_sil.tga");
    CHECK(stock.nameStringId(137) == 137);
    CHECK(stock.encyclopediaStringId(909) == 909);

    prof.ui.icon         = UIAssetName("declared_icon.tga");
    prof.ui.nameStringId = 4200;
    UnitDefinition authored("v_lctank.fit", prof);
    CHECK(std::string(authored.iconAsset("leg_icon.tga")) == "declared_icon.tga");
    CHECK(authored.nameStringId(137) == 4200);
    // Undeclared fields still fall through to legacy.
    CHECK(std::string(authored.portraitAsset("leg_port.tga")) == "leg_port.tga");
    CHECK(authored.encyclopediaStringId(909) == 909);
}

// --- mergeUIProfile: the ONE resolution point both readers share. -------------
TEST_CASE("mergeUIProfile: null sources yield an all-unset (legacy) profile") {
    const UIProfile merged = mergeUIProfile(nullptr, nullptr);
    CHECK(merged.icon.empty());
    CHECK(merged.portrait.empty());
    CHECK(merged.minimap.empty());
    CHECK(merged.silhouette.empty());
    CHECK(merged.nameStringId == UI_STRING_ID_UNSET);
    CHECK(merged.encyclopediaStringId == UI_STRING_ID_UNSET);
}

TEST_CASE("mergeUIProfile: inline base used when registry absent") {
    UIProfile base;
    base.icon         = UIAssetName("inline_icon.tga");
    base.nameStringId = 11;
    const UIProfile merged = mergeUIProfile(&base, nullptr);
    CHECK(std::string(merged.icon.c_str()) == "inline_icon.tga");
    CHECK(merged.nameStringId == 11);
}

TEST_CASE("mergeUIProfile: registry override wins per declared field, unset falls through") {
    UIProfile base;                           // inline .fit block
    base.icon         = UIAssetName("inline_icon.tga");
    base.silhouette   = UIAssetName("inline_sil.tga");
    base.nameStringId = 11;

    UIProfile over;                           // additive-merge registry entry
    over.icon                 = UIAssetName("reg_icon.tga");   // overrides inline
    over.portrait             = UIAssetName("reg_port.tga");   // adds (inline had none)
    over.encyclopediaStringId = 77;                            // adds

    const UIProfile merged = mergeUIProfile(&base, &over);
    // Registry (over) wins where it declared:
    CHECK(std::string(merged.icon.c_str())     == "reg_icon.tga");
    CHECK(std::string(merged.portrait.c_str()) == "reg_port.tga");
    CHECK(merged.encyclopediaStringId == 77);
    // Inline (base) survives where the registry was silent:
    CHECK(std::string(merged.silhouette.c_str()) == "inline_sil.tga");
    CHECK(merged.nameStringId == 11);
    // Neither declared minimap -> still unset (legacy downstream).
    CHECK(merged.minimap.empty());
}

// --- The slice-6 split fix, expressed as an equivalence: whichever reader builds
//     the facade, feeding the SAME two sources through mergeUIProfile yields the
//     SAME facet -> the sim-side ui() can no longer diverge from the mechlab. ---
TEST_CASE("both readers resolve to identical UI facet via the shared merge") {
    UIProfile inline_block;  inline_block.icon = UIAssetName("a.tga");
    UIProfile registry;      registry.portrait = UIAssetName("b.tga");

    // Sim/mover side and logistics side each call the same merge with the same
    // inputs; the resulting facet is identical field-for-field.
    const UIProfile sim   = mergeUIProfile(&inline_block, &registry);
    const UIProfile logis = mergeUIProfile(&inline_block, &registry);

    CHECK(std::string(sim.icon.c_str())     == std::string(logis.icon.c_str()));
    CHECK(std::string(sim.portrait.c_str()) == std::string(logis.portrait.c_str()));
    CHECK(sim.nameStringId == logis.nameStringId);

    UnitProfile prof; prof.ui = sim;
    UnitDefinition def("x.fit", prof);
    CHECK(std::string(def.iconAsset("leg.tga"))     == "a.tga");
    CHECK(std::string(def.portraitAsset("leg.tga")) == "b.tga");
}
