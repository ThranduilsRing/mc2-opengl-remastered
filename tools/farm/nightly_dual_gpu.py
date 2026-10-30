"""Nightly smoke-lite on the host's 7900 XTX and RTX 2070 SUPER, serially."""
from __future__ import annotations

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts import console_child
from scripts.smoke_lib import vk_profile, vk_validation

GPUS = ("7900 XTX", "2070")
FLOOR = 512 * 1024 * 1024


def grade(text, gpu):
    """Fail closed for missing evidence, wrong device, refused work or excess residency."""
    problems = []
    profiles = vk_profile.parse(text)
    if not profiles or any(gpu.lower() not in p.device.lower() for p in profiles):
        problems.append("device_missing_or_mismatched")
    problems.extend(vk_profile.grade(profiles))
    messages = vk_validation.parse(text)
    problems.extend(vk_validation.grade(messages, asked=True))
    if "[VK_CAPABILITY v1]" not in text:
        problems.append("capability_missing")
    # Successful mip/resample reductions are the requested low texture policy,
    # rather than missing uploads. Terminal refusals (placeholder/off/skip) fail.
    refusals = sum(1 for line in text.splitlines() if "[VK_RESIDENCY v1] refused=" in line
                   and not re.search(r"fallback=(?:mip|resample)_\d+x\d+", line))
    if refusals:
        problems.append(f"refused={refusals}")
    ledgers = [json.loads(line.split("phase=run_end ", 1)[1]) for line in text.splitlines()
               if "[VK_RESIDENCY v1] phase=run_end " in line]
    if not ledgers:
        problems.append("residency_missing")
    for ledger in ledgers:
        budget = ledger["budget_bytes"]
        if not (0 < budget <= FLOOR and ledger["peak_bytes"] <= budget and ledger["within_budget"]):
            problems.append("residency_over_budget")
        if ledger["denials"]:
            problems.append("residency_denials")
        if ledger["vma_usage_bytes"] > FLOOR:
            problems.append("vram_over_floor")
    peak = max((x["peak_bytes"] for x in ledgers), default=0)
    errors = vk_validation.summarize(messages).errors
    return problems, peak, refusals, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=Path(os.environ.get(
        "MC2_FARM_RESULT_DIR", "E:/mc2-archive-2026-10/proofs/nightly-dual-gpu")))
    args = parser.parse_args()
    output = args.out / datetime.now().strftime("%Y%m%d-%H%M%S")
    output.mkdir(parents=True, exist_ok=False)
    failed = False
    reports = []
    for gpu in GPUS:
        artifacts = output / gpu.replace(" ", "-")
        env = dict(os.environ, MC2_VK_DEVICE=gpu, MC2_HARNESS_GPU="any",
                   MC2_VULKAN_VALIDATION="1", MC2_DEBUG_STATE_DUMP="1",
                   MC2_OPTIONS_OVERRIDE="ShadowQuality=0,SpatialAA=0",
                   MC2_RENDER_SCALE="0.5", MC2_SHADOW_ENABLE="0",
                   MC2_VK_POST_SCREENFX_SHADOW="0", MC2_CLOUD_SHADOW="0",
                   MC2_WIDTH="1920", MC2_HEIGHT="1080",
                   MC2_HOST_LEASE_TIMEOUT="1200",
                   TMP="G:/mc2-tmp", TEMP="G:/mc2-tmp", TMPDIR="G:/mc2-tmp",
                   MC2_SMOKE_ARTIFACT_ROOT=str(artifacts))
        with (output / f"{gpu.replace(' ', '-')}.log").open("w", encoding="utf-8") as log:
            result = subprocess.run([sys.executable, str(ROOT / "scripts/run_smoke.py"),
                "--lite", "--keep-logs", "--exe", str(args.exe), "--ref-exe", str(args.exe)],
                cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                **console_child.child_kwargs(redirected=True))
        logs = [list(artifacts.glob(f"*/{mission}.log")) for mission in ("mc2_10", "mc2_01")]
        problems, peak, refusals, errors = [], 0, 0, 0
        if result.returncode:
            problems.append(f"smoke_exit={result.returncode}")
        for mission_logs in logs:
            if len(mission_logs) != 1:
                problems.append("mission_log_missing")
            for path in mission_logs:
                try:
                    issues, high, refused, count = grade(path.read_text(encoding="utf-8", errors="replace"), gpu)
                    problems.extend(issues)
                    peak, refusals, errors = max(peak, high), refusals + refused, errors + count
                except (ValueError, KeyError, TypeError):
                    problems.append("malformed_evidence")
        failed |= bool(problems)
        line = (f"GPU={gpu} smoke-lite={'FAIL' if problems else 'PASS'} peak_mib={peak / 1048576:.1f} "
                f"floor_mib=512 refused={refusals} validation_errors={errors} "
                f"problems={','.join(problems) or 'none'}")
        reports.append(line)
        print(line, flush=True)
        (output / "report.txt").write_text("\n".join(reports) + "\n", encoding="utf-8")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
