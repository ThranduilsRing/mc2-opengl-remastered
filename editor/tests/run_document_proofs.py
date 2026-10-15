"""Build bounded editor proofs and copy fresh lane3 stock inputs without launching an editor."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import zlib
import re

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))
from console_child import child_kwargs
from run_editor_smoke import StockInputs, STOCK_MISSIONS, compare_fit
from editor.tests.legacy_reference import (verify_frozen, compare_frozen, FROZEN, typed_fields,
    canonical_units, preserve_authored_units)
from editor.tests.run_catalog_proofs import oracle


def grade_player_defaults(fields, player, object_id, profile):
    object_profile = (("PM207300" if player == 0 else "PM101100") if object_id == 53
                      else ("pv20600" if player == 0 else "pv20500"))
    expected = {
        "Part30/TeamID": [f"c {3 if player == 0 else 0}"], "Part30/CommanderID": [f"c {player}"],
        "Part30/PlayerPart": ["b TRUE" if player == 0 else "b FALSE"],
        "Part30/ControlDataType": [f"ul {1 if object_id == 53 else 2}"],
        "Part30/ObjectProfile": [f'st "{object_profile}"'],
        "Warrior30/Profile": [f'st "{profile}"'], "Warrior30/Brain": ['st "PBrain"' if player == 0 else 'st "DredAttack01"'],
        "Warrior30/NumCells": [f"l {0 if player == 0 else 3}"], "Warrior30/NumStaticVars": ["l 0"],
    }
    for index, (number, kind, value) in enumerate(() if player == 0 else ((2, 1, 150), (9, 0, 0), (28, 0, 0))):
        for key, item in (("Cell", f"l {number}"), ("MemType", f"l {kind}"), ("Value", f"f {value}")):
            expected[f"Warrior30Cell{index}/{key}"] = [item]
    selected = {key: fields[key] for key in expected if key in fields}
    selected.update({key: value for key, value in fields.items() if key.startswith("Warrior30Cell")})
    if typed_fields(selected) != typed_fields(expected):
        raise ValueError("player/profile/brain/cell policy values differ")


def grade_placements(output, evidence):
    result = compare_frozen(output, evidence)
    (output / "placement-grade.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    report = json.loads((output / "mod-placement.json").read_text())
    if report["id"] != 2 or report["appearance"] != "Starslayer":
        raise ValueError("mod placement identity mismatch")
    independent = oracle(Path("A:/Games/mc2-opengl/mc2-win64-lane3"), Path("A:/Games/mc2-opengl"), "MC2X-CVE-G")
    for source in report["sources"]:
        contents, layer, path = independent(source["key"])
        if layer != source["layer"] or path.resolve() != Path(source["source"]).resolve() or (output / "provenance" / source["key"]).read_bytes() != contents:
            raise ValueError("mod placement provenance disagrees with independent resolution")
        source["sha256"] = hashlib.sha256(contents).hexdigest()
    if not any(s["key"] == "data/objects/object2.pak" and s["layer"] == "mod:MC2X-CVE-G" for s in report["sources"]):
        raise ValueError("mod placement did not consume overriding definition")
    from run_editor_smoke import fit_fields
    stock = oracle(Path("A:/Games/mc2-opengl/mc2-win64-lane3"), Path("A:/Games/mc2-opengl"), "")
    profiles = []
    for key in ("data/objects/pilots.csv", "data/objects/badpilots.csv"):
        table = stock(key)[0]
        if table.startswith(b"\x78"):
            table = zlib.decompress(table)
        for name in table.decode("ascii").splitlines():
            if not name:
                break
            try:
                stock("data/missions/warriors/" + name.lower() + ".fit")
            except KeyError:
                continue
            profiles.append(name)
            break
    if len(profiles) != 2:
        raise ValueError("independent pilot resolution found no usable defaults")
    before = fit_fields((output / "e5_reference_before.fit").read_bytes())
    for row in result["placements"].values():
        preserve_authored_units(before, row["fitFields"])
    for player in (0, 1, 2):
        for object_id in (53, 117):
            fields = fit_fields((output / f"player{player}-{object_id}.fit").read_bytes())
            grade_player_defaults(fields, player, object_id, profiles[0 if player == 0 else 1])
            preserve_authored_units(before, fields)
            if canonical_units(fields, 30, 30) != canonical_units(before):
                raise ValueError("player policy insertion changed existing Parts/Warriors")
    print("Player policy grader: 6/6 player, enemy and remapped-team saved defaults equal independent pilot tables", flush=True)
    fields = fit_fields((output / "mod-placement.fit").read_bytes())
    preserve_authored_units(before, fields)
    if fields.get("Part30/ObjectNumber") != ["ul 2"] or fields.get("Part30/CSVFile") != [f'st "{report["stem"]}"']:
        raise ValueError("mod provenance does not identify the saved placement")
    profile = fields["Warrior30/Profile"][0][4:-1]
    expected_sources = {"data/art/buildings.csv", "data/objects/object2.pak", "data/objects/pilots.csv",
                        "data/missions/warriors/" + profile.lower() + ".fit"}
    variant = "data/objects/" + report["stem"].lower() + ".csv"
    try:
        independent(variant)
    except KeyError:
        pass
    else:
        expected_sources.add(variant)
    actual_sources = [s["key"] for s in report["sources"]]
    if len(actual_sources) != len(set(actual_sources)) or set(actual_sources) != expected_sources:
        raise ValueError("mod placement provenance coverage differs from independently required files")
    (output / "mod-placement-grade.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Placement grader: 9/9 frozen parity; actual MC2X-CVE-G insertion and independent provenance PASS", flush=True)


def grade_outputs(inputs, output):
    from editor.tests.objective_census import grade_typed_objectives, grade_payload_outputs
    grade_typed_objectives(inputs, output)
    grade_payload_outputs(output)
    print("Condition payload grader: 32 changed exports and wrong-prefix/value, unchanged, unrelated-byte and missing-output controls PASS", flush=True)
    print("Objective semantic grader: 173/173 typed models equal independently parsed stock reader values", flush=True)
    for mission in STOCK_MISSIONS:
        original = (inputs / f"{mission}.fit").read_bytes()
        written = (output / f"{mission}.fit").read_bytes()
        if not compare_fit(original, written)["byte_identical"]:
            raise ValueError(f"stock byte grader rejected {mission}.fit")
        corruption = (output / f"{mission}.corrupted.fit").read_bytes()
        if compare_fit(original, corruption)["byte_identical"]:
            raise ValueError(f"stock byte grader accepted corrupted output: {mission}.fit")
    print("Stock byte grader: 24/24 unchanged accepted; 24/24 corrupted outputs rejected", flush=True)


def grade_shipping_shell():
    binaries = ROOT / "build64/out/editor/RelWithDebInfo"
    shipping = (binaries / "Mission Editor Shell.exe").read_bytes()
    smoke = (binaries / "Mission Editor Smoke.exe").read_bytes()
    for marker in (b"--panel-smoke", b"panel_fixture_seed_failed", b"[EDITOR_PANEL_OPERATION v1]",
                   b"Q3 long mission ", b"--file-smoke", b"file_fixture_seed_failed", b"[EDITOR_FILE v1]",
                   b"chooser_catalog_missing", b"[EDITOR_CHOOSER_VIEW v1]", b"marker_fixture_replace_failed",
                   b"objective_fixture_missing", b"objective_widget_value_wrong",
                   b"objective_crud_fixture_missing", b"objective_crud_reopen_widget_wrong"):
        if marker in shipping or marker not in smoke:
            raise ValueError(f"shipping/smoke isolation failed: {marker!r}")
    print("Shipping shell: proof markers absent; smoke host: proof markers present", flush=True)


def main():
    from editor.tests.objective_census import census
    current = census(StockInputs(Path("A:/Games/mc2-opengl/mc2-win64-lane3")))
    if current != json.loads((ROOT / "editor/tests/objective-census.json").read_text()):
        raise ValueError("stock objective census differs from the committed input hashes and schema")
    print("Objective census: 24 mission hashes, scalar fields and four lists match the committed census", flush=True)
    verify_frozen()
    print("Frozen corpus: nine committed captures, manifest and baseline fingerprints verified", flush=True)
    directory = ROOT / "scratch" / "e4-proofs" / time.strftime("%Y-%m-%dT%H-%M-%S")
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
    env = dict(os.environ, _MSPDBSRV_ENDPOINT_=f"sol-editor-egl-{os.getpid()}",
               ASAN_OPTIONS="detect_leaks=0:halt_on_error=1")
    commands = []
    for target in ("editor_document_tests", "editor_document_tests_asan", "EditorShell", "EditorShellSmoke", "editor_panel_tests", "editor_objective_reader_tests"):
        commands.append(([sys.executable, "tools/build_quiet.py", "--target", target], 300, f"{target}-build"))
    for target in ("editor_document_tests", "editor_document_tests_asan", "editor_panel_tests"):
        label = f"{target}-run"
        command = [str(ROOT / "build64/out/editor/RelWithDebInfo" / (target + ".exe")),
                   "--no-breaks", "--stock-dir", str(inputs)]
        if target != "editor_panel_tests":
            command += ["--byte-grade-dir", str(directory / label), "--require-stock-corpus"]
            output = directory / (label + "-placements")
            output.mkdir()
            for extension in ("fit", "pak"):
                (output / f"e5_reference_before.{extension}").write_bytes((FROZEN / f"e5_reference_before.{extension}").read_bytes())
            command += ["--placement-base", "A:/Games/mc2-opengl/mc2-win64-lane3", "--placement-mods", "A:/Games/mc2-opengl",
                        "--placement-goldens", str(FROZEN), "--placement-output", str(output)]
        else:
            command += ["--placement-base", "A:/Games/mc2-opengl/mc2-win64-lane3", "--placement-mods", "A:/Games/mc2-opengl",
                        "--placement-goldens", str(FROZEN), "--placement-output", str(directory / "panel-placements")]
        if target == "editor_document_tests_asan":
            # Memory-heavy ASan suites need headroom under host load and must cover every native case.
            commands.append((command + ["--test-suite-exclude=placement-live"], 300, "editor_document_tests_asan-core-run"))
            commands.append((command + ["--test-suite=placement-live"], 300, "editor_document_tests_asan-placement-run"))
        else:
            commands.append((command, 120, label))
    native_cases = 0
    asan_cases = []
    for command, timeout, label in commands:
        print(subprocess.list2cmdline(command), flush=True)
        try:
            result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, errors="replace",
                                    timeout=timeout, **child_kwargs(redirected=True))
        except subprocess.TimeoutExpired as error:
            partial = error.stdout or b""
            if isinstance(partial, bytes):
                partial = partial.decode("utf-8", errors="replace")
            (directory / f"{label}.timeout.log").write_text(partial, encoding="utf-8")
            print(f"{label}: TIMEOUT ({timeout}s; no compiler/test verdict)", flush=True)
            return 124
        (directory / f"{label}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        print(result.stdout + result.stderr, end="", flush=True)
        if result.returncode:
            return result.returncode
        if label.startswith("editor_document_tests_asan-") and label.endswith("-run"):
            match = re.search(r"\[doctest\] test cases:\s+(\d+)\s+\|\s+(\d+) passed\s+\|\s+0 failed", result.stdout)
            if not match or int(match[1]) == 0 or match[1] != match[2]:
                raise ValueError("ASan split has missing or failed test cases")
            asan_cases.append(int(match[1]))
            if label == "editor_document_tests_asan-core-run":
                grade_outputs(inputs, directory / "editor_document_tests_asan-run")
            else:
                grade_placements(directory / "editor_document_tests_asan-run-placements", result.stdout)
        elif label == "editor_document_tests-run":
            match = re.search(r"\[doctest\] test cases:\s+(\d+)", result.stdout)
            if not match:
                raise ValueError("normal document test coverage summary missing")
            native_cases = int(match[1])
            grade_outputs(inputs, directory / label)
            grade_placements(directory / (label + "-placements"), result.stdout)
        elif label == "editor_panel_tests-run":
            from scripts.run_editor_smoke import grade_panel, panel_negative_controls
            rows = re.findall(r"\[EDITOR_WIDGET_ARTIFACT v1\] name=(\S+) directory=(\S+)", result.stdout)
            if [name for name, _ in rows] != ["chooser", "chooser_mod", "chooser_player", "chooser_render", "marker_panel", "objectives_panel", "objectives_crud_panel"]:
                raise ValueError("chooser/marker native widget artifacts missing or duplicated")
            for name, path in rows:
                grade_panel(name, ROOT / path, 0, require_render=False)
                panel_negative_controls(name, ROOT / path)
            objective_widgets = ROOT / dict(rows)["objectives_panel"]
            command = [str(ROOT / "build64/out/editor/RelWithDebInfo/editor_objective_reader_tests.exe"),
                       "--no-breaks", "--byte-grade-dir", str(objective_widgets),
                       "--placement-base", "A:/Games/mc2-opengl/mc2-win64-lane3",
                       "--placement-mods", "A:/Games/mc2-opengl", "--placement-goldens", str(FROZEN),
                       "--placement-output", str(directory / "reader-placements")]
            print(subprocess.list2cmdline(command), flush=True)
            try:
                reader = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, errors="replace",
                                        timeout=120, **child_kwargs(redirected=True))
            except subprocess.TimeoutExpired as error:
                partial = error.stdout or b""
                if isinstance(partial, bytes):
                    partial = partial.decode("utf-8", errors="replace")
                (directory / "editor_objective_reader_tests-run.timeout.log").write_text(partial, encoding="utf-8")
                print("editor_objective_reader_tests-run: TIMEOUT (120s; no test verdict)", flush=True)
                return 124
            (directory / "editor_objective_reader_tests-run.log").write_text(reader.stdout + reader.stderr, encoding="utf-8")
            print(reader.stdout + reader.stderr, end="", flush=True)
            if reader.returncode:
                return reader.returncode
            if not re.search(r"\[doctest\] test cases:\s+7\s+\|\s+7 passed\s+\|\s+0 failed\s+\|\s+0 skipped", reader.stdout):
                raise ValueError("real objective reader cases were skipped or omitted")
            print("Offline proofs with installed prerequisites: widget graders and seven real FIT/text reader cases executed; species dispatch remains source-derived; rendered-widget proof awaits smoke host", flush=True)
    if len(asan_cases) != 2 or sum(asan_cases) != native_cases:
        raise ValueError("ASan split did not cover the complete normal document suite")
    print(f"ASan coverage: {' + '.join(map(str, asan_cases))} = {native_cases} native cases; no cases omitted", flush=True)
    grade_shipping_shell()
    print(f"Proof artifacts: {directory}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
