"""Negative controls for E0: absent facts and stale outputs cannot become proof."""
import importlib.util
from pathlib import Path
import sys
import json
import re
import struct
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


class ShadowEditGradingTests(unittest.TestCase):
    def test_population_requires_unique_actors_in_the_live_union_fit(self):
        proof="[EDITOR_CLI v1] event=mover_population count=100 ok=1 frame=120\n[VK_SHADOW_ISLAND] movers actors_feed=110 actors_visible=100 rigid_leaves=3 map=4096 format=D16"
        self.assertEqual(smoke.grade_mover_population(proof),"")
        self.assertTrue(smoke.grade_mover_population(proof.replace("count=100","count=99")))
        self.assertTrue(smoke.grade_mover_population(proof.replace("actors_visible=100","actors_visible=99")))
        self.assertTrue(smoke.grade_mover_population(proof.replace("format=D16","format=D32")))

    def test_each_edit_must_reach_and_drain_the_renderer(self):
        lines = []
        for stage in range(6):
            if stage >= 3 and stage % 2 == 1:
                lines.append("[EDITOR_CLI v1] event=shadow_delete recipe=7 tracked_recipe=7 undo_index=0 found=1")
            lines.append(f"[EDITOR_CLI v1] event=shadow_edit stage={stage} ok=1 frame={240 * (stage + 1)}")
            if stage >= 2:
                lines.append(f"[SHADOW_BAKE] caster event: {'added' if stage % 2 == 0 else 'removed'} leaves 1")
            lines.append("[SHADOW_BAKE] dirty queue drained: sub_tiles=4")
        proof = "\n".join(lines)
        self.assertEqual(smoke.grade_shadow_edits(proof), "")
        for line in lines:
            self.assertTrue(smoke.grade_shadow_edits(proof.replace(line, "", 1)))
        self.assertTrue(smoke.grade_shadow_edits(proof.replace("ok=1", "ok=0", 1)))
        self.assertTrue(smoke.grade_shadow_edits(proof + "\n" + lines[0]))
        self.assertTrue(smoke.grade_shadow_edits(proof.replace("tracked_recipe=7", "tracked_recipe=8")))
        self.assertTrue(smoke.grade_shadow_edits(proof.replace("caster event: added leaves 1", "caster event: added leaves 1\n[SHADOW_BAKE] caster event: added leaves 1")))


class ObjectiveScalarGradingTests(unittest.TestCase):
    def test_legacy_reload_grades_values_and_rejects_noop_or_wrong_resave(self):
        from editor.tests.legacy_reference import FROZEN
        before = (FROZEN / "e5_reference_before.fit").read_bytes()
        start = before.index(b"[Team0Objective1]")
        end = before.index(b"\n[", start)
        before = before[:start] + before[start:end].replace(b"ModelType = 0", b"ModelType = 14") + before[end:]
        end = before.index(b"\n[", start)
        changed = before[:start] + before[start:end].replace(b"ResourcePoints = 7500", b"ResourcePoints = 17") + before[end:]
        self.assertNotEqual(changed, before)
        with scratch_directory() as directory:
            source = directory / "source"
            (directory / "fixture.fit").write_bytes(before)
            (directory / "input.fit").write_bytes(before)
            source.with_suffix(".fit").write_bytes(changed)
            pak = (FROZEN / "e5_reference_before.pak").read_bytes()
            source.with_suffix(".pak").write_bytes(pak)
            for name in ("fixture.pak", "input.pak"):
                (directory / name).write_bytes(pak)
            import hashlib
            (directory / "gate.json").write_text(json.dumps(dict(result=dict(name="objectives_panel", passed=True),
                provenance=dict(input_pak_sha256=hashlib.sha256(pak).hexdigest(), input_fit_sha256=hashlib.sha256(before).hexdigest()))))
            for index, output in enumerate((changed, before, changed.replace(b"ResourcePoints = 17", b"ResourcePoints = 18"), None)):
                deploy = directory / str(index)
                report = deploy / "report"
                report.mkdir(parents=True)
                if output is None:
                    stale = deploy / "terrain_gen_out/owned_objective_reload"
                    stale.mkdir(parents=True)
                    (stale / "reloaded.fit").write_bytes(changed)
                    (stale / "reloaded.pak").write_bytes(pak)
                def resave(*args):
                    target = deploy / "terrain_gen_out/owned_objective_reload"
                    if output is None:
                        self.assertFalse((target / "reloaded.fit").exists())
                        self.assertFalse((target / "reloaded.pak").exists())
                        return dict(passed=True, name="owned_objective_reload")
                    (target / "reloaded.fit").write_bytes(output)
                    (target / "reloaded.pak").write_bytes(source.with_suffix(".pak").read_bytes())
                    return dict(passed=True, name="owned_objective_reload")
                with patch.object(smoke, "run_case_with_retry", side_effect=resave) as launch:
                    row = smoke.run_owned_objective_reload(deploy / "fake.exe", deploy, source, report, 3, 30)
                    self.assertEqual(row["passed"], index == 0)
                    launch.assert_called_once()

    def test_independent_scalar_grader_rejects_wrong_noop_and_unrelated_changes(self):
        from editor.tests.objective_census import grade_objective_edit
        before = (b"FITini\r\n[Team0Objective0]\r\nul ResourcePoints = 10\r\n"
                  b"[Team0Objective1]\r\nul ResourcePoints = 7500 // original comment\r\nst Title = \"second\"\r\n"
                  b"[Team0Objective1Action0]\r\nst ActionSpeciesString = \"Vendor\"\r\nx Opaque = KEEP\r\nFITend\r\n")
        after = before.replace(b"7500", b"17")
        grade_objective_edit(before, after, "ResourcePoints")
        for wrong in (before, after.replace(b"17", b"18"), after.replace(b"KEEP", b"LOST"), after.replace(b"= 10", b"= 11"), after.replace(b"original comment", b"changed comment")):
            with self.subTest(wrong=wrong), self.assertRaises(ValueError):
                grade_objective_edit(before, wrong, "ResourcePoints")

    def test_census_records_spaced_keys_and_all_four_lists(self):
        from editor.tests.objective_census import objective_fields
        parsed = objective_fields(b"[Team0Objective1]\nul ModelID Version = 2\nul NumFailureActions = 1\n"
                                  b"[Team0Objective1FailureAction0]\nst FailureActionSpeciesString = \"Vendor\"\n")
        self.assertEqual(parsed["Team0Objective1/ModelID Version"], ["ul 2"])
        self.assertEqual(parsed["Team0Objective1/NumFailureActions"], ["ul 1"])
        self.assertEqual(parsed["Team0Objective1FailureAction0/FailureActionSpeciesString"], ['st "Vendor"'])

    def test_multiline_widget_grader_matches_reader_and_rejects_literal_escape_or_lost_break(self):
        from editor.tests.objective_census import grade_objective_edit, objective_fields
        before = b'FITini\r\n[Team0Objective1]\r\nst Title = "old" // keep\r\nFITend\r\n'
        wire = b'"\r\nE6a objective\r\n\r\nsecond line\r\n"'
        edited = before.replace(b'"old"', wire)
        self.assertEqual(objective_fields(edited)["Team0Objective1/Title"], ['st ' + wire.decode()])
        grade_objective_edit(before, edited, "Title")
        for wrong in (before, edited.replace(b'"\r\nE6a', b'"E6a'),
                      before.replace(b'"old"', b'"E6a objective\\n\\nsecond line\\n"'),
                      edited.replace(b'// keep', b'// changed'), edited.replace(b'st Title', b'x Title')):
            with self.subTest(wrong=wrong), self.assertRaises(ValueError):
                grade_objective_edit(before, wrong, "Title")


@contextmanager
def scratch_directory():
    path = TEMP_ROOT / uuid.uuid4().hex
    path.mkdir()
    yield path


def panel_lines():
    panels = "\n".join(f"[EDITOR_PANEL v1] workflow={w} panel={p} coverage={'model-only' if p in smoke.MODEL_ONLY else 'action-handler'} reached={0 if p in smoke.MODEL_ONLY else 1} ok=1 authored=1 undo=1 saved=1 reloaded=1"
                     for p, w in smoke.WORKFLOW_PANELS.items())
    definitions = "[EDITOR_DEFINITIONS v1] stage=duplicate_unit_refresh_undo_redo ok=1\n[EDITOR_DEFINITIONS v1] stage=script_boundary ok=1\n[EDITOR_DEFINITIONS v1] stage=display_equality ok=1\n[EDITOR_DEFINITIONS v1] preservation_commands_undo_reload_malformed=1"
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
    definitions += "\n[EDITOR_DEFINITIONS v1] stage=reader_budget rejected=1 unchanged=1 ok=1"
    for field, value in (("ModelScale", "0"), ("ModelScale", "-1"), ("ModelScale", "1e100"),
                         ("ModelScale", "1garbage"), ("ModelType", "256"), ("Num", "1garbage")):
        definitions += f"\n[EDITOR_DEFINITIONS v1] stage=candidate_rejection field={field} value={value} rejected=1 unchanged=1 history=1 ok=1"
        reason = "condition_read_failed" if field == "Num" else "malformed_value"
        definitions += f"\n[EDITOR_DOCUMENT v1] mission=terrain_gen_out/cq_candidate_{field}_{value}.fit field=Team0Objective0/{field} reason={reason} rejected=1"
    definitions += "\n[EDITOR_DOCUMENT v1] mission=terrain_gen_out/ew_reader_budget.fit length=16777217 limit=16777216 reason=document_too_large rejected=1"
    for case in ("ObjectiveModel", "ModelType", "ModelScale", "all"):
        flags = ' '.join(f"{key}={int(case in (key, 'all'))}" for key in ("ObjectiveModel", "ModelType", "ModelScale"))
        definitions += f"\n[EDITOR_DEFINITIONS v1] stage=model_default case={case} ok=1"
        definitions += f"\n[EDITOR_DOCUMENT v1] mission=terrain_gen_out/ew_model_default_{case}.fit block=Probe defaulted=1 {flags}"
    for case in ("Condition", "FailureCondition"):
        definitions += f"\n[EDITOR_DEFINITIONS v1] stage=condition_failure case={case} ok=1"
        definitions += f"\n[EDITOR_DOCUMENT v1] mission=terrain_gen_out/ew_condition_failure_{case}.fit block=Team0Objective0{case}0 species=DestroyNumberOfEnemyUnits reason=condition_read_failed rejected=1"
    return panels + "\n" + definitions


class EditorSmokeTests(unittest.TestCase):
    def test_candidate_rejection_requires_executable_history_and_single_diagnostic(self):
        lines = panel_lines()
        self.assertEqual(smoke.grade_workflows(lines), "")
        for changed in (lines.replace("history=1", "history=0"),
                        lines.replace("stage=candidate_rejection", "stage=old_rejection"),
                        lines + "\n[EDITOR_DOCUMENT v1] mission=terrain_gen_out/cq_candidate_ModelScale_0.fit reason=malformed_value rejected=1"):
            with self.subTest(changed=changed):
                self.assertTrue(smoke.grade_workflows(changed))
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
                    if " length=" in line and " limit=" in line:
                        self.assertTrue(smoke.grade_workflows(lines.replace(line, line.replace(" length=", " missing_length="))))
                        self.assertTrue(smoke.grade_workflows(lines.replace(line, line.replace(" limit=", " missing_limit="))))

    def test_merged_reader_review_requires_defaults_context_and_case_sensitive_edits(self):
        lines = panel_lines()
        for changed in (lines.replace("display_equality ok=1", "display_equality ok=0"),
                        lines.replace("defaulted=1", "defaulted=0"),
                        lines.replace("ObjectiveModel=1", "ObjectiveModel=0"),
                        lines.replace("species=DestroyNumberOfEnemyUnits", "species=unknown"),
                        lines.replace("block=Team0Objective0FailureCondition0", "block=unknown"),
                        lines.replace("limit=16777216", "limit=16777215")):
            with self.subTest(changed=changed):
                self.assertTrue(smoke.grade_workflows(changed))

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
        self.assertEqual(len(set(smoke.WORKFLOW_PANELS.values())), 8)

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
        with self.assertRaisesRegex(ValueError, "restricted"):  # a lane install is an install too
            smoke.prepare_stock_scratch(Path("A:/Games/mc2-opengl/mc2-win64-lane2"), smoke.REPO / "unused.json", {})

    def test_clone_for_stock_scratch_leaves_the_install_untouched(self):
        with scratch_directory() as temp:
            install = temp / "install"
            (install / "data/art").mkdir(parents=True)
            (install / "data/art/x.csv").write_bytes(b"mod")
            (install / "mc2.exe").write_bytes(b"exe")
            (install / "mc2.pdb").write_bytes(b"pdb")
            (install / "shaders").mkdir()
            (install / "shaders/a.glsl").write_bytes(b"s")
            with patch.object(smoke, "REPO", temp / "repo"):
                clone = smoke.clone_install_for_stock_scratch(install)
            self.assertTrue((clone / "mc2.exe").exists())
            self.assertTrue((clone / "shaders/a.glsl").exists())
            self.assertFalse((clone / "mc2.pdb").exists())
            self.assertEqual(list((clone / "data").iterdir()), [])
            self.assertEqual((install / "data/art/x.csv").read_bytes(), b"mod")

    def test_malformed_baseline_is_rejected_with_a_diagnostic(self):
        with scratch_directory() as temp:
            file = temp / "baseline.json"
            file.write_text("[]")
            with self.assertRaisesRegex(ValueError, "JSON object"):
                smoke.read_json_object(file)
            file.write_text('{"missions": []}')
            with self.assertRaisesRegex(ValueError, "invalid baseline missions"):
                smoke.gate_baseline([dict(name="stock_roundtrip:mc2_01", passed=True)], file, False, {})


class NewModelGateTests(unittest.TestCase):
    def setUp(self):
        self.directory = TEMP_ROOT / ("new-model-" + uuid.uuid4().hex)
        self.directory.mkdir()
        (self.directory / "input").mkdir()
        self.rows = []
        for mission in smoke.STOCK_MISSIONS:
            original = b'FITini\n[MissionSettings]\nst MissionName = "original"\n[Tail]\nFITend\n'
            edited = original.replace(b'"original"', b'"Q3 authored mission"')
            packet = b"original packet encoding"
            output = self.directory / mission
            output.mkdir()
            for ext, data in (("fit", original), ("pak", packet)):
                (self.directory / "input" / f"{mission}.{ext}").write_bytes(data)
                for stage in ("roundtrip", "undone", "edited", "long_name"):
                    written = edited if stage == "edited" and ext == "fit" else data
                    if stage == "long_name" and ext == "fit":
                        written = original.replace(b'"original"', b'"Q3 long mission ' + b'x' * 8192 + b' retained tail"')
                    (output / f"{stage}.{ext}").write_bytes(written)
            (output / "shell.log").write_text("[EDITOR_MODEL v1] mission=malformed.fit field=syntax reason=embedded_nul rejected=1\n")
            self.rows.append(dict(mission=mission, status="PASS", rc=0,
                input_fit_sha256=smoke.hashlib.sha256(original).hexdigest(),
                input_pak_sha256=smoke.hashlib.sha256(packet).hexdigest(),
                facts=dict(result="PASS", frames="3", **{key: "1" for key in
                    ("roundtrip", "edited", "reopened", "undo", "redo", "malformed", "unchanged", "long_name")})))

    def test_truncated_long_name_fails(self):
        path = self.directory / "mc2_01/long_name.fit"
        path.write_bytes(path.read_bytes().replace(b'x' * 8192, b'x' * 2000))
        with self.assertRaisesRegex(ValueError, "long name truncated"):
            smoke.grade_new_model(self.rows, self.directory)

    def test_exact_24_fit_and_pak_pass(self):
        smoke.grade_new_model(self.rows, self.directory)

    def test_missing_extra_duplicate_skipped_blocked_and_finding_fail(self):
        for rows in (self.rows[:-1], self.rows + [self.rows[0]], self.rows[:-1] + [self.rows[0]]):
            with self.assertRaises(ValueError):
                smoke.grade_new_model(rows, self.directory)
        for status in ("SKIP", "BLOCKED", "FINDING", "FLAKY_PASS", "TIMEOUT"):
            with self.subTest(status=status), self.assertRaises(ValueError):
                rows = [dict(row) for row in self.rows]
                rows[0]["status"] = status
                smoke.grade_new_model(rows, self.directory)

    def test_corrupted_fit_pak_or_undo_output_fails(self):
        for stage, ext in (("roundtrip", "fit"), ("roundtrip", "pak"), ("undone", "fit"), ("undone", "pak")):
            path = self.directory / "mc2_01" / f"{stage}.{ext}"
            original = path.read_bytes()
            path.write_bytes(original + b"corrupt")
            with self.subTest(stage=stage, ext=ext), self.assertRaisesRegex(ValueError, "byte mismatch"):
                smoke.grade_new_model(self.rows, self.directory)
            path.write_bytes(original)

    def test_missing_fact_or_duplicate_malformed_diagnostic_fails(self):
        for key in self.rows[0]["facts"]:
            rows = [dict(row, facts=dict(row["facts"])) for row in self.rows]
            del rows[0]["facts"][key]
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "facts missing"):
                smoke.grade_new_model(rows, self.directory)
        path = self.directory / "mc2_01/shell.log"
        path.write_text(path.read_text() * 2)
        with self.assertRaisesRegex(ValueError, "diagnostic missing"):
            smoke.grade_new_model(self.rows, self.directory)

    def test_changed_input_and_absent_output_fail(self):
        path = self.directory / "input/mc2_01.fit"
        original = path.read_bytes()
        path.write_bytes(original + b"changed")
        with self.assertRaisesRegex(ValueError, "input changed"):
            smoke.grade_new_model(self.rows, self.directory)
        path.write_bytes(original)
        path = self.directory / "mc2_01/roundtrip.pak"
        path.rename(path.with_suffix(".retained"))
        with self.assertRaises(FileNotFoundError):
            smoke.grade_new_model(self.rows, self.directory)


def crud_edit_fixture(original, operation):
    """Synthetic saves exercise the Python gate without invoking a native binary."""
    from editor.tests.objective_crud import blocks, NEW_FIELDS
    prefix, sections, ending = blocks(original)
    if operation is None or operation.startswith("entry-"):
        suffixes = ("Condition", "Action", "FailureCondition", "FailureAction")
        owner = "Team0Objective1"
        if operation is None:
            for index, suffix in enumerate(suffixes):
                key = "Num" + suffix + "s"
                sections[owner] = re.sub(rb"(ul " + key.encode() + rb" = )\d+", rb"\g<1>2", sections[owner])
                for name in list(sections):
                    if re.fullmatch(owner + suffix + r"\d+", name):
                        del sections[name]
                species = "BooleanFlagIsSet" if index % 2 == 0 else "SetBooleanFlag"
                for number, flag in enumerate(("first", "second")):
                    sections[f"{owner}{suffix}{number}"] = (
                        f'\r\nst {suffix}SpeciesString = "{species}"\r\nst FlagID = "e6c-{flag}"\r\nb Value = {"TRUE" if number else "FALSE"}\r\n').encode()
        else:
            _, index, action = operation.split("-")
            suffix = suffixes[int(index)]
            base = owner + suffix
            count = 2
            if action == "add":
                count = 3
                species = "BooleanFlagIsSet" if int(index) % 2 == 0 else "SetBooleanFlag"
                flag = "created-flag" if int(index) % 2 else ""  # f82f77ee8: action creation carries its payload
                sections[base + "2"] = f'\r\nst {suffix}SpeciesString = "{species}"\r\nst FlagID = "{flag}"\r\nb Value = TRUE\r\n'.encode()
            elif action == "delete":
                count = 1
                sections[base + "0"] = sections.pop(base + "1")
            elif action in ("up", "down"):
                sections[base + "0"], sections[base + "1"] = sections[base + "1"], sections[base + "0"]
            elif action == "flag":
                sections[base + "0"] = sections[base + "0"].replace(b'e6c-first', f'e6c-edited-{index}'.encode()).replace(b'Value = FALSE', b'Value = TRUE')
            else:
                raise ValueError(operation)
            sections[owner] = sections[owner].replace(f"Num{suffix}s = 2".encode(), f"Num{suffix}s = {count}".encode())
        return prefix + b"".join(b"[" + name.encode() + b"]" + body for name, body in sections.items()) + ending
    count = int(re.search(rb"NumObjectives = (\d+)", sections["Team0Objectives"])[1])
    output = []
    copies = []
    for name, body in sections.items():
        match = re.fullmatch(r"Team0Objective(\d+)(\D.*)?", name)
        if match:
            index = int(match[1]); suffix = match[2] or ""
            if operation == "objective-copy":
                if index == 1:
                    copies.append(("Team0Objective2" + suffix, body))
                if index >= 2:
                    index += 1
            elif operation == "objective-delete":
                if index == 1:
                    continue
                if index > 1:
                    index -= 1
            elif operation == "objective-up" and index in (0, 1):
                index = 1 - index
            elif operation == "objective-down" and index in (1, 2):
                index = 3 - index
            name = f"Team0Objective{index}{suffix}"
        if name == "Team0Objectives":
            delta = 1 if operation in ("objective-add", "objective-copy") else -1 if operation == "objective-delete" else 0
            body = body.replace(f"NumObjectives = {count}".encode(), f"NumObjectives = {count + delta}".encode())
        output.append((name, body))
    if operation == "objective-add":
        body = b"\r\n" + b"".join((value[0].split(" ", 1)[0] + " " + key + " = " + value[0].split(" ", 1)[1] + "\r\n").encode()
                                  for key, value in NEW_FIELDS.items())
        output.append((f"Team0Objective{count}", body))
    return prefix + b"".join(b"[" + name.encode() + b"]" + body for name, body in output + copies) + ending


class ObjectiveCrudGradingTests(unittest.TestCase):
    def test_each_operation_rejects_consistently_wrong_noop_missing_and_corrupt_outputs(self):
        directory = PanelGateTests().fixture("objectives_crud_panel")
        smoke.grade_panel("objectives_crud_panel", directory, 0)
        smoke.crud_negative_controls(directory)
        smoke.grade_panel("objectives_crud_panel", directory, 0)
        from editor.tests.objective_crud import grade_crud_edit
        original = (directory / "fixture.fit").read_bytes()
        added = (directory / "objective-add-edited.fit").read_bytes()
        start = added.index(b"[Team0Objective9]")
        for invalid in (b"ul Priority = 1\r\n", b"invalid garbage\r\n", b"x MissingType = raw\r\n"):
            broken = added[:start] + added[start:].replace(b"ul Priority = 1\r\n", b"ul Priority = 1\r\n" + invalid, 1)
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                grade_crud_edit(original, broken, "objective-add")

    def test_reload_requires_both_panel_inputs_and_expected_counts_order_content(self):
        import hashlib
        directory = PanelGateTests().fixture("objectives_crud_panel")
        source = directory / "objective-copy-edited"
        for extension in ("fit", "pak"):
            (directory / f"input.{extension}").write_bytes((directory / f"fixture.{extension}").read_bytes())
        gate = dict(result=dict(name="objectives_crud_panel", passed=True), provenance={
            f"input_{ext}_sha256": hashlib.sha256((directory / f"input.{ext}").read_bytes()).hexdigest() for ext in ("fit", "pak")})
        (directory / "gate.json").write_text(json.dumps(gate))
        report = directory / "report"; report.mkdir()
        expected = source.with_suffix(".fit").read_bytes()
        for output in (expected, (directory / "fixture.fit").read_bytes(), expected.replace(b"NumObjectives = 10", b"NumObjectives = 11", 1)):
            def resave(*args):
                target = directory / "terrain_gen_out/owned_objective_crud_reload"
                null = args[0].endswith("_null")
                (target / ("null-reloaded.fit" if null else "reloaded.fit")).write_bytes((directory / "fixture.fit").read_bytes() if null else output)
                (target / ("null-reloaded.pak" if null else "reloaded.pak")).write_bytes(source.with_suffix(".pak").read_bytes())
                return dict(passed=True, name="owned_objective_crud_reload")
            with patch.object(smoke, "run_case_with_retry", side_effect=resave) as launch:
                row = smoke.run_owned_objective_reload(directory / "fake.exe", directory, source, report, 3, 30, crud=True)
                self.assertEqual(row["passed"], output == expected); self.assertEqual(launch.call_count, 2)
                self.assertEqual(launch.call_args_list[0].args[1], launch.call_args_list[1].args[1])
        from editor.tests.legacy_reference import packet, buildings
        from collections import Counter
        original_pak = source.with_suffix(".pak").read_bytes()
        table = struct.unpack_from("<I", original_pak, 4)[0]
        packet_count = table // 4 - 2

        def repacked_pak(changed=False):
            data = bytearray(original_pak[:table])
            for index in range(packet_count):
                struct.pack_into("<I", data, 8 + 4 * index, len(data))
                decoded = packet(original_pak, index)
                if index == 1:
                    decoded = bytearray(decoded)
                    struct.pack_into("<I", decoded, 8, struct.unpack_from("<I", decoded, 8)[0] ^ 1)
                    if changed:
                        decoded = decoded[:-1] + bytes([decoded[-1] ^ 1])
                    else:
                        records = [decoded[offset:offset + 40] for offset in range(4, len(decoded), 40)]
                        decoded = decoded[:4] + b"".join(reversed(records))
                if index == packet_count - 1:
                    decoded = uuid.uuid4().bytes_le
                data.extend(decoded)
            return bytes(data)

        null_pak = repacked_pak()
        self.assertNotEqual(Counter(buildings(original_pak)), Counter(buildings(null_pak)))
        for label, saved_pak, accepted in (("repacked", repacked_pak(), True),
                                           ("corrupted", b"CORRUPTED PAK", False),
                                           ("changed-record", repacked_pak(changed=True), False),
                                           ("missing", None, False)):
            target = directory / "terrain_gen_out/owned_objective_crud_reload/reloaded.pak"

            def resave_pak(*args):
                if args[0].endswith("_null"):
                    target.with_name("null-reloaded.fit").write_bytes((directory / "fixture.fit").read_bytes())
                    target.with_name("null-reloaded.pak").write_bytes(null_pak)
                    return dict(passed=True, name="owned_objective_crud_reload_null")
                target.with_suffix(".fit").write_bytes(expected)
                if saved_pak is None:
                    self.assertFalse(target.exists())
                else:
                    target.write_bytes(saved_pak)
                return dict(passed=True, name="owned_objective_crud_reload")

            with self.subTest(reloaded_pak=label), patch.object(smoke, "run_case_with_retry", side_effect=resave_pak) as launch:
                row = smoke.run_owned_objective_reload(directory / "fake.exe", directory, source, report, 3, 30, crud=True)
                self.assertEqual(row["passed"], accepted)
                if not accepted:
                    self.assertEqual(row["bucket"], "owned_objective_reload_changed")
                self.assertEqual(launch.call_count, 2)
        with patch.object(smoke, "run_case_with_retry", return_value=dict(passed=False)) as launch:
            row = smoke.run_owned_objective_reload(directory / "fake.exe", directory, source, report, 3, 30, crud=True)
            self.assertFalse(row["passed"]); self.assertEqual(row["bucket"], "owned_objective_null_reload_failed")
            launch.assert_called_once()
        for stale_stem in ("null-reloaded", "reloaded"):
            target = directory / "terrain_gen_out/owned_objective_crud_reload"
            for stem in ("null-reloaded", "reloaded"):
                (target / f"{stem}.fit").write_bytes((directory / "fixture.fit").read_bytes() if stem == "null-reloaded" else expected)
                (target / f"{stem}.pak").write_bytes(null_pak)

            def no_save(*args):
                null = args[0].endswith("_null")
                stem = "null-reloaded" if null else "reloaded"
                self.assertFalse((target / f"{stem}.fit").exists())
                self.assertFalse((target / f"{stem}.pak").exists())
                if null and stale_stem == "reloaded":
                    (target / "null-reloaded.fit").write_bytes((directory / "fixture.fit").read_bytes())
                    (target / "null-reloaded.pak").write_bytes(null_pak)
                return dict(passed=True)

            with self.subTest(stale=stale_stem), patch.object(smoke, "run_case_with_retry", side_effect=no_save) as launch:
                row = smoke.run_owned_objective_reload(directory / "fake.exe", directory, source, report, 3, 30, crud=True)
                self.assertFalse(row["passed"])
                self.assertEqual(launch.call_count, 1 if stale_stem == "null-reloaded" else 2)
        for name in ("fixture.fit", "fixture.pak", "input.fit", "input.pak", "objective-copy-edited.fit", "objective-copy-edited.pak", "gate.json"):
            path = directory / name; original = path.read_bytes()
            path.write_bytes(original + b"corrupt")
            with self.subTest(name=name), patch.object(smoke, "run_case_with_retry") as launch:
                with self.assertRaises((ValueError, OSError)):
                    smoke.run_owned_objective_reload(directory / "fake.exe", directory, source, report, 3, 30, crud=True)
                launch.assert_not_called()
            path.write_bytes(original)
            retained = path.with_suffix(path.suffix + ".retained")
            path.rename(retained)
            try:
                with self.subTest(missing=name), patch.object(smoke, "run_case_with_retry") as launch:
                    with self.assertRaises((ValueError, OSError)):
                        smoke.run_owned_objective_reload(directory / "fake.exe", directory, source, report, 3, 30, crud=True)
                    launch.assert_not_called()
            finally:
                retained.rename(path)


def objective_panel_fixture(original):
    from editor.tests.objective_crud import blocks
    from editor.tests.objective_census import ACTION_PAYLOADS
    prefix, sections, ending = blocks(crud_edit_fixture(original, "objective-add"))
    owner = "Team0Objective1"
    for name in list(sections):
        if re.fullmatch(owner + r"(?:Condition|Action|FailureAction)\d+", name):
            del sections[name]
    for key, count in (("NumConditions", 3), ("NumActions", 5), ("NumFailureActions", 5)):
        sections[owner] = re.sub(rb"(ul " + key.encode() + rb" = )\d+", lambda m: m[1] + str(count).encode(), sections[owner])
    fields = smoke.fit_fields(original)
    x = fields["Part1/PositionX"][0].split(" ", 1)[1]
    y = fields["Part1/PositionY"][0].split(" ", 1)[1]
    for index in range(3):
        sections[f"{owner}Condition{index}"] = (f'\r\nst ConditionSpeciesString = "DestroySpecificEnemyUnit"\r\nf PositionX = {x if index else "-1e30"}\r\nf PositionY = {y if index else "-1e30"}\r\n').encode()
    for suffix in ("Action", "FailureAction"):
        for index, (species, key) in enumerate(ACTION_PAYLOADS):
            sections[f"{owner}{suffix}{index}"] = (f'\r\nst {suffix}SpeciesString = "{species}"\r\n' +
                (f'ul {key} = 7\r\n' if key == "ResourceStringID" else f'st {key} = "original"\r\n')).encode()
    for suffix in ("Condition", "FailureCondition"):
        sections["Team0Objective9"] = sections["Team0Objective9"].replace(f"Num{suffix}s = 0".encode(), f"Num{suffix}s = 3".encode())
        for index, values in enumerate(("ul Num = 1", "f Time = 1", "f TargetCenterX = 1\r\nf TargetCenterY = 2\r\nf TargetRadius = 3")):
            sections[f"Team0Objective9{suffix}{index}"] = ("\r\n" + values + "\r\n").encode()
    original = prefix + b"".join(b"[" + name.encode() + b"]" + body for name, body in sections.items()) + ending
    outputs = {}
    start = original.index(b"[Team0Objective1Condition0]")
    end = original.index(b"\n[", start)
    outputs["objective-resolve"] = original[:start] + original[start:end].replace(b"-1e30", x.encode(), 1).replace(b"-1e30", y.encode(), 1) + original[end:]
    for listing, suffix in ((0, "Condition"), (2, "FailureCondition"), (1, "Action"), (3, "FailureAction")):
        keys = ("Num", "Time", "TargetCenterX", "TargetCenterY", "TargetRadius") if listing % 2 == 0 else tuple(key for _, key in ACTION_PAYLOADS)
        for index, key in enumerate(keys):
            block = f"Team0Objective9{suffix}{min(index, 2)}" if listing % 2 == 0 else f"{owner}{suffix}{index}"
            start = original.index(b"[" + block.encode() + b"]")
            end = original.index(b"\n[", start) if b"\n[" in original[start:] else original.index(b"FITend", start)
            text = key in ("AVIFileName", "WAVFileName", "Message", "PurchaseFileName")
            value = b'"\r\n../Exact/Name\r\n\r\nsecond line\r\n"' if text else b"-17" if key in ("Num", "ResourceStringID") else b"-1.25"
            body = re.sub(rb"(?m)(^\w+ " + key.encode() + rb" = )[^\r\n]*", lambda m: m[1] + value, original[start:end])
            outputs[f"widget-payload-{listing}-{key}"] = original[:start] + body + original[end:]
        if listing % 2 == 0:
            start = original.index(b"[Team0Objective9]")
            end = original.index(b"\n[", start)
            body = original[start:end].replace(f"Num{suffix}s = 3".encode(), f"Num{suffix}s = 4".encode())
            added = (f'[Team0Objective9{suffix}3]\r\nst {suffix}SpeciesString = "DestroySpecificEnemyUnit"\r\nf PositionX = {x}\r\nf PositionY = {y}\r\n').encode()
            outputs[f"widget-payload-{listing}-create"] = (original[:start] + body + original[end:]).replace(b"FITend", added + b"FITend")
    return original, outputs


class PanelGateTests(unittest.TestCase):
    def fixture(self, name):
        directory = TEMP_ROOT / ("e3b-panel-" + uuid.uuid4().hex)
        directory.mkdir()
        original = b'FITini\nfixture\nFITend\n'
        if name in ("objectives_panel", "objectives_crud_panel"):
            original = (SCRIPT.parents[1] / "editor/tests/goldens/e5a-stock/e5_reference_before.fit").read_bytes()
        if name == "objectives_crud_panel":
            original = crud_edit_fixture(original, None)
        objective_outputs = {}
        if name == "objectives_panel":
            original, objective_outputs = objective_panel_fixture(original)
        pak = ((SCRIPT.parents[1] / "editor/tests/goldens/e5a-stock/e5_reference_before.pak").read_bytes()
               if name in ("objectives_panel", "objectives_crud_panel") else b"original pak")
        hybrid = b'FITini\ncompatibilityMode = "Hybrid";\nFITend\n'
        for stage, fit in (("fixture", original), ("existing-hybrid", hybrid), ("hybrid-roundtrip", hybrid)):
            (directory / f"{stage}.fit").write_bytes(fit)
            (directory / f"{stage}.pak").write_bytes(pak)
        (directory / "widget-save.fit").write_bytes(hybrid if name == "brain_modes" else original)
        (directory / "widget-save.pak").write_bytes(pak)
        lines = [f"[EDITOR_PANEL v1] name={name} result=PASS widgets=1 saved_widget=1 reopened_widget=1 rendered=1"]
        for operation in smoke.PANEL_OPERATIONS[name]:
            before = hybrid if operation == "brain-Legacy" else original
            edited = hybrid if operation == "brain-Hybrid" else before + b"authored change"
            if operation in objective_outputs:
                edited = objective_outputs[operation]
            elif name == "objectives_panel":
                from editor.tests.objective_census import objective_fields
                key = operation.removeprefix("objective-").replace("_", " ")
                kind, old = objective_fields(original)["Team0Objective1/" + key][0].split(" ", 1)
                value = ('"\r\nE6a objective\r\n\r\nsecond line\r\n"' if key in ("Title", "Description") else '"E6a objective scalar"' if kind == "st" else
                         ("FALSE" if old == "TRUE" else "TRUE") if kind == "b" else
                         "123.25" if kind == "f" else "2" if key == "Priority" else "13" if key == "ModelType" else "17")
                start = original.index(b"[Team0Objective1]")
                end = original.index(b"\n[", start)
                body = re.sub(rb"(?m)(^\w+\s+" + re.escape(key.encode()) + rb"\s*=\s*)[^\r\n]*",
                              lambda m: m[1] + value.encode(), original[start:end])
                edited = original[:start] + body + original[end:]
            elif name == "objectives_crud_panel":
                edited = crud_edit_fixture(original, operation)
            for stage, fit in (("edited", edited), ("undone", before), ("redone", edited)):
                (directory / f"{operation}-{stage}.fit").write_bytes(fit)
                (directory / f"{operation}-{stage}.pak").write_bytes(pak)
            lines.append(f"[EDITOR_PANEL_OPERATION v1] name={operation} changed=1 reopened=1 undo=1 redo=1 widgets=1")
        if name in ("objectives_panel", "objectives_crud_panel"):
            (directory / "widget-save.fit").write_bytes(edited)
        (directory / "shell.log").write_text("\n".join(lines))
        if name == "objectives_panel":
            fields = smoke.fit_fields(original)
            target = fields["Part1/CSVFile"][0][4:-1] + " / Part 1"
            for stem in ("before", "resolved"):
                rows = [f"Team0Objective1Condition{i}\t{'Missing' if i == 0 and stem == 'before' else 'Resolved'}\t{'' if i == 0 and stem == 'before' else target}\t{'no eligible XY target' if i == 0 and stem == 'before' else 'unique eligible XY target'}" for i in range(3)]
                (directory / f"objective-rows-{stem}.tsv").write_text("\n".join(rows))
        (directory / "panels.bmp").write_bytes(b"BM" + bytes(52) + (bytes(range(256)) * (1440 * 960 * 3 // 256)))
        return directory

    def test_each_new_row_requires_changed_output_and_widget_reach(self):
        for name in smoke.PANEL_OPERATIONS:
            if name.startswith("chooser") or name == "marker_panel": continue
            directory = self.fixture(name)
            smoke.grade_panel(name, directory, 0)
            with self.subTest(name=name), self.assertRaises(ValueError):
                smoke.grade_panel(name, directory, 2)
            log = directory / "shell.log"
            original = log.read_text()
            log.write_text(original.replace("widgets=1", "widgets=0"))
            with self.assertRaises(ValueError):
                smoke.grade_panel(name, directory, 0)
            log.write_text(original + "\n" + original.splitlines()[0])
            with self.assertRaises(ValueError):
                smoke.grade_panel(name, directory, 0)

    def test_noop_undo_corruption_redo_corruption_and_empty_capture_fail(self):
        directory = self.fixture("unit_panel")
        for stage in ("edited", "undone", "redone"):
            path = directory / f"unit-team-{stage}.fit"
            original = path.read_bytes()
            path.write_bytes((directory / "fixture.fit").read_bytes() if stage == "edited" else original + b"corrupt")
            with self.subTest(stage=stage), self.assertRaises(ValueError):
                smoke.grade_panel("unit_panel", directory, 0)
            path.write_bytes(original)
        (directory / "panels.bmp").write_bytes(b"BM" + bytes(52 + 1440 * 960 * 3))
        with self.assertRaisesRegex(ValueError, "rendered panel"):
            smoke.grade_panel("unit_panel", directory, 0)

    def test_existing_hybrid_must_remain_byte_identical(self):
        directory = self.fixture("brain_modes")
        path = directory / "hybrid-roundtrip.fit"
        path.write_bytes(path.read_bytes() + b"corrupt")
        with self.assertRaisesRegex(ValueError, "Hybrid bytes"):
            smoke.grade_panel("brain_modes", directory, 0)


class ChooserGateTests(unittest.TestCase):
    def fixture(self):
        from editor.tests.legacy_reference import FROZEN, reference_evidence, verify_frozen
        import shutil
        directory = TEMP_ROOT / ("e5b2-chooser-" + uuid.uuid4().hex)
        directory.mkdir()
        for path in FROZEN.iterdir(): shutil.copyfile(path, directory / path.name)
        lines = ["[EDITOR_PANEL v1] name=chooser result=PASS widgets=1 saved_widget=1 reopened_widget=1 rendered=1"]
        for family, row in verify_frozen()["placements"].items():
            for extension in ("fit", "pak"):
                before = (FROZEN / f"e5_reference_before.{extension}").read_bytes()
                after = (FROZEN / f"e5_reference_{family}.{extension}").read_bytes()
                (directory / f"fixture.{extension}").write_bytes(before)
                for stage, data in (("edited", after), ("undone", before), ("redone", after)):
                    (directory / f"chooser-{family}-{stage}.{extension}").write_bytes(data)
                (directory / f"widget-save.{extension}").write_bytes((FROZEN / f"e5_reference_marker.{extension}").read_bytes())
            fact = row["facts"]
            source = (directory / "object2.pak").as_posix()
            preview = f'{fact["fit_id"]}\t{fact["stem"]}\t{family}\tfastfile\t{source}\n'
            other = f'9999\tother\t{family}\tfastfile\t{source}\n'
            (directory / f"{family}-preview.tsv").write_text(preview)
            keys = {"data/art/buildings.csv", "data/objects/object2.pak", f'data/objects/{fact["stem"].lower()}.csv'}
            if row["part"]:
                prefix = next(iter(row["part"])).split("/", 1)[0] + "/"
                pilot = row["part"][prefix + "Pilot"][0].split(" ", 1)[1]
                profile = row["fitFields"][f"Warrior{pilot}/Profile"][0][4:-1]
                keys.update({"data/objects/pilots.csv", "data/missions/warriors/" + profile.lower() + ".fit"})
            (directory / f"{family}-sources.tsv").write_text("".join(f"{key}\tfastfile\t{source}\n" for key in sorted(keys)))
            for stage, before, after in (("browse", preview + "other family\n", preview + other), ("search", preview + other, preview)):
                (directory / f"{family}-{stage}-before.tsv").write_text(before)
                (directory / f"{family}-{stage}-after.tsv").write_text(after)
            lines.append(f"[EDITOR_PANEL_OPERATION v1] name=chooser-{family} changed=1 reopened=1 undo=1 redo=1 widgets=1")
        for operation in smoke.PANEL_OPERATIONS["chooser"]:
            family = operation.removeprefix("chooser-")
            lines.extend(f"[EDITOR_CHOOSER_VIEW v1] family={family} stage={stage} changed=1 widgets=1" for stage in ("browse", "search"))
        lines.append(reference_evidence(verify_frozen()))
        (directory / "shell.log").write_text("\n".join(lines))
        (directory / "panels.bmp").write_bytes(b"BM" + bytes(52) + bytes(range(256)) * (1440 * 960 * 3 // 256))
        return directory

    def grade(self, directory):
        with patch("editor.tests.run_catalog_proofs.oracle", return_value=lambda key: (b"", "fastfile", directory / "object2.pak")):
            smoke.grade_panel("chooser", directory, 0)

    def test_corrupted_placement_field_cannot_pass_widget_or_count_evidence(self):
        directory = self.fixture()
        self.grade(directory)
        path = directory / "e5_reference_mech.fit"
        from editor.tests.legacy_reference import verify_frozen
        original = path.read_bytes()
        part = next(iter(verify_frozen()["placements"]["mech"]["part"])).split("/", 1)[0]
        start = original.index(f"[{part}]".encode())
        end = original.find(b"\n[", start + 1)
        body = original[start:end]
        changed = body.replace(b"VariantNumber = 0", b"VariantNumber = 1")
        self.assertNotEqual(body, changed)
        path.write_bytes(original[:start] + changed + original[end:])
        with self.assertRaisesRegex(ValueError, "widget/reference bytes"):
            self.grade(directory)

    def test_widget_outputs_cannot_hide_behind_clean_reference_saves(self):
        directory = self.fixture()
        self.grade(directory)
        from editor.tests.legacy_reference import verify_frozen
        part = next(iter(verify_frozen()["placements"]["mech"]["part"])).split("/", 1)[0]
        for extension in ("fit", "pak"):
            paths = [directory / f"chooser-mech-{stage}.{extension}" for stage in ("edited", "redone")]
            originals = {path: path.read_bytes() for path in paths}
            if extension == "fit":
                original = originals[paths[0]]
                start = original.index(f"[{part}]".encode())
                end = original.find(b"\n[", start + 1)
                body = original[start:end]
                changed = body.replace(b"VariantNumber = 0", b"VariantNumber = 1")
                self.assertNotEqual(body, changed)
                corrupted = original[:start] + changed + original[end:]
            else:
                corrupted = originals[paths[0]] + b"corrupt packet"
            for path in paths: path.write_bytes(corrupted)
            self.assertEqual(paths[0].read_bytes(), paths[1].read_bytes())
            with self.subTest(extension=extension), self.assertRaisesRegex(ValueError, "widget/reference bytes"):
                self.grade(directory)
            for path, contents in originals.items(): path.write_bytes(contents)
        path = directory / "widget-save.fit"
        original = path.read_bytes()
        edited = directory / "chooser-marker-edited.fit"
        redone = directory / "chooser-marker-redone.fit"
        for target in (path, edited, redone): target.write_bytes(original + b"corrupt")
        with self.assertRaisesRegex(ValueError, "widget/reference bytes"):
            self.grade(directory)

    def test_runtime_negative_control_corrupts_widget_history_and_restores_it(self):
        directory = self.fixture()
        paths = [directory / f"chooser-mech-{stage}.fit" for stage in ("edited", "redone")]
        originals = {path: path.read_bytes() for path in paths}
        with patch("editor.tests.run_catalog_proofs.oracle", return_value=lambda key: (b"", "fastfile", directory / "object2.pak")):
            smoke.panel_negative_controls("chooser", directory)
        for path, original in originals.items():
            self.assertEqual(path.read_bytes(), original)
            self.assertNotEqual(path.with_suffix(".negative-control.fit").read_bytes(), original)
        self.assertIn("widget/reference bytes", json.loads((directory / "negative-control.json").read_text())["reason"])

    def test_filters_preview_provenance_and_each_widget_fact_are_required(self):
        directory = self.fixture()
        for name in ("mech-browse-after.tsv", "mech-search-after.tsv", "mech-preview.tsv", "mech-sources.tsv", "shell.log"):
            path = directory / name
            original = path.read_bytes()
            if "-after" in name:
                path.write_bytes(path.with_name(name.replace("-after", "-before")).read_bytes())
            elif name == "shell.log": path.write_bytes(original.replace(b"stage=search changed=1", b"stage=search changed=0"))
            else: path.write_bytes(original.replace(b"fastfile", b"unknown-source"))
            with self.subTest(name=name), self.assertRaises(ValueError): self.grade(directory)
            path.write_bytes(original)
        original = (directory / "shell.log").read_text()
        for fact in ("undo", "redo", "saved_widget", "reopened_widget", "widgets"):
            (directory / "shell.log").write_text(original.replace(f"{fact}=1", f"{fact}=0"))
            with self.subTest(fact=fact), self.assertRaises(ValueError): self.grade(directory)


class MarkerGateTests(unittest.TestCase):
    def test_moved_deleted_and_desync_controls_keep_exact_counts(self):
        from editor.tests.legacy_reference import FROZEN, buildings, packet
        with scratch_directory() as directory:
            before_fit = (FROZEN / "e5_reference_marker.fit").read_bytes()
            before_pak = (FROZEN / "e5_reference_marker.pak").read_bytes()
            moved_fit = before_fit.replace(b"xPos = -4288.000000", b"xPos = -4000.000000")
            self.assertNotEqual(moved_fit, before_fit)
            table = struct.unpack_from("<I", before_pak, 4)[0]
            raw = bytearray(packet(before_pak, 1))
            struct.pack_into("<f", raw, 4 + (len(buildings(before_pak)) - 1) * 40 + 4, -4000)
            moved_pak = bytearray(before_pak[:table])
            struct.pack_into("<I", moved_pak, 0, 0xfeedface)
            for index in range(table // 4 - 2):
                struct.pack_into("<I", moved_pak, 8 + 4 * index, len(moved_pak))
                moved_pak.extend(raw if index == 1 else packet(before_pak, index))
            moved_pak = bytes(moved_pak)
            deleted_fit = (FROZEN / "e5_reference_before.fit").read_bytes()
            deleted_pak = (FROZEN / "e5_reference_before.pak").read_bytes()
            for extension, original, moved, deleted in (("fit", before_fit, moved_fit, deleted_fit), ("pak", before_pak, moved_pak, deleted_pak)):
                for stem, data in (("fixture", original), ("marker-transform-edited", moved), ("marker-delete-edited", deleted)):
                    (directory / f"{stem}.{extension}").write_bytes(data)
            smoke.grade_marker_edits(directory)
            smoke.grade_marker_source(moved_fit, moved_pak, "moved")
            smoke.grade_marker_source(deleted_fit, deleted_pak, "deleted")
            with self.assertRaises(ValueError): smoke.grade_marker_source(before_fit, moved_pak, "moved")
            (directory / "marker-transform-edited.fit").write_bytes(before_fit)
            with self.assertRaises(ValueError): smoke.grade_marker_edits(directory)


class FileGateTests(unittest.TestCase):
    def fixture(self, name):
        directory = TEMP_ROOT / ("e4-file-" + uuid.uuid4().hex)
        directory.mkdir()
        original = b'FITini\nst MissionName = "' + b'x' * 8192 + b' retained tail"\nul DefaultTeam = 0\nFITend\n'
        edited = original.replace(b'ul DefaultTeam = 0', b'ul DefaultTeam = 3')
        for ext in ("fit", "pak"):
            before, after = (original, edited) if ext == "fit" else (b"unchanged pak", b"unchanged pak")
            for stage, data in (("baseline", before), ("edited", after), ("undone", before), ("redone", after)):
                (directory / f"{stage}.{ext}").write_bytes(data)
            saved = after if name in ("file_save", "file_failures", "file_open_save", "file_reopen_save", "file_close_save") else before
            (directory / f"first.{ext}").write_bytes(saved)
            if name in ("file_save_as", "file_recent"):
                (directory / ("mission with spaces Ω " + "p" * 100 + f" retained tail.{ext}")).write_bytes(after)
        lines = [f"[EDITOR_FILE v1] name={name} result=PASS widgets=1 changed=1 undo=1 redo=1 reopened=1 state=1"]
        closed = name in ("file_close_save", "file_close_discard")
        lines.append(f"[EDITOR_FILE_LOOP v1] name={name} terminated={int(closed)} complete=1")
        if name == "file_failures":
            lines.extend(["[EDITOR_MODEL v1] reason=injected rejected=1"] * 5)
        (directory / "shell.log").write_text("\n".join(lines))
        return directory

    def test_registered_cases_require_widget_and_state_proof(self):
        for name in smoke.FILE_CASES:
            with self.subTest(name=name):
                directory = self.fixture(name)
                smoke.grade_file(name, directory, 0)
                log = directory / "shell.log"
                original = log.read_text()
                for key in ("widgets", "state", "undo", "redo", "reopened"):
                    log.write_text(original.replace(f"{key}=1", f"{key}=0"))
                    with self.assertRaises(ValueError):
                        smoke.grade_file(name, directory, 0)
                log.write_text(original + "\n" + original.splitlines()[0])
                with self.assertRaises(ValueError):
                    smoke.grade_file(name, directory, 0)

    def test_wrong_save_target_noop_truncation_and_duplicate_diagnostic_fail(self):
        directory = self.fixture("file_save_as")
        for path in (directory / "first.fit", directory / "undone.pak", directory / "redone.fit",
                     directory / ("mission with spaces Ω " + "p" * 100 + " retained tail.fit")):
            original = path.read_bytes()
            path.write_bytes(original + b"corruption")
            with self.subTest(path=path.name), self.assertRaises(ValueError):  # str: xdist cannot serialize a Path subtest param
                smoke.grade_file("file_save_as", directory, 0)
            path.write_bytes(original)
        path = directory / "edited.fit"
        original = path.read_bytes()
        for bad in ((directory / "baseline.fit").read_bytes(), original.replace(b"x" * 8192, b"x" * 2000)):
            path.write_bytes(bad)
            with self.assertRaises(ValueError):
                smoke.grade_file("file_save_as", directory, 0)
        directory = self.fixture("file_failures")
        log = directory / "shell.log"
        log.write_text(log.read_text() + "\n[EDITOR_MODEL v1] reason=duplicate rejected=1")
        with self.assertRaisesRegex(ValueError, "diagnostic count"):
            smoke.grade_file("file_failures", directory, 0)

    def test_close_completion_cannot_substitute_for_production_loop_termination(self):
        for name in ("file_close_save", "file_close_discard", "file_close_cancel"):
            directory = self.fixture(name)
            log = directory / "shell.log"
            original = log.read_text()
            loop = original.splitlines()[1]
            for bad in (original.splitlines()[0], original + "\n" + loop,
                        original.replace("complete=1", "complete=0"),
                        original.replace("terminated=1", "terminated=0") if name != "file_close_cancel"
                        else original.replace("terminated=0", "terminated=1")):
                with self.subTest(name=name, bad=bad):
                    log.write_text(bad)
                    with self.assertRaisesRegex(ValueError, "loop termination"):
                        smoke.grade_file(name, directory, 0)


class LegacyCatalogReferenceTests(unittest.TestCase):
    def fixture(self, directory):
        from editor.tests.legacy_reference import PLACEMENTS, TEAMS
        def pak(records):
            payload = struct.pack("<I", len(records)) + b"".join(records)
            return struct.pack("<IIIIIII", 0xfeedface, 28, 28 | 0xe0000000, 28, (28 + len(payload)) | 0xe0000000,
                               (28 + len(payload)) | 0xe0000000, (28 + len(payload)) | 0xe0000000) + payload
        baseline = b"FITini\n[Parts]\nul NumParts = 0\nFITend\n"
        (directory / "e5_reference_before.fit").write_bytes(baseline)
        (directory / "e5_reference_before.pak").write_bytes(pak([]))
        evidence = []
        for family, (object_id, stem) in PLACEMENTS.items():
            fields = {"ObjectNumber": ("ul", object_id), "ControlType": ("ul", 2), "ControlDataType": ("ul", 1),
                      "PlayerPart": ("b", "TRUE"), "MyIcon": ("c", 0), "TeamID": ("c", 0), "CommanderID": ("c", 0),
                      "Pilot": ("ul", 1), "PositionX": ("f", 10), "PositionY": ("f", 20), "Rotation": ("f", 0),
                      "Active": ("l", 1), "Exists": ("l", 1), "Damage": ("f", 0), "BaseColor": ("ul", 0xffffff),
                      "HighlightColor1": ("ul", 0xc0c0c0), "HighlightColor2": ("ul", 0x808080), "SelfRepairBehavior": ("ul", 1),
                      "ObjectProfile": ("st", '"PM207300"'), "CSVFile": ("st", f'"{stem}"'), "VariantNumber": ("ul", 0),
                      "SquadNum": ("ul", 1), "NumAlternatives": ("ul", 0)}
            if family in ("mech", "vehicle"):
                text = "FITini\n[Parts]\nul NumParts = 1\n[Warrior1]\nst Profile = \"pilot\"\nst Brain = \"PBrain\"\nl NumCells = 0\nl NumStaticVars = 0\n[Part1]\n"
                text += "".join(f"{kind} {name} = {value}\n" for name, (kind, value) in fields.items()) + "FITend\n"
                records = []
            else:
                text = baseline.decode()
                records = [struct.pack("<Iffffiiiii", object_id, 10, 20, 30, 0, 0, TEAMS[family], -1, 0, 0)]
            (directory / f"e5_reference_{family}.fit").write_text(text)
            (directory / f"e5_reference_{family}.pak").write_bytes(pak(records))
            evidence.append(f"[EDITOR_LEGACY_PLACEMENT v1] family={family} fit_id={object_id} stem={stem} x=10 y=20 z=30 input_team=0 team={TEAMS[family]} painted=1 undo=1 redo=1 saved=1 removed=1 reopened=1")
        return "\n".join(evidence)

    def test_reference_rejects_noop_wrong_identity_incomplete_units_and_false_facts(self):
        from editor.tests.legacy_reference import grade
        with scratch_directory() as directory:
            evidence = self.fixture(directory)
            self.assertEqual(len(grade(directory, evidence)["placements"]), 9)
            for bad in (evidence.splitlines()[0], evidence + "\n" + evidence.splitlines()[0],
                        evidence.replace("fit_id=53", "fit_id=51"), evidence.replace("redo=1", "redo=0")):
                with self.subTest(bad=bad), self.assertRaises(ValueError):
                    grade(directory, bad)
            for name, contents in (("building.pak", (directory / "e5_reference_before.pak").read_bytes()),
                                   ("mech.fit", (directory / "e5_reference_mech.fit").read_bytes().replace(b"ul SelfRepairBehavior = 1", b"ul Missing = 1")),
                                   ("mech.fit", (directory / "e5_reference_mech.fit").read_bytes().replace(b'st Profile = "pilot"', b'st Profile = ""'))):
                path = directory / f"e5_reference_{name}"
                original = path.read_bytes()
                path.write_bytes(contents)
                with self.subTest(name=name), self.assertRaises(ValueError):
                    grade(directory, evidence)
                path.write_bytes(original)

    def test_reference_is_immutable_and_forest_scale_are_integer_words(self):
        from editor.tests.legacy_reference import grade, retain, buildings
        with scratch_directory() as directory:
            result = grade(directory, self.fixture(directory))
            retain(directory, directory / "frozen", result)
            with self.assertRaises(FileExistsError):
                retain(directory, directory / "frozen", result)
            data = bytearray((directory / "e5_reference_tree.pak").read_bytes())
            data[-8:] = struct.pack("<ii", 4, 125)
            self.assertEqual(struct.unpack("<ii", buildings(data)[0][-8:]), (4, 125))
            (directory / "e5_reference_tree.pak").write_bytes(data)
            with self.assertRaisesRegex(ValueError, "forest/scale"):
                grade(directory, self.fixture_facts(result))

    def test_new_unit_identity_survives_legacy_part_reordering(self):
        from editor.tests.legacy_reference import grade
        with scratch_directory() as directory:
            evidence = self.fixture_with_existing(directory)
            self.assertEqual(len(grade(directory, evidence)["placements"]), 9)
            for family in ("mech", "vehicle"):
                path = directory / f"e5_reference_{family}.fit"
                path.write_text(re.sub(r"(Part|Warrior|ul Pilot = )([23])", lambda match: match[1] + ("3" if match[2] == "2" else "2"), path.read_text()))
            self.assertEqual(len(grade(directory, evidence)["placements"]), 9)

    def fixture_with_existing(self, directory):
        from editor.tests.legacy_reference import PLACEMENTS
        evidence = self.fixture(directory)
        def existing(first):
            text = ""
            for number, object_id in ((first, 51), (first + 1, 52)):
                text += f'[Part{number}]\nul ObjectNumber = {object_id}\nf PositionX = 99\nf PositionY = 88\nul Pilot = {number}\n'
                text += f'[Warrior{number}]\nst Profile = "existing{object_id}"\nst Brain = "DredAttack01"\nl NumCells = 1\nl NumStaticVars = 0\n'
                text += f'[Warrior{number}Cell0]\nl Cell = 2\nl MemType = 1\nf Value = 150\n'
            return text
        baseline = "FITini\n[Parts]\nul NumParts = 2\n" + existing(1) + "FITend\n"
        (directory / "e5_reference_before.fit").write_text(baseline)
        for family in PLACEMENTS:
            path = directory / f"e5_reference_{family}.fit"
            if family in ("mech", "vehicle"):
                text = path.read_text().replace("NumParts = 1", "NumParts = 3")
                path.write_text(text.replace("FITend", existing(2) + "FITend"))
            else:
                path.write_text(baseline)
        return evidence

    def test_reference_rejects_replaced_modified_and_missing_existing_units(self):
        from editor.tests.legacy_reference import grade
        with scratch_directory() as directory:
            evidence = self.fixture_with_existing(directory)
            for family in ("mech", "vehicle", "building"):
                path = directory / f"e5_reference_{family}.fit"
                original = path.read_bytes()
                changes = ((b"ObjectNumber = 51", b"ObjectNumber = 52"),
                           (b"PositionX = 99", b"PositionX = 100"),
                           (b'Profile = "existing51"', b'Profile = "replacement"'),
                           (b'Brain = "DredAttack01"', b'Brain = "OtherBrain"'),
                           (b"ul Pilot = 2", b"ul Pilot = 3" if family != "building" else b"ul Pilot = 1"),
                           (b"ul Pilot = 2", b"l Pilot = 2"),
                           (b"Value = 150", b"Value = 151"))
                for before, after in changes:
                    path.write_bytes(original.replace(before, after, 1))
                    with self.subTest(family=family, field=before), self.assertRaisesRegex(ValueError, "pre-existing Parts or Warriors"):
                        grade(directory, evidence)
                number = 2 if family != "building" else 1
                missing = re.sub(rb'\[Part' + str(number).encode() + rb'\].*?(?=\[Warrior)', b'', original, count=1, flags=re.S)
                path.write_bytes(missing)
                with self.subTest(family=family, field="missing Part"), self.assertRaises(ValueError):
                    grade(directory, evidence)
                path.write_bytes(original)

    def test_reference_requires_saved_alignment_and_meaningful_pilot_brain_defaults(self):
        from editor.tests.legacy_reference import grade
        with scratch_directory() as directory:
            evidence = self.fixture(directory)
            for family in ("mech", "vehicle"):
                path = directory / f"e5_reference_{family}.fit"
                original = path.read_bytes()
                for before, after, reason in ((b"c TeamID = 0", b"c TeamID = 1", "alignment"),
                                             (b"c CommanderID = 0", b"c CommanderID = 1", "alignment"),
                                             (b"b PlayerPart = TRUE", b"b PlayerPart = FALSE", "alignment"),
                                             (b'st Brain = "PBrain"', b'st Brain = ""', "defaults"),
                                             (b'st Brain = "PBrain"', b'st Brain = "   "', "defaults"),
                                             (b'st Brain = "PBrain"', b'st Brain = "OtherBrain"', "defaults"),
                                             (b'st Profile = "pilot"', b'st Profile = "   "', "defaults"),
                                             (b"l NumCells = 0", b"l NumCells = -1", "defaults"),
                                             (b"l NumStaticVars = 0", b"l NumStaticVars = -1", "defaults")):
                    path.write_bytes(original.replace(before, after))
                    with self.subTest(family=family, field=after), self.assertRaisesRegex(ValueError, reason):
                        grade(directory, evidence)
                path.write_bytes(original)

    @staticmethod
    def fixture_facts(result):
        return "\n".join("[EDITOR_LEGACY_PLACEMENT v1] " + " ".join(f"{k}={v}" for k, v in p["facts"].items()) for p in result["placements"].values())


class NativePanelHostOwnershipTests(unittest.TestCase):
    def test_current_registered_builder_can_render_panels(self):
        with patch.object(smoke, "REPO", Path("A:/Games/mc2-lane2")):
            self.assertTrue(smoke.panel_render_host_allowed(Path("A:/Games/mc2-opengl/mc2-win64-lane2/Mission Editor Smoke.exe")))

    def test_other_worktree_and_unregistered_installs_are_rejected(self):
        with patch.object(smoke, "REPO", Path("A:/Games/different-worktree")):
            self.assertFalse(smoke.panel_render_host_allowed(Path("A:/Games/mc2-opengl/mc2-win64-lane2/Mission Editor Smoke.exe")))
        self.assertFalse(smoke.panel_render_host_allowed(Path("A:/Games/mc2-opengl/mc2-win64-v0.4c/Mission Editor Smoke.exe")))

    def test_owned_render_case_reaches_fingerprint_validation(self):
        with scratch_directory() as directory:
            exe = directory / "smoke.exe"
            exe.write_bytes(b"not a PE")
            with patch.object(smoke, "panel_render_host_allowed", return_value=True), patch.object(smoke.exe_fingerprint, "extract", return_value={"present": False}):
                result = smoke.run_panel("panel_render", exe, None, directory / "result", 1)
            self.assertIn("fingerprint", result["detail"])


if __name__ == "__main__":
    unittest.main()
