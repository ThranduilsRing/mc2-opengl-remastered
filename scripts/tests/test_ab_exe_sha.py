"""ab.py records the sha256 of the exe each arm launches and warns when both arms ran the same exe."""
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
import ab  # noqa: E402


def test_arm_records_hash_and_prints_line(tmp_path, capsys):
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"build-a")
    rec = ab.arm_exe_record("A", exe)
    digest = hashlib.sha256(b"build-a").hexdigest()
    assert rec == {"arm": "A", "path": str(exe), "exe_sha256": digest}
    assert capsys.readouterr().out.strip() == f"[AB] arm=A exe_sha256={digest} path={exe}"


def test_same_exe_warns_and_different_does_not(tmp_path):
    a, b, c = (tmp_path / n for n in ("a.exe", "b.exe", "c.exe"))
    a.write_bytes(b"one")
    b.write_bytes(b"one")
    c.write_bytes(b"two")
    ra, rb, rc = (ab.arm_exe_record(n, p) for n, p in (("A", a), ("B", b), ("B", c)))
    assert "same exe" in ab.same_exe_warning(ra, rb)
    assert ab.same_exe_warning(ra, rc) is None
