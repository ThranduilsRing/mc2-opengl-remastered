#!/usr/bin/env python3
"""merge_gate_editor - the merge gate's `editor` stage, kept in its own module.

Proves the Mission Editor built by this tree still launches and works against the gate's install:
  0. run build64-ninja/out/editor/editor_document_tests.exe (offline document model tests);
  1. clone the install's runtime into this worktree's scratch (run_editor_smoke.clone_install_for_stock_scratch;
     the install itself is never written, so the game stages' deploy manifest is untouched);
  2. drop the freshly built editor (build64-ninja/out/editor: Mission Editor.exe, splash bmps, data/defs) into the clone;
  3. run scripts/run_editor_smoke.py on the clone (minimized, non-activating, host-leased like every game run)
     with one stock FIT/PAK byte roundtrip (load mc2_01, save, compare, exit);
  4. delete the clone.

Usage: py -3 scripts/merge_gate_editor.py --install DIR [--build-dir build64-ninja] [--out DIR] [--case NAME ...]
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)
import run_editor_smoke as smoke  # noqa: E402

EXE = "Mission Editor.exe"
CASES = smoke.GATE_CASES  # owned by run_editor_smoke.py; the editor-tools lane appends workflow checks there


def stage_editor(build: Path, clone: Path) -> None:
    for name in (EXE, "esplash.bmp", "tacsplash.bmp"):
        shutil.copy2(build / name, clone / name)
    shutil.copytree(build / "data" / "defs", clone / "data" / "defs", dirs_exist_ok=True)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--install", type=Path, default=Path("A:/Games/mc2-opengl/mc2-win64-v0.4"))  # drive-path: ok PC gate default; the laptop gate passes --install
    ap.add_argument("--build-dir", type=Path, default=REPO / "build64-ninja")
    ap.add_argument("--out", type=Path, default=REPO / "scratch" / "editor-smoke")
    ap.add_argument("--case", action="append", help=f"editor smoke case (default: {', '.join(CASES)})")
    args = ap.parse_args(argv)
    build = args.build_dir / "out" / "editor"
    if not (build / EXE).is_file():
        print(f"[EDITOR_GATE] FAIL: {build / EXE} missing (EditRel is in the default build; build the tree first)")
        return 2
    if not (args.install / "mc2.exe").is_file():
        print(f"[EDITOR_GATE] FAIL: install {args.install} has no mc2.exe")
        return 2
    # Offline first: the document model (FitReader, MissionDocument, packets, catalog) with no engine or window.
    tests = build / "editor_document_tests.exe"
    rc = subprocess.run([str(tests)], cwd=str(REPO), **console_child.child_kwargs()).returncode if tests.is_file() else 2
    if rc != 0:
        print(f"[EDITOR_GATE] FAIL: editor_document_tests rc={rc} ({tests})")
        return rc
    allowances = smoke.load_stock_allowances(argparse.Namespace(stock_allowlist=None, baseline=smoke.BASELINE))
    clone = smoke.clone_install_for_stock_scratch(args.install, allowances)
    try:
        stage_editor(build, clone)
        command = [sys.executable, str(REPO / "scripts" / "run_editor_smoke.py"), "--exe", str(clone / EXE),
                   "--prepare-stock-scratch", "--keep-logs", "--out", str(args.out)]
        for case in args.case or CASES:
            command += ["--case", case]
        rc = subprocess.run(command, cwd=str(REPO), **console_child.child_kwargs()).returncode
    finally:
        shutil.rmtree(clone, ignore_errors=True)
    print(f"[EDITOR_GATE] {'PASS' if rc == 0 else 'FAIL'} rc={rc}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
