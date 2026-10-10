"""objhash: the shared COFF digest used by comment_guard and the clang-tidy safe-set runner."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import objhash  # noqa: E402
from test_comment_guard import make_obj  # noqa: E402


def _fixture(tmp_path, text=b"\x90\xc3"):
    repo = tmp_path / "repo"
    build = tmp_path / "build"
    (repo / "code").mkdir(parents=True)
    (build / "CMakeFiles" / "t.dir" / "code").mkdir(parents=True)
    src = repo / "code" / "a.cpp"
    src.write_text("int a;\n")
    obj = build / "CMakeFiles" / "t.dir" / "code" / "a.cpp.obj"
    obj.write_bytes(make_obj(text=text))
    t = src.stat().st_mtime_ns + 10_000_000
    os.utime(obj, ns=(t, t))
    return repo, build, src, obj


def test_digest_ignores_timestamp_and_debug_sections(tmp_path):
    a, b, c = tmp_path / "a.obj", tmp_path / "b.obj", tmp_path / "c.obj"
    a.write_bytes(make_obj(ts=1, dbg=b"AAAA"))
    b.write_bytes(make_obj(ts=999, dbg=b"longer debug payload"))
    c.write_bytes(make_obj(text=b"\x90\xc3"))
    da, db, dc = (objhash.obj_digest(str(p)) for p in (a, b, c))
    assert da["code_sha256"] == db["code_sha256"] != dc["code_sha256"]


def test_hash_then_compare_reports_identical_and_changed(tmp_path):
    repo, build, src, obj = _fixture(tmp_path)
    doc, problems = objhash.hash_files(str(repo), str(build), ["code/a.cpp"])
    assert not problems
    for text, expect in ((b"\x90\xc3", "OK"), (b"\x90\x90\xc3", "FAIL")):
        obj.write_bytes(make_obj(text=text))
        t = obj.stat().st_mtime_ns + 5_000_000
        os.utime(obj, ns=(t, t))
        doc["files"]["code/a.cpp"]["objs"][0]["mtime_ns"] = 0
        lines, bad = objhash.compare_files(doc)
        assert lines[0].startswith(expect), lines
        assert bad == (expect == "FAIL")


def test_header_is_skipped_and_unbuilt_source_is_a_problem(tmp_path):
    repo, build, _src, _obj = _fixture(tmp_path)
    (repo / "code" / "b.cpp").write_text("int b;\n")
    doc, problems = objhash.hash_files(str(repo), str(build), ["code/a.h", "code/b.cpp"])
    assert doc["files"]["code/a.h"]["status"] == "skip"
    assert any("no object" in p for p in problems)


def test_digest_ignores_the_incremental_link_checksum_section(tmp_path):
    a, b, c = tmp_path / "a.obj", tmp_path / "b.obj", tmp_path / "c.obj"
    a.write_bytes(make_obj(chks=b"\x01" * 8))
    b.write_bytes(make_obj(chks=b"\x02" * 8))          # only the checksum differs (NULL -> nullptr style edit)
    c.write_bytes(make_obj(text=b"\x90\xc3", chks=b"\x01" * 8))   # real code change
    da, db, dc = (objhash.obj_digest(str(p)) for p in (a, b, c))
    assert da["code_sha256"] == db["code_sha256"] != dc["code_sha256"]
