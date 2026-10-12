"""run_vk_nointerop_strict_visible.py never reads the gate table at import time."""
import importlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))


def test_import_does_not_read_the_gate_table(monkeypatch):
    import gate_table_reader

    def boom(*a, **k):
        raise gate_table_reader.GateTableError("no gate_table.json here")

    monkeypatch.setattr(gate_table_reader, "gates", boom)
    sys.modules.pop("run_vk_nointerop_strict_visible", None)
    mod = importlib.import_module("run_vk_nointerop_strict_visible")  # must not raise
    assert "MC2_VK_SHARED_GBUFFER" in mod.STRICT_ENV
    sys.modules.pop("run_vk_nointerop_strict_visible", None)
