import json
import os
from pathlib import Path
from types import SimpleNamespace

import pytest
from tools import pipeline_warm as warm


def test_pipeline_warm_process_env_status_and_freshness(tmp_path, monkeypatch, capsys):
    install = tmp_path / "install"
    install.mkdir()
    exe = install / "mc2.exe"
    exe.write_bytes(b"exe")
    (install / "launcher_env.json").write_text(json.dumps({
        "MC2_CLOUD_SHADOW": "1", "MC2_TERRAIN_LOD_CHUNK": "64",
        "MC2_COLORMAP_KTX2": "1", "ShadowQuality": "2", "last_campaign": "ignored",
    }))
    monkeypatch.setattr(warm.deploy_payload, "_enumerate_matching_processes", lambda _: [(123, str(exe))])
    with pytest.raises(RuntimeError, match="pid=123"):
        warm.warm(install, ["mc2_01", "mc2_10"])

    monkeypatch.setattr(warm.deploy_payload, "_enumerate_matching_processes", lambda _: [])
    calls, releases = [], []
    monkeypatch.setattr(warm.host_lease, "hold", lambda *a, **k: None)
    monkeypatch.setattr(warm.host_lease, "release_batch", lambda tag: releases.append(tag))
    cache = install / "cache/vulkan/current.bin"
    cache.parent.mkdir(parents=True)

    def smoke(command, **kwargs):
        calls.append(command)
        env = kwargs["env"]
        assert env["MC2_CLOUD_SHADOW"] == "1"
        assert env["MC2_TERRAIN_LOD_CHUNK"] == "64"
        assert env["MC2_COLORMAP_KTX2"] == "1"
        assert "ShadowQuality=2" in env["MC2_OPTIONS_OVERRIDE"]
        assert "last_campaign" not in env
        assert env["MC2_SMOKE_FIXED_TIMESTEP"] == "0"
        assert "--headless" not in command
        assert "--no-lease" not in command
        root = Path(env["MC2_SMOKE_ARTIFACT_ROOT"])
        root.mkdir(parents=True)
        mission = command[command.index("--mission") + 1]
        (root / f"{mission}.log").write_text(
            f"[VK_PIPECACHE] status={'rejected' if len(calls) == 1 else 'warm'} reason=none\n"
            "[VK_LOAD_RESOURCE v1] pipeline_creation_ms=12.5\n"
            "[TIMING v1] event=mission_ready elapsed_ms=100\n")
        cache.write_bytes(b"cache")
        os.utime(cache, ns=(exe.stat().st_mtime_ns + 1000000,) * 2)
        return SimpleNamespace(returncode=0, stdout="", stderr="")

    monkeypatch.setattr(warm.subprocess, "run", smoke)
    warm.warm(install, ["mc2_01", "mc2_10"])
    assert len(calls) == 2 and len(releases) == 1
    assert "status_before=rejected creation_ms_first=12.500 cache_bytes=5" in capsys.readouterr().out
    os.utime(cache, ns=(exe.stat().st_mtime_ns,) * 2)
    with pytest.raises(RuntimeError, match="newer than mc2.exe"):
        warm.cache_bytes(install)
    with pytest.raises(RuntimeError, match="missing pipeline"):
        warm.parse_metrics("[VK_PIPECACHE] status=warm")


def test_pipeline_warm_explicit_launcher_override(tmp_path):
    (tmp_path / "launcher_env.json").write_text('{"MC2_CLOUD_SHADOW":"1","ShadowQuality":"2"}')
    env = warm.launcher_env(tmp_path, {"MC2_CLOUD_SHADOW": "0", "MC2_OPTIONS_OVERRIDE": "ShadowQuality=1"})
    assert env["MC2_CLOUD_SHADOW"] == "0"
    assert env["MC2_OPTIONS_OVERRIDE"] == "ShadowQuality=2,ShadowQuality=1"
