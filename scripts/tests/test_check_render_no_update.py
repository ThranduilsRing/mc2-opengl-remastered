"""RENDER-THREAD-R2 item 3: the render-no-update row passes on the tree and rejects planted violations."""
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("check_render_no_update", ROOT / "scripts" / "check-render-no-update.py")
chk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chk)


def test_tree_is_clean():
    assert chk.scan(ROOT) == []


def test_planted_update_in_renderview_is_rejected():
    src = "void Foo::renderView(const R& v) {\n  appearance->setBarColor(1);\n  appearance->update();\n}\n"
    assert chk.violations_in(chk.strip(src), False, "x.cpp") == ["x.cpp:3: render path calls update()"]


def test_planted_update_in_render_tu_is_rejected():
    assert chk.violations_in(chk.strip("long A::render(){ updateGeometry(); }"), True, "y.cpp")


def test_comments_strings_and_other_functions_are_ignored():
    src = ('void Foo::update(){ appearance->update(); }\n'
           'void Foo::renderView(const R& v) {\n  // appearance->update();\n  log("update()");\n}\n')
    assert chk.violations_in(chk.strip(src), False, "z.cpp") == []
