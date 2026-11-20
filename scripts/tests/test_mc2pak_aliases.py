"""mc2pak.py read_aliases rejects the alias tables the engine reader rejects."""
import importlib.util
import io
import sys
import zipfile
from pathlib import Path

import pytest

pytestmark = pytest.mark.skipif(sys.version_info < (3, 14), reason="mc2pak needs Python 3.14 (ZIP Zstandard)")
SPEC = importlib.util.spec_from_file_location("mc2pak_under_test", Path(__file__).resolve().parents[2] / "tools" / "mc2pak.py")


def table(text):
    mod = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(mod)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(mod.ALIASES, text.encode())
    return mod, zipfile.ZipFile(buf)


def test_good_table_loads():
    mod, z = table("c.txt\ta.txt\tloose\n")
    assert mod.read_aliases(z) == {"c.txt": ("a.txt", "loose")}


@pytest.mark.parametrize("text", [
    "c.txt\ta.txt\tloose\r\n", "\nc.txt\ta.txt\tloose\n", "../x\ta.txt\tloose\n", "/abs\ta.txt\tloose\n",
    "c:/x\ta.txt\tloose\n", "c\0x\ta.txt\tloose\n", "c.txt\ta.txt\n", "c.txt\ta.txt\tloose\nc.txt\ta.txt\tloose\n",
    "mc2pak.json\ta.txt\tloose\n", "C.txt\ta.txt\tloose\n"])
def test_bad_tables_rejected(text):
    mod, z = table(text)
    with pytest.raises(ValueError):
        mod.read_aliases(z)
