#ifndef UNIT_PROFILE_VALIDATOR_H
#define UNIT_PROFILE_VALIDATOR_H
// UNITPROFILE-VALIDATOR-1 (slice 6 of the UNIT-PROFILE arc).
//
// Load-time CONSTRUCTION-RULES validator: proves a unit's facets COHERE and
// yields actionable, modder-readable findings. This is the tabletop
// "can you legally build this unit?" layer, expressed as pure logic.
//
// REPORT-FIRST this slice: the validator only *describes* incoherence — it
// never rejects a spawn. The engine glue logs each finding (gated behind
// MC2_UNIT_PROFILE_DATA) so gate-OFF and gate-ON stay behavioral parity;
// enforcement (refusing an impossible profile) is a deliberate future toggle.
//
// Pure, header-only (plain enums / bools), so it compiles into the offline
// `mc2_tests` target and drags NO engine/GL/IO code. It reuses the facet
// vocabularies already established by earlier slices:
//   * capability.h                 -- CAP_JUMP (the granted-capability bit)
//   * action_executor_registry.h   -- ActionExecutorRegistry, UnitAction, LocomotionModel
//   * locomotion_facet.h / damage_schema.h -- facet id enums (pulled transitively)
//
// The join it proves: "the profile GRANTS an ability, and the unit's other
// facets can actually deliver it." The one live check is the real audit case:
// CAP_JUMP granted but no compatible jump executor exists for this locomotion
// model -> the "capability-granted vehicle reports MissingExecutor" failure,
// caught at LOAD instead of silently at query time.
#include <vector>
#include "capability.h"                  // CAP_JUMP, UnitAction
#include "action_executor_registry.h"    // ActionExecutorRegistry, LocomotionModel
#include "hardpoint.h"                   // HardpointSocketName (PRESENTATION-SOCKET-PROFILE-1)
#include "action_anim_tag.h"             // ActionAnimTag, animTagResolves (ACTION-ANIMATION-BINDINGS-1)

// -------------------------------------------------------------------------
// REASON-CODE VOCABULARY.
//
// Stable, modder-facing identifiers. Order is NOT significance; each code is a
// distinct coherence rule. Future slices (esp. Phase-2 UNIT-DEFINITION-VALIDATOR-1)
// ADD codes here and grow the vocabulary — they must NOT renumber existing ones,
// because logs / mod tooling key on the name. `UPV_` = Unit-Profile-Validator.
//
// LIVE:
//   UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR  (UNITPROFILE-VALIDATOR-1, slice 6)
//   UPV_HARDPOINT_SOCKET_MISSING             (PRESENTATION-SOCKET-PROFILE-1, Phase-2
//        slice 2) — a presentation profile DECLARES a model socket (node name) that
//        the actually-loaded model shape does not expose. Report-only: tells a modder
//        "declared socket X does not exist on the model." Emitted by the pure
//        validateDeclaredSockets() below; the engine glue supplies the present/absent
//        bool per declared socket from the loaded shape (Mech3DAppearanceType::init).
//
//   UPV_HARDPOINT_DAMAGE_LOCATION_MISSING  (now LIVE — LOADOUT-HARDPOINT-JOIN-1,
//        Phase-2 slice 3) — a loadout entry DECLARES a HardpointId whose damage
//        location does not exist in the unit's DamageSchema ("mounts on LeftArm but
//        the unit's DamageSchema has no LeftArm"). Emitted by the pure
//        validateLoadoutHardpointJoins() below via hardpointResolvesInSchema().
//   UPV_HARDPOINT_WEAPON_CLASS_REJECTED  (LIVE — LOADOUT-HARDPOINT-JOIN-1, Phase-2
//        slice 3) — a loadout entry mounts a weapon whose mount-class is NOT in the
//        declared hardpoint's accepted weapon-tag set ("AC20 (Ballistic) on an
//        Energy-only hardpoint"). Emitted via hardpointAcceptsWeapon().
//
//   UPV_ANIMATION_FALLBACK_MISSING  (now LIVE — ACTION-ANIMATION-BINDINGS-1,
//        Phase-2 slice 5) — an action's ActionAnimTag has a fallback chain that
//        dead-ends: NONE of its candidate legacy gesture ids is available on the
//        unit's loaded skeleton ("Jump exists but no JumpLand/JumpLoop/Move/Idle
//        clip delivers it"). Emitted by the pure validateAnimationFallbacks()
//        below via animTagResolves() (code/action_anim_tag.h); the engine glue
//        supplies the per-gesture availability predicate from the loaded shape.
enum UnitProfileReasonCode {
    // --- LIVE ---
    UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR = 0,

    // --- LIVE (LOADOUT-HARDPOINT-JOIN-1, Phase-2 slice 3) ---
    UPV_HARDPOINT_DAMAGE_LOCATION_MISSING,

    // --- LIVE (PRESENTATION-SOCKET-PROFILE-1, Phase-2 slice 2) ---
    UPV_HARDPOINT_SOCKET_MISSING,

    // --- LIVE (ACTION-ANIMATION-BINDINGS-1, Phase-2 slice 5) ---
    UPV_ANIMATION_FALLBACK_MISSING,

    // --- LIVE (LOADOUT-HARDPOINT-JOIN-1, Phase-2 slice 3) ---
    // Appended AFTER the reserved stub so existing codes keep their numbers (logs /
    // mod tooling key on the value); the vocabulary never renumbers.
    UPV_HARDPOINT_WEAPON_CLASS_REJECTED,

    // --- LIVE (UI-PROFILE-1, Phase-2 slice 6) ---
    // A UIProfile DECLARES a UI asset (icon / portrait / minimap / silhouette
    // filename) that does NOT resolve/exist where the engine glue could check it
    // ("declared icon file 'foo.tga' not found"). REPORT-ONLY: tells a modder the
    // named asset is missing so the override would silently fall through to the
    // legacy slot. Emitted by the pure validateDeclaredUIAssets() below; the engine
    // glue supplies the resolves/absent bool per declared asset (a file existence
    // check). Appended last so existing codes keep their numbers.
    UPV_UI_ASSET_MISSING,

    UPV__COUNT
};

// A single coherence finding. `code` is the stable machine key; `message` is a
// short human-readable, modder-facing summary (the engine glue prefixes it with
// the unit's type name when logging). Kept a flat POD for trivial test use.
struct UnitProfileFinding {
    UnitProfileReasonCode code;
    const char*           message;   // static string literal; no ownership
};

// Pure facet inputs the validator reasons over. Booleans / facet-enums ONLY —
// no engine types — so this compiles into `mc2_tests`. Future slices add fields
// (hardpoint table, socket set, animation bindings) for the stubbed checks; each
// stays at a non-blocking default until its slice lands, exactly like
// ActionQueryInputs in unit_action.h.
struct UnitProfileFacts {
    bool            grantsJump    = false;            // CAP_JUMP granted by the profile
    LocomotionModel locomotion    = LOCO_WHEELED;     // the unit's locomotion facet
};

// LIVE CHECK (CAP_JUMP <-> executor coherence):
//   grantsJump && !registry.isCompatible(Jump, locomotion)
//     -> UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR
//
// A walker mech granted CAP_JUMP is compatible (the jump executor lists
// LOCO_MECH_WALKER) -> ZERO findings. A vehicle NOT granted CAP_JUMP has nothing
// to prove -> ZERO findings. A vehicle (or any non-walker locomotion) that IS
// granted CAP_JUMP has an incoherent profile: it claims jump but no bound
// executor can run it on this chassis -> exactly this finding. That is the stock
// parity guarantee: stock mechs (walker + jump) and stock vehicles (no jump)
// both produce nothing; only an impossible authored profile trips the check.
//
// `registry` is the same ActionExecutorRegistry gameplay queries at runtime, so
// the validator's verdict and the runtime query can never diverge.
inline std::vector<UnitProfileFinding>
validateUnitProfile(const UnitProfileFacts& facts,
                    const ActionExecutorRegistry& registry) {
    std::vector<UnitProfileFinding> findings;

    // --- LIVE: JUMP granted but no compatible executor for this locomotion. ---
    if (facts.grantsJump &&
        !registry.isCompatible(UnitAction::Jump, facts.locomotion)) {
        findings.push_back(UnitProfileFinding{
            UPV_JUMP_GRANTED_NO_COMPATIBLE_EXECUTOR,
            "JUMP granted but no compatible jump executor for this locomotion model"});
    }

    // --- STUBBED (Phase 2): hardpoint->damage-location, missing socket, missing
    // animation fallback. Not implemented this slice; the reason codes exist so
    // the vocabulary is stable and this is the single site future slices extend.
    // Do NOT add Phase-2 checks here without their facet inputs on UnitProfileFacts.

    return findings;
}

// -------------------------------------------------------------------------
// PRESENTATION-SOCKET-PROFILE-1 (Phase-2 slice 2): declared-socket coherence.
//
// A presentation profile MAY declare model sockets (node-name strings) that
// later slices resolve against the model via GetTransformedNodePosition. This
// pure check answers the modder question: "does every socket I declared
// actually EXIST on the model I ship?" — WITHOUT this header knowing anything
// about the engine's shape type. The caller (engine glue in
// Mech3DAppearanceType::init) supplies, per declared socket, a single bool:
// `present` == "a node with this exact name exists on the loaded shape."
//
// PURE + REPORT-ONLY. One finding per DECLARED-but-ABSENT socket, carrying the
// live UPV_HARDPOINT_SOCKET_MISSING code. An empty declaration list (every stock
// unit — none declare sockets) yields ZERO findings, so gate-ON stock stays at
// full parity. This does NOT touch getWeaponNode / nodeData / any render path;
// the socket table is read-and-validated only, not yet authoritative (slice 4).
//
// The message is a fixed literal (findings are flat POD with no ownership); the
// engine glue already prefixes the type name and can append the socket name when
// logging, exactly like the jump finding. The bool-per-socket split keeps the
// engine's node-name lookup (the only impure part) OUT of this header.

// One declared presentation socket + whether it resolved on the model.
struct DeclaredSocketCheck {
    HardpointSocketName socket;   // the declared node-name (from data)
    bool                present;  // engine glue: does a node of this name exist?
};

// LIVE CHECK: for each declared socket that is NOT present on the model, emit one
// UPV_HARDPOINT_SOCKET_MISSING finding. Empty input (or every socket present) ->
// no findings (the stock-parity guarantee: stock units declare no sockets).
inline std::vector<UnitProfileFinding>
validateDeclaredSockets(const std::vector<DeclaredSocketCheck>& declared) {
    std::vector<UnitProfileFinding> findings;
    for (const DeclaredSocketCheck& d : declared) {
        if (!d.present) {
            findings.push_back(UnitProfileFinding{
                UPV_HARDPOINT_SOCKET_MISSING,
                "declared presentation socket not found on model"});
        }
    }
    return findings;
}

// -------------------------------------------------------------------------
// LOADOUT-HARDPOINT-JOIN-1 (Phase-2 slice 3): declared loadout->hardpoint join.
//
// A loadout entry MAY optionally name a HardpointId (from data). When it does,
// this pure check proves the declared join COHERES across the three index spaces
// the recon found joined-by-nothing today:
//   1. weapon mount-class  -> the hardpoint's accepted weapon-tag set
//        (hardpointAcceptsWeapon)          -> UPV_HARDPOINT_WEAPON_CLASS_REJECTED
//   2. hardpoint damage-loc -> the unit's DamageSchema
//        (hardpointResolvesInSchema)       -> UPV_HARDPOINT_DAMAGE_LOCATION_MISSING
//   3. hardpoint socket     -> present on the loaded model
//        (engine glue supplies the bool)   -> UPV_HARDPOINT_SOCKET_MISSING
//
// REPORT-ONLY, PURE. It reasons only over declared joins the caller gathered; a
// loadout entry that names NO hardpoint is never passed in, so it produces
// nothing. Stock loadouts declare no HardpointId -> empty input -> ZERO findings
// -> gate-ON stock parity. It does NOT touch getWeaponNode / inventory[].facing /
// any mount or render path — the actual visual mount stays legacy (slice 4).
//
// The `socketPresent` bool is the SAME impure split as DeclaredSocketCheck: the
// engine glue does the model node-name lookup and hands this header a bool, so
// the header stays engine/GL/IO-free and offline-testable. A hardpoint whose
// socket field is EMPTY ("not mapped yet") is not socket-checked — an unmapped
// socket is a different (future) finding, not a "missing on model" one; the glue
// signals this by setting socketPresent=true for empty-socket joins.

// One declared loadout->hardpoint join to validate. The caller (engine glue at
// LogisticsData::initVariants / spawn) resolves the loadout entry's optional
// HardpointId to its HardpointProfile, reads the weapon's mount-class, knows the
// unit's DamageSchema, and supplies socketPresent per the loaded model.
struct LoadoutHardpointJoin {
    HardpointProfile hardpoint;      // the resolved declared hardpoint
    WeaponTag        weaponTag;      // the mounted weapon's mount-class
    DamageSchemaId   unitSchema;     // the unit's damage schema
    bool             socketPresent;  // engine glue: node of hardpoint.socket exists?

    LoadoutHardpointJoin()
        : weaponTag(WTAG_ENERGY), unitSchema(DMG_MECH_8PART), socketPresent(true) {}
};

// LIVE CHECK: for each declared join, emit one finding per failed coherence rule
// (a single join can trip more than one). Empty input -> no findings (the
// stock-parity guarantee: stock loadouts declare no HardpointId). Order of
// emitted findings mirrors the check order above so glue can pair them with the
// join it walked.
inline std::vector<UnitProfileFinding>
validateLoadoutHardpointJoins(const std::vector<LoadoutHardpointJoin>& joins) {
    std::vector<UnitProfileFinding> findings;
    for (const LoadoutHardpointJoin& j : joins) {
        // 1. weapon mount-class accepted by this hardpoint?
        if (!hardpointAcceptsWeapon(j.hardpoint, j.weaponTag)) {
            findings.push_back(UnitProfileFinding{
                UPV_HARDPOINT_WEAPON_CLASS_REJECTED,
                "weapon mount-class not accepted by the declared hardpoint"});
        }
        // 2. hardpoint's damage location exists in the unit's DamageSchema?
        if (!hardpointResolvesInSchema(j.hardpoint, j.unitSchema)) {
            findings.push_back(UnitProfileFinding{
                UPV_HARDPOINT_DAMAGE_LOCATION_MISSING,
                "declared hardpoint damage location absent from unit DamageSchema"});
        }
        // 3. hardpoint's model socket present on the loaded model?
        if (!j.socketPresent) {
            findings.push_back(UnitProfileFinding{
                UPV_HARDPOINT_SOCKET_MISSING,
                "declared hardpoint socket not found on model"});
        }
    }
    return findings;
}

// -------------------------------------------------------------------------
// ACTION-ANIMATION-BINDINGS-1 (Phase-2 slice 5): animation-fallback coherence.
//
// A unit's AnimationBindingProfile maps each action's ActionAnimTag to a fallback
// chain of legacy gesture ids (code/action_anim_tag.h). This pure check answers
// the modder question: "for every action this unit can perform, does its skeleton
// actually deliver SOME animation?" A tag whose entire chain is unavailable on the
// loaded skeleton is the failure "Jump exists but no JumpLand fallback" — reported
// at load in modder terms instead of a silent frozen mech in-mission.
//
// PURE + REPORT-ONLY. The caller (engine glue at Mech3DAppearanceType::init) gathers
// the tags this unit needs (from its granted actions) and supplies a predicate
// `gestureAvailable(gestureId)` == "the loaded shape has a clip for this gesture."
// A fully-rigged stock mech has every locomotion/idle gesture -> every needed tag
// resolves -> ZERO findings (gate-ON stock parity). Only a skeleton MISSING the
// clips its actions need trips the check. This touches NO gesture trigger and
// NO render path; setGestureGoal(int) stays the final legacy sink.

// One action's ActionAnimTag that a unit needs an animation for.
struct NeededAnimTag {
    ActionAnimTag tag;
};

// LIVE CHECK: for each needed tag whose fallback chain does NOT resolve to any
// available gesture, emit one UPV_ANIMATION_FALLBACK_MISSING finding. Empty input
// (or every tag resolving) -> no findings. `gestureAvailable` is the SAME impure
// split as socketPresent: the engine glue does the per-gesture presence lookup and
// hands this header a predicate, keeping the header engine/GL/IO-free.
template <typename GestureAvailablePred>
inline std::vector<UnitProfileFinding>
validateAnimationFallbacks(const std::vector<NeededAnimTag>& needed,
                           GestureAvailablePred gestureAvailable) {
    std::vector<UnitProfileFinding> findings;
    for (const NeededAnimTag& n : needed) {
        if (!animTagResolves(n.tag, gestureAvailable)) {
            findings.push_back(UnitProfileFinding{
                UPV_ANIMATION_FALLBACK_MISSING,
                "action animation tag has no available gesture in its fallback chain"});
        }
    }
    return findings;
}

// -------------------------------------------------------------------------
// UI-PROFILE-1 (Phase-2 slice 6): declared-UI-asset coherence.
//
// A UIProfile MAY declare UI asset filenames (icon / portrait / minimap /
// silhouette). This pure check answers the modder question: "does every UI asset
// I named actually resolve?" -- WITHOUT this header knowing how the engine checks
// a file (the impure existence lookup lives in the engine glue, which hands this
// header one bool per DECLARED asset: `resolves` == "the named asset exists").
//
// PURE + REPORT-ONLY. One finding per DECLARED-but-UNRESOLVED asset, carrying the
// live UPV_UI_ASSET_MISSING code. An empty declaration list (every stock unit --
// none declare UI assets) yields ZERO findings, so gate-ON stock stays at full
// parity. This does NOT touch the icon-atlas blit math or any accessor -- the UI
// assets are read-and-validated only; the override decision (chooseUIAsset) is a
// separate, equally pure step in ui_profile.h.

// One declared UI asset + whether it resolved. Mirrors DeclaredSocketCheck: the
// engine glue does the (impure) file existence check and hands this header a bool.
struct DeclaredUIAssetCheck {
    bool resolves;   // engine glue: does the named asset exist/resolve?
    DeclaredUIAssetCheck() : resolves(true) {}
};

// LIVE CHECK: for each declared asset that does NOT resolve, emit one
// UPV_UI_ASSET_MISSING finding. Empty input (or every asset resolving) -> no
// findings (the stock-parity guarantee: stock units declare no UI assets).
inline std::vector<UnitProfileFinding>
validateDeclaredUIAssets(const std::vector<DeclaredUIAssetCheck>& declared) {
    std::vector<UnitProfileFinding> findings;
    for (const DeclaredUIAssetCheck& d : declared) {
        if (!d.resolves) {
            findings.push_back(UnitProfileFinding{
                UPV_UI_ASSET_MISSING,
                "declared UI asset not found/resolvable"});
        }
    }
    return findings;
}

#endif // UNIT_PROFILE_VALIDATOR_H
