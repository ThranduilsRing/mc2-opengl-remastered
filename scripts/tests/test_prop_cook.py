"""The deploy cook and runtime agree on stock's sibling tier directories."""
import importlib.util
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/mc2texcook'))
spec = importlib.util.spec_from_file_location('prop_cook', ROOT / 'tools/mc2texcook/cook_prop_tiers.py')
cook = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cook)


def test_stock_layout_cooks_native_bc7_and_preserves_source_sidecar(tmp_path, monkeypatch):
    source = tmp_path / '128/building.tga'
    source.parent.mkdir()
    source.write_bytes(b'source')
    original = source.with_suffix('.ktx2')
    original.write_bytes(b'original source-manager sidecar')
    calls = []
    def encode(src, dst, tool, size, **kwargs):
        calls.append((src, size, kwargs))
        header = bytearray(80)
        header[:12] = b'\xabKTX 20\xbb\r\n\x1a\n'
        struct.pack_into('<I', header, 12, 145)
        struct.pack_into('<I', header, 40, 8)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(header)
    monkeypatch.setattr(cook, '_cook_one_bc7', encode)
    cook.cook(tmp_path, 1)
    assert calls == [(source, 0, {'preset': 'normal'})]
    assert original.read_bytes() == b'original source-manager sidecar'
    for tier in (256, 512, 1024):
        assert cook.current(source, tmp_path / str(tier) / 'building.ktx2')
    assert not (tmp_path / '128/512').exists()
    cook.cook(tmp_path, 1)
    assert len(calls) == 1


def test_deploy_invokes_stock_cook_for_target_only(tmp_path):
    from scripts import deploy_payload as deploy
    calls = []
    def runner(command, **kwargs):
        calls.append(command)
        return type('Result', (), {'returncode': 0, 'stdout': '', 'stderr': ''})()
    deploy.ensure_prop_cooks(str(tmp_path), runner)
    assert not calls
    (tmp_path / 'data/tgl/128').mkdir(parents=True)
    deploy.ensure_prop_cooks(str(tmp_path), runner)
    assert calls[0][-2:] == ['--tgl', str(tmp_path / 'data/tgl')]
