"""Self-tests for cxx17_ratchet (run: py -3 tools/cxx17_ratchet/test_cxx17_ratchet.py)."""
import io
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cxx17_ratchet as r  # noqa: E402
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)


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


class RawEnvReads(unittest.TestCase):
    """LG19: raw MC2_ environment reads (G1a of the typed gate registry)."""

    def test_literal_getenv_forms(self):
        self.assertEqual(n('const char* v = getenv("MC2_X");\n', "LG19"), 1)
        self.assertEqual(n('const char* v = std::getenv("MC2_X");\n', "LG19"), 1)
        self.assertEqual(n('const char* v = ::getenv("MC2_X");\n', "LG19"), 1)
        self.assertEqual(n('const wchar_t* v = _wgetenv(L"MC2_X");\n', "LG19"), 1)

    def test_non_literal_getenv_counts_but_other_literals_do_not(self):
        self.assertEqual(n("const char* v = getenv(name);\n", "LG19"), 1)
        self.assertEqual(n("const char* v = ::getenv(key.c_str());\n", "LG19"), 1)
        self.assertEqual(n('const char* v = getenv("PATH");\n', "LG19"), 0)
        self.assertEqual(n('const char* v = std::getenv("HOME");\n', "LG19"), 0)

    def test_windows_and_crt_variants(self):
        self.assertEqual(n('GetEnvironmentVariableA("MC2_X", buf, n);\n', "LG19"), 1)
        self.assertEqual(n('GetEnvironmentVariableW(L"MC2_X", buf, n);\n', "LG19"), 1)
        self.assertEqual(n('GetEnvironmentVariableA("TEMP", buf, n);\n', "LG19"), 0)
        self.assertEqual(n("_dupenv_s(&p, &len, name);\n", "LG19"), 1)
        self.assertEqual(n('getenv_s(&len, buf, n, "MC2_X");\n', "LG19"), 1)
        self.assertEqual(n("char** e = _environ;\n", "LG19"), 1)
        self.assertEqual(n("auto* b = GetEnvironmentStringsA();\n", "LG19"), 1)
        self.assertEqual(n('auto* v = SDL_getenv("MC2_X");\n', "LG19"), 1)

    def test_comments_and_string_text_do_not_count(self):
        self.assertEqual(n('// getenv("MC2_X") in a comment\n', "LG19"), 0)
        self.assertEqual(n('/* getenv("MC2_X") */ int a;\n', "LG19"), 0)
        self.assertEqual(n('puts("call getenv(\\"MC2_X\\") here");\n', "LG19"), 0)
        self.assertEqual(n("int my_getenv_wrapper = 1;\nobj.getenv(name);\n", "LG19"), 0)

    def test_two_calls_on_one_line_count_once(self):
        self.assertEqual(n('bool b = getenv("MC2_A") && getenv("MC2_B");\n', "LG19"), 1)

    def test_preprocessor_lines_do_not_count(self):
        self.assertEqual(n('#define READ(x) getenv("MC2_" x)\n', "LG19"), 0)

    def test_gate_module_is_exempt_by_path(self):
        self.assertEqual(n('const char* v = std::getenv("MC2_CONFIG_DUMP");\n', "LG19", "Gates/gate.cpp"), 0)
        self.assertEqual(n('const char* v = std::getenv("MC2_CONFIG_DUMP");\n', "LG19", "GameOS/gameos/x.cpp"), 1)

    def test_a_gate_read_through_the_registry_is_not_counted(self):
        self.assertEqual(n('bool b = gate::get<"MC2_X">();\n', "LG19"), 0)

    def test_strict_per_file_even_when_the_total_falls(self):
        # two files: a.cpp drops 3 -> 1, b.cpp rises 0 -> 1: the total falls (3 -> 2), the check must still fail.
        base = {"head": "t", "counts": {"code": {"LG19": 3}},
                "files": {"code/a.cpp": {"LG19": 3}}}
        res = {"code/a.cpp": {"LG19": [1]}, "code/b.cpp": {"LG19": [4]}}
        out = io.StringIO()
        self.assertEqual(r.check(res, base, out=out), 1)
        self.assertIn("code/b.cpp", out.getvalue())
        self.assertIn("drive-to-zero", out.getvalue())

    def test_a_file_the_baseline_does_not_know_must_have_none(self):
        base = {"head": "t", "counts": {"code": {"LG19": 0}}, "files": {}}
        self.assertEqual(r.check({"code/new.cpp": {"LG19": [1]}}, base, out=io.StringIO()), 1)
        self.assertEqual(r.check({"code/new.cpp": {"LG01": [1]}}, base, out=io.StringIO()), 1)  # LG01 is a total rise
        self.assertEqual(r.check({"code/new.cpp": {}}, base, out=io.StringIO()), 0)

    def test_equal_or_lower_per_file_passes(self):
        base = {"head": "t", "counts": {"code": {"LG19": 4}}, "files": {"code/a.cpp": {"LG19": 4}}}
        self.assertEqual(r.check({"code/a.cpp": {"LG19": [1, 2, 3, 4]}}, base, out=io.StringIO()), 0)
        self.assertEqual(r.check({"code/a.cpp": {"LG19": [1]}}, base, out=io.StringIO()), 0)

    def test_tracked_dirs_only_warn(self):
        base = {"head": "t", "counts": {}, "files": {}, "tracked": {"LG19": {"editor": 3}}}
        out = io.StringIO()
        self.assertEqual(r.check({}, base, out=out, tracked={"editor": 5}), 0)
        self.assertIn("editor LG19", out.getvalue())

    def test_tracked_counts_group_by_top_level_dir(self):
        res = {"editor/a.cpp": {"LG19": [1, 2]}, "gui/b.cpp": {"LG19": [3]}, "editor/c.cpp": {}}
        old = r.TRACKED_DIRS
        r.TRACKED_DIRS = ["editor", "gui", "Viewer"]
        try:
            self.assertEqual(r.tracked_counts(res), {"editor": 2, "gui": 1, "Viewer": 0})
        finally:
            r.TRACKED_DIRS = old

    def test_launcher_raw_reads_are_counted_other_tools_are_not(self):
        text = 'void f() { GetEnvironmentVariableA("MC2_IMPORTER_DIR", b, n); }'
        self.assertTrue(r._scan_item(("tools/mc2_launcher/x.cpp", text.encode()))[1].get("LG19"))
        self.assertFalse(r._scan_item(("tools/other/x.cpp", text.encode()))[1].get("LG19"))

    def test_legacy_tool_dirs_are_strict_for_the_gate_idioms_only(self):
        for d in ("editor", "gui", "Viewer"):
            self.assertIn(d, r.DEFAULT_DIRS)
        text = 'void f() { NULL; getenv("MC2_X"); }'
        hits = r._scan_item(("editor/a.cpp", text.encode()))[1]
        self.assertEqual(set(hits) - {"LG19", "LG20"}, set())
        self.assertTrue(hits.get("LG19"))


def _git(cwd, *a):
    return subprocess.run(["git", "-c", "user.email=t@t", "-c", "user.name=t"] + list(a), cwd=cwd,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, **console_child.child_kwargs(redirected=True))


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


class NameLiterals(unittest.TestCase):
    """LG20: every "MC2_X" literal that is not a typed reader's name (companion of LG19)."""

    def test_helper_call_reads_count(self):
        self.assertEqual(n('bool b = subGate("MC2_VK_X");\n', "LG20"), 1)
        self.assertEqual(n('bool b = envFlag("MC2_X", false);\n', "LG20"), 1)
        self.assertEqual(n('const char* v = getenv("MC2_X");\n', "LG20"), 1)

    def test_define_lines_count(self):
        self.assertEqual(n('#define GATE_NAME "MC2_X"\n', "LG20"), 1)

    def test_typed_readers_do_not_count(self):
        self.assertEqual(n('bool b = gate::get<"MC2_X">();\n', "LG20"), 0)
        self.assertEqual(n('bool b = mc2::gate::get<"MC2_X", Rule::Presence>();\n', "LG20"), 0)
        self.assertEqual(n('auto s = gate::str<"MC2_LOG">(); bool a = gate::isSet<"MC2_Y">();\n', "LG20"), 0)
        self.assertEqual(n('bool a = gate::is<"MC2_CONFIG_DUMP", "all">();\n', "LG20"), 0)
        self.assertEqual(n('auto t = gate::state<"MC2_Z">();\n', "LG20"), 0)

    def test_two_literals_on_one_line_count_once(self):
        self.assertEqual(n('a = f("MC2_A") && f("MC2_B");\n', "LG20"), 1)

    def test_non_gate_strings_and_comments_do_not_count(self):
        self.assertEqual(n('puts("MC2 is a game");\n', "LG20"), 0)
        self.assertEqual(n('// getenv("MC2_X")\nint a;\n', "LG20"), 0)
        self.assertEqual(n('const char* s = "prefix MC2_X suffix";\n', "LG20"), 0)   # not a whole-literal name

    def test_gate_module_and_its_unit_test_are_exempt(self):
        self.assertEqual(n('_putenv_s("MC2_UNITTEST_INT", "1");\n', "LG20", "tests/unit/test_gate.cpp"), 0)
        self.assertEqual(n('x("MC2_A");\n', "LG20", "Gates/gate.cpp"), 0)
        self.assertEqual(n('x("MC2_A");\n', "LG20", "tests/unit/test_other.cpp"), 1)

    def test_tools_and_tests_dirs_keep_only_lg20(self):
        _, hits = r._scan_item(("tools/x/h.cpp", b'NULL; char* p = (char*)q; bool b = f("MC2_X");\n'))
        self.assertEqual(set(hits), {"LG20"})
        _, hits = r._scan_item(("code/x.cpp", b'void* p = NULL; bool b = f("MC2_X");\n'))
        self.assertIn("LG01", hits)

    def test_strict_per_file(self):
        base = {"head": "t", "counts": {"tools": {"LG20": 2}}, "files": {"tools/a.cpp": {"LG20": 2}}}
        res = {"tools/a.cpp": {"LG20": [1]}, "tools/b.cpp": {"LG20": [3]}}   # a drops, b is new: fails per file
        self.assertEqual(r.check(res, base, out=io.StringIO()), 1)
        self.assertEqual(r.check({"tools/a.cpp": {"LG20": [1, 2]}}, base, out=io.StringIO()), 0)
