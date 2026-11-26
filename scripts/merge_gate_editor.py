"""Incremental editor build and offline suites (the editor gate row is advisory)."""
from pathlib import Path
import subprocess
import sys

import console_child

ROOT = Path(__file__).resolve().parents[1]
TESTS = ("editor_document_tests", "editor_panel_tests", "editor_objective_reader_tests")


def main(jobs):
    result = subprocess.call([sys.executable, "scripts/ninja_build.py", "-k", "0", f"-j{jobs}",
                            "EditRel", *TESTS, "ui_editor"], cwd=ROOT,
                           **console_child.child_kwargs(redirected=True))

    if result:
        print(f"editor build: FAIL ({result})", flush=True)
        return result
    results = (
        (TESTS[0], subprocess.call(["build64-ninja/out/editor/editor_document_tests.exe"],
                                  executable=str(ROOT / "build64-ninja/out/editor/editor_document_tests.exe"),
                                  cwd=ROOT, **console_child.child_kwargs(redirected=True))),
        (TESTS[1], subprocess.call(["build64-ninja/out/editor/editor_panel_tests.exe"],
                                  executable=str(ROOT / "build64-ninja/out/editor/editor_panel_tests.exe"),
                                  cwd=ROOT, **console_child.child_kwargs(redirected=True))),
        (TESTS[2], subprocess.call(["build64-ninja/out/editor/editor_objective_reader_tests.exe"],
                                  executable=str(ROOT / "build64-ninja/out/editor/editor_objective_reader_tests.exe"),
                                  cwd=ROOT, **console_child.child_kwargs(redirected=True))),
    )
    failed = False
    for name, result in results:
        print(f"editor suite {name}: {'FAIL' if result else 'PASS'} ({result})", flush=True)
        failed |= bool(result)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1])))
