"""Self-tests for cxx17_ratchet (run: py -3 tools/cxx17_ratchet/test_cxx17_ratchet.py)."""
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cxx17_ratchet as r  # noqa: E402


def hits(src, path="x.cpp"):
    return r.scan_text(src, path)


def n(src, idiom, path="x.cpp"):
    return len(hits(src, path).get(idiom, []))


class Idioms(unittest.TestCase):
    def test_lg01(self):
        self.assertEqual(n("T* p = NULL;\n// NULL in comment\nconst char* s = \"NULL\";\n", "LG01"), 1)
        self.assertEqual(n("#define NULL 0\nnullptr;\n", "LG01"), 0)

    def test_lg02(self):
        self.assertEqual(n("typedef unsigned int u;\nusing v = int;\n", "LG02"), 1)

    def test_lg03(self):
        self.assertEqual(n("Foo* f = new Foo;\ndelete f;\ndelete[] a;\n", "LG03"), 3)
        self.assertEqual(n("void* operator new(size_t);\nFoo(const Foo&) = delete;\nnew (buf) Foo;\n", "LG03"), 0)
        self.assertEqual(n("p = new Foo;\n", "LG03", "heap.cpp"), 0)

    def test_lg04(self):
        src = "enum Kind { A, B };\nstruct V {\n  Kind kind;\n  union { int i; float f; };\n};\n"
        self.assertEqual(n(src, "LG04"), 1)
        self.assertEqual(n("switch (obj->type) {\n", "LG04"), 1)
        self.assertEqual(n("switch (x) {\n", "LG04"), 0)

    def test_lg05(self):
        self.assertEqual(n("bool findThing(int id, Thing& out);\n", "LG05"), 1)
        self.assertEqual(n("bool findThing(int id, const Thing& in);\n", "LG05"), 0)
        self.assertEqual(n("int f() {\n  return -1;\n}\n", "LG05"), 1)
        self.assertEqual(n("Foo* findFoo(int i)\n{\n  return nullptr;\n}\n", "LG05"), 1)
        self.assertEqual(n("Foo* make()\n{\n  return nullptr;\n}\n", "LG05"), 0)

    def test_lg06(self):
        self.assertEqual(n("void load(const char* name, int a);\n", "LG06"), 1)
        self.assertEqual(n("void load(const std::string& name);\n", "LG06"), 1)
        self.assertEqual(n("void load(std::string_view name);\nconst char* g = x;\n", "LG06"), 0)

    def test_lg07(self):
        self.assertEqual(n("template<class T, class = std::enable_if_t<X<T>>> void f();\n", "LG07"), 1)
        self.assertEqual(n("if constexpr (A) {}\n", "LG07"), 0)

    def test_lg08(self):
        self.assertEqual(n("for (auto it = m.begin(); it != m.end(); ++it) f(it->first, it->second);\n", "LG08"), 1)

    def test_lg09(self):
        self.assertEqual(n("int a = (int)x;\nFoo* p = (Foo*)q;\n", "LG09"), 2)
        self.assertEqual(n("void f(int) override;\nint a = sizeof(int);\n(void)x;\nint b = static_cast<int>(x);\n"
                           "if (a) b();\nint c = foo(int) + 1;\n", "LG09"), 0)

    def test_lg10(self):
        self.assertEqual(n("x = max(lo, min(hi, x));\n", "LG10"), 1)
        self.assertEqual(n("if (v < lo) v = lo;\nif (v > hi) v = hi;\n", "LG10"), 1)
        self.assertEqual(n("x = std::clamp(x, lo, hi);\ny = max(a, b);\n", "LG10"), 0)

    def test_lg11(self):
        self.assertEqual(n("CreateDirectoryA(p, 0);\n", "LG11"), 1)
        self.assertEqual(n('std::string p = dir + "\\\\" + name;\n', "LG11"), 1)
        self.assertEqual(n("auto p = std::filesystem::path(a) / b;\n", "LG11"), 0)

    def test_lg12(self):
        self.assertEqual(n('sprintf(buf, "%d", 1);\nprintf("x");\nfprintf(f, "x");\n', "LG12"), 3)
        self.assertEqual(n("auto s = std::format(\"x\");\n", "LG12"), 0)

    def test_lg13(self):
        self.assertEqual(n("#define MAX_TEX 4096\n#define NAME \"x\"\n#define EMPTY\n#define F(x) (x)\n"
                           "#define FOO_H\n#define TYPE_ALIAS int\n", "LG13"), 2)

    def test_lg14(self):
        self.assertEqual(n("enum Mode { A, B };\nenum class M2 { A };\nenum Foo : int { X };\nenum Bar\n{\n  Y\n};\n", "LG14"), 3)
        self.assertEqual(n("enum Mode m;\n", "LG14"), 0)

    def test_lg15(self):
        self.assertEqual(n("std::auto_ptr<int> p;\nregister int i;\nvoid f() throw();\n", "LG15"), 3)

    def test_strip_keeps_lines(self):
        src = "/* a\nb */ int x = NULL; // NULL\n"
        self.assertEqual(hits(src).get("LG01"), [2])


class Check(unittest.TestCase):
    def _res(self, **kw):
        return {"code/a.cpp": {"LG01": list(range(1, kw.get("a", 1) + 1))},
                "mclib/b.cpp": {"LG01": [1]}}

    def test_pass_and_fail(self):
        base = {"head": "t", "counts": r.aggregate(self._res(a=2)), "files": r.per_file_counts(self._res(a=2))}
        out = io.StringIO()
        self.assertEqual(r.check(self._res(a=2), base, out=out), 0)       # equal
        self.assertEqual(r.check(self._res(a=1), base, out=out), 0)       # decreased
        out = io.StringIO()
        self.assertEqual(r.check(self._res(a=3), base, out=out), 1)       # increased
        self.assertIn("code/a.cpp", out.getvalue())
        self.assertIn("LG01", out.getvalue())

    def test_new_dir_counts_as_increase(self):
        base = {"head": "t", "counts": {"code": dict.fromkeys(r.IDS, 0)}, "files": {}}
        self.assertEqual(r.check({"code/a.cpp": {"LG02": [1]}}, base, out=io.StringIO()), 1)


if __name__ == "__main__":
    unittest.main()
