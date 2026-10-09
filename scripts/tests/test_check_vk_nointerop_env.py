#!/usr/bin/env python3
"""check_env() in scripts/check_vk_nointerop_log.py: H5 reconciles MC2_VK_*_INTEROP=1
names through the MC2_VK_SHARED_GBUFFER umbrella.

Run:
  py -3 -m pytest scripts/tests/test_check_vk_nointerop_env.py -v
"""
from __future__ import annotations

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location(
    "check_vk_nointerop_log", ROOT / "scripts" / "check_vk_nointerop_log.py")
chk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(chk)

BASE = ("MC2_VK_NO_GL_INTEROP_STRICT=1\nMC2_VK_NO_GL_INTEROP_RESOURCE_STRICT=1\n"
        "MC2_VK_PRESENT_NATIVE_STRICT=1\n")


def test_umbrella_reconciles_interop_names(capsys):
    env = BASE + "MC2_VK_SHARED_GBUFFER=1\nMC2_VK_SHADOW_INTEROP=1\nMC2_VK_CSM_ARRAY_INTEROP=1\n"
    assert chk.check_env(env, no_resource_strict=False) is False
    assert "umbrella-default" in capsys.readouterr().out


def test_interop_without_umbrella_fails(capsys):
    env = BASE + "MC2_VK_SHADOW_INTEROP=1\n"
    assert chk.check_env(env, no_resource_strict=False) is True
    assert "NONE" in capsys.readouterr().out


def test_no_interop_names_needs_no_reconciliation():
    assert chk.check_env(BASE, no_resource_strict=False) is False


def test_checker_never_reads_retired_shadow_direct_chain():
    assert "MC2_VK_SHADOW_DIRECT_CHAIN" not in (ROOT / "scripts" / "check_vk_nointerop_log.py").read_text()
