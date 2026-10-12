# tests/smoke/test_marker_buckets.py
# META-FIX INFRA-3: table-driven marker buckets — proves the mechanism can
# fail a run (the "self-test leg" for the smoke-side marker table).

from scripts.smoke_lib.logparse import LogSummary, MARKER_BUCKETS, PerfRow, parse_log
from scripts.smoke_lib.gates import GateConfig, evaluate

CFG = GateConfig(heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3,
                 duration_s=30)


def _base():
    return LogSummary(instr_banner_seen=True, smoke_banner_seen=True,
                      mission_resolve_seen=True, smoke_summary_result="pass",
                      mission_ready_ms=5000.0, heartbeats_play=30,
                      last_heartbeat_wall_s_play=30.5,
                      perf=PerfRow(avg_fps=60))


def test_red_marker_fails_run():
    s = _base()
    s.marker_counts = {"mover_coherence": 1}
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert not v.passed and "mover_coherence" in v.buckets


def test_marker_override_downgrades_to_advisory():
    s = _base()
    s.marker_counts = {"mover_coherence": 1}
    cfg = GateConfig(heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3,
                     duration_s=30,
                     marker_overrides={"mover_coherence": True})
    v = evaluate(s, cfg, exit_code=0, walltime_s=31)
    assert v.passed and "mover_coherence" not in v.buckets
    assert any("advisory" in d for d in v.details)


# --- META-FIX-BATCH-3 rows (designs A + B) ----------------------------------

def test_batch3_rows_registered():
    rows = {mb.bucket: mb for mb in MARKER_BUCKETS}
    assert rows["mover_coherence"].severity == "red"
    assert rows["mover_coherence"].manifest_override_key == "allow_mover_coherence"
    assert rows["mover_coherence_advisory"].severity == "advisory"
    assert rows["asset_load_fail"].severity == "red"
    assert rows["asset_load_fail"].manifest_override_key == "allow_asset_load_fail"
    assert rows["icon_oob"].severity == "red"  # armed 2026-07-16 after icon_atlas_harness 16/16
    # appear_audit stays DISARMED until the 24-map failed=0 baseline.
    assert rows["appear_audit"].severity == "advisory"


def test_mover_coherence_violation_counts_and_fails():
    s = _base()
    parsed = parse_log("[MOVER_COHERENCE] VIOLATION rule=I2 part=513 "
                       "name=Atlas detail=selected_offgui\n")
    assert parsed.marker_counts.get("mover_coherence") == 1
    s.marker_counts = {"mover_coherence": 1}
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert not v.passed and "mover_coherence" in v.buckets


def test_mover_coherence_advisory_never_fails():
    s = _base()
    parsed = parse_log("[MOVER_COHERENCE] ADVISORY rule=I1b part=2 name=X "
                       "detail=bar_full\n")
    assert parsed.marker_counts.get("mover_coherence_advisory") == 1
    assert parsed.marker_counts.get("mover_coherence", 0) == 0
    s.marker_counts = {"mover_coherence_advisory": 3}
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert v.passed and "mover_coherence_advisory" not in v.buckets


def test_asset_load_fail_counts_three_events_not_file_missing():
    parsed = parse_log(
        "[ASSET_LOAD v1] event=parse_fail loader=ase reason=bad_header path=a\n"
        "[ASSET_LOAD v1] event=required_missing ctx=mech3d.base rc=-1 path=b\n"
        "[ASSET_LOAD v1] event=appr_type_fail name=c apprNum=0x01000000\n"
        "[ASSET_LOAD v1] event=file_missing loader=ase reason=no_ase path=d\n")
    assert parsed.marker_counts.get("asset_load_fail") == 3


def test_icon_oob_red_and_appear_audit_advisory():
    s = _base()
    parsed = parse_log(
        "[ASSET_LOAD v1] event=icon_oob site=mechlistbox index=999 "
        "cell=25x30 atlas=256x256\n"
        "[APPEAR_AUDIT v1] event=missing reason=appearance_null handle=5 "
        "class=2 part=101\n"
        "[APPEAR_AUDIT v1] summary scanned=100 missing=1\n")
    assert parsed.marker_counts.get("icon_oob") == 1
    assert parsed.marker_counts.get("appear_audit") == 1  # summary not counted
    s.marker_counts = dict(parsed.marker_counts)
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert "icon_oob" in v.buckets           # armed red
    assert "appear_audit" not in v.buckets   # disarmed (advisory detail only)
    assert any("appear_audit" in d for d in v.details)


# G4: a VK gate resolved before main() fails the smoke run (the lazy-read contract of the VK gates).

def test_premain_vk_gate_line_is_a_red_marker():
    s = parse_log("[CONFIG v1] warn=premain_read name=MC2_VK_MECH_TIER1 premain_value=unset\n")
    assert s.marker_counts.get("config_premain_vk") == 1
    r = _base()
    r.marker_counts = dict(s.marker_counts)
    v = evaluate(r, CFG, exit_code=0, walltime_s=31)
    assert not v.passed and "config_premain_vk" in v.buckets


def test_premain_line_for_a_non_vk_gate_is_not_this_bucket():
    s = parse_log("[CONFIG v1] warn=premain_read name=MC2_TERRAIN_LOD_CHUNK premain_value=0\n")
    assert s.marker_counts.get("config_premain_vk", 0) == 0


def test_changed_value_premain_line_alone_is_not_this_bucket():
    # warn=premain (value changed) is the registry's own warning; the bucket keys on the read itself
    s = parse_log("[CONFIG v1] warn=premain name=MC2_VK_MECH_TIER1 premain_value=0 env_now=1\n")
    assert s.marker_counts.get("config_premain_vk", 0) == 0
