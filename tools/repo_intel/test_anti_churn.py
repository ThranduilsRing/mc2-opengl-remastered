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
    (root / "RenderCore").mkdir()
    (root / "scripts" / "run_smoke.py").write_text(
        "env = {k: v for k, v in os.environ.items()\n"
        '       if k in ("MC2_ALLOWED_GATE",\n'
        '                "MC2_OTHER_GATE",\n'
        "                )},\n")
    (root / "scripts" / "check-env-registry.sh").write_text(
        "ALLOWLIST=(\n"
        "    MC2_LEGACY_TRACE           # trace\n"
        ")\n")
    (root / "docs" / "tier1_env_vars.md").write_text(
        "- `MC2_ALLOWED_GATE=1` — does a thing. Default **OFF**.\n")
    (root / "RenderCore" / "RendererFeatureRegistry.h").write_text(
        '{ "MC2_FEATURE_ALLOWED", "MC2_ALLOWED_GATE", EnvVarKind::Flag, '
        'false, "allowed gate" },\n')
    return root


def test_smoke_allowlist_parse(tmp_path):
    root = _mini_repo(tmp_path)
    names, found = gs.parse_smoke_allowlist(root)
    assert found
    assert names == {"MC2_ALLOWED_GATE", "MC2_OTHER_GATE"}


def test_registry_check_allowlist_parse(tmp_path):
    root = _mini_repo(tmp_path)
    names, found = gs.parse_registry_check_allowlist(root)
    assert found
    assert names == {"MC2_LEGACY_TRACE"}


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
    assert "0.4" in names
    assert "0.5-testing" in names
