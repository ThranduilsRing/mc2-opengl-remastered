"""The nightly must reject false greens even when smoke-lite exits successfully."""
import json

import pytest

from tools.farm.nightly_dual_gpu import FLOOR, grade


def evidence(**overrides):
    ledger = dict(budget_bytes=FLOOR, peak_bytes=FLOOR - 1, within_budget=True,
                  denials=0, vma_usage_bytes=FLOOR - 1)
    ledger.update(overrides)
    return ('[VK_PROFILE v1] verdict=ok device="AMD Radeon RX 7900 XTX" api=1.3.0 '
            'instance_api=1.3.0 floor=1.2 required_missing=- wanted_missing=- items=- enabled=- extensions=-\n'
            '[VK_CAPABILITY v1] low_floor_mib=512 refused=-\n'
            '[VK_VALIDATION v1] severity=info id=layer_enabled text="enabled"\n'
            '[VK_RESIDENCY v1] phase=run_end ' + json.dumps(ledger) + '\n')


def test_clean_potato_and_successful_low_quality_resampling():
    assert grade(evidence(), "7900 XTX")[0] == []
    assert grade(evidence() + '[VK_RESIDENCY v1] refused=texture extent=4096x4096 fallback=resample_2048x2048\n', "7900 XTX")[0] == []


@pytest.mark.parametrize("change,reason", [
    ({"peak_bytes": FLOOR + 1}, "residency_over_budget"),
    ({"budget_bytes": FLOOR * 2}, "residency_over_budget"),
    ({"denials": 1}, "residency_denials"),
    ({"vma_usage_bytes": FLOOR + 1}, "vram_over_floor"),
])
def test_rejects_budget_failures(change, reason):
    assert reason in grade(evidence(**change), "7900 XTX")[0]


def test_missing_evidence_and_wrong_gpu():
    assert "residency_missing" in grade("", "7900 XTX")[0]
    assert "device_missing_or_mismatched" in grade(evidence(), "2070")[0]
    assert grade(evidence().replace("id=layer_enabled", "id=other"), "7900 XTX")[0]


def test_skipped_draws_and_validation_errors():
    text = evidence() + ('[VK_RESIDENCY v1] refused=framering fallback=skip_draw\n'
                         '[VK_VALIDATION v1] severity=error id=VUID-test text="bad"\n')
    problems, _, refused, errors = grade(text, "7900 XTX")
    assert problems and refused == errors == 1
