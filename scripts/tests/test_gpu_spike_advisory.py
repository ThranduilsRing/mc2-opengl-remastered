from scripts.smoke_lib.logparse import parse_log
from scripts.smoke_lib.gates import GateConfig, evaluate


def test_gpu_spike_advisory_from_mc2_10_bake_fixture():
    # followups-1126g/smoke-artifacts/mc2_10.log:1192-1196, numerical receipts unchanged.
    text = """[SHADOW_BAKE] object map re-recorded: generation=2 casters=22340 timings(ms): total 2.797, height 0.666, resolve 1.937, bin 0.000, record 0.193
[SHADOW_BAKE] gpu_ms object_map=164.97 cpu_ms=2.80
[SHADOW_BAKE] vram resident_mb=98.7 (mask 66.7 + object_map 32.0) transient_peak_mb=72.2 retained for dirty updates
[SHADOW_BAKE] ledger preset=2 mask_mb=66.67 object_mb=32.00 mover_mb=32.00 resident_mb=130.67 transient_peak_mb=72.18
[SHADOW_BAKE] dirty queue drained: drained=true sub_tiles=1137 batches=569 batch_gpu_ms mean=8.994 max=20.984 (budget 2 sub-tiles a frame) object_map_records=2 dirty_batches=569 local_tile_rebakes=0 frames=578 records_per_1000=3.46 batches_per_1000=984.43 local_tiles_per_1000=0.00
[VK_GPU_PASSES v1] frame=120 n=120 span_ms=1.0 max_frame_ms=12.3 max_scope=VKIsl.Shadow.Bake:11.0 | foo=1.0(x1.0)
"""
    summary = parse_log(text)
    assert summary.marker_counts["gpu_spike"] == 3
    verdict = evaluate(summary, GateConfig(), exit_code=0, walltime_s=1)
    assert "gpu_spike" not in verdict.buckets
    assert any("[advisory] 3 gpu_spike" in row for row in verdict.details)
    safe = text.replace("164.97", "10.0").replace("8.994", "1.0").replace("12.3", "10.0")
    assert "gpu_spike" not in parse_log(safe).marker_counts
