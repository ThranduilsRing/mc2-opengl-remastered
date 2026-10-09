"""Unit tests: py -3 -m unittest tools/comment_tidy/test_comment_tidy.py  (or run directly)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import comment_tidy as ct  # noqa: E402


def ctx(envs=(), idents=(), files=(), max_block=12):
    return ct.Ctx(os.getcwd(), [], list(files), set(envs), set(idents), max_block=max_block,
                  have_git=False)


def run(src, kind="c", enabled=None, name="a/x.cpp", **kw):
    blocks, _ = ct.extract(name, src, kind)
    out = []
    for b in blocks:
        out += ct.check_block(b, ctx(**kw), kind, enabled or set(ct.CHECKS))
    return out


class Extract(unittest.TestCase):
    def test_c_styles_and_strings(self):
        src = 'int a; // one\nconst char* s = "// not"; /* two\n * three */\n// x\n// y\n'
        blocks, code = ct.extract("f.cpp", src, "c")
        self.assertEqual([b.text for b in blocks], ["one", "two\nthree", "x\ny"])
        self.assertIn('"// not"', code)
        self.assertNotIn("one", code)

    def test_py_and_hash(self):
        src = 'x = "# no"  # yes\n"""doc # not"""\n# a\n# b\n'
        blocks, _ = ct.extract("f.py", src, "py")
        self.assertEqual([b.text for b in blocks], ["yes", "a\nb"])
        blocks, _ = ct.extract("f.sh", "echo $#  # c\n", "hash")
        self.assertEqual([b.text for b in blocks], ["c"])


class Checks(unittest.TestCase):
    def ids(self, fs):
        return sorted(f["id"] for f in fs)

    def test_ct001(self):
        fs = run("// MC2_DEAD_GATE and MC2_LIVE\n", enabled={"CT001"}, envs={"MC2_LIVE"})
        self.assertEqual([f["token"] for f in fs], ["MC2_DEAD_GATE"])

    def test_ct002_ct003(self):
        src = "// see GameOS/gos.cpp:123 and docs/nope.md and docs/ok.md\n"
        fs = run(src, enabled={"CT002", "CT003"}, files=["GameOS/gameos/gos.cpp", "docs/ok.md"])
        self.assertEqual(self.ids(fs), ["CT002", "CT003"])
        self.assertEqual([f["token"] for f in fs if f["id"] == "CT002"], ["docs/nope.md"])

    def test_ct005(self):
        fs = run("// calls FooBarBaz() and RealFunc_x() and Klass::missing\n", enabled={"CT005"},
                 idents={"RealFunc_x", "Klass"})
        self.assertEqual(sorted(f["token"] for f in fs), ["FooBarBaz", "Klass::missing"])

    def test_ct006(self):
        src = "// int a = 1;\n// foo(a);\n// return a;\n// prose line here\n"
        self.assertEqual(self.ids(run(src, enabled={"CT006"})), ["CT006"])
        self.assertEqual(run("// This is prose;\n// more prose\n// and more\n", enabled={"CT006"}), [])

    def test_ct007(self):
        src = "".join("// VK-RUNG8 lane P8a landed 2026-05-01 step %d\n" % i for i in range(14))
        self.assertEqual(self.ids(run(src, enabled={"CT007"})), ["CT007"])
        self.assertEqual(run(src[:200], enabled={"CT007"}), [])

    def test_ct009_ct010(self):
        self.assertEqual(self.ids(run("// moved from the nifty-mendeleev worktree\n", enabled={"CT009"})), ["CT009"])
        self.assertEqual(run("// SIMD lane mask\n", enabled={"CT009"}), [])
        self.assertEqual(self.ids(run("// removed the old shim\n", enabled={"CT010"})), ["CT010"])
        self.assertEqual(run("// will be removed later\n", enabled={"CT010"}), [])


class Fix(unittest.TestCase):
    def test_ct003_fix(self):
        src = ('int x = 1; // see foo.cpp:12-20, bar.h:3/9 and a/b.py:7\n'
               'const char* s = "keep.cpp:5";\n/* gos.h:99\n * more */\n')
        out = ct.fix_ct003_text(src, "c")
        self.assertIn("see foo.cpp, bar.h and a/b.py", out)
        self.assertIn('"keep.cpp:5"', out)
        self.assertIn("/* gos.h\n", out)
        self.assertEqual(ct.fix_ct003_text(out, "c"), out)   # idempotent


if __name__ == "__main__":
    unittest.main()
