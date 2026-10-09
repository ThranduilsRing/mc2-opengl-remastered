#!/usr/bin/env python3
"""scripts/check-ktx2-probe.py -- CI wrapper for the KTX2 bake+inspect probe.

Part of TRACKG-ASSET-PIPELINE-PROBE-OPUS-1 (slice KTX2-BAKE-PROBE-1).

Runs tools/asset_probe/ktx2_probe.py and exits 0 only if every assertion
passes. If Pillow is unavailable (CI without the dependency), it SKIPs
gracefully with exit 0 and a clear message so the gate does not break.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PROBE = REPO_ROOT / "tools" / "asset_probe" / "ktx2_probe.py"


def main() -> int:
    try:
        import PIL  # noqa: F401
    except Exception:
        print("SKIP check-ktx2-probe: Pillow (PIL) not available; "
              "skipping offline KTX2 bake probe.")
        return 0

    res = subprocess.run([sys.executable, str(PROBE)], creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if res.returncode != 0:
        print(f"FAIL check-ktx2-probe: probe exited {res.returncode}",
              file=sys.stderr)
        return 1
    print("PASS check-ktx2-probe: KTX2 bake+inspect assertions all passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
