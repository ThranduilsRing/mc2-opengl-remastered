"""configure_analysis.py -- configure-only Ninja tree for clangd (compile_commands.json).
build64 uses the VS generator (no compile_commands). This configures build-analysis/
via configure_ninja.py (same cache options as every other tree), then copies
compile_commands.json to the repo root. NEVER builds, never touches build64.
Usage: py -3 scripts/configure_analysis.py
"""
import os, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from configure_ninja import configure  # noqa: E402


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
    configure(root, "build-analysis")
    shutil.copyfile(f"{root}/build-analysis/compile_commands.json",
                    f"{root}/compile_commands.json")
    print("OK -> compile_commands.json")


if __name__ == "__main__":
    main()
