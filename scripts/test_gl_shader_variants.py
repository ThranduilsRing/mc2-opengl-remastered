#!/usr/bin/env python3
"""Tests for the GL shader variant-matrix check (pytest or `py -3 -m unittest`).

  * the extractor resolves prefixes the way the C++ builds them (literals, +=,
    ternary, snprintf'd arrays, conditional defines -> optional groups);
  * the committed manifest matches a fresh extraction;
  * the historical static_prop.frag include-splice regression FAILS the matrix
    and the real file passes (the fixture lives in
    tests/shader_variants/static_prop.frag.splice-regression).
"""
from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import extract_gl_shader_variants as ex  # noqa: E402

_spec = importlib.util.spec_from_file_location("check_glsv", HERE / "check-gl-shader-variants.py")
chk = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(chk)

CPP = r'''
void init() {
    std::string pfx = "#version 430\n";
    if (gateA()) pfx += "#define A 1\n";
    if (gateB()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "#define B %d\n", 7);
        pfx += buf;
    }
    glsl_program::makeProgram("demo", "shaders/demo.vert", "shaders/demo.frag", pfx.c_str());
    const char* v = useX ? "#version 430\n#define X\n" : "#version 460\n";
    glsl_program::makeProgram("demo2", "shaders/d2.vert", "shaders/d2.frag", v);
}
'''


class ExtractorTests(unittest.TestCase):
    def _variants(self):
        ex.WORLD = ex.World()
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "t.cpp"
            p.write_text(CPP)
            ex.WORLD.files = {p: None}
            out = []
            ex.collect_make_program(ex.WORLD, out)
            return out

    def test_optional_groups_and_snprintf(self):
        v = self._variants()
        demo = {tuple(x["defines"]) for x in v if x["id"] == "demo"}
        self.assertEqual(demo, {(), ("A",), ("B=7",), ("A", "B=7")})

    def test_ternary_yields_both_alternatives(self):
        v = self._variants()
        vers = {ex.version_of(x["prefix"]) for x in v if x["id"] == "demo2"}
        self.assertEqual(vers, {430, 460})

    def test_real_cpp_extracts_and_manifest_is_fresh(self):
        fresh = ex.dumps(ex.extract())
        self.assertEqual(ex.MANIFEST.read_text(encoding="utf-8"), fresh,
                         "stale: py -3 scripts/extract_gl_shader_variants.py")


class RegressionTests(unittest.TestCase):
    def setUp(self):
        self.glslang = chk.find_glslang()
        if not self.glslang:
            self.skipTest("glslangValidator not installed")

    def test_static_prop_splice_is_caught(self):
        import json
        variants = [v for v in json.loads(ex.MANIFEST.read_text(encoding="utf-8"))["variants"]
                    if "shaders/static_prop.frag" in v["stages"].values()]
        self.assertTrue(variants)
        self.assertEqual(chk.selftest(self.glslang, variants, jobs=4), [])


if __name__ == "__main__":
    unittest.main()
