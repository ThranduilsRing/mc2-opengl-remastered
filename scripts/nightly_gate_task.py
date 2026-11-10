#!/usr/bin/env python3
"""Render scripts/nightly_gate.task.xml for registration: the template's @PY@ becomes the full path of the Python launcher.

The task's PATH has no `py` (the 04:30 run failed with 0x80070002), so the launcher is resolved here with shutil.which at
registration time and never hardcoded in the repo. This script writes the rendered XML and PRINTS the schtasks command; it never
registers or changes the scheduled task (the orchestrator runs the printed command).

    py -3 scripts/nightly_gate_task.py [--out <rendered.xml>] [--py <launcher>]
"""
import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(HERE, "nightly_gate.task.xml")
TASK_NAME = "\\mc2-nightly-gate"


def resolve_launcher(which=shutil.which, override=None):
    """Full path of the Python launcher: `override`, else py.exe, else python.exe; SystemExit when none is found."""
    for cand in ([override] if override else ["py", "python"]):
        full = cand if override else which(cand)
        if full and os.path.isabs(full):
            return os.path.normpath(full)
    sys.exit("nightly_gate_task: no py/python launcher found on PATH (pass --py <full path>)")


def render(template_text, launcher):
    """Template text with @PY@ replaced by the launcher path; `-3` stays in <Arguments> only for the py launcher."""
    if "@PY@" not in template_text:
        raise ValueError("template has no @PY@ placeholder")
    args = "-3 " if os.path.basename(launcher).lower().startswith("py.") else ""
    return template_text.replace("<Arguments>-3 ", "<Arguments>" + args).replace("@PY@", launcher)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "mc2-nightly-gate.task.xml"))
    ap.add_argument("--py", help="launcher full path (default: shutil.which py, then python)")
    a = ap.parse_args(argv)
    launcher = resolve_launcher(override=a.py)
    with open(TEMPLATE, encoding="utf-8") as f:
        text = render(f.read(), launcher)
    with open(a.out, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print(f"rendered {a.out} (launcher {launcher}); NOT registered. The orchestrator registers it with:")
    print(f'schtasks /Create /TN "{TASK_NAME}" /XML {a.out} /F')


if __name__ == "__main__":
    main()
