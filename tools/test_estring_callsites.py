"""estring_callsites: the content seal of the call-site list is the git blob id of the file's LF form, so it does not depend on the checkout's line endings."""
import hashlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tests", "analyze"))
sys.path.insert(0, os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402
import estring_callsites as mod  # noqa: E402

TEXT = b"first line\nsecond line\n"


def git_blob_id(data: bytes) -> str:
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def test_seal_is_the_blob_id_of_the_lf_form_whatever_the_line_endings(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "ROOT", str(tmp_path))
    (tmp_path / "lf.cpp").write_bytes(TEXT)
    (tmp_path / "crlf.cpp").write_bytes(TEXT.replace(b"\n", b"\r\n"))
    got = mod.blob_hashes(["lf.cpp", "crlf.cpp", "gone.cpp"])
    assert got["lf.cpp"] == got["crlf.cpp"] == git_blob_id(TEXT)
    assert "gone.cpp" not in got                         # a file that cannot be read is left out


def test_seal_equals_what_git_prints_for_the_lf_form(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "ROOT", str(tmp_path))
    (tmp_path / "crlf.cpp").write_bytes(TEXT.replace(b"\n", b"\r\n"))
    printed = subprocess.run(["git", "hash-object", "--no-filters", "--stdin"], input=TEXT, capture_output=True, check=True,
                             **console_child.child_kwargs(redirected=True)).stdout.decode().strip()
    assert mod.blob_hashes(["crlf.cpp"])["crlf.cpp"] == printed


def test_check_follows_the_content_not_the_checkout(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "ROOT", str(tmp_path))
    listing = tmp_path / "callsites.txt"
    listing.write_text("# total 1 sites in 1 files\nlf.cpp:1:Data\n", encoding="utf-8")
    (tmp_path / "lf.cpp").write_bytes(b"x.Data()\n")
    assert mod.seal(str(listing)) == 0
    assert mod.check(str(listing)) == 0
    (tmp_path / "lf.cpp").write_bytes(b"x.Data()\r\n")        # the same content in the other line ending: still sealed
    assert mod.check(str(listing)) == 0
    (tmp_path / "lf.cpp").write_bytes(b"y.Data()\n")          # a real edit: the row was not verified against this text
    assert mod.check(str(listing)) == 1
