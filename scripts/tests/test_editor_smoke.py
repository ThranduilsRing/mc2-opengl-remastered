"""Negative controls for E0: absent facts and stale outputs cannot become proof."""
import importlib.util
from pathlib import Path
import sys
import shutil
import json
import uuid
from contextlib import contextmanager
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "run_editor_smoke.py"
TEMP_ROOT = SCRIPT.parents[1] / "scratch" / "editor-smoke-tests"
TEMP_ROOT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(SCRIPT.parent))
spec = importlib.util.spec_from_file_location("editor_smoke", SCRIPT)
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


@contextmanager
def scratch_directory():
    path = TEMP_ROOT / uuid.uuid4().hex
    path.mkdir()
    try:
        yield path
    finally:
        assert path.resolve().is_relative_to(TEMP_ROOT.resolve())
        shutil.rmtree(path)


def panel_lines():
    panels = "\n".join(f"[EDITOR_PANEL v1] workflow={w} panel={p} coverage={'model-only' if p in smoke.MODEL_ONLY else 'action-handler'} reached={0 if p in smoke.MODEL_ONLY else 1} ok=1 authored=1 undo=1 saved=1 reloaded=1"
                     for p, w in smoke.WORKFLOW_PANELS.items())
    definitions = "[EDITOR_DEFINITIONS v1] stage=duplicate_unit_refresh_undo_redo ok=1\n[EDITOR_DEFINITIONS v1] stage=script_boundary ok=1\n[EDITOR_DEFINITIONS v1] preservation_commands_undo_reload_malformed=1"
    for field in ("Title", "Description", "ActivateFlagID", "ResetStatusFlagID", "ObjectiveModel",
                  "MissionName", "Author", "Blurb", "Blurb2", "AVIFilename", "DownloadURL", "ScenarioScript"):
        limit = 1023 if field == "ScenarioScript" else 2000
        definitions += f"\n[EDITOR_DEFINITIONS v1] stage=oversized_text field={field} rejected=1 continued=1 ok=1"
        definitions += f"\n[EDITOR_DOCUMENT v1] mission=fixture.fit field=Block/{field} length={limit + 1} limit={limit} reason=too_long rejected=1"
    for field in ("NumConditions", "NumObjectives", "Version", "scenarioTuneNum", "MissionName", "Blurb2",
                  "ScenarioScript", "MissionNameResourceStringID"):
        definitions += f"\n[EDITOR_DEFINITIONS v1] stage=missing_required field={field} rejected=1 unchanged=1 ok=1"
        definitions += f"\n[EDITOR_DOCUMENT v1] mission=fixture.fit field=Block/{field} missing=1 rejected=1"
    def reader_case(stage, case, filename, reason, field, flags):
        nonlocal definitions
        definitions += f"\n[EDITOR_DEFINITIONS v1] stage={stage} case={case} field={field} {flags} ok=1"
        if reason:
            definitions += f"\n[EDITOR_DOCUMENT v1] mission=terrain_gen_out/{filename}.fit field=Block/{field} {reason} rejected=1"
    for kind, field in smoke.READER_PAYLOADS.items():
        for mode in ("oversized", "unterminated", "trailing", "nul"):
            case = f"{kind}_{mode}"
            reader_case("payload", case, f"ew_payload_{case}",
                        "length=2001 limit=2000 reason=too_long" if mode == "oversized" else "length=10 limit=2000 reason=malformed_literal",
                        field, "retained=1 following=1")
    for mode in smoke.READER_LITERALS:
        reader_case("literal", mode, f"ew_literal_{mode}", None if mode == "cr" else "length=10 limit=2000 reason=malformed_literal",
                    "Title", "retained=1 following=1")
    for mode in ("unterminated", "trailing", "nul", "unquoted"):
        reader_case("metadata_literal", mode, f"ew_metadata_{mode}", "length=10 limit=2000 reason=malformed_literal",
                    "Author", "retained=1 following=1")
    for case in smoke.READER_SPECIES:
        reader_case("species", case, f"ew_species_{case}",
                    "length=10 limit=2000 reason=malformed_literal" if case.startswith("literal_") else "length=10 limit=23 reason=invalid_species",
                    "ConditionSpecies", "following=1")
    for case, field in smoke.READER_MISSING_PAYLOADS.items():
        reader_case("missing_payload", case, f"ew_missing_{case}", "missing=1", field, "rejected=1 unchanged=1")
    return panels + "\n" + definitions


class EditorSmokeTests(unittest.TestCase):
    def test_review_probes_require_versioned_facts_and_single_rejection_diagnostic(self):
        lines = panel_lines()
        for changed in (lines.replace("[EDITOR_DEFINITIONS v1]", "[EDITOR_DEFINITIONS]"),
                        lines.replace("[EDITOR_DOCUMENT v1]", "[EDITOR_DOCUMENT]"),
                        lines.replace("unchanged=1", "unchanged=0"),
                        lines.replace("continued=1", "continued=0"),
                        lines.replace("limit=2000", "limit=1999"),
                        lines.replace(" missing=1", " missing=0"),
                        lines.replace("script_boundary ok=1", "script_boundary ok=0"),
                        lines.replace("duplicate_unit_refresh_undo_redo ok=1", "duplicate_unit_refresh_undo_redo ok=0"),
                        lines + "\n[EDITOR_DOCUMENT v1] mission=fixture.fit field=Block/Title length=2001 limit=2000 reason=too_long rejected=1",
                        lines + "\n[EDITOR_DOCUMENT v1] mission=fixture.fit field=Block/NumConditions missing=1 rejected=1"):
            with self.subTest(changed=changed):
                self.assertTrue(smoke.grade_workflows(changed))

    def test_reader_review_cases_require_exact_preservation_and_one_diagnostic(self):
        lines = panel_lines()
        for changed in (lines.replace("retained=1", "retained=0"), lines.replace("following=1", "following=0"),
                        lines.replace("reason=malformed_literal", "reason=accepted"),
                        lines.replace("reason=invalid_species", "reason=accepted"),
                        lines.replace("case=legacy_unit_watch", "case=missing_probe"),
                        lines.replace("case=action_flag_oversized", "case=missing_probe")):
            with self.subTest(changed=changed):
                self.assertTrue(smoke.grade_workflows(changed))
        for line in lines.splitlines():
            if "mission=terrain_gen_out/ew_" in line:
                with self.subTest(line=line):
                    self.assertTrue(smoke.grade_workflows(lines.replace(line, "")))
                    self.assertTrue(smoke.grade_workflows(lines + "\n" + line))
                    if "reason=" in line:
                        self.assertTrue(smoke.grade_workflows(lines.replace(line, line.replace(" length=", " missing_length="))))
                        self.assertTrue(smoke.grade_workflows(lines.replace(line, line.replace(" limit=", " missing_limit="))))

    def test_pinned_delta_uses_exact_input_and_reports_disappeared_classes(self):
        with scratch_directory() as directory:
            original = b'[Part1]\nul ControlDataType = 2\nst CSVFile = "Authored_CASE"\n[Cameras]\nf ZoomMax = 1.0\n'
            rewritten = original.replace(b'= 2', b'= 1').replace(b'Authored_CASE', b'authored_case').replace(b'= 1.0', b'= 2.0')
            old = smoke.compare_fit(original, rewritten)
            (directory / "mc2_01.json").write_text(json.dumps(old), encoding="utf-8")
            (directory / "mc2_01.written.fit").write_bytes(rewritten)
            current = smoke.compare_fit(original, original)
            delta = smoke.pinned_class_delta("mc2_01", original, current, directory)
            self.assertEqual(delta["disappeared"], ["Cameras", "Part CSV spelling", "Part control/profile"])
            self.assertEqual(delta["remaining"], [])
            mismatch = smoke.pinned_class_delta("mc2_01", original + b"\n", current, directory)
            self.assertEqual(mismatch["status"], "different pinned input")
            missing = smoke.pinned_class_delta("mc2_22", original, current, directory)
            self.assertEqual(missing["status"], "no pinned comparison")

    def test_definition_preservation_and_command_probe_is_required(self):
        lines = panel_lines()
        without_definitions = "\n".join(line for line in lines.splitlines() if "panel=MissionDefinitions" not in line)
        self.assertIn("mission_definitions/MissionDefinitions", smoke.grade_workflows(without_definitions))
        self.assertEqual(smoke.grade_workflows(lines), "")

    def test_adopted_fixture_contains_all_24_exact_hash_pairs_and_stock_provenance(self):
        baseline = json.loads(smoke.BASELINE.read_text(encoding="utf-8"))
        self.assertEqual(baseline["version"], 1)
        self.assertIn("stock", baseline["reason"])
        self.assertEqual(set(baseline["missions"]), set(smoke.STOCK_MISSIONS))
        for fact in baseline["missions"].values():
            self.assertIn(fact["outcome"], {"identical", "differs", "blocked"})
            self.assertRegex(fact["input_sha256"], r"^[0-9a-f]{64}$")
            if fact["outcome"] != "blocked":
                self.assertRegex(fact["output_sha256"], r"^[0-9a-f]{64}$")
            else:
                self.assertIsNone(fact["output_sha256"])
        self.assertEqual(len(baseline["archives"]), 8)
        for allowance in baseline["stock_allowlist"].values():
            self.assertRegex(allowance["sha256"], r"^[0-9a-f]{64}$")
            self.assertTrue(allowance["reason"].strip())

    def setUp(self):
        def manifest(deploy, destination, allowlist):
            destination.write_text('{}', encoding="utf-8")
            return dict(archives={"art.fst": "stock"}, loose_files=[])
        self.audit = patch.object(smoke, "dependency_manifest", side_effect=manifest)
        self.audit.start()
        self.addCleanup(self.audit.stop)

    def test_all_required_workflows(self):
        self.assertEqual(smoke.grade_workflows(panel_lines()), "")
        self.assertEqual(len(set(smoke.WORKFLOW_PANELS.values())), 7)

    def test_missing_failed_and_mislabelled_panel_are_red(self):
        lines = panel_lines()
        for panel, workflow in smoke.WORKFLOW_PANELS.items():
            line = next(line for line in lines.splitlines() if f"panel={panel} " in line)
            reached = "0" if panel in smoke.MODEL_ONLY else "1"
            for replacement in ("", line.replace("ok=1", "ok=0"), line.replace(f"reached={reached}", f"reached={1-int(reached)}"), line.replace(f"workflow={workflow}", "workflow=wrong")):
                with self.subTest(panel=panel, replacement=replacement):
                    self.assertTrue(smoke.grade_workflows(lines.replace(line, replacement)))

    def test_duplicate_ipc_facts_must_agree(self):
        lines = panel_lines()
        duplicate_panels = "\n".join(line for line in lines.splitlines() if line.startswith("[EDITOR_PANEL v1]"))
        self.assertEqual(smoke.grade_workflows(lines + "\n" + duplicate_panels), "")
        self.assertIn("conflicting", smoke.grade_workflows(lines + "\n" + lines.replace("ok=1", "ok=0")))

    def test_workflow_report_lists_all_failures(self):
        lines = panel_lines().replace("panel=PaintMaterial coverage=action-handler reached=1 ok=1", "panel=PaintMaterial coverage=action-handler reached=1 ok=0").replace("panel=ObjectivesSummary coverage=model-only reached=0 ok=1", "panel=ObjectivesSummary coverage=model-only reached=0 ok=0")
        detail = smoke.grade_workflows(lines)
        self.assertIn("terrain_paint/PaintMaterial", detail)
        self.assertIn("objectives/ObjectivesSummary", detail)

    def test_byte_match_and_line_endings(self):
        original = b"FITini\r\r\n[A]\r\r\nl Count = 1\r\r\nFitEnd\r\r\n"
        same = smoke.compare_fit(original, original)
        self.assertTrue(same["byte_identical"])
        self.assertEqual(same["diff_size"], 0)
        result = smoke.compare_fit(original, original.replace(b"\r\r\n", b"\n"))
        self.assertFalse(result["byte_identical"])
        self.assertIn("FINDING: Line-ending normalization only", result["explanation"])
        self.assertEqual(result["changed"], [])

    def test_diff_explains_removed_added_changed_fields(self):
        result = smoke.compare_fit(b"[A]\nl Keep = 1\nl Lost = 2\n", b"[A]\nl Keep = 3\nl New = 4\n")
        self.assertEqual(result["removed"], ["A/Lost"])
        self.assertEqual(result["added"], ["A/New"])
        self.assertEqual(result["changed"], ["A/Keep"])
        self.assertEqual(result["diff_size"], 10)

    def test_duplicates_and_array_continuations(self):
        fields = smoke.fit_fields(b"[A]\nf[3] Cells = 1,\n  2, 3,\nl Key = 1\nl Key = 2\n")
        self.assertEqual(fields["A/Cells"], ["f[3] 1, 2, 3,"])
        self.assertEqual(fields["A/Key"], ["l 1", "l 2"])

    def test_numeric_spelling_is_lossless_but_coordinate_drift_is_semantic(self):
        original = b"[NavMarker0]\nf yPos = -789.333313\n"
        drift = smoke.compare_fit(original, original.replace(b"-789.333313", b"-789.333496"))
        self.assertEqual(drift["classification"], "semantic")
        self.assertEqual(drift["classes"][0]["name"], "NavMarker coordinates")
        formatted = smoke.compare_fit(b"[Cameras]\nf NewScale = 1.0\n", b"[Cameras]\nf NewScale = 1.000000\n")
        self.assertEqual(formatted["classification"], "lossless-only")

    def test_case_only_csv_does_not_bless_control_changes_or_other_paths(self):
        original = b'[Part1]\nst CSVFile = "LRMC"\nul ControlDataType = 2\n'
        spelling = smoke.compare_fit(original, original.replace(b"LRMC", b"Lrmc"))
        self.assertEqual(spelling["classification"], "lossless-only")
        changed = smoke.compare_fit(original, original.replace(b"LRMC", b"Lrmc").replace(b"= 2", b"= 1"))
        self.assertEqual(changed["classification"], "semantic")
        self.assertEqual({c["name"] for c in changed["classes"]}, {"Part CSV spelling", "Part control/profile"})
        other = smoke.compare_fit(original, original.replace(b"LRMC", b"Other"))
        self.assertEqual(other["classification"], "semantic")

    def test_unreviewed_fields_and_explicit_defaults_are_not_assumed_lossless(self):
        for written in (b"[A]\nl Unknown = 2\n", b"[A]\nl Unknown = 1\nl Default = 0\n"):
            self.assertEqual(smoke.compare_fit(b"[A]\nl Unknown = 1\n", written)["classification"], "semantic")

    def test_ledger_refresh_preserves_reviewed_writer_findings(self):
        with scratch_directory() as temp:
            ledger = temp / "ledger.md"
            notes = "\n## Writer findings\n\nReviewed cause and artifact decisions.\n"
            ledger.write_text("Old census\n" + notes, encoding="utf-8")
            smoke.write_roundtrip_ledger(ledger, [], {}, Path("scratch/evidence"))
            self.assertTrue(ledger.read_text(encoding="utf-8").endswith(notes))
            self.assertIn("0 semantic; 24 blocked", ledger.read_text(encoding="utf-8"))

    def test_missing_comparison_is_unverified_in_all_24_ledger_rows(self):
        with scratch_directory() as temp:
            ledger = Path(temp) / "ledger.md"
            smoke.write_roundtrip_ledger(ledger, [], {}, Path("scratch/evidence"))
            data = ledger.read_text(encoding="utf-8")
            self.assertEqual(data.count("| blocked |"), 24)
            self.assertNotIn("| no |", data)

    def test_ledger_finding_does_not_hide_missing_rows(self):
        with scratch_directory() as temp:
            ledger = Path(temp) / "ledger.md"
            diff = smoke.compare_fit(b"[A]\nl Count = 1\n", b"[A]\nl Count = 2\n")
            smoke.write_roundtrip_ledger(ledger, [{"name": "stock_roundtrip:mc2_01", "roundtrip": diff}], {0: ["mc2_01"]}, Path("scratch/evidence"))
            data = ledger.read_text(encoding="utf-8")
            self.assertEqual(data.count("| blocked |"), 23)
            self.assertIn("| mc2_01 | no | 1 | FINDING:", data)

    def test_campaign_group_representatives_cover_every_group(self):
        blocks = ["[Campaign]", "l GroupCount = 2"]
        for group, missions in enumerate((smoke.STOCK_MISSIONS[:8], smoke.STOCK_MISSIONS[8:])):
            blocks += [f"[Group{group}]", f"l MissionCount = {len(missions)}"]
            for n, mission in enumerate(missions):
                blocks += [f"[Group{group}Mission{n}]", f'st FileName = "{mission}"']
        groups = smoke.campaign_groups("\n".join(blocks).encode())
        self.assertEqual([m[0] for m in groups.values()], ["mc2_01", "mc2_09"])
        with self.assertRaises(ValueError):
            smoke.campaign_groups("\n".join(blocks).replace("mc2_24", "mc2_23").encode())

    def test_stock_shadow_and_truncated_archive_are_rejected(self):
        with scratch_directory() as temp:
            deploy = Path(temp)
            entry = (0, 3, 3, "data/test.fit", False, b"abc")
            with patch.object(smoke, "fst_entries", return_value=[entry]):
                stock = smoke.StockInputs(deploy)
            self.assertEqual(stock.read("data/test.fit"), b"abc")
            loose = deploy / "data/test.fit"
            loose.parent.mkdir()
            loose.write_bytes(b"mod")
            with self.assertRaisesRegex(ValueError, "shadowed"):
                stock.read("data/test.fit")
            loose.unlink()
            stock.entries["data/test.fit"] = (0, 4, 4, "data/test.fit", False, b"abc")
            with self.assertRaisesRegex(ValueError, "length mismatch"):
                stock.read("data/test.fit")
            stock.entries["data/test.fit"] = (0, 3, 4, "data/test.fit", False, b"abc")
            with self.assertRaisesRegex(ValueError, "decompression failed"):
                stock.read("data/test.fit")

    def test_stale_pak_and_fit_deleted_on_each_retry_attempt(self):
        with scratch_directory() as temp:
            deploy = Path(temp)
            output = deploy / "terrain_gen_out/test.pak"
            output.parent.mkdir()
            output.write_bytes(b"stale")
            output.with_suffix(".fit").write_bytes(b"stale")
            with patch.object(smoke.nofocus, "spawn", side_effect=PermissionError("lease denied")):
                result = smoke.run_case("test", deploy / "Mission Editor.exe", deploy, 1, 2, ["-smoke-save=terrain_gen_out/test.pak"], "saved")
            self.assertEqual(result["bucket"], "launch_refused")
            self.assertFalse(output.exists())
            self.assertFalse(output.with_suffix(".fit").exists())

    def test_stale_intermediate_workflow_saves_cannot_prove_reload(self):
        with scratch_directory() as temp:
            fixtures = [temp / "terrain_gen_out" / name for name in ("e0_authored.pak", "e0_authored.fit", "e0_objectives.fit", "e0_campaign.fit")]
            fixtures[0].parent.mkdir()
            for file in fixtures:
                file.write_bytes(b"stale proof")
            with patch.object(smoke.nofocus, "spawn", side_effect=PermissionError("lease denied")):
                result = smoke.run_case("workflows", temp / "Mission Editor.exe", temp, 1, 2, ["-smoke-workflows"], "workflows")
            self.assertEqual(result["bucket"], "launch_refused")
            self.assertTrue(all(not file.exists() for file in fixtures))

    def test_successful_exit_without_workflow_facts_is_red(self):
        with scratch_directory() as temp:
            deploy = Path(temp)
            class Child:
                returncode = 0
                def communicate(self, timeout):
                    return "[ESMOKE v1] event=summary clean_exit=1 autoload=1 saved=1\n", None
            with patch.object(smoke.nofocus, "spawn", return_value=Child()):
                row = smoke.run_case("workflows", deploy / "Mission Editor.exe", deploy, 1, 2, [], "workflows")
            self.assertFalse(row["passed"])
            self.assertEqual(row["bucket"], "workflow_probe_failed")

    def test_stock_row_compares_all_24_and_records_findings(self):
        with scratch_directory() as temp:
            exe = temp / "Mission Editor.exe"
            exe.write_bytes(b"test double, never launched")
            original = b"[A]\nl Count = 1\n"
            ledger = temp / "ledger.md"
            calls = []
            def save(name, exe, deploy, seconds, timeout, flags, expect):
                calls.append((name, flags, expect))
                output = deploy / flags[1].split("=", 1)[1]
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(b"fresh PAK")
                data = original.replace(b"1", b"2") if name.endswith("mc2_01") else original
                output.with_suffix(".fit").write_bytes(data)
                return dict(name=name, passed=True, bucket="", esmoke={"saved": "1"})
            with patch.object(smoke, "StockInputs") as inputs, patch.object(smoke, "campaign_groups", return_value={0: list(smoke.STOCK_MISSIONS)}), patch.object(smoke, "run_case_with_retry", side_effect=save), patch.object(sys, "argv", [str(SCRIPT), "--exe", str(exe), "--case", "stock_roundtrip", "--out", str(temp / "evidence"), "--ledger", str(ledger), "--baseline", str(temp / "baseline.json"), "--rebaseline"]), patch("builtins.print"):
                inputs.return_value.read.return_value = original
                self.assertEqual(smoke.main(), 0)
            self.assertEqual(len(calls), 24)
            for (name, flags, expect), mission in zip(calls, smoke.STOCK_MISSIONS):
                self.assertEqual(name, f"stock_roundtrip:{mission}")
                self.assertEqual(flags[0], f"-mission=data/missions/{mission}.pak")
                self.assertEqual(expect, "saved")
            text = ledger.read_text(encoding="utf-8")
            self.assertEqual(text.count("| yes |"), 23)
            self.assertEqual(text.count("| no |"), 1)
            self.assertIn("0 blocked of 24", text)
            report = next((temp / "evidence").glob("*/report.md")).read_text(encoding="utf-8")
            self.assertIn("(24/24)", report)
            self.assertIn("Round-trip findings: 1", report)
            self.assertEqual(len(list((temp / "evidence").glob("*/mc2_*.json"))), 24)

    def test_save_fact_without_output_is_not_a_roundtrip(self):
        with scratch_directory() as temp:
            exe = temp / "Mission Editor.exe"
            exe.write_bytes(b"test double, never launched")
            ledger = temp / "ledger.md"
            def missing(name, *args):
                return dict(name=name, passed=True, bucket="", esmoke={"saved": "1"})
            with patch.object(smoke, "StockInputs") as inputs, patch.object(smoke, "campaign_groups", return_value={0: list(smoke.STOCK_MISSIONS)}), patch.object(smoke, "run_case_with_retry", side_effect=missing), patch.object(sys, "argv", [str(SCRIPT), "--exe", str(exe), "--case", "stock_roundtrip", "--out", str(temp / "evidence"), "--ledger", str(ledger)]), patch("builtins.print"):
                inputs.return_value.read.return_value = b"[A]\nl Count = 1\n"
                self.assertEqual(smoke.main(), 1)
            text = ledger.read_text(encoding="utf-8")
            self.assertEqual(text.count("| blocked |"), 24)
            self.assertIn("Exact baseline mismatch", text)

    def test_single_stock_case_runs_only_requested_mission(self):
        with scratch_directory() as temp:
            exe = temp / "Mission Editor.exe"
            exe.write_bytes(b"test double, never launched")
            original = b"[Cameras]\nf ZoomMax = 1.0\n"
            def save(name, exe, deploy, seconds, timeout, flags, expect):
                self.assertEqual(name, "stock_roundtrip:mc2_22")
                self.assertEqual(flags[0], "-mission=data/missions/mc2_22.pak")
                output = deploy / flags[1].split("=", 1)[1]
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(b"fresh PAK")
                output.with_suffix(".fit").write_bytes(original)
                return dict(name=name, passed=True, bucket="", esmoke={"saved": "1"})
            baseline = temp / "baseline.json"
            comparison = smoke.compare_fit(original, original)
            rows = [dict(name=f"stock_roundtrip:{m}", passed=True, roundtrip=comparison) for m in smoke.STOCK_MISSIONS]
            smoke.gate_baseline(rows, baseline, True, {"art.fst": "stock"})
            with patch.object(smoke, "StockInputs") as inputs, patch.object(smoke, "campaign_groups", return_value={0: list(smoke.STOCK_MISSIONS)}), patch.object(smoke, "run_case_with_retry", side_effect=save) as run, patch.object(sys, "argv", [str(SCRIPT), "--exe", str(exe), "--case", "stock_roundtrip:mc2_22", "--out", str(temp / "evidence"), "--baseline", str(baseline)]), patch("builtins.print"):
                inputs.return_value.read.return_value = original
                self.assertEqual(smoke.main(), 0)
                self.assertEqual(run.call_count, 1)

    def test_model_only_cannot_claim_panel_reach_and_each_step_is_required(self):
        lines = panel_lines()
        self.assertTrue(smoke.grade_workflows(lines.replace("coverage=model-only reached=0", "coverage=action-handler reached=1")))
        for field in ("authored", "undo", "saved", "reloaded"):
            self.assertTrue(smoke.grade_workflows(lines.replace(f"{field}=1", f"{field}=0")))

    def test_baseline_rejects_writer_input_outcome_missing_and_archive_changes(self):
        with scratch_directory() as temp:
            baseline = temp / "baseline.json"
            comparison = smoke.compare_fit(b"input", b"output")
            rows = [dict(name=f"stock_roundtrip:{m}", passed=True, roundtrip=comparison.copy()) for m in smoke.STOCK_MISSIONS]
            with patch("builtins.print") as printed:
                smoke.gate_baseline(rows, baseline, True, {"art.fst": "stock"})
            self.assertEqual(printed.call_count, 25)
            for changed in (dict(input_sha256="changed"), dict(output_sha256="changed"), dict(byte_identical=True), None):
                row = dict(name="stock_roundtrip:mc2_01", passed=True)
                if changed is not None:
                    row["roundtrip"] = comparison | changed
                smoke.gate_baseline([row], baseline, False, {"art.fst": "stock"})
                self.assertFalse(row["passed"])
                self.assertEqual(row["result_label"], "FAIL")
            row = dict(name="stock_roundtrip:mc2_01", passed=True, roundtrip=comparison)
            smoke.gate_baseline([row], baseline, False, {"art.fst": "mod"})
            self.assertFalse(row["passed"])
            with self.assertRaisesRegex(ValueError, "all 24"):
                smoke.gate_baseline([row], baseline, True, {})
            data = json.loads(baseline.read_text())
            del data["missions"]["mc2_24"]
            baseline.write_text(json.dumps(data))
            row = dict(name="stock_roundtrip:mc2_01", passed=True, roundtrip=comparison)
            smoke.gate_baseline([row], baseline, False, {"art.fst": "stock"})
            self.assertFalse(row["passed"])

    def test_dependency_audit_rejects_catalog_shadow_and_unknown_metadata(self):
        self.audit.stop()
        with scratch_directory() as temp:
            (temp / "system.cfg").write_bytes(b'[FastFiles]\nl NumFastFiles = 1\nst File0 = "art.fst"\n')
            (temp / "art.fst").write_bytes(b"archive")
            loose = temp / "data/art/Buildings.csv"
            loose.parent.mkdir(parents=True)
            loose.write_bytes(b"stock")
            entry = (0, 5, 5, "data/art/Buildings.csv", False, b"stock")
            destination = temp / "dependencies.json"
            with patch.object(smoke, "fst_entries", return_value=[entry]):
                facts = smoke.dependency_manifest(temp, destination)
                self.assertTrue(facts["loose_files"][0]["stock"])
                loose.write_bytes(b"MECH mod")
                with self.assertRaisesRegex(ValueError, "non-stock loose"):
                    smoke.dependency_manifest(temp, destination)
                facts = json.loads(destination.read_text())
                self.assertEqual(facts["violations"], ["data/art/buildings.csv"])
                self.assertEqual(facts["loose_files"][0]["archive_entry_sha256"], smoke.hashlib.sha256(b"stock").hexdigest())
                digest = smoke.hashlib.sha256(b"MECH mod").hexdigest()
                with self.assertRaises(ValueError):
                    smoke.dependency_manifest(temp, destination, {"data/art/buildings.csv": dict(sha256=digest, reason="")})
                facts = smoke.dependency_manifest(temp, destination, {"data/art/buildings.csv": dict(sha256=digest, reason="test explicit waiver")})
                self.assertEqual(facts["loose_files"][0]["reason"], "test explicit waiver")
                loose.write_bytes(b"changed again")
                with self.assertRaises(ValueError):
                    smoke.dependency_manifest(temp, destination, {"data/art/buildings.csv": dict(sha256=digest, reason="stale waiver")})
                loose.write_bytes(b"stock")
                (loose.parent / "mod.ini").write_bytes(b"mod")
                with self.assertRaisesRegex(ValueError, "mod.ini"):
                    smoke.dependency_manifest(temp, destination)

    def test_stock_preparation_is_scratch_only_and_records_overlay_removal(self):
        with scratch_directory() as temp:
            (temp / "system.cfg").write_bytes(b'[FastFiles]\nl NumFastFiles = 1\nst File0 = "art.fst"\n')
            (temp / "art.fst").write_bytes(b"archive")
            catalog = temp / "data/art/buildings.csv"
            catalog.parent.mkdir(parents=True)
            catalog.write_bytes(b"mod catalog")
            cache = temp / "data/cache.txm"
            cache.write_bytes(b"old cache")
            allowed = temp / "data/stock-derived.pak"
            allowed.write_bytes(b"derived")
            entry = (0, 5, 5, "data/art/buildings.csv", False, b"stock")
            allowance = {"data/stock-derived.pak": dict(sha256=smoke.hashlib.sha256(b"derived").hexdigest(), reason="stock packets")}
            with patch.object(smoke, "fst_entries", return_value=[entry]), patch("builtins.print"):
                smoke.prepare_stock_scratch(temp, temp / "preparation.json", allowance)
            self.assertEqual(catalog.read_bytes(), b"stock")
            self.assertFalse(cache.exists())
            self.assertEqual(allowed.read_bytes(), b"derived")
            changes = json.loads((temp / "preparation.json").read_text())["changes"]
            self.assertEqual({c["action"] for c in changes}, {"remove-nonstock-loose", "replace-from-stock"})
        with self.assertRaisesRegex(ValueError, "restricted"):
            smoke.prepare_stock_scratch(smoke.REPO.parent / "mc2-opengl/mc2-win64-v0.4c", smoke.REPO / "unused.json", {})

    def test_malformed_baseline_is_rejected_with_a_diagnostic(self):
        with scratch_directory() as temp:
            file = temp / "baseline.json"
            file.write_text("[]")
            with self.assertRaisesRegex(ValueError, "JSON object"):
                smoke.read_json_object(file)
            file.write_text('{"missions": []}')
            with self.assertRaisesRegex(ValueError, "invalid baseline missions"):
                smoke.gate_baseline([dict(name="stock_roundtrip:mc2_01", passed=True)], file, False, {})


if __name__ == "__main__":
    unittest.main()
