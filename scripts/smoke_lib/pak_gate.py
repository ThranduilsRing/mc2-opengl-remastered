"""Pak-default deploy gate: an install deployed with the pak default must actually serve stock assets from the pak.

deploy_payload.ensure_stock_pak writes <install>/.pak_deploy_status ("OK ..." | "FALLBACK <why>" | "SKIP <why>"). A smoke log of
such an install must show `[VFS] mount pak ... eligible=N` with N > 0 and a final `[PAK_IO v1] ... fst_opens=0`.
Installs without the marker (pre-pak deploys) are not judged.
"""
from __future__ import annotations

import re
from pathlib import Path

STATUS_NAME = ".pak_deploy_status"
_MOUNT = re.compile(r"\[VFS\] mount pak .*?eligible=(\d+)")
_FST_OPENS = re.compile(r"\[PAK_IO v1\] .*?\bfst_opens=(\d+)")


def pak_failures(install_dir, log_text: str) -> list[str]:
    try:
        status = (Path(install_dir) / STATUS_NAME).read_text(encoding="utf-8").strip()
    except OSError:
        return []
    if not status.startswith(("OK", "FALLBACK")):
        return []
    if status.startswith("FALLBACK"):
        return [f"deploy log lacks '[PAK_DEPLOY] OK' ({status}); the install serves stock assets from FSTs"]
    out = []
    mounts = [int(m) for m in _MOUNT.findall(log_text or "")]
    if not mounts or max(mounts) <= 0:
        out.append("smoke log has no '[VFS] mount pak ... eligible>0'")
    opens = [int(m) for m in _FST_OPENS.findall(log_text or "")]
    if not opens:
        out.append("smoke log has no '[PAK_IO v1]' line (pak status OK but fst_opens was never reported)")
    elif opens[-1] > 0:
        out.append(f"[PAK_IO v1] fst_opens={opens[-1]} (stock assets were read from FSTs, not the pak)")
    return out
