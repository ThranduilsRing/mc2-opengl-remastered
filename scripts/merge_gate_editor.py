"""Incremental MFC editor build and offline unit tier for the merge gate."""
from pathlib import Path
import subprocess
import sys

import console_child

ROOT = Path(__file__).resolve().parents[1]
TESTS = ("editor_document_tests", "editor_panel_tests", "editor_objective_reader_tests")


def main(jobs):
    commands = [[sys.executable, "scripts/ninja_build.py", "-k", "0", f"-j{jobs}",
                 "EditRel", *TESTS, "ui_editor"]]
    commands += [[str(ROOT / "build64-ninja/out/editor" / (name + ".exe"))] for name in TESTS]
    for command in commands:
        result = subprocess.call(command, cwd=ROOT, **console_child.child_kwargs(redirected=True))
        if result:
            return result
    return 0


if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1])))
