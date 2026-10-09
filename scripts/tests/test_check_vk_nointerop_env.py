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


def test_umbrella_must_be_whole_line(capsys):
    for bad in ("MC2_VK_SHARED_GBUFFER=10", "XMC2_VK_SHARED_GBUFFER=1", "MC2_VK_SHARED_GBUFFER=0"):
        env = BASE + bad + "\nMC2_VK_SHADOW_INTEROP=1\n"
        assert chk.check_env(env, no_resource_strict=False) is True, bad
        assert "NONE" in capsys.readouterr().out


def test_missing_required_receipt_fails():
    env = "MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT=1\nMC2_VK_PRESENT_NATIVE_STRICT=1\n"
    assert chk.check_env(env, no_resource_strict=False) is True


def test_prefixed_required_receipt_does_not_count():
    env = BASE.replace("MC2_VK_PRESENT_NATIVE_STRICT=1", "XMC2_VK_PRESENT_NATIVE_STRICT=1")
    assert chk.check_env(env, no_resource_strict=False) is True


def test_no_resource_strict_drops_only_that_receipt():
    env = "MC2_VK_NO_GL_INTEROP_STRICT=1\nMC2_VK_PRESENT_NATIVE_STRICT=1\n"
    assert chk.check_env(env, no_resource_strict=True) is False
    assert chk.check_env(env, no_resource_strict=False) is True


GOOD_LOG = (
    "[VK_POST_FOG_NATIVE] [HEALTH] runtime_gate=1 attempted=9 used_vulkan=9 direct_chain=1 used_interop=0\n"
    "[VK_POST_SSAO_NATIVE] [HEALTH] runtime_gate=1 attempted=9 used_vulkan=9 direct_chain=1 used_interop=0\n"
    "[VK_POST_SCREENFX_NATIVE] [HEALTH] runtime_gate=1 attempted=9 used_vulkan=9 direct_chain=1 used_interop=0\n"
    "[VK_POST_HZB_NATIVE] [HEALTH] runtime_gate=1 attempted=9 used_vulkan=9 direct_chain=1 used_interop=0\n"
    "[VK_SHADOW_ISLAND] [VK_SHADOW_INTEROP] used_vulkan=9 used_interop=0 direct_chain=1 validation_errors=0\n")


def test_direct_chain_islands_pass_on_good_log():
    assert chk.check_direct_chain(GOOD_LOG) is False


def test_direct_chain_missing_line_fails():
    assert chk.check_direct_chain(GOOD_LOG.replace("[VK_POST_HZB_NATIVE] [HEALTH]", "[X]")) is True


def test_direct_chain_interop_or_unused_fails():
    assert chk.check_direct_chain(GOOD_LOG.replace("used_interop=0 direct_chain=1", "used_interop=2 direct_chain=1")) is True
    assert chk.check_direct_chain(GOOD_LOG.replace("used_vulkan=9 direct_chain=1 used_interop=0\n[VK_POST_SSAO", "used_vulkan=0 direct_chain=1 used_interop=0\n[VK_POST_SSAO")) is True
    assert chk.check_direct_chain(GOOD_LOG.replace("direct_chain=1 used_interop=0\n[VK_POST_SSAO", "direct_chain=0 used_interop=0\n[VK_POST_SSAO")) is True
