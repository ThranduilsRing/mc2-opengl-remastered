"""Build bounded editor proofs and copy fresh lane3 stock inputs without launching an editor."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))
from console_child import child_kwargs
from run_editor_smoke import StockInputs, STOCK_MISSIONS, compare_fit


def grade_outputs(inputs, output):
    for mission in STOCK_MISSIONS:
        original = (inputs / f"{mission}.fit").read_bytes()
        written = (output / f"{mission}.fit").read_bytes()
        if not compare_fit(original, written)["byte_identical"]:
            raise ValueError(f"stock byte grader rejected {mission}.fit")
        corruption = (output / f"{mission}.corrupted.fit").read_bytes()
        if compare_fit(original, corruption)["byte_identical"]:
            raise ValueError(f"stock byte grader accepted corrupted output: {mission}.fit")
    print("Stock byte grader: 24/24 unchanged accepted; 24/24 corrupted outputs rejected", flush=True)


def main():
    directory = ROOT / "scratch" / "e3a-proofs" / time.strftime("%Y-%m-%dT%H-%M-%S")
    inputs = directory / "stock"
    inputs.mkdir(parents=True, exist_ok=False)
    stock = StockInputs(Path("A:/Games/mc2-opengl/mc2-win64-lane3"))
    hashes = {}
    for mission in STOCK_MISSIONS:
        for extension in ("fit", "pak"):
            name = f"{mission}.{extension}"
            data = stock.read(f"data/missions/{name}")
            (inputs / name).write_bytes(data)
            hashes[name] = hashlib.sha256(data).hexdigest()
    (directory / "inputs.json").write_text(json.dumps(hashes, indent=2) + "\n", encoding="utf-8")
    env = dict(os.environ, _MSPDBSRV_ENDPOINT_=f"sol-editor-e3a-{os.getpid()}",
               MC2_EDITOR_STOCK_DIR=str(inputs), ASAN_OPTIONS="detect_leaks=0:halt_on_error=1")
    commands = []
    for target in ("editor_document_tests", "editor_document_tests_asan", "EditorShell"):
        commands.append(([sys.executable, "tools/build_quiet.py", "--target", target], 300, f"{target}-build"))
    for target in ("editor_document_tests", "editor_document_tests_asan"):
        commands.append(([str(ROOT / "build64/out/editor/RelWithDebInfo" / (target + ".exe")), "--no-breaks"],
                         120, f"{target}-run"))
    for command, timeout, label in commands:
        env["MC2_EDITOR_BYTE_GRADE_DIR"] = str(directory / label)
        print(subprocess.list2cmdline(command), flush=True)
        try:
            result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, errors="replace",
                                    timeout=timeout, **child_kwargs(redirected=True))
        except subprocess.TimeoutExpired:
            print(f"{label}: TIMEOUT ({timeout}s; no compiler/test verdict)", flush=True)
            return 124
        (directory / f"{label}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        print(result.stdout + result.stderr, end="", flush=True)
        if result.returncode:
            return result.returncode
        if label.endswith("-run"):
            grade_outputs(inputs, Path(env["MC2_EDITOR_BYTE_GRADE_DIR"]))
    print(f"Proof artifacts: {directory}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
