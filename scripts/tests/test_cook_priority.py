"""Cook children cannot inherit an oversubscribed OpenMP setting."""
import os
from pathlib import Path
import subprocess
import sys

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.content_pipeline import texcook
import kit_payload


@pytest.mark.skipif(sys.platform != "win32", reason="Windows priority class")
def test_texconv_env_and_priority(tmp_path, monkeypatch):
    monkeypatch.setenv("OMP_NUM_THREADS", "16")
    monkeypatch.setattr(texcook.console_child, "has_console", lambda: True)

    def run(command, **kwargs):
        assert kwargs["env"]["OMP_NUM_THREADS"] == "1"
        assert kwargs["env"]["PATH"] == os.environ["PATH"]
        assert kwargs["creationflags"] & subprocess.BELOW_NORMAL_PRIORITY_CLASS
        raise RuntimeError("spawn verified")

    monkeypatch.setattr(texcook.subprocess, "run", run)
    with pytest.raises(RuntimeError, match="spawn verified"):
        texcook._texconv_level(np.zeros((4, 4, 4), dtype=np.uint8), "BC7_UNORM", tmp_path)


def test_deploy_pool_cannot_exceed_jobs(tmp_path, monkeypatch):
    executor = kit_payload.ThreadPoolExecutor
    workers = []
    def pool(*, max_workers):
        workers.append(max_workers)
        return executor(max_workers=max_workers)
    monkeypatch.setattr(kit_payload, "ThreadPoolExecutor", pool)
    kit_payload.reconcile_stock_textures(tmp_path, {"sha256": {}, "cooked_sha256": {}}, jobs=999)
    assert workers == [int(Path("A:/Games/mc2-ops/JOBS").read_text())]
