#!/usr/bin/env python3
"""MCP-ANTI-CHURN-1 tests: lane_registry / mission_facts / gate_status /
deploy_status. Pure pytest, tmp_path fixtures, no game/deploy touch.

Run:  py -3 -m pytest tools/repo_intel/test_anti_churn.py -q
"""
from __future__ import annotations

import json
import struct
import sys
import zlib
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import env_index as ei
import gate_status as gs
import lane_registry as lr
import mission_facts as mf

REPO_ROOT = Path(__file__).resolve().parents[2]


# ---------------------------------------------------------------------------
# lane_registry
# ---------------------------------------------------------------------------

@pytest.fixture()
def lane_store(tmp_path, monkeypatch):
    store = tmp_path / "lane_registry.json"
    monkeypatch.setenv("MC2_LANE_REGISTRY_PATH", str(store))
    return store


def test_register_list_release(lane_store, tmp_path):
    r = lr.register_lane(tmp_path, "LANE-A", worktree=str(tmp_path),
                         files_claimed=["mclib/tgl.*", "scripts/mcp/"],
                         deploy_lane="0.4", note="test lane")
    assert r["registered"]["name"] == "LANE-A"
    assert r["file_conflicts"] == []
    assert Path(r["store"]) == lane_store

    lanes = lr.list_lanes(tmp_path)
    assert lanes["count"] == 1
    assert lanes["lanes"][0]["deploy_lane"] == "0.4"
    assert lanes["lanes"][0]["stale"] is False

    rel = lr.release_lane(tmp_path, "LANE-A")
    assert rel["released"] is True
    assert lr.list_lanes(tmp_path)["count"] == 0


def test_conflict_detection(lane_store, tmp_path):
    lr.register_lane(tmp_path, "LANE-A", files_claimed=["mclib/tgl.*", "code/"])
    # exact-glob hit
    c = lr.check_conflict(tmp_path, ["mclib/tgl.cpp"])
    assert not c["clear"]
    assert c["conflicts"][0]["lane"] == "LANE-A"
    # dir-claim hit
    c = lr.check_conflict(tmp_path, ["code/mission.cpp"])
    assert not c["clear"]
    # non-overlapping path is clear
    c = lr.check_conflict(tmp_path, ["shaders/gos_terrain.frag"])
    assert c["clear"]
    # a lane never conflicts with itself
    c = lr.check_conflict(tmp_path, ["mclib/tgl.cpp"], exclude_lane="LANE-A")
    assert c["clear"]


def test_release_worktree_matches_under_one_lock(lane_store, tmp_path, monkeypatch):
    import contextlib
    lr.register_lane(tmp_path, "local-a", worktree="A:/Games/slot05/")
    lr.register_lane(tmp_path, "local-b", worktree="a:\\games\\SLOT05")
    lr.register_lane(tmp_path, "foreign", worktree="A:/Games/slot09")
    locks = []
    real_lock = lr._lock

    @contextlib.contextmanager
    def locked(path):
        with real_lock(path):
            locks.append(path)
            # A claim arriving before lock acquisition must also be removed.
            data = lr._read(path)
            data["lanes"]["late"] = {"worktree": "A:/Games/slot05"}
            lr._write(path, data)
            yield

    monkeypatch.setattr(lr, "_lock", locked)
    assert lr.release_worktree(tmp_path, "A:/Games/slot05")["released"] == ["late", "local-a", "local-b"]
    assert len(locks) == 1
    assert set(lr._read(lane_store)["lanes"]) == {"foreign"}


def test_registry_lock_timeout_preserves_claims(lane_store, tmp_path, monkeypatch):
    lr.register_lane(tmp_path, "local", worktree=str(tmp_path))
    before = lane_store.read_bytes()
    lane_store.with_suffix(".lock").write_text("busy")
    monkeypatch.setattr(lr, "_LOCK_WAIT_SECS", 0)
    with pytest.raises(TimeoutError, match="lock unavailable"):
        lr.release_worktree(tmp_path, str(tmp_path))
    assert lane_store.read_bytes() == before


def test_second_lane_gets_warned(lane_store, tmp_path):
    lr.register_lane(tmp_path, "LANE-A", files_claimed=["mclib/bdactor.cpp"],
                     deploy_lane="0.4")
    r = lr.register_lane(tmp_path, "LANE-B", files_claimed=["mclib/bdactor.cpp"],
                         deploy_lane="0.4")
    assert r["file_conflicts"], "overlapping claim must be reported"
    assert r["deploy_lane_clash"] == ["LANE-A"]
    assert r["warnings"]


def test_stale_lane_ignored(lane_store, tmp_path, monkeypatch):
    lr.register_lane(tmp_path, "LANE-OLD", files_claimed=["mclib/"])
    monkeypatch.setenv("MC2_LANE_TTL_SECS", "0")
    c = lr.check_conflict(tmp_path, ["mclib/terrain.cpp"])
    assert c["clear"], "stale lanes must not produce conflicts"
    lanes = lr.list_lanes(tmp_path)
    assert lanes["lanes"][0]["stale"] is True


# ---------------------------------------------------------------------------
# mission_facts — synthetic pak fixture
# ---------------------------------------------------------------------------

def _make_pak(path: Path, side: int = 60, elev: float = 5.0,
              compress: bool = False) -> None:
    """Minimal 1-packet PacketFile whose packet 0 is a MapData block."""
    verts = side * side
    block = bytearray(verts * mf.PCV_SIZE)
    for i in range(verts):
        struct.pack_into("<f", block, i * mf.PCV_SIZE + mf.OFF_ELEV,
                         elev + (i % 7))
    payload = bytes(block)
    st = mf.ST_RAW
    if compress:
        payload = struct.pack("<I", len(payload)) + zlib.compress(payload)
        st = mf.ST_ZLIB
    num = 1
    first_off = (num + 2) * 4
    header = struct.pack("<II", mf.PACKET_MAGIC, first_off)
    entry = struct.pack("<I", (st << mf.TYPE_SHIFT) | first_off)
    path.write_bytes(header + entry + payload)


def test_parse_pak_raw(tmp_path):
    pak = tmp_path / "mc2_99.pak"
    _make_pak(pak, side=60, elev=5.0)
    g = mf.parse_pak(pak)
    assert g["side"] == 60
    assert g["verts"] == 3600
    assert g["world_size_wu"] == 60 * 128.0
    assert g["elev_min"] == 5.0
    assert g["elev_max"] == 11.0


def test_parse_pak_zlib(tmp_path):
    pak = tmp_path / "mc2_98.pak"
    _make_pak(pak, side=80, compress=True)
    g = mf.parse_pak(pak)
    assert g["side"] == 80
    assert g["verts"] == 6400


def test_parse_pak_rejects_garbage(tmp_path):
    pak = tmp_path / "junk.pak"
    pak.write_bytes(b"\x00" * 64)
    g = mf.parse_pak(pak)
    assert g["side"] is None
    assert "failed" in g["note"]


def test_water_elevation(tmp_path):
    fit = tmp_path / "mc2_99.fit"
    fit.write_text("[Terrain]\nf Something = 1.0\n[Water]\nf Elevation = 42.5\n")
    assert mf.read_water_elevation(fit) == 42.5
    assert mf.read_water_elevation(tmp_path / "absent.fit") is None
    (tmp_path / "nowater.fit").write_text("[Terrain]\nf X = 1\n")
    assert mf.read_water_elevation(tmp_path / "nowater.fit") == 0.0


def test_mission_facts_lane_scan(tmp_path, monkeypatch):
    # Fake a deploy lane layout + point the lane table at it.
    lane = tmp_path / "deploy"
    md = lane / "data" / "missions"
    beauty = md / "mc2_99.beauty"
    beauty.mkdir(parents=True)
    _make_pak(md / "mc2_99.pak", side=60)
    (md / "mc2_99.fit").write_text("[Water]\nf Elevation = 7.0\n")
    (beauty / "control_map.png").write_bytes(b"png")
    (beauty / "shoreline_mask.png").write_bytes(b"png")
    monkeypatch.setattr(mf, "deploy_lanes",
                        lambda root: [("test-lane", str(lane))])
    facts = mf.mission_facts(tmp_path, "mc2_99")
    assert facts["geometry"]["side"] == 60
    assert facts["water_elevation"] == 7.0
    lane_rec = facts["lanes"]["test-lane"]
    assert lane_rec["pak"] is not None
    assert lane_rec["sidecars_present"] == ["control_map.png",
                                            "shoreline_mask.png"]


def test_mission_facts_requires_stem(tmp_path):
    assert "error" in mf.mission_facts(tmp_path, "")


# ---------------------------------------------------------------------------
# gate_status — synthetic repo fixture + live-repo smoke checks
# ---------------------------------------------------------------------------

def _mini_repo(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    (root / "scripts").mkdir(parents=True)
    (root / "docs").mkdir()
    (root / "docs" / "tier1_env_vars.md").write_text(
        "- `MC2_ALLOWED_GATE=1` — does a thing. Default **OFF**.\n")
    # A structurally real mini gate table (Gates/gate_table.json): one feature-table row and one aux row,
    # as tools/gate_dump emits them.
    (root / "Gates").mkdir()
    (root / "Gates" / "feature_only_rows.json").write_text("[]")
    (root / "Gates" / "gate_table.json").write_text(json.dumps({
        "version": 1, "count": 2, "gates": [
            {"name": "MC2_ALLOWED_GATE", "kind": "Bool", "rule": "Std", "default": False, "owner": "t",
             "retireBy": "", "doc": "allowed gate", "tokens": "", "range": None, "reread": False,
             "divergent": False, "smokePassthrough": True, "verified": False,
             "registry": {"featureId": "MC2_FEATURE_ALLOWED", "kind": "Feature", "order": 0, "defaultOn": False}},
            {"name": "MC2_SPLIT_DOC_GATE", "kind": "Bool", "rule": "Std", "default": True, "owner": "t",
             "retireBy": "", "doc": "doc written as two adjacent literals", "tokens": "", "range": None,
             "reread": False, "divergent": False, "smokePassthrough": False, "verified": False,
             "registry": {"featureId": "MC2_TRACE_SPLIT_DOC", "kind": "Trace", "order": -1, "defaultOn": True}},
        ]}))
    return root


def test_smoke_allowlist_parse(tmp_path):
    root = _mini_repo(tmp_path)
    names, found = gs.parse_smoke_allowlist(root)
    assert found
    assert names == {"MC2_ALLOWED_GATE"}


def test_gate_status_verdicts(tmp_path):
    root = _mini_repo(tmp_path)
    # not a git repo -> reader grep returns [] -> GHOST for known-but-unread
    r = gs.gate_status(root, "MC2_ALLOWED_GATE")
    assert r["registered"] is True
    assert r["tier1_documented"] is True
    assert r["smoke_allowlisted"] is True
    assert r["default"] == "OFF"
    assert any(f.startswith("GHOST") for f in r["flags"])
    # completely unknown var
    r = gs.gate_status(root, "MC2_NO_SUCH_VAR")
    assert any(f.startswith("UNKNOWN") for f in r["flags"])
    # invalid name
    assert "error" in gs.gate_status(root, "not_a_gate")


def test_gate_status_live_repo_known_gate():
    """MC2_DEBUG_STATE_DUMP is read by the engine and smoke-allowlisted —
    ENV_DROP_RISK must NOT fire on it (real-repo anchor sanity)."""
    r = gs.gate_status(REPO_ROOT, "MC2_DEBUG_STATE_DUMP")
    assert r["smoke_allowlisted"] is True
    assert r["reader_count"] > 0
    assert not any(f.startswith("ENV_DROP_RISK") for f in r["flags"])
    assert not r["parse_notes"], f"anchor drift: {r['parse_notes']}"


# ---------------------------------------------------------------------------
# ENV_DROP_RISK is conditional on the runner's real policy
# ---------------------------------------------------------------------------

def test_runner_env_policy_live_repo():
    """The env-composition facts must be READ from smoke_lib/runner.py.

    If this ever fails with inherits=False, the run_smoke.py passthrough tuple
    has genuinely become load-bearing and ~100 registered gates start dropping —
    see scripts/check-smoke-env-inheritance.py, which enforces the same line.
    """
    p = gs.parse_runner_env_policy(REPO_ROOT)
    assert p["anchors_ok"], "FORCED_ENV/SOFT_ENV/POPPED_ENV tables not found"
    assert p["inherits"] is True
    assert "MC2_SMOKE_MODE" in p["forced"]
    assert "MC2_FORCE_ALL_VISIBLE" in p["soft"]
    assert p["popped"] == set()


def test_gate_status_no_false_env_drop_while_inheriting():
    """THE REGRESSION THIS LOCKS OUT.

    MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE and MC2_STATIC_UPDATE_TRACE are read by
    the engine and absent from the run_smoke.py tuple. The old tool called that
    ENV_DROP_RISK and a lane wrote off a correctly-declared A/B as "void" on its
    word. While the runner inherits os.environ the ambient value reaches the
    engine, so the flag must NOT fire — and the absence must be reported as a
    convention gap in plain words, not left as a bare boolean for the next
    reader to re-derive "voided A/B" from.
    """
    for var in ("MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE", "MC2_STATIC_UPDATE_TRACE"):
        r = gs.gate_status(REPO_ROOT, var)
        assert r["reader_count"] > 0, var
        assert r["smoke_env_inherited"] is True, var
        assert not any(f.startswith("ENV_DROP_RISK") for f in r["flags"]), \
            f"{var}: false ENV_DROP_RISK -> {r['flags']}"
        if not r["smoke_allowlisted"]:
            assert any("convention gap ONLY" in n for n in r["parse_notes"]), \
                f"{var}: tuple gap reported without the correction: {r['parse_notes']}"


def test_gate_status_flags_genuinely_undriveable(monkeypatch):
    """POPPED_ENV / FORCED_ENV vars ARE undeliverable — that must still fire.

    This is the true half of the old flag, and the old tool never reported it:
    a POPPED_ENV name is deleted from the child env outright, so an A/B arm that
    sets it really does not run. The shipped table is empty, so one member is injected.
    """
    real = gs.parse_runner_env_policy
    monkeypatch.setattr(gs, "parse_runner_env_policy",
                        lambda root: {**real(root), "popped": {"MC2_GL_ERROR_DRAIN_SILENT"}})
    r = gs.gate_status(REPO_ROOT, "MC2_GL_ERROR_DRAIN_SILENT")
    assert r["smoke_env_popped"] is True
    assert any(f.startswith("ENV_UNDRIVEABLE") for f in r["flags"])

    r = gs.gate_status(REPO_ROOT, "MC2_SMOKE_MODE")
    assert r["smoke_env_forced"] is True
    assert any(f.startswith("ENV_UNDRIVEABLE") for f in r["flags"])


def test_registry_rows_are_read_from_the_gate_table(tmp_path):
    """Feature rows and aux rows both come from Gates/gate_table.json; the doc and default carry over."""
    root = _mini_repo(tmp_path)
    reg = ei._parse_registry(root)
    assert set(reg) == {"MC2_ALLOWED_GATE", "MC2_SPLIT_DOC_GATE"}, reg
    assert reg["MC2_SPLIT_DOC_GATE"]["doc"] == "doc written as two adjacent literals"
    assert reg["MC2_SPLIT_DOC_GATE"]["default"] == "on"
    assert reg["MC2_ALLOWED_GATE"]["feature_id"] == "MC2_FEATURE_ALLOWED"


def test_registry_parse_failure_is_not_unregistered(tmp_path):
    """THE REGRESSION THIS LOCKS OUT.

    gate_status answered `registered: false, verdict: "OK", flags: []` about
    MC2_LIGHTING_DEBUG_VIEW -- a registered gate -- because its parser dropped
    the row and the tool had no way to say "I could not tell". A parse failure
    must NEVER render as a negative answer: no `registered: false`, no "OK"
    verdict, and a flag that says so in words.
    """
    root = _mini_repo(tmp_path)
    hdr = root / "Gates" / "gate_table.json"

    # 1. table truncated mid-document (a torn read / concurrent write)
    hdr.write_text(hdr.read_text()[:120])
    r = gs.gate_status(root, "MC2_ALLOWED_GATE")
    assert r["registered"] == "UNKNOWN", r["registered"]
    assert r["registered"] is not False
    assert r["verdict"] == "UNDETERMINED", r["verdict"]
    assert r["registry_parse_ok"] is False
    assert any(f.startswith("REGISTRY_UNPARSED") for f in r["flags"]), r["flags"]
    assert not any(f.startswith("UNREGISTERED") for f in r["flags"]), r["flags"]
    assert not any(f.startswith("GHOST") for f in r["flags"]), r["flags"]

    # 2. header absent entirely -- the OTHER silent-degradation path: a parser
    #    that returns {} for "file missing" and "registry empty" alike.
    hdr.unlink()
    r = gs.gate_status(root, "MC2_ALLOWED_GATE")
    assert r["registered"] == "UNKNOWN"
    assert r["verdict"] == "UNDETERMINED"
    assert any("cannot read Gates/gate_table.json" in f for f in r["flags"]), r["flags"]


def test_gate_status_env_drop_fails_closed_without_runner(tmp_path):
    """Unknown policy must be treated as 'the tuple is load-bearing'.

    A missing/renamed runner.py must not be read as 'nothing is dropped'; it
    restores the conservative flag AND says the policy could not be parsed.
    """
    root = _mini_repo(tmp_path)
    p = gs.parse_runner_env_policy(root)
    assert p["anchors_ok"] is False and p["inherits"] is False


# ---------------------------------------------------------------------------
# deploy_status — fake deploy dir (no exe fingerprint, manifest only)
# ---------------------------------------------------------------------------

def test_deploy_status_fake_dir(tmp_path):
    import deploy_status as ds
    lane = tmp_path / "fake-deploy"
    lane.mkdir()
    (lane / "mc2.exe").write_bytes(b"\x00not-a-real-exe\x00")
    (lane / ".deployed_manifest.csv").write_text(
        "manifest_version,v1,,,\n"
        "relpath,sha256,bytes,src_commit,timestamp\n"
        "mc2.exe,deadbeef,16,aaaaaaaaaaaa,2026-07-01T00:00:00Z\n")
    r = ds.deploy_status(REPO_ROOT, str(lane), expected_sha="bbbbbbbbbbbb")
    assert r["verdict"] == "STALE"          # manifest sha != expected
    assert r["manifest"]["src_commit"] == "aaaaaaaaaaaa"
    assert r["exe"]["present"] is True
    # matching sha -> exe has no embedded fingerprint -> not a mismatch -> OK
    r = ds.deploy_status(REPO_ROOT, str(lane), expected_sha="aaaaaaaaaaaa")
    assert r["verdict"] == "OK"
    assert r["fingerprint"]["mismatch"] is False


def test_deploy_status_missing_dir(tmp_path):
    import deploy_status as ds
    r = ds.deploy_status(REPO_ROOT, str(tmp_path / "nope"))
    assert r["verdict"] == "MISSING"


def test_deploy_status_known_lane_names():
    import deploy_status as ds
    names = ds._lane_names(REPO_ROOT)
    # Only the dev lane (0.4) and the golden lane (0.4c) are registered, plus
    # lane2; release/testing lanes are archived off-disk and absent from the maps.
    assert "0.4" in names
    assert "0.4c" in names
    # Second builder lane.
    assert names["lane2"].replace("\\", "/").endswith("/mc2-win64-lane2")
    # Archived lanes must stay absent.
    assert not any(n.startswith("0.5") or n.startswith("0.4d") for n in names)


def test_lane2_expected_follows_owning_worktree(tmp_path, monkeypatch):
    """lane2's fingerprint is compared against lane2's worktree HEAD, not the
    caller's; a lane2 entry whose worktree is absent falls back to the caller."""
    import deploy_status as ds
    import lane_model
    lane_dir = tmp_path / "install"
    lane_dir.mkdir()
    wt = tmp_path / "wt"
    wt.mkdir()
    entry = dict(lane_model.LANES["lane2"], install=str(lane_dir),
                 worktree=str(wt), branch="claude/lane2-s3")
    monkeypatch.setitem(lane_model.LANES, "lane2", entry)
    monkeypatch.setattr(ds, "_git_head", lambda root: "L2" * 6 if Path(root) == wt else "NIFTY" * 2)
    monkeypatch.setattr(ds, "_git_branch", lambda root: "claude/lane2-s3" if Path(root) == wt else "claude/nifty-mendeleev")
    r = ds.deploy_status(REPO_ROOT, "lane2")
    assert r["expected"]["worktree"] == str(wt)
    assert r["expected"]["branch"] == "claude/lane2-s3"
    assert r["expected"]["sha"] == ("L2" * 6).lower()
    # path form of the same install resolves to the same owner
    assert ds.deploy_status(REPO_ROOT, str(lane_dir))["expected"]["worktree"] == str(wt)
    # dev lane keeps the caller's worktree
    assert ds.deploy_status(REPO_ROOT, "0.4")["expected"]["worktree"] == str(REPO_ROOT)
    # absent worktree -> caller's worktree
    monkeypatch.setitem(lane_model.LANES, "lane2", dict(entry, worktree=str(tmp_path / "gone")))
    assert ds.deploy_status(REPO_ROOT, "lane2")["expected"]["worktree"] == str(REPO_ROOT)


def test_mission_facts_lanes_exclude_archived():
    import mission_facts as mf
    names = [n for n, _ in mf.deploy_lanes(REPO_ROOT)]
    assert "lane2" in names and "0.4" in names
    assert not any(n.startswith("0.5") for n in names)


# ---------------------------------------------------------------------------
# typed gate registry (Gates/): gate::get<"MC2_X">() is a reader
# ---------------------------------------------------------------------------

def _write_cpp(root: Path, rel: str, text: str) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")


def test_env_index_sees_gate_get_readers(tmp_path):
    _write_cpp(tmp_path, "GameOS/a.cpp",
               'bool a = gate::get<"MC2_ALPHA">();\n'
               'bool b = mc2::gate::get<"MC2_BETA", gate::Rule::Presence>();\n'
               'auto t = gate::state<"MC2_GAMMA">();\n'
               'bool s = gate::isSet<"MC2_DELTA">();\n'
               'auto p = gate::str<"MC2_EPSILON">();\n'
               'bool e = gate::is<"MC2_ZETA", "all">();\n')
    readers = ei._grep_getenv(tmp_path)
    for name in ("MC2_ALPHA", "MC2_BETA", "MC2_GAMMA", "MC2_DELTA", "MC2_EPSILON", "MC2_ZETA"):
        assert [r["file"] for r in readers[name]] == ["GameOS/a.cpp"], name


def test_env_index_does_not_count_the_table_or_its_unit_test(tmp_path):
    _write_cpp(tmp_path, "Gates/gate_table.cpp", 'G("MC2_ROWONLY", Bool, false, "x", "", "doc"),\n')
    _write_cpp(tmp_path, "tests/unit/test_gate.cpp", 'bool a = gate::get<"MC2_FIXTURE">();\n')
    _write_cpp(tmp_path, "tests/unit/other.cpp", 'bool a = gate::get<"MC2_REAL">();\n')
    readers = ei._grep_getenv(tmp_path)
    assert "MC2_ROWONLY" not in readers
    assert "MC2_FIXTURE" not in readers
    assert "MC2_REAL" in readers


def test_env_index_still_sees_getenv_readers(tmp_path):
    _write_cpp(tmp_path, "code/a.cpp", 'const char* v = getenv("MC2_LEGACY");\n')
    assert "MC2_LEGACY" in ei._grep_getenv(tmp_path)


def test_dead_gate_scan_judges_a_gate_get_result(tmp_path):
    import dead_gate_scan as dgs
    lines = ['void f() {', '    gate::get<"MC2_X">();', '    bool b = gate::get<"MC2_Y">();', '    use(b);', '}']
    assert dgs._result_unused_at(lines, 2, "MC2_X") is True      # bare statement: result thrown away
    assert dgs._result_unused_at(lines, 3, "MC2_Y") is False     # consumed
    cached = ['static const bool s = gate::get<"MC2_Z">();']
    assert dgs._result_unused_at(cached, 1, "MC2_Z") is False    # cached idiom: consumer is elsewhere
