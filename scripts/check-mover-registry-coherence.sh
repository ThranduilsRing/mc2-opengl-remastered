#!/bin/sh
# scripts/check-mover-registry-coherence.sh
#
# META-FIX design A (CLASS A — entity-registry divergence, #50 class): static
# guard for the parallel per-mover registries (isOnGUI render gate, force-bar
# roster, tacmap blips, selection eligibility, commandability).
# Contract: .claude/META-FIX-DESIGNS-2026-07-16.json CLASS A. Runtime sibling:
# MC2_MOVER_COHERENCE_AUDIT (code/mission.cpp auditMoverCoherence).
#
# Rules (any hard hit = exit 1; anchors match SYMBOLS + SHAPE, never frozen
# line numbers):
#   S1a setOnGUI( call-site allowlist — outside the three definitions
#       (code/mover.h, code/gameobj.h, code/gvehicl.h) the exhaustive caller
#       set is {code/mission.cpp x4, code/mech_update.cpp x1}. A new writer turns
#       the repo red until consciously allowlisted with a pairing review.
#   S1b direct isOnGui member writes — pattern excludes the serializer LHS
#       (data->isOnGui = ...); allowlist {code/mover*.cpp x2 (init + load
#       restore), code/mover.h x1, code/gvehicl.h x1}.
#   S2  write-pairing windows — mech_update.cpp link-up setOnGUI(true) has
#       missionInterface->addMover(this) within 10 lines above; mission.cpp
#       part-spawn setOnGUI(true) has missionInterface->addMover(mover)
#       within 60 lines above (same function); missionInterface->removeMover
#       sites exactly {mission.cpp x2}; missionInterface->addMover sites
#       exactly {mission.cpp x2, mech_update.cpp x1}; ObjectManager->freeMover's
#       sole game-side caller (mission.cpp) preceded by removeMover within
#       10 lines.
#   S3  read-gate pins, TRIMMED — HARD pins only the three gates with
#       distinct symptom classes: GameObjectManager::moverInRect keeps
#       isOnGUI() (#50 selection class), gametacmap home-commander blip
#       branch keeps it, ControlGui::initMechs keeps it (roster-rebuild /
#       load-reconciliation). The four objmgr pick paths + missiongui
#       select-all trio are ADVISORY (WARN, exit stays 0) — their drift is
#       runtime-caught by I2/I5 in every tier1 run.
#   S4  #42b guard shape — hover guard must remain the narrow form
#       `target->isDisabled() && !target->getAppearance()`; a bare
#       `target && target->isDisabled())` guard is forbidden. THIS RULE IS
#       THE SOLE DEFENSE against historical #42b (runtime I3 cannot see it)
#       — never demote.
#   S5  link-up gate shape — BattleMech::update link-up branch keys on
#       `!isOnGUI()` alone; `status == OBJECT_STATUS_SHUTDOWN &&` within the
#       gate window is forbidden (reverting the #50 latch fix goes red).
#   S6  OFF-MAP STAGING CARVE-OUT (INVIS-VEHICLES-FIX-1) — the runtime audit
#       region (MOVER-COHERENCE-AUDIT-BEGIN/END sentinels in mission.cpp)
#       must exist, must NOT contain any positional term (getPosition,
#       IsGameSelectTerrainPosition, land->, Terrain::, GameMap, ->position):
#       off-map staged movers (mc2_10 convoy: authored off-map, onGUI=1,
#       brains alive, waiting on a mission flag) are a LEGITIMATE mover
#       state and position outside map bounds is NOT a registry violation.
#
# Usage:
#   sh scripts/check-mover-registry-coherence.sh              # check (CI)
#   sh scripts/check-mover-registry-coherence.sh --self-test
#
# Exit: 0 pass / 1 violation / 2 harness error.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# strip line-leading comments from grep -n output (house pattern).
# The Mission implementation is split across mission.cpp and mission_{update,update_audit,movers,init}.cpp; the S2 / S6 rules read it as one file.
mission_all_to() {
    cat "$ROOT/code/mission.cpp" "$ROOT/code/mission_update.cpp" "$ROOT/code/mission_update_audit.cpp" "$ROOT/code/mission_movers.cpp" "$ROOT/code/mission_init.cpp" "$ROOT/code/mission_init_load.cpp" > "$1"
}

strip_comments() {
    grep -Ev '^[^:]*:[0-9]+:[[:space:]]*(//|\*)' || true
}

# ---------------------------------------------------------------------------
# S1a: setOnGUI( caller allowlist. $1 = dir holding a code/ tree.
# Emits violations; empty when clean.
# ---------------------------------------------------------------------------
s1a_setongui_sites() {
    scan_root="$1"
    dirs=""
    for d in code mclib gui editor GameAdapters; do
        [ -d "$scan_root/$d" ] && dirs="$dirs $scan_root/$d"
    done
    [ -n "$dirs" ] || return 0
    # shellcheck disable=SC2086
    hits="$(grep -rn 'setOnGUI[[:space:]]*(' $dirs \
        --include='*.cpp' --include='*.h' 2>/dev/null \
        | strip_comments \
        | grep -Ev '(code/)?(mover|gameobj|gvehicl)\.h:' || true)"
    mission_n="$(printf '%s\n' "$hits" | grep -Ec 'mission(_update|_movers|_init)?\.cpp:' || true)"
    mech_n="$(printf '%s\n' "$hits" | grep -c 'mech_update\.cpp:' || true)"
    other="$(printf '%s\n' "$hits" | grep -Ev 'mission(_update|_movers|_init)?\.cpp:|mech_update\.cpp:' | grep -v '^$' || true)"
    [ "$mission_n" -eq 4 ] || echo "  S1a: mission.cpp setOnGUI caller count $mission_n != 4 (allowlisted: part-spawn x3 + Mission::addMover)"
    [ "$mech_n" -eq 1 ]    || echo "  S1a: mech_update.cpp setOnGUI caller count $mech_n != 1 (allowlisted: link-up)"
    if [ -n "$other" ]; then
        echo "  S1a: setOnGUI caller outside the allowlist:"
        printf '%s\n' "$other" | sed 's/^/    /'
    fi
}

# ---------------------------------------------------------------------------
# S1b: direct isOnGui member writes. $1 = dir holding a code/ tree.
# Pattern excludes the serializer LHS (data->isOnGui = ...).
# ---------------------------------------------------------------------------
s1b_member_writes() {
    scan_root="$1"
    dirs=""
    for d in code mclib gui editor GameAdapters; do
        [ -d "$scan_root/$d" ] && dirs="$dirs $scan_root/$d"
    done
    [ -n "$dirs" ] || return 0
    # shellcheck disable=SC2086
    hits="$(grep -rEn '(^|[^.>[:alnum:]_])isOnGui[[:space:]]*=[^=]' $dirs \
        --include='*.cpp' --include='*.h' 2>/dev/null \
        | strip_comments || true)"
    movercpp_n="$(printf '%s\n' "$hits" | grep -Ec 'mover(_[a-z]+)?\.cpp:' || true)"
    moverh_n="$(printf '%s\n' "$hits" | grep -c 'mover\.h:' || true)"
    gvh_n="$(printf '%s\n' "$hits" | grep -c 'gvehicl\.h:' || true)"
    other="$(printf '%s\n' "$hits" | grep -Ev 'mover(_[a-z]+)?\.cpp:|mover\.h:|gvehicl\.h:' | grep -v '^$' || true)"
    [ "$movercpp_n" -eq 2 ] || echo "  S1b: mover*.cpp direct isOnGui writes $movercpp_n != 2 (allowlisted: init default + load restore)"
    [ "$moverh_n" -eq 1 ]   || echo "  S1b: mover.h direct isOnGui writes $moverh_n != 1 (allowlisted: setOnGUI body)"
    [ "$gvh_n" -eq 1 ]      || echo "  S1b: gvehicl.h direct isOnGui writes $gvh_n != 1 (allowlisted: setOnGUI body)"
    if [ -n "$other" ]; then
        echo "  S1b: direct isOnGui write outside the allowlist:"
        printf '%s\n' "$other" | sed 's/^/    /'
    fi
}

# ---------------------------------------------------------------------------
# S2: write-pairing windows. $1 = mech_update.cpp path, $2 = mission.cpp path.
# ---------------------------------------------------------------------------
s2_pairing() {
    mech="$1"; mission="$2"

    # (a) mech_update.cpp link-up: setOnGUI(true) has addMover(this) <=10 lines above.
    ln="$(grep -n 'setOnGUI(true);' "$mech" | strip_comments | head -1 | cut -d: -f1 || true)"
    if [ -z "${ln:-}" ]; then
        echo "  S2a: mech_update.cpp link-up setOnGUI(true) not found"
    else
        start=$((ln - 10)); [ "$start" -lt 1 ] && start=1
        if ! sed -n "${start},${ln}p" "$mech" | grep -q 'missionInterface->addMover(this)'; then
            echo "  S2a: mech_update.cpp setOnGUI(true) at :$ln lacks missionInterface->addMover(this) within 10 lines above"
        fi
    fi

    # (b) mission.cpp Mission::addMover: setOnGUI(true) paired with a
    #     missionInterface->addMover(mover) <=60 lines above (same function).
    #     The part-spawn trio carries the "link up with it later" comment;
    #     the pairing-window target is the bare `mover->setOnGUI(true);`.
    ln="$(grep -n 'mover->setOnGUI(true);' "$mission" | strip_comments | grep -v 'link up with it later' | head -1 | cut -d: -f1 || true)"
    if [ -z "${ln:-}" ]; then
        echo "  S2b: mission.cpp Mission::addMover setOnGUI(true) not found"
    else
        start=$((ln - 60)); [ "$start" -lt 1 ] && start=1
        if ! sed -n "${start},${ln}p" "$mission" | grep -q 'missionInterface->addMover(mover)'; then
            echo "  S2b: mission.cpp setOnGUI(true) at :$ln lacks missionInterface->addMover(mover) within 60 lines above"
        fi
    fi

    # (c) removeMover sites: exactly 2, both in mission.cpp.
    rm_n="$(grep -c 'missionInterface->removeMover' "$mission" || true)"
    [ "$rm_n" -eq 2 ] || echo "  S2c: mission.cpp missionInterface->removeMover count $rm_n != 2 (Mission::removeMover + Mission::tradeMover)"

    # (d) addMover sites: exactly {mission.cpp x2, mech_update.cpp x1}.
    am_mission_n="$(grep -c 'missionInterface->addMover' "$mission" || true)"
    am_mech_n="$(grep -c 'missionInterface->addMover' "$mech" || true)"
    [ "$am_mission_n" -eq 2 ] || echo "  S2d: mission.cpp missionInterface->addMover count $am_mission_n != 2 (Mission::addMover + Mission::tradeMover)"
    [ "$am_mech_n" -eq 1 ]    || echo "  S2d: mech_update.cpp missionInterface->addMover count $am_mech_n != 1 (link-up)"

    # (e) freeMover sole game-side caller, preceded by removeMover <=10 lines.
    ln="$(grep -n 'ObjectManager->freeMover(' "$mission" | strip_comments | head -1 | cut -d: -f1 || true)"
    fm_n="$(grep -c 'ObjectManager->freeMover(' "$mission" || true)"
    if [ -z "${ln:-}" ] || [ "$fm_n" -ne 1 ]; then
        echo "  S2e: mission.cpp ObjectManager->freeMover caller count ${fm_n:-0} != 1"
    else
        start=$((ln - 10)); [ "$start" -lt 1 ] && start=1
        if ! sed -n "${start},${ln}p" "$mission" | grep -q 'missionInterface->removeMover'; then
            echo "  S2e: mission.cpp freeMover at :$ln lacks missionInterface->removeMover within 10 lines above"
        fi
    fi
}

# ---------------------------------------------------------------------------
# S3: read-gate pins. $1 = objmgr_pick.cpp (the pick paths and moverInRect), $2 = gametacmap.cpp, $3 = controlgui.cpp.
# Hard pins print violations; advisory drift prints WARN lines to stdout with
# an ADVISORY prefix (caller filters them out of the fail decision).
# ---------------------------------------------------------------------------
s3_read_gates() {
    objmgr="$1"; tacmap="$2"; controlgui="$3"

    # HARD pin 1: moverInRect body keeps isOnGUI() (the #50 fix — selection).
    ln="$(grep -n 'GameObjectManager::moverInRect' "$objmgr" | head -1 | cut -d: -f1 || true)"
    if [ -z "${ln:-}" ]; then
        echo "  S3: GameObjectManager::moverInRect not found in objmgr_pick.cpp"
    else
        if ! sed -n "${ln},$((ln + 40))p" "$objmgr" | grep -q 'isOnGUI()'; then
            echo "  S3: moverInRect body lost its isOnGUI() gate (#50 selection class)"
        fi
    fi

    # HARD pin 2: tacmap home-commander blip branch keeps isOnGUI().
    if ! grep -q 'mover->isOnGUI()' "$tacmap"; then
        echo "  S3: gametacmap.cpp lost its mover->isOnGUI() blip gate"
    fi

    # HARD pin 3: ControlGui::initMechs keeps isOnGUI() (load reconciliation).
    ln="$(grep -n 'ControlGui::initMechs' "$controlgui" | head -1 | cut -d: -f1 || true)"
    if [ -z "${ln:-}" ]; then
        echo "  S3: ControlGui::initMechs not found in controlgui.cpp"
    else
        if ! sed -n "${ln},$((ln + 30))p" "$controlgui" | grep -q 'isOnGUI()'; then
            echo "  S3: ControlGui::initMechs body lost its isOnGUI() gate (roster rebuild)"
        fi
    fi

    # ADVISORY: objmgr pick paths (4) + missiongui select-all trio (3).
    pick_n="$(grep -c 'obj->isOnGUI()' "$objmgr" || true)"
    [ "$pick_n" -ge 4 ] || echo "  ADVISORY S3: objmgr_pick.cpp pick-path obj->isOnGUI() count $pick_n < 4 (drift runtime-caught by I2/I5)"
}

s3_advisory_selectall() {
    # "$@" = the missiongui.cpp pieces (the select-all trio lives in missiongui_orders.cpp / missiongui_update.cpp).
    sel_n="$(cat "$@" | grep -c 'pMover->isOnGUI()' || true)"
    [ "$sel_n" -ge 3 ] || echo "  ADVISORY S3: missiongui.cpp select-all pMover->isOnGUI() count $sel_n < 3 (drift runtime-caught by I2/I5)"
}

# ---------------------------------------------------------------------------
# S4: #42b guard shape. "$@" = the missiongui.cpp pieces (the hover guard is in updateTarget, missiongui_update.cpp). SOLE DEFENSE — never demote.
# ---------------------------------------------------------------------------
s4_guard_shape() {
    if ! cat "$@" | grep -q 'target->isDisabled() && !target->getAppearance()'; then
        echo "  S4: narrow hover guard 'target->isDisabled() && !target->getAppearance()' MISSING (historical #42b revert — this rule is the sole defense)"
    fi
    bare="$(cat "$@" | grep -En 'if[[:space:]]*\([[:space:]]*target[[:space:]]*&&[[:space:]]*target->isDisabled\(\)[[:space:]]*\)' | strip_comments || true)"
    if [ -n "$bare" ]; then
        echo "  S4: bare widened hover guard found (historical #42b shape):"
        printf '%s\n' "$bare" | sed 's/^/    /'
    fi
}

# ---------------------------------------------------------------------------
# S5: link-up gate shape. $1 = mech_update.cpp.
# ---------------------------------------------------------------------------
s5_linkup_gate() {
    mech="$1"
    ln="$(grep -n 'if (!isOnGUI())' "$mech" | strip_comments | head -1 | cut -d: -f1 || true)"
    if [ -z "${ln:-}" ]; then
        echo "  S5: link-up gate 'if (!isOnGUI())' MISSING from mech_update.cpp (#50 latch fix reverted?)"
        return 0
    fi
    start=$((ln - 10)); [ "$start" -lt 1 ] && start=1
    if sed -n "${start},$((ln + 10))p" "$mech" | strip_window_comments | grep -Eq 'status[[:space:]]*==[[:space:]]*OBJECT_STATUS_SHUTDOWN[)[:space:]]*&&'; then
        echo "  S5: forbidden 'status == OBJECT_STATUS_SHUTDOWN &&' inside the link-up gate window (pre-#50 latch shape)"
    fi
}

# comment stripper for raw (un-numbered) window text.
strip_window_comments() {
    grep -Ev '^[[:space:]]*(//|\*)' || true
}

# ---------------------------------------------------------------------------
# S6: audit region exists and is position-free (off-map staging carve-out).
# $1 = mission.cpp.
# ---------------------------------------------------------------------------
s6_audit_region() {
    mission="$1"
    b="$(grep -n 'MOVER-COHERENCE-AUDIT-BEGIN' "$mission" | head -1 | cut -d: -f1 || true)"
    e="$(grep -n 'MOVER-COHERENCE-AUDIT-END' "$mission" | head -1 | cut -d: -f1 || true)"
    if [ -z "${b:-}" ] || [ -z "${e:-}" ] || [ "$b" -ge "$e" ]; then
        echo "  S6: MOVER-COHERENCE-AUDIT-BEGIN/END sentinels missing from mission.cpp (runtime audit gone?)"
        return 0
    fi
    region="$(sed -n "${b},${e}p" "$mission")"
    printf '%s\n' "$region" | grep -q 'auditMoverCoherence' \
        || echo "  S6: audit region lacks auditMoverCoherence"
    printf '%s\n' "$region" | grep -q 'MOVER_COHERENCE' \
        || echo "  S6: audit region lacks the [MOVER_COHERENCE] marker emit"
    pos="$(printf '%s\n' "$region" | strip_window_comments \
        | grep -En 'getPosition|IsGameSelectTerrainPosition|land->|Terrain::|->position|GameMap|inMapBounds' || true)"
    if [ -n "$pos" ]; then
        echo "  S6: POSITIONAL TERM inside the audit region — off-map staging is a"
        echo "      LEGITIMATE mover state (INVIS-VEHICLES-FIX-1: mc2_10 convoy is"
        echo "      authored off-map with onGUI=1, brains alive). Position outside"
        echo "      map bounds is NOT a registry violation; remove the term:"
        printf '%s\n' "$pos" | sed 's/^/    /'
    fi
    grep -q 'MC2_MOVER_COHERENCE_AUDIT' "$mission" \
        || echo "  S6: MC2_MOVER_COHERENCE_AUDIT latch missing from mission.cpp"
}

# ---------------------------------------------------------------------------
run_check() {
    fail=0
    out=""

    v="$(s1a_setongui_sites "$ROOT")"
    [ -n "$v" ] && { echo "[mover-coherence] S1a FAIL:"; printf '%s\n' "$v"; fail=1; }
    v="$(s1b_member_writes "$ROOT")"
    [ -n "$v" ] && { echo "[mover-coherence] S1b FAIL:"; printf '%s\n' "$v"; fail=1; }
    mc_all="$(mktemp)"
    mission_all_to "$mc_all"
    v="$(s2_pairing "$ROOT/code/mech_update.cpp" "$mc_all")"
    [ -n "$v" ] && { echo "[mover-coherence] S2 FAIL:"; printf '%s\n' "$v"; fail=1; }

    v="$(s3_read_gates "$ROOT/code/objmgr_pick.cpp" "$ROOT/code/gametacmap.cpp" "$ROOT/code/controlgui.cpp"; s3_advisory_selectall "$ROOT"/code/missiongui*.cpp)"
    hard="$(printf '%s\n' "$v" | grep -v 'ADVISORY' | grep -v '^$' || true)"
    adv="$(printf '%s\n' "$v" | grep 'ADVISORY' || true)"
    [ -n "$hard" ] && { echo "[mover-coherence] S3 FAIL:"; printf '%s\n' "$hard"; fail=1; }
    [ -n "$adv" ]  && { echo "[mover-coherence] S3 WARN (advisory, exit unchanged):"; printf '%s\n' "$adv"; }

    v="$(s4_guard_shape "$ROOT"/code/missiongui*.cpp)"
    [ -n "$v" ] && { echo "[mover-coherence] S4 FAIL:"; printf '%s\n' "$v"; fail=1; }
    v="$(s5_linkup_gate "$ROOT/code/mech_update.cpp")"
    [ -n "$v" ] && { echo "[mover-coherence] S5 FAIL:"; printf '%s\n' "$v"; fail=1; }
    v="$(s6_audit_region "$mc_all")"
    [ -n "$v" ] && { echo "[mover-coherence] S6 FAIL:"; printf '%s\n' "$v"; fail=1; }
    rm -f "$mc_all"

    if [ "$fail" -ne 0 ]; then
        echo "FAIL: mover-registry coherence contract violated (see above)"
        return 1
    fi
    echo "OK: mover-registry-coherence (S1 allowlists, S2 pairing, S3 pins, S4 #42b shape, S5 link-up shape, S6 staging carve-out)"
    return 0
}

# ---------------------------------------------------------------------------
self_test() {
    if ! run_check > /dev/null 2>&1; then
        echo "mover-coherence --self-test: FAIL (not green on current tree)"
        return 1
    fi
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/code"

    # S1a synthetic: rogue setOnGUI caller in a new game TU.
    cat > "$tmp/code/selftest_rogue.cpp" <<'EOF'
void f(Mover* m) { m->setOnGUI(true); }
EOF
    v="$(s1a_setongui_sites "$tmp")"
    if [ -z "$v" ]; then
        echo "mover-coherence --self-test: FAIL (S1a rogue setOnGUI caller NOT detected)"
        return 1
    fi

    # S1b synthetic: rogue direct member write.
    cat > "$tmp/code/selftest_write.cpp" <<'EOF'
void g() { isOnGui = true; }
EOF
    v="$(s1b_member_writes "$tmp")"
    if ! printf '%s\n' "$v" | grep -q 'selftest_write.cpp'; then
        echo "mover-coherence --self-test: FAIL (S1b rogue isOnGui write NOT detected)"
        return 1
    fi

    # S2a revert-simulation: delete the addMover pairing above the mech_update.cpp
    # link-up setOnGUI(true) — pairing window must go red.
    sed 's/missionInterface->addMover(this);//' "$ROOT/code/mech_update.cpp" > "$tmp/mech_nopair.cpp"
    mission_all_to "$tmp/mission_all.cpp"
    v="$(s2_pairing "$tmp/mech_nopair.cpp" "$tmp/mission_all.cpp")"
    if ! printf '%s\n' "$v" | grep -q 'S2a\|S2d'; then
        echo "mover-coherence --self-test: FAIL (S2 unpaired setOnGUI NOT detected)"
        return 1
    fi

    # S4 revert-simulation: widen the hover guard back to the historical #42b
    # shape (bare isDisabled()).
    cat "$ROOT"/code/missiongui*.cpp | sed 's/target->isDisabled() && !target->getAppearance()/target->isDisabled()/' \
        > "$tmp/missiongui_wide.cpp"
    v="$(s4_guard_shape "$tmp/missiongui_wide.cpp")"
    if [ -z "$v" ]; then
        echo "mover-coherence --self-test: FAIL (S4 widened #42b guard NOT detected)"
        return 1
    fi

    # S5 revert-simulation: re-key the link-up gate on SHUTDOWN status
    # (the pre-#50 latch shape).
    sed 's/if (!isOnGUI())/if ((status == OBJECT_STATUS_SHUTDOWN) \&\& !isOnGUI())/' \
        "$ROOT/code/mech_update.cpp" > "$tmp/mech_latch.cpp"
    v="$(s5_linkup_gate "$tmp/mech_latch.cpp")"
    if [ -z "$v" ]; then
        echo "mover-coherence --self-test: FAIL (S5 SHUTDOWN-keyed link-up gate NOT detected)"
        return 1
    fi

    # S6 carve-out leg: inject a positional read into the audit region — the
    # off-map-staging pin must go red (position is NOT an audited dimension).
    awk '{ print; if ($0 ~ /MOVER-COHERENCE-AUDIT-BEGIN/) print "\tstatic void poke(MoverPtr m) { (void)m->getPosition(); }" }' \
        "$tmp/mission_all.cpp" > "$tmp/mission_pos.cpp"
    v="$(s6_audit_region "$tmp/mission_pos.cpp")"
    if ! printf '%s\n' "$v" | grep -q 'POSITIONAL TERM'; then
        echo "mover-coherence --self-test: FAIL (S6 positional term in audit region NOT detected)"
        return 1
    fi

    # S6 sentinel leg: audit region deleted entirely.
    grep -v 'MOVER-COHERENCE-AUDIT-BEGIN' "$tmp/mission_all.cpp" > "$tmp/mission_nosentinel.cpp"
    v="$(s6_audit_region "$tmp/mission_nosentinel.cpp")"
    if [ -z "$v" ]; then
        echo "mover-coherence --self-test: FAIL (S6 missing sentinel NOT detected)"
        return 1
    fi

    echo "mover-coherence --self-test: PASS (green on tree; S1a/S1b/S2/S4/S5/S6 red on synthetic violations incl. S4/S5 revert-simulations + S6 staging carve-out)"
    return 0
}

case "${1:-}" in
    --self-test) self_test ;;
    "")          run_check ;;
    *) echo "usage: $0 [--self-test]" >&2; exit 2 ;;
esac
