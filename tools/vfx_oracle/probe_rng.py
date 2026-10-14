"""Bounded standalone RNG probe; the orchestrator still builds the VS engine tree."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which("clang++")
    if not compiler:
        parser.error("clang++ required for the standalone probe")
    exe = args.out.resolve() / "rng_probe.exe"
    command = [compiler, "-std=c++23", "-O2", "-g", "-DNDEBUG", "-fms-extensions",
               "-ffunction-sections", "-DPLATFORM_WINDOWS", "-DLINUX_BUILD",
               "-Wno-deprecated-declarations"]
    for include in ("mclib", "GameOS/include", "GameOS/gameos", "."):
        command += ["-I", str(ROOT / include)]
    command += [str(ROOT / "tools/vfx_oracle/rng_probe.cpp"),
                str(ROOT / "mclib/stuff/random.cpp"), "-Xlinker", "/OPT:REF", "-o", str(exe)]
    (args.out / "compile-command.json").write_text(json.dumps(command, indent=2) + "\n", encoding="utf-8")
    for name, argv, timeout in (("compile", command, 30), ("run", [str(exe)], 10)):
        try:
            result = subprocess.run(argv, capture_output=True, text=True, cwd=ROOT, timeout=timeout,
                                    **console_child.child_kwargs(redirected=True))
        except subprocess.TimeoutExpired:
            (args.out / f"{name}.log").write_text(f"TIMEOUT after {timeout}s\n", encoding="utf-8")
            print(f"RNG probe {name}: TIMEOUT ({timeout}s)")
            return 124
        (args.out / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        print(f"RNG probe {name}: rc={result.returncode}")
        if result.returncode:
            return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
