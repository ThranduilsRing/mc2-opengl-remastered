"""A documented MC2_* gate must be read from the env/gate table, not hardcoded off.

`static const bool s_x = false;` next to a gate's comment or log tag looks like a switch but no env var can ever turn it
on (MC2_BRAIN_FREEZE_TRACE, MC2_AIMED_FIRE_FIXTURE and MC2_LINKUP_TRACE all shipped as dead switches). This test finds
every such hardcoded-false constant whose +-WINDOW source lines name a registered or documented gate (the full name, or
its name without the MC2_ prefix such as a "[BRAIN_FREEZE_TRACE]" log tag) and compares it with KNOWN_OFFENDERS.

Ratchet: a NEW offender fails the test; a fixed one must be deleted from KNOWN_OFFENDERS (the list may only shrink).
Fix an offender with `static const bool s_x = mc2::gate::get<"MC2_NAME">();`, never by editing the allowlist upwards.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WINDOW = 30
HARDCODED = re.compile(r"static\s+(?:const|constexpr)\s+bool\s+(\w+)\s*=\s*false\s*(?:\|\|\s*false\s*)?;")
NAME = re.compile(r"MC2_[A-Z0-9_]{4,}")

# (file relative to code/, variable) -> gate named nearby. Only ever delete lines.
KNOWN_OFFENDERS = {
    # known dead gates (documented env var, no read)
    ("mech_update.cpp", "s_linkupTrace"): "MC2_LINKUP_TRACE",
    # heuristic hits (a gate name is only nearby); review each, delete the line once it is a gate read or judged intentional
    ("controlgui_objectives.cpp", "s_on"): "MC2_IMGUI",
    ("controlgui_update.cpp", "s_cgSplitOn"): "MC2_BOOT_INFO",
    ("gamecam.cpp", "s_forcePos"): "MC2_HZB_VIEW_FILE",
    ("gamecam.cpp", "s_hzbForceHorizon"): "MC2_HZB_VIEW_FILE",
    ("gamecam.cpp", "s_vanishProbe"): "MC2_VIEW_UNIFORMS",
    ("infowindow.cpp", "s_on"): "MC2_IMGUI",
    ("infowindow.cpp", "s_scrollPin"): "MC2_BOOT_INFO",
    ("loadscreen.cpp", "s_legacyBanner"): "MC2_LEGACY_LOAD_BANNER",
    ("logisticsdata_init.cpp", "s_smartLoadTrace"): "MC2_SMART_LOAD",
    ("missiongui_orders.cpp", "s_tgtGuard"): "MC2_DIAG_TAGS",
    ("missiongui_update.cpp", "s_cursorTargetTrace"): "MC2_DIAG_TAGS",
    ("mover_combat.cpp", "s_tgtGuard"): "MC2_DIAG_TAGS",
    ("objmgr_pick.cpp", "s_cursorTargetTrace"): "MC2_DIAG_TAGS",
    ("objmgr_update.cpp", "s_gpuCullLifecycle"): "MC2_GOM_RECON",
    ("pausewindow.cpp", "s_on"): "MC2_IMGUI",
}


def _gate_names():
    names = set()
    for p in (ROOT / "Gates").glob("gate_rows_*.inc"):
        names.update(re.findall(r'"(MC2_[A-Z0-9_]{4,})"', p.read_text(encoding="utf-8", errors="replace")))
    doc = ROOT / "docs" / "tier1_env_vars.md"
    names.update(re.findall(r"`(MC2_[A-Z0-9_]{4,})", doc.read_text(encoding="utf-8", errors="replace")))
    return names


def _offenders(files=None, names=None):
    names = _gate_names() if names is None else names
    bare = {n[4:]: n for n in names}
    found = {}
    for path in (files if files is not None else sorted((ROOT / "code").glob("*.cpp"))):
        text = Path(path).read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        for m in HARDCODED.finditer(text):
            line = text.count("\n", 0, m.start())
            win = "\n".join(lines[max(0, line - WINDOW):line + WINDOW + 1])
            hit = sorted({t for t in NAME.findall(win) if t in names} |
                         {full for b, full in bare.items() if re.search(r"(?<![A-Za-z0-9_])" + b + r"(?![A-Za-z0-9_])", win)})
            if hit:
                found[(Path(path).name, m.group(1))] = hit[0]
    return found


def test_scanner_flags_a_hardcoded_gate(tmp_path):
    f = tmp_path / "x.cpp"
    f.write_text("// MC2_FAKE_TRACE=1 logs things\nstatic const bool s_t =\n\tfalse;\n"
                 "static const bool s_ok = mc2::gate::get<\"MC2_FAKE_TRACE\">();\n"
                 + "\n" * (WINDOW + 5) + "static const bool s_unrelated = false; // no gate named\n", encoding="utf-8")
    assert _offenders([f], {"MC2_FAKE_TRACE"}) == {("x.cpp", "s_t"): "MC2_FAKE_TRACE"}


def test_no_new_hardcoded_false_gates():
    found = _offenders()
    new = {k: v for k, v in found.items() if k not in KNOWN_OFFENDERS}
    assert not new, "hardcoded-false constant next to a documented gate (read the gate instead): " + repr(new)
    fixed = sorted(set(KNOWN_OFFENDERS) - set(found))
    assert not fixed, "fixed or removed; delete from KNOWN_OFFENDERS (ratchet only shrinks): " + repr(fixed)
