"""tools/pch_audit.py: the skip-list file format, the normalisation, and that every listed source still exists."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pch_audit as pa  # noqa: E402


def test_render_and_existing_round_trip(tmp_path):
    result = {"gameos": ["GameOS/gameos/a.cpp"], "mclib": [], "mc2": ["code/b.cpp", "code/c.cpp"]}
    path = tmp_path / "skip.cmake"
    path.write_text(pa.render(result), encoding="utf-8")
    assert pa.existing(str(path)) == result
    assert pa.existing(str(tmp_path / "missing.cmake")) == {t: [] for t in pa.TARGETS}


def test_norm_masks_only_what_legitimately_differs():
    assert pa.norm("?f@?A0x4272ba3b@N@@YAXXZ") == pa.norm("?f@?A0x11111111@N@@YAXXZ")
    assert pa.norm("$LN12") == pa.norm("$LN99")
    assert pa.norm("??_C@_0BE@ILGDDEBH@2026?910?911T01?306?346@") == pa.norm("??_C@_0BE@NMEFPGFP@2026?910?911T01?304?315@")
    assert pa.norm("??_C@_0FK@PIEHMNCK@MC2FP1?$HM1a9757c2c890") == pa.norm("??_C@_0FK@DEHACPJA@MC2FP1?$HM1a9757c2c890")
    assert pa.norm("?real@@YAXXZ") == "?real@@YAXXZ"                 # an ordinary symbol keeps its name


def test_target_of_object_paths():
    assert pa.target_of("out/mclib/CMakeFiles/mclib.dir/x.cpp.obj") == "mclib"
    assert pa.target_of("CMakeFiles/mc2.dir/code/y.cpp.obj") == "mc2"
    assert pa.target_of("out/GameOS/gameos/CMakeFiles/gameos_editor.dir/z.cpp.obj") is None


def test_every_skip_list_source_exists():
    listed = pa.existing(os.path.join(pa.ROOT, "cmake", "pch", "pch_skip.cmake"))
    missing = [p for files in listed.values() for p in files if not os.path.exists(os.path.join(pa.ROOT, p))]
    assert not missing, "stale entries in cmake/pch/pch_skip.cmake (regenerate with tools/pch_audit.py): " + ", ".join(missing[:5])
