#!/bin/sh
# scripts/check-tgl-load-callers.sh
#
# META-FIX design B PART 5 (CLASS B — silent asset-load failure): static
# guard for the fail-loud asset-load policy. Contract:
# .claude/META-FIX-DESIGNS-2026-07-16.json CLASS B. Runtime siblings:
# msl.cpp PART-0 loader chokepoint, mclib/tgl_load_guard (PART 1),
# apprtype.cpp getAppearance backstop (PART 2), icon chokepoints (PART 3),
# MC2_APPEAR_AUDIT (PART 4).
#
# Rules (any hit = exit 1):
#   P1 loader-call confinement — raw ->LoadTGMultiShapeFromASE( /
#      ->LoadFromFile( / ->LoadBinaryCopy( call sites outside the loader
#      surface {mclib/msl.cpp, mclib/msl.h, mclib/tgl_load_guard.cpp} must
#      route through tglLoadShapeChecked/tglVerifyShapeLoaded (same
#      statement or the 3 lines below) OR carry an inline tag:
#          // TGL-LOAD-EXEMPT(<reason>)   optional slot, loader-side logging
#          // TGL-LOAD-VERIFIED           reviewed required slot
#      (tag convention new here; grep-exclusion precedent
#      scripts/check-asset-scale-callers.sh). tools/ and tests/ not scanned.
#   P2 presence pins — the PART 0-4 mechanisms exist: parse_fail chokepoint
#      (msl.cpp), required_missing + MC2_ASSET_FAILLOUD (tgl_load_guard),
#      appr_type_fail backstop (apprtype.cpp), cellForIndexChecked header +
#      both UV callers, iconTileChecked x3 blits (mechicon.cpp),
#      APPEAR-AUDIT sentinels + MC2_APPEAR_AUDIT latch (mission.cpp).
#   P3 OFF-MAP STAGING CARVE-OUT (INVIS-VEHICLES-FIX-1) — the APPEAR-AUDIT
#      sentinel region must NOT contain positional OR visibility terms
#      (getPosition, IsGameSelectTerrainPosition, land->, Terrain::,
#      ->position, canBeSeen, inView): off-map staged movers (mc2_10 convoy
#      — authored off-map, onGUI=1, valid appearances) are a LEGITIMATE
#      mover state; the scan may key only on appearance/base-shape state.
#
# Usage:
#   sh scripts/check-tgl-load-callers.sh              # check (CI)
#   sh scripts/check-tgl-load-callers.sh --self-test
#
# Exit: 0 pass / 1 violation / 2 harness error.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

strip_comments() {
    grep -Ev '^[^:]*:[0-9]+:[[:space:]]*(//|\*)' || true
}

# ---------------------------------------------------------------------------
# P1: loader-call confinement. $1 = dir holding the source tree.
# ---------------------------------------------------------------------------
p1_loader_confinement() {
    scan_root="$1"
    dirs=""
    for d in code mclib gui editor GameAdapters GameOS Viewer EditorBridge; do
        [ -d "$scan_root/$d" ] && dirs="$dirs $scan_root/$d"
    done
    [ -n "$dirs" ] || return 0
    # shellcheck disable=SC2086
    hits="$(grep -rn -- '->LoadTGMultiShapeFromASE[[:space:]]*(\|->LoadFromFile[[:space:]]*(\|->LoadBinaryCopy[[:space:]]*(' $dirs \
        --include='*.cpp' --include='*.h' 2>/dev/null \
        | strip_comments \
        | grep -v 'mclib/msl\(_[a-z]*\)\?\.cpp:\|mclib/msl\.h:\|mclib/tgl_load_guard\.cpp:' \
        | grep -v 'TGL-LOAD-EXEMPT\|TGL-LOAD-VERIFIED' || true)"
    # A raw call is fine when the checked verify wraps it: accept sites whose
    # own line or the 3 lines below carry tglVerifyShapeLoaded (the import
    # branches capture rc then verify).
    printf '%s\n' "$hits" | grep -v '^$' | while IFS= read -r hit; do
        f="${hit%%:*}"
        rest="${hit#*:}"
        ln="${rest%%:*}"
        end=$((ln + 3))
        if ! sed -n "${ln},${end}p" "$f" 2>/dev/null | grep -q 'tglVerifyShapeLoaded\|tglLoadShapeChecked'; then
            echo "  P1: raw loader call bypasses tgl_load_guard (tag TGL-LOAD-EXEMPT(<reason>) if optional):"
            echo "    $hit"
        fi
    done
}

# ---------------------------------------------------------------------------
# P2: presence pins. Args: msl (msl_load.cpp, where the PART-0 chokepoint lives) tgl_guard apprtype icon_hdr logi mechlist mechicon mission
# ---------------------------------------------------------------------------
p2_presence() {
    msl="$1"; guard="$2"; apprtype="$3"; icon_hdr="$4"; logi="$5"; mechlist="$6"; mechicon="$7"; mission="$8"
    grep -q '"parse_fail"' "$msl" \
        || echo "  P2: msl.cpp PART-0 parse_fail chokepoint missing"
    grep -q 'event=required_missing' "$guard" \
        || echo "  P2: tgl_load_guard required_missing emit missing"
    grep -q 'MC2_ASSET_FAILLOUD' "$guard" \
        || echo "  P2: tgl_load_guard MC2_ASSET_FAILLOUD killswitch missing"
    grep -q 'event=appr_type_fail' "$apprtype" \
        || echo "  P2: apprtype.cpp appr_type_fail backstop missing"
    grep -q 'cellForIndexChecked' "$icon_hdr" \
        || echo "  P2: icon_atlas_cell.h cellForIndexChecked missing"
    grep -q 'cellForIndexChecked' "$logi" \
        || echo "  P2: logisticsmechicon.cpp not using cellForIndexChecked"
    grep -q 'cellForIndexChecked' "$mechlist" \
        || echo "  P2: mechlistbox.cpp not using cellForIndexChecked"
    n="$(grep -c 'iconTileChecked("' "$mechicon" || true)"
    [ "$n" -ge 3 ] || echo "  P2: mechicon.cpp iconTileChecked call sites $n < 3 (three blits must stay guarded)"
    grep -q 'APPEAR-AUDIT-BEGIN' "$mission" && grep -q 'APPEAR-AUDIT-END' "$mission" \
        || echo "  P2: mission.cpp APPEAR-AUDIT sentinels missing"
    grep -q 'MC2_APPEAR_AUDIT' "$mission" \
        || echo "  P2: mission.cpp MC2_APPEAR_AUDIT latch missing"
}

# ---------------------------------------------------------------------------
# P3: APPEAR-AUDIT region position/visibility-free. $1 = mission.cpp.
# ---------------------------------------------------------------------------
strip_window_comments() {
    grep -Ev '^[[:space:]]*(//|\*)' || true
}

p3_appear_region() {
    mission="$1"
    b="$(grep -n 'APPEAR-AUDIT-BEGIN' "$mission" | head -1 | cut -d: -f1 || true)"
    e="$(grep -n 'APPEAR-AUDIT-END' "$mission" | head -1 | cut -d: -f1 || true)"
    if [ -z "${b:-}" ] || [ -z "${e:-}" ] || [ "$b" -ge "$e" ]; then
        echo "  P3: APPEAR-AUDIT-BEGIN/END sentinels missing (scan gone?)"
        return 0
    fi
    pos="$(sed -n "${b},${e}p" "$mission" | strip_window_comments \
        | grep -En 'getPosition|IsGameSelectTerrainPosition|land->|Terrain::|->position|canBeSeen|inView' || true)"
    if [ -n "$pos" ]; then
        echo "  P3: POSITIONAL/VISIBILITY TERM inside the APPEAR-AUDIT region —"
        echo "      off-map staging is a LEGITIMATE mover state (INVIS-VEHICLES-"
        echo "      FIX-1: mc2_10 convoy is authored off-map with onGUI=1 and"
        echo "      valid appearances). The scan keys on appearance state only:"
        printf '%s\n' "$pos" | sed 's/^/    /'
    fi
}

# ---------------------------------------------------------------------------
run_check() {
    fail=0

    v="$(p1_loader_confinement "$ROOT")"
    [ -n "$v" ] && { echo "[tgl-load-callers] P1 FAIL:"; printf '%s\n' "$v"; fail=1; }

    v="$(p2_presence "$ROOT/mclib/msl_load.cpp" "$ROOT/mclib/tgl_load_guard.cpp" \
        "$ROOT/mclib/apprtype.cpp" "$ROOT/code/icon_atlas_cell.h" \
        "$ROOT/code/logisticsmechicon.cpp" "$ROOT/code/mechlistbox.cpp" \
        "$ROOT/code/mechicon.cpp" "$ROOT/code/mission.cpp")"
    [ -n "$v" ] && { echo "[tgl-load-callers] P2 FAIL:"; printf '%s\n' "$v"; fail=1; }

    v="$(p3_appear_region "$ROOT/code/mission.cpp")"
    [ -n "$v" ] && { echo "[tgl-load-callers] P3 FAIL:"; printf '%s\n' "$v"; fail=1; }

    if [ "$fail" -ne 0 ]; then
        echo "FAIL: tgl-load-callers contract violated (see above)"
        return 1
    fi
    echo "OK: tgl-load-callers (P1 confinement, P2 presence pins, P3 appear-audit staging carve-out)"
    return 0
}

# ---------------------------------------------------------------------------
self_test() {
    if ! run_check > /dev/null 2>&1; then
        echo "tgl-load-callers --self-test: FAIL (not green on current tree)"
        return 1
    fi
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/code"

    # P1 synthetic: unchecked raw loader call in a new game TU.
    cat > "$tmp/code/selftest_loader.cpp" <<'EOF'
void f(TG_TypeMultiShape* s) { s->LoadTGMultiShapeFromASE("x.ase"); }
EOF
    v="$(p1_loader_confinement "$tmp")"
    if [ -z "$v" ]; then
        echo "tgl-load-callers --self-test: FAIL (P1 unchecked raw loader call NOT detected)"
        return 1
    fi

    # P1 exemption honored.
    cat > "$tmp/code/selftest_exempt.cpp" <<'EOF'
void g(TG_TypeMultiShape* s) { s->LoadTGMultiShapeFromASE("y.ase"); } // TGL-LOAD-EXEMPT(selftest)
EOF
    rm "$tmp/code/selftest_loader.cpp"
    v="$(p1_loader_confinement "$tmp")"
    if [ -n "$v" ]; then
        echo "tgl-load-callers --self-test: FAIL (P1 TGL-LOAD-EXEMPT tag not honored)"
        return 1
    fi

    # P1 checked-wrap honored (verify within 3 lines below).
    cat > "$tmp/code/selftest_checked.cpp" <<'EOF'
void h(TG_TypeMultiShape* s) {
	long rc = s->LoadFromFile("z");
	tglVerifyShapeLoaded(s, rc, "selftest", "z", 0);
}
EOF
    v="$(p1_loader_confinement "$tmp")"
    if [ -n "$v" ]; then
        echo "tgl-load-callers --self-test: FAIL (P1 verified import wrap not honored)"
        return 1
    fi

    # P2 revert-simulation: appr_type_fail backstop deleted.
    grep -v 'appr_type_fail' "$ROOT/mclib/apprtype.cpp" > "$tmp/apprtype_nobackstop.cpp"
    v="$(p2_presence "$ROOT/mclib/msl_load.cpp" "$ROOT/mclib/tgl_load_guard.cpp" \
        "$tmp/apprtype_nobackstop.cpp" "$ROOT/code/icon_atlas_cell.h" \
        "$ROOT/code/logisticsmechicon.cpp" "$ROOT/code/mechlistbox.cpp" \
        "$ROOT/code/mechicon.cpp" "$ROOT/code/mission.cpp")"
    if ! printf '%s\n' "$v" | grep -q 'appr_type_fail'; then
        echo "tgl-load-callers --self-test: FAIL (P2 deleted backstop NOT detected)"
        return 1
    fi

    # P3 carve-out leg: inject a positional read into the APPEAR-AUDIT region.
    awk '{ print; if ($0 ~ /APPEAR-AUDIT-BEGIN/) print "\tstatic void poke(GameObjectPtr o) { (void)o->getPosition(); }" }' \
        "$ROOT/code/mission.cpp" > "$tmp/mission_pos.cpp"
    v="$(p3_appear_region "$tmp/mission_pos.cpp")"
    if ! printf '%s\n' "$v" | grep -q 'POSITIONAL/VISIBILITY TERM'; then
        echo "tgl-load-callers --self-test: FAIL (P3 positional term in appear-audit region NOT detected)"
        return 1
    fi

    # P3 visibility leg: canBeSeen() gate would also skip staged movers.
    awk '{ print; if ($0 ~ /APPEAR-AUDIT-BEGIN/) print "\tstatic bool poke2(GameObjectPtr o) { return o->canBeSeen(); }" }' \
        "$ROOT/code/mission.cpp" > "$tmp/mission_vis.cpp"
    v="$(p3_appear_region "$tmp/mission_vis.cpp")"
    if ! printf '%s\n' "$v" | grep -q 'POSITIONAL/VISIBILITY TERM'; then
        echo "tgl-load-callers --self-test: FAIL (P3 visibility term in appear-audit region NOT detected)"
        return 1
    fi

    echo "tgl-load-callers --self-test: PASS (green on tree; P1 raw/exempt/checked legs, P2 revert-simulation, P3 staging carve-out position+visibility legs all behave)"
    return 0
}

case "${1:-}" in
    --self-test) self_test ;;
    "")          run_check ;;
    *) echo "usage: $0 [--self-test]" >&2; exit 2 ;;
esac
