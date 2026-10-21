#!/usr/bin/env python3
"""Compile-level check for ABL by-reference real out-params (spec 'v', gettargetrelativeposition).

Takes a kept mod_smoke stage (py -3 tools/mod_smoke.py --mod MechCommanderOmnitech --keep-stages), injects one call per variant into
the staged a_0_1_guard.abl, runs the staged exe through run_smoke and reads the SYNTAX ERROR lines. The bare-real-variable call must
compile; an expression, literal, array element and wrong-typed variable must each be a compile error (no write can happen then).
"""
import argparse, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASES = [('ok bare real var', 'gettargetrelativeposition(-1, tr1, tr1);', False),
         ('expression x + 1.0', 'gettargetrelativeposition(-1, tr1 + 1.0, tr1);', True),
         ('literal', 'gettargetrelativeposition(-1, 5.0, tr1);', True),
         ('array element', 'gettargetrelativeposition(-1, tra[1], tr1);', True),
         ('wrong-typed var', 'gettargetrelativeposition(-1, tri, tr1);', True)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stage', help='kept MechCommanderOmnitech stage dir')
    stage = Path(ap.parse_args().stage)
    f = stage / 'data/missions/warriors/a_0_1_guard.abl'
    orig = f.read_bytes()
    text = orig.decode('latin-1')
    nl = '\r\n' if '\r\n' in text else '\n'
    decl = '		static integer			scanRange;'
    marker = '		setWillHelp(False);'
    assert decl in text and marker in text
    bad = 0
    try:
        for name, call, must_fail in CASES:
            t = text.replace(decl, decl + nl + '		static real tr1;' + nl + '		static real[3] tra;' + nl + '		static integer tri;', 1)
            t = t.replace(marker, '		' + call + nl + marker, 1)
            f.write_bytes(t.encode('latin-1'))
            subprocess.run([sys.executable, str(ROOT / 'scripts/run_smoke.py'), '--exe', str(stage / 'mc2.exe'), '--mission', 'a_0_1',
                            '--allow-any-mission', '--duration', '12', '--keep-logs'], cwd=str(stage), capture_output=True)
            art = max((ROOT / 'tests/smoke/artifacts').iterdir(), key=lambda p: p.stat().st_mtime)
            log = ''.join(p.read_text(errors='replace') for p in art.glob('*.log'))
            failed = 'SYNTAX ERROR data/missions/warriors/a_0_1_guard.abl' in log
            ok = failed == must_fail
            bad += not ok
            print(f"{'PASS' if ok else 'FAIL'} {name}: compile {'rejected' if failed else 'accepted'}")
    finally:
        f.write_bytes(orig)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
