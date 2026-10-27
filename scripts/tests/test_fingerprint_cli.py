"""Reference CLI validation stops before leases or game launches."""
import sys
from unittest.mock import patch

import pytest

from scripts import run_smoke
from scripts.smoke_lib import host_lease


@pytest.mark.parametrize("args,commit,exe", [([], None, None),
    (["--ref-commit", "adec261b5"], "adec261b5", None),
    (["--ref-exe", "pre.exe"], None, "pre.exe")])
def test_cli_passes_reference_selection(args, commit, exe):
    with patch.object(sys, "argv", ["run_smoke.py", *args]), \
         patch.dict(run_smoke.os.environ, {"MC2_SMOKE_REQUIRE_FINGERPRINT": "1"}), \
         patch.object(run_smoke._fingerprint, "resolve_reference",
                      side_effect=ValueError("test stop before leases")) as resolve, \
         patch.object(run_smoke, "run_one") as launch:
        with pytest.raises(SystemExit) as exc:
            run_smoke.main()
    assert exc.value.code == 2
    resolve.assert_called_once_with(run_smoke.ROOT, ref_commit=commit, ref_exe=exe)
    launch.assert_not_called()


def test_cli_missing_head_preserves_fingerprint_opt_out(capsys, tmp_path):
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"")
    ok = type("V", (), {"ok": True, "banner": lambda self: ""})()
    with patch.object(sys, "argv", ["run_smoke.py", "--exe", str(exe)]), \
         patch.object(run_smoke._exe_guard, "check_exe", return_value=ok), \
         patch.dict(run_smoke.os.environ, {"MC2_SMOKE_REQUIRE_FINGERPRINT": "0"}), \
         patch.object(run_smoke._fingerprint, "resolve_reference",
                      side_effect=ValueError("cannot resolve fingerprint commit reference HEAD")), \
         patch.object(host_lease, "acquire",
                      side_effect=RuntimeError("test stop before lease")) as lease, \
         patch.object(run_smoke, "run_one") as launch:
        with pytest.raises(RuntimeError, match="test stop before lease"):
            run_smoke.main()
    lease.assert_called_once()
    launch.assert_not_called()
    err = capsys.readouterr().err
    assert "HEAD unavailable" in err and "identity is NOT verified" in err


@pytest.mark.parametrize("args", [["--ref-commit", "deadbeef"],
                                  ["--ref-exe", "missing-test.exe"]])
def test_cli_invalid_explicit_reference_is_fatal_with_opt_out(args, capsys):
    with patch.object(sys, "argv", ["run_smoke.py", *args]), \
         patch.dict(run_smoke.os.environ, {"MC2_SMOKE_REQUIRE_FINGERPRINT": "0"}), \
         patch.object(run_smoke._fingerprint, "resolve_reference",
                      side_effect=ValueError("invalid explicit reference")), \
         patch.object(host_lease, "acquire") as lease, \
         patch.object(run_smoke, "run_one") as launch:
        with pytest.raises(SystemExit) as exc:
            run_smoke.main()
    assert exc.value.code == 2
    assert "invalid explicit reference" in capsys.readouterr().err
    lease.assert_not_called()
    launch.assert_not_called()


def test_cli_references_are_mutually_exclusive():
    with patch.object(sys, "argv", ["run_smoke.py", "--ref-commit", "adec261b5",
                                   "--ref-exe", "pre.exe"]), \
         patch.object(run_smoke._fingerprint, "resolve_reference") as resolve, \
         patch.object(run_smoke, "run_one") as launch:
        with pytest.raises(SystemExit) as exc:
            run_smoke.main()
    assert exc.value.code == 2
    resolve.assert_not_called()
    launch.assert_not_called()


def test_cli_override_cannot_skip_broken_fingerprint_module():
    with patch.object(sys, "argv", ["run_smoke.py", "--ref-exe", "pre.exe"]), \
         patch.object(run_smoke, "_fingerprint", None), \
         patch.dict(run_smoke.os.environ, {"MC2_SMOKE_REQUIRE_FINGERPRINT": "0"}), \
         patch.object(run_smoke, "run_one") as launch:
        with pytest.raises(SystemExit) as exc:
            run_smoke.main()
    assert exc.value.code == 2
    launch.assert_not_called()
