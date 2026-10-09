#!/usr/bin/env python3
"""Run the visible Vulkan no-GL-interop strict canary with the current gate stack."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


# NAMING NOTE (H5, VK-NOINTEROP-STRICT-REVIEW): several gates below are historically
# named MC2_VK_*_INTEROP=1 (e.g. MC2_VK_SHADOW_INTEROP, MC2_VK_CSM_ARRAY_INTEROP,
# MC2_VK_CSM_FULLMAP_INTEROP). Inside THIS no-interop proof they do NOT mean "use a
# GL<->VK exportable-memory handoff". They arm the VK producer/consumer islands, which
# are reconciled to the no-interop invariant by the shadow producer running VK-native
# (direct-chain) and the terrain consumer pointing at its VkImageView directly, so no
# per-frame GL CSM readback fires. The hardened checker enforces this: if any
# MC2_VK_*_INTEROP=1 is set, it REQUIRES the MC2_VK_SHARED_GBUFFER umbrella in the
# exe-confirmed gate_env.
STRICT_ENV = {
    "MC2_RENDER_BACKEND": "vulkan",
    "MC2_DEBUG_STATE_DUMP": "1",
    "MC2_DIAG_TAGS": "CONFIG,BUILD,DEVICE",
    "MC2_VK_SHARED_GBUFFER": "1",
    "MC2_VK_SHARED_SCENE_STRICT": "1",
    "MC2_VK_POST_COMPOSITE": "1",
    "MC2_VK_POST_COMPOSITE_DIRECT_CHAIN": "1",
    "MC2_VK_PRESENT_DIRECT_FBO": "1",
    "MC2_VK_PRESENT_NO_GL_IMPORT": "1",
    "MC2_VK_PRESENT_TAIL_HOOK": "1",
    "MC2_VK_PRESENT_SKIP_GL_HANDOFF": "1",
    "MC2_VK_PRESENT_NATIVE_STRICT": "1",
    "MC2_VK_HUD": "1",
    "MC2_VK_HUD_COLOR_PROOF": "1",
    "MC2_VK_HUD_SKIP_GL": "1",
    "MC2_VK_IMGUI": "1",
    "MC2_VK_TERRAIN_ALL_LODS": "1",
    "MC2_VK_TERRAIN_SKIRTS": "1",
    "MC2_VK_TERRAIN_VISUALDISPLACE_FAR": "1",
    "MC2_VK_PROP_DIRECT_CHAIN": "1",
    "MC2_VK_MECH_FEED_NATIVE": "1",
    "MC2_VK_MECH_DIRECT_CHAIN": "1",
    "MC2_VK_WATER_TIER1": "1",
    "MC2_VK_WATER_DIRECT_CHAIN": "1",
    "MC2_VK_OBJECTID_DIRECT_CHAIN": "1",
    "MC2_SHADOW_ENABLE": "1",
    "MC2_SHADOW_CSM": "1",
    "MC2_VK_SHADOW_INTEROP": "1",
    "MC2_VK_CSM_ARRAY_INTEROP": "1",
    "MC2_VK_CSM_FULLMAP_INTEROP": "1",
    "MC2_VK_STATIC_SHADOW_MAP": "0",
    "MC2_VK_POST_FOG_NATIVE": "1",
    "MC2_VK_POST_FOG_DIRECT_CHAIN": "1",
    "MC2_SSAO": "1",
    "MC2_VK_POST_SSAO": "1",
    "MC2_VK_POST_SSAO_DIRECT_CHAIN": "1",
    "MC2_VK_POST_SCREENFX": "1",
    "MC2_VK_POST_SCREENFX_SHADOW": "1",
    "MC2_VK_POST_SCREENFX_DIRECT_CHAIN": "1",
    "MC2_HZB_BUILD": "1",
    "MC2_VK_POST_HZB": "1",
    "MC2_VK_POST_HZB_DIRECT_CHAIN": "1",
    "MC2_VK_NO_GL_INTEROP_STRICT": "1",
    # H4: arm the GL draw / screen-shadow canaries so the checker can REQUIRE them
    # present with draws=0 / any_samples=0 (proves the GL scene-draw and screen-shadow
    # paths issued nothing under the VK-owned strict stack).
    "MC2_VK_TERRAIN_OVERLAY_GL_CANARY": "1",
    "MC2_VK_DECAL_STATIC_GL_CANARY": "1",
    "MC2_VK_TERRAIN_DECAL_GL_CANARY": "1",
    "MC2_VK_SCREENSHADOW_GL_CANARY": "1",
}

# VK-UMBRELLA-DIRECT-CHAIN-DEFAULT-1 (item-4 prerequisite): the direct-chain gates
# now default ON under the MC2_VK_SHARED_GBUFFER umbrella. With --minimal-promotion the
# runner DROPS these from the explicit env so the proof shows the umbrella arming them by
# default (every island still reports direct_chain=1 used_interop=0). The hardened
# checker's H5 reconciles the MC2_VK_*_INTEROP producer names via the umbrella
# (MC2_VK_SHARED_GBUFFER=1). MC2_VK_OBJECTID_DIRECT_CHAIN is NOT a
# default member (view-mode-only canary) and is retained.
DIRECT_CHAIN_GATES = (
    "MC2_VK_PROP_DIRECT_CHAIN",
    "MC2_VK_MECH_DIRECT_CHAIN",
    "MC2_VK_WATER_DIRECT_CHAIN",
    "MC2_VK_POST_FOG_DIRECT_CHAIN",
    "MC2_VK_POST_SSAO_DIRECT_CHAIN",
    "MC2_VK_POST_SCREENFX_DIRECT_CHAIN",
    "MC2_VK_POST_HZB_DIRECT_CHAIN",
    "MC2_VK_POST_COMPOSITE_DIRECT_CHAIN",
)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def main() -> int:
    root = repo_root()
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe")
    ap.add_argument("--mission", default="mc2_01")
    ap.add_argument("--out-dir")
    ap.add_argument("--duration", type=int, default=210)
    ap.add_argument("--settle", type=int, default=80)
    ap.add_argument("--trigger", type=int, default=120)
    ap.add_argument("--view-mode", help="Set MC2_VIEWMODE_FRAMEWORK=1 and MC2_VIEW_MODE to this value.")
    ap.add_argument("--resource-strict", action="store_true",
                    help="Also abort on exportable Win32 resource creation / GL memory import.")
    ap.add_argument("--minimal-promotion", action="store_true",
                    help="Drop the direct-chain gates from the explicit env so the run "
                         "proves the MC2_VK_SHARED_GBUFFER umbrella arms them by default.")
    ap.add_argument("--env", action="append", default=[],
                    help="Additional KEY=VALUE environment override. May be passed multiple times.")
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--allow-partial", action="store_true")
    args = ap.parse_args()

    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H-%M-%SZ")
    out_dir = Path(args.out_dir) if args.out_dir else root / ".claude" / f"vk-nointerop-strict-visible-{stamp}"

    strict_env = dict(STRICT_ENV)
    if args.minimal_promotion:
        for key in DIRECT_CHAIN_GATES:
            strict_env.pop(key, None)

    env = os.environ.copy()
    env.update(strict_env)
    # Ensure a stale outer-shell direct-chain gate can't leak into a minimal-promotion
    # proof (would defeat the point). Explicitly remove them from the child env.
    if args.minimal_promotion:
        for key in DIRECT_CHAIN_GATES:
            env.pop(key, None)
    # Clean shutdown after the bookmark sweep so the VK island destroy lines
    # (forbidden_* counters) + final [VK_PRESENT] logHealth reach the capture log --
    # the hardened checker REQUIRES them. Without this the game is killed on timeout
    # and those shutdown-only lines are absent (the prior proof's exact gap).
    env["MC2_VISUAL_CAPTURE_EXIT_AFTER"] = "1"
    if args.resource_strict:
        env["MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT"] = "1"
    if args.view_mode:
        env["MC2_VIEWMODE_FRAMEWORK"] = "1"
        env["MC2_VIEW_MODE"] = args.view_mode
    for item in args.env:
        if "=" not in item:
            ap.error("--env entries must be KEY=VALUE")
        key, value = item.split("=", 1)
        if not key:
            ap.error("--env key must be non-empty")
        env[key] = value

    cmd = [
        sys.executable,
        str(root / "scripts" / "run_visual_capture.py"),
        "--exe",
        args.exe,
        "--mission",
        args.mission,
        "--out-dir",
        str(out_dir),
        "--duration",
        str(args.duration),
        "--settle",
        str(args.settle),
        "--trigger",
        str(args.trigger),
    ]
    if not args.headless:
        cmd.append("--show-window")
    if args.allow_partial:
        cmd.append("--allow-partial")

    print("[vk-strict-visible] out_dir=%s" % out_dir)
    extra = []
    if args.resource_strict:
        extra.append("MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT")
    if args.view_mode:
        extra.extend(["MC2_VIEWMODE_FRAMEWORK", "MC2_VIEW_MODE"])
    extra.extend(item.split("=", 1)[0] for item in args.env)
    gates = sorted(set(strict_env) | set(extra))
    print("[vk-strict-visible] gates=%s" % ",".join(gates))
    out_dir.mkdir(parents=True, exist_ok=True)
    with (out_dir / "vk_strict_env.txt").open("w", encoding="utf-8") as f:
        for key in gates:
            f.write(f"{key}={env.get(key, '')}\n")
    # The capture scrubs undeclared ambient MC2_* vars, so every proof gate must be
    # declared or the run silently measures the default (GL) configuration.
    for key in gates:
        cmd.extend(["--env-declare", f"{key}={env.get(key, '')}"])
    return subprocess.call(cmd, cwd=str(root), env=env)


if __name__ == "__main__":
    raise SystemExit(main())
