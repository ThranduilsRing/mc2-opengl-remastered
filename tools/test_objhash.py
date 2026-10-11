"""objhash: the shared COFF digest used by comment_guard and the clang-tidy safe-set runner."""
import os
import struct
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


def test_same_basename_in_two_directories_resolves_to_the_sources_own_object(tmp_path):
    build = tmp_path / "build"
    for d in ("out/mclib/CMakeFiles/mclib.dir", "out/mclib/stuff/CMakeFiles/stuff.dir"):
        (build / d).mkdir(parents=True)
        (build / d / "color.cpp.obj").write_bytes(make_obj())
    own = objhash.find_objs(str(build), "mclib/color.cpp")
    assert len(own) == 1 and own[0].replace("\\", "/").endswith("out/mclib/CMakeFiles/mclib.dir/color.cpp.obj")
    nested = objhash.find_objs(str(build), "mclib/stuff/color.cpp")
    assert len(nested) == 1 and "stuff.dir" in nested[0]


def test_digest_masks_the_build_fingerprint_literal_in_rdata(tmp_path):
    lit_clean = b"MC2FP1|3f45f77dfeb8|0|claude/x|2026-10-10T10:00:00|A:/Games/mc2-main|FP1END\x00"
    lit_dirty = b"MC2FP1|3f45f77dfeb8|1|claude/x|2026-10-10T10:09:55|A:/Games/mc2-main|FP1END\x00"
    assert len(lit_clean) == len(lit_dirty)
    a, b, c = tmp_path / "a.obj", tmp_path / "b.obj", tmp_path / "c.obj"
    a.write_bytes(make_obj(rdata=b"hello" + lit_clean))
    b.write_bytes(make_obj(rdata=b"hello" + lit_dirty))                    # dirty flag + build time differ only
    c.write_bytes(make_obj(rdata=b"jello" + lit_clean))                    # a real data change
    da, db, dc = (objhash.obj_digest(str(p)) for p in (a, b, c))
    assert da["code_sha256"] == db["code_sha256"] != dc["code_sha256"]


def test_date_and_time_literals_are_masked_but_other_strings_are_not():
    a = b"build: \x00Oct 10 2026\x0019:41:07\x00tail"
    b = b"build: \x00Nov  3 2027\x0003:02:59\x00tail"
    assert objhash._mask_fingerprint(a) == objhash._mask_fingerprint(b)
    assert objhash._mask_fingerprint(a) != objhash._mask_fingerprint(a.replace(b"build", b"built"))
    assert len(objhash._mask_fingerprint(a)) == len(a)


def test_date_and_time_string_symbols_mask_hash_and_text():
    a = "??_C@_0M@BBCJAGCC@Oct?510?52026@|1|0|0|x"
    b = "??_C@_0M@AAAAAAAA@Oct?510?52026@|1|0|0|x"
    assert objhash._mask_symbol(a) == objhash._mask_symbol(b)
    t1, t2 = "??_C@_08BAGEGGNC@20?303?350@", "??_C@_08AAAAAAAA@20?303?350@"
    assert objhash._mask_symbol(t1) == objhash._mask_symbol(t2)
    other = "??_C@_05ABCDEFGH@hello@"
    assert objhash._mask_symbol(other) == other             # an ordinary literal keeps its hash


def _coff(code, data, symbols, reloc_sym, bss=0):
    """Synthetic COFF: .text (one relocation to symbols[reloc_sym]) + .data (+ .bss of `bss` bytes). symbols = (name, value, section number, class)."""
    secs = [(".text$mn", code, 0x60000020), (".data", data, 0xC0000040)]
    if bss:
        secs.append((".bss", b"", 0xC0000080))
    hdr = 20
    off = hdr + 40 * len(secs)
    reloc = struct.pack("<IIH", 1, reloc_sym, 4)
    entries, blob = [], b""
    for nm, d, ch in secs:
        ptr = off + len(blob) if d else 0
        entries.append((nm, d, ch, ptr))
        blob += d
    rel_ptr = off + len(blob)
    symptr = rel_ptr + len(reloc)
    out = struct.pack("<HHIIIHH", 0x8664, len(secs), 0, symptr, len(symbols), 0, 0)
    for k, (nm, d, ch, ptr) in enumerate(entries):
        size = bss if nm == ".bss" else len(d)
        out += struct.pack("<8sIIIIIIHHI", nm.encode(), 0, 0, size, ptr, rel_ptr if k == 0 else 0, 0, 1 if k == 0 else 0, 0, ch)
    out += blob + reloc
    for nm, val, secno, cls in symbols:
        out += struct.pack("<8sIhHBB", nm.encode(), val, secno, 0, cls, 0)
    return out + struct.pack("<I", 4)


A, B = b"AAAA", b"BBBB"
CODE = b"\x48\x8b\x05\0\0\0\0\xc3"


def _layout_pair(tmp_path, second_data=B, second_name="beta"):
    """alpha then beta in .data, versus beta then alpha: same statics, other layout and other symbol indices; the code reads beta."""
    p1 = tmp_path / "ab.obj"
    p2 = tmp_path / "ba.obj"
    p1.write_bytes(_coff(CODE, A + second_data, [("alpha", 0, 2, 2), (second_name, 4, 2, 2)], 1))
    p2.write_bytes(_coff(CODE, second_data + A, [(second_name, 0, 2, 2), ("alpha", 4, 2, 2)], 0))
    return str(p1), str(p2)


def test_a_data_layout_reorder_is_layout_only_with_per_symbol(tmp_path):
    a, b = _layout_pair(tmp_path)
    assert objhash.obj_digest(a)["code_sha256"] != objhash.obj_digest(b)["code_sha256"]          # the plain proof still says changed
    da, db = objhash.obj_digest(a, per_symbol=True), objhash.obj_digest(b, per_symbol=True)
    verdict, why = objhash.classify(da, db, da["persym"], db["persym"])
    assert verdict == "LAYOUT-ONLY" and why == ["1:.data"]


def _pair_in(tmp_path, sub, **kw):
    (tmp_path / sub).mkdir()
    return _layout_pair(tmp_path / sub, **kw)


def _ps(path):
    return objhash.persym_digest(open(path, "rb").read())


def test_a_changed_static_byte_a_renamed_symbol_or_a_moved_relocation_is_not_layout_only(tmp_path):
    base_a, base_b = _pair_in(tmp_path, "base")
    assert objhash.persym_diff(_ps(base_a), _ps(base_b)) == []
    _, changed = _pair_in(tmp_path, "bytes", second_data=b"BBBC")
    assert objhash.persym_diff(_ps(base_a), _ps(changed)) == [".data symbol beta (bytes)"]
    _, renamed = _pair_in(tmp_path, "name", second_name="gamma")
    assert "symbol set" in objhash.persym_diff(_ps(base_a), _ps(renamed))
    moved = tmp_path / "moved.obj"                     # same statics and layout as ba.obj, but the code now reads alpha
    moved.write_bytes(_coff(CODE, B + A, [("beta", 0, 2, 2), ("alpha", 4, 2, 2)], 1))
    assert "section .text$mn" in objhash.persym_diff(_ps(base_b), _ps(str(moved)))


def test_bss_is_compared_by_symbol_set_and_code_changes_are_never_layout_only(tmp_path):
    p1, p2, p3 = (tmp_path / n for n in ("1.obj", "2.obj", "3.obj"))
    p1.write_bytes(_coff(CODE, A, [("x", 0, 2, 2), ("y", 8, 3, 2), ("z", 0, 3, 2)], 0, bss=16))
    p2.write_bytes(_coff(CODE, A, [("x", 0, 2, 2), ("z", 0, 3, 2), ("y", 8, 3, 2)], 0, bss=16))
    p3.write_bytes(_coff(CODE[:-1] + b"\x90", A, [("x", 0, 2, 2), ("y", 8, 3, 2), ("z", 0, 3, 2)], 0, bss=16))
    d1, d2, d3 = (objhash.obj_digest(str(p), per_symbol=True) for p in (p1, p2, p3))
    assert objhash.classify(d1, d3, d1["persym"], d3["persym"])[0] == "DIFF"
    assert objhash.classify(d1, d1, d1["persym"], d1["persym"])[0] == "IDENTICAL"
    assert objhash.classify(d1, d2, d1["persym"], d2["persym"])[0] in ("ORDER-ONLY", "IDENTICAL")


def test_compare_per_symbol_reports_layout_only_and_default_compare_still_fails(tmp_path):
    repo, build, src, obj = _fixture(tmp_path)
    a, b = _layout_pair(tmp_path)
    obj.write_bytes(open(a, "rb").read())
    t = src.stat().st_mtime_ns + 10_000_000
    os.utime(obj, ns=(t, t))
    doc, problems = objhash.hash_files(str(repo), str(build), ["code/a.cpp"], per_symbol=True)
    assert not problems and doc["files"]["code/a.cpp"]["objs"][0]["persym"]
    obj.write_bytes(open(b, "rb").read())
    os.utime(obj, ns=(t + 5_000_000, t + 5_000_000))
    lines, bad = objhash.compare_files(doc)
    assert lines[0].startswith("FAIL") and bad == 1
    lines, bad = objhash.compare_files(doc, per_symbol=True)
    assert lines[0].startswith("LAYOUT-ONLY") and bad == 0
    plain_doc, _ = objhash.hash_files(str(repo), str(build), ["code/a.cpp"])
    plain_doc["files"]["code/a.cpp"]["objs"][0].update(code_sha256="0" * 64, mtime_ns=0)
    lines, bad = objhash.compare_files(plain_doc, per_symbol=True)
    assert lines[0].startswith("FAIL") and "no per-symbol record" in lines[0] and bad == 1


def test_diff_subcommand_exit_codes(tmp_path, capsys):
    a, b = _pair_in(tmp_path, "base")
    assert objhash.main(["diff", a, b]) == 0 and capsys.readouterr().out.startswith("LAYOUT-ONLY")
    assert objhash.main(["diff", a, a]) == 0
    _, changed = _pair_in(tmp_path, "bytes", second_data=b"BBBC")
    assert objhash.main(["diff", a, changed]) == 1
