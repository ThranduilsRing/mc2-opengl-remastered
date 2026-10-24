"""Firewall diagnostics match the exact executable and never mutate processes."""
import subprocess
from pathlib import Path

from smoke_lib import firewall_dialog as fw


def test_exact_exe_only(tmp_path):
    exe = tmp_path / "lane space" / "mc2.exe"
    cmd = f'rundll32.exe FirewallControlPanel.dll,ShowNotificationDialog 0 "{exe}"'
    rows = [
        {"ProcessId": 12, "CommandLine": cmd},
        {"ProcessId": 13, "CommandLine": cmd.replace("mc2.exe", "other.exe")},
        {"ProcessId": 14, "CommandLine": cmd.replace("FirewallControlPanel", "Other")},
        {"ProcessId": 15, "CommandLine": None},
    ]
    assert fw.matching_pids(rows, exe) == {12}


def test_poll_warns_once_and_checks_at_end(monkeypatch, capsys):
    monkeypatch.setattr(fw.sys, "platform", "win32")
    calls = []
    monkeypatch.setattr(fw, "query", lambda exe: calls.append(exe) or {42})
    guard = fw.Guard(Path("mc2.exe"))
    guard.poll()
    guard.poll()
    guard.poll(final=True)
    assert len(calls) == 2
    assert capsys.readouterr().err.count("WARN FirewallControlPanel dialog pid=42") == 1


def test_query_failure_is_advisory(monkeypatch, capsys):
    monkeypatch.setattr(fw.sys, "platform", "win32")
    def fail(exe):
        raise subprocess.TimeoutExpired("powershell", 5)
    monkeypatch.setattr(fw, "query", fail)
    fw.Guard(Path("mc2.exe")).poll()
    assert "WARN dialog query failed" in capsys.readouterr().err
