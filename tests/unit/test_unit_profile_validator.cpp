// tests/unit/test_unit_profile_validator.cpp
// UNITPROFILE-VALIDATOR-1 (slice 6 of the UNIT-PROFILE arc).
//
// Pure-logic doctest coverage for unit_profile_validator.h: the load-time
// construction-rules validator. Every LIVE reason code must be reachable, and
// the stock-parity guarantee (walker+jump -> none, vehicle-no-jump -> none)
// must hold. Header-only, drags no engine/GL/IO. Mirrors test_unit_actions.cpp.
#include "doctest.h"
#include "unit_profile_validator.h"
#include "action_executor_registry.h"

// The engine-side singleton binds UnitAction::Jump -> mech jump-jet executor,
// compatible ONLY with LOCO_MECH_WALKER (action_executor_registry.h). Reproduce
// that binding with a local registry so the validator sees the same truth the
// runtime does (the .cpp holding theActionExecutorRegistry() is not linked here).
static ActionExecutorRegistry stockJumpRegistry() {
    ActionExecutorRegistry reg;
    reg.bind(UnitAction::Jump, kMechJumpExecutor, {LOCO_MECH_WALKER});
    return reg;
}

// -------------------------------------------------------------------------
// Stock parity: the two shapes a clean stock run produces.
// -------------------------------------------------------------------------

TEST_CASE("validator: stock jet mech (walker + CAP_JUMP) produces ZERO findings") {
    // A walker mech granted jump IS compatible with the bound executor -> coherent.
    const ActionExecutorRegistry reg = stockJumpRegistry();
    UnitProfileFacts facts;
    facts.grantsJump = true;
    facts.locomotion = LOCO_MECH_WALKER;
    CHECK(validateUnitProfile(facts, reg).empty());
}

TEST_CASE("validator: stock vehicle (no CAP_JUMP) produces ZERO findings") {
    // A vehicle that does not claim jump has nothing to prove, regardless of
    // its (non-walker) locomotion model -> coherent.
    const ActionExecutorRegistry reg = stockJumpRegistry();
    UnitProfileFacts facts;
    facts.grantsJump = false;
    facts.locomotion = LOCO_WHEELED;
    CHECK(validateUnitProfile(facts, reg).empty());

    facts.locomotion = LOCO_TRACKED;  CHECK(validateUnitProfile(facts, reg).empty());
    facts.locomotion = LOCO_HOVER;    CHECK(validateUnitProfile(facts, reg).empty());
    facts.locomotion = LOCO_INFANTRY; CHECK(validateUnitProfile(facts, reg).empty());
}

// -------------------------------------------------------------------------
// LIVE reason code: JUMP granted but no compatible executor.
// -------------------------------------------------------------------------

TEST_CASE("validator: vehicle granted CAP_JUMP -> JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR") {
    // The real audit case: a wheeled/tracked/hover unit authored with jump. The
    // jump executor is bound only for LOCO_MECH_WALKER, so no compatible executor
    // exists for this chassis -> exactly one finding with the live reason code.
    const ActionExecutorRegistry reg = stockJumpRegistry();
    for (LocomotionModel loco : {LOCO_WHEELED, LOCO_TRACKED, LOCO_HOVER, LOCO_INFANTRY}) {
        UnitProfileFacts facts;
        facts.grantsJump = true;
        facts.locomotion = loco;
        std::vector<UnitProfileFinding> f = validateUnitProfile(facts, reg);
        REQUIRE(f.size() == 1);
        CHECK(f[0].code == UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR);
        CHECK(f[0].message != nullptr);
    }
}

TEST_CASE("validator: reflects the registry, not the class -- vehicle+MechWalker is coherent") {
    // If a vehicle's locomotion facet is DATA-overridden to MechWalker, the bound
    // executor IS compatible -> no finding. Proves the validator keys on the facet
    // + registry, never on C++ class (the whole point of the arc).
    const ActionExecutorRegistry reg = stockJumpRegistry();
    UnitProfileFacts facts;
    facts.grantsJump = true;
    facts.locomotion = LOCO_MECH_WALKER;   // e.g. .fit declared Locomotion=MechWalker
    CHECK(validateUnitProfile(facts, reg).empty());
}

TEST_CASE("validator: empty registry -> even a walker+jump trips the check") {
    // Defensive: if NOTHING is bound (executor registry not populated), a
    // jump-granting profile has no compatible executor at all -> the finding
    // fires. Confirms the check depends on the registry, not a hardcoded walker
    // assumption.
    ActionExecutorRegistry empty;   // no bindings
    UnitProfileFacts facts;
    facts.grantsJump = true;
    facts.locomotion = LOCO_MECH_WALKER;
    std::vector<UnitProfileFinding> f = validateUnitProfile(facts, empty);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR);
}

// -------------------------------------------------------------------------
// Every LIVE reason code is reachable (guards against a refactor making one
// dead). Stubbed Phase-2 codes are intentionally NOT emitted this slice.
// -------------------------------------------------------------------------

TEST_CASE("validator: every LIVE reason code is reachable") {
    const ActionExecutorRegistry reg = stockJumpRegistry();

    // UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR
    UnitProfileFacts jumpVeh;
    jumpVeh.grantsJump = true;
    jumpVeh.locomotion = LOCO_WHEELED;
    std::vector<UnitProfileFinding> f = validateUnitProfile(jumpVeh, reg);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR);

    // UPV_HARDPOINT_SOCKET_MISSING is now LIVE (PRESENTATION-SOCKET-PROFILE-1);
    // its reachability is covered in the declared-socket cases below.
    // ACTION-ANIMATION-BINDINGS-1 (slice 5) made UPV_ANIMATION_FALLBACK_MISSING
    // live; its reachability is covered in test_action_anim_tag.cpp
    // (validateAnimationFallbacks). UI-PROFILE-1 (slice 6) APPENDED
    // UPV_UI_ASSET_MISSING (reachability covered in test_ui_profile.cpp) -> the
    // vocabulary grew to 6. Appended, never renumbered.
    CHECK(UPV__COUNT == 6);   // vocabulary size lock
}

// -------------------------------------------------------------------------
// LOADOUT-HARDPOINT-JOIN-1 (Phase-2 slice 3): declared loadout->hardpoint join.
// Pure check: for each DECLARED join, three coherence rules — weapon mount-class
// accepted, damage location in schema, socket present. Stock loadouts declare no
// HardpointId -> empty input -> zero findings (gate-ON parity).
// -------------------------------------------------------------------------

// Build a well-formed mech energy hardpoint on the right torso, socket present.
static LoadoutHardpointJoin goodEnergyJoin() {
    LoadoutHardpointJoin j;
    j.hardpoint.id = makeHardpointId("RightTorso_Energy_1");
    addWeaponTag(j.hardpoint, WTAG_ENERGY);
    j.hardpoint.damageLocation = DLOC_MECH_RTORSO;
    j.hardpoint.socket = HardpointSocketName("node_rt_energy");
    j.weaponTag     = WTAG_ENERGY;
    j.unitSchema    = DMG_MECH_8PART;
    j.socketPresent = true;
    return j;
}

TEST_CASE("join: no declared joins (stock) -> ZERO findings") {
    std::vector<LoadoutHardpointJoin> none;
    CHECK(validateLoadoutHardpointJoins(none).empty());
}

TEST_CASE("join: a fully coherent declared join -> ZERO findings") {
    std::vector<LoadoutHardpointJoin> joins = { goodEnergyJoin() };
    CHECK(validateLoadoutHardpointJoins(joins).empty());
}

TEST_CASE("join: weapon mount-class not accepted -> WEAPON_CLASS_REJECTED") {
    // AC20 (Ballistic) mounted on an Energy-only hardpoint.
    LoadoutHardpointJoin j = goodEnergyJoin();
    j.weaponTag = WTAG_BALLISTIC;
    std::vector<UnitProfileFinding> f = validateLoadoutHardpointJoins({j});
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_HARDPOINT_WEAPON_CLASS_REJECTED);
    CHECK(f[0].message != nullptr);
}

TEST_CASE("join: hardpoint damage location absent from schema -> DAMAGE_LOCATION_MISSING") {
    // A mech-arm hardpoint declared on a vehicle-facings unit (no arms).
    LoadoutHardpointJoin j = goodEnergyJoin();
    j.hardpoint.damageLocation = DLOC_MECH_RARM;
    j.unitSchema = DMG_VEHICLE_FACINGS;
    std::vector<UnitProfileFinding> f = validateLoadoutHardpointJoins({j});
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_HARDPOINT_DAMAGE_LOCATION_MISSING);
}

TEST_CASE("join: declared socket absent on model -> SOCKET_MISSING") {
    LoadoutHardpointJoin j = goodEnergyJoin();
    j.socketPresent = false;
    std::vector<UnitProfileFinding> f = validateLoadoutHardpointJoins({j});
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_HARDPOINT_SOCKET_MISSING);
}

TEST_CASE("join: a single join can trip all three checks at once") {
    // Ballistic weapon on an energy-only, wrong-schema, missing-socket hardpoint.
    LoadoutHardpointJoin j = goodEnergyJoin();
    j.weaponTag = WTAG_BALLISTIC;              // -> WEAPON_CLASS_REJECTED
    j.hardpoint.damageLocation = DLOC_MECH_RARM;
    j.unitSchema = DMG_VEHICLE_FACINGS;        // -> DAMAGE_LOCATION_MISSING
    j.socketPresent = false;                   // -> SOCKET_MISSING
    std::vector<UnitProfileFinding> f = validateLoadoutHardpointJoins({j});
    REQUIRE(f.size() == 3);
    CHECK(f[0].code == UPV_HARDPOINT_WEAPON_CLASS_REJECTED);
    CHECK(f[1].code == UPV_HARDPOINT_DAMAGE_LOCATION_MISSING);
    CHECK(f[2].code == UPV_HARDPOINT_SOCKET_MISSING);
}

TEST_CASE("join: multiple joins accumulate findings per entry") {
    std::vector<LoadoutHardpointJoin> joins = { goodEnergyJoin(), goodEnergyJoin() };
    joins[1].weaponTag = WTAG_MISSILE;   // second one rejected
    std::vector<UnitProfileFinding> f = validateLoadoutHardpointJoins(joins);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_HARDPOINT_WEAPON_CLASS_REJECTED);
}

// -------------------------------------------------------------------------
// PRESENTATION-SOCKET-PROFILE-1 (Phase-2 slice 2): declared-socket coherence.
// Pure check: a socket declared in data but absent on the model -> one
// UPV_HARDPOINT_SOCKET_MISSING finding. Stock units declare none -> zero.
// -------------------------------------------------------------------------

static DeclaredSocketCheck sock(const char* name, bool present) {
    DeclaredSocketCheck d;
    d.socket  = HardpointSocketName(name);
    d.present = present;
    return d;
}

TEST_CASE("sockets: no declared sockets (stock) -> ZERO findings") {
    // The stock-parity guarantee: every shipped unit declares no presentation
    // sockets, so the check is a no-op and gate-ON stock stays byte-parity.
    std::vector<DeclaredSocketCheck> none;
    CHECK(validateDeclaredSockets(none).empty());
}

TEST_CASE("sockets: all declared sockets present -> ZERO findings") {
    std::vector<DeclaredSocketCheck> declared = {
        sock("RightTorso_Energy_1", true),
        sock("LeftArm_Ballistic_1", true),
    };
    CHECK(validateDeclaredSockets(declared).empty());
}

TEST_CASE("sockets: one declared socket absent -> one SOCKET_MISSING finding") {
    std::vector<DeclaredSocketCheck> declared = {
        sock("RightTorso_Energy_1", true),
        sock("Vehicle_Turret_Heavy_1", false),   // declared but not on the model
    };
    std::vector<UnitProfileFinding> f = validateDeclaredSockets(declared);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_HARDPOINT_SOCKET_MISSING);
    CHECK(f[0].message != nullptr);
}

TEST_CASE("sockets: every declared socket absent -> one finding each") {
    std::vector<DeclaredSocketCheck> declared = {
        sock("A", false), sock("B", false), sock("C", false),
    };
    std::vector<UnitProfileFinding> f = validateDeclaredSockets(declared);
    REQUIRE(f.size() == 3);
    for (const UnitProfileFinding& x : f)
        CHECK(x.code == UPV_HARDPOINT_SOCKET_MISSING);
}
