"""The board's selftest commands run under pytest so one row reports their failures.

Each child keeps its original mode and a private interpreter; checker imports,
global state and command-line arguments cannot spill into another test.
"""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts import console_child
from tools.lane_merge import git_bash   # a bare "bash" from a Python child is the WSL launcher, not Git Bash

# Each selftest is a windowless console child (spelled out per call so the launch and caller scanners see it).
OPTS = dict(cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120,
            env={**os.environ, "PYTHONUTF8": "1"})


def _assert_pass(result):
    assert result.returncode == 0, result.stdout + result.stderr


def test_gate_echo_reads_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-gate-echo-reads.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_cpu_gpu_cogating_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-cpu-gpu-cogating.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_light_row_freshness_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-light-row-freshness.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))



def test_golden_coverage_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-golden-coverage.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_bless_selftest():
    _assert_pass(subprocess.run([sys.executable, "tools/visual_verify/bless_selftest.py"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_cxx17_ratchet_selftest():
    _assert_pass(subprocess.run([sys.executable, "tools/cxx17_ratchet/test_cxx17_ratchet.py"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_comment_guard_selftest():
    _assert_pass(subprocess.run([sys.executable, "tools/comment_guard.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_cite_guard_selftest():
    _assert_pass(subprocess.run([sys.executable, "tools/cite_guard.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_vulkan_bindings_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-vulkan-bindings.py", "--self-test"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_render_contract_alpha_classes_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-render-contract-alpha-classes.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_input_capture_visibility_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-input-capture-visibility.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_capture_determinism_ratchet():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-capture-determinism.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_smoke_fingerprint_ratchet():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-smoke-fingerprint-honesty.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_capture_env_ratchet():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-capture-env-declared.py", "--self-test"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_capture_resolution_ratchet():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-capture-resolution.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_env_drive_ratchet():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-env-drive-guard.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_deploy_lane_ratchet():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-deploy-lane-parity.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_delivery_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-delivery.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_terrain_daylight_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-terrain-daylight-constants.py", "--self-test"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_split_path_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-split-path-contracts.py", "--self-test"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_path_norm_selftest():
    # Same fork-heavy sh pipeline as the mover-registry selftest: 2.7 s idle, 231 s at Idle priority under full CPU load.
    _assert_pass(subprocess.run(["sh", "scripts/check-path-normalization.sh", "--self-test"], **dict(OPTS, timeout=600), **console_child.child_kwargs(redirected=True)))


def test_res_aspect_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-res-aspect-contract.py", "--self-test"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_launcher_ascii_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-launcher-ascii.py", "--self-test"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_order_gate_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-order-gate-coverage.py", "--self-test"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_mover_registry_selftest():
    # Idle-priority shell pipelines can spend most of their wall time unscheduled.
    _assert_pass(subprocess.run(["sh", "scripts/check-mover-registry-coherence.sh", "--self-test"], **dict(OPTS, timeout=600), **console_child.child_kwargs(redirected=True)))


def test_tgl_load_selftest():
    _assert_pass(subprocess.run(["sh", "scripts/check-tgl-load-callers.sh", "--self-test"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_counter_honesty_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-counter-honesty.py", "--self-test"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_link_guard_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-hardlink-isolation.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_iso_deploy_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/make-isolated-deploy.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_beauty_sidecars_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-beauty-sidecars.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_stock_mission_data_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-stock-mission-data.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_own_inventory_selftest():
    _assert_pass(subprocess.run([sys.executable, "tools/own_inventory.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_pointer_order_scan_selftest():
    _assert_pass(subprocess.run([sys.executable, "tools/pointer_order_scan.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_capture_provenance_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-capture-provenance.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_provenance_gate_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/provenance_gate.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_review_anchor_stability_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-review-anchor-stability.py", "--self-test"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_review_pack_registry_guard():
    _assert_pass(subprocess.run([sys.executable, "scripts/build_review_pack.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_fail_reason_selftest():
    _assert_pass(subprocess.run([git_bash(), "scripts/check-contracts.sh", "--selftest"], **dict(OPTS, timeout=600), **console_child.child_kwargs(redirected=True)))


def test_object_mvp_currency_selftest():
    _assert_pass(subprocess.run([sys.executable, "scripts/check-object-mvp-currency.py", "--selftest"], **OPTS, **console_child.child_kwargs(redirected=True)))


def test_board_has_no_separate_selftest_rows():
    standalone = {"sampler_bindings_selftest", "bless_selftest", "cxx17_ratchet_selftest"}
    rows = [line for line in (ROOT / "scripts/check-contracts.sh").read_text(encoding="utf-8").splitlines()
            if line.startswith(("run_check ", "run_check_tri "))]
    remaining = [line for line in rows if "--selftest" in line or "--self-test" in line
                 or line.split(chr(34))[1] in standalone]
    assert not remaining, "Selftests belong in scripts/tests:\n" + "\n".join(remaining)


def _load_script(path):
    spec = importlib.util.spec_from_file_location("batch4_" + Path(path).stem, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_injected_blind_gate_echo_checker_fails(monkeypatch):
    module = _load_script("scripts/check-gate-echo-reads.py")
    assert module.selftest() == 0
    monkeypatch.setattr(module, "check_text", lambda *args: [])
    assert module.selftest() != 0


def test_injected_broken_alpha_comment_stripper_fails(monkeypatch):
    module = _load_script("scripts/check-render-contract-alpha-classes.py")
    assert module.selftest() == 0
    monkeypatch.setattr(module, "split_comments", lambda text: (text, []))
    assert module.selftest() != 0
