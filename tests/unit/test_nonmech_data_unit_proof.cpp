// tests/unit/test_nonmech_data_unit_proof.cpp
// NON-MECH-DATA-UNIT-PROOF-1 (Phase 2, slice 9 — the Phase-2 CLOSER).
//
// Offline pure-logic coverage for the DATA-DRIVEN presentation/loadout stack the
// slice-9 engine proof hook exercises end-to-end behind
// MC2_UNIT_PROFILE_NONMECH_PROOF. The engine hook (BattleMech::
// nonmechDataUnitMountProof) synthesizes exactly the DATA a hardpoint/UI source
// would declare and drives the AUTHORITATIVE branches; here we prove those
// branches' pure verdicts in isolation, GL/IO-free:
//
//   1. Mount route (slices 2+4): a declared HardpointProfile whose socket names a
//      REAL model node resolves to that node's INDEX, and chooseWeaponNode() picks
//      it OVER the legacy round-robin -> the mount is DATA-selected, not class.
//   2. UI override (slices 6/7): a declared UIProfile icon wins over the legacy
//      accessor value via the SAME pure chooseUIAsset() the logistics UI uses.
//   3. Validation (slice 8): validateUnitDefinition() reports the declared config
//      COHERENT (zero findings) when the socket is present; flipping the socket to
//      absent yields exactly one finding; restoring it returns to zero.
//
// HONEST FRAMING: this proves the DATA path is selected by DATA (not the C++
// class) and is coherent. It is a proof of the mechanism, not a shipped data unit
// (Phase 3 = UnitDefinition facade + legacy synthesis).
#include "doctest.h"

#include <string>

#include "hardpoint.h"                 // chooseWeaponNode, HardpointProfile, socket
#include "ui_profile.h"                // UIProfile, chooseUIAsset
#include "unit_profile_validator.h"    // validateUnitDefinition + input facets
#include "action_executor_registry.h"  // ActionExecutorRegistry for the jump facet

// A model whose weapon nodes are indices [numSmokeNodes, numSmokeNodes+N). The
// engine's resolveWeaponNodeBySocketName() is a name->index scan of that range;
// here we model the OUTPUT of that scan directly (the pure verdict is what slice
// 9 relies on), matching the two integers chooseWeaponNode() consumes.

TEST_CASE("mount route: declared socket resolving to a real node wins over round-robin") {
    // The proof declares a hardpoint socketed to the LAST weapon node. Say the
    // shape has weapon nodes at indices 2..5 (numSmokeNodes=2, numWeaponNodes=4),
    // so the declared socket resolves to index 5. The legacy round-robin picks the
    // least-used node, index 2 (first weapon). DATA must win -> mount == 5, != 2.
    const int declaredSocketNode = 5;   // resolveWeaponNodeBySocketName(lastName)
    const int roundRobinNode     = 2;   // legacy getWeaponNode() least-used pick
    const int mount = chooseWeaponNode(/*hasHardpoint*/ true, declaredSocketNode, roundRobinNode);
    CHECK(mount == declaredSocketNode);         // authoritative branch fired
    CHECK(mount != roundRobinNode);             // NOT the class-bucket round-robin
}

TEST_CASE("mount route: an absent socket falls back (a bad declaration never breaks the mount)") {
    // If the declared socket name did not resolve on the shape (-1), the authoritative
    // branch declines and the legacy round-robin still mounts -> safety net intact.
    CHECK(chooseWeaponNode(/*hasHardpoint*/ true, /*socketNode*/ -1, /*roundRobin*/ 2) == 2);
}

TEST_CASE("UI override: a declared icon is DATA-selected over the legacy accessor value") {
    UIProfile ui;
    ui.icon = UIAssetName("proof_nonmech_icon");
    const char* legacy = "legacy_stock_icon";
    // The exact decision the engine hook logs and the logistics UI makes.
    CHECK(std::string(chooseUIAsset(ui.icon, legacy)) == "proof_nonmech_icon");
    // Stock parity sanity: an undeclared icon falls back to legacy verbatim.
    UIProfile stock;
    CHECK(std::string(chooseUIAsset(stock.icon, legacy)) == "legacy_stock_icon");
}

// Build the validation inputs the slice-9 proof config implies: a walker+jump
// unit (coherent) that declares ONE presentation socket. `socketPresent` toggles
// whether that socket resolves on the model.
static UnitDefinitionValidationInputs proofConfig(bool socketPresent) {
    UnitDefinitionValidationInputs in;
    in.profile.grantsJump = true;              // coherent jump facet on a walker
    in.profile.locomotion = LOCO_MECH_WALKER;
    DeclaredSocketCheck sock;
    sock.socket  = HardpointSocketName("core:proof_nonmech_socket");
    sock.present = socketPresent;
    in.declaredSockets.push_back(sock);
    return in;
}

TEST_CASE("validation: the declared proof config is COHERENT (zero findings) when its socket is present") {
    ActionExecutorRegistry reg;
    reg.bind(UnitAction::Jump, kMechJumpExecutor, {LOCO_MECH_WALKER});

    // Present socket + coherent walker+jump -> the whole definition validates clean.
    CHECK(validateUnitDefinition(proofConfig(/*socketPresent*/ true), reg).empty());

    // Deliberately break ONE field: mark the declared socket absent on the model.
    // Exactly one finding (the missing socket); the jump facet stays coherent.
    const std::vector<UnitProfileFinding> broken =
        validateUnitDefinition(proofConfig(/*socketPresent*/ false), reg);
    CHECK(broken.size() == 1);
    CHECK(broken[0].code == UPV_HARDPOINT_SOCKET_MISSING);

    // Fix it back -> coherent again (the break was the single declared field).
    CHECK(validateUnitDefinition(proofConfig(/*socketPresent*/ true), reg).empty());
}
