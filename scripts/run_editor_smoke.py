#!/usr/bin/env python3
# scripts/run_editor_smoke.py
"""Console smoke runner for the MC2 Mission Editor (EditRel / "Mission Editor.exe").

The editor is an MFC/WIN32-subsystem GUI app, so there is no console stdout to
scrape and no way to click menus headlessly. Instead we drive the editor's
existing self-run CLI (editor/EditorMFC.cpp s_cli_parse: -gen-map / -mission /
-frames / -exit-on-load-fail / -smoke-foliage) and read the result back from the
machine-readable lines it writes:

  * `[ESMOKE v1] event=summary clean_exit=1 frames=N autoload=1 gen_map=1
     foliage_smoke=1 foliage_count=K`   -- emitted at ExitInstance
  * `[EDITOR_CLI v1] event=...`         -- per-step facts
  * EarlyTrace milestones               -- in <deploy>/editor-startup.log

editor-startup.log is the reliable IPC channel (opened "w" at process start);
stderr is also captured via an inherited pipe when available.

Verdict (mirrors the game smoke's gates philosophy: the runner judges, the app
only reports facts):
  PASS  iff process exited (not timeout) AND returncode==0 AND the ESMOKE summary
        shows clean_exit=1 AND autoload=1 AND no crash/asset/STOP/FAILED tokens,
        plus any per-case expectation (e.g. foliage_count>0 for foliage_present).

This validates the editor-facing half of the Phase 5 manual smoke:
  launch -> generate/load map -> (load foliage / toggle) -> clean exit, and that
  a missing or garbage foliage JSON / missing sprite texture does NOT crash.

KNOWN SMOKE FLAKE (visible-quarantine policy, see run_case_with_retry):
  foliage_present and foliage_menu_commands occasionally crash with
  0xC0000005 (STATUS_ACCESS_VIOLATION) ~1s into a back-to-back full-suite run,
  yet PASS reliably in isolation. Suspected editor process teardown/startup
  contention or a deployed asset/runtime-state race -- NOT introduced by the
  Scene Outliner / Inspector work (those cases pass). Policy: an 0xC0000005
  failure is retried ONCE; crash-then-pass is reported as FLAKY_PASS and the
  suite header is starred (`result=PASS* (.. , N flaky-pass)`) so the run is
  never mistaken for strictly clean. A double 0xC0000005 stays a hard FAIL.
  Only 0xC0000005 is retried; all other failures fail immediately.

Usage:
  py -3 scripts/run_editor_smoke.py [--exe PATH] [--frames N] [--timeout SEC]
                                    [--case NAME ...] [--keep-logs]
  Cases: gen_map_basic, foliage_present, foliage_missing, foliage_garbage (default: all)
"""
import argparse
import difflib
import hashlib
import json
import os
import re
import subprocess
import struct
import sys
import time
import zlib
from collections import Counter
from decimal import Decimal, InvalidOperation
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from smoke_lib import nofocus  # noqa: E402
from smoke_lib import host_lease, exe_guard  # noqa: E402
from console_child import child_kwargs  # noqa: E402
import exe_fingerprint  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.mc2x_import.fst import fst_entries, decompress_entry  # noqa: E402

DEFAULT_EXE = r"A:\Games\mc2-opengl\mc2-win64-v0.4\Mission Editor.exe"

# Run the editor window minimized/non-activating during smoke (set from --minimized).
MINIMIZED = True
# Strong crash signatures only -- the editor trace log is verbose, so weak tokens
# like "FAILED"/"assert"/"STOP(" would false-positive. A load failure is already
# caught by rc!=0 (the editor exit(1)s under -exit-on-load-fail).
CRASH_TOKENS = ("unhandled exception", "access violation", "stack overflow", "fatal error")
REPO = Path(__file__).resolve().parents[1]
STOCK_MISSIONS = tuple(f"mc2_{n:02}" for n in range(1, 25))
WORKFLOW_PANELS = {
    "PaintMaterial": "terrain_paint", "AssetBrowser": "object_placement",
    "Inspector": "teams_abl", "UnitBrain": "teams_abl",
    "ObjectivesSummary": "objectives", "CampaignOverview": "campaign_links",
    "MissionWriter": "mission_export",
    "MissionDefinitions": "mission_definitions",
}
MODEL_ONLY = {"ObjectivesSummary", "CampaignOverview"}
BASELINE = REPO / "tests/smoke/editor/roundtrip-baseline.json"
PINNED_BEFORE = REPO / "tests/smoke/editor/old-editor-2026-10-13"
READER_PAYLOADS = {"bik": "AVIFileName", "wav": "WAVFileName", "message": "Message",
                   "technology": "PurchaseFileName", "action_flag": "FlagID", "condition_flag": "FlagID"}
READER_LITERALS = ("unterminated", "block_boundary", "trailing", "extra_quote", "nul", "unquoted", "cr")
READER_SPECIES = ("condition", "action", "failure_condition", "failure_action", "negative",
                  "out_of_range", "unsupported", "numeric_garbage", "literal_unterminated",
                  "literal_trailing", "literal_nul", "literal_unquoted")
READER_MISSING_PAYLOADS = {"legacy_num": "Num", "legacy_unit_watch": "PositionX",
                           "legacy_structure_watch": "PositionY", "string_unit_watch": "PositionX",
                           "string_structure_watch": "PositionY", "capture_unit": "PositionX",
                           "capture_structure": "PositionY", "action_file": "AVIFileName"}


def read_json_object(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"expected a JSON object: {path}")
    return data


def parse_panels(text: str) -> dict:
    """Facts from the main-thread probes; duplicate IPC copies must agree."""
    panels = {}
    for line in text.splitlines():
        if not line.startswith("[EDITOR_PANEL v1] "):
            continue
        fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
        name = fields.get("panel", "")
        if name in panels and panels[name] != fields:
            raise ValueError(f"conflicting panel facts: {name}")
        panels[name] = fields
    return panels


def grade_workflows(text: str) -> str:
    try:
        panels = parse_panels(text)
    except ValueError as exc:
        return str(exc)
    failures = []
    for panel, workflow in WORKFLOW_PANELS.items():
        fields = panels.get(panel, {})
        coverage = "model-only" if panel in MODEL_ONLY else "action-handler"
        reached = "0" if panel in MODEL_ONLY else "1"
        if (fields.get("workflow") != workflow or fields.get("reached") != reached
                or fields.get("coverage") != coverage or fields.get("ok") != "1"
                or any(fields.get(key) != "1" for key in ("authored", "undo", "saved", "reloaded"))):
            failures.append(f"{workflow}/{panel}: missing or unsuccessful probe")
    definitions = [dict(re.findall(r"(\w+)=([^\s]+)", line)) for line in text.splitlines()
                   if line.startswith("[EDITOR_DEFINITIONS v1] ")]
    for stage in ("duplicate_unit_refresh_undo_redo", "script_boundary", "display_equality"):
        if not any(row.get("stage") == stage and row.get("ok") == "1" for row in definitions):
            failures.append(f"mission_definitions/{stage}: missing or unsuccessful probe")
    if not any(row.get("preservation_commands_undo_reload_malformed") == "1" for row in definitions):
        failures.append("mission_definitions: missing versioned summary")
    for field in ("Title", "Description", "ActivateFlagID", "ResetStatusFlagID", "ObjectiveModel",
                  "MissionName", "Author", "Blurb", "Blurb2", "AVIFilename", "DownloadURL", "ScenarioScript"):
        limit = 1023 if field == "ScenarioScript" else 2000
        if not any(row.get("stage") == "oversized_text" and row.get("field") == field
                   and all(row.get(key) == "1" for key in ("rejected", "continued", "ok")) for row in definitions):
            failures.append(f"mission_definitions/{field}: missing rejected/continued proof")
        diagnostics = [line for line in text.splitlines() if line.startswith("[EDITOR_DOCUMENT v1] ")
                       and f"/{field} " in line and f"length={limit + 1} limit={limit}" in line
                       and "reason=too_long rejected=1" in line and "mission=" in line]
        if len(diagnostics) != 1:
            failures.append(f"mission_definitions/{field}: expected one mission/field/length/limit diagnostic")
    for field in ("NumConditions", "NumObjectives", "Version", "scenarioTuneNum", "MissionName", "Blurb2",
                  "ScenarioScript", "MissionNameResourceStringID"):
        if not any(row.get("stage") == "missing_required" and row.get("field") == field
                   and all(row.get(key) == "1" for key in ("rejected", "unchanged", "ok")) for row in definitions):
            failures.append(f"mission_definitions/{field}: missing rejected/unchanged proof")
        diagnostics = [line for line in text.splitlines() if line.startswith("[EDITOR_DOCUMENT v1] ")
                       and f"/{field} missing=1 rejected=1" in line and "mission=" in line]
        if len(diagnostics) != 1:
            failures.append(f"mission_definitions/{field}: expected one missing-field diagnostic")
    def reader_case(stage, case, filename, reason, flags, field=None):
        if not any(row.get("stage") == stage and row.get("case") == case
                   and (stage not in ("payload", "missing_payload") or row.get("field") == field)
                   and all(row.get(key) == "1" for key in (*flags, "ok")) for row in definitions):
            failures.append(f"mission_definitions/{stage}/{case}: missing or unsuccessful probe")
        diagnostics = [line for line in text.splitlines() if line.startswith("[EDITOR_DOCUMENT v1] ")
                       and f"/{filename}.fit " in line and " rejected=1" in line]
        expected = 0 if reason is None else 1
        if len(diagnostics) != expected or (expected and (f"{reason} rejected=1" not in diagnostics[0]
                or (field is not None and f"/{field} " not in diagnostics[0])
                or (stage != "condition_failure" and reason.startswith("reason=")
                    and not re.search(r" length=\d+ limit=\d+ reason=", diagnostics[0]))
                or (stage == "payload" and case.endswith("_oversized")
                    and "length=2001 limit=2000 reason=too_long" not in diagnostics[0]))):
            failures.append(f"mission_definitions/{stage}/{case}: expected {expected} rejection diagnostic")

    for kind, field in READER_PAYLOADS.items():
        for mode in ("oversized", "unterminated", "trailing", "nul"):
            case = f"{kind}_{mode}"
            reader_case("payload", case, f"ew_payload_{case}",
                        "reason=too_long" if mode == "oversized" else "reason=malformed_literal",
                        ("retained", "following"), field)
    for mode in READER_LITERALS:
        reader_case("literal", mode, f"ew_literal_{mode}", None if mode == "cr" else "reason=malformed_literal",
                    ("retained", "following"), "Title" if mode != "cr" else None)
    for mode in ("unterminated", "trailing", "nul", "unquoted"):
        reader_case("metadata_literal", mode, f"ew_metadata_{mode}", "reason=malformed_literal",
                    ("retained", "following"), "Author")
    for case in READER_SPECIES:
        reader_case("species", case, f"ew_species_{case}",
                    "reason=malformed_literal" if case.startswith("literal_") else "reason=invalid_species", ("following",))
    for case, field in READER_MISSING_PAYLOADS.items():
        reader_case("missing_payload", case, f"ew_missing_{case}", "missing=1", ("rejected", "unchanged"), field)
    if not any(row.get("stage") == "reader_budget" and all(row.get(key) == "1" for key in ("rejected", "unchanged", "ok"))
               for row in definitions):
        failures.append("mission_definitions/reader_budget: missing unchanged-document rejection proof")
    budget = [line for line in text.splitlines() if "/ew_reader_budget.fit " in line and "rejected=1" in line]
    if len(budget) != 1 or " length=16777217 limit=16777216 reason=document_too_large" not in budget[0]:
        failures.append("mission_definitions/reader_budget: expected one byte-limit diagnostic")
    for field, value in (("ModelScale", "0"), ("ModelScale", "-1"), ("ModelScale", "1e100"),
                         ("ModelScale", "1garbage"), ("ModelType", "256"), ("Num", "1garbage")):
        if not any(row.get("stage") == "candidate_rejection" and row.get("field") == field and row.get("value") == value
                   and all(row.get(key) == "1" for key in ("rejected", "unchanged", "history", "ok")) for row in definitions):
            failures.append(f"mission_definitions/candidate/{field}/{value}: missing state/history preservation")
        diagnostics = [line for line in text.splitlines() if line.startswith("[EDITOR_DOCUMENT v1] ")
                       and f"/cq_candidate_{field}_{value}.fit " in line and "rejected=1" in line]
        reason = "condition_read_failed" if field == "Num" else "malformed_value"
        if len(diagnostics) != 1 or f"reason={reason} rejected=1" not in diagnostics[0]:
            failures.append(f"mission_definitions/candidate/{field}/{value}: expected one rejection diagnostic")
    for case in ("ObjectiveModel", "ModelType", "ModelScale", "all"):
        if not any(row.get("stage") == "model_default" and row.get("case") == case and row.get("ok") == "1" for row in definitions):
            failures.append(f"mission_definitions/model_default/{case}: missing default proof")
        defaults = [line for line in text.splitlines() if f"/ew_model_default_{case}.fit " in line and "defaulted=1" in line]
        if len(defaults) != 1 or any(f"{key}={int(case in (key, 'all'))}" not in defaults[0]
                                     for key in ("ObjectiveModel", "ModelType", "ModelScale")):
            failures.append(f"mission_definitions/model_default/{case}: expected one default diagnostic")
    for case in ("Condition", "FailureCondition"):
        reader_case("condition_failure", case, f"ew_condition_failure_{case}", "reason=condition_read_failed", ())
        contextual = [line for line in text.splitlines() if f"/ew_condition_failure_{case}.fit " in line
                      and f"block=Team0Objective0{case}0 species=DestroyNumberOfEnemyUnits" in line]
        if len(contextual) != 1:
            failures.append(f"mission_definitions/condition_failure/{case}: missing block/species diagnostic")
    return "; ".join(failures)


def fit_fields(data: bytes) -> dict:
    """Diagnostic field census, preserving duplicate keys and array continuations."""
    fields = {}
    section = "preamble"
    key = None
    for line in data.decode("latin-1").splitlines():
        line = line.strip()
        if not line or line.startswith("//"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section, key = line[1:-1], None
            continue
        match = re.match(r"(\w+(?:\[\d+\])?)\s+(\w+)\s*=\s*(.*)", line)
        if match:
            kind, name, value = match.groups()
            key = f"{section}/{name}"
            fields.setdefault(key, []).append(f"{kind} {value}")
        elif key and line not in ("FITini", "FitEnd"):
            fields[key][-1] += " " + line
    return fields


def compare_fit(original: bytes, written: bytes) -> dict:
    """Byte equality is authoritative; the field census only explains findings."""
    size = sum(a != b for a, b in zip(original, written)) + abs(len(original) - len(written))
    before, after = fit_fields(original), fit_fields(written)
    removed = sorted(before.keys() - after.keys())
    added = sorted(after.keys() - before.keys())
    changed = sorted(k for k in before.keys() & after.keys() if before[k] != after[k])
    if not size:
        explanation = "Exact byte match."
    else:
        explanations = []
        if original.replace(b"\r\n", b"\n").replace(b"\r", b"") == written.replace(b"\r\n", b"\n").replace(b"\r", b""):
            explanations.append("Line-ending normalization only")
        for label, keys in (("removed", removed), ("added", added), ("changed", changed)):
            if keys:
                sections = sorted({key.split("/", 1)[0] for key in keys})
                explanations.append(f"{label} {len(keys)} fields in {', '.join(sections)}")
        if not explanations:
            explanations.append("Formatting, section ordering or non-field text changed")
        explanation = "FINDING: " + "; ".join(explanations) + ". Full keys and text diff in evidence."
    classes = {}
    for key in removed + added + changed:
        section, name = key.split("/", 1)
        old, new = before.get(key), after.get(key)
        lossless = False
        label = "Unclassified fields"
        if old and new and len(old) == len(new) == 1:
            old_kind, old_value = old[0].split(" ", 1)
            new_kind, new_value = new[0].split(" ", 1)
            if old_kind == new_kind and old_kind in ("f", "l", "ul", "s", "us", "uc"):
                try:
                    lossless = Decimal(old_value) == Decimal(new_value)
                except InvalidOperation:
                    pass
            if lossless:
                label = "Number formatting"
        if not lossless:
            if section == "Cameras":
                label = "Cameras"
            elif re.fullmatch(r"Part\d+", section) and name in ("ControlDataType", "ObjectProfile"):
                label = "Part control/profile"
            elif re.fullmatch(r"Part\d+", section) and name == "CSVFile":
                lossless = bool(old and new and [s.casefold() for s in old] == [s.casefold() for s in new])
                label = "Part CSV spelling" if lossless else "Part CSV value"
            elif re.fullmatch(r"NavMarker\d+", section):
                label = "NavMarker coordinates"
            elif "Objective" in section and "Condition" in section and name in ("PositionX", "PositionY"):
                label = "Objective condition coordinates"
            elif re.fullmatch(r"Team\d+Objective\d+", section) and name == "ModelType":
                label = "Objective ModelType"
        fact = classes.setdefault(label, dict(name=label, semantic=False, fields=[]))
        fact["semantic"] |= not lossless
        fact["fields"].append(key)
    if size and not classes:
        classes["Text formatting/order"] = dict(name="Text formatting/order", semantic=False, fields=[])
    classification = "byte-identical" if not size else ("semantic" if any(c["semantic"] for c in classes.values()) else "lossless-only")
    return dict(byte_identical=(original == written), diff_size=size,
                input_size=len(original), output_size=len(written),
                input_sha256=hashlib.sha256(original).hexdigest(),
                output_sha256=hashlib.sha256(written).hexdigest(),
                removed=removed, added=added, changed=changed, explanation=explanation,
                classes=list(classes.values()), classification=classification)


def pinned_class_delta(mission: str, original: bytes, current: dict, directory: Path = PINNED_BEFORE) -> dict:
    """Historical overlays remain qualified; only equal input hashes are comparable."""
    metadata, written = directory / f"{mission}.json", directory / f"{mission}.written.fit"
    if not metadata.exists() or not written.exists():
        return dict(status="no pinned comparison", disappeared=[], remaining=[])
    before = read_json_object(metadata)
    if before.get("input_sha256") != hashlib.sha256(original).hexdigest():
        return dict(status="different pinned input", disappeared=[], remaining=[])
    old_classes = {c["name"] for c in compare_fit(original, written.read_bytes())["classes"]}
    new_classes = {c["name"] for c in current["classes"]}
    return dict(status="same input; historical non-stock catalog", disappeared=sorted(old_classes - new_classes),
                remaining=sorted(old_classes & new_classes), introduced=sorted(new_classes - old_classes))


class StockInputs:
    """Stock archive bytes pin the input; loose overrides fail instead of hiding mods."""
    def __init__(self, deploy: Path):
        self.deploy = deploy
        self.entries = {e[3].lower().replace("\\", "/"): e for e in fst_entries(deploy / "mission.fst")}

    def read(self, relative: str) -> bytes:
        entry = self.entries[relative.lower()]
        off, comp, real, _, is_lz, archive = entry
        try:
            data = decompress_entry(off, comp, real, is_lz, archive)
        except zlib.error as exc:
            raise ValueError(f"stock archive decompression failed: {relative}") from exc
        if len(data) != real:
            raise ValueError(f"stock archive length mismatch: {relative}")
        loose = self.deploy / relative
        if loose.exists() and loose.read_bytes() != data:
            raise ValueError(f"stock input shadowed by loose override: {relative}")
        return data


def dependency_manifest(deploy: Path, destination: Path, allowlist: dict | None = None) -> dict:
    """Audit the complete VFS namespace, including shadows outside mission.fst.

    Allowances name exact bytes and a reason; a path-only exemption cannot hide
    a changed catalog. Unknown loose data also needs an explicit allowance.
    """
    allowlist = allowlist or {}
    archives, resolved = {}, {}
    system = fit_fields((deploy / "system.cfg").read_bytes())
    count = int(system["FastFiles/NumFastFiles"][0].split(" ", 1)[1])
    if {key for key in system if key.startswith("FastFiles/File")} != {f"FastFiles/File{n}" for n in range(count)}:
        raise ValueError("FastFiles count/entries disagree")
    for key, values in system.items():
        if key.startswith("systemPaths/") and key.lower().endswith("path"):
            value = values[0].split(" ", 1)[1].strip('"').replace("\\", "/")
            if not value.lower().startswith("data/") or ".." in Path(value).parts:
                raise ValueError(f"non-stock data path in system.cfg: {key}={value}")
    for index in range(count):
        name = system[f"FastFiles/File{index}"][0].split(" ", 1)[1].strip('"')
        archive = deploy / name
        if archive.parent != deploy or archive.suffix.lower() != ".fst":
            raise ValueError(f"invalid stock archive path: {name}")
        archive_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
        archives[name] = archive_hash
        for entry in fst_entries(archive):
            relative = entry[3].lower().replace("\\", "/")
            if relative in resolved:
                continue  # FastFile lookup uses the first matching archive.
            data = decompress_entry(entry[0], entry[1], entry[2], entry[4], entry[5])
            if len(data) != entry[2]:
                raise ValueError(f"stock archive length mismatch: {name}/{relative}")
            resolved[relative] = dict(path=relative, sha256=hashlib.sha256(data).hexdigest(),
                                      archive=name, archive_sha256=archive_hash, source="archive")
    shadows, violations = [], []
    for loose in sorted((deploy / "data").rglob("*")):
        if not loose.is_file():
            continue
        relative = loose.relative_to(deploy).as_posix().lower()
        digest = hashlib.sha256(loose.read_bytes()).hexdigest()
        archived = resolved.get(relative)
        allowance = allowlist.get(relative, {})
        allowed = allowance.get("sha256") == digest and bool(allowance.get("reason", "").strip())
        stock = archived is not None and archived["sha256"] == digest
        fact = dict(path=relative, sha256=digest, source="loose",
                    archive=archived["archive"] if archived else None,
                    archive_sha256=archived["archive_sha256"] if archived else None,
                    archive_entry_sha256=archived["sha256"] if archived else None,
                    stock=stock, reason=allowance.get("reason") if allowed else None)
        shadows.append(fact)
        resolved[relative] = fact
        if not stock and not allowed:
            violations.append(relative)
    manifest = dict(version=1, system_sha256=hashlib.sha256((deploy / "system.cfg").read_bytes()).hexdigest(),
                    archives=archives, loose_files=shadows,
                    resolved=list(sorted(resolved.values(), key=lambda item: item["path"])),
                    violations=violations)
    destination.write_bytes((json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode())
    if violations:
        raise ValueError(f"non-stock loose data: {', '.join(violations[:12])}; see {destination}")
    return manifest


def prepare_stock_scratch(deploy: Path, destination: Path, allowances: dict) -> None:
    """Replace overlays only in this worktree's scratch runtime; record every change."""
    scratch = (REPO / "scratch").resolve()
    deploy = deploy.resolve()
    data_root = (deploy / "data").resolve()
    lane3 = Path("A:/Games/mc2-opengl/mc2-win64-lane3").resolve()
    if (not deploy.is_relative_to(scratch) and deploy != lane3) or not data_root.is_relative_to(deploy):
        raise ValueError("--prepare-stock-scratch is restricted to this worktree's scratch runtime or lane3")
    system = fit_fields((deploy / "system.cfg").read_bytes())
    count = int(system["FastFiles/NumFastFiles"][0].split(" ", 1)[1])
    stock, changes = {}, []
    for index in range(count):
        name = system[f"FastFiles/File{index}"][0].split(" ", 1)[1].strip('"')
        archive = (deploy / name).resolve()
        if archive.parent != deploy or archive.suffix.lower() != ".fst":
            raise ValueError(f"invalid archive path: {name}")
        for entry in fst_entries(archive):
            relative = entry[3].lower().replace("\\", "/")
            target = (deploy / relative).resolve()
            if not target.is_relative_to(data_root):
                raise ValueError(f"archive entry outside scratch data: {relative}")
            if relative not in stock:
                content = decompress_entry(entry[0], entry[1], entry[2], entry[4], entry[5])
                if len(content) != entry[2]:
                    raise ValueError(f"archive length mismatch: {relative}")
                stock[relative] = content
    for loose in sorted(data_root.rglob("*")):
        if not loose.is_file():
            continue
        if not loose.resolve().is_relative_to(data_root):
            raise ValueError(f"loose file outside scratch data: {loose}")
        relative = loose.relative_to(deploy).as_posix().lower()
        content = loose.read_bytes()
        digest = hashlib.sha256(content).hexdigest()
        waiver = allowances.get(relative, {})
        if waiver.get("sha256") == digest and waiver.get("reason", "").strip():
            continue
        if relative in stock and stock[relative] == content:
            continue
        changes.append(dict(path=relative, previous_sha256=digest,
                            action="replace-from-stock" if relative in stock else "remove-nonstock-loose"))
        loose.unlink()
    for relative, content in stock.items():
        target = deploy / relative
        if not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
    destination.write_bytes((json.dumps(dict(changes=changes), indent=2, sort_keys=True) + "\n").encode())
    print(f"[STOCK_SCRATCH] {len(changes)} loose files removed/replaced; {len(stock)} stock entries materialized")


def baseline_fact(row: dict) -> dict:
    comparison = row.get("roundtrip")
    return dict(input_sha256=row.get("input_sha256") if not comparison else comparison["input_sha256"],
                output_sha256=None if not comparison else comparison["output_sha256"],
                outcome="blocked" if not comparison else ("identical" if comparison["byte_identical"] else "differs"))


def gate_baseline(results: list, path: Path, rebaseline: bool, archives: dict, allowances: dict | None = None) -> None:
    rows = {r["name"].split(":", 1)[1]: r for r in results if r["name"].startswith("stock_roundtrip:")}
    if not rows:
        return
    previous = read_json_object(path) if path.exists() else {}
    expected = previous.get("missions", {})
    if not isinstance(expected, dict):
        raise ValueError(f"invalid baseline missions: {path}")
    current = {mission: baseline_fact(row) for mission, row in rows.items()}
    if rebaseline:
        if set(rows) != set(STOCK_MISSIONS) or any(not r.get("roundtrip") or not r["passed"] for r in rows.values()):
            raise ValueError("--rebaseline requires all 24 successful fresh comparisons")
        for mission, fact in current.items():
            if expected.get(mission) != fact:
                print(f"[REBASELINE] {mission}: {json.dumps(expected.get(mission))} -> {json.dumps(fact)}")
        if previous.get("archives") != archives:
            print(f"[REBASELINE] archives: {json.dumps(previous.get('archives'))} -> {json.dumps(archives)}")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes((json.dumps(dict(version=1, reason="E-W stock editor FIT writer floor; authored document state, exact bytes, no masks.",
                                         archives=archives, stock_allowlist=allowances or {}, missions=current), indent=2, sort_keys=True) + "\n").encode())
        return
    for mission, row in rows.items():
        if (not row.get("roundtrip") or expected.get(mission) != current[mission]
                or previous.get("version") != 1 or set(expected) != set(STOCK_MISSIONS) or previous.get("archives") != archives):
            row.update(passed=False, result_label="FAIL", bucket="roundtrip_baseline_changed",
                       detail=f"{row.get('detail', '')} Exact baseline mismatch: expected={expected.get(mission)}, observed={current[mission]}; archive identities must agree. Explicit --rebaseline required.")


def campaign_groups(data: bytes) -> dict:
    fields = fit_fields(data)
    count = int(fields["Campaign/GroupCount"][0].split(" ", 1)[1])
    groups = {}
    for group in range(count):
        missions = int(fields[f"Group{group}/MissionCount"][0].split(" ", 1)[1])
        if missions <= 0:
            raise ValueError(f"empty campaign group {group}")
        groups[group] = [fields[f"Group{group}Mission{n}/FileName"][0].split(" ", 1)[1].strip('"')
                         for n in range(missions)]
    if {m for missions in groups.values() for m in missions} != set(STOCK_MISSIONS):
        raise ValueError("campaign does not cover exactly the 24 stock missions")
    return groups


def write_roundtrip_ledger(path: Path, results: list, groups: dict, evidence: Path, dependency_hash="", baseline=BASELINE):
    rows = {r["name"].removeprefix("stock_roundtrip:"): r for r in results if r["name"].startswith("stock_roundtrip:")}
    counts = {name: sum(r.get("roundtrip", {}).get("classification") == name for r in rows.values())
              for name in ("byte-identical", "lossless-only", "semantic")}
    unverified = len(STOCK_MISSIONS) - sum(counts.values())
    # Reviewed writer provenance and artifact decisions survive automatic census refreshes.
    marker = "\n## Writer findings"
    previous = path.read_text(encoding="utf-8") if path.exists() else ""
    notes = marker + previous.split(marker, 1)[1] if marker in previous else ""
    lines = ["# Editor round-trip ledger (E-W)", "",
             f"Summary: {counts['byte-identical']} byte-identical, {counts['lossless-only']} lossless-only, "
             f"{counts['semantic']} semantic; {unverified} blocked of 24.", "",
             "Owner finding: dev `mc2-win64-v0.4` AND golden `mc2-win64-v0.4c` ship non-stock loose "
             "`data/art/Buildings.csv`, SHA-256 `86153dd5c4793672d35033ce4e8e90a871df8b1e131ccde7e8d61ab1cf083ae9`. "
             "Neither install was changed. The stock art.fst entry is `3a1b449d5e4336c7e2345ebd577181fcc7f9f41107cbc1ffa6da7e0ff9b6ebce`.", "",
             f"Dependency manifest: `{evidence.as_posix()}/dependencies.json`; SHA-256 `{dependency_hash}`. "
             f"Frozen exact-byte baseline: `{baseline.as_posix()}`; changes fail unless explicitly rebaselined.", "",
             "Stock inputs come from mission.fst; all resolved archive entries and loose data are fingerprinted, "
             "and non-stock loose overrides are rejected. "
             "Each row loads the stock PAK through -mission and writes a separate PAK/FIT through -smoke-save (quick save). "
             "This proves the FIT writer and editor reload payload, not a full MOVE/tac-map export.", "",
             "Diff size = unequal bytes at matching offsets + the absolute length difference. "
             "Byte equality is unmasked; the field census explains changes without blessing them.", "",
             f"Evidence: `{evidence.as_posix()}` (input FIT, written FIT, unified text diff, SHA-256 JSON and report).", "",
             "Campaign representatives: " + "; ".join(f"group {g}: {missions[0]}" for g, missions in groups.items()) + ".", "",
             "| Mission | Byte-identical yes/no | Diff size | Explanation |", "|---|---|---:|---|"]
    for mission in STOCK_MISSIONS:
        row = rows.get(mission, {})
        comparison = row.get("roundtrip")
        if comparison:
            answer = "yes" if comparison["byte_identical"] else "no"
            size, reason = comparison["diff_size"], comparison["explanation"]
            if comparison.get("classes"):
                reason = "FINDING: " + comparison["classification"] + "; classes: " + ", ".join(c["name"] for c in comparison["classes"]) + f". {len(comparison['changed'])} altered, {len(comparison['added'])} added, {len(comparison['removed'])} removed fields; see evidence."
        else:
            answer, size = "blocked", "—"
            reason = row.get("detail") or row.get("bucket") or "Not run; no byte comparison observed."
        lines.append(f"| {mission} | {answer} | {size} | {reason.replace('|', '/')} |")
    if any("pinned_delta" in row for row in rows.values()):
        lines += ["", "## Pinned before class comparison", "",
                  "Pinned commit: `73b111f33`. That record used a non-stock loose catalog; equal mission input hashes permit a class comparison, not a stock-environment claim. mc2_22 has no pinned comparison.", "",
                  "| Mission | Qualification | Disappeared classes | Remaining / introduced |", "|---|---|---|---|"]
        for mission in STOCK_MISSIONS:
            delta = rows.get(mission, {}).get("pinned_delta", {})
            lines.append(f"| {mission} | {delta.get('status', 'not run')} | {', '.join(delta.get('disappeared', [])) or '-'} | "
                         f"{', '.join(delta.get('remaining', []) + delta.get('introduced', [])) or '-'} |")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(("\n".join(lines) + "\n" + notes).encode("utf-8"))


def marker_reload_provenance(exe):
    if exe.parent != Path("A:/Games/mc2-opengl/mc2-win64-lane3").resolve():
        raise ValueError("owned marker reload is restricted to lane3")
    fingerprint = exe_fingerprint.extract(exe)
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True,
        timeout=10, **child_kwargs(redirected=True)).strip()
    if not fingerprint.get("present") or fingerprint.get("sha12") != head[:12] or fingerprint.get("dirty"):
        raise ValueError("legacy editor requires a clean build fingerprint matching worktree HEAD; "
                         f"exe={exe.as_posix()} expected_sha={head[:12]} "
                         f"observed={json.dumps(fingerprint, sort_keys=True)}; "
                         "rebuild EditRel and deploy the matching executable to lane3")
    return dict(fingerprint=fingerprint, exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), head=head)


def placement_fields(data):
    return fit_fields(b"\n".join(line for line in data.splitlines() if line.strip().lower() not in (b"fitini", b"fitend")))


def grade_marker_source(before_fit, before_pak, action="inserted"):
    from editor.tests.legacy_reference import FROZEN, buildings, marker_values, verify_frozen, canonical_units
    verify_frozen()
    before_fields = placement_fields(before_fit)
    baseline = placement_fields((FROZEN / "e5_reference_before.fit").read_bytes())
    source_markers, baseline_markers = marker_values(before_fields), marker_values(baseline)
    source_records = Counter(buildings(before_pak))
    baseline_records = Counter(buildings((FROZEN / "e5_reference_before.pak").read_bytes()))
    if canonical_units(before_fields) != canonical_units(baseline):
        raise ValueError("marker control changed authored units")
    if action == "deleted":
        if source_markers != baseline_markers or source_records != baseline_records:
            raise ValueError("deleted marker control must remove the identified frozen marker only")
        return
    added, records = source_markers - baseline_markers, source_records - baseline_records
    if sum(added.values()) != 1 or sum(records.values()) != 1 or baseline_markers - source_markers or baseline_records - source_records:
        raise ValueError("owned marker control requires one inserted FIT and packet record")
    record = next(iter(records))
    object_id, x, y = struct.unpack_from("<Iff", record)
    values = dict(next(iter(added)))
    if object_id != 290 or values.get("xPos") != (("f", (x,)),) or values.get("yPos") != (("f", (y,)),):
        raise ValueError("owned marker control FIT and packet identities differ")
    if action == "moved":
        original = Counter(buildings((FROZEN / "e5_reference_marker.pak").read_bytes())) - baseline_records
        previous = next(iter(original))
        if records == original or values.get("radius") != (("f", (256.0,)),) or record[:4] != previous[:4] or record[20:] != previous[20:]:
            raise ValueError("moved marker control did not change the identified marker")


def run_owned_marker_reload(exe, deploy, source, report, exit_sec, timeout, action="inserted"):
    """The legacy reader must accept the owned writer's identified marker placement."""
    from editor.tests.legacy_reference import buildings, marker_values
    before_fit = source.with_suffix(".fit").read_bytes()
    before_pak = source.with_suffix(".pak").read_bytes()
    before_fields = placement_fields(before_fit)
    grade_marker_source(before_fit, before_pak, action)
    directory = deploy / "terrain_gen_out/owned_marker_reload"
    directory.mkdir(parents=True, exist_ok=True)
    for extension, data in (("fit", before_fit), ("pak", before_pak)):
        (directory / f"input.{extension}").write_bytes(data)
        (report / f"input.{extension}").write_bytes(data)
    row = run_case_with_retry("owned_marker_reload", exe, deploy, exit_sec, timeout,
        ["-mission=terrain_gen_out/owned_marker_reload/input.pak", "-smoke-save=terrain_gen_out/owned_marker_reload/reloaded.pak"], "saved")
    row["input_sha256"] = {"fit": hashlib.sha256(before_fit).hexdigest(), "pak": hashlib.sha256(before_pak).hexdigest()}
    if row["passed"]:
        try:
            after_fit = (directory / "reloaded.fit").read_bytes()
            after_pak = (directory / "reloaded.pak").read_bytes()
            if marker_values(placement_fields(after_fit)) != marker_values(before_fields) or Counter(buildings(after_pak)) != Counter(buildings(before_pak)):
                raise ValueError("legacy reload changed marker FIT values or packet placements")
            for extension, data in (("fit", after_fit), ("pak", after_pak)):
                (report / f"reloaded.{extension}").write_bytes(data)
            row["detail"] = "Owned Nav1 save accepted by legacy loader; re-saved marker FIT and packet placements equal."
        except (OSError, ValueError, KeyError, struct.error) as error:
            row.update(passed=False, bucket="owned_marker_reload_changed", detail=str(error))
    return row


def run_owned_objective_reload(exe, deploy, source, report, exit_sec, timeout, crud=False):
    """The legacy authoring reader must retain an independently graded owned edit."""
    from editor.tests.objective_census import grade_objective_edit, objective_fields
    from editor.tests.legacy_reference import FROZEN, typed_fields, verify_frozen
    verify_frozen()
    before_fit = source.with_suffix(".fit").read_bytes()
    before_pak = source.with_suffix(".pak").read_bytes()
    if crud:
        from editor.tests.objective_crud import grade_crud_edit
        operation = source.name.removesuffix("-edited") if source.name != "widget-save" else PANEL_OPERATIONS["objectives_crud_panel"][-1]
        grade_crud_edit((source.parent / "fixture.fit").read_bytes(), before_fit, operation)
    else:
        grade_objective_edit((source.parent / "fixture.fit").read_bytes(), before_fit, "ResourcePoints")
    fixture_pak = (source.parent / "fixture.pak").read_bytes()
    panel_gate = read_json_object(source.parent / "gate.json")
    panel_name = "objectives_crud_panel" if crud else "objectives_panel"
    if (panel_gate.get("result", {}).get("name") != panel_name
            or panel_gate.get("result", {}).get("passed") is not True
            or hashlib.sha256(fixture_pak).hexdigest() != panel_gate.get("provenance", {}).get("input_pak_sha256")
            or fixture_pak != (source.parent / "input.pak").read_bytes()):
        raise ValueError("objective reload fixture PAK does not match proven panel input")
    fixture_fit = (source.parent / "fixture.fit").read_bytes()
    if (hashlib.sha256(fixture_fit).hexdigest() != panel_gate.get("provenance", {}).get("input_fit_sha256")
            or fixture_fit != (source.parent / "input.fit").read_bytes()):
        raise ValueError("objective reload fixture FIT does not match proven panel input")
    if crud:
        grade_panel(panel_name, source.parent, 0)
    if before_pak != fixture_pak:
        raise ValueError("objective reload control changed the PAK")
    case = "owned_objective_crud_reload" if crud else "owned_objective_reload"
    directory = deploy / "terrain_gen_out" / case
    directory.mkdir(parents=True, exist_ok=True)
    null_reload = None
    if crud:
        for extension, data in (("fit", fixture_fit), ("pak", fixture_pak)):
            (directory / f"null-input.{extension}").write_bytes(data)
            (report / f"null-input.{extension}").write_bytes(data)
        for extension in ("fit", "pak"):
            (directory / f"null-reloaded.{extension}").unlink(missing_ok=True)
        null_reload = run_case_with_retry(case + "_null", exe, deploy, exit_sec, timeout,
            [f"-mission=terrain_gen_out/{case}/null-input.pak", f"-smoke-save=terrain_gen_out/{case}/null-reloaded.pak"], "saved")
        if not null_reload["passed"] or any(not (directory / f"null-reloaded.{ext}").is_file() for ext in ("fit", "pak")):
            return dict(name=case, passed=False, bucket="owned_objective_null_reload_failed", null_reload=null_reload)
    for extension, data in (("fit", before_fit), ("pak", before_pak)):
        (directory / f"input.{extension}").write_bytes(data)
        (report / f"input.{extension}").write_bytes(data)
    for extension in ("fit", "pak"):
        (directory / f"reloaded.{extension}").unlink(missing_ok=True)
    row = run_case_with_retry(case, exe, deploy, exit_sec, timeout,
        [f"-mission=terrain_gen_out/{case}/input.pak", f"-smoke-save=terrain_gen_out/{case}/reloaded.pak"], "saved")
    row["input_sha256"] = {"fit": hashlib.sha256(before_fit).hexdigest(), "pak": hashlib.sha256(before_pak).hexdigest()}
    if crud:
        row["null_reload"] = null_reload
    if row["passed"]:
        try:
            after = (directory / "reloaded.fit").read_bytes()
            after_pak = (directory / "reloaded.pak").read_bytes()
            if typed_fields(objective_fields(before_fit)) != typed_fields(objective_fields(after)):
                raise ValueError("legacy reload changed objective scalar/list values")
            if crud:
                from editor.tests.legacy_reference import buildings, packet
                null_fit = (directory / "null-reloaded.fit").read_bytes()
                null_pak = (directory / "null-reloaded.pak").read_bytes()
                if typed_fields(objective_fields(fixture_fit)) != typed_fields(objective_fields(null_fit)):
                    raise ValueError("legacy null reload changed objective values")
                # Legacy position quantization requires a same-run null-save control.
                for data in (null_pak, after_pak):
                    packet(data, 0)
                    count = struct.unpack_from("<I", data, 4)[0] // 4 - 2
                    for index in range(1, count):
                        packet(data, index)
                if Counter(buildings(after_pak)) != Counter(buildings(null_pak)):
                    raise ValueError("legacy CRUD reload changed PAK placement records")
                (report / "null-reloaded.fit").write_bytes(null_fit)
                (report / "null-reloaded.pak").write_bytes(null_pak)
            for extension, data in (("fit", after), ("pak", after_pak)):
                (report / f"reloaded.{extension}").write_bytes(data)
            row["detail"] = ("Owned CRUD edit" if crud else "Owned ResourcePoints edit") + " accepted and re-saved by legacy reader; all objective counts/order/content retained." + (" PAK placement records retained; archive packets decode." if crud else "") + " Runtime outcomes remain unproven."
        except (OSError, ValueError, KeyError, struct.error) as error:
            row.update(passed=False, bucket="owned_objective_reload_changed", detail=str(error))
    return row


def _gen_foliage_json(deploy: Path, instances: bool) -> bool:
    """Produce <deploy>/terrain_gen_out/genmap.foliage.json via the deployed
    generator. instances=True -> real rules (placed trees); the editor's later
    -gen-map run does not pass --generate-foliage so it won't clobber this file."""
    tg = deploy / "tools" / "terrain_gen" / "terrain_gen.py"
    rules = deploy / "tools" / "terrain_gen" / "recipes" / "foliage_rules_example.json"
    if not tg.exists():
        print(f"  [setup] generator not found at {tg}; skipping foliage pre-gen")
        return False
    out = deploy / "terrain_gen_out"
    out.mkdir(parents=True, exist_ok=True)
    recipe = out / "smoke_genmap_recipe.json"
    recipe.write_text('{"version":1,"name":"genmap","size":60,'
                      '"biome":"temperate_hills","seed":7}\n', encoding="utf-8")
    argv = ["py", "-3", str(tg), str(recipe), "--out", str(out), "--generate-foliage"]
    if rules.exists():
        argv += ["--foliage-rules", str(rules)]
    r = subprocess.run(argv, cwd=str(deploy), capture_output=True, text=True,
                       **nofocus.hidden_kwargs())
    ok = (r.returncode == 0) and (out / "genmap.foliage.json").exists()
    print(f"  [setup] foliage pre-gen rc={r.returncode} present={ok}")
    return ok


def _write_garbage(deploy: Path) -> Path:
    out = deploy / "terrain_gen_out"
    out.mkdir(parents=True, exist_ok=True)
    p = out / "garbage.foliage.json"
    p.write_bytes(b'{ this is not valid json ]]] "instances" 0xDEADBEEF \x00\x01\x02')
    return p


def _parse_esmoke(text: str) -> dict:
    m = re.search(r"\[ESMOKE v1\] event=summary (.+)", text)
    if not m:
        return {}
    fields = {}
    for kv in m.group(1).split():
        if "=" in kv:
            k, v = kv.split("=", 1)
            fields[k] = v
    return fields


def run_case(name: str, exe: Path, deploy: Path, exit_sec: int, timeout: int,
             extra_flags: list, expect) -> dict:
    """Launch the editor for one case; return a result dict with verdict."""
    log = deploy / "editor-startup.log"
    # Stale outputs cannot prove a save, including on the access-violation retry.
    destinations = []
    for flag in extra_flags:
        if flag.startswith("-smoke-save="):
            destinations.append(flag.split("=", 1)[1].replace("\\", "/"))
    if "-smoke-workflows" in extra_flags:
        destinations += ["terrain_gen_out/e0_authored.pak", "terrain_gen_out/e0_objectives.fit", "terrain_gen_out/e0_campaign.fit"]
    if "-smoke-catalog-reference" in extra_flags:
        from editor.tests.legacy_reference import PLACEMENTS
        destinations += [f"terrain_gen_out/e5_reference_{name}.pak" for name in ("before", *PLACEMENTS)]
    for destination in destinations:
        output = deploy / destination
        output.parent.mkdir(parents=True, exist_ok=True)
        for stale in (output, output.with_suffix(".fit"), Path(str(output) + ".old"), Path(str(output.with_suffix(".fit")) + ".old")):
            stale.unlink(missing_ok=True)
    try:
        if log.exists():
            log.unlink()
    except OSError:
        pass

    # -smoke-exit-sec drives a reliable time-based hard exit(0) after the engine is
    # up + the map generated (the -frames counter path is unreliable and a WM_CLOSE
    # stalls on the save-changes modal in a headless run). extra_flags carries the
    # map source (-gen-map / -mission) plus any -smoke-foliage / -smoke-save.
    argv = [str(exe), f"-smoke-exit-sec={exit_sec}", "-exit-on-load-fail"] + extra_flags
    env = dict(os.environ)
    env["MC2_EDITOR_TRACE"] = "1"
    env["MC2_ACTIVE_MOD"] = ""
    env["MC2_MOD_DEPS"] = ""
    env["MC2_LAUNCHER_ENV_JSON"] = "0"

    # Launch minimized + non-activating so the smoke does not steal focus or pop a
    # window over the user's work. The editor's MFC main frame honors nCmdShow from
    # the process STARTUPINFO on its first ShowWindow, and the GL viewport is a child
    # of that frame, so the whole editor comes up minimized.
    #
    # BACKGROUND-SMOKE-1: this is now UNCONDITIONAL (nofocus), not gated on
    # MINIMIZED. --minimized survives only as a no-op alias; it must never again
    # be what decides whether the run steals focus.
    print(f"[case {name}] launch: {' '.join(extra_flags) or '(gen-map only)'}  exit_sec={exit_sec}"
          f"{'  [minimized]' if MINIMIZED else ''}")
    t0 = time.time()
    timed_out = False
    try:
        proc = nofocus.spawn(argv, cwd=str(deploy), env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            out, _ = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            out, _ = proc.communicate()
        rc = proc.returncode
    except FileNotFoundError:
        return dict(name=name, passed=False, bucket="exe_missing",
                    detail=f"editor exe not found: {exe}")
    except (OSError, host_lease.HostLeaseBusy, nofocus.ExeRefused) as exc:
        return dict(name=name, passed=False, bucket="launch_refused", detail=str(exc), rc="-", seconds=round(time.time() - t0, 1), esmoke={})
    dt = time.time() - t0

    logtext = ""
    if log.exists():
        logtext = log.read_text(errors="replace")
    combined = (out or "") + "\n" + logtext
    esmoke = _parse_esmoke(combined)

    # Buckets (first match wins) -------------------------------------------------
    bucket = ""
    low = combined.lower()
    if timed_out:
        bucket = "timeout"
    elif rc != 0:
        bucket = "nonzero_exit"
    elif not esmoke:
        bucket = "no_summary"
    elif esmoke.get("clean_exit") != "1":
        bucket = "unclean_exit"
    elif esmoke.get("autoload") != "1":
        bucket = "autoload_missing"
    else:
        for tok in CRASH_TOKENS:
            if tok.lower() in low:
                bucket = f"crash_token:{tok}"
                break

    # Per-case expectation -------------------------------------------------------
    detail = ""
    if not bucket and expect == "workflows":
        detail = grade_workflows(combined)
        if detail:
            bucket = "workflow_probe_failed"
        elif esmoke.get("saved") != "1":
            bucket, detail = "save_failed", "workflow export did not save"
    if not bucket and expect:
        fc = int(esmoke.get("foliage_count", "-1"))
        if expect == "foliage_present" and fc <= 0:
            bucket = "foliage_empty"; detail = f"expected foliage_count>0, got {fc}"
        if expect == "foliage_tolerated":
            # missing/garbage JSON must load to 0 instances WITHOUT crashing.
            # Reaching a clean summary with foliage_smoke=1 is the pass condition;
            # count is expected to be 0.
            if esmoke.get("foliage_smoke") != "1":
                bucket = "foliage_not_run"; detail = "foliage smoke step did not run"
        if expect == "saved" and esmoke.get("saved") != "1":
            bucket = "save_failed"; detail = f"expected saved=1, got {esmoke.get('saved')}"
        if expect == "menu":
            # Drove the foliage menu commands via WM_COMMAND: visibility must toggle
            # off (vis1) then back on (vis2), Clear must drop the count to 0, and
            # Generate/reload must place instances again (>0).
            v0, v1, v2 = esmoke.get("menu_vis0"), esmoke.get("menu_vis1"), esmoke.get("menu_vis2")
            clr, rld = esmoke.get("menu_clear"), esmoke.get("menu_reload")
            if not (v0 == "1" and v1 == "0" and v2 == "1"):
                bucket = "menu_toggle_bad"; detail = f"visibility {v0}->{v1}->{v2} (want 1,0,1)"
            elif clr != "0":
                bucket = "menu_clear_bad"; detail = f"count after Clear = {clr} (want 0)"
            elif not (rld and rld.lstrip("-").isdigit() and int(rld) > 0):
                bucket = "menu_reload_bad"; detail = f"count after reload = {rld} (want >0)"
        if expect == "outliner":
            # Read-only Scene Outliner enumerated placed objects. An auto-generated
            # empty map legitimately has 0 objects (selected=0); the pass condition
            # is that the step ran (count present, >=0) and the select-first result
            # is consistent: objects present -> selected, none -> not selected.
            cnt = esmoke.get("outliner_count")
            sel = esmoke.get("outliner_selected")
            if cnt is None or not cnt.lstrip("-").isdigit() or int(cnt) < 0:
                bucket = "outliner_not_run"; detail = f"outliner_count={cnt} (want >=0)"
            elif sel not in ("0", "1"):
                bucket = "outliner_sel_bad"; detail = f"outliner_selected={sel} (want 0/1)"
            elif int(cnt) > 0 and sel != "1":
                bucket = "outliner_sel_bad"; detail = f"count={cnt} but selected={sel} (want 1)"
            elif int(cnt) == 0 and sel != "0":
                bucket = "outliner_sel_bad"; detail = f"count=0 but selected={sel} (want 0)"
        if expect == "inspector":
            # Read-only Inspector produced a selection summary. An auto-generated
            # empty map has nothing to select (selected=0, type=none); a non-empty
            # one selects the first object (selected=1, type != none). Either way
            # the step must run crash-free with a consistent type token.
            isel = esmoke.get("inspector_selected")
            itype = esmoke.get("inspector_type")
            if isel not in ("0", "1"):
                bucket = "inspector_not_run"; detail = f"inspector_selected={isel} (want 0/1)"
            elif isel == "1" and (not itype or itype == "none"):
                bucket = "inspector_type_bad"; detail = f"selected=1 but type={itype}"
            elif isel == "0" and itype != "none":
                bucket = "inspector_type_bad"; detail = f"selected=0 but type={itype} (want none)"
        if expect == "validate":
            # The live validator ran on a freshly generated empty map (no units),
            # so it MUST report a unit-staffing warning (no player/enemy units),
            # and at least one warning overall. No blocking failures are expected
            # on a generated map (terrain is loaded). This proves the save-time
            # warnings are now visible pre-save without a modal.
            vw = esmoke.get("validate_warning")
            uw = esmoke.get("validate_units_warn")
            if uw != "1":
                bucket = "validate_no_staffing_warn"
                detail = f"expected unit-staffing warning on empty map, units_warn={uw}"
            elif not (vw and vw.lstrip("-").isdigit() and int(vw) >= 1):
                bucket = "validate_no_warning"; detail = f"expected warning>=1, got {vw}"
        if expect == "inspector_edit":
            # End-to-end editable Inspector: a throwaway object is transformed via
            # applyObjectTransform (must move it) then undone (must restore it).
            # Both must succeed -- this proves the Action/undo path, not just a
            # direct mutation.
            ea = esmoke.get("inspector_edit_applied")
            eu = esmoke.get("inspector_edit_undo")
            if ea != "1":
                bucket = "edit_not_applied"; detail = f"transform did not move object, applied={ea}"
            elif eu != "1":
                bucket = "edit_undo_failed"; detail = f"undo did not restore position, undo={eu}"
        if expect == "asset_browser":
            # The browser enumerated the existing object catalog and activated the
            # first placeable object through the existing placement path. Expect a
            # non-empty catalog and a brush activation.
            ag = esmoke.get("asset_groups")
            aa = esmoke.get("asset_activated")
            if not (ag and ag.lstrip("-").isdigit() and int(ag) >= 1):
                bucket = "asset_no_groups"; detail = f"expected groups>=1, got {ag}"
            elif aa != "1":
                bucket = "asset_not_activated"; detail = f"placement not activated, activated={aa}"
        if expect == "place_oob":
            # Off-map BuildingBrush placement must be clamped to the valid cell
            # grid (regression guard for the terrainElevation 0xC0000005 crash).
            # survived=1 means every far-off-map probe snapped back in-grid; the
            # unguarded build returns 0 (snapped result still off the grid).
            so = esmoke.get("place_oob_survived")
            if so != "1":
                bucket = "place_oob_unguarded"
                detail = f"off-map placement not clamped, survived={so} (want 1)"

    passed = (bucket == "")
    # The gate registry's startup dump (Gates/, written to stderr by EditorMFCApp::InitInstance).
    config = [ln.strip() for ln in (out or "").splitlines()
              if ln.startswith("[CONFIG v1]") and ("summary" in ln or "warn=" in ln or "fatal=" in ln)]
    return dict(name=name, passed=passed, bucket=bucket, detail=detail,
                rc=rc, seconds=round(dt, 1), esmoke=esmoke, config=config, captured=combined)


# Windows STATUS_ACCESS_VIOLATION (0xC0000005) returncode, in both the unsigned
# and signed forms Python's subprocess may report it as.
ACCESS_VIOLATION_RCS = (3221225477, -1073741819)


def run_case_with_retry(name, exe, deploy, exit_sec, timeout, extra_flags, expect):
    """run_case + ONE automatic retry, but ONLY for an 0xC0000005 access violation.

    This is the visible-quarantine policy for the known editor launch flake: the
    GUI editor occasionally crashes on process teardown/startup contention during
    back-to-back smoke launches (foliage_present / foliage_menu_commands seen).
    We do NOT silently swallow it -- a case that crashes then passes on retry is
    reported as FLAKY_PASS (counts toward green, but the suite header flags it so
    the run is never mistaken for strictly clean). A double crash stays a FAIL.

    Only 0xC0000005 is retried; every other failure (timeout, bad verdict,
    nonzero non-AV exit) is returned as-is, with no retry.
    """
    r = run_case(name, exe, deploy, exit_sec, timeout, extra_flags, expect)
    if r.get("passed") or r.get("rc") not in ACCESS_VIOLATION_RCS:
        return r

    print(f"[case {name}] FAIL rc=0xC0000005 (access violation) -> retry 1/1 "
          f"(known editor launch flake)")
    r2 = run_case(name, exe, deploy, exit_sec, timeout, extra_flags, expect)
    r2["flaky"] = True
    if r2.get("passed"):
        # Crash-then-pass: green, but explicitly labelled, never hidden.
        r2["result_label"] = "FLAKY_PASS"
        r2["detail"] = ("0xC0000005 on attempt 1, PASS on retry "
                        "(known back-to-back-launch flake)")
        print(f"[case {name}] retry 1/1 PASS -> FLAKY_PASS")
    else:
        r2["detail"] = (f"0xC0000005 on attempt 1; retry bucket={r2.get('bucket')} "
                        f"rc={r2.get('rc')} (flaky x2 -> hard FAIL)")
        print(f"[case {name}] retry 1/1 FAIL -> hard FAIL")
    return r2


def rand_gen_flag():
    """Random map: size index 0..3 (cellSide 60/80/.../label) x terrain type 0..15.
    The user asked for randomized settings (mountains/terrain/size)."""
    import hashlib
    # Avoid Math.random-style nondeterminism complaints: seed from PID+clock once.
    seed = (os.getpid() ^ int(time.time())) & 0xffff
    size = seed % 2            # 0..1 (keep the save/load smoke fast; big maps crawl)
    terrain = (seed // 2) % 16  # 0..15 (random terrain type, incl. mountains/snow/etc.)
    return f"-gen-map={size},{terrain}", size, terrain


def build_suite(deploy: Path):
    cases = {
        "gen_map_basic":   (["-gen-map=0,0"], None),                    # launch+generate+render+exit
        "foliage_present": (["-gen-map=0,0", "-smoke-foliage"], "foliage_present"),
        "foliage_missing": (["-gen-map=0,0", "-smoke-foliage=terrain_gen_out\\__does_not_exist__.json"], "foliage_tolerated"),
        "foliage_garbage": (["-gen-map=0,0", "-smoke-foliage=terrain_gen_out\\garbage.foliage.json"], "foliage_tolerated"),
        "foliage_menu_commands": (["-gen-map=0,0", "-smoke-foliage-menu"], "menu"),
        "outliner":        (["-gen-map=0,0", "-smoke-outliner"], "outliner"),  # read-only Scene Outliner
        "inspector":       (["-gen-map=0,0", "-smoke-inspector"], "inspector"),  # read-only Inspector
        "validate":        (["-gen-map=0,0", "-smoke-validate"], "validate"),  # live Mission validator
        "inspector_edit":  (["-gen-map=0,0", "-smoke-inspector-edit"], "inspector_edit"),  # transform+undo
        "asset_browser":   (["-gen-map=0,0", "-smoke-asset-browser"], "asset_browser"),  # catalog + activate
        "gameplay_debugger": (["-gen-map=0,0", "-smoke-gameplay-debugger"], None),  # read-only debugger (no-crash)
        "undo_history":    (["-gen-map=0,0", "-smoke-undo-history"], None),  # display-only undo panel (no-crash)
        "place_oob":       (["-gen-map=0,0", "-smoke-place-oob"], "place_oob"),  # off-map placement OOB crash guard
        # gen_save_load is handled specially (two phases) in main().
    }
    return cases


def grade_new_model(rows: list[dict], directory: Path) -> None:
    """Exactly 24 new-document comparisons; facts never replace byte comparisons."""
    if (len(rows) != 24 or {row.get("mission") for row in rows} != set(STOCK_MISSIONS)
            or any(row.get("status") != "PASS" or row.get("rc") != 0 for row in rows)):
        raise ValueError("new model requires exactly 24 fresh PASS rows")
    for row in rows:
        mission = row["mission"]
        facts = row.get("facts", {})
        if (facts.get("result") != "PASS" or facts.get("frames") != "3"
                or any(facts.get(key) != "1" for key in ("roundtrip", "edited", "reopened", "undo", "redo", "malformed", "unchanged", "long_name"))):
            raise ValueError(f"new model facts missing: {mission}")
        for extension in ("fit", "pak"):
            original = (directory / "input" / f"{mission}.{extension}").read_bytes()
            if hashlib.sha256(original).hexdigest() != row.get(f"input_{extension}_sha256"):
                raise ValueError(f"new model input changed: {mission}.{extension}")
            for stage in ("roundtrip", "undone"):
                written = (directory / mission / f"{stage}.{extension}").read_bytes()
                if original != written:
                    raise ValueError(f"new model byte mismatch: {mission}/{stage}.{extension}")
            edited = (directory / mission / f"edited.{extension}").read_bytes()
            if extension == "pak" and edited != original:
                raise ValueError(f"new model edited PAK changed: {mission}")
            if extension == "fit":
                fields = fit_fields(edited)
                if (edited == original or fields.get("MissionSettings/MissionName") != ['st "Q3 authored mission"']):
                    raise ValueError(f"new model authored edit missing: {mission}")
                long_fields = fit_fields((directory / mission / "long_name.fit").read_bytes())
                expected = 'st "Q3 long mission ' + 'x' * 8192 + ' retained tail"'
                if long_fields.get("MissionSettings/MissionName") != [expected]:
                    raise ValueError(f"new model long name truncated: {mission}")
            elif (directory / mission / "long_name.pak").read_bytes() != original:
                raise ValueError(f"new model long-name PAK changed: {mission}")
        log = (directory / mission / "shell.log").read_text(encoding="utf-8")
        if sum(line.startswith("[EDITOR_MODEL v1] ") and "mission=malformed.fit " in line and "rejected=1" in line for line in log.splitlines()) != 1:
            raise ValueError(f"new model malformed diagnostic missing: {mission}")


def run_new_model(exe: Path, stock: StockInputs, directory: Path, timeout: int) -> dict:
    directory.mkdir(exist_ok=False)
    (directory / "input").mkdir()
    rows = []
    provenance = {}
    start = time.time()
    try:
        if not exe.exists():
            raise ValueError(f"new shell executable missing: {exe}")
        if exe.parent != Path("A:/Games/mc2-opengl/mc2-win64-lane3").resolve():
            raise ValueError("new shell smoke is restricted to lane3")
        if not exe_guard.exe_has_offscreen(exe):
            raise ValueError("new shell lacks background-window support")
        fingerprint = exe_fingerprint.extract(exe)
        head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True,
                                       **child_kwargs(redirected=True)).strip()
        if not fingerprint.get("present") or fingerprint.get("sha12") != head[:12]:
            raise ValueError("new shell build fingerprint does not match worktree HEAD")
        provenance = dict(fingerprint=fingerprint, exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), head=head)
        host_lease.ensure_held("Q3 document SDL shell smoke")
        for mission in STOCK_MISSIONS:
            row = dict(mission=mission, status="FAIL", rc=None, facts={})
            for extension in ("fit", "pak"):
                source = stock.read(f"data/missions/{mission}.{extension}")
                (directory / "input" / f"{mission}.{extension}").write_bytes(source)
                row[f"input_{extension}_sha256"] = hashlib.sha256(source).hexdigest()
            output = directory / mission
            output.mkdir()
            command = [str(exe), "--smoke", "--mission", str(directory / "input" / f"{mission}.fit"),
                       "--output", str(output), "--log", str(output / "shell.log")]
            print(f"[new_model {mission}] fresh FIT + PAK", flush=True)
            process = nofocus.spawn(command, cwd=str(exe.parent), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, errors="replace")
            try:
                captured, _ = process.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                process.kill()
                captured, _ = process.communicate()
                row["status"] = "TIMEOUT"
            (output / "process.log").write_text(captured or "", encoding="utf-8")
            row["rc"] = process.returncode
            if (output / "shell.log").exists():
                lines = [line for line in (output / "shell.log").read_text(encoding="utf-8").splitlines()
                         if line.startswith("[EDITOR_SHELL v1] ")]
                if len(lines) == 1:
                    row["facts"] = dict(re.findall(r"(\w+)=([^\s]+)", lines[0]))
            if row["rc"] == 0 and row["facts"].get("result") == "PASS":
                row["status"] = "PASS"
            rows.append(row)
        grade_new_model(rows, directory)
        if hashlib.sha256(exe.read_bytes()).hexdigest() != provenance["exe_sha256"]:
            raise ValueError("new shell executable changed during the gate")
        # The same strict grader must reject real corrupted output, not a synthetic status.
        corrupt = directory / STOCK_MISSIONS[0] / "roundtrip.fit"
        original = corrupt.read_bytes()
        retained = corrupt.with_suffix(".negative-control.fit")
        retained.write_bytes(original + b"corrupted output")
        corrupt.write_bytes(retained.read_bytes())
        caught = False
        try:
            grade_new_model(rows, directory)
        except ValueError as error:
            caught = "byte mismatch" in str(error)
        finally:
            corrupt.write_bytes(original)
        if not caught:
            raise ValueError("new model corrupted-output negative control did not fail")
        grade_new_model(rows, directory)
        result = dict(name="new_model", passed=True, bucket="", detail="24/24 fresh FIT+PAK byte-identical; edit/reopen, undo/redo, malformed rejection, corrupted-output negative control PASS")
    except (OSError, KeyError, ValueError, host_lease.HostLeaseBusy, nofocus.ExeRefused) as error:
        result = dict(name="new_model", passed=False, bucket="new_model_gate", detail=str(error))
    result["seconds"] = round(time.time() - start, 1)
    (directory / "gate.json").write_text(json.dumps(dict(rows=rows, result=result, provenance=provenance), indent=2), encoding="utf-8")
    return result


PANEL_OPERATIONS = {
    "objectives_crud_panel": ("objective-add", "objective-copy", "objective-delete", "objective-up", "objective-down") +
        tuple(f"entry-{i}-{action}" for i in range(4) for action in ("add", "flag", "delete", "up", "down")),
    "objectives_panel": ("objective-resolve",) + tuple("objective-" + key.replace(" ", "_") for key in (
        "Title", "Description", "Priority", "ResourcePoints", "PreviousPrimaryObjectiveMustBeComplete",
        "AllPreviousPrimaryObjectivesMustBeComplete", "DisplayMarker", "MarkerX", "MarkerY", "HiddenTrigger",
        "ActivateOnFlag", "ActivateFlagID", "ResetStatusOnFlag", "ResetStatusFlagID", "TitleUseResourceString",
        "TitleResourceStringID", "DescriptionUseResourceString", "DescriptionResourceStringID", "ObjectiveModel",
        "ModelType", "BaseColor", "HighlightColor", "HighlightColor2", "ModelScale", "ModelID", "ModelID Version")) +
        tuple(f"widget-payload-{listing}-{key}" for listing in (1, 3)
              for key in ("AVIFileName", "WAVFileName", "Message", "PurchaseFileName", "ResourceStringID")) +
        tuple(f"widget-payload-{listing}-{key}" for listing in (0, 2)
              for key in ("Num", "Time", "TargetCenterX", "TargetCenterY", "TargetRadius", "create")),
    "unit_panel": ("unit-transform", "unit-team", "unit-place", "unit-delete"),
    "building_panel": ("building-transform", "building-team", "building-place", "building-delete"),
    "player_panel": ("player-alignment",),
    "brain_modes": ("brain-Hybrid", "brain-Enhanced", "brain-module", "brain-Legacy"),
    "panel_render": (),
    "marker_panel": ("marker-transform", "marker-delete"),
    "chooser": tuple("chooser-" + family for family in ("mech", "vehicle", "building", "turret", "tree", "gate", "bridge", "light", "marker")),
    "chooser_mod": ("chooser-mech",),
    "chooser_player": ("chooser-mech",),
    "chooser_render": ("chooser-mech",),
}

FILE_CASES = ("file_save", "file_save_as", "file_recent", "file_failures") + tuple(
    f"file_{action}_{decision}" for action in ("open", "reopen", "close")
    for decision in ("save", "discard", "cancel"))


def grade_file(name: str, directory: Path, rc: int) -> None:
    if rc != 0:
        raise ValueError(f"file process failed: {name} rc={rc}")
    lines = (directory / "shell.log").read_text(encoding="utf-8").splitlines()
    summaries = [dict(re.findall(r"(\w+)=([^\s]+)", line)) for line in lines if line.startswith("[EDITOR_FILE v1] ")]
    expected = dict(name=name, result="PASS", **{key: "1" for key in ("widgets", "changed", "undo", "redo", "reopened", "state")})
    if summaries != [expected]:
        raise ValueError("file workflow widget/state evidence missing or duplicated")
    loops = [dict(re.findall(r"(\w+)=([^\s]+)", line)) for line in lines if line.startswith("[EDITOR_FILE_LOOP v1] ")]
    closed = name in ("file_close_save", "file_close_discard")
    if loops != [dict(name=name, terminated="1" if closed else "0", complete="1")]:
        raise ValueError("file loop termination/completion evidence missing or duplicated")
    diagnostics = [line for line in lines if "rejected=1" in line]
    if len(diagnostics) != (5 if name == "file_failures" else 0):
        raise ValueError("file failure diagnostic count differs")
    for extension in ("fit", "pak"):
        original = (directory / f"baseline.{extension}").read_bytes()
        edited = (directory / f"edited.{extension}").read_bytes()
        if (directory / f"undone.{extension}").read_bytes() != original or (directory / f"redone.{extension}").read_bytes() != edited:
            raise ValueError("file workflow undo/redo bytes differ")
        if extension == "fit":
            if edited == original or b"x" * 8192 + b" retained tail" not in edited:
                raise ValueError("file workflow output unchanged or text truncated")
            if b"ul DefaultTeam = 3" not in edited or b"ul DefaultTeam = 0" not in original:
                raise ValueError("file workflow authored alignment missing")
        elif edited != original:
            raise ValueError("file workflow unrelated PAK changed")
        saved = edited if name in ("file_save", "file_failures", "file_open_save", "file_reopen_save", "file_close_save") else original
        if (directory / f"first.{extension}").read_bytes() != saved:
            raise ValueError("file lifecycle saved/discarded/cancelled bytes differ")
        if name in ("file_save_as", "file_recent"):
            destination = directory / ("mission with spaces Ω " + "p" * 100 + f" retained tail.{extension}")
            if destination.read_bytes() != edited:
                raise ValueError("file save-as destination or recent output differs")


def grade_panel(name: str, directory: Path, rc: int, require_render=True) -> None:
    if rc != 0:
        raise ValueError(f"panel process failed: {name} rc={rc}")
    lines = (directory / "shell.log").read_text(encoding="utf-8").splitlines()
    summaries = [dict(re.findall(r"(\w+)=([^\s]+)", line)) for line in lines
                 if line.startswith("[EDITOR_PANEL v1] ")]
    if summaries != [dict(name=name, result="PASS", widgets="1", rendered="1" if require_render else "0", saved_widget="1", reopened_widget="1")]:
        raise ValueError(f"panel summary missing or duplicated: {name}")
    operations = [dict(re.findall(r"(\w+)=([^\s]+)", line)) for line in lines
                  if line.startswith("[EDITOR_PANEL_OPERATION v1] ")]
    expected = PANEL_OPERATIONS[name]
    if (len(operations) != len(expected) or {row.get("name") for row in operations} != set(expected)
            or any(any(row.get(key) != "1" for key in ("changed", "reopened", "undo", "redo", "widgets")) for row in operations)):
        raise ValueError(f"panel operation reach missing: {name}")
    for operation in expected:
        baseline = "existing-hybrid" if operation == "brain-Legacy" else "fixture"
        changed = False
        for extension in ("fit", "pak"):
            original = (directory / f"{baseline}.{extension}").read_bytes()
            edited = (directory / f"{operation}-edited.{extension}").read_bytes()
            undone = (directory / f"{operation}-undone.{extension}").read_bytes()
            redone = (directory / f"{operation}-redone.{extension}").read_bytes()
            if undone != original or redone != edited:
                raise ValueError(f"panel undo/redo bytes differ: {operation}.{extension}")
            changed |= original != edited
            if extension == "pak" and not operation.startswith(("building-", "marker-", "chooser-")) and edited != original:
                raise ValueError(f"unrelated panel PAK changed: {operation}")
        if not changed:
            raise ValueError(f"panel changed output missing: {operation}")
    if name == "brain_modes":
        for extension in ("fit", "pak"):
            if (directory / f"existing-hybrid.{extension}").read_bytes() != (directory / f"hybrid-roundtrip.{extension}").read_bytes():
                raise ValueError("existing Hybrid bytes changed")
        if b'compatibilityMode = "Hybrid";' not in (directory / "brain-Hybrid-edited.fit").read_bytes():
            raise ValueError("Hybrid assignment missing")
    saved_baseline = (expected[-1] + "-edited" if name.startswith("chooser") or name in ("objectives_panel", "objectives_crud_panel") else
                      "existing-hybrid" if name == "brain_modes" else "fixture")
    for extension in ("fit", "pak"):
        if (directory / f"widget-save.{extension}").read_bytes() != (directory / f"{saved_baseline}.{extension}").read_bytes():
            raise ValueError("save widget output differs")
    if require_render:
        capture = (directory / "panels.bmp").read_bytes()
        if len(capture) != 54 + 1440 * 960 * 3 or capture[:2] != b"BM" or len(set(capture[54:])) < 10:
            raise ValueError("rendered panel evidence missing or empty")
    if name.startswith("chooser"):
        grade_chooser(name, directory, lines)
    elif name == "marker_panel":
        grade_marker_edits(directory)
    elif name == "objectives_panel":
        from editor.tests.objective_census import AUTHORING_KEYS, grade_objective_edit, grade_reference_rows, grade_payload_edit, grade_condition_creation, objective_fields
        grade_reference_rows(directory)
        for key in AUTHORING_KEYS:
            grade_objective_edit((directory / "fixture.fit").read_bytes(),
                                 (directory / ("objective-" + key.replace(" ", "_") + "-edited.fit")).read_bytes(), key)
        baseline = (directory / "fixture.fit").read_bytes()
        fields = objective_fields(baseline)
        for listing, suffix in ((0, "Condition"), (2, "FailureCondition")):
            for key in ("Num", "Time", "TargetCenterX", "TargetCenterY", "TargetRadius"):
                candidates = [path.partition("/")[0] for path in fields if path.endswith("/" + key)
                              and re.fullmatch(r"Team0Objective\d+" + suffix + r"\d+", path.partition("/")[0])]
                block = max(candidates, key=lambda name: int(re.search(r"Objective(\d+)", name)[1]))
                grade_payload_edit(baseline, (directory / f"widget-payload-{listing}-{key}-edited.fit").read_bytes(), block, key)
            owner = re.match(r"Team0Objective\d+", block)[0]
            grade_condition_creation(baseline, (directory / f"widget-payload-{listing}-create-edited.fit").read_bytes(), owner, suffix)
        from editor.tests.objective_census import ACTION_PAYLOADS
        for listing, suffix in ((1, "Action"), (3, "FailureAction")):
            for index, (_, key) in enumerate(ACTION_PAYLOADS):
                grade_payload_edit(baseline, (directory / f"widget-payload-{listing}-{key}-edited.fit").read_bytes(),
                                   f"Team0Objective1{suffix}{index}", key)
    elif name == "objectives_crud_panel":
        from editor.tests.objective_crud import grade_crud_panel
        grade_crud_panel(directory)


def grade_marker_edits(directory):
    from editor.tests.legacy_reference import buildings, marker_values
    grade_marker_source((directory / "marker-transform-edited.fit").read_bytes(),
                        (directory / "marker-transform-edited.pak").read_bytes(), "moved")
    grade_marker_source((directory / "marker-delete-edited.fit").read_bytes(),
                        (directory / "marker-delete-edited.pak").read_bytes(), "deleted")
    original = placement_fields((directory / "fixture.fit").read_bytes())
    moved = placement_fields((directory / "marker-transform-edited.fit").read_bytes())
    deleted = placement_fields((directory / "marker-delete-edited.fit").read_bytes())
    before_records = Counter(buildings((directory / "fixture.pak").read_bytes()))
    move_records = Counter(buildings((directory / "marker-transform-edited.pak").read_bytes()))
    delete_records = Counter(buildings((directory / "marker-delete-edited.pak").read_bytes()))
    added = list((move_records - before_records).elements())
    removed = list((before_records - delete_records).elements())
    if len(added) != 1 or len(removed) != 1 or before_records - move_records != Counter(removed) or delete_records - before_records:
        raise ValueError("marker packet edit changed unrelated records")
    changed_markers = marker_values(moved) - marker_values(original)
    if sum(changed_markers.values()) != 1:
        raise ValueError("marker move did not change one FIT record")
    marker = dict(next(iter(changed_markers.elements())))
    object_id, x, y = struct.unpack_from("<Iff", added[0])
    if object_id != 290 or x != -4000 or marker.get("xPos") != (("f", (x,)),) or marker.get("yPos") != (("f", (y,)),):
        raise ValueError("marker desync negative control: FIT and packet positions differ")
    if sum(marker_values(original).values()) != sum(marker_values(deleted).values()) + 1:
        raise ValueError("marker delete FIT count differs")
    if sum(marker_values(original).values()) != sum(marker_values(moved).values()):
        raise ValueError("marker move FIT count differs")


def grade_chooser(name, directory, lines):
    from editor.tests.legacy_reference import PLACEMENTS, compare_frozen, inserted_unit, typed_fields, verify_frozen
    from editor.tests.run_catalog_proofs import oracle
    families = [operation.removeprefix("chooser-") for operation in PANEL_OPERATIONS[name]]
    # Identical placements require identical widget/reference byte streams.
    # Legacy parity compares decoded content, not archive encoding.
    for extension in ("fit", "pak"):
        if (directory / f"fixture.{extension}").read_bytes() != (directory / f"e5_reference_before.{extension}").read_bytes():
            raise ValueError(f"chooser widget/reference baseline bytes differ: {extension}")
    for family in families:
        for extension in ("fit", "pak"):
            reference = (directory / f"e5_reference_{family}.{extension}").read_bytes()
            for stage in ("edited", "redone"):
                if (directory / f"chooser-{family}-{stage}.{extension}").read_bytes() != reference:
                    raise ValueError(f"chooser widget/reference bytes differ: {family}-{stage}.{extension}")
            if family == families[-1] and (directory / f"widget-save.{extension}").read_bytes() != reference:
                raise ValueError(f"chooser widget/reference bytes differ: widget-save.{extension}")
    views = [dict(re.findall(r"(\w+)=([^\s]+)", line)) for line in lines if line.startswith("[EDITOR_CHOOSER_VIEW v1] ")]
    if views != [dict(family=family, stage=stage, changed="1", widgets="1") for family in families for stage in ("browse", "search")]:
        raise ValueError("chooser browse/search widget evidence missing or duplicated")
    independent = oracle(Path("A:/Games/mc2-opengl/mc2-win64-lane3"), Path("A:/Games/mc2-opengl"),
                         "MC2X-CVE-G" if name == "chooser_mod" else "")
    for family in families:
        preview = (directory / f"{family}-preview.tsv").read_text().strip().split("\t")
        if len(preview) != 5: raise ValueError("chooser preview identity incomplete")
        expected_id, stem = (2, preview[1]) if name == "chooser_mod" else PLACEMENTS[family]
        if len(preview) != 5 or preview[:3] != [str(expected_id), stem, family]:
            raise ValueError("chooser preview identity differs")
        for stage in ("browse", "search"):
            before = (directory / f"{family}-{stage}-before.tsv").read_bytes()
            after = (directory / f"{family}-{stage}-after.tsv").read_bytes()
            rows = [row.split("\t") for row in after.decode().splitlines()]
            if before == after or not rows or any(len(row) != 5 or row[2] != family for row in rows):
                raise ValueError("chooser filter changed-output missing")
            if stage == "search" and (any(stem.lower() not in row[1].lower() for row in rows) or preview not in rows):
                raise ValueError("chooser search output differs")
        sources = [row.split("\t") for row in (directory / f"{family}-sources.tsv").read_text().splitlines()]
        if not sources or any(len(row) != 3 for row in sources) or len({row[0] for row in sources}) != len(sources):
            raise ValueError("chooser resolved sources missing or duplicated")
        for key, layer, source in sources:
            _, wanted_layer, wanted_source = independent(key)
            if layer != wanted_layer or Path(source).resolve() != wanted_source.resolve():
                raise ValueError("chooser resolved provenance differs")
        definition = next((row for row in sources if row[0] == "data/objects/object2.pak"), None)
        if not definition or preview[3:] != definition[1:] or (name == "chooser_mod" and preview[3] != "mod:MC2X-CVE-G"):
            raise ValueError("chooser overridden preview provenance differs")
        required = {"data/art/buildings.csv", "data/objects/object2.pak"}
        variant_key = "data/objects/" + stem.lower() + ".csv"
        if _oracle_exists(independent, variant_key): required.add(variant_key)
        if family in ("mech", "vehicle"):
            fields = placement_fields((directory / f"chooser-{family}-edited.fit").read_bytes())
            prefix = chooser_unit_prefix(fields, expected_id)
            pilot = int(fields[prefix + "Pilot"][0].split(" ", 1)[1])
            profile = fields[f"Warrior{pilot}/Profile"][0][4:-1]
            required.update({"data/objects/badpilots.csv" if name == "chooser_player" else "data/objects/pilots.csv",
                             "data/missions/warriors/" + profile.lower() + ".fit"})
        if {row[0] for row in sources} != required:
            raise ValueError("chooser consumed provenance coverage differs")
    if name == "chooser":
        result = compare_frozen(directory, "\n".join(lines))
        (directory / "frozen-grade.json").write_text(json.dumps(result, indent=2))
    elif name == "chooser_player":
        from editor.tests.run_document_proofs import grade_player_defaults
        from editor.tests.legacy_reference import preserve_authored_units, canonical_units
        table = independent("data/objects/badpilots.csv")[0]
        if table.startswith(b"\x78"): table = zlib.decompress(table)
        profile = next(profile for profile in table.decode("ascii").splitlines() if profile and
                       _oracle_exists(independent, "data/missions/warriors/" + profile.lower() + ".fit"))
        fields = placement_fields((directory / "chooser-mech-edited.fit").read_bytes())
        before = placement_fields((directory / "fixture.fit").read_bytes())
        grade_player_defaults(fields, 2, 53, profile)
        preserve_authored_units(before, fields)
        if canonical_units(fields, 30, 30) != canonical_units(before):
            raise ValueError("chooser player insertion changed authored units")
    else:
        fields = placement_fields((directory / "chooser-mech-edited.fit").read_bytes())
        reference = verify_frozen()["placements"]["mech"]
        prefix = chooser_unit_prefix(fields, expected_id)
        row = dict(part={key: value for key, value in fields.items() if key.startswith(prefix)}, fitFields=fields)
        actual, expected = inserted_unit(row), inserted_unit(reference)
        if name == "chooser_mod":
            expected["Part/ObjectNumber"] = typed_fields({"x": ["ul 2"]})["x"]
            expected["Part/CSVFile"] = typed_fields({"x": [f'st "{stem}"']})["x"]
        if actual != expected:
            raise ValueError("chooser complete placement fields differ")


def chooser_unit_prefix(fields, object_id):
    prefixes = [key.removesuffix("ObjectNumber") for key, values in fields.items() if key.endswith("/ObjectNumber") and
                values == [f"ul {object_id}"] and fields.get(key.removesuffix("ObjectNumber") + "PositionX") and
                fields.get(key.removesuffix("ObjectNumber") + "PositionY") and
                float(fields[key.removesuffix("ObjectNumber") + "PositionX"][0].split(" ", 1)[1]) == -4288 and
                float(fields[key.removesuffix("ObjectNumber") + "PositionY"][0].split(" ", 1)[1]) == 4288]
    if len(prefixes) != 1: raise ValueError("chooser inserted unit identity missing or ambiguous")
    return prefixes[0]


def objective_negative_controls(directory):
    """Consistent edited/redo files must still fail when the widget save is wrong or unchanged."""
    paths = [directory / "objective-ResourcePoints-edited.fit", directory / "objective-ResourcePoints-redone.fit"]
    originals = [path.read_bytes() for path in paths]
    baseline = (directory / "fixture.fit").read_bytes()
    wrong = originals[0].replace(b"ul ResourcePoints = 17", b"ul ResourcePoints = 18")
    if wrong == originals[0]:
        raise ValueError("objective wrong-value control did not change output")
    rendered = " rendered=1" in (directory / "shell.log").read_text()
    for data in (baseline, wrong):
        caught = False
        try:
            for path in paths:
                path.write_bytes(data)
            grade_panel("objectives_panel", directory, 0, require_render=rendered)
        except ValueError:
            caught = True
        finally:
            for path, original in zip(paths, originals):
                path.write_bytes(original)
        if not caught:
            raise ValueError("objective wrong/no-op widget output was accepted")
    row_path = directory / "objective-rows-resolved.tsv"
    rows = row_path.read_bytes()
    resolve_paths = [directory / f"objective-resolve-{stage}.fit" for stage in ("edited", "redone")]
    resolved = [path.read_bytes() for path in resolve_paths]
    controls = (rows.split(b"\n", 1)[1], rows.replace(b" / Part ", b" / WrongPart ", 1), None)
    for control in controls:
        caught = False
        try:
            if control is None:
                for path in resolve_paths: path.write_bytes(baseline)
            else:
                row_path.write_bytes(control)
            grade_panel("objectives_panel", directory, 0, require_render=rendered)
        except ValueError:
            caught = True
        finally:
            row_path.write_bytes(rows)
            for path, original in zip(resolve_paths, resolved): path.write_bytes(original)
        if not caught:
            raise ValueError("objective missing row/wrong target/no-op resolve was accepted")
    for listing in (0, 2):
        paths = [directory / f"widget-payload-{listing}-create-{stage}.fit" for stage in ("edited", "redone")]
        originals = [path.read_bytes() for path in paths]
        begin = originals[0].rfind(b"f PositionX = ")
        end = originals[0].find(b"\r\n", begin)
        if begin < 0 or end < 0:
            raise ValueError("specific creation negative control lacks XY")
        skipped = originals[0][:begin] + b"f PositionX = -1e30" + originals[0][end:]
        caught = False
        try:
            for path in paths: path.write_bytes(skipped)
            grade_panel("objectives_panel", directory, 0, require_render=rendered)
        except ValueError:
            caught = True
        finally:
            for path, original in zip(paths, originals): path.write_bytes(original)
        if not caught:
            raise ValueError("game-skipped specific condition creation was accepted")


def crud_negative_controls(directory):
    rendered = (directory / "panels.bmp").exists()
    baseline = (directory / "fixture.fit").read_bytes()
    for operation in PANEL_OPERATIONS["objectives_crud_panel"]:
        paths = [directory / f"{operation}-{stage}.fit" for stage in ("edited", "redone")]
        originals = [path.read_bytes() for path in paths]
        wrong = originals[0].replace(b"ResourcePoints = ", b"ResourcePoints = 9", 1)
        swapped = originals[0].replace(b"[Team0Objective0]", b"[swap]", 1).replace(
            b"[Team0Objective1]", b"[Team0Objective0]", 1).replace(b"[swap]", b"[Team0Objective1]", 1)
        if wrong == originals[0]:
            raise ValueError("CRUD wrong-value negative control did not change output")
        controls = [baseline, wrong, swapped]
        if operation.startswith("entry-"):
            for source, replacement in ((b'e6c-first', b'wrong-id'), (b'Value = FALSE', b'Value = TRUE'),
                                        (b'BooleanFlagIsSet', b'UnknownCondition'), (b'SetBooleanFlag', b'UnknownAction'),
                                        (b'st FailureActionSpeciesString = "SetBooleanFlag"', b'ul FailureActionSpecies = 4')):
                changed = originals[0].replace(source, replacement, 1)
                if changed == originals[0]:
                    raise ValueError("flag/species negative control did not mutate output")
                controls.append(changed)
        for data in controls:
            caught = False
            try:
                for path in paths:
                    path.write_bytes(data)
                grade_panel("objectives_crud_panel", directory, 0, require_render=rendered)
            except ValueError:
                caught = True
            finally:
                for path, original in zip(paths, originals):
                    path.write_bytes(original)
            if not caught:
                raise ValueError("CRUD consistently wrong/no-op artifact was accepted")
        retained = paths[0].with_suffix(".missing-control.fit")
        paths[0].rename(retained)
        try:
            try:
                grade_panel("objectives_crud_panel", directory, 0, require_render=rendered)
            except FileNotFoundError:
                pass
            else:
                raise ValueError("CRUD missing artifact was accepted")
        finally:
            retained.rename(paths[0])


def panel_negative_controls(name, directory):
    if name == "objectives_crud_panel":
        crud_negative_controls(directory)
        return
    if name == "objectives_panel":
        objective_negative_controls(directory)
        return
    if not name.startswith("chooser") and name != "marker_panel": return
    if name.startswith("chooser"):
        path = directory / "chooser-mech-edited.fit"
        original = path.read_bytes()
        prefix = chooser_unit_prefix(placement_fields(original), 2 if name == "chooser_mod" else 53)
        start = original.index(("[" + prefix[:-1] + "]").encode())
        end = original.find(b"\n[", start + 1)
        if end < 0: end = len(original)
        body = original[start:end]
        corrupted, count = re.subn(rb"(?m)^(ul\s+VariantNumber\s*=\s*)0", lambda match: match[1] + b"1", body)
        if count != 1: raise ValueError("chooser corruption control did not alter one placed field")
        corrupted = original[:start] + corrupted + original[end:]
        # Matching history artifacts alone cannot prove a correct edit.
        paths = [path, directory / "chooser-mech-redone.fit"]
        if len(PANEL_OPERATIONS[name]) == 1: paths.append(directory / "widget-save.fit")
        lines = (directory / "shell.log").read_text().splitlines()
        rendered = any(line.startswith("[EDITOR_PANEL v1] ") and " rendered=1" in line for line in lines)
        grader = lambda: grade_panel(name, directory, 0, require_render=rendered)
    else:
        path = directory / "marker-transform-edited.fit"
        original = path.read_bytes()
        corrupted = original.replace(b"xPos = -4000", b"xPos = -2000")
        paths = [path]
        grader = lambda: grade_marker_edits(directory)
    if corrupted == original: raise ValueError("panel corruption control did not change output")
    originals = {target: target.read_bytes() for target in paths}
    for target in paths: target.with_suffix(".negative-control.fit").write_bytes(corrupted)
    caught = None
    try:
        for target in paths: target.write_bytes(corrupted)
        try: grader()
        except ValueError as error: caught = str(error)
    finally:
        for target, contents in originals.items(): target.write_bytes(contents)
    if not caught: raise ValueError("panel corrupted-output negative control was accepted")
    grader()
    (directory / "negative-control.json").write_text(json.dumps(dict(name=name, rejected=True, reason=caught), indent=2))


def _oracle_exists(independent, key):
    try:
        independent(key)
        return True
    except KeyError:
        return False


def run_panel(name: str, exe: Path, stock: StockInputs, directory: Path, timeout: int) -> dict:
    directory.mkdir(exist_ok=False)
    provenance = {}
    start = time.time()
    try:
        if exe.parent != Path("A:/Games/mc2-opengl/mc2-win64-lane3").resolve() or not exe.exists():
            raise ValueError("panel smoke requires the lane3 smoke host executable")
        head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True,
                                       **child_kwargs(redirected=True)).strip()
        fingerprint = exe_fingerprint.extract(exe)
        if not fingerprint.get("present") or fingerprint.get("sha12") != head[:12] or fingerprint.get("dirty") or not exe_guard.exe_has_offscreen(exe):
            raise ValueError("panel shell fingerprint/background support does not match worktree HEAD")
        provenance = dict(fingerprint=fingerprint, exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), head=head)
        for extension in ("fit", "pak"):
            if name.startswith("chooser") or name == "marker_panel":
                from editor.tests.legacy_reference import FROZEN, verify_frozen
                verify_frozen()
                seed = "marker" if name == "marker_panel" else "before"
                data = (FROZEN / f"e5_reference_{seed}.{extension}").read_bytes()
            else:
                data = stock.read(f"data/missions/mc2_01.{extension}")
            if name == "objectives_panel" and extension == "fit":
                # Model-name edits require a camera-supported type in the fingerprinted input.
                begin = data.index(b"[Team0Objective1]")
                end = data.index(b"\n[", begin)
                body, count = re.subn(rb"(?m)^(l\s+ModelType\s*=\s*)[^\r\n]*", lambda match: match[1] + b"14", data[begin:end])
                if count != 1:
                    raise ValueError("objective panel fixture needs one ModelType")
                data = data[:begin] + body + data[end:]
            (directory / f"input.{extension}").write_bytes(data)
            provenance[f"input_{extension}_sha256"] = hashlib.sha256(data).hexdigest()
        host_lease.ensure_held("E3-b SDL panel widgets")
        command = [str(exe), "--file-smoke" if name in FILE_CASES else "--panel-smoke", name,
                   "--mission", str(directory / "input.fit"), "--output", str(directory),
                   "--log", str(directory / "shell.log"), "--recent-file", str(directory / "recent.txt")]
        if name.startswith("chooser"):
            command += ["--base", str(exe.parent)]
            if name == "chooser_mod": command += ["--mods", "A:/Games/mc2-opengl", "--mod", "MC2X-CVE-G"]
        process = nofocus.spawn(command, cwd=str(exe.parent), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, errors="replace")
        try:
            output, _ = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill(); output, _ = process.communicate()
            raise ValueError(f"panel timeout: {name}")
        finally:
            if process.poll() is not None:
                (directory / "process.log").write_text(output or "", encoding="utf-8")
        (grade_file if name in FILE_CASES else grade_panel)(name, directory, process.returncode)
        if name in PANEL_OPERATIONS: panel_negative_controls(name, directory)
        if hashlib.sha256(exe.read_bytes()).hexdigest() != provenance["exe_sha256"]:
            raise ValueError("panel executable changed during smoke")
        for extension in ("fit", "pak"):
            if hashlib.sha256((directory / f"input.{extension}").read_bytes()).hexdigest() != provenance[f"input_{extension}_sha256"]:
                raise ValueError("panel input copy changed")
        result = dict(name=name, passed=True, bucket="", detail="rendered widgets, changed save/reopen, undo and redo bytes PASS")
    except (OSError, ValueError, host_lease.HostLeaseBusy, nofocus.ExeRefused) as error:
        result = dict(name=name, passed=False, bucket="panel_gate", detail=str(error))
    result["seconds"] = round(time.time() - start, 1)
    (directory / "gate.json").write_text(json.dumps(dict(result=result, provenance=provenance), indent=2), encoding="utf-8")
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description="MC2 Mission Editor console smoke runner")
    ap.add_argument("--exe", default=DEFAULT_EXE, help="path to Mission Editor.exe")
    ap.add_argument("--shell-exe", type=Path, help="SDL smoke host executable (default: Mission Editor Smoke.exe beside Mission Editor.exe)")
    ap.add_argument("--catalog-exe", type=Path, default=REPO / "build64/out/editor/RelWithDebInfo/editor_catalog_probe.exe")
    ap.add_argument("--catalog-mods", type=Path, default=Path("A:/Games/mc2-opengl"))
    ap.add_argument("--catalog-mod", default="MC2X-CVE-G")
    ap.add_argument("--freeze-legacy-reference", type=Path, help="new immutable directory for legacy_catalog_reference outputs")
    ap.add_argument("--compare-legacy-reference", type=Path, help="compare complete placement values with an existing frozen corpus")
    ap.add_argument("--owned-marker-placement", type=Path, help="owned marker save stem for the explicit owned_marker_reload case")
    ap.add_argument("--owned-marker-action", choices=("inserted", "moved", "deleted"), default="inserted")
    ap.add_argument("--owned-objective-edit", type=Path, help="objective-ResourcePoints-edited save stem from objectives_panel")
    ap.add_argument("--owned-objective-crud-edit", type=Path, help="objective-{add,copy,delete,up,down}-edited save stem from objectives_crud_panel")
    ap.add_argument("--exit-sec", type=int, default=12,
                    help="editor hard-exits this many seconds after the engine is up (per case)")
    ap.add_argument("--timeout", type=int, default=180, help="per-case timeout seconds (hard backstop)")
    ap.add_argument("--case", action="append", help="run only these cases (repeatable)")
    ap.add_argument("--keep-logs", action="store_true", help="save each case's captured stderr/stdout (incl. the [CONFIG v1] lines) into the report dir")
    ap.add_argument("--ledger", type=Path, help="write the 24-row round-trip ledger to this Markdown path")
    ap.add_argument("--out", type=Path, default=REPO / "scratch" / "editor-smoke", help="transient evidence directory root")
    ap.add_argument("--baseline", type=Path, default=BASELINE, help="frozen exact FIT hashes for all stock missions")
    ap.add_argument("--rebaseline", action="store_true", help="explicitly replace baseline after all 24 fresh comparisons; print changes")
    ap.add_argument("--stock-allowlist", type=Path, help="JSON map: relative loose path -> exact sha256 and reason")
    ap.add_argument("--prepare-stock-scratch", action="store_true", help="remove/replace overlays only under this worktree's scratch runtime, then audit")
    ap.add_argument("--minimized", dest="minimized", action="store_true", default=True,
                    help="launch the editor minimized/non-activating (default; Windows only)")
    ap.add_argument("--no-minimized", dest="minimized", action="store_false",
                    help="launch the editor with a normal visible window")
    args = ap.parse_args()
    if args.case and "owned_marker_reload" in args.case and not args.owned_marker_placement:
        ap.error("owned_marker_reload requires --owned-marker-placement from run_document_proofs.py")
    if args.case and "owned_objective_reload" in args.case and not args.owned_objective_edit:
        ap.error("owned_objective_reload requires --owned-objective-edit from objectives_panel")
    if args.case and "owned_objective_crud_reload" in args.case and not args.owned_objective_crud_edit:
        ap.error("owned_objective_crud_reload requires --owned-objective-crud-edit from objectives_crud_panel")
    if args.freeze_legacy_reference and args.freeze_legacy_reference.exists():
        ap.error("freeze directory already exists; frozen placement references are immutable")
    if args.freeze_legacy_reference and args.compare_legacy_reference:
        ap.error("freeze and acceptance comparison are separate commands")
    if args.compare_legacy_reference:
        from editor.tests.legacy_reference import verify_frozen
        try:
            verify_frozen(args.compare_legacy_reference)
        except (OSError, ValueError, KeyError) as error:
            ap.error(str(error))

    global MINIMIZED
    MINIMIZED = args.minimized

    exe = Path(args.exe).resolve()
    deploy = exe.parent
    if args.case and set(args.case) <= {"catalog_stock", "catalog_mod"}:
        if "catalog_stock" in args.case and any(os.environ.get(key) for key in ("MC2_ACTIVE_MOD", "MC2_MOD_DEPS")):
            print("ERROR: stock catalog proof refuses environment mod selection")
            return 2
        from editor.tests.run_catalog_proofs import prove
        try:
            return prove(args.catalog_exe, deploy, args.catalog_mods, args.catalog_mod,
                         args.out / time.strftime("%Y-%m-%dT%H-%M-%S"), "catalog_mod" in args.case)
        except (OSError, ValueError, KeyError, StopIteration, subprocess.TimeoutExpired) as error:
            print(f"ERROR: catalog proof failed: {error}")
            return 1
    if not exe.exists():
        print(f"ERROR: editor exe not found: {exe}\n  Build EditRel in RelWithDebInfo and deploy to a private runtime first.")
        return 2
    if any(os.environ.get(key) for key in ("MC2_ACTIVE_MOD", "MC2_MOD_DEPS")):
        print("ERROR: stock editor proof refuses MC2_ACTIVE_MOD or MC2_MOD_DEPS; clear mod selection before running")
        return 2

    suite = build_suite(deploy)
    ALL = ["stock_roundtrip", "workflows", "legacy_catalog_reference"] + list(suite.keys()) + ["gen_save_load", "new_model"] + list(PANEL_OPERATIONS) + list(FILE_CASES)
    single_stock = {f"stock_roundtrip:{mission}": mission for mission in STOCK_MISSIONS}
    selected = args.case or ALL
    for c in selected:
        if c not in ALL and c not in single_stock and c not in ("owned_marker_reload", "owned_objective_reload", "owned_objective_crud_reload"):
            print(f"ERROR: unknown case '{c}'. Valid: {', '.join(ALL)}")
            return 2

    ts = time.strftime("%Y-%m-%dT%H-%M-%S")
    rptdir = args.out / ts
    rptdir.mkdir(parents=True, exist_ok=False)
    stock, groups = None, {}
    try:
        allowances = (read_json_object(args.stock_allowlist) if args.stock_allowlist
                      else read_json_object(args.baseline).get("stock_allowlist", {}) if args.baseline.exists() else {})
        if not isinstance(allowances, dict) or any(not isinstance(value, dict)
                or not isinstance(value.get("reason"), str) or not isinstance(value.get("sha256"), str)
                for value in allowances.values()):
            raise ValueError("stock allowances must map paths to sha256/reason strings")
        if args.prepare_stock_scratch:
            prepare_stock_scratch(deploy, rptdir / "preparation.json", allowances)
        dependencies = dependency_manifest(deploy, rptdir / "dependencies.json", allowances)
        dependency_hash = hashlib.sha256((rptdir / "dependencies.json").read_bytes()).hexdigest()
        print(f"[STOCK_DEPENDENCIES] PASS sha256={dependency_hash} loose={len(dependencies['loose_files'])}")
    except (OSError, KeyError, ValueError, zlib.error) as exc:
        print(f"ERROR: stock dependency audit failed: {exc}")
        return 2
    if any(c == "stock_roundtrip" or c in single_stock or c in PANEL_OPERATIONS or c in FILE_CASES for c in selected) or "workflows" in selected or "new_model" in selected or "legacy_catalog_reference" in selected:
        try:
            stock = StockInputs(deploy)
            campaign = stock.read("data/campaign/campaign.fit")
            groups = campaign_groups(campaign)
            campaign_path = deploy / "terrain_gen_out" / "e0_stock_campaign.fit"
            campaign_path.parent.mkdir(parents=True, exist_ok=True)
            campaign_path.write_bytes(campaign)
        except (OSError, KeyError, ValueError) as exc:
            print(f"ERROR: stock setup failed: {exc}")
            return 2

    # Setup fixtures
    need_present = ("foliage_present" in selected) or ("foliage_menu_commands" in selected)
    need_garbage = "foliage_garbage" in selected
    if need_present:
        _gen_foliage_json(deploy, instances=True)   # initial genmap.foliage.json to toggle/clear/reload
    if need_garbage:
        _write_garbage(deploy)

    results = []
    for c in selected:
        if c in ("owned_objective_reload", "owned_objective_crud_reload"):
            control = rptdir / c
            control.mkdir()
            try:
                provenance = marker_reload_provenance(exe)
                source = args.owned_objective_crud_edit if c == "owned_objective_crud_reload" else args.owned_objective_edit
                row = run_owned_objective_reload(exe, deploy, source, control, args.exit_sec, args.timeout, crud=c == "owned_objective_crud_reload")
                row["provenance"] = provenance
            except (OSError, ValueError, KeyError, struct.error) as error:
                row = dict(name=c, passed=False, bucket="owned_objective_control_invalid", detail=str(error), esmoke={})
            results.append(row)
            continue
        if c == "owned_marker_reload":
            control = rptdir / c
            control.mkdir()
            try:
                provenance = marker_reload_provenance(exe)
                row = run_owned_marker_reload(exe, deploy, args.owned_marker_placement, control, args.exit_sec, args.timeout, args.owned_marker_action)
                row["provenance"] = provenance
            except (OSError, ValueError, KeyError, struct.error) as error:
                row = dict(name=c, passed=False, bucket="owned_marker_control_invalid", detail=str(error), esmoke={})
            results.append(row)
            continue
        if c in PANEL_OPERATIONS or c in FILE_CASES:
            results.append(run_panel(c, (args.shell_exe or deploy / "Mission Editor Smoke.exe").resolve(),
                                     stock, (rptdir / c).resolve(), args.timeout))
            continue
        if c == "new_model":
            results.append(run_new_model((args.shell_exe or deploy / "Mission Editor Smoke.exe").resolve(),
                                         stock, (rptdir / "new-model").resolve(), args.timeout))
            continue
        if c == "stock_roundtrip" or c in single_stock:
            missions = (single_stock[c],) if c in single_stock else STOCK_MISSIONS
            for mission in missions:
                name = f"stock_roundtrip:{mission}"
                original = None
                try:
                    original = stock.read(f"data/missions/{mission}.fit")
                    stock.read(f"data/missions/{mission}.pak")
                    (rptdir / f"{mission}.input.fit").write_bytes(original)
                    save_rel = f"terrain_gen_out/e0_roundtrip/{mission}.pak"
                    row = run_case_with_retry(name, exe, deploy, args.exit_sec, args.timeout,
                                              [f"-mission=data/missions/{mission}.pak", f"-smoke-save={save_rel}"], "saved")
                    output = deploy / Path(save_rel).with_suffix(".fit")
                    if row["passed"]:
                        if not output.exists() or not (deploy / save_rel).exists():
                            row.update(passed=False, bucket="roundtrip_output_missing", detail="save=1 but fresh PAK/FIT output missing")
                        else:
                            written = output.read_bytes()
                            comparison = compare_fit(original, written)
                            row["roundtrip"] = comparison
                            row["pinned_delta"] = pinned_class_delta(mission, original, comparison)
                            row["detail"] = comparison["explanation"]
                            if not comparison["byte_identical"]:
                                row["result_label"] = "FINDING"
                            (rptdir / f"{mission}.written.fit").write_bytes(written)
                            (rptdir / f"{mission}.json").write_text(json.dumps(comparison, indent=2) + "\n", encoding="utf-8")
                            diff = difflib.unified_diff(original.decode("latin-1").splitlines(keepends=True), written.decode("latin-1").splitlines(keepends=True), fromfile=f"{mission}.input.fit", tofile=f"{mission}.written.fit")
                            (rptdir / f"{mission}.diff").write_bytes("".join(diff).encode("latin-1"))
                except (OSError, KeyError, ValueError) as exc:
                    row = dict(name=name, passed=False, bucket="roundtrip_failed", detail=str(exc), esmoke={})
                if original is not None:
                    row["input_sha256"] = hashlib.sha256(original).hexdigest()
                results.append(row)
        elif c == "legacy_catalog_reference":
            from editor.tests.legacy_reference import grade, retain, compare_frozen
            row = run_case_with_retry(c, exe, deploy, args.exit_sec, args.timeout,
                                      ["-mission=data/missions/mc2_01.pak", "-smoke-catalog-reference"], None)
            if row["passed"]:
                try:
                    reference = (compare_frozen(deploy / "terrain_gen_out", row["captured"], args.compare_legacy_reference)
                                 if args.compare_legacy_reference else grade(deploy / "terrain_gen_out", row["captured"]))
                    destination = args.freeze_legacy_reference or rptdir / "legacy-reference"
                    retain(deploy / "terrain_gen_out", destination, reference)
                    row["detail"] = f"9/9 identified placements changed output, undo/redo/save/reopen; retained {destination}"
                except (OSError, ValueError, KeyError, struct.error) as error:
                    row.update(passed=False, bucket="legacy_reference_failed", detail=str(error))
            results.append(row)
        elif c == "workflows":
            row = run_case_with_retry("workflows", exe, deploy, args.exit_sec, args.timeout,
                                     ["-mission=data/missions/mc2_01.pak", "-smoke-workflows", "-smoke-save=terrain_gen_out/e0_workflows.pak"], "workflows")
            if row["passed"]:
                output = deploy / "terrain_gen_out/e0_workflows.fit"
                if not output.exists() or not (deploy / "terrain_gen_out/e0_workflows.pak").exists():
                    row.update(passed=False, bucket="workflow_export_missing", detail="fresh workflow PAK/FIT missing")
                else:
                    row["detail"] = "Six action-handler probes and two model-only probes graded; authored state, undo, save and reload required."
            results.append(row)
        elif c == "gen_save_load":
            # Two phases: (1) generate a RANDOM map + save to .pak; (2) load that
            # .pak back. Both must reach a clean summary -> generate->save->load
            # works through the editor itself.
            save_rel = "terrain_gen_out\\smoke_saved.pak"
            pak = deploy / "terrain_gen_out" / "smoke_saved.pak"
            try:
                if pak.exists():
                    pak.unlink()
            except OSError:
                pass
            gflag, sz, terr = rand_gen_flag()
            print(f"[gen_save_load] random map: size_idx={sz} terrain={terr}")
            r1 = run_case_with_retry("gen_save_load:gen+save", exe, deploy, args.exit_sec, args.timeout,
                          [gflag, f"-smoke-save={save_rel}"], "saved")
            results.append(r1)
            if r1["passed"] and pak.exists():
                r2 = run_case_with_retry("gen_save_load:load", exe, deploy, args.exit_sec, args.timeout,
                              [f"-mission={save_rel}"], None)
            else:
                r2 = dict(name="gen_save_load:load", passed=False, bucket="no_saved_pak",
                          detail="phase-1 save failed or .pak missing", rc="-", seconds="-", esmoke={})
            results.append(r2)
        else:
            flags, expect = suite[c]
            results.append(run_case_with_retry(c, exe, deploy, args.exit_sec, args.timeout, list(flags), expect))

    # Report
    try:
        gate_baseline(results, args.baseline, args.rebaseline, dependencies["archives"], allowances)
    except (OSError, ValueError) as exc:
        print(f"ERROR: baseline gate failed: {exc}")
        for row in results:
            if row["name"].startswith("stock_roundtrip:"):
                row.update(passed=False, result_label="FAIL", bucket="rebaseline_failed", detail=str(exc))
    if args.ledger:
        write_roundtrip_ledger(args.ledger, results, groups, rptdir.relative_to(REPO) if rptdir.is_relative_to(REPO) else rptdir,
                               dependency_hash, args.baseline.relative_to(REPO) if args.baseline.is_relative_to(REPO) else args.baseline)
    npass = sum(1 for r in results if r["passed"])
    nflaky = sum(1 for r in results if r.get("flaky") and r.get("passed"))
    nfindings = sum(1 for r in results if r.get("result_label") == "FINDING")
    # A clean green run is npass==N AND no flaky-passes. Flaky-passes still count
    # toward npass (the suite is usable as a gate) but the header marks the run so
    # it is never read as strictly clean.
    all_pass = (npass == len(results))
    verdict = "PASS" if all_pass else "FAIL"
    flaky_note = f", {nflaky} flaky-pass (0xC0000005 retried)" if nflaky else ""
    header_star = "*" if nflaky else ""
    lines = [f"# Editor smoke {ts}  result={verdict}{header_star} "
             f"({npass}/{len(results)}{flaky_note})", "",
             f"Round-trip findings: {nfindings}. PASS requires exact frozen input/output hashes and outcome (no masks).", "",
             "| Case | Result | Bucket | rc | sec | foliage_count |",
             "|------|--------|--------|----|----|---------------|"]
    for r in results:
        es = r.get("esmoke", {})
        result_label = r.get("result_label") or ("PASS" if r["passed"] else "FAIL")
        lines.append(f"| {r['name']} | {result_label} | "
                     f"{r['bucket'] or '-'} | {r.get('rc','-')} | {r.get('seconds','-')} | "
                     f"{es.get('foliage_count','-')} |")
        if r.get("detail"):
            lines.append(f"|  | | {r['detail']} | | | |")
        for cl in r.get("config", []):
            lines.append(f"|  | | `{cl}` | | | |")
        if args.keep_logs and r.get("captured"):
            safe = r["name"].replace(":", "_")
            (rptdir / f"{safe}.stderr.txt").write_text(r["captured"], encoding="utf-8", errors="replace")
    report = "\n".join(lines) + "\n"
    (rptdir / "report.md").write_text(report, encoding="utf-8")
    print("\n" + report)
    print(f"report: {rptdir / 'report.md'}")

    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
