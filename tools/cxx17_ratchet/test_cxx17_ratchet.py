"""Self-tests for cxx17_ratchet (run: py -3 tools/cxx17_ratchet/test_cxx17_ratchet.py)."""
import io
import os
import subprocess
import sys
import tempfile
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

    def test_lg12a_logging(self):
        self.assertEqual(n('printf("x");\nfprintf(f, "x");\nputs("x");\nOutputDebugStringA("x");\nSPEW(("a", "b"));\n', "LG12a"), 5)
        self.assertEqual(n('sprintf(buf, "%d", 1);\n', "LG12a"), 0)
        self.assertEqual(n("auto s = std::format(\"x\");\nlog.printf(1);\n", "LG12a"), 0)

    def test_lg12b_buffer_format(self):
        self.assertEqual(n('sprintf(buf, "%d", 1);\nsnprintf(b, 4, "x");\n_snprintf(b, 4, "x");\nvsprintf(b, f, ap);\n', "LG12b"), 4)
        self.assertEqual(n('printf("x");\nfprintf(f, "x");\n', "LG12b"), 0)
        self.assertEqual(n("auto s = std::format(\"x\");\n", "LG12b"), 0)

    def test_lg13(self):
        self.assertEqual(n("#define MAX_TEX 4096\n#define NAME \"x\"\n#define EMPTY\n#define F(x) (x)\n"
                           "#define FOO_H\n#define TYPE_ALIAS int\n", "LG13"), 2)

    def test_lg14(self):
        self.assertEqual(n("enum Mode { A, B };\nenum class M2 { A };\nenum Foo : int { X };\nenum Bar\n{\n  Y\n};\n", "LG14"), 3)
        self.assertEqual(n("enum Mode m;\n", "LG14"), 0)

    def test_lg15(self):
        self.assertEqual(n("std::auto_ptr<int> p;\nregister int i;\nvoid f() throw();\n", "LG15"), 3)

    def test_lg16(self):
        self.assertEqual(n("v = h[y*w+x];\nw = a[(z*h+y)*w+x];\nq = t[row * stride + col];\n", "LG16"), 3)
        self.assertEqual(n("v = h[i+1];\nv = a[2*3+1];\nv = a[i*2];\nint x = b*c+d;\n", "LG16"), 0)
        self.assertEqual(n("float** g = new float*[h];\n", "LG16"), 1)

    def test_lg17(self):
        self.assertEqual(n("void fill(float* p, size_t n);\nvoid g(int a, const Foo* items, int count);\n", "LG17"), 2)
        self.assertEqual(n("void f(void* p, size_t n);\nint x = a * b, n2 = 3;\nvoid h(const Foo& r, int n);\n"
                           "void k(float* p);\n", "LG17"), 0)

    def test_lg18(self):
        self.assertEqual(n("uint32_t b = *reinterpret_cast<uint32_t*>(&f);\nint c = *(DWORD*)&f;\n"
                           "memcpy(&u, &f, sizeof(float));\n", "LG18"), 3)
        self.assertEqual(n("union P { float f; uint32_t u; };\n", "LG18"), 1)
        self.assertEqual(n("union P { float f; double d; };\nauto b = std::bit_cast<uint32_t>(f);\n"
                           "auto* c = reinterpret_cast<Foo*>(p);\n", "LG18"), 0)

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

    def test_total_strict_dir_warning_only(self):
        base = {"head": "t", "counts": {"code": {"LG01": 5}, "mclib": {"LG01": 1}}, "files": {}}
        moved = {"code/a.cpp": {"LG01": [1, 2]}, "mclib/b.cpp": {"LG01": [1, 2, 3, 4]}}   # 6 -> 6
        out = io.StringIO()
        self.assertEqual(r.check(moved, base, out=out), 0)
        self.assertIn("warn: mclib LG01", out.getvalue())
        grew = {"code/a.cpp": {"LG01": [1, 2, 3, 4, 5]}, "mclib/b.cpp": {"LG01": [1, 2]}}  # 7 > 6
        self.assertEqual(r.check(grew, base, out=io.StringIO()), 1)


def _git(cwd, *a):
    return subprocess.run(["git", "-c", "user.email=t@t", "-c", "user.name=t"] + list(a), cwd=cwd,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)


class CommittedTree(unittest.TestCase):
    """Default scan reads HEAD; dirty edits only count with worktree, index with staged."""

    def test_modes(self):
        with tempfile.TemporaryDirectory() as td:
            _git(td, "init", "-q")
            os.makedirs(os.path.join(td, "code"))
            f = os.path.join(td, "code", "a.cpp")
            with open(f, "w") as fh:
                fh.write("int* p = NULL;\n")
            _git(td, "add", "code/a.cpp")
            _git(td, "commit", "-qm", "c")
            with open(f, "w") as fh:                       # dirty: 3 NULLs
                fh.write("int* p = NULL;\nint* q = NULL;\nint* s = NULL;\n")
            old = r.ROOT
            r.ROOT = td
            try:
                n_ = lambda src: len(r.scan_repo(["code"], 1, src)["code/a.cpp"].get("LG01", []))  # noqa: E731
                self.assertEqual(n_("head"), 1)
                self.assertEqual(n_("worktree"), 3)
                self.assertEqual(n_("staged"), 1)
                _git(td, "add", "code/a.cpp")
                self.assertEqual(n_("staged"), 3)
                self.assertEqual(n_("head"), 1)
            finally:
                r.ROOT = old


if __name__ == "__main__":
    unittest.main()
