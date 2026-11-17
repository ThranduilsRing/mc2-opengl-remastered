import importlib.util
import json
from pathlib import Path
import struct
import sys
import zipfile
import zlib

spec = importlib.util.spec_from_file_location('mc2pak', Path(__file__).resolve().parents[2] / 'tools/mc2pak.py')
pak = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pak)

def make_install(root):
    root.mkdir(parents=True, exist_ok=True)
    (root / 'system.cfg').write_text('[FastFiles]\nl NumFastFiles=2\nst File0="first.fst"\nst File1="second.fst"\n')
    payloads = {'data/pak/store.bin': b'abc', 'data/pak/zstd.txt': b'hello Zstandard\n' * 100,
                'data/pak/128/strip.bin': b'xyz'}
    for archive in ('first', 'second'):
        body = bytearray()
        toc = bytearray(struct.pack('<II', 0xCADDECAF, len(payloads)))
        for name, raw in payloads.items():
            compressed = zlib.compress(raw)
            toc += struct.pack('<IIII250s', 8 + len(payloads)*266 + len(body), len(compressed), len(raw), 0, name.encode())
            body += compressed
        (root / f'{archive}.fst').write_bytes(toc + body)
    return payloads

def test_roundtrip_deterministic_and_corruption(tmp_path, capsys):
    install = tmp_path / 'install'
    payloads = make_install(install)
    a, b = tmp_path / 'a.mc2pak', tmp_path / 'b.mc2pak'
    assert pak.main(['pack', '--install', str(install), '--out', str(a)]) == 0
    assert pak.main(['pack', '--install', str(install), '--out', str(b)]) == 0
    assert a.read_bytes() == b.read_bytes()
    assert pak.main(['list', str(a)]) == 0
    assert pak.main(['verify', str(a), '--against-install', str(install)]) == 0
    out = tmp_path / 'out'
    assert pak.main(['extract', str(a), str(out), '--match', 'data/*']) == 0
    for name, raw in payloads.items():
        assert (out / name).read_bytes() == raw
    with zipfile.ZipFile(a) as z:
        assert [i.compress_type for i in z.infolist()] == [0, 0, 93, 0]
        i = z.getinfo('data/pak/store.bin')
        assert i.comment == b'first.fst'
    data = bytearray(a.read_bytes())
    offset = i.header_offset
    nl, xl = struct.unpack_from('<HH', data, offset + 26)
    data[offset + 30 + nl + xl] ^= 1
    a.write_bytes(data)
    assert pak.main(['verify', str(a)]) == 1
    assert 'data/pak/store.bin' in capsys.readouterr().err

def test_equal_size_zlib():
    # Find a small equal-size stream deterministically, without depending on a zlib version's bytes.
    raw = next(b'a'*n + bytes(range(32)) for n in range(1, 128)
               if len(zlib.compress(b'a'*n + bytes(range(32)))) == n + 32)
    compressed = zlib.compress(raw)
    assert pak.decode((0, len(raw), len(raw), 'equal', True, compressed)) == raw
    assert pak.decode((0, 3, 3, 'raw', True, b'xyz')) == b'xyz'

def test_extract_rejects_traversal(tmp_path):
    archive = tmp_path / 'unsafe.zip'
    with zipfile.ZipFile(archive, 'w') as z:
        z.writestr('../escape', b'x')
    assert pak.main(['extract', str(archive), str(tmp_path / 'out')]) == 1
    assert not (tmp_path / 'escape').exists()
