"""Frozen comparisons must catch field corruption that preserves placement counts."""
import hashlib
from pathlib import Path
import re
import shutil
import struct
import unittest
from unittest.mock import patch
import uuid

from editor.tests.legacy_reference import (FROZEN, PLACEMENTS, buildings, compare_frozen,
    packet, reference_evidence, verify_frozen)


class FrozenParityTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parents[2] / "scratch/e5b1-negative" / uuid.uuid4().hex
        self.root.mkdir(parents=True)
        for path in FROZEN.iterdir():
            shutil.copyfile(path, self.root / path.name)
        self.evidence = reference_evidence(verify_frozen())

    def corrupt_unit(self, family, key, replacement):
        row = verify_frozen()["placements"][family]
        prefix = next(iter(row["part"])).split("/", 1)[0]
        pilot = row["part"][prefix + "/Pilot"][0].split(" ", 1)[1]
        section = "Warrior" + pilot if key in ("Profile", "Brain", "NumCells", "NumStaticVars") else prefix
        path = self.root / f"e5_reference_{family}.fit"
        contents = path.read_bytes()
        start = contents.index(("[" + section + "]").encode())
        end = contents.find(b"\n[", start + 1)
        if end < 0:
            end = len(contents)
        body = contents[start:end]
        mutated, count = re.subn(rb"(?m)^(\w+(?:\[\d+\])?\s+" + key.encode() + rb"\s*=\s*)[^\r\n]*",
                                lambda m: m[1] + replacement.encode(), body)
        self.assertEqual(count, 1)
        path.write_bytes(contents[:start] + mutated + contents[end:])

    def test_frozen_capture_self_compare_and_raw_packet_storage(self):
        self.assertIn("9/9", compare_frozen(self.root, self.evidence)["frozenComparison"])
        for family in PLACEMENTS:
            path = self.root / f"e5_reference_{family}.pak"
            data = path.read_bytes()
            table = struct.unpack_from("<I", data, 4)[0]
            count = table // 4 - 2
            rewritten = bytearray(data[:table])
            for index in range(count):
                struct.pack_into("<I", rewritten, 8 + 4 * index, len(rewritten))
                rewritten.extend(packet(data, index))
            path.write_bytes(rewritten)
        compare_frozen(self.root, self.evidence)

    def test_fresh_baseline_save_guid_keeps_content_exact(self):
        path = self.root / "e5_reference_before.pak"
        original = path.read_bytes()
        changed = original[:-16] + uuid.uuid4().bytes_le
        self.assertNotEqual(changed, original)
        path.write_bytes(changed)
        compare_frozen(self.root, self.evidence)
        # A fresh save identity cannot excuse terrain or tactical-map changes.
        for index in (0, 2):
            with self.subTest(packet=index):
                corrupted = bytearray(changed)
                offset = struct.unpack_from("<I", corrupted, 8 + 4 * index)[0] & 0x1fffffff
                corrupted[offset] ^= 1
                path.write_bytes(corrupted)
                with self.assertRaisesRegex(ValueError, "baseline PAK differs"):
                    compare_frozen(self.root, self.evidence)
        for corrupted in (changed[:-1], changed + b"extra"):
            path.write_bytes(corrupted)
            with self.assertRaisesRegex(ValueError, "16-byte GUID"):
                compare_frozen(self.root, self.evidence)
        path.write_bytes(changed)
        fit = self.root / "e5_reference_before.fit"
        fit.write_bytes(fit.read_bytes().replace(b"\r\n", b"\n"))
        with self.assertRaisesRegex(ValueError, "baseline FIT differs"):
            compare_frozen(self.root, self.evidence)

    def test_complete_unit_values_same_count_controls(self):
        for family in ("mech", "vehicle"):
            for key, replacement in (("VariantNumber", "1"), ("Damage", "1.000000"), ("BaseColor", "0"),
                ("HighlightColor1", "0"), ("HighlightColor2", "0"), ("SelfRepairBehavior", "0"),
                ("ObjectProfile", '"other-profile"'), ("Profile", '"other-pilot"'), ("Brain", '"OtherBrain"'),
                ("NumStaticVars", "1"), ("SquadNum", "108"), ("AlternativeStartIndex", "36"),
                ("ControlDataType", "3"), ("Rotation", "1.000000")):
                with self.subTest(family=family, key=key):
                    self.corrupt_unit(family, key, replacement)
                    with self.assertRaises(ValueError):
                        compare_frozen(self.root, self.evidence)
                    shutil.copyfile(FROZEN / f"e5_reference_{family}.fit", self.root / f"e5_reference_{family}.fit")

    def test_complete_building_records_same_count_controls(self):
        for family in PLACEMENTS.keys() - {"mech", "vehicle"}:
            path = self.root / f"e5_reference_{family}.pak"
            original = path.read_bytes()
            for field in range(10):
                with self.subTest(family=family, field=field):
                    data = bytearray(original)
                    table = struct.unpack_from("<I", data, 4)[0]
                    count = table // 4 - 2
                    records = buildings(data)
                    raw = bytearray(packet(data, 1))
                    offset = 4 + (len(records) - 1) * 40 + field * 4
                    struct.pack_into("<I", raw, offset, struct.unpack_from("<I", raw, offset)[0] ^ 0x40000000)
                    output = bytearray(data[:table])
                    for index in range(count):
                        struct.pack_into("<I", output, 8 + 4 * index, len(output))
                        output.extend(raw if index == 1 else packet(data, index))
                    path.write_bytes(output)
                    with self.assertRaises(ValueError):
                        compare_frozen(self.root, self.evidence)
                    path.write_bytes(original)

    def test_extra_cells_and_missing_fields_are_not_presence_parity(self):
        path = self.root / "e5_reference_mech.fit"
        content = path.read_bytes()
        path.write_bytes(content.replace(b"FITend", b"[Warrior4Cell0]\r\nl Cell = 2\r\nl MemType = 1\r\nf Value = 150\r\nFITend"))
        with self.assertRaisesRegex(ValueError, "complete Part/Warrior/cell"):
            compare_frozen(self.root, self.evidence)

    def test_frozen_fingerprints_and_freeze_refusal(self):
        manifest = self.root / "placements.json"
        manifest.write_bytes(manifest.read_bytes() + b" ")
        with self.assertRaisesRegex(ValueError, "manifest fingerprint"):
            verify_frozen(self.root)
        from editor.tests.legacy_reference import retain
        with self.assertRaises(FileExistsError):
            retain(FROZEN, FROZEN, {})
        self.assertEqual(hashlib.sha256((FROZEN / "placements.json").read_bytes()).hexdigest(),
                         "3e3ed40dfb5c057fadf4d88a2ee36c128335b78bedfa28a745eeb24bb2e5eb37")
        from scripts.run_editor_smoke import main
        with patch("sys.argv", ["run_editor_smoke.py", "--freeze-legacy-reference", str(FROZEN)]), patch("scripts.run_editor_smoke.nofocus.spawn") as launch:
            with self.assertRaises(SystemExit) as stopped:
                main()
            self.assertEqual(stopped.exception.code, 2)
            launch.assert_not_called()

    def test_marker_fit_values_same_count_control(self):
        path = self.root / "e5_reference_marker.fit"
        content = path.read_bytes()
        start = content.index(b"[NavMarker1]")
        end = content.index(b"[NavMarker2]")
        body = content[start:end].replace(b"radius = 256.000000", b"radius = 257.000000")
        self.assertNotEqual(body, content[start:end])
        path.write_bytes(content[:start] + body + content[end:])
        with self.assertRaisesRegex(ValueError, "marker FIT"):
            compare_frozen(self.root, self.evidence)

    def test_owned_marker_legacy_reload_control_requires_loader_save_and_equal_records(self):
        from scripts.run_editor_smoke import run_owned_marker_reload
        deploy, report = self.root / "deploy", self.root / "reload-report"
        report.mkdir()
        source = self.root / "e5_reference_marker"
        def accepted(*args):
            directory = deploy / "terrain_gen_out/owned_marker_reload"
            for extension in ("fit", "pak"):
                shutil.copyfile(source.with_suffix("." + extension), directory / ("reloaded." + extension))
            return {"passed": True, "captured": "", "name": "owned_marker_reload"}
        with patch("scripts.run_editor_smoke.run_case_with_retry", side_effect=accepted) as launch:
            result = run_owned_marker_reload(Path("editor.exe"), deploy, source, report, 3, 180)
            self.assertTrue(result["passed"])
            self.assertIn("legacy loader", result["detail"])
            self.assertEqual(launch.call_args.args[-1], "saved")
            self.assertIn("-mission=terrain_gen_out/owned_marker_reload/input.pak", launch.call_args.args[-2])
        def corrupted(*args):
            result = accepted(*args)
            path = deploy / "terrain_gen_out/owned_marker_reload/reloaded.fit"
            path.write_bytes(path.read_bytes().replace(b"radius = 256.000000", b"radius = 257.000000"))
            return result
        with patch("scripts.run_editor_smoke.run_case_with_retry", side_effect=corrupted):
            self.assertFalse(run_owned_marker_reload(Path("editor.exe"), deploy, source, report, 3, 180)["passed"])
        with patch("scripts.run_editor_smoke.run_case_with_retry", return_value={"passed": False, "bucket": "autoload_missing"}):
            self.assertFalse(run_owned_marker_reload(Path("editor.exe"), deploy, source, report, 3, 180)["passed"])
        shutil.copyfile(FROZEN / "e5_reference_before.fit", source.with_suffix(".fit"))
        with patch("scripts.run_editor_smoke.run_case_with_retry") as launch:
            with self.assertRaisesRegex(ValueError, "one inserted FIT"):
                run_owned_marker_reload(Path("editor.exe"), deploy, source, report, 3, 180)
            launch.assert_not_called()

    def test_owned_marker_reload_requires_clean_current_lane3_executable(self):
        from scripts.run_editor_smoke import marker_reload_provenance
        exe = Path("A:/Games/mc2-opengl/mc2-win64-lane3/Mission Editor.exe").resolve()
        head = "0123456789abcdef"
        with patch("scripts.run_editor_smoke.subprocess.check_output", return_value=head), patch.object(Path, "read_bytes", return_value=b"editor"):
            for fingerprint, observed in (({"present": False, "reason": "banner-absent"}, '"reason": "banner-absent"'),
                ({"present": True, "sha12": "old", "dirty": False}, '"sha12": "old"'),
                ({"present": True, "sha12": head[:12], "dirty": True}, '"dirty": true')):
                with patch("scripts.run_editor_smoke.exe_fingerprint.extract", return_value=fingerprint):
                    with self.assertRaisesRegex(ValueError, "clean build fingerprint") as failure:
                        marker_reload_provenance(exe)
                    detail = str(failure.exception)
                    self.assertIn(exe.as_posix(), detail)
                    self.assertIn("expected_sha=" + head[:12], detail)
                    self.assertIn(observed, detail)
                    self.assertIn("rebuild EditRel", detail)
            with patch("scripts.run_editor_smoke.exe_fingerprint.extract", return_value={"present": True, "sha12": head[:12], "dirty": False}):
                self.assertEqual(marker_reload_provenance(exe)["head"], head)
        with self.assertRaisesRegex(ValueError, "restricted to lane3"):
            marker_reload_provenance(self.root / "editor.exe")

    def test_enemy_cells_same_count_and_remapped_player_controls(self):
        from editor.tests.run_document_proofs import grade_player_defaults
        fields = {"Part30/TeamID": ["c 0"], "Part30/CommanderID": ["c 2"], "Part30/PlayerPart": ["b FALSE"],
            "Part30/ControlDataType": ["ul 1"], "Part30/ObjectProfile": ['st "PM101100"'],
            "Warrior30/Profile": ['st "PMWGreen"'], "Warrior30/Brain": ['st "DredAttack01"'],
            "Warrior30/NumCells": ["l 3"], "Warrior30/NumStaticVars": ["l 0"]}
        for index, (number, kind, value) in enumerate(((2, 1, 150), (9, 0, 0), (28, 0, 0))):
            fields.update({f"Warrior30Cell{index}/Cell": [f"l {number}"], f"Warrior30Cell{index}/MemType": [f"l {kind}"],
                           f"Warrior30Cell{index}/Value": [f"f {value}"]})
        grade_player_defaults(fields, 2, 53, "PMWGreen")
        for key in fields:
            with self.subTest(key=key):
                corrupted = dict(fields)
                kind, value = fields[key][0].split(" ", 1)
                corrupted[key] = [kind + ' "other"' if kind == "st" else kind + " 7"]
                with self.assertRaisesRegex(ValueError, "policy values"):
                    grade_player_defaults(corrupted, 2, 53, "PMWGreen")
        with self.assertRaises(ValueError):
            grade_player_defaults(fields, 0, 53, "PMWGreen")

    def test_owned_writer_preserves_existing_cursors_and_cells(self):
        from editor.tests.legacy_reference import preserve_authored_units
        before = {"Part1/AlternativeStartIndex": ["ul 35"], "Warrior1Cell0/Value": ["f 150"]}
        preserve_authored_units(before, {**before, "Part30/ObjectNumber": ["ul 53"]})
        for key in before:
            with self.assertRaisesRegex(ValueError, "authored"):
                preserve_authored_units(before, {**before, key: ["ul 99"]})
        with self.assertRaisesRegex(ValueError, "authored"):
            preserve_authored_units(before, {**before, "Warrior1/Extra": ["l 1"]})


if __name__ == "__main__":
    unittest.main()
