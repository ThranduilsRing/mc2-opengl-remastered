"""laptop-rows-green: a row whose tool cannot exist on the host exits 77 with a HOST_INFRA line (never 0), and the board
only turns that into INFRA under MC2_HOST_INFRA_OK=1 (the farm laptop's gate env); the PC keeps it red."""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "mc2texcook"))
import check_ktx2_valid


def test_missing_ktx_is_host_infra_77_never_a_pass(monkeypatch, capsys):
    monkeypatch.setattr(check_ktx2_valid, "find_ktx", lambda: None)
    monkeypatch.setattr(sys, "argv", ["check_ktx2_valid.py"])
    assert check_ktx2_valid.main() == 77
    assert "HOST_INFRA ktx2_valid" in capsys.readouterr().out


def _bash():
    for cand in (os.environ.get("MC2_BASH"), "C:/Program Files/Git/bin/bash.exe", "bash"):
        if cand:
            return cand


def _board(infra_ok):
    """Run the real board's _finish_check path on a fake row by sourcing the function definitions is brittle; instead run the
    one row that is guaranteed host-less here: a nonexistent tool through _host_need inside a --rows selection is not possible,
    so the shell contract is checked textually plus _host_need itself."""
    text = (ROOT / "scripts" / "check-contracts.sh").read_text(encoding="utf-8")
    return text


def test_board_wires_host_infra_only_behind_the_env_opt_in():
    text = _board(False)
    assert '"$rc" -eq 77' in text and 'MC2_HOST_INFRA_OK:-0}" = "1"' in text and "grep -q 'HOST_INFRA'" in text
    assert "[HOST_INFRA_ROWS]" in text and "NOT a pass" in text
    for row in ("analyze_dangling", "shadow_bake_init", "abl_static_alloc", "mission_codec_golden"):
        line = next(l for l in text.splitlines() if f'run_nightly "{row}"' in l)
        assert "HOST_INFRA" in line or "_host_need" in line, row


def test_host_need_returns_77_for_a_missing_tool():
    text = (ROOT / "scripts" / "check-contracts.sh").read_text(encoding="utf-8")
    start = text.index("_host_need() {")
    end = text.index("\n}\n", start) + 3
    fn = text[start:end]
    r = subprocess.run([_bash(), "-c", fn + '\n_host_need definitely-not-a-tool-xyz; echo rc=$?'],
                       capture_output=True, text=True)
    assert "HOST_INFRA: definitely-not-a-tool-xyz" in r.stdout and "rc=77" in r.stdout
    r = subprocess.run([_bash(), "-c", fn + '\n_host_need bash; echo rc=$?'], capture_output=True, text=True)
    assert "rc=0" in r.stdout
